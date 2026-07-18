#pragma once
/*
 * prefab_system.hpp — Gap 3: Prefab System
 *
 * Supports:
 *   - prefab assets (.prefab) with nested child trees
 *   - prefab instances with prefab_source / prefab_guid / prefab_overrides
 *   - prefab variants via variant_of delta chains
 *   - instance refresh / override bookkeeping for the editor UI
 *
 * File format:
 *   A prefab asset is a JSON object with a single top-level key "root"
 *   containing one entity tree. Nested prefab instances inside that tree
 *   keep their own prefab_source / prefab_guid / prefab_overrides fields.
 *
 * Notes:
 *   - Runtime scene entities keep the prefab metadata fields so the editor
 *     can track overrides and save them back to disk.
 *   - Template-only child arrays are stored under "_prefab_children" inside
 *     prefab assets. Scene entities never need that field; it is stripped
 *     after instantiation.
 */

#include "../editor/src/editor_state.hpp"
#include "../editor/src/scene_io.hpp"
#include "determinism.hpp"
#include <nlohmann/json.hpp>
#include <../editor/third_party/imgui/imgui.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <random>
#include <sstream>
#include <iomanip>
#include <functional>
#include <algorithm>

#ifndef ICON_FA_CUBE
#  define ICON_FA_CUBE "[P]"
#endif

namespace fs = std::filesystem;

namespace prefab_detail {
    inline std::string generate_guid();
    inline bool is_internal_key(const std::string& key);
    inline void strip_runtime_keys(Entity& e, bool strip_prefab_meta);
    inline nlohmann::json merge_node(nlohmann::json base, const nlohmann::json& patch);
    inline nlohmann::json diff_components(const nlohmann::json& tpl_comps,
                                          const nlohmann::json& inst_comps);
    inline nlohmann::json children_array_from(const nlohmann::json& n);
    inline Entity from_json_safe(const nlohmann::json& j);
    inline void sanitize_prefab_json(nlohmann::json& node);
    inline int max_entity_id(const EntityList& list);
    inline std::string to_lower(std::string s);
}

namespace prefab {
    inline Entity load(const std::string& path);
    inline bool save(const Entity& e, const std::string& path, const std::string& variant_of = "");
    inline bool save_entity_as_prefab(int root_id, EditorState& st,
                                      const std::string& path,
                                      const std::string& variant_of = "");
    inline bool save_entity_as_prefab(int root_id, EntityList& scene,
                                      const std::string& path,
                                      const std::string& variant_of = "");
    inline std::string resolve_prefab_path(const std::string& path,
                                           const std::string& asset_dir = "");
    inline int instantiate(const std::string& path, EditorState& st,
                           float wx = 0.f, float wy = 0.f);
    inline int instantiate(const std::string& path, EntityList& scene,
                           const std::string& asset_dir = "",
                           float wx = 0.f, float wy = 0.f);
    inline bool is_instance(const Entity& e);
    inline nlohmann::json compute_overrides(const Entity& instance, const Entity& tpl);
    inline bool is_field_overridden(const Entity& instance,
                                    const std::string& comp,
                                    const std::string& field);
    inline void revert_field(Entity& instance,
                             const std::string& comp,
                             const std::string& field);
    inline void revert_all(Entity& instance);
    inline bool apply_overrides(Entity& instance, EditorState& st);
    inline void record_override(Entity& instance,
                                const std::string& comp,
                                const std::string& field,
                                const nlohmann::json& value);
    inline void unpack(int root_id, EditorState& st);
    inline bool create_variant(const std::string& base_path,
                               const std::string& variant_path);
}

namespace prefab_detail {

inline std::string generate_guid() {
        std::uniform_int_distribution<uint64_t> dist;
    std::mt19937_64& rng = engine_det::session_rng();
    uint64_t a = dist(rng), b = dist(rng);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(16) << a
       << std::setw(16) << b;
    return ss.str();
}

inline bool is_internal_key(const std::string& key) {
    return key == "id" || key == "children" || key == "_prefab_children";
}

inline void strip_runtime_keys(Entity& e, bool strip_prefab_meta) {
    for (auto key : {"_prev_x","_prev_y","_sleeping","_sleep_t",
                     "_force_x","_force_y","_torque","_particles",
                     "_destroyed","_pending_events","_pending_graph_events","_runtime_only"}) {
        e.erase(key);
    }
    e.erase("children");
    if (strip_prefab_meta) {
        e.erase("prefab_source");
        e.erase("prefab_guid");
        e.erase("prefab_overrides");
    }
}

inline nlohmann::json children_array_from(const nlohmann::json& n) {
    if (!n.is_object() || !n.contains("_prefab_children") || !n["_prefab_children"].is_array())
        return nlohmann::json::array();
    return n["_prefab_children"];
}

inline Entity from_json_safe(const nlohmann::json& j) {
    try {
        if (j.is_null()) return Entity{};
        if (j.is_boolean()) return Entity(j.get<bool>());
        if (j.is_number_integer()) return Entity(j.get<long long>());
        if (j.is_number_unsigned()) return Entity(static_cast<unsigned long long>(j.get<unsigned long long>()));
        if (j.is_number_float()) return Entity(j.get<double>());
        if (j.is_string()) return Entity(j.get<std::string>());

        if (j.is_array()) {
            Entity arr = Entity::array();
            for (const auto& el : j) {
                try {
                    arr.push_back(from_json_safe(el));
                } catch (...) {
                    arr.push_back(Entity{});
                }
            }
            return arr;
        }

        if (j.is_object()) {
            Entity obj = Entity::object();
            for (auto it = j.begin(); it != j.end(); ++it) {
                try {
                    obj[it.key()] = from_json_safe(it.value());
                } catch (...) {
                    obj[it.key()] = Entity{};
                }
            }
            return obj;
        }
    } catch (...) {
    }
    return Entity{};
}

inline void sanitize_prefab_json(nlohmann::json& node) {
    if (!node.is_object()) return;

    if (node.contains("components") && node["components"].is_object()) {
        for (auto& [cname, comp] : node["components"].items()) {
            if (!comp.is_object()) continue;

            if (comp.contains("field_overrides")) {
                auto& fo = comp["field_overrides"];
                if (fo.is_null()) {
                    fo = nlohmann::json::object();
                } else if (!fo.is_object()) {
                    // Older assets occasionally wrote this as a list or null.
                    // Keep the editor/runtime stable by normalizing it to an object.
                    fo = nlohmann::json::object();
                }
            }

            if (cname == "Script" && comp.contains("scripts")) {
                auto& scripts = comp["scripts"];
                if (scripts.is_null()) {
                    scripts = nlohmann::json::array();
                } else if (!scripts.is_array()) {
                    scripts = nlohmann::json::array();
                }
            }
        }
    }

    if (node.contains("_prefab_children") && node["_prefab_children"].is_array()) {
        for (auto& child : node["_prefab_children"]) sanitize_prefab_json(child);
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) sanitize_prefab_json(child);
    }
}

