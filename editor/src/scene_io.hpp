#pragma once
/*
 * scene_io.hpp — Load and save scene JSON files.
 */

#include "editor_state.hpp"
#include "../../engine_cpp/script_system.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>

namespace SceneIO {

inline nlohmann::json scene_document(const EditorState& st);

// Older scenes and a few interrupted third-party writes can contain explicit
// JSON nulls (for example `"constraints": null`).  nlohmann::json::value()
// deliberately throws when it is called on a null value, which used to take
// the whole Editor down on its first frame.  Null is not a meaningful engine
// field value: a missing field has the documented default instead.  Normalize
// these legacy values while loading, before any panel or runtime system sees
// the entity.  Objects and arrays remain intact; only unusable null entries
// are removed.
inline void remove_null_fields(nlohmann::json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ) {
            if (it->is_null()) {
                it = value.erase(it);
            } else {
                remove_null_fields(*it);
                ++it;
            }
        }
    } else if (value.is_array()) {
        for (auto it = value.begin(); it != value.end(); ) {
            if (it->is_null()) it = value.erase(it);
            else { remove_null_fields(*it); ++it; }
        }
    }
}

// EditorState stores entities in the engine's lightweight runtime::Value,
// rather than nlohmann::json directly.  Apply the same defensive cleanup to
// that representation after parsing so panels never receive an explicit null
// where they expect a component/property object.
inline void remove_null_fields(Entity& value) {
    if (value.is_object()) {
        auto& fields = value.items();
        for (auto it = fields.begin(); it != fields.end(); ) {
            if (it->second.is_null()) it = fields.erase(it);
            else { remove_null_fields(it->second); ++it; }
        }
    } else if (value.is_array()) {
        size_t index = 0;
        while (index < value.size()) {
            if (value[index].is_null()) value.erase_at(index);
            else { remove_null_fields(value[index]); ++index; }
        }
    }
}

inline void normalize_loaded_entity(Entity& entity) {
    remove_null_fields(entity);
    if (!entity.contains("components") || !entity["components"].is_object())
        entity["components"] = Entity::object();
    // Transform is the one component every scene/system reads immediately.
    // Keep malformed/missing legacy transforms as an empty object so the
    // normal per-field defaults apply instead of exposing a null JSON value.
    if (!entity["components"].contains("Transform") ||
        !entity["components"]["Transform"].is_object())
        entity["components"]["Transform"] = Entity::object();
}

inline bool load(EditorState& st) {
    namespace fs = std::filesystem;
    std::string path = st.scene_path;
    if (!fs::exists(path)) {
        st.log_error("Scene not found: " + path + "  (path is relative to project root)");
        return false;
    }
    try {
        std::ifstream f(path);
        if (!f) {
            st.log_error("Could not open scene: " + path);
            return false;
        }
        nlohmann::json j; f >> j;
        if (!j.is_object()) {
            st.log_error("Load error: scene root must be a JSON object: " + path);
            return false;
        }

        // Build a complete replacement first.  A parse/type error must never
        // leave the editor with half its previous scene erased.
        EntityList loaded_entities;
        if (j.contains("entities") && j["entities"].is_array()) {
            for (const auto& raw : j["entities"]) {
                if (raw.is_null()) continue;
                if (!raw.is_object()) {
                    st.log_error("Load error: every scene entity must be an object: " + path);
                    return false;
                }
                Entity normalized = raw;
                normalize_loaded_entity(normalized);
                loaded_entities.push_back(std::move(normalized));
            }
        } else if (j.contains("entities")) {
            st.log_error("Load error: 'entities' must be an array: " + path);
            return false;
        }
        // Sorting layers are project/scene-level config, same idea as
        // Unity's Tags & Layers asset. Absent in older scene files — keep
        // EditorState's built-in default order ("Background","Default",
        // "Foreground","UI") in that case so legacy scenes don't lose layers.
        std::vector<std::string> loaded_layers = {"Background", "Default", "Foreground", "UI"};
        if (j.contains("sorting_layers") && j["sorting_layers"].is_array()) {
            std::vector<std::string> layers;
            for (auto& v : j["sorting_layers"])
                if (v.is_string()) layers.push_back(v.get<std::string>());
            if (!layers.empty()) loaded_layers = std::move(layers);
        }

        st.entities = std::move(loaded_entities);
        st.sorting_layers = std::move(loaded_layers);
        ScriptRegistry::instance().set_active_project_from_scene_path(path);
        Matchmaking::SetProjectName(project_name_from_scene_path(path));
        st.clear_selection();
        st.update_asset_dir();
        st.add_recent(path);
        st.resync_children_arrays();
        transform::mark_structure_dirty();
        // Capture the canonical *loaded* state after legacy normalization and
        // children-array repair. Comparing raw file bytes here would mark an
        // untouched legacy scene as dirty solely because the Editor repaired
        // metadata in memory.
        st.saved_scene_fingerprint = scene_document(st).dump();
        st.log("Scene loaded: " + path);
        return true;
    } catch (std::exception& ex) {
        st.log_error(std::string("Load error: ") + ex.what());
        return false;
    }
}