inline nlohmann::json merge_children(const nlohmann::json& base_children,
                                     const nlohmann::json& patch_children) {
    if (!base_children.is_array()) return patch_children.is_array() ? patch_children : base_children;
    if (!patch_children.is_array()) return base_children;
    nlohmann::json out = nlohmann::json::array();
    const std::size_t n = std::max(base_children.size(), patch_children.size());
    for (std::size_t i = 0; i < n; ++i) {
        bool has_base = i < base_children.size();
        bool has_patch = i < patch_children.size();
        if (has_base && has_patch) out.push_back(merge_node(base_children[i], patch_children[i]));
        else if (has_patch) out.push_back(patch_children[i]);
        else if (has_base) out.push_back(base_children[i]);
    }
    return out;
}

inline nlohmann::json merge_node(nlohmann::json base, const nlohmann::json& patch) {
    if (!patch.is_object() || !base.is_object()) return patch;
    for (auto& [k, v] : patch.items()) {
        if (k == "_prefab_children") {
            if (base.contains(k)) base[k] = merge_children(base[k], v);
            else base[k] = v;
            continue;
        }
        if (base.contains(k) && base[k].is_object() && v.is_object()) {
            base[k] = merge_node(base[k], v);
        } else {
            base[k] = v;
        }
    }
    return base;
}

inline nlohmann::json diff_components(const nlohmann::json& tpl_comps,
                                      const nlohmann::json& inst_comps) {
    nlohmann::json overrides = nlohmann::json::object();
    if (!inst_comps.is_object()) return overrides;

    for (auto& [comp, inst_c] : inst_comps.items()) {
        if (!inst_c.is_object()) continue;
        nlohmann::json comp_ovr = nlohmann::json::object();
        const nlohmann::json* tpl_c = nullptr;
        if (tpl_comps.is_object() && tpl_comps.contains(comp))
            tpl_c = &tpl_comps[comp];

        for (auto& [field, val] : inst_c.items()) {
            if (field == "parent") continue;
            if (field == "_prefab_children") continue;
            if (tpl_c && tpl_c->contains(field) && (*tpl_c)[field] == val) continue;
            comp_ovr[field] = val;
        }
        if (!comp_ovr.empty()) overrides[comp] = comp_ovr;
    }
    return overrides;
}

inline int max_entity_id(const EntityList& list) {
    int max_id = 0;
    for (const auto& e : list) max_id = std::max(max_id, e.value("id", 0));
    return max_id;
}

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

} // namespace prefab_detail

namespace prefab {

inline std::string resolve_prefab_path(const std::string& path, const std::string& asset_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);

    // 1. Try as-is (works if path is already absolute, or if CWD is right)
    if (fs::exists(p, ec)) return fs::absolute(p, ec).string();

    if (!asset_dir.empty()) {
        fs::path adir(asset_dir);

        // 2. asset_dir / path  (handles "assets/AbyssPlayer.prefab" or just "AbyssPlayer.prefab")
        fs::path candidate = adir / p;
        if (fs::exists(candidate, ec)) return fs::absolute(candidate, ec).string();

        // 3. absolute(asset_dir) / path  — critical fix: when the editor runs from
        //    build/editor/Release/ but asset_dir is a relative path like
        //    "games/game5/assets", the join above resolves relative to the wrong CWD.
        //    Absolutising asset_dir first corrects this.
        fs::path abs_adir = fs::absolute(adir, ec);
        if (!ec && abs_adir != adir) {
            candidate = abs_adir / p;
            if (fs::exists(candidate, ec)) return candidate.lexically_normal().string();
        }

        // 4. asset_dir / filename only  — strips any subdirectory from path so
        //    "player/AbyssPlayer.prefab" still finds "assets/AbyssPlayer.prefab"
        fs::path fname = p.filename();
        candidate = adir / fname;
        if (fs::exists(candidate, ec)) return fs::absolute(candidate, ec).string();
        candidate = abs_adir / fname;
        if (fs::exists(candidate, ec)) return candidate.lexically_normal().string();

        // 5. parent of asset_dir / path  — handles the case where SpawnAllPlayers
        //    passes the scene directory (e.g. "games/game5") instead of the
        //    assets subdir, so "games/game5" / "AbyssPlayer.prefab" fails but
        //    "games/game5/assets" / "AbyssPlayer.prefab" would succeed.
        //    Try both the dir itself and its "assets" child.
        fs::path assets_subdir = adir / "assets";
        candidate = assets_subdir / p;
        if (fs::exists(candidate, ec)) return fs::absolute(candidate, ec).string();
        candidate = assets_subdir / fname;
        if (fs::exists(candidate, ec)) return fs::absolute(candidate, ec).string();
    }

    // Return what we have even if it doesn't exist — callers check the return
    // value via load() which will return a null Entity on open failure.
    return fs::absolute(p, ec).string();
}

inline Entity load(const std::string& path) {
    try {
        std::error_code ec;
        fs::path p(path);

        if (!fs::exists(p, ec)) {
            std::cerr << "[prefab::load] FILE NOT FOUND: '" << path << "'\n";
            return Entity{};
        }
        std::cerr << "[prefab::load] Opening: '" << path << "'\n";

        std::ifstream f(p);
        if (!f.is_open()) {
            std::cerr << "[prefab::load] FAILED to open (permissions?): '" << path << "'\n";
            return Entity{};
        }

        // Read raw content first so we can report parse errors
        std::string raw((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        std::cerr << "[prefab::load] Read " << raw.size() << " bytes\n";
        if (raw.empty()) {
            std::cerr << "[prefab::load] EMPTY FILE: '" << path << "'\n";
            return Entity{};
        }

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(raw);
        } catch (const std::exception& ex) {
            std::cerr << "[prefab::load] JSON PARSE ERROR in '" << path
                      << "': " << ex.what() << "\n"
                      << "  First 200 chars: " << raw.substr(0, 200) << "\n";
            return Entity{};
        }

        if (!j.is_object()) {
            std::cerr << "[prefab::load] Top-level JSON is not an object (type="
                      << j.type_name() << ") in '" << path << "'\n";
            return Entity{};
        }

        if (j.contains("variant_of") && j["variant_of"].is_string()) {
            std::string base_path = j["variant_of"].get<std::string>();
            fs::path resolved = base_path;
            if (!fs::exists(resolved, ec)) {
                resolved = p.parent_path() / base_path;
            }
            std::cerr << "[prefab::load] variant_of -> '" << resolved.string() << "'\n";
            Entity base = load(resolved.string());
            if (base.is_null()) {
                std::cerr << "[prefab::load] variant_of base FAILED: '" << resolved.string() << "'\n";
                return Entity{};
            }
            if (j.contains("root") && j["root"].is_object()) {
                nlohmann::json merged = prefab_detail::merge_node(static_cast<nlohmann::json>(base),
                                                                  j["root"]);
                return prefab_detail::from_json_safe(merged);
            }
            return base;
        }

        if (!j.contains("root")) {
            std::cerr << "[prefab::load] Missing 'root' key in '" << path << "'\n"
                      << "  Top-level keys: ";
            for (auto& [k, v] : j.items()) std::cerr << "'" << k << "' ";
            std::cerr << "\n";
            return Entity{};
        }
        if (!j["root"].is_object()) {
            std::cerr << "[prefab::load] 'root' is not an object (type="
                      << j["root"].type_name() << ") in '" << path << "'\n";
            return Entity{};
        }

        std::cerr << "[prefab::load] OK: loaded root from '" << path << "'\n";
        prefab_detail::sanitize_prefab_json(j["root"]);
        return prefab_detail::from_json_safe(j["root"]);
    } catch (const std::exception& ex) {
        std::cerr << "[prefab::load] EXCEPTION for '" << path << "': " << ex.what() << "\n";
        return Entity{};
    } catch (...) {
        std::cerr << "[prefab::load] UNKNOWN EXCEPTION for '" << path << "'\n";
        return Entity{};
    }
}

inline bool save(const Entity& e, const std::string& path, const std::string& variant_of) {
    try {
        fs::create_directories(fs::path(path).parent_path());
        nlohmann::json j;
        if (!variant_of.empty()) j["variant_of"] = variant_of;
        Entity copy = e.deep_clone();
        prefab_detail::strip_runtime_keys(copy, false);
        j["root"] = static_cast<nlohmann::json>(copy);
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << j.dump(2);

        return true;
    } catch (...) {
        return false;
    }
}

inline bool is_instance(const Entity& e) {
    return e.contains("prefab_source") && e["prefab_source"].is_string()
        && !e["prefab_source"].get<std::string>().empty();
}

inline bool _has_source_cycle(const std::string& path, const std::string& asset_dir,
                              std::unordered_set<std::string>& stack) {
    std::string resolved = resolve_prefab_path(path, asset_dir);
    if (resolved.empty()) return false;
    if (stack.count(resolved)) return true;
    stack.insert(resolved);
    Entity e = load(resolved);
    stack.erase(resolved);
    return false;
}

inline Entity _resolve_instance_template(const Entity& node, const std::string& asset_dir,
                                        std::unordered_set<std::string>& resolving) {
    if (!is_instance(node)) return node.deep_clone();

    std::string src = node["prefab_source"].get<std::string>();
    std::string resolved_src = resolve_prefab_path(src, asset_dir);
    if (resolved_src.empty()) return node.deep_clone();
    if (resolving.count(resolved_src)) return node.deep_clone();

    resolving.insert(resolved_src);
    Entity base = load(resolved_src);
    resolving.erase(resolved_src);
    if (base.is_null()) return node.deep_clone();

    nlohmann::json merged = prefab_detail::merge_node(static_cast<nlohmann::json>(base),
                                                      static_cast<nlohmann::json>(node));
    return prefab_detail::from_json_safe(merged);
}

inline int _materialize_node(const Entity& template_node, EntityList& scene, int parent_id,
                             const std::string& asset_dir, float wx, float wy,
                             bool is_root, std::unordered_set<std::string>& resolving) {
    Entity resolved = _resolve_instance_template(template_node, asset_dir, resolving);

    // Keep a copy of the merged prefab child list before we strip the
    // template-only array from the live scene entity.
    nlohmann::json prefab_children = (resolved.contains("_prefab_children") && resolved["_prefab_children"].is_array())
        ? static_cast<nlohmann::json>(resolved["_prefab_children"])
        : nlohmann::json::array();

    // Determine if the root should be placed at a requested world position.
    if (is_root && parent_id < 0 && resolved.contains("components") && resolved["components"].contains("Transform")) {
        resolved["components"]["Transform"]["x"] = wx;
        resolved["components"]["Transform"]["y"] = wy;
    }

    int new_id = prefab_detail::max_entity_id(scene) + 1;
    resolved["id"] = new_id;
    if (!resolved.contains("components") || !resolved["components"].contains("Transform")) {
        resolved["components"]["Transform"] = {
            {"x",0.f},{"y",0.f},{"rotation",0.f},{"scale_x",1.f},{"scale_y",1.f},{"parent",parent_id}
        };
    } else {
        resolved["components"]["Transform"]["parent"] = parent_id;
    }

    // Stamp prefab metadata onto the root instance so the editor can track it.
    if (is_root) {
        resolved["prefab_source"] = template_node.value("prefab_source", std::string{});
        resolved["prefab_guid"] = prefab_detail::generate_guid();
        resolved["prefab_overrides"] = nlohmann::json::object();
    }

    // Scene entities should not keep template-only child arrays.
    resolved.erase("_prefab_children");
    scene.push_back(resolved);

    // Recurse into the preserved child tree. Nested prefab roots keep their
    // own prefab metadata, but only the top-level root gets world placement.
    for (const auto& child : prefab_children) {
        if (!child.is_object()) continue;
        bool child_is_prefab = child.contains("prefab_source") && child["prefab_source"].is_string()
                             && !child["prefab_source"].get<std::string>().empty();
        _materialize_node(prefab_detail::from_json_safe(child), scene, new_id, asset_dir, 0.f, 0.f, child_is_prefab, resolving);
    }

    return new_id;
}

inline int instantiate(const std::string& path, EntityList& scene,
                       const std::string& asset_dir, float wx, float wy) {
    std::cerr << "[prefab::instantiate] path='" << path
              << "'  asset_dir='" << asset_dir << "'\n";

    std::string resolved_path = resolve_prefab_path(path, asset_dir);
    std::cerr << "[prefab::instantiate] resolved='" << resolved_path << "'\n";

    {
        std::error_code ec;
        bool ex = fs::exists(fs::path(resolved_path), ec);
        std::cerr << "[prefab::instantiate] file exists=" << (ex ? "YES" : "NO")
                  << (ec ? (" ec=" + ec.message()) : "") << "\n";
    }

    Entity tmpl = load(resolved_path);

    if (tmpl.is_null() || !tmpl.is_object()) {
        std::cerr << "[prefab::instantiate] LOAD RETURNED NULL/INVALID — returning -1\n";
        return -1;
    }

    std::cerr << "[prefab::instantiate] load() OK — materializing\n";
    tmpl["prefab_source"] = resolved_path;

    try {
        std::unordered_set<std::string> resolving;
        int root_id = _materialize_node(tmpl, scene, -1,
                                        fs::path(resolved_path).parent_path().string(),
                                        wx, wy, true, resolving);
        std::cerr << "[prefab::instantiate] _materialize_node id=" << root_id << "\n";
        transform::rebuild_registry(scene);
        return root_id;
    } catch (const std::exception& ex) {
        std::cerr << "[prefab::instantiate] EXCEPTION in _materialize_node: " << ex.what() << "\n";
        return -1;
    } catch (...) {
        std::cerr << "[prefab::instantiate] UNKNOWN EXCEPTION in _materialize_node\n";
        return -1;
    }
}

inline int instantiate(const std::string& path, EditorState& st, float wx, float wy) {
    return instantiate(path, st.entities, st.asset_dir, wx, wy);
}

inline bool save_entity_as_prefab(int root_id, EntityList& scene,
                                  const std::string& path,
                                  const std::string& variant_of) {
    auto* root = [&]() -> Entity* {
        for (auto& e : scene) if (e.value("id", 0) == root_id) return &e;
        return nullptr;
    }();
    if (!root) return false;

    std::vector<int> all_ids;
    std::vector<int> stack{root_id};
    while (!stack.empty()) {
        int id = stack.back(); stack.pop_back();
        if (std::find(all_ids.begin(), all_ids.end(), id) != all_ids.end()) continue;
        all_ids.push_back(id);
        for (auto& e : scene) {
            if (EditorState::parent_of(e) == id) stack.push_back(e.value("id", 0));
        }
    }

    std::unordered_map<int, nlohmann::json> copies;
    for (int id : all_ids) {
        Entity* ep = nullptr;
        for (auto& e : scene) if (e.value("id", 0) == id) { ep = &e; break; }
        if (!ep) continue;
        Entity c = ep->deep_clone();
        prefab_detail::strip_runtime_keys(c, id == root_id);
        copies[id] = static_cast<nlohmann::json>(c);
    }

    std::function<nlohmann::json(int)> embed = [&](int eid) -> nlohmann::json {
        auto it = copies.find(eid);
        if (it == copies.end()) return nlohmann::json{};
        nlohmann::json node = it->second;
        std::vector<int> kids;
        for (const auto& e : scene) {
            if (EditorState::parent_of(e) == eid) kids.push_back(e.value("id", 0));
        }
        if (!kids.empty()) {
            node["_prefab_children"] = nlohmann::json::array();
            for (int kid : kids) node["_prefab_children"].push_back(embed(kid));
        }
        return node;
    };

    try {
        fs::create_directories(fs::path(path).parent_path());
        nlohmann::json j;
        if (!variant_of.empty()) j["variant_of"] = variant_of;
        j["root"] = embed(root_id);
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << j.dump(2);

        return true;
    } catch (...) {
        return false;
    }
}

inline bool save_entity_as_prefab(int root_id, EditorState& st,
                                  const std::string& path,
                                  const std::string& variant_of) {
    return save_entity_as_prefab(root_id, st.entities, path, variant_of);
}

inline nlohmann::json compute_overrides(const Entity& instance, const Entity& tpl) {
    nlohmann::json overrides = nlohmann::json::object();

    nlohmann::json tpl_comps = nlohmann::json::object();
    if (tpl.contains("components")) tpl_comps = tpl["components"];
    if (instance.contains("components")) {
        nlohmann::json comp_overrides = prefab_detail::diff_components(tpl_comps, instance["components"]);
        for (auto& [comp, data] : comp_overrides.items()) overrides[comp] = data;
    }

    // Track root-level fields such as name / active / tag so prefab instances
    // can revert and apply them just like component fields.
    nlohmann::json root_ovr = nlohmann::json::object();
    for (auto& [k, v] : instance.items()) {
        if (k == "id" || k == "components" || k == "children" || k == "_prefab_children" ||
            k == "prefab_source" || k == "prefab_guid" || k == "prefab_overrides")
            continue;
        if (tpl.contains(k) && static_cast<nlohmann::json>(tpl[k]) == static_cast<nlohmann::json>(v)) continue;
        root_ovr[k] = v;
    }
    if (!root_ovr.empty()) overrides["__root__"] = root_ovr;

    return overrides;
}

inline bool is_field_overridden(const Entity& instance,
                                const std::string& comp,
                                const std::string& field) {
    if (!instance.contains("prefab_overrides")) return false;
    const auto& ovr = instance["prefab_overrides"];
    if (!ovr.is_object() || !ovr.contains(comp)) return false;
    return ovr[comp].contains(field);
}

inline void _apply_override_payload(Entity& instance, const nlohmann::json& ovr) {
    if (!ovr.is_object()) return;
    if (!instance.contains("components") || !instance["components"].is_object())
        instance["components"] = nlohmann::json::object();
    for (auto& [comp, fields] : ovr.items()) {
        if (!fields.is_object()) continue;
        if (comp == "__root__") {
            for (auto& [field, val] : fields.items()) {
                instance[field] = val;
            }
            continue;
        }
        for (auto& [field, val] : fields.items()) {
            instance["components"][comp][field] = val;
        }
    }
}

inline void revert_field(Entity& instance, const std::string& comp, const std::string& field) {
    if (!is_instance(instance)) return;
    std::string src_path = instance["prefab_source"].get<std::string>();
    std::string abs_src = resolve_prefab_path(src_path, "");
    Entity tpl = load(abs_src);
    if (tpl.is_null()) return;

    if (comp == "__root__") {
        if (tpl.contains(field)) instance[field] = tpl[field];
        else instance.erase(field);
    } else if (tpl.contains("components") && tpl["components"].contains(comp)
        && tpl["components"][comp].contains(field)) {
        instance["components"][comp][field] = tpl["components"][comp][field];
    } else if (instance.contains("components") && instance["components"].contains(comp)) {
        instance["components"][comp].erase(field);
    }

    if (instance.contains("prefab_overrides") &&
        instance["prefab_overrides"].contains(comp)) {
        instance["prefab_overrides"][comp].erase(field);
        if (instance["prefab_overrides"][comp].empty())
            instance["prefab_overrides"].erase(comp);
    }
}

inline void revert_all(Entity& instance) {
    if (!is_instance(instance)) return;
    std::string src_path = instance["prefab_source"].get<std::string>();
    std::string abs_src = resolve_prefab_path(src_path, "");
    Entity tpl = load(abs_src);
    if (tpl.is_null()) return;

    // Remove stale root-level data first so the instance matches the template
    // exactly except for its prefab bookkeeping fields.
    std::vector<std::string> inst_keys;
    if (instance.is_object()) {
        for (auto& [k, _] : instance.items()) inst_keys.push_back(k);
    }
    for (const auto& k : inst_keys) {
        if (k == "id" || k == "prefab_source" || k == "prefab_guid" || k == "prefab_overrides" ||
            k == "_prefab_children" || k == "children" || k == "components")
            continue;
        if (!tpl.contains(k)) instance.erase(k);
    }

    // Restore root-level fields first, then components.
    for (auto& [k, v] : tpl.items()) {
        if (k == "id" || k == "prefab_source" || k == "prefab_guid" || k == "prefab_overrides" ||
            k == "_prefab_children" || k == "children")
            continue;
        instance[k] = v;
    }
    if (tpl.contains("components")) instance["components"] = tpl["components"];
    instance["prefab_overrides"] = nlohmann::json::object();
}

inline void _sync_instance_subtree(Entity& live,
                                  const Entity& tmpl,
                                  EditorState& st,
                                  const nlohmann::json& own_ovr) {
    // Remove stale root-level fields that no longer exist in the template.
    std::vector<std::string> live_keys;
    if (live.is_object()) {
        for (auto& [k, _] : live.items()) live_keys.push_back(k);
    }
    for (const auto& k : live_keys) {
        if (k == "id" || k == "prefab_source" || k == "prefab_guid" ||
            k == "prefab_overrides" || k == "_prefab_children" || k == "children" ||
            k == "components")
            continue;
        if (!tmpl.contains(k)) live.erase(k);
    }

    if (live.contains("components") && live["components"].is_object()) {
        std::vector<std::string> comps_to_check;
        for (auto& [comp, _] : live["components"].items()) comps_to_check.push_back(comp);
        for (const auto& comp : comps_to_check) {
            if (!tmpl.contains("components") || !tmpl["components"].contains(comp)) {
                live["components"].erase(comp);
                continue;
            }
            if (!tmpl["components"][comp].is_object() || !live["components"][comp].is_object())
                continue;
            std::vector<std::string> field_names;
            for (auto& [field, _] : live["components"][comp].items()) field_names.push_back(field);
            for (const auto& field : field_names) {
                if (!tmpl["components"][comp].contains(field))
                    live["components"][comp].erase(field);
            }
        }
    }

    for (auto& [k, v] : tmpl.items()) {
        if (k == "id" || k == "prefab_source" || k == "prefab_guid" ||
            k == "prefab_overrides" || k == "_prefab_children" || k == "children")
            continue;
        live[k] = v;
    }

    _apply_override_payload(live, own_ovr);

    if (!live.contains("id")) return;
    std::vector<int> live_children = st.children_of(live.value("id", -1));
    nlohmann::json tmpl_children = tmpl.contains("_prefab_children") && tmpl["_prefab_children"].is_array()
        ? static_cast<nlohmann::json>(tmpl["_prefab_children"])
        : nlohmann::json::array();

    const std::size_t count = std::min(live_children.size(), tmpl_children.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!tmpl_children[i].is_object()) continue;
        Entity* child = st.find_entity(live_children[i]);
        if (!child) continue;
        nlohmann::json child_ovr = child->contains("prefab_overrides")
            ? static_cast<nlohmann::json>((*child)["prefab_overrides"])
            : nlohmann::json::object();
        _sync_instance_subtree(*child, Entity{tmpl_children[i]}, st, child_ovr);
    }
}