// One canonical scene document is used for both disk writes and the Editor's
// close-safety comparison.  Keeping the two paths together prevents a false
// "unsaved" warning merely because the comparison forgot to omit a transient
// runtime key that save() correctly excludes.
inline nlohmann::json scene_document(const EditorState& st) {
    nlohmann::json j;
    j["entities"] = nlohmann::json::array();
    for (const auto& e : st.entities) {
            // deep_clone() so that erasing runtime-only keys below does NOT
            // mutate the live entity in st.entities (shallow copy shared the map).
            auto copy = e.deep_clone();
            for (auto key : {"_prev_x","_prev_y","_sleeping","_sleep_t",
                             "_force_x","_force_y","_torque","_particles",
                             "_destroyed","_pending_events","_pending_graph_events","_runtime_only"})
                copy.erase(key);
            j["entities"].push_back(copy);
    }
    j["sorting_layers"] = st.sorting_layers;
    return j;
}

// Compare the in-memory authoring scene with the last successful scene file
// only when the user asks to close.  This covers every edit path (Inspector,
// gizmos, hierarchy, tile painting, visual tools, shortcuts) without relying
// on each individual panel remembering to flip a separate dirty flag.
inline bool has_unsaved_changes(const EditorState& st) {
    namespace fs = std::filesystem;
    try {
        const std::string current = scene_document(st).dump();
        if (!st.saved_scene_fingerprint.empty())
            return current != st.saved_scene_fingerprint;
        const fs::path scene_path(st.scene_path);
        if (!fs::exists(scene_path)) return !st.entities.empty();
        std::ifstream f(scene_path);
        if (!f) return true; // never risk discarding if the saved scene is unreadable.
        nlohmann::json on_disk;
        f >> on_disk;
        return scene_document(st) != on_disk;
    } catch (...) {
        // A malformed/locked scene must be treated as unsaved work.  The Save
        // action will surface the actual error while Discard remains explicit.
        return true;
    }
}