inline bool apply_overrides(Entity& instance, EditorState& st) {
    if (!is_instance(instance)) return false;
    std::string src_path = instance["prefab_source"].get<std::string>();

    std::string abs_src = resolve_prefab_path(src_path, "");
    Entity tpl = load(abs_src);
    if (tpl.is_null()) return false;

    // Bake the edited instance's current values into the source asset.
    for (auto& [k, v] : instance.items()) {
        if (k == "id" || k == "prefab_source" || k == "prefab_guid" ||
            k == "prefab_overrides" || k == "_prefab_children" || k == "children")
            continue;
        tpl[k] = v;
    }

    if (!save(tpl, src_path)) return false;

    std::string resolved_src = resolve_prefab_path(src_path, st.asset_dir);
    Entity resolved_tpl = load(resolved_src);
    if (resolved_tpl.is_null()) return false;

    for (auto& e : st.entities) {
        if (!is_instance(e)) continue;
        if (resolve_prefab_path(e["prefab_source"].get<std::string>(), st.asset_dir) != resolved_src)
            continue;

        nlohmann::json own_ovr = e.contains("prefab_overrides")
            ? static_cast<nlohmann::json>(e["prefab_overrides"])
            : nlohmann::json::object();

        if (e.value("id", -1) == instance.value("id", -1)) {
            // The edited instance has just been baked into the prefab asset.
            own_ovr = nlohmann::json::object();
            e["prefab_overrides"] = nlohmann::json::object();
        }

        _sync_instance_subtree(e, resolved_tpl, st, own_ovr);

        if (e.value("id", -1) == instance.value("id", -1))
            e["prefab_overrides"] = nlohmann::json::object();
    }

    return true;
}
inline void record_override(Entity& instance,
                           const std::string& comp,
                           const std::string& field,
                           const nlohmann::json& value) {
    if (!is_instance(instance)) return;
    if (!instance.contains("prefab_overrides") || !instance["prefab_overrides"].is_object())
        instance["prefab_overrides"] = nlohmann::json::object();

    std::string src_path = instance["prefab_source"].get<std::string>();
    Entity tpl = load(src_path);
    bool matches_template = false;
    if (!tpl.is_null()) {
        if (comp == "__root__") {
            matches_template = tpl.contains(field) && static_cast<nlohmann::json>(tpl[field]) == value;
        } else if (tpl.contains("components") && tpl["components"].contains(comp) &&
                   tpl["components"][comp].contains(field) && static_cast<nlohmann::json>(tpl["components"][comp][field]) == value) {
            matches_template = true;
        }
    }

    if (matches_template) {
        if (instance["prefab_overrides"].contains(comp)) {
            instance["prefab_overrides"][comp].erase(field);
            if (instance["prefab_overrides"][comp].empty())
                instance["prefab_overrides"].erase(comp);
        }
        return;
    }

    instance["prefab_overrides"][comp][field] = value;
}

inline void unpack(int root_id, EditorState& st) {
    std::vector<int> all_ids;
    std::vector<int> stack{root_id};
    while (!stack.empty()) {
        int id = stack.back(); stack.pop_back();
        if (std::find(all_ids.begin(), all_ids.end(), id) != all_ids.end()) continue;
        all_ids.push_back(id);
        for (int kid : st.children_of(id)) stack.push_back(kid);
    }
    for (int id : all_ids) {
        Entity* e = st.find_entity(id);
        if (!e) continue;
        e->erase("prefab_source");
        e->erase("prefab_guid");
        e->erase("prefab_overrides");
        e->erase("_prefab_children");
    }
}

inline bool create_variant(const std::string& base_path,
                           const std::string& variant_path) {
    try {
        fs::create_directories(fs::path(variant_path).parent_path());
        nlohmann::json j;
        j["variant_of"] = base_path;
        j["root"] = nlohmann::json::object();
        std::ofstream f(variant_path);
        if (!f.is_open()) return false;
        f << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace prefab

namespace prefab_ui {

inline bool draw_override_toolbar(Entity& e, EditorState& st) {
    if (!prefab::is_instance(e)) return false;

    std::string src = e["prefab_source"].get<std::string>();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(52, 80, 140, 60));
    ImGui::BeginChild("##prefab_bar", {0, 54}, true);

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(146, 186, 255, 255));
    ImGui::Text(ICON_FA_CUBE " Prefab");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    std::string src_display = fs::path(src).filename().string();
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180,200,255,255));
    ImGui::TextUnformatted(src_display.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", src.c_str());

    bool unpacked = false;
    if (ImGui::Button("Revert All")) {
        st.undo.push_deep(st.entities);
        prefab::revert_all(e);
        st.log("Prefab: reverted all overrides on '" + e.value("name", "entity") + "'");
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply All")) {
        st.undo.push_deep(st.entities);
        if (prefab::apply_overrides(e, st))
            st.log_success("Prefab: applied overrides to '" + src_display + "'");
        else
            st.log_error("Prefab: apply failed — could not write '" + src + "'");
    }
    ImGui::SameLine();
    if (ImGui::Button("Unpack")) {
        st.undo.push_deep(st.entities);
        prefab::unpack(e.value("id", -1), st);
        st.log("Prefab: unpacked '" + e.value("name", "entity") + "' — now a plain entity");
        unpacked = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Select Asset")) {
        if (fs::exists(src)) st.select_asset(src);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    return unpacked;
}

inline bool is_overridden(const Entity& e,
                          const std::string& comp,
                          const std::string& field) {
    return prefab::is_field_overridden(e, comp, field);
}

inline int draw_hierarchy_prefab_menu(int eid, EditorState& st,
                                      const std::string& asset_dir) {
    Entity* e = st.find_entity(eid);
    if (!e) return 0;
    int result = 0;
    bool is_inst = prefab::is_instance(*e);

    ImGui::Separator();

    if (!is_inst) {
        if (ImGui::MenuItem("Save as Prefab")) {
            std::string name = e->value("name", "Prefab");
            for (auto& c : name)
                if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                    c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                    c = '_';
            std::string path = (fs::path(asset_dir) / (name + ".prefab")).string();
            if (fs::exists(path)) {
                int n = 1;
                while (fs::exists((fs::path(asset_dir) / (name + std::to_string(n) + ".prefab")).string()))
                    ++n;
                path = (fs::path(asset_dir) / (name + std::to_string(n) + ".prefab")).string();
            }
            st.undo.push_deep(st.entities);
            if (prefab::save_entity_as_prefab(eid, st, path)) {
                st.log_success("Saved prefab: " + fs::path(path).filename().string());
                result = 1;
            } else {
                st.log_error("Prefab save failed: " + path);
            }
        }
    } else {
        std::string src = (*e)["prefab_source"].get<std::string>();
        std::string fname = fs::path(src).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(146,186,255,255));
        ImGui::Text("Prefab: %s", fname.c_str());
        ImGui::PopStyleColor();

        if (ImGui::MenuItem("Apply All Overrides")) {
            st.undo.push_deep(st.entities);
            if (prefab::apply_overrides(*e, st)) {
                st.log_success("Applied overrides to " + fname);
                result = 3;
            } else {
                st.log_error("Apply overrides failed — could not write " + src);
            }
        }
        if (ImGui::MenuItem("Revert All Overrides")) {
            st.undo.push_deep(st.entities);
            prefab::revert_all(*e);
            st.log("Reverted all overrides on '" + e->value("name", "entity") + "'");
            result = 4;
        }
        if (ImGui::MenuItem("Unpack Prefab")) {
            st.undo.push_deep(st.entities);
            prefab::unpack(eid, st);
            st.log("Unpacked prefab '" + e->value("name", "entity") + "'");
            result = 2;
        }
        if (ImGui::MenuItem("Create Variant")) {
            std::string var_name = fs::path(src).stem().string() + "_Variant.prefab";
            std::string var_path = (fs::path(asset_dir) / var_name).string();
            if (prefab::create_variant(src, var_path))
                st.log_success("Created prefab variant: " + var_name);
            else
                st.log_error("Could not create variant at: " + var_path);
        }
        if (ImGui::MenuItem("Select Prefab Asset")) {
            if (fs::exists(src)) st.select_asset(src);
        }
    }

    return result;
}