inline bool save(EditorState& st) {
    namespace fs = std::filesystem;
    try {
        const fs::path scene_path(st.scene_path);
        const fs::path parent = scene_path.parent_path();
        if (!parent.empty()) fs::create_directories(parent);
        nlohmann::json j = scene_document(st);

        // Write the complete JSON to a sibling first.  This protects the
        // user's last good scene from an interrupted write (crash, full disk,
        // antivirus lock, etc.).  The previous revision is retained as .bak.
        const fs::path temp_path = scene_path.string() + ".tmp";
        const fs::path backup_path = scene_path.string() + ".bak";
        {
            std::ofstream f(temp_path, std::ios::trunc);
            if (!f) throw std::runtime_error("could not create temporary scene file");
            f << j.dump(2);
            f.flush();
            if (!f) throw std::runtime_error("could not finish writing temporary scene file");
        }

        std::error_code ec;
        if (fs::exists(scene_path)) {
            fs::copy_file(scene_path, backup_path, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fs::remove(temp_path);
                throw std::runtime_error("could not create scene backup: " + ec.message());
            }
        }
        fs::rename(temp_path, scene_path, ec);
        if (ec) {
            // std::filesystem::rename cannot replace a destination on some
            // Windows standard libraries. The .bak remains recoverable if
            // the fallback replacement is interrupted.
            ec.clear();
            fs::remove(scene_path, ec);
            if (ec) {
                fs::remove(temp_path);
                throw std::runtime_error("could not replace scene file: " + ec.message());
            }
            fs::rename(temp_path, scene_path, ec);
            if (ec) {
                fs::remove(temp_path);
                throw std::runtime_error("could not install saved scene: " + ec.message());
            }
        }
        st.add_recent(st.scene_path);
        st.saved_scene_fingerprint = j.dump();
        st.log("Scene saved: " + st.scene_path);
        return true;
    } catch (std::exception& ex) {
        st.log_error(std::string("Save error: ") + ex.what());
        return false;
    }
}

inline Entity make_entity(const std::string& name, EditorState& st) {
    Entity e;
    e["id"]   = st.next_id();
    e["name"] = name;
    e["active"] = true;
    e["components"]["Transform"] = {
        {"x",0.f},{"y",0.f},{"rotation",0.f},{"scale_x",1.f},{"scale_y",1.f},{"parent",-1}
    };
    return e;
}

inline Entity duplicate_entity(const Entity& src, EditorState& st) {
    // deep_clone() produces a truly independent entity; plain copy shares the
    // underlying object_t map so both entities would mutate each other.
    Entity e = src.deep_clone();
    e["id"] = st.next_id();
    std::string n = e.value("name","Entity");
    e["name"] = n + " (Copy)";
    if (e.contains("components") && e["components"].contains("Transform")) {
        e["components"]["Transform"]["x"] = (float)e["components"]["Transform"]["x"] + 20.f;
        e["components"]["Transform"]["y"] = (float)e["components"]["Transform"]["y"] + 20.f;
    }
    return e;
}

// Duplicates `root_id` together with its entire descendant subtree (Unity-
// style), remapping internal parent links to the fresh ids while keeping the
// new root attached to the *original* external parent. Returns the new
// root's id, or -1 if root_id wasn't found.
//
// Appends each copy to st.entities immediately (rather than collecting them
// in a local vector and pushing all at once at the end) because next_id()
// derives the new id from the current max id in st.entities — if copies
// aren't visible there yet, every copy in the same batch is handed back the
// SAME new id, leaving two+ entities with identical ids and corrupting
// find_entity()/parent_of() lookups and the transform registry.
inline int duplicate_with_descendants(int root_id, EditorState& st) {
    std::vector<int> all_ids;
    {
        std::vector<int> stack{root_id};
        while (!stack.empty()) {
            int id = stack.back(); stack.pop_back();
            all_ids.push_back(id);
            for (int kid : st.children_of(id)) stack.push_back(kid);
        }
    }
    std::sort(all_ids.begin(), all_ids.end());
    all_ids.erase(std::unique(all_ids.begin(), all_ids.end()), all_ids.end());

    std::unordered_map<int,int> old_to_new;
    for (int id : all_ids) {
        Entity* ep = st.find_entity(id);
        if (!ep) continue;
        Entity c = duplicate_entity(*ep, st);
        old_to_new[id] = c.value("id",0);
        st.entities.push_back(std::move(c)); // visible to next iteration's next_id()
    }
    for (auto& [old_id, new_id] : old_to_new) {
        Entity* cp = st.find_entity(new_id);
        if (!cp) continue;
        int old_parent = EditorState::parent_of(*cp);
        auto it = old_to_new.find(old_parent);
        if (it != old_to_new.end()) (*cp)["components"]["Transform"]["parent"] = it->second;
    }
    transform::rebuild_registry(st.entities);
    st.resync_children_arrays();
    auto it = old_to_new.find(root_id);
    return it != old_to_new.end() ? it->second : -1;
}

} // namespace SceneIO