inline void draw_asset_prefab_menu(const std::string& abs_path, EditorState& st) {
    if (ImGui::MenuItem("Instantiate in Scene")) {
        st.undo.push_deep(st.entities);
        int new_id = prefab::instantiate(abs_path, st, st.cam_x, st.cam_y);
        if (new_id >= 0) {
            st.select(new_id);
            st.log_success("Instantiated: " + fs::path(abs_path).filename().string());
        } else {
            st.log_error("Prefab instantiate failed: " + abs_path);
        }
    }
    if (ImGui::MenuItem("Create Variant")) {
        std::string base_stem = fs::path(abs_path).stem().string();
        std::string var_name  = base_stem + "_Variant.prefab";
        std::string var_path  = (fs::path(abs_path).parent_path() / var_name).string();
        if (!fs::exists(var_path)) {
            if (prefab::create_variant(abs_path, var_path))
                st.log_success("Created variant: " + var_name);
            else
                st.log_error("Create variant failed.");
        } else {
            st.log_error("Variant already exists: " + var_name);
        }
    }
}

inline void create_empty_prefab(const std::string& current_dir, EditorState& st) {
    auto unique_name = [&](const std::string& base) {
        std::string name = base + ".prefab";
        int n = 1;
        while (fs::exists(fs::path(current_dir) / name))
            name = base + std::to_string(n++) + ".prefab";
        return name;
    };

    std::string fname = unique_name("NewPrefab");
    fs::path fpath = fs::path(current_dir) / fname;

    nlohmann::json j;
    j["root"]["name"] = fs::path(fname).stem().string();
    j["root"]["id"] = 1;
    j["root"]["active"] = true;
    j["root"]["components"]["Transform"] = {
        {"x",0.f},{"y",0.f},{"rotation",0.f},{"scale_x",1.f},{"scale_y",1.f},{"parent",-1}
    };

    try {
        std::ofstream f(fpath);
        if (!f.is_open()) throw std::runtime_error("open failed");
        f << j.dump(2);
        st.select_asset(fpath.string());
        st.log("Created prefab: " + fname);
    } catch (...) {
        st.log_error("Could not create prefab: " + fpath.string());
    }
}

inline bool handle_viewport_prefab_drop(const char* payload_str,
                                        float drop_world_x, float drop_world_y,
                                        EditorState& st) {
    std::string fname = payload_str ? payload_str : "";
    if (fs::path(fname).extension() != ".prefab") return false;

    std::string abs_path = (fs::path(st.asset_dir) / fname).string();
    if (!fs::exists(abs_path)) abs_path = fname;

    st.undo.push_deep(st.entities);
    int new_id = prefab::instantiate(abs_path, st, drop_world_x, drop_world_y);
    if (new_id >= 0) {
        st.select(new_id);
        st.log_success("Instantiated: " + fs::path(abs_path).filename().string()
                        + " at (" + std::to_string((int)drop_world_x) + ", "
                        + std::to_string((int)drop_world_y) + ")");
        return true;
    }
    st.log_error("Prefab instantiate failed: " + abs_path);
    return false;
}

// Same ASSET_PATH payload handling as handle_viewport_prefab_drop, but for
// drops landing on the Hierarchy panel (a tree row, or the Scene Root zone)
// instead of the viewport. There's no drop world-position here, so the new
// instance spawns at the origin (0,0) — matching Unity's behavior when you
// drag a prefab from the Project window onto the Hierarchy. If target_eid
// is >= 0, the new instance is reparented under it (e.g. dropped directly
// onto an existing row); -1 means drop at scene root.
inline bool handle_hierarchy_prefab_drop(const char* payload_str,
                                         int target_eid,
                                         EditorState& st) {
    std::string fname = payload_str ? payload_str : "";
    if (fs::path(fname).extension() != ".prefab") return false;

    std::string abs_path = (fs::path(st.asset_dir) / fname).string();
    if (!fs::exists(abs_path)) abs_path = fname;

    st.undo.push_deep(st.entities);
    int new_id = prefab::instantiate(abs_path, st, 0.f, 0.f);
    if (new_id < 0) {
        st.log_error("Prefab instantiate failed: " + abs_path);
        return false;
    }

    if (target_eid >= 0) st.reparent(new_id, target_eid, /*keep_world_position=*/true);

    st.select(new_id);
    st.log_success("Instantiated: " + fs::path(abs_path).filename().string()
                    + (target_eid >= 0 ? " under selected entity" : " at Scene Root"));
    return true;
}

inline bool field_context_menu(Entity& e,
                              const std::string& comp,
                              const std::string& field,
                              EditorState& st) {
    if (!prefab::is_instance(e)) return false;
    if (!prefab::is_field_overridden(e, comp, field)) return false;
    std::string popup_id = "##pfov_" + comp + "_" + field;
    if (ImGui::BeginPopupContextItem(popup_id.c_str())) {
        ImGui::Text("Override: %s.%s", comp.c_str(), field.c_str());
        if (ImGui::MenuItem("Revert to Prefab Value")) {
            st.undo.push_deep(st.entities);
            prefab::revert_field(e, comp, field);
            ImGui::EndPopup();
            return true;
        }
        ImGui::EndPopup();
    }
    return false;
}

} // namespace prefab_ui
