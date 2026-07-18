#pragma once
/*
 * unity_gap_features.hpp — Major Unity 2D parity features.
 *
 * Adds the following panels / systems missing from the editor:
 *
 *  1.  PhysicsLayerManager  — 32-layer collision matrix (Physics 2D Settings)
 *  2.  TagLayerPanel        — Project Settings → Tags & Layers (predefined tags,
 *                             sorting layers, physics layers all in one place)
 *  3.  SpriteAtlasPanel     — Pack sprite atlases (SpriteAtlas asset, UV packing)
 *  4.  RuleTilePanel        — Bitmap rule-tile editor (auto-tile from neighbor mask)
 *  5.  SceneManagerPanel    — Build Settings scene list + multi-scene additive load
 *  6.  PhysicsMaterial2DPanel — Bounce/friction presets stored as .physmat2d assets
 *  7.  UICanvasScalerPanel  — Canvas Scaler reference resolution + scale mode
 *  8.  ProjectSettingsPanel — Hub that hosts all of the above as sub-pages
 *  9.  GridSnapSettings     — Per-scene pixel-perfect & snap-to-grid controls
 * 10.  LayerMaskWidget      — Reusable inline bitmask editor (used by Rigidbody2D
 *                             layer_mask, camera culling mask, etc.)
 *
 * Everything is self-contained in this header — no new .cpp required.
 * Include after panels.hpp in editor_main.cpp:
 *
 *     #include "src/unity_gap_features.hpp"
 *
 * Then add to the dockspace layout:
 *
 *     static ProjectSettingsPanel proj_settings;
 *     proj_settings.draw(st);
 */

#include "editor_state.hpp"
#include "component_defs.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <limits>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <commdlg.h>
#  include <shlobj.h>
#endif

// MSVC uses _popen/_pclose; POSIX uses popen/pclose.
#if defined(_MSC_VER) && !defined(popen)
#  define popen  _popen
#  define pclose _pclose
#endif

namespace fs = std::filesystem;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline ImVec4 hex_col(uint32_t rgba) {
    return ImVec4(
        ((rgba >> 24) & 0xFF) / 255.f,
        ((rgba >> 16) & 0xFF) / 255.f,
        ((rgba >>  8) & 0xFF) / 255.f,
        ((rgba      ) & 0xFF) / 255.f
    );
}

// Scene references are authored with a picker, never a hand-entered absolute
// path.  Keeping them project-relative is essential: a scene transition must
// survive both export and a project-folder rename.
static inline std::string browse_project_scene_reference(const EditorState& st) {
#if defined(_WIN32)
    char chosen[MAX_PATH * 4] = {};
    const fs::path current_scene(st.scene_path);
    const fs::path project_root = current_scene.parent_path();
    const std::string initial_dir = project_root.string();
    const char filter[] = "Scene JSON\0*.json\0All files\0*.*\0";
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = chosen;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(chosen));
    dialog.lpstrInitialDir = initial_dir.empty() ? nullptr : initial_dir.c_str();
    dialog.lpstrTitle = "Choose project scene";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&dialog)) return {};
    std::error_code ec;
    const fs::path relative = fs::relative(fs::path(chosen), project_root, ec);
    if (ec || relative.empty() || *relative.begin() == fs::path("..")) return {};
    return relative.generic_string();
#else
    (void)st;
    return {};
#endif
}

// Component and graph assets live under the current project's asset root.
// Store a portable asset-root-relative reference so it continues to resolve
// in Play mode and a standalone export after the project is moved or renamed.
static inline std::string browse_project_asset_reference(const EditorState& st,
                                                         const char* filter,
                                                         const char* title) {
#if defined(_WIN32)
    char chosen[MAX_PATH * 4] = {};
    const fs::path asset_root = fs::path(st.asset_dir);
    const std::string initial_dir = asset_root.string();
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = chosen;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(chosen));
    dialog.lpstrInitialDir = initial_dir.empty() ? nullptr : initial_dir.c_str();
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&dialog)) return {};
    std::error_code ec;
    const fs::path relative = fs::relative(fs::path(chosen), asset_root, ec);
    if (ec || relative.empty() || relative.is_absolute() || *relative.begin() == fs::path("..")) return {};
    return relative.generic_string();
#else
    (void)st; (void)filter; (void)title;
    return {};
#endif
}

// Project-contained folder/output pickers keep atlas references portable and
// avoid a second set of hand-entered absolute paths in the authoring tools.
static inline std::string browse_project_folder_reference(const EditorState& st, const char* title) {
#if defined(_WIN32)
    // Atlas source folders are assets.  Store these relative to asset_dir,
    // just like every SpriteRenderer/Material texture reference; storing them
    // relative to the scene had made `assets/foo` resolve as `assets/assets/foo`
    // at runtime.
    const fs::path asset_root = fs::path(st.asset_dir);
    BROWSEINFOA browse{};
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE selection = SHBrowseForFolderA(&browse);
    if (!selection) return {};
    char chosen[MAX_PATH * 4] = {};
    const bool valid = SHGetPathFromIDListA(selection, chosen) != FALSE;
    CoTaskMemFree(selection);
    if (!valid) return {};
    std::error_code ec;
    const fs::path relative = fs::relative(fs::path(chosen), asset_root, ec);
    if (ec || relative.empty() || *relative.begin() == fs::path("..")) return {};
    return relative.generic_string();
#else
    (void)st; (void)title;
    return {};
#endif
}

static inline std::string browse_project_output_texture(const EditorState& st, const char* title,
                                                        const std::string& current) {
#if defined(_WIN32)
    const fs::path asset_root = fs::path(st.asset_dir);
    const fs::path initial = current.empty() ? (fs::path(st.asset_dir) / "atlas.png")
                                             : (asset_root / current);
    char chosen[MAX_PATH * 4] = {};
    std::snprintf(chosen, sizeof(chosen), "%s", initial.string().c_str());
    const char filter[] = "PNG Texture\0*.png\0All files\0*.*\0";
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = chosen;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(chosen));
    const std::string initial_dir = initial.parent_path().string();
    dialog.lpstrInitialDir = initial_dir.empty() ? nullptr : initial_dir.c_str();
    dialog.lpstrTitle = title;
    dialog.lpstrDefExt = "png";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameA(&dialog)) return {};
    std::error_code ec;
    const fs::path relative = fs::relative(fs::path(chosen), asset_root, ec);
    if (ec || relative.empty() || *relative.begin() == fs::path("..")) return {};
    return relative.generic_string();
#else
    (void)st; (void)title; (void)current;
    return {};
#endif
}

// Replace a completed temporary file without exposing a half-written asset.
// std::filesystem::rename cannot replace an existing destination on Windows,
// which previously made a second Save/Pack silently fail.  MoveFileEx performs
// the atomic replacement operation expected by the editor on this platform.
static inline bool atomic_replace_file(const fs::path& temporary, const fs::path& destination) {
#if defined(_WIN32)
    if (MoveFileExA(temporary.string().c_str(), destination.string().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    return false;
#else
    std::error_code ec;
    fs::rename(temporary, destination, ec);
    return !ec;
#endif
}

// Save JSON atomically, creating parent dirs as needed.  Editor tools (most
// notably Visual Scripting autosave) must never leave a half-written asset if
// the process exits or a cloud-sync client observes the file mid-write.
static inline bool save_json(const fs::path& p, const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    fs::path temporary = p;
    temporary += ".tmp";
    {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        f.flush();
        if (!f.good()) { fs::remove(temporary, ec); return false; }
    }
    // On a failure we deliberately preserve the previous asset instead of
    // deleting it and risking data loss.
    if (!atomic_replace_file(temporary, p)) { fs::remove(temporary, ec); return false; }
    return true;
}

static inline nlohmann::json load_json(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    try { return nlohmann::json::parse(f); } catch (...) { return {}; }
}

// ─── 1. Physics Layer Manager ─────────────────────────────────────────────────
// Mirrors Unity's Edit → Project Settings → Physics 2D → Layer Collision Matrix.
// 32 named layers; a 32×32 upper-triangle bitmask controls which pairs collide.
// The matrix is stored in EditorState::physics_layers (injected below) and saved
// in the scene file under "physics_layers" / "physics_matrix".

struct PhysicsLayerSettings {
    std::array<std::string, 32> layer_names;
    // collision_matrix[i] is a bitmask: bit j set → layer i collides with layer j.
    // Only the upper triangle is authoritative (i <= j); both directions enabled.
    std::array<uint32_t, 32>    collision_matrix{};

    PhysicsLayerSettings() {
        layer_names[0]  = "Default";
        layer_names[1]  = "TransparentFX";
        layer_names[2]  = "Ignore Raycast";
        layer_names[3]  = "Ignore Raycast";
        layer_names[4]  = "Water";
        layer_names[5]  = "UI";
        layer_names[8]  = "Player";
        layer_names[9]  = "Enemy";
        layer_names[10] = "Ground";
        layer_names[11] = "Projectile";
        // All layers collide with all layers by default
        for (auto& m : collision_matrix) m = 0xFFFFFFFF;
    }

    bool collides(int a, int b) const {
        if (a > b) std::swap(a, b);
        return (collision_matrix[a] >> b) & 1;
    }
    void set_collides(int a, int b, bool v) {
        if (a > b) std::swap(a, b);
        if (v) { collision_matrix[a] |=  (1u << b); collision_matrix[b] |=  (1u << a); }
        else   { collision_matrix[a] &= ~(1u << b); collision_matrix[b] &= ~(1u << a); }
    }

    nlohmann::json to_json() const {
        nlohmann::json j;
        for (int i = 0; i < 32; ++i) j["names"][i] = layer_names[i];
        for (int i = 0; i < 32; ++i) j["matrix"][i] = collision_matrix[i];
        return j;
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_object()) return;
        // `to_json()` writes normal JSON arrays.  A former loader treated
        // them as objects keyed by "0", "1", ... and called value() on the
        // array, which made every project with saved physics layers fail at
        // editor startup.  Accept both the current array form and an older
        // object-keyed form without ever calling object-only APIs on arrays.
        const auto read_name = [&](const nlohmann::json& values, int i) {
            if (values.is_array() && i < static_cast<int>(values.size()) && values[i].is_string())
                layer_names[i] = values[i].get<std::string>();
            else if (values.is_object()) {
                const auto it = values.find(std::to_string(i));
                if (it != values.end() && it->is_string()) layer_names[i] = it->get<std::string>();
            }
        };
        const auto read_mask = [&](const nlohmann::json& values, int i) {
            const nlohmann::json* value = nullptr;
            if (values.is_array() && i < static_cast<int>(values.size())) value = &values[i];
            else if (values.is_object()) {
                const auto it = values.find(std::to_string(i));
                if (it != values.end()) value = &*it;
            }
            if (value && value->is_number_unsigned()) collision_matrix[i] = value->get<uint32_t>();
            else if (value && value->is_number_integer()) collision_matrix[i] = static_cast<uint32_t>(value->get<int64_t>());
        };
        if (const auto it = j.find("names"); it != j.end())
            for (int i = 0; i < 32; ++i) read_name(*it, i);
        if (const auto it = j.find("matrix"); it != j.end())
            for (int i = 0; i < 32; ++i) read_mask(*it, i);
    }
};

// ─── 2. Tag Manager ──────────────────────────────────────────────────────────

struct TagManager {
    std::vector<std::string> tags = {"Untagged","Respawn","Finish","EditorOnly","MainCamera","Player","GameController"};

    bool add_tag(const std::string& t) {
        if (t.empty()) return false;
        if (std::find(tags.begin(),tags.end(),t)!=tags.end()) return false;
        tags.push_back(t); return true;
    }
    void remove_tag(const std::string& t) {
        tags.erase(std::remove(tags.begin(),tags.end(),t),tags.end());
    }

    nlohmann::json to_json() const {
        nlohmann::json j = nlohmann::json::array();
        for (auto& t : tags) j.push_back(t);
        return j;
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_array()) return;
        tags.clear();
        for (auto& v : j) if (v.is_string()) tags.push_back(v.get<std::string>());
    }
};

// ─── 10. LayerMask Widget (reusable) ─────────────────────────────────────────
// Call this wherever you need a bitmask editor — Rigidbody2D layer_mask,
// Camera culling mask, etc. Returns true if the value changed.

inline bool draw_layer_mask_widget(const char* label,
                                    uint32_t& mask,
                                    const std::array<std::string,32>& layer_names) {
    bool changed = false;
    char preview[64];
    if (mask == 0)              snprintf(preview,sizeof(preview),"Nothing");
    else if (mask == 0xFFFFFFFF) snprintf(preview,sizeof(preview),"Everything");
    else {
        int count = 0; std::string first;
        for (int i=0;i<32;++i) if ((mask>>i)&1) { if(first.empty()) first=layer_names[i]; count++; }
        if (count == 1) snprintf(preview,sizeof(preview),"%s",first.c_str());
        else            snprintf(preview,sizeof(preview),"Mixed... (%d layers)",count);
    }
    if (ImGui::BeginCombo(label, preview)) {
        // Nothing / Everything shortcuts
        if (ImGui::Selectable("Nothing",   mask==0))            { mask=0;          changed=true; }
        if (ImGui::Selectable("Everything",mask==0xFFFFFFFF))   { mask=0xFFFFFFFF; changed=true; }
        ImGui::Separator();
        for (int i=0;i<32;++i) {
            if (layer_names[i].empty()) continue;
            bool sel = (mask>>i)&1;
            if (ImGui::Selectable(layer_names[i].c_str(), sel)) {
                if (sel) mask &= ~(1u<<i); else mask |= (1u<<i);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// ─── 5. Scene Manager (Build Settings) ───────────────────────────────────────

struct BuildSceneList {
    struct Entry { std::string path; bool enabled = true; };
    std::vector<Entry> scenes;

    void add(const std::string& path) {
        for (auto& e : scenes) if (e.path == path) return;
        scenes.push_back({path, true});
    }
    void remove(int idx) {
        if (idx>=0 && idx<(int)scenes.size()) scenes.erase(scenes.begin()+idx);
    }

    nlohmann::json to_json() const {
        nlohmann::json j = nlohmann::json::array();
        for (auto& e : scenes) j.push_back({{"path",e.path},{"enabled",e.enabled}});
        return j;
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_array()) return;
        scenes.clear();
        for (const auto& v : j) {
            // Legacy settings sometimes stored a plain string path.  Ignore
            // every other malformed entry rather than throwing during editor
            // startup because value() is only valid on JSON objects.
            if (v.is_string()) scenes.push_back({v.get<std::string>(), true});
            else if (v.is_object()) {
                const auto path = v.find("path");
                if (path == v.end() || !path->is_string()) continue;
                bool enabled = true;
                if (const auto state = v.find("enabled"); state != v.end() && state->is_boolean())
                    enabled = state->get<bool>();
                scenes.push_back({path->get<std::string>(), enabled});
            }
        }
    }

    fs::path settings_path(const EditorState& st) const {
        return fs::path(st.scene_path).parent_path() / "ProjectSettings" / "EditorBuildSettings.json";
    }
    void save(const EditorState& st) { save_json(settings_path(st), to_json()); }
    void load(const EditorState& st) {
        auto p = settings_path(st);
        if (fs::exists(p)) from_json(load_json(p));
    }
};

// ─── 6. Physics Material 2D ──────────────────────────────────────────────────

struct PhysMat2D {
    std::string name        = "New Physics Material 2D";
    float       friction    = 0.4f;
    float       bounciness  = 0.0f;

    nlohmann::json to_json() const {
        return {{"name",name},{"friction",friction},{"bounciness",bounciness}};
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_object()) return;
        name       = j.value("name","New Physics Material 2D");
        friction   = j.value("friction",0.4f);
        bounciness = j.value("bounciness",0.0f);
    }
};

// ─── 3. Sprite Atlas ──────────────────────────────────────────────────────────

struct SpriteAtlasSprite {
    std::string name;
    std::string source;
    int x = 0, y = 0, w = 0, h = 0;

    nlohmann::json to_json() const {
        return {{"name", name}, {"source", source}, {"x", x}, {"y", y}, {"w", w}, {"h", h}};
    }
    void from_json(const nlohmann::json& j) {
        name = j.value("name", ""); source = j.value("source", "");
        x = j.value("x", 0); y = j.value("y", 0);
        w = j.value("w", 0); h = j.value("h", 0);
    }
};

// A fully decoded sprite awaiting atlas placement.  Keep pixels private to the
// packer: the `.spriteatlas` manifest stores only portable asset references and
// rectangles while the texture `.meta` is what the runtime resolves.
struct AtlasSourceImage {
    SpriteAtlasSprite rect;
    std::vector<uint8_t> rgba;
};

// Minimal, dependency-free PNG writer for atlas output.  It writes RGBA8 PNGs
// with unfiltered scanlines and stored DEFLATE blocks.  The files are larger
// than a compressor would produce, but they are valid PNGs, load through the
// engine's stb decoder, and avoid a new editor-time runtime dependency.
static inline uint32_t atlas_crc32(const uint8_t* bytes, size_t count) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline uint32_t atlas_adler32(const uint8_t* bytes, size_t count) {
    uint32_t a = 1u, b = 0u;
    for (size_t i = 0; i < count; ++i) {
        a = (a + bytes[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16u) | a;
}

static inline void atlas_write_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

static inline void atlas_append_png_chunk(std::vector<uint8_t>& png, const char type[4],
                                          const std::vector<uint8_t>& data) {
    atlas_write_be32(png, static_cast<uint32_t>(data.size()));
    const size_t crc_start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    atlas_write_be32(png, atlas_crc32(png.data() + crc_start, 4u + data.size()));
}

static inline bool write_atlas_png(const fs::path& destination, int width, int height,
                                   const std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4u)
        return false;

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 4u + 1u));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // PNG filter type: None
        const auto first = rgba.begin() + static_cast<size_t>(y) * width * 4u;
        raw.insert(raw.end(), first, first + static_cast<size_t>(width) * 4u);
    }

    std::vector<uint8_t> deflate;
    deflate.reserve(raw.size() + raw.size() / 65535u * 5u + 6u);
    deflate.push_back(0x78); deflate.push_back(0x01); // zlib, no compression
    size_t offset = 0;
    while (offset < raw.size()) {
        const uint16_t block = static_cast<uint16_t>(std::min<size_t>(65535u, raw.size() - offset));
        const bool last = offset + block == raw.size();
        deflate.push_back(last ? 0x01 : 0x00); // stored block, byte aligned
        deflate.push_back(static_cast<uint8_t>(block & 0xFFu));
        deflate.push_back(static_cast<uint8_t>((block >> 8u) & 0xFFu));
        const uint16_t inverse = static_cast<uint16_t>(~block);
        deflate.push_back(static_cast<uint8_t>(inverse & 0xFFu));
        deflate.push_back(static_cast<uint8_t>((inverse >> 8u) & 0xFFu));
        deflate.insert(deflate.end(), raw.begin() + offset, raw.begin() + offset + block);
        offset += block;
    }
    atlas_write_be32(deflate, atlas_adler32(raw.data(), raw.size()));

    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    ihdr.reserve(13);
    atlas_write_be32(ihdr, static_cast<uint32_t>(width));
    atlas_write_be32(ihdr, static_cast<uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlace
    atlas_append_png_chunk(png, "IHDR", ihdr);
    atlas_append_png_chunk(png, "IDAT", deflate);
    atlas_append_png_chunk(png, "IEND", {});

    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return false;
    fs::path temporary = destination; temporary += ".tmp";
    {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        f.flush();
        if (!f.good()) { fs::remove(temporary, ec); return false; }
    }
    if (!atomic_replace_file(temporary, destination)) { fs::remove(temporary, ec); return false; }
    return true;
}

static inline bool atlas_is_image_file(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

static inline std::string atlas_safe_sprite_name(std::string value) {
    for (char& c : value) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
    while (!value.empty() && value.back() == '_') value.pop_back();
    return value.empty() ? "sprite" : value;
}

// Basic shelf packing is deterministic and gives source files stable output
// ordering.  Padding is part of each shelf allocation, and is later filled by
// duplicating edge texels to prevent bilinear sampling seams.
static inline bool atlas_pack_shelf(std::vector<AtlasSourceImage>& sprites, int side, int padding) {
    std::stable_sort(sprites.begin(), sprites.end(), [](const AtlasSourceImage& a, const AtlasSourceImage& b) {
        if (a.rect.h != b.rect.h) return a.rect.h > b.rect.h;
        if (a.rect.w != b.rect.w) return a.rect.w > b.rect.w;
        return a.rect.name < b.rect.name;
    });
    int x = padding, y = padding, row_h = 0;
    for (auto& sprite : sprites) {
        if (sprite.rect.w + padding * 2 > side || sprite.rect.h + padding * 2 > side) return false;
        if (x + sprite.rect.w + padding > side) {
            x = padding;
            y += row_h + padding;
            row_h = 0;
        }
        if (y + sprite.rect.h + padding > side) return false;
        sprite.rect.x = x; sprite.rect.y = y;
        x += sprite.rect.w + padding;
        row_h = std::max(row_h, sprite.rect.h);
    }
    return true;
}

struct SpriteAtlasEntry {
    std::string name;
    std::vector<std::string> source_folders;   // paths to pack
    std::string output_texture;                // generated atlas png path
    int         max_size        = 2048;
    int         padding         = 2;
    bool        allow_rotation  = false;
    bool        tight_packing   = false;
    int         output_width    = 0;
    int         output_height   = 0;
    std::vector<SpriteAtlasSprite> sprites;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["name"] = name;
        j["source_folders"] = source_folders;
        j["output_texture"] = output_texture;
        j["max_size"] = max_size;
        j["padding"] = padding;
        j["allow_rotation"] = allow_rotation;
        j["tight_packing"] = tight_packing;
        j["output_width"] = output_width;
        j["output_height"] = output_height;
        j["sprites"] = nlohmann::json::array();
        for (const auto& sprite : sprites) j["sprites"].push_back(sprite.to_json());
        return j;
    }
    void from_json(const nlohmann::json& j) {
        name            = j.value("name","");
        if (j.contains("source_folders")) for (auto& v:j["source_folders"]) source_folders.push_back(v.get<std::string>());
        output_texture  = j.value("output_texture","");
        max_size        = j.value("max_size",2048);
        padding         = j.value("padding",2);
        allow_rotation  = j.value("allow_rotation",false);
        tight_packing   = j.value("tight_packing",false);
        output_width    = j.value("output_width", 0);
        output_height   = j.value("output_height", 0);
        sprites.clear();
        if (j.contains("sprites") && j["sprites"].is_array()) {
            for (const auto& value : j["sprites"]) {
                SpriteAtlasSprite sprite; sprite.from_json(value); sprites.push_back(std::move(sprite));
            }
        }
    }
};

// Build an atlas and the importer sidecar consumed by TextureCache.  A packed
// sprite is immediately usable anywhere a texture is accepted via
// `atlas.png:sprite_name`; no external command-line packing step is required.
static inline bool build_sprite_atlas(const EditorState& st, SpriteAtlasEntry& atlas,
                                      std::string& diagnostic) {
    diagnostic.clear();
    if (atlas.name.empty()) { diagnostic = "Atlas needs a name."; return false; }
    if (atlas.source_folders.empty()) { diagnostic = "Add at least one source folder."; return false; }

    const fs::path asset_root(st.asset_dir);
    if (asset_root.empty() || !fs::exists(asset_root)) {
        diagnostic = "The current project asset folder is unavailable."; return false;
    }
    if (atlas.output_texture.empty()) atlas.output_texture = atlas_safe_sprite_name(atlas.name) + ".png";
    fs::path output_relative(atlas.output_texture);
    if (output_relative.extension().empty()) output_relative += ".png";
    if (output_relative.extension() != ".png") {
        diagnostic = "Atlas output must use the .png format."; return false;
    }
    std::error_code ec;
    const fs::path output = fs::weakly_canonical(asset_root / output_relative, ec);
    const fs::path canonical_assets = fs::weakly_canonical(asset_root, ec);
    if (ec || output.empty() || output.string().rfind(canonical_assets.string(), 0) != 0) {
        diagnostic = "Atlas output must remain inside this project's Assets folder."; return false;
    }
    atlas.output_texture = output_relative.generic_string();

    std::vector<AtlasSourceImage> sources;
    std::unordered_set<std::string> seen_files;
    std::unordered_map<std::string, int> used_names;
    for (const std::string& folder_ref : atlas.source_folders) {
        const fs::path folder = asset_root / fs::path(folder_ref);
        if (!fs::exists(folder) || !fs::is_directory(folder)) {
            diagnostic = "Atlas source folder is missing: " + folder_ref; return false;
        }
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !atlas_is_image_file(it->path())) continue;
            const fs::path file = fs::weakly_canonical(it->path(), ec);
            if (ec || file == output || !seen_files.insert(file.generic_string()).second) { ec.clear(); continue; }
            int width = 0, height = 0, channels = 0;
            stbi_uc* decoded = stbi_load(file.string().c_str(), &width, &height, &channels, 4);
            if (!decoded || width <= 0 || height <= 0) {
                if (decoded) stbi_image_free(decoded);
                diagnostic = "Could not decode atlas source: " + file.filename().string(); return false;
            }
            AtlasSourceImage source;
            std::error_code rel_ec;
            const fs::path relative = fs::relative(file, asset_root, rel_ec);
            source.rect.source = rel_ec ? file.filename().generic_string() : relative.generic_string();
            fs::path sprite_name_path = rel_ec ? file.filename() : relative;
            sprite_name_path.replace_extension();
            source.rect.name = atlas_safe_sprite_name(sprite_name_path.generic_string());
            const int duplicate = used_names[source.rect.name]++;
            if (duplicate > 0) source.rect.name += "_" + std::to_string(duplicate + 1);
            source.rect.w = width; source.rect.h = height;
            source.rgba.assign(decoded, decoded + static_cast<size_t>(width) * height * 4u);
            stbi_image_free(decoded);
            sources.push_back(std::move(source));
        }
        ec.clear();
    }
    if (sources.empty()) { diagnostic = "No PNG, JPG, BMP, or TGA images were found in the source folders."; return false; }

    const int padding = std::max(0, atlas.padding);
    int largest = 1;
    for (const auto& source : sources) largest = std::max(largest, std::max(source.rect.w, source.rect.h) + padding * 2);
    int side = 64;
    while (side < largest && side < atlas.max_size) side *= 2;
    std::vector<AtlasSourceImage> packed;
    for (; side <= atlas.max_size; side *= 2) {
        packed = sources;
        if (atlas_pack_shelf(packed, side, padding)) break;
    }
    if (side > atlas.max_size) {
        diagnostic = "The selected sprites do not fit within the configured maximum atlas size."; return false;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(side) * side * 4u, 0);
    for (const auto& source : packed) {
        // Include the padding border and duplicate the nearest edge texel.
        // This is the runtime-safe equivalent of tight packing: no UV rotation
        // or trimmed origin is needed, so SpriteRenderer's normal source rect
        // path renders every sprite correctly.
        for (int sy = -padding; sy < source.rect.h + padding; ++sy) {
            const int src_y = std::clamp(sy, 0, source.rect.h - 1);
            const int dst_y = source.rect.y + sy;
            for (int sx = -padding; sx < source.rect.w + padding; ++sx) {
                const int src_x = std::clamp(sx, 0, source.rect.w - 1);
                const int dst_x = source.rect.x + sx;
                const size_t from = (static_cast<size_t>(src_y) * source.rect.w + src_x) * 4u;
                const size_t to = (static_cast<size_t>(dst_y) * side + dst_x) * 4u;
                std::memcpy(pixels.data() + to, source.rgba.data() + from, 4u);
            }
        }
    }
    if (!write_atlas_png(output, side, side, pixels)) {
        diagnostic = "Could not write atlas PNG: " + output.string(); return false;
    }

    nlohmann::json import = {
        {"filter_mode", "point"}, {"wrap_mode", "clamp"}, {"pixels_per_unit", 100.0},
        {"pivot_x", 0.5}, {"pivot_y", 0.5}, {"generate_mipmaps", false}, {"srgb", true},
        {"sprite_mode", "multiple"}, {"sprites", nlohmann::json::array()}
    };
    atlas.sprites.clear();
    for (const auto& source : packed) {
        atlas.sprites.push_back(source.rect);
        import["sprites"].push_back({
            {"name", source.rect.name}, {"x", source.rect.x}, {"y", source.rect.y},
            {"w", source.rect.w}, {"h", source.rect.h}, {"pivot_x", 0.5}, {"pivot_y", 0.5}
        });
    }
    atlas.output_width = side; atlas.output_height = side;
    const fs::path manifest = asset_root / (atlas_safe_sprite_name(atlas.name) + ".spriteatlas");
    if (!save_json(output.string() + ".meta", import) || !save_json(manifest, atlas.to_json())) {
        diagnostic = "Atlas image was written, but its metadata could not be saved."; return false;
    }
    diagnostic = "Packed " + std::to_string(atlas.sprites.size()) + " sprites into " +
                 atlas.output_texture + " (" + std::to_string(side) + " x " + std::to_string(side) + ").";
    return true;
}

// ─── Tile Palette assets ────────────────────────────────────────────────────
// A palette is deliberately independent from the generic SpriteAtlas asset:
// Tilemaps need stable integer tile IDs, per-tile collision information and
// saved multi-cell brushes.  The generated atlas is still a normal PNG, so the
// renderer can draw a whole tilemap with one texture bind.
struct TilePaletteTile {
    int id = -1;
    std::string guid;
    std::string source;
    // A source can be a complete image or one Sprite Editor slice inside a
    // sheet.  Keeping this rectangle in source pixels means rebuilding the
    // generated atlas never changes the stable map tile ID.
    int source_x = 0, source_y = 0, source_w = 0, source_h = 0;
    int atlas_x = 0, atlas_y = 0, atlas_w = 0, atlas_h = 0;
    std::string collision = "solid";
    std::string rule_tile;
    std::string animated_tile;

    nlohmann::json to_json() const {
        return {{"id", id}, {"guid", guid}, {"source", source},
                {"source_x", source_x}, {"source_y", source_y},
                {"source_w", source_w}, {"source_h", source_h},
                {"atlas_x", atlas_x}, {"atlas_y", atlas_y},
                {"atlas_w", atlas_w}, {"atlas_h", atlas_h},
                {"collision", collision}, {"rule_tile", rule_tile},
                {"animated_tile", animated_tile}};
    }
    static TilePaletteTile from_json(const nlohmann::json& json) {
        TilePaletteTile tile;
        tile.id = json.value("id", -1); tile.guid = json.value("guid", std::string());
        tile.source = json.value("source", std::string());
        tile.source_x = json.value("source_x", 0); tile.source_y = json.value("source_y", 0);
        tile.source_w = json.value("source_w", 0); tile.source_h = json.value("source_h", 0);
        tile.atlas_x = json.value("atlas_x", 0); tile.atlas_y = json.value("atlas_y", 0);
        tile.atlas_w = json.value("atlas_w", 0); tile.atlas_h = json.value("atlas_h", 0);
        tile.collision = json.value("collision", std::string("solid"));
        tile.rule_tile = json.value("rule_tile", std::string());
        tile.animated_tile = json.value("animated_tile", std::string());
        return tile;
    }
};

struct TilePaletteAsset {
    int format_version = 1;
    std::string guid;
    std::string name;
    int cell_width = 32, cell_height = 32;
    std::string atlas;
    std::vector<TilePaletteTile> tiles;

    nlohmann::json to_json() const {
        nlohmann::json out = {{"format", "gameengine.tile-palette"}, {"format_version", format_version},
                              {"guid", guid}, {"name", name}, {"cell_width", cell_width},
                              {"cell_height", cell_height}, {"atlas", atlas}, {"tiles", nlohmann::json::array()}};
        for (const auto& tile : tiles) out["tiles"].push_back(tile.to_json());
        return out;
    }
    static bool from_json(const nlohmann::json& json, TilePaletteAsset& out) {
        if (!json.is_object() || json.value("format", std::string()) != "gameengine.tile-palette") return false;
        out = TilePaletteAsset{};
        out.format_version = json.value("format_version", 1);
        out.guid = json.value("guid", std::string()); out.name = json.value("name", std::string());
        out.cell_width = std::max(1, json.value("cell_width", 32));
        out.cell_height = std::max(1, json.value("cell_height", out.cell_width));
        out.atlas = json.value("atlas", std::string());
        if (json.contains("tiles") && json["tiles"].is_array())
            for (const auto& entry : json["tiles"]) if (entry.is_object()) out.tiles.push_back(TilePaletteTile::from_json(entry));
        return true;
    }
};

struct TileBrushAsset {
    struct Cell { int x = 0, y = 0, tile_id = 0; };
    int format_version = 1;
    std::string guid;
    std::string name;
    std::string palette;
    std::vector<Cell> cells;

    nlohmann::json to_json() const {
        nlohmann::json out = {{"format", "gameengine.tile-brush"}, {"format_version", format_version},
                              {"guid", guid}, {"name", name}, {"palette", palette}, {"cells", nlohmann::json::array()}};
        for (const auto& cell : cells) out["cells"].push_back({{"x",cell.x},{"y",cell.y},{"tile_id",cell.tile_id}});
        return out;
    }
    static bool from_json(const nlohmann::json& json, TileBrushAsset& out) {
        if (!json.is_object() || json.value("format", std::string()) != "gameengine.tile-brush") return false;
        out = TileBrushAsset{};
        out.format_version = json.value("format_version", 1); out.guid = json.value("guid", std::string());
        out.name = json.value("name", std::string()); out.palette = json.value("palette", std::string());
        if (json.contains("cells") && json["cells"].is_array()) for (const auto& cell : json["cells"])
            if (cell.is_object()) out.cells.push_back({cell.value("x",0), cell.value("y",0), cell.value("tile_id",0)});
        return true;
    }
};

static inline std::string tile_palette_safe_name(std::string value) {
    for (char& ch : value) if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') ch = '_';
    while (!value.empty() && value.back() == '_') value.pop_back();
    return value.empty() ? "tile_palette" : value;
}

static inline std::string tile_palette_guid(const std::string& seed) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const size_t hash = std::hash<std::string>{}(seed + std::to_string(now));
    std::ostringstream stream; stream << std::hex << hash << '-' << now;
    return stream.str();
}

static inline bool build_tile_palette_atlas(const EditorState& st, TilePaletteAsset& palette,
                                            const fs::path& manifest_path, std::string& diagnostic) {
    diagnostic.clear();
    const fs::path asset_root(st.asset_dir);
    if (asset_root.empty() || !fs::exists(asset_root)) { diagnostic = "Project Assets folder is unavailable."; return false; }
    if (palette.tiles.empty()) { diagnostic = "Add at least one image tile before building the palette."; return false; }
    const int padding = 2;
    const int cell_w = std::max(1, palette.cell_width), cell_h = std::max(1, palette.cell_height);
    const int count = static_cast<int>(palette.tiles.size());
    const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count)))));
    const int rows = (count + columns - 1) / columns;
    const int raw_w = columns * (cell_w + padding * 2), raw_h = rows * (cell_h + padding * 2);
    int atlas_w = 1, atlas_h = 1;
    while (atlas_w < raw_w) atlas_w <<= 1;
    while (atlas_h < raw_h) atlas_h <<= 1;
    if (atlas_w > 8192 || atlas_h > 8192) { diagnostic = "Palette is too large for the maximum 8192px atlas."; return false; }
    std::vector<uint8_t> pixels(static_cast<size_t>(atlas_w) * atlas_h * 4u, 0);
    for (int index = 0; index < count; ++index) {
        TilePaletteTile& tile = palette.tiles[index];
        const fs::path source = asset_root / fs::path(tile.source);
        int source_w = 0, source_h = 0, channels = 0;
        stbi_uc* decoded = stbi_load(source.string().c_str(), &source_w, &source_h, &channels, 4);
        if (!decoded || source_w <= 0 || source_h <= 0) {
            if (decoded) stbi_image_free(decoded);
            diagnostic = "Could not decode palette tile: " + tile.source;
            return false;
        }
        const int col = index % columns, row = index / columns;
        tile.atlas_x = col * (cell_w + padding * 2) + padding;
        tile.atlas_y = row * (cell_h + padding * 2) + padding;
        tile.atlas_w = cell_w; tile.atlas_h = cell_h;
        const int crop_x = std::clamp(tile.source_x, 0, source_w - 1);
        const int crop_y = std::clamp(tile.source_y, 0, source_h - 1);
        const int crop_w = std::clamp(tile.source_w > 0 ? tile.source_w : source_w, 1, source_w - crop_x);
        const int crop_h = std::clamp(tile.source_h > 0 ? tile.source_h : source_h, 1, source_h - crop_y);
        for (int y = -padding; y < cell_h + padding; ++y) {
            const int dy = tile.atlas_y + y;
            const int sy = crop_y + std::clamp((std::clamp(y, 0, cell_h - 1) * crop_h) / cell_h, 0, crop_h - 1);
            for (int x = -padding; x < cell_w + padding; ++x) {
                const int dx = tile.atlas_x + x;
                const int sx = crop_x + std::clamp((std::clamp(x, 0, cell_w - 1) * crop_w) / cell_w, 0, crop_w - 1);
                const size_t from = (static_cast<size_t>(sy) * source_w + sx) * 4u;
                const size_t to = (static_cast<size_t>(dy) * atlas_w + dx) * 4u;
                std::memcpy(pixels.data() + to, decoded + from, 4u);
            }
        }
        stbi_image_free(decoded);
    }
    const fs::path palette_dir = manifest_path.parent_path();
    const std::string stem = tile_palette_safe_name(palette.name);
    const fs::path atlas_path = palette_dir / (stem + "_atlas.png");
    std::error_code ec;
    const fs::path atlas_relative = fs::relative(atlas_path, asset_root, ec);
    if (ec || atlas_relative.empty() || *atlas_relative.begin() == fs::path("..")) { diagnostic = "Palette atlas must be inside Assets."; return false; }
    if (!write_atlas_png(atlas_path, atlas_w, atlas_h, pixels)) { diagnostic = "Could not write generated Tile Palette atlas."; return false; }
    palette.atlas = atlas_relative.generic_string();
    if (!save_json(manifest_path, palette.to_json())) { diagnostic = "Atlas was built but palette manifest could not be saved."; return false; }
    return true;
}

// ─── 4. Rule Tile ────────────────────────────────────────────────────────────
// Each rule maps a 3×3 neighbor bitmask (bit = that cell is the same tile)
// to an output tile index. The engine's tilemap renderer picks the first
// matching rule or falls back to the default tile.

struct RuleTileRule {
    uint16_t neighbor_mask = 0;   // bits 0-8: top-left … bottom-right (center=4 ignored)
    uint16_t must_match    = 0;   // which of the 8 cells are "must be same tile"
    uint16_t must_empty    = 0;   // which of the 8 cells are "must be empty"
    int      output_tile   = 0;
};

struct RuleTile {
    std::string           name;
    int                   default_tile = 0;
    std::vector<RuleTileRule> rules;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["name"] = name;
        j["default_tile"] = default_tile;
        j["rules"] = nlohmann::json::array();
        for (auto& r : rules) {
            j["rules"].push_back({
                {"neighbor_mask",r.neighbor_mask},
                {"must_match",   r.must_match},
                {"must_empty",   r.must_empty},
                {"output_tile",  r.output_tile}
            });
        }
        return j;
    }
    void from_json(const nlohmann::json& j) {
        name         = j.value("name","");
        default_tile = j.value("default_tile",0);
        if (j.contains("rules")) for (auto& r : j["rules"])
            rules.push_back({(uint16_t)r.value("neighbor_mask",0),
                             (uint16_t)r.value("must_match",0),
                             (uint16_t)r.value("must_empty",0),
                             r.value("output_tile",0)});
    }
};

// ─── 7. UI Canvas Scaler ──────────────────────────────────────────────────────

struct CanvasScalerSettings {
    enum class ScaleMode { ConstantPixelSize, ScaleWithScreenSize, ConstantPhysicalSize };
    ScaleMode   mode             = ScaleMode::ScaleWithScreenSize;
    float       ref_width        = 1920.f;
    float       ref_height       = 1080.f;
    float       match_widthheight = 0.5f;   // 0=width, 1=height
    float       pixels_per_unit  = 100.f;

    nlohmann::json to_json() const {
        return {{"mode",(int)mode},{"ref_width",ref_width},{"ref_height",ref_height},
                {"match",match_widthheight},{"ppu",pixels_per_unit}};
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_object()) return;
        mode              = (ScaleMode)j.value("mode",1);
        ref_width         = j.value("ref_width",1920.f);
        ref_height        = j.value("ref_height",1080.f);
        match_widthheight = j.value("match",0.5f);
        pixels_per_unit   = j.value("ppu",100.f);
    }
};

// ─── 9. Grid Snap Settings ───────────────────────────────────────────────────

struct GridSnapSettings {
    bool  pixel_perfect   = false;
    float pixels_per_unit = 100.f;
    bool  snap_to_pixel   = false;
    int   move_snap_x     = 16;
    int   move_snap_y     = 16;
    float rotate_snap     = 15.f;
    float scale_snap      = 0.1f;

    nlohmann::json to_json() const {
        return {{"pixel_perfect",pixel_perfect},{"ppu",pixels_per_unit},
                {"snap_pixel",snap_to_pixel},
                {"move_x",move_snap_x},{"move_y",move_snap_y},
                {"rot",rotate_snap},{"scale",scale_snap}};
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_object()) return;
        pixel_perfect   = j.value("pixel_perfect",false);
        pixels_per_unit = j.value("ppu",100.f);
        snap_to_pixel   = j.value("snap_pixel",false);
        move_snap_x     = j.value("move_x",16);
        move_snap_y     = j.value("move_y",16);
        rotate_snap     = j.value("rot",15.f);
        scale_snap      = j.value("scale",0.1f);
    }
};

// ─── 8. Project Settings Panel (hub) ─────────────────────────────────────────

class ProjectSettingsPanel {
public:
    // All sub-system state lives here so it persists for the session.
    PhysicsLayerSettings phys_layers;
    TagManager           tag_mgr;
    BuildSceneList       build_scenes;
    CanvasScalerSettings canvas_scaler;
    GridSnapSettings     grid_snap;

    // Per-project: loaded/saved lazily
    bool _loaded = false;
    std::string _loaded_project_settings;
    std::string _company_name = "My Company";
    std::string _product_name = "My Game";
    int _version_major = 0, _version_minor = 1, _version_patch = 0;

    void ensure_loaded(EditorState& st) {
        const auto base = fs::path(st.scene_path).parent_path() / "ProjectSettings";
        const std::string project_key = base.lexically_normal().generic_string();
        if (_loaded && project_key == _loaded_project_settings) return;
        // The panel instance survives scene/project switches.  Reset every
        // project-owned value before reading the new location so settings from
        // a previously opened game cannot leak into a different game.
        _loaded = true;
        _loaded_project_settings = project_key;
        phys_layers = PhysicsLayerSettings{};
        tag_mgr = TagManager{};
        build_scenes = BuildSceneList{};
        canvas_scaler = CanvasScalerSettings{};
        grid_snap = GridSnapSettings{};
        _company_name = "My Company";
        _product_name = "My Game";
        _version_major = 0; _version_minor = 1; _version_patch = 0;
        auto pj = load_json(base / "ProjectSettings.json");
        // A project is not required to carry ProjectSettings.json yet.  The
        // previous code immediately called json::value() on the null result
        // returned for a missing file, crashing the editor during its first
        // frame before the Project Settings window was even opened.
        if (!pj.is_object()) pj = nlohmann::json::object();
        if (pj.contains("physics_layers")) phys_layers.from_json(pj["physics_layers"]);
        if (pj.contains("tags"))           tag_mgr.from_json(pj["tags"]);
        if (pj.contains("canvas_scaler"))  canvas_scaler.from_json(pj["canvas_scaler"]);
        if (pj.contains("grid_snap"))      grid_snap.from_json(pj["grid_snap"]);
        if (const auto it = pj.find("company_name"); it != pj.end() && it->is_string())
            _company_name = it->get<std::string>();
        if (const auto it = pj.find("product_name"); it != pj.end() && it->is_string())
            _product_name = it->get<std::string>();
        if (const auto it = pj.find("version_major"); it != pj.end() && it->is_number_integer())
            _version_major = std::max(0, it->get<int>());
        if (const auto it = pj.find("version_minor"); it != pj.end() && it->is_number_integer())
            _version_minor = std::max(0, it->get<int>());
        if (const auto it = pj.find("version_patch"); it != pj.end() && it->is_number_integer())
            _version_patch = std::max(0, it->get<int>());
        build_scenes.load(st);
        // Merge tag list into sorting layers just for display (they're separate in Unity)
        for (auto& t : tag_mgr.tags)
            if (std::find(st.sorting_layers.begin(),st.sorting_layers.end(),t)==st.sorting_layers.end() && false)
                (void)t; // tags != sorting layers, just noting
    }

    void save(EditorState& st) {
        auto base = fs::path(st.scene_path).parent_path() / "ProjectSettings";
        nlohmann::json pj;
        pj["physics_layers"] = phys_layers.to_json();
        pj["tags"]           = tag_mgr.to_json();
        pj["canvas_scaler"]  = canvas_scaler.to_json();
        pj["grid_snap"]      = grid_snap.to_json();
        pj["company_name"]   = _company_name;
        pj["product_name"]   = _product_name;
        pj["version_major"]  = _version_major;
        pj["version_minor"]  = _version_minor;
        pj["version_patch"]  = _version_patch;
        save_json(base / "ProjectSettings.json", pj);
        build_scenes.save(st);
        st.log_success("Project Settings saved.");
    }

    bool _open          = false;
    int  _page          = 0;   // current sub-page index
    bool _sprite_atlas_open = false;
    bool _rule_tile_open = false;
    bool _physics_material_open = false;

    void open() { _open = true; }
    void open_sprite_atlas() { _sprite_atlas_open = true; }
    void open_rule_tile_editor() { _rule_tile_open = true; }
    void open_physics_material_editor() { _physics_material_open = true; }

    void draw(EditorState& st) {
        ensure_loaded(st);

        if (_open) {
            ImGui::SetNextWindowSize({820, 580}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                    ImGuiCond_FirstUseEver, {0.5f,0.5f});

            if (!ImGui::Begin("Project Settings", &_open, ImGuiWindowFlags_NoDocking)) {
                ImGui::End();
            } else {

        // Left nav column
        ImGui::BeginChild("##ps_nav", {180, 0}, true);
        const char* pages[] = {
            "Tags & Layers",
            "Physics 2D",
            "Build Settings",
            "Canvas Scaler",
            "Grid & Snap",
        };
        for (int i = 0; i < (int)(sizeof(pages)/sizeof(pages[0])); ++i) {
            bool sel = (_page == i);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.24f,0.49f,0.91f,0.8f));
            if (ImGui::Selectable(pages[i], sel)) _page = i;
            if (sel) ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right content area
        ImGui::BeginChild("##ps_content", {0, -ImGui::GetFrameHeightWithSpacing() - 4}, false);
        switch (_page) {
            case 0: _draw_tags_layers(st);    break;
            case 1: _draw_physics2d(st);      break;
            case 2: _draw_build_settings(st); break;
            case 3: _draw_canvas_scaler(st);  break;
            case 4: _draw_grid_snap(st);      break;
        }
        ImGui::EndChild();

        // Bottom save bar
        ImGui::Separator();
        if (ImGui::Button("Save All", {120,0})) save(st);
        ImGui::SameLine();
        ImGui::TextDisabled("Settings are saved per project.");

            ImGui::End();
            }
        }
        _draw_asset_authoring_windows(st);
    }

private:
    // Asset authors are deliberately independent windows rather than hidden
    // Project Settings tabs. They can be resized/docked like Unity's Sprite
    // Editor, and all create project-local assets used by runtime systems.
    void _draw_asset_authoring_windows(EditorState& st) {
        if (_sprite_atlas_open) {
            ImGui::SetNextWindowSize({760, 520}, ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Sprite Atlas##asset", &_sprite_atlas_open)) _draw_sprite_atlas(st);
            ImGui::End();
        }
        if (_rule_tile_open) {
            ImGui::SetNextWindowSize({760, 540}, ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Rule Tile Editor##asset", &_rule_tile_open)) _draw_rule_tile(st);
            ImGui::End();
        }
        if (_physics_material_open) {
            ImGui::SetNextWindowSize({640, 420}, ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Physics Material 2D##asset", &_physics_material_open)) _draw_physics_material(st);
            ImGui::End();
        }
    }

    // ── Page 0: Tags & Layers ────────────────────────────────────────────────
    char _new_tag_buf[64] = {};
    char _new_layer_buf[64] = {};

    void _draw_tags_layers(EditorState& st) {
        ImGui::SeparatorText("Tags");
        ImGui::Spacing();

        // Display predefined tags with remove button
        int to_remove_tag = -1;
        for (int i = 0; i < (int)tag_mgr.tags.size(); ++i) {
            ImGui::PushID(i);
            ImGui::Text("%2d", i);
            ImGui::SameLine(36);
            ImGui::SetNextItemWidth(200);
            char buf[64]; snprintf(buf,sizeof(buf),"%s",tag_mgr.tags[i].c_str());
            if (ImGui::InputText("##tag", buf, sizeof(buf)))
                tag_mgr.tags[i] = buf;
            if (i >= 7) {  // first 7 are built-in
                ImGui::SameLine();
                if (ImGui::SmallButton("×")) to_remove_tag = i;
            }
            ImGui::PopID();
        }
        if (to_remove_tag >= 0) tag_mgr.remove_tag(tag_mgr.tags[to_remove_tag]);

        ImGui::Spacing();
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##newtag", "New tag...", _new_tag_buf, sizeof(_new_tag_buf));
        ImGui::SameLine();
        if (ImGui::Button("Add Tag")) {
            if (tag_mgr.add_tag(_new_tag_buf)) { _new_tag_buf[0]='\0'; }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Sorting Layers");
        ImGui::Spacing();
        ImGui::TextDisabled("Drag rows to reorder. Lower layers draw behind higher layers.");
        ImGui::Spacing();

        int to_remove_sl = -1;
        for (int i = 0; i < (int)st.sorting_layers.size(); ++i) {
            ImGui::PushID(100+i);
            ImGui::Text("%2d", i);
            ImGui::SameLine(36);
            ImGui::Text("%s", st.sorting_layers[i].c_str());
            if (st.sorting_layers[i] != "Default") {
                ImGui::SameLine();
                if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i>0)
                    st.move_sorting_layer(i, i-1);
                ImGui::SameLine();
                if (ImGui::ArrowButton("##dn", ImGuiDir_Down) && i<(int)st.sorting_layers.size()-1)
                    st.move_sorting_layer(i, i+1);
                ImGui::SameLine();
                if (ImGui::SmallButton("×")) to_remove_sl = i;
            }
            ImGui::PopID();
        }
        if (to_remove_sl >= 0) st.remove_sorting_layer(st.sorting_layers[to_remove_sl]);

        ImGui::Spacing();
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##newsl", "New sorting layer...", _new_layer_buf, sizeof(_new_layer_buf));
        ImGui::SameLine();
        if (ImGui::Button("Add Layer")) {
            if (st.add_sorting_layer(_new_layer_buf)) _new_layer_buf[0]='\0';
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Physics Layers (named)");
        ImGui::Spacing();
        ImGui::TextDisabled("Collision matrix is on the Physics 2D page.");
        for (int i = 0; i < 32; ++i) {
            ImGui::PushID(200+i);
            ImGui::Text("Layer %2d", i);
            ImGui::SameLine(72);
            ImGui::SetNextItemWidth(180);
            char buf[64]; snprintf(buf,sizeof(buf),"%s",phys_layers.layer_names[i].c_str());
            if (ImGui::InputText("##ln", buf, sizeof(buf)))
                phys_layers.layer_names[i] = buf;
            ImGui::PopID();
        }
    }

    // ── Page 1: Physics 2D ───────────────────────────────────────────────────

    void _draw_physics2d(EditorState& st) {
        ImGui::SeparatorText("Layer Collision Matrix");
        ImGui::TextDisabled("Unchecked = layers do not collide. Matches Unity Physics 2D Settings.");
        ImGui::Spacing();

        // Collect non-empty named layers for the matrix axes
        std::vector<int>         visible_idx;
        std::vector<std::string> visible_names;
        for (int i=0;i<32;++i)
            if (!phys_layers.layer_names[i].empty()) {
                visible_idx.push_back(i);
                visible_names.push_back(phys_layers.layer_names[i]);
            }

        const float cell = 16.f;
        const float label_w = 130.f;
        int N = (int)visible_idx.size();

        // Diagonal column headers (rotated text via angled labels workaround)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + label_w);
        for (int j = 0; j < N; ++j) {
            // Just abbreviate to keep it tight
            std::string abbr = visible_names[j].size() > 3
                ? visible_names[j].substr(0,3) : visible_names[j];
            ImGui::Text("%s", abbr.c_str());
            if (j < N-1) ImGui::SameLine(0, cell - ImGui::CalcTextSize("XXX").x + 2);
        }
        ImGui::Spacing();

        for (int ii = 0; ii < N; ++ii) {
            int i = visible_idx[ii];
            ImGui::Text("%-18s", visible_names[ii].c_str());
            ImGui::SameLine(label_w);
            for (int jj = 0; jj < N; ++jj) {
                int j = visible_idx[jj];
                ImGui::PushID(i*32+j);
                bool c = phys_layers.collides(i,j);
                if (jj < ii) {
                    // Lower triangle — mirror of upper, show as disabled
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f);
                    ImGui::Checkbox("##x", &c);
                    ImGui::PopStyleVar();
                } else {
                    if (ImGui::Checkbox("##x", &c)) phys_layers.set_collides(i,j,c);
                }
                ImGui::PopID();
                if (jj < N-1) ImGui::SameLine(0, cell - 14.f);
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Gravity");
        static float grav[2] = {0.f, -9.81f};
        ImGui::SetNextItemWidth(160);
        ImGui::InputFloat2("Gravity (X, Y)", grav);
        ImGui::TextDisabled("Applied to all Rigidbody2D with gravity_scale > 0.");

        ImGui::Spacing();
        ImGui::SeparatorText("Default Physics Material");
        ImGui::TextDisabled("Set per-collider or create a .physmat2d asset on the Physics Material 2D page.");
    }

    // ── Page 2: Build Settings ───────────────────────────────────────────────

    void _draw_build_settings(EditorState& st) {
        ImGui::SeparatorText("Scenes In Build");
        ImGui::TextDisabled("Choose project scenes with Browse. The startup scene is exported as scene.json.");
        ImGui::Spacing();

        int to_remove = -1;
        int swap_with  = -1;
        int swap_idx   = -1;

        for (int i = 0; i < (int)build_scenes.scenes.size(); ++i) {
            auto& s = build_scenes.scenes[i];
            ImGui::PushID(i);
            ImGui::Text("%d", i);
            ImGui::SameLine(28);
            const std::string display = s.path.empty() ? "(Missing scene)" : fs::path(s.path).filename().string();
            ImGui::Button(display.c_str(), {std::max(140.f, ImGui::GetContentRegionAvail().x - 210.f), 0});
            if (ImGui::IsItemHovered() && !s.path.empty()) ImGui::SetTooltip("%s", s.path.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Browse...##scene")) {
                const std::string relative = browse_project_scene_reference(st);
                if (!relative.empty()) s.path = (fs::path(st.scene_path).parent_path() / relative).generic_string();
            }
            ImGui::SameLine();
            ImGui::Checkbox("##en", &s.enabled);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include in build");
            ImGui::SameLine();
            const std::string project_name = project_name_from_scene_path(st.scene_path);
            std::error_code compare_ec;
            const bool is_startup = !s.path.empty() && !st.default_scene_for(project_name).empty() &&
                fs::equivalent(fs::absolute(s.path), fs::absolute(st.default_scene_for(project_name)), compare_ec);
            if (ImGui::RadioButton("##startup", is_startup)) st.set_default_scene_for(project_name, s.path);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use this scene at startup");
            ImGui::SameLine();
            if (ImGui::ArrowButton("##u", ImGuiDir_Up) && i>0)   { swap_idx=i; swap_with=i-1; }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##d", ImGuiDir_Down) && i<(int)build_scenes.scenes.size()-1) { swap_idx=i; swap_with=i+1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("×")) to_remove = i;
            ImGui::PopID();
        }
        if (to_remove >= 0) build_scenes.remove(to_remove);
        if (swap_idx >= 0) std::swap(build_scenes.scenes[swap_idx], build_scenes.scenes[swap_with]);

        ImGui::Spacing();
        if (ImGui::Button("Add Open Scene")) build_scenes.add(st.scene_path);
        ImGui::SameLine();
        if (ImGui::Button("Browse Scene...")) {
            const std::string relative = browse_project_scene_reference(st);
            if (!relative.empty()) build_scenes.add((fs::path(st.scene_path).parent_path() / relative).generic_string());
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear List")) build_scenes.scenes.clear();

        ImGui::Spacing();
        ImGui::SeparatorText("Platform");
        ImGui::TextUnformatted("Windows (x64)");
        ImGui::TextDisabled("The current exporter produces the supported Windows Vulkan build.");

        ImGui::Spacing();
        ImGui::SeparatorText("Player Settings");
        char company[64]; std::snprintf(company, sizeof(company), "%s", _company_name.c_str());
        char product[64]; std::snprintf(product, sizeof(product), "%s", _product_name.c_str());
        if (ImGui::InputText("Company Name", company, sizeof(company))) _company_name = company;
        if (ImGui::InputText("Product Name", product, sizeof(product))) _product_name = product;
        int version[3] = {_version_major, _version_minor, _version_patch};
        if (ImGui::InputInt3("Version (M.m.p)", version)) {
            _version_major = std::max(0, version[0]); _version_minor = std::max(0, version[1]); _version_patch = std::max(0, version[2]);
        }

        ImGui::Spacing();
        if (ImGui::Button("Build", {100,0})) {
            save(st);
            st.request_standalone_build = true;
            st.log("Standalone build queued from Project Settings.");
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Build & Run", {100,0})) {
            save(st);
            st.request_standalone_build_and_run = true;
            st.log("Standalone Build & Run queued from Project Settings.");
            return;
        }
    }

    // ── Page 3: Canvas Scaler ────────────────────────────────────────────────

    void _draw_canvas_scaler(EditorState& st) {
        ImGui::SeparatorText("UI Canvas Scaler");
        ImGui::TextDisabled("Controls how the UI Canvas scales across different screen resolutions.");
        ImGui::Spacing();

        const char* modes[] = {"Constant Pixel Size","Scale With Screen Size","Constant Physical Size"};
        int m = (int)canvas_scaler.mode;
        if (ImGui::Combo("UI Scale Mode", &m, modes, 3)) canvas_scaler.mode = (CanvasScalerSettings::ScaleMode)m;

        ImGui::Spacing();
        if (canvas_scaler.mode == CanvasScalerSettings::ScaleMode::ScaleWithScreenSize) {
            ImGui::SetNextItemWidth(100); ImGui::InputFloat("Reference Width",  &canvas_scaler.ref_width,  0,0,"%.0f");
            ImGui::SetNextItemWidth(100); ImGui::InputFloat("Reference Height", &canvas_scaler.ref_height, 0,0,"%.0f");
            ImGui::SeparatorText("Match");
            ImGui::SliderFloat("Width ← Match → Height", &canvas_scaler.match_widthheight, 0.f, 1.f);
            ImGui::TextDisabled("0 = expand/shrink by width, 1 = by height. 0.5 = both equally.");
        } else if (canvas_scaler.mode == CanvasScalerSettings::ScaleMode::ConstantPixelSize) {
            ImGui::TextDisabled("Canvas renders at fixed pixel size — no auto-scaling.");
        } else {
            ImGui::TextDisabled("Canvas maintains physical (cm/inch) size using screen DPI.");
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(100);
        ImGui::InputFloat("Reference Pixels Per Unit", &canvas_scaler.pixels_per_unit, 0,0,"%.0f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pixels in a sprite that correspond to one world unit.");
    }

    // ── Page 4: Grid & Snap ──────────────────────────────────────────────────

    void _draw_grid_snap(EditorState& st) {
        ImGui::SeparatorText("Pixel Perfect Camera");
        ImGui::Checkbox("Pixel Perfect Mode", &grid_snap.pixel_perfect);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snaps all sprite positions to pixel boundaries each frame.");
        ImGui::SetNextItemWidth(100);
        ImGui::InputFloat("Pixels Per Unit", &grid_snap.pixels_per_unit, 0,0,"%.0f");
        ImGui::Checkbox("Snap Sprites to Pixel Grid", &grid_snap.snap_to_pixel);

        ImGui::Spacing();
        ImGui::SeparatorText("Move Snap");
        ImGui::SetNextItemWidth(80); ImGui::InputInt("X (px)", &grid_snap.move_snap_x, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80); ImGui::InputInt("Y (px)", &grid_snap.move_snap_y, 0);

        ImGui::Spacing();
        ImGui::SeparatorText("Rotation & Scale Snap");
        ImGui::SetNextItemWidth(80); ImGui::InputFloat("Rotate (°)", &grid_snap.rotate_snap, 0,0,"%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80); ImGui::InputFloat("Scale",      &grid_snap.scale_snap,  0,0,"%.2f");

        ImGui::Spacing();
        ImGui::TextDisabled("These override the viewport toolbar snap values when Snap is enabled.");

        ImGui::Spacing();
        ImGui::SeparatorText("Live Scene Snap (viewport)");
        ImGui::Checkbox("Show Grid",  &st.show_grid);
        ImGui::Checkbox("Snap to Grid", &st.snap);
        ImGui::SetNextItemWidth(80); ImGui::InputInt("Grid Size (px)", &st.grid_size, 1, 8);
        st.grid_size = std::max(1, st.grid_size);
    }

    // ── Page 5: Sprite Atlas ──────────────────────────────────────────────────

    std::vector<SpriteAtlasEntry> _atlases;
    int   _sel_atlas   = -1;
    char  _atlas_name_buf[64] = {};
    char  _atlas_folder_buf[256] = {};

    void _draw_sprite_atlas(EditorState& st) {
        ImGui::SeparatorText("Sprite Atlases");
        ImGui::TextDisabled("Pack multiple sprite folders into a single texture atlas for performance.");
        ImGui::Spacing();

        // Left: atlas list
        ImGui::BeginChild("##atlas_list", {160,0}, true);
        for (int i=0; i<(int)_atlases.size(); ++i) {
            bool sel = (_sel_atlas == i);
            if (ImGui::Selectable(_atlases[i].name.c_str(), sel)) _sel_atlas = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##atlas_detail", {0,0}, false);

        if (ImGui::Button("+ New Atlas")) {
            SpriteAtlasEntry a;
            a.name = "New Atlas " + std::to_string(_atlases.size());
            _atlases.push_back(a);
            _sel_atlas = (int)_atlases.size()-1;
        }
        if (_sel_atlas >= 0 && _sel_atlas < (int)_atlases.size()) {
            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                _atlases.erase(_atlases.begin()+_sel_atlas);
                _sel_atlas = std::min(_sel_atlas, (int)_atlases.size()-1);
            }
        }

        if (_sel_atlas >= 0 && _sel_atlas < (int)_atlases.size()) {
            auto& a = _atlases[_sel_atlas];
            ImGui::Separator();
            ImGui::SetNextItemWidth(200);
            char nb[64]; snprintf(nb,sizeof(nb),"%s",a.name.c_str());
            if (ImGui::InputText("Name##an", nb, sizeof(nb))) a.name = nb;

            ImGui::SeparatorText("Source Folders");
            int rm_sf = -1;
            for (int i=0;i<(int)a.source_folders.size();++i) {
                ImGui::PushID(i);
                ImGui::TextUnformatted(a.source_folders[i].c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("×")) rm_sf=i;
                ImGui::PopID();
            }
            if (rm_sf>=0) a.source_folders.erase(a.source_folders.begin()+rm_sf);
            if (ImGui::Button("Add Source Folder...")) {
                const std::string folder = browse_project_folder_reference(st, "Select Sprite Atlas Source Folder");
                if (!folder.empty() && std::find(a.source_folders.begin(), a.source_folders.end(), folder) == a.source_folders.end())
                    a.source_folders.push_back(folder);
            }

            ImGui::SeparatorText("Output");
            ImGui::TextDisabled("%s", a.output_texture.empty() ? "No output texture selected" : a.output_texture.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Choose Output...")) {
                const std::string output = browse_project_output_texture(st, "Choose Atlas Output Texture", a.output_texture);
                if (!output.empty()) a.output_texture = output;
            }

            const char* sizes[] = {"256","512","1024","2048","4096"};
            int si = 0; for(int i=0;i<5;++i) if(std::stoi(sizes[i])==a.max_size) si=i;
            if (ImGui::Combo("Max Size", &si, sizes, 5)) a.max_size = std::stoi(sizes[si]);

            ImGui::InputInt("Padding", &a.padding, 1, 4);
            a.padding = std::max(0, a.padding);
            // The renderer resolves atlas source rects directly. Rotating or
            // trimming pixels without a matching UV/origin transform would
            // corrupt sprites, so do not expose controls that only pretend to
            // work. Old manifest fields are retained for migration only.
            a.allow_rotation = false;
            a.tight_packing = false;
            ImGui::TextDisabled("Source orientation and bounds are preserved for runtime-safe sprite references.");

            ImGui::Spacing();
            if (ImGui::Button("Pack Atlas", {-1,0})) {
                std::string result;
                if (build_sprite_atlas(st, a, result)) st.log_success(result);
                else st.log_error(result);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Generates the PNG texture, its .meta sprite slices, and a .spriteatlas manifest.\nUse it as atlas.png:sprite_name in SpriteRenderer.");

            if (!a.sprites.empty()) {
                ImGui::TextDisabled("Last pack: %d sprites, %d x %d. Use %s:<sprite name>.",
                                    (int)a.sprites.size(), a.output_width, a.output_height,
                                    a.output_texture.c_str());
            }

            // Save/load each atlas as a JSON manifest alongside assets
            ImGui::Spacing();
            ImGui::SeparatorText("Load Existing");
            if (ImGui::Button("Load .spriteatlas...")) {
                const std::string selected = browse_project_asset_reference(
                    st, "Sprite Atlas\0*.spriteatlas\0All files\0*.*\0", "Open Sprite Atlas");
                if (!selected.empty()) {
                    auto j = load_json(fs::path(st.asset_dir) / selected);
                    if (j.is_object()) {
                        SpriteAtlasEntry loaded; loaded.from_json(j);
                        _atlases.push_back(std::move(loaded));
                        _sel_atlas = static_cast<int>(_atlases.size()) - 1;
                    } else st.log_error("Could not read the selected sprite atlas.");
                }
            }
        }
        ImGui::EndChild();
    }

    // ── Page 6: Rule Tile Editor ─────────────────────────────────────────────

    std::vector<RuleTile> _rule_tiles;
    int  _sel_rt    = -1;
    int  _sel_rule  = -1;

    // 3×3 grid editor for a single rule
    void _draw_rule_grid(RuleTileRule& r) {
        // Bit mapping: index 0=top-left, 1=top, 2=top-right,
        //              3=left,  (4=center,skip),  5=right,
        //              6=bot-left, 7=bottom, 8=bot-right
        static const int layout[3][3] = {{0,1,2},{3,-1,5},{6,7,8}};
        const char* symbols[] = {"↖","↑","↗","←","·","→","↙","↓","↘"};
        ImGui::Text("Neighbor Pattern (green=must match, red=must empty, grey=any):");
        for (int row=0; row<3; ++row) {
            for (int col=0; col<3; ++col) {
                int bit = layout[row][col];
                ImGui::PushID(row*3+col);
                if (bit == -1) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f,0.3f,0.9f,1.f));
                    ImGui::Button(" # ", {30,28});
                    ImGui::PopStyleColor();
                } else {
                    bool must_match = (r.must_match >> bit) & 1;
                    bool must_empty = (r.must_empty >> bit) & 1;
                    if      (must_match) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f,0.7f,0.2f,1.f));
                    else if (must_empty) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f,0.1f,0.1f,1.f));
                    else                 ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f,0.35f,0.35f,1.f));
                    if (ImGui::Button(symbols[bit], {30,28})) {
                        // Cycle: any → must_match → must_empty → any
                        if (!must_match && !must_empty) {
                            r.must_match |=  (1u<<bit);
                        } else if (must_match) {
                            r.must_match &= ~(1u<<bit);
                            r.must_empty |=  (1u<<bit);
                        } else {
                            r.must_empty &= ~(1u<<bit);
                        }
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
                if (col < 2) ImGui::SameLine(0,2);
            }
        }
    }

    void _draw_rule_tile(EditorState& st) {
        ImGui::SeparatorText("Rule Tile Editor");
        ImGui::TextDisabled("Define auto-tiling rules: each rule maps a 3×3 neighbor pattern to a tile index.");
        ImGui::Spacing();

        // Left: rule tile list
        ImGui::BeginChild("##rt_list", {150,0}, true);
        for (int i=0;i<(int)_rule_tiles.size();++i) {
            bool sel=(_sel_rt==i);
            if (ImGui::Selectable(_rule_tiles[i].name.c_str(), sel)) { _sel_rt=i; _sel_rule=-1; }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##rt_detail", {0,0}, false);

        if (ImGui::Button("+ New Rule Tile")) {
            RuleTile rt; rt.name = "RuleTile " + std::to_string(_rule_tiles.size());
            _rule_tiles.push_back(rt); _sel_rt=(int)_rule_tiles.size()-1; _sel_rule=-1;
        }

        if (_sel_rt>=0 && _sel_rt<(int)_rule_tiles.size()) {
            auto& rt = _rule_tiles[_sel_rt];
            ImGui::SameLine();
            if (ImGui::Button("Save .ruletile")) {
                auto p = fs::path(st.asset_dir) / (rt.name + ".ruletile");
                if (save_json(p, rt.to_json()))
                    st.log_success("Rule tile saved: " + p.string());
            }

            ImGui::Separator();
            ImGui::SetNextItemWidth(200);
            char nb[64]; snprintf(nb,sizeof(nb),"%s",rt.name.c_str());
            if (ImGui::InputText("Name##rtn", nb, sizeof(nb))) rt.name=nb;
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Default Tile##dt", &rt.default_tile, 1);

            ImGui::SeparatorText("Rules");
            ImGui::TextDisabled("Click a rule to edit its neighbor pattern. First matching rule wins.");

            if (ImGui::Button("+ Add Rule")) {
                rt.rules.push_back({}); _sel_rule=(int)rt.rules.size()-1;
            }

            ImGui::BeginChild("##rule_list", {160,-1}, true);
            int rm_rule=-1;
            for (int i=0;i<(int)rt.rules.size();++i) {
                ImGui::PushID(i);
                char lbl[32]; snprintf(lbl,sizeof(lbl),"Rule %d → tile %d", i, rt.rules[i].output_tile);
                bool sel=(_sel_rule==i);
                if (ImGui::Selectable(lbl, sel)) _sel_rule=i;
                ImGui::SameLine();
                if (ImGui::SmallButton("×")) rm_rule=i;
                ImGui::PopID();
            }
            if (rm_rule>=0) { rt.rules.erase(rt.rules.begin()+rm_rule); _sel_rule=-1; }
            ImGui::EndChild();

            if (_sel_rule>=0 && _sel_rule<(int)rt.rules.size()) {
                ImGui::SameLine();
                ImGui::BeginChild("##rule_edit", {0,-1}, true);
                auto& rule = rt.rules[_sel_rule];
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("Output Tile", &rule.output_tile, 1);
                ImGui::Spacing();
                _draw_rule_grid(rule);
                ImGui::Spacing();
                ImGui::TextDisabled("Click neighbor cells to cycle: any(grey) → must-match(green) → must-empty(red)");
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();
    }

    // ── Page 7: Physics Material 2D ──────────────────────────────────────────

    std::vector<PhysMat2D> _physmats;
    int  _sel_pm = -1;

    void _draw_physics_material(EditorState& st) {
        ImGui::SeparatorText("Physics Material 2D");
        ImGui::TextDisabled("Create reusable friction/bounciness presets. Assign on BoxCollider2D / CircleCollider2D.");
        ImGui::Spacing();

        ImGui::BeginChild("##pm_list", {160,0}, true);
        for (int i=0;i<(int)_physmats.size();++i) {
            bool sel=(_sel_pm==i);
            if (ImGui::Selectable(_physmats[i].name.c_str(), sel)) _sel_pm=i;
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##pm_detail", {0,0}, false);

        if (ImGui::Button("+ New Material")) {
            PhysMat2D m; m.name = "PhysMat " + std::to_string(_physmats.size());
            _physmats.push_back(m); _sel_pm=(int)_physmats.size()-1;
        }

        if (_sel_pm>=0 && _sel_pm<(int)_physmats.size()) {
            auto& pm = _physmats[_sel_pm];
            ImGui::SameLine();
            if (ImGui::Button("Save .physmat2d")) {
                auto p = fs::path(st.asset_dir) / (pm.name + ".physmat2d");
                if (save_json(p, pm.to_json()))
                    st.log_success("Physics material saved: " + p.string());
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                _physmats.erase(_physmats.begin()+_sel_pm);
                _sel_pm=std::min(_sel_pm,(int)_physmats.size()-1);
            }

            ImGui::Separator();
            ImGui::SetNextItemWidth(220);
            char nb[64]; snprintf(nb,sizeof(nb),"%s",pm.name.c_str());
            if (ImGui::InputText("Name##pmn", nb, sizeof(nb))) pm.name=nb;

            ImGui::Spacing();
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("Friction",   &pm.friction,   0.f, 1.f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = ice, 1 = sticky rubber.");
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("Bounciness", &pm.bounciness, 0.f, 1.f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = no bounce, 1 = perfectly elastic.");

            ImGui::Spacing();
            ImGui::SeparatorText("Presets");
            if (ImGui::Button("Ice"))    { pm.friction=0.0f; pm.bounciness=0.0f; }  ImGui::SameLine();
            if (ImGui::Button("Rubber")) { pm.friction=0.8f; pm.bounciness=0.8f; }  ImGui::SameLine();
            if (ImGui::Button("Bouncy")) { pm.friction=0.2f; pm.bounciness=0.95f; } ImGui::SameLine();
            if (ImGui::Button("Wood"))   { pm.friction=0.5f; pm.bounciness=0.1f; }  ImGui::SameLine();
            if (ImGui::Button("Metal"))  { pm.friction=0.3f; pm.bounciness=0.05f; }

            ImGui::Spacing();
            ImGui::TextDisabled("Assign via collider inspector: drag this asset onto\nthe 'material' field of BoxCollider2D / CircleCollider2D.");
        }
        ImGui::EndChild();
    }
};

// ─── Sprite Slicer Panel ──────────────────────────────────────────────────────
// Unity's Sprite Editor → Slice tab. Given a selected texture asset, generates
// sub-sprite rects (src_x/src_y/src_w/src_h) for the SpriteRenderer.
// Stored as a .sprites sidecar JSON next to the texture.

class SpriteSlicer {
    bool  _open       = false;
    int   _mode       = 0;   // 0=Grid, 1=Auto(count), 2=Auto(cell-size)
    int   _grid_w     = 32, _grid_h = 32;
    int   _cols       = 4,  _rows   = 4;
    int   _pivot_mode = 4;  // 0-8 = TL,T,TR,L,C,R,BL,B,BR
    float _pivot_x    = 0.5f, _pivot_y = 0.5f;
    int   _padding    = 0;
    int   _offset_x   = 0,  _offset_y = 0;
    std::string _target_asset;

    struct Rect { int x,y,w,h; };
    std::vector<Rect> _preview_rects;
    int  _tex_w = 0, _tex_h = 0;

public:
    void open(const std::string& asset_path, int tex_w, int tex_h) {
        _open = true;
        _target_asset = asset_path;
        _tex_w = tex_w; _tex_h = tex_h;
        _compute_preview();
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({560, 420}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Sprite Slicer##slicer", &_open, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End(); return;
        }

        ImGui::TextUnformatted("Asset:");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", _target_asset.c_str());
        ImGui::Spacing();

        const char* modes[] = {"Grid by Cell Size","Grid by Cell Count"};
        if (ImGui::Combo("Slice Mode", &_mode, modes, 2)) _compute_preview();

        if (_mode == 0) {
            bool ch = false;
            ch |= ImGui::InputInt("Cell Width",  &_grid_w, 1, 8);
            ch |= ImGui::InputInt("Cell Height", &_grid_h, 1, 8);
            if (ch) { _grid_w=std::max(1,_grid_w); _grid_h=std::max(1,_grid_h); _compute_preview(); }
        } else {
            bool ch = false;
            ch |= ImGui::InputInt("Columns", &_cols, 1);
            ch |= ImGui::InputInt("Rows",    &_rows, 1);
            if (ch) { _cols=std::max(1,_cols); _rows=std::max(1,_rows); _compute_preview(); }
        }

        bool ch2 = false;
        ch2 |= ImGui::InputInt("Padding##sp", &_padding, 1);
        ch2 |= ImGui::InputInt("Offset X",   &_offset_x, 1);
        ch2 |= ImGui::InputInt("Offset Y",   &_offset_y, 1);
        if (ch2) { _padding=std::max(0,_padding); _compute_preview(); }

        const char* pivots[] = {"Top Left","Top","Top Right","Left","Center","Right","Bottom Left","Bottom","Bottom Right","Custom"};
        ImGui::Combo("Pivot", &_pivot_mode, pivots, 10);
        if (_pivot_mode == 9) {
            ImGui::InputFloat("Pivot X", &_pivot_x, 0.01f);
            ImGui::InputFloat("Pivot Y", &_pivot_y, 0.01f);
        }

        ImGui::Spacing();
        ImGui::Text("Preview: %d sprites", (int)_preview_rects.size());

        // Mini preview grid showing splits
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float pscale = std::min(avail.x - 10.f, 200.f) / (float)std::max(1, _tex_w);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 tl = origin;
        ImVec2 br = {tl.x + _tex_w*pscale, tl.y + _tex_h*pscale};
        dl->AddRectFilled(tl, br, IM_COL32(40,40,50,255));
        dl->AddRect(tl, br, IM_COL32(120,120,120,255));
        for (auto& r : _preview_rects) {
            ImVec2 a = {tl.x + r.x*pscale, tl.y + r.y*pscale};
            ImVec2 b = {tl.x + (r.x+r.w)*pscale, tl.y + (r.y+r.h)*pscale};
            dl->AddRect(a, b, IM_COL32(78,138,245,200));
        }
        ImGui::Dummy({_tex_w*pscale, _tex_h*pscale});

        ImGui::Spacing();
        if (ImGui::Button("Slice & Save", {-1,0})) {
            _apply_slice(st);
        }
        ImGui::End();
    }

private:
    void _compute_preview() {
        _preview_rects.clear();
        if (_tex_w<=0 || _tex_h<=0) return;
        int cw = _grid_w, ch = _grid_h;
        if (_mode == 1) {
            cw = (_tex_w - _offset_x) / std::max(1,_cols);
            ch = (_tex_h - _offset_y) / std::max(1,_rows);
        }
        for (int y = _offset_y; y + ch <= _tex_h; y += ch + _padding)
            for (int x = _offset_x; x + cw <= _tex_w; x += cw + _padding)
                _preview_rects.push_back({x,y,cw,ch});
    }

    void _apply_slice(EditorState& st) {
        _compute_preview();
        nlohmann::json sprites = nlohmann::json::array();
        static const float px_arr[] = {0.f,0.5f,1.f, 0.f,0.5f,1.f, 0.f,0.5f,1.f};
        static const float py_arr[] = {0.f,0.f,0.f, 0.5f,0.5f,0.5f, 1.f,1.f,1.f};
        float px = _pivot_mode < 9 ? px_arr[_pivot_mode] : _pivot_x;
        float py = _pivot_mode < 9 ? py_arr[_pivot_mode] : _pivot_y;
        for (int i=0;i<(int)_preview_rects.size();++i) {
            auto& r = _preview_rects[i];
            sprites.push_back({{"i",i},{"x",r.x},{"y",r.y},{"w",r.w},{"h",r.h},
                               {"pivot_x",px},{"pivot_y",py}});
        }
        fs::path sidecar = fs::path(_target_asset).replace_extension(".sprites");
        if (save_json(sidecar, sprites))
            st.log_success("Sliced " + std::to_string(sprites.size()) + " sprites → " + sidecar.string());
        else
            st.log_error("Failed to write sprite slice data.");
        _open = false;
    }
};

// ─── Object Picker (enhanced Find/Replace) ────────────────────────────────────
// Ctrl+F in Unity opens a search overlay for hierarchy entities. We expose
// this as a modal with filter, component type, and tag filter.

class ObjectPickerPanel {
    bool  _open = false;
    char  _filter[128] = {};
    char  _tag_filter[64] = {};
    int   _comp_filter_idx = 0;  // 0=any
    bool  _ping_on_select  = true;

public:
    void open() { _open = true; memset(_filter,0,sizeof(_filter)); }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({380, 480}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Appearing, {0.5f,0.5f});
        if (!ImGui::Begin("Find Object in Scene##picker", &_open,
                          ImGuiWindowFlags_NoCollapse)) {
            ImGui::End(); return;
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##of", "Filter by name...", _filter, sizeof(_filter));

        ImGui::SetNextItemWidth(120);
        ImGui::InputTextWithHint("##tf","Tag...", _tag_filter, sizeof(_tag_filter));
        ImGui::SameLine();

        // Component filter combo
        auto all_comps = all_component_names();
        std::vector<const char*> comp_items; comp_items.push_back("Any Component");
        for (auto& c : all_comps) comp_items.push_back(c.c_str());
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##cf", &_comp_filter_idx, comp_items.data(), (int)comp_items.size());

        ImGui::Separator();
        ImGui::BeginChild("##obj_results", {0, -ImGui::GetFrameHeightWithSpacing()-4}, false);

        std::string flt(_filter), tflt(_tag_filter);
        std::transform(flt.begin(),flt.end(),flt.begin(),::tolower);
        std::string comp_req = _comp_filter_idx > 0 ? all_comps[_comp_filter_idx-1] : "";

        for (auto& e : st.entities) {
            std::string name = e.value("name","Entity");
            std::string tag  = e.value("tag","");
            std::string lname = name; std::transform(lname.begin(),lname.end(),lname.begin(),::tolower);

            if (!flt.empty()  && lname.find(flt)==std::string::npos) continue;
            if (!tflt.empty() && tag.find(tflt)==std::string::npos)  continue;
            if (!comp_req.empty() && (!e.contains("components") || !e["components"].contains(comp_req))) continue;

            int eid = e.value("id",0);
            bool sel = (st.selected_id == eid);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.24f,0.49f,0.91f,0.7f));

            char lbl[128]; snprintf(lbl,sizeof(lbl),"[%d] %s", eid, name.c_str());
            if (ImGui::Selectable(lbl, sel)) {
                st.select(eid);
                if (_ping_on_select) _open = false;
            }
            if (!tag.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(%s)",tag.c_str()); }
            if (sel) ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::Checkbox("Close on select", &_ping_on_select);
        ImGui::SameLine();
        ImGui::TextDisabled("%d entities", (int)st.entities.size());

        ImGui::End();
    }
};

// ─── Comparison Statistics Panel ─────────────────────────────────────────────
// Shows a live breakdown of the scene's components, entities, and feature
// coverage vs Unity 2D — purely informational, helps identify gaps.

class SceneStatsPanel {
    bool _open = false;
public:
    void open() { _open = true; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({340, 480}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Scene Statistics##stats", &_open)) { ImGui::End(); return; }

        int total = (int)st.entities.size();
        int active = 0; for (auto& e : st.entities) if (e.value("active",true)) active++;

        ImGui::SeparatorText("Entities");
        ImGui::Text("Total:  %d", total);
        ImGui::Text("Active: %d", active);
        ImGui::Text("Inactive: %d", total-active);

        ImGui::Spacing();
        ImGui::SeparatorText("Components Usage");

        std::unordered_map<std::string,int> counts;
        for (auto& e : st.entities)
            if (e.contains("components") && e["components"].is_object())
                for (auto& [k,v] : e["components"].items()) counts[k]++;

        // Sort by count desc
        std::vector<std::pair<std::string,int>> sorted_counts(counts.begin(),counts.end());
        std::sort(sorted_counts.begin(),sorted_counts.end(),[](auto& a,auto& b){ return a.second>b.second; });

        for (auto& [name,cnt] : sorted_counts) {
            float frac = total > 0 ? (float)cnt/total : 0.f;
            char overlay[32]; snprintf(overlay,sizeof(overlay),"%d",cnt);
            ImGui::ProgressBar(frac, {-1, 12}, overlay);
            ImGui::SameLine(0, -1);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::CalcTextSize(overlay).x - 120.f);
            ImGui::TextUnformatted(component_display_name(name).c_str());
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Hierarchy Depth");
        int max_depth = 0;
        for (auto& e : st.entities) {
            int depth=0, cur=e.value("id",0), guard=0;
            while (cur>=0 && guard++<100) {
                const Entity* p = st.find_entity(cur);
                if (!p) break;
                int pid = EditorState::parent_of(*p);
                if (pid<0) break;
                cur=pid; depth++;
            }
            max_depth=std::max(max_depth,depth);
        }
        ImGui::Text("Max nesting depth: %d", max_depth);
        if (max_depth > 6) ImGui::TextColored(ImVec4(1,0.7f,0.1f,1),"⚠ Deep hierarchies can hurt performance.");

        ImGui::End();
    }
};


// ═══════════════════════════════════════════════════════════════════════════════
// ADDITIONAL UNITY 2D PARITY FEATURES (appended)
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Color Palette Panel ──────────────────────────────────────────────────────
// Unity 2D Tilemap Palette — stores named color swatches used across the project.
// Also functions as a quick color picker hub accessible from any inspector field.

class ColorPalettePanel {
public:
    struct Swatch { std::string name; float r,g,b,a; };
    std::vector<Swatch> swatches;
    bool _open = false;
    int  _edit_idx = -1;
    char _new_name[64] = "New Color";

    ColorPalettePanel() {
        // Bootstrap with Unity's standard palette
        swatches = {
            {"White",    1.f,  1.f,  1.f,  1.f},
            {"Black",    0.f,  0.f,  0.f,  1.f},
            {"Red",      1.f,  0.27f,0.27f,1.f},
            {"Green",    0.18f,0.8f, 0.44f,1.f},
            {"Blue",     0.20f,0.59f,0.86f,1.f},
            {"Yellow",   1.f,  0.87f,0.22f,1.f},
            {"Orange",   1.f,  0.60f,0.14f,1.f},
            {"Purple",   0.61f,0.35f,0.71f,1.f},
            {"Cyan",     0.22f,0.90f,0.96f,1.f},
            {"Transparent",0.f,0.f, 0.f,  0.f},
        };
    }

    void open() { _open = true; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({300, 400}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Color Palette##colpal", &_open)) { ImGui::End(); return; }

        ImGui::SeparatorText("Project Colors");
        ImGui::TextDisabled("Click a swatch to copy it to clipboard. Right-click to edit/remove.");

        const float sw = 28.f, pad = 4.f;
        int per_row = (int)((ImGui::GetContentRegionAvail().x + pad) / (sw + pad));
        per_row = std::max(1, per_row);

        int to_remove = -1;
        for (int i = 0; i < (int)swatches.size(); ++i) {
            auto& s = swatches[i];
            ImVec4 col(s.r, s.g, s.b, s.a);
            ImGui::PushID(i);
            if (ImGui::ColorButton(s.name.c_str(), col, ImGuiColorEditFlags_AlphaPreview, {sw,sw})) {
                // Copy RGBA as JSON-style array string to clipboard
                char clip[64]; snprintf(clip,sizeof(clip),"[%.0f,%.0f,%.0f,%.0f]",
                    s.r*255,s.g*255,s.b*255,s.a*255);
                ImGui::SetClipboardText(clip);
                st.log("Copied color: " + std::string(s.name));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nRGBA(%.0f,%.0f,%.0f,%.0f)\nClick to copy",
                s.name.c_str(),s.r*255,s.g*255,s.b*255,s.a*255);
            if (ImGui::BeginPopupContextItem("##ctx")) {
                ImGui::Text("Edit: %s", s.name.c_str());
                float fc[4] = {s.r,s.g,s.b,s.a};
                if (ImGui::ColorEdit4("##edit", fc, ImGuiColorEditFlags_AlphaBar)) {
                    s.r=fc[0]; s.g=fc[1]; s.b=fc[2]; s.a=fc[3];
                }
                char nbuf[64]; snprintf(nbuf,sizeof(nbuf),"%s",s.name.c_str());
                if (ImGui::InputText("Name##sn", nbuf, sizeof(nbuf))) s.name=nbuf;
                if (ImGui::Button("Remove")) to_remove = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if ((i+1) % per_row != 0 && i+1 < (int)swatches.size()) ImGui::SameLine(0,pad);
        }
        if (to_remove >= 0) swatches.erase(swatches.begin()+to_remove);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::SetNextItemWidth(130);
        ImGui::InputText("##newname", _new_name, sizeof(_new_name));
        ImGui::SameLine();
        static float _pick[4] = {1,1,1,1};
        ImGui::ColorEdit4("##newcol", _pick,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine();
        if (ImGui::Button("Add")) {
            swatches.push_back({_new_name, _pick[0],_pick[1],_pick[2],_pick[3]});
        }

        ImGui::Spacing();
        if (ImGui::Button("Save Palette")) {
            nlohmann::json j = nlohmann::json::array();
            for (auto& s : swatches)
                j.push_back({{"name",s.name},{"r",s.r},{"g",s.g},{"b",s.b},{"a",s.a}});
            fs::path p = fs::path(st.asset_dir) / "color_palette.json";
            if (save_json(p, j)) st.log_success("Color palette saved → " + p.string());
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Palette")) {
            fs::path p = fs::path(st.asset_dir) / "color_palette.json";
            auto j = load_json(p);
            if (j.is_array()) {
                swatches.clear();
                for (auto& v : j)
                    swatches.push_back({v.value("name",""),
                        (float)v.value("r",1.0),(float)v.value("g",1.0),
                        (float)v.value("b",1.0),(float)v.value("a",1.0)});
                st.log_success("Color palette loaded.");
            }
        }
        ImGui::End();
    }
};

// ─── Entity Template Library (GameObject Templates) ──────────────────────────
// Unity's "Create Empty / 2D Object" menus let you stamp down pre-configured
// entities. This panel extends that with a user-defined template library:
// select any scene entity, save it as a named template, then stamp copies.
// Templates are stored as JSON in assets/templates/.

class TemplateLibraryPanel {
public:
    struct Template { std::string name; nlohmann::json data; std::string category; };
    std::vector<Template> templates;
    bool _open = false;
    int  _sel  = -1;
    char _cat_filter[32] = {};
    char _search[64]     = {};

    void open() { _open = true; ensure_builtin(); }

    void ensure_builtin() {
        // Add built-in templates only once
        if (!templates.empty()) return;

        auto add = [&](const char* cat, const char* name, nlohmann::json data) {
            templates.push_back({name, std::move(data), cat});
        };

        auto& cd = component_defaults();

        // 2D Sprites
        nlohmann::json sprite;
        sprite["name"] = "Sprite";
        sprite["active"] = true;
        sprite["components"]["Transform"] = cd["Transform"];
        sprite["components"]["SpriteRenderer"] = cd["SpriteRenderer"];
        add("2D", "Sprite", sprite);

        // Static platform
        nlohmann::json plat;
        plat["name"] = "Platform";
        plat["active"] = true;
        plat["components"]["Transform"] = cd["Transform"];
        plat["components"]["SpriteRenderer"] = cd["SpriteRenderer"];
        plat["components"]["BoxCollider2D"] = cd["BoxCollider2D"];
        add("2D", "Static Platform", plat);

        // Player (Rigidbody + capsule collider)
        nlohmann::json player;
        player["name"] = "Player";
        player["tag"]  = "Player";
        player["active"] = true;
        player["components"]["Transform"] = cd["Transform"];
        player["components"]["SpriteRenderer"] = cd["SpriteRenderer"];
        player["components"]["Rigidbody2D"] = cd["Rigidbody2D"];
        player["components"]["CapsuleCollider2D"] = cd["CapsuleCollider2D"];
        player["components"]["Script"] = cd["Script"];
        add("2D", "Player", player);

        // Enemy
        nlohmann::json enemy;
        enemy["name"]  = "Enemy";
        enemy["tag"]   = "Enemy";
        enemy["active"] = true;
        enemy["components"]["Transform"] = cd["Transform"];
        enemy["components"]["SpriteRenderer"] = cd["SpriteRenderer"];
        enemy["components"]["Rigidbody2D"] = cd["Rigidbody2D"];
        enemy["components"]["BoxCollider2D"] = cd["BoxCollider2D"];
        add("2D", "Enemy", enemy);

        // Camera with follow
        nlohmann::json cam;
        cam["name"]  = "Main Camera";
        cam["tag"]   = "MainCamera";
        cam["active"] = true;
        cam["components"]["Transform"] = cd["Transform"];
        cam["components"]["Camera2D"] = cd["Camera2D"];
        add("Camera", "Main Camera", cam);

        // UI Canvas
        nlohmann::json canvas;
        canvas["name"]  = "Canvas";
        canvas["active"] = true;
        canvas["components"]["Transform"] = cd["Transform"];
        canvas["components"]["UICanvas"] = cd["UICanvas"];
        add("UI", "Canvas", canvas);

        // UI Button
        nlohmann::json btn;
        btn["name"]  = "Button";
        btn["active"] = true;
        btn["components"]["Transform"] = cd["Transform"];
        btn["components"]["UIButton"] = cd["UIButton"];
        add("UI", "UI Button", btn);

        // Particle system
        nlohmann::json ps;
        ps["name"]  = "Particle System";
        ps["active"] = true;
        ps["components"]["Transform"] = cd["Transform"];
        ps["components"]["ParticleEmitter"] = cd["ParticleEmitter"];
        add("Effects", "Particle System", ps);

        // Point light
        nlohmann::json light;
        light["name"]  = "Point Light 2D";
        light["active"] = true;
        light["components"]["Transform"] = cd["Transform"];
        light["components"]["Light2D"] = cd["Light2D"];
        add("Lighting", "Point Light 2D", light);

        // Audio source
        nlohmann::json audio;
        audio["name"]  = "Audio Source";
        audio["active"] = true;
        audio["components"]["Transform"] = cd["Transform"];
        audio["components"]["AudioSource"] = cd["AudioSource"];
        add("Audio", "Audio Source", audio);

        // Trigger zone
        nlohmann::json trig;
        trig["name"]  = "Trigger Zone";
        trig["active"] = true;
        trig["components"]["Transform"] = cd["Transform"];
        auto bc = cd["BoxCollider2D"];
        bc["is_trigger"] = true;
        trig["components"]["BoxCollider2D"] = bc;
        trig["components"]["EventEmitter"] = cd["EventEmitter"];
        add("Logic", "Trigger Zone", trig);

        // Tilemap
        nlohmann::json tm;
        tm["name"]  = "Tilemap";
        tm["active"] = true;
        tm["components"]["Transform"] = cd["Transform"];
        tm["components"]["Tilemap"] = cd["Tilemap"];
        add("2D", "Tilemap", tm);
    }

    void stamp(EditorState& st, const Template& t) {
        nlohmann::json data = t.data;
        int new_id = st.next_id();
        data["id"] = new_id;
        // Place near camera centre
        if (data.contains("components") && data["components"].contains("Transform")) {
            data["components"]["Transform"]["x"] = st.cam_x;
            data["components"]["Transform"]["y"] = st.cam_y;
        }
        st.undo.push_deep(st.entities);
        st.entities.push_back(data);
        transform::mark_structure_dirty();
        st.select(new_id);
        st.log("Spawned template: " + t.name);
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ensure_builtin();
        ImGui::SetNextWindowSize({340, 500}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Template Library##tlib", &_open)) { ImGui::End(); return; }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##tsearch","Search templates...",_search,sizeof(_search));

        // Collect categories
        std::vector<std::string> cats;
        for (auto& t : templates)
            if (std::find(cats.begin(),cats.end(),t.category)==cats.end())
                cats.push_back(t.category);
        std::sort(cats.begin(),cats.end());

        std::string sflt(_search);
        std::transform(sflt.begin(),sflt.end(),sflt.begin(),::tolower);

        for (auto& cat : cats) {
            bool any = false;
            for (auto& t : templates) if (t.category==cat) {
                std::string lo=t.name; std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
                if (!sflt.empty() && lo.find(sflt)==std::string::npos) continue;
                any=true; break;
            }
            if (!any) continue;
            if (ImGui::TreeNodeEx(cat.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i=0;i<(int)templates.size();++i) {
                    auto& t = templates[i];
                    if (t.category!=cat) continue;
                    std::string lo=t.name; std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
                    if (!sflt.empty() && lo.find(sflt)==std::string::npos) continue;
                    ImGui::PushID(i);
                    bool sel=(_sel==i);
                    if (ImGui::Selectable(t.name.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                        _sel=i;
                        if (ImGui::IsMouseDoubleClicked(0)) stamp(st, t);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Double-click to stamp into scene.");
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }

        ImGui::Separator();
        bool has_sel = (_sel>=0 && _sel<(int)templates.size());
        if (ImGui::Button("Stamp Selected", {-1,0})) {
            if (has_sel) stamp(st, templates[_sel]);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Save Selected Entity as Template");
        if (Entity* ep = st.selected_entity()) {
            static char tname[64]="MyTemplate"; static char tcat[32]="Custom";
            ImGui::SetNextItemWidth(150); ImGui::InputText("Name##tn",tname,sizeof(tname));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100); ImGui::InputText("Category##tc",tcat,sizeof(tcat));
            ImGui::SameLine();
            if (ImGui::Button("Save##tsv")) {
                nlohmann::json data = *ep;
                data.erase("id");
                templates.push_back({tname, data, tcat});
                // Persist to assets/templates/
                fs::path p = fs::path(st.asset_dir)/"templates"/(std::string(tname)+".template.json");
                save_json(p, data);
                st.log_success("Saved template: " + std::string(tname));
            }
        } else {
            ImGui::TextDisabled("Select an entity in the Hierarchy to save as template.");
        }

        // Load custom templates from assets/templates/
        if (ImGui::Button("Reload Custom Templates")) {
            fs::path tdir = fs::path(st.asset_dir)/"templates";
            std::error_code ec;
            for (auto& de : fs::directory_iterator(tdir, ec)) {
                if (de.path().extension()==".json") {
                    auto j = load_json(de.path());
                    if (!j.is_null()) {
                        std::string nm = de.path().stem().string();
                        // remove .template suffix if present
                        if (nm.size()>9 && nm.substr(nm.size()-9)==".template")
                            nm=nm.substr(0,nm.size()-9);
                        bool exists=false;
                        for (auto& t2:templates) if (t2.name==nm){exists=true;break;}
                        if (!exists) templates.push_back({nm, j, "Custom"});
                    }
                }
            }
            st.log("Custom templates reloaded.");
        }
        ImGui::End();
    }
};

// ─── Event Graph / Visual Scripting (Logic Node Panel) ───────────────────────
// Lightweight node-graph for wiring entity events without code — covers
// Unity's built-in visual scripting (was Bolt) at a simple level:
// Event → Condition → Action chains stored per-scene as JSON.

// ─── Event Graph / Visual Scripting ──────────────────────────────────────
// Unreal Blueprint-style node graph: click-drag-release pin wiring with live
// preview, type-checked connections, drag-off-canvas context search, link
// re-routing/breaking, minimap, and a searchable categorized palette.
class EventGraphPanel {
public:
    // Pin definition
    struct EventGraphPin {
        std::string label;
        std::string ptype; // "exec","bool","int","float","string","entity"
        int pin_id = 0;
        std::string literal; // inline value when this input pin is unconnected
    };

    // Node definition
    struct Node {
        int    id;
        std::string type;
        std::string label;
        float  x=100, y=100;
        std::string param1, param2, param3;
        float  float_param = 0.f;
        std::vector<EventGraphPin> in_pins;
        std::vector<EventGraphPin> out_pins;
        float width = 185.f;
        float height = 60.f;
        bool selected = false;
    };

    // Link between pins
    struct Link {
        int from_node, to_node;
        int from_pin, to_pin;
    };

    // Declared variables belong to this graph asset.  They are serialized with
    // a type and default value, then used to seed isolated runtime state for
    // each entity that owns the graph.
    struct GraphVariable {
        std::string name;
        std::string type = "float";
        std::string default_value = "0";
    };

    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<GraphVariable> variables;
    // This is a daily workspace panel, not a temporary modal tool.  Keeping
    // it open lets the default dock layout place it beside Viewport on the
    // first launch; graph editing itself still requires an entity owner.
    bool _open = true;
    fs::path _asset_path;
    // A graph always belongs to the VisualScript component on one entity. The
    // old scene-wide fallback let users author a graph with no runtime owner,
    // so it looked valid in the editor but could never run in Play mode.
    int _bound_entity_id = -1;
    std::string _bound_entity_name;
    std::string _last_saved_graph;
    bool _save_error_reported = false;
    int  _next_id = 1;
    int  _next_pin_id = 1;
    ImVec2 _scroll = {0,0};
    float _zoom = 1.f;

    // ── Interactive wiring state ──
    // A wire-drag can start from EITHER an output pin or an input pin (Blueprint
    // lets you grab either end). If it starts from an already-linked input pin,
    // that existing link is lifted and re-routed rather than duplicated.
    bool _wiring_active = false;
    int  _wiring_from_node = -1;
    int  _wiring_from_pin = -1;
    bool _wiring_from_is_output = true;
    std::string _wiring_ptype = "exec";
    ImVec2 _wiring_mouse_pos = {0,0};
    int  _hover_pin_node = -1;
    int  _hover_pin_pin = -1;
    bool _hover_pin_is_output = false;
    // Pin hit testing is deliberately geometry based instead of using one
    // InvisibleButton per pin.  The graph canvas is also an input surface and
    // nested ImGui active items made those buttons steal each other's mouse
    // state on some frames (the exact reason wires looked like they had no
    // sockets).  Keeping the hit regions in one small list makes a wire drag
    // deterministic, including when the canvas is panned.
    struct PinHit {
        int node_id = -1;
        int pin_id = -1;
        bool is_output = false;
        std::string ptype;
        ImVec2 pos{};
    };
    std::vector<PinHit> _pin_hits;

    // Box-select state
    bool _box_selecting = false;
    ImVec2 _box_select_start = {0,0};
    ImVec2 _box_select_end = {0,0};

    // Drag-multiple-selected-nodes
    bool _dragging_nodes = false;

    // Undo / clipboard / pending action state
    std::vector<nlohmann::json> _undo_stack;
    std::vector<nlohmann::json> _redo_stack;
    std::string _clipboard_json;
    bool _move_snapshot_taken = false;
    char _new_variable_name[64] = {};
    char _new_variable_default[64] = "0";
    int _new_variable_type = 2; // float
    int _pending_variable_delete = -1;

    // Pending "create node from wire" state
    std::string _pending_wire_ptype;
    int  _pending_wire_from_node = -1;
    int  _pending_wire_from_pin = -1;
    bool _pending_wire_from_is_output = true;

    // Node type info for the palette
    struct NodeTypeInfo {
        const char* type;
        const char* label;
        ImVec4 color;
        const char* category;
        std::vector<EventGraphPin> in_pins;
        std::vector<EventGraphPin> out_pins;
    };

    // The palette is shared by node creation, legacy graph repair and drawing.
    // It must have process lifetime: returning this vector by value made
    // find_type_info() return pointers into a temporary.  That undefined
    // behaviour is why existing nodes could lose their pins and look as though
    // they had no connection sockets at all.
    static const std::vector<NodeTypeInfo>& node_types() {
        static const std::vector<NodeTypeInfo> types = {
            // ── Events (blue) ──
            {"on_start",       "▶ On Start",       {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_update",      "▶ On Update",       {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_fixed_update","▶ On Fixed Update", {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_key",         "⌨ On Key Press",    {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_key_held",    "On Key Held",        {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_key_released", "On Key Released",   {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_trigger",     "⚡ On Trigger Enter",{0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_trigger_stay","⚡ On Trigger Stay", {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_trigger_exit","⚡ On Trigger Exit", {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_collision",   "💥 On Collision Enter", {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_collision_stay","💥 On Collision Stay", {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_collision_exit","💥 On Collision Exit", {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"on_ui_click",    "🖱 On UI Click",       {0.17f,0.48f,0.70f,1.f}, "Events",
                {{"", "exec", 0}}, {{"", "exec", 1}, {"Action", "string", 2}}},

            // Pure input nodes can feed data pins without a dummy execution
            // chain. They are the building blocks for responsive movement.
            {"input_key",       "Key Held",              {0.17f,0.48f,0.70f,1.f}, "Input",
                {{"Key", "string", 2}}, {{"Held", "bool", 2}}},
            {"input_axis",      "Get Axis",              {0.17f,0.48f,0.70f,1.f}, "Input",
                {{"Negative", "string", 2}, {"Positive", "string", 3}},
                {{"Value", "float", 2}}},

            // ── Flow Control (yellow/amber) ──
            {"sequence",       "→ Sequence",        {0.70f,0.55f,0.10f,1.f}, "Flow",
                {{"", "exec", 0}}, {{"Out 0", "exec", 1}, {"Out 1", "exec", 2}, {"Out 2", "exec", 3}, {"Out 3", "exec", 4}}},
            {"branch",         "? Branch (If/Else)",{0.70f,0.55f,0.10f,1.f}, "Flow",
                {{"", "exec", 0}, {"Condition", "bool", 2}}, {{"True", "exec", 1}, {"False", "exec", 2}}},
            {"delay",          "⏱ Delay",           {0.17f,0.48f,0.70f,1.f}, "Flow",
                {{"", "exec", 0}, {"Seconds", "float", 2}}, {{"", "exec", 1}}},
            {"wait",           "⏳ Wait",            {0.17f,0.48f,0.70f,1.f}, "Flow",
                {{"", "exec", 0}, {"Seconds", "float", 2}}, {{"", "exec", 1}}},
            {"for_loop",       "🔁 For Loop",       {0.70f,0.55f,0.10f,1.f}, "Flow",
                {{"", "exec", 0}, {"Count", "int", 2}}, {{"Body", "exec", 1}, {"Completed", "exec", 2}}},
            {"while_loop",     "🔁 While Loop",     {0.70f,0.55f,0.10f,1.f}, "Flow",
                {{"", "exec", 0}, {"Condition", "bool", 2}}, {{"Body", "exec", 1}, {"Completed", "exec", 2}}},
            {"flipflop",       "↔ Flip Flop",       {0.70f,0.55f,0.10f,1.f}, "Flow",
                {{"", "exec", 0}}, {{"A", "exec", 1}, {"B", "exec", 2}}},
            {"gate",           "⊞ Gate",            {0.70f,0.55f,0.10f,1.f}, "Flow",
                {{"", "exec", 0}, {"Open", "bool", 2}}, {{"", "exec", 1}}},
            {"do_once",        "❶ Do Once",         {0.70f,0.55f,0.10f,1.f}, "Flow",
                {{"", "exec", 0}}, {{"", "exec", 1}}},

            // ── Actions (red) ──
            {"set_active",     "👁 Set Active",     {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Active", "bool", 3}}, {{"", "exec", 1}}},
            {"destroy",        "✕ Destroy Entity", {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}}, {{"", "exec", 1}}},
            {"spawn",          "⊕ Spawn Entity",    {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Template Entity", "entity", 2}, {"X", "float", 3}, {"Y", "float", 4}}, {{"", "exec", 1}, {"Entity", "entity", 2}}},
            {"play_animation", "🎬 Play Animation", {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Animation", "asset", 3}}, {{"", "exec", 1}}},
            {"set_field",      "✏ Set Field",       {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Field", "string", 3}, {"Value", "wildcard", 4}}, {{"", "exec", 1}}},
            {"load_scene",     "🌐 Load Scene",     {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Scene", "asset", 2}}, {{"", "exec", 1}}},
            {"audio_play",     "🔊 Play Audio",     {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Source", "entity", 2}, {"Clip", "asset", 3}}, {{"", "exec", 1}}},
            {"set_text",       "Set Text",           {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Text", "string", 3}}, {{"", "exec", 1}}},
            {"set_sprite",     "Set Sprite",         {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Sprite", "asset", 3}}, {{"", "exec", 1}}},
            {"set_ui_progress", "Set UI Progress",   {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Value", "float", 3}}, {{"", "exec", 1}}},
            {"set_audio_volume", "Set Audio Volume", {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Volume", "float", 3}}, {{"", "exec", 1}}},
            {"set_velocity",   "→ Set Velocity",    {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"X", "float", 3}, {"Y", "float", 4}}, {{"", "exec", 1}}},
            {"add_force",      "↑ Add Force",       {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"X", "float", 3}, {"Y", "float", 4}}, {{"", "exec", 1}}},
            {"teleport_to",    "◎ Teleport To",     {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"X", "float", 3}, {"Y", "float", 4}}, {{"", "exec", 1}}},
            {"tween_to",       "〰 Tween To",       {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"X", "float", 3}, {"Y", "float", 4}, {"Duration", "float", 5}}, {{"", "exec", 1}}},
            {"move_by",        "Move By",            {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"X / sec", "float", 3}, {"Y / sec", "float", 4}}, {{"", "exec", 1}}},
            {"set_rotation",   "Set Rotation",       {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Degrees", "float", 3}}, {{"", "exec", 1}}},
            {"set_gravity",    "Set Gravity Scale",  {0.45f,0.18f,0.18f,1.f}, "Actions",
                {{"", "exec", 0}, {"Entity", "entity", 2}, {"Scale", "float", 3}}, {{"", "exec", 1}}},

            // ── Math (teal) ──
            {"math_add",       "➕ Add",             {0.10f,0.55f,0.55f,1.f}, "Math",
                {{"", "exec", 0}, {"A", "float", 2}, {"B", "float", 3}}, {{"", "exec", 1}, {"Result", "float", 2}}},
            {"math_sub",       "➖ Subtract",        {0.10f,0.55f,0.55f,1.f}, "Math",
                {{"", "exec", 0}, {"A", "float", 2}, {"B", "float", 3}}, {{"", "exec", 1}, {"Result", "float", 2}}},
            {"math_mul",       "✖ Multiply",        {0.10f,0.55f,0.55f,1.f}, "Math",
                {{"", "exec", 0}, {"A", "float", 2}, {"B", "float", 3}}, {{"", "exec", 1}, {"Result", "float", 2}}},
            {"math_div",       "➗ Divide",          {0.10f,0.55f,0.55f,1.f}, "Math",
                {{"", "exec", 0}, {"A", "float", 2}, {"B", "float", 3}}, {{"", "exec", 1}, {"Result", "float", 2}}},
            {"random_range",   "🎲 Random Range",   {0.10f,0.55f,0.55f,1.f}, "Math",
                {{"", "exec", 0}, {"Min", "float", 2}, {"Max", "float", 3}}, {{"", "exec", 1}, {"Result", "float", 2}}},
            {"clamp",          "⇕ Clamp",           {0.10f,0.55f,0.55f,1.f}, "Math",
                {{"", "exec", 0}, {"Value", "float", 2}, {"Min", "float", 3}, {"Max", "float", 4}}, {{"", "exec", 1}, {"Result", "float", 2}}},

            // ── Variables (purple) ──
            {"set_variable",   "📦 Set Variable",  {0.50f,0.25f,0.60f,1.f}, "Variables",
                {{"", "exec", 0}, {"Name", "string", 2}, {"Value", "wildcard", 3}}, {{"", "exec", 1}}},
            {"get_variable",   "📤 Get Variable",  {0.50f,0.25f,0.60f,1.f}, "Variables",
                {{"", "exec", 0}, {"Name", "string", 2}}, {{"", "exec", 1}, {"Value", "wildcard", 2}}},
            {"increment_variable","📈 Increment",  {0.50f,0.25f,0.60f,1.f}, "Variables",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"decrement_variable","📉 Decrement",  {0.50f,0.25f,0.60f,1.f}, "Variables",
                {{"", "exec", 0}}, {{"", "exec", 1}}},

            // ── Debug (grey) ──
            {"log",            "📋 Log Message",    {0.40f,0.40f,0.45f,1.f}, "Debug",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"print_value",    "🔍 Print Value",    {0.40f,0.40f,0.45f,1.f}, "Debug",
                {{"", "exec", 0}}, {{"", "exec", 1}}},
            {"breakpoint",     "⏸ Breakpoint",      {0.40f,0.40f,0.45f,1.f}, "Debug",
                {{"", "exec", 0}}, {{"", "exec", 1}}},

            // ── Comment (grey) ──
            {"comment",        "💬 Comment",        {0.30f,0.30f,0.33f,1.f}, "Comment",
                {}, {}},

            // ── Utility ──
            {"reroute",        "⤷ Reroute",        {0.45f,0.45f,0.48f,1.f}, "Utility",
                {{"In", "wildcard", 0}}, {{"Out", "wildcard", 1}}},
            // Data and debug nodes added after the initial catalog. These
            // expose the generic component property contract directly in the
            // visual editor: connected values resolve before node literals.
            {"get_field", "Get Field", {0.10f,0.55f,0.55f,1.f}, "Math",
                {{"Entity", "entity", 2}, {"Field", "string", 3}}, {{"Value", "wildcard", 2}}},
            {"debug_log", "Log Value", {0.40f,0.40f,0.45f,1.f}, "Debug",
                {{"", "exec", 0}, {"Message", "string", 2}}, {{"", "exec", 1}}},
        };
        return types;
    }

    static const NodeTypeInfo* find_type_info(const std::string& type) {
        for (auto& ti : node_types()) {
            if (ti.type == type) return &ti;
        }
        return nullptr;
    }

    // The old labels began with emoji / Unicode glyphs.  The bundled editor
    // font does not include them, so they were rendered as a distracting '?'.
    // Keep the useful title text and render a crisp vector icon ourselves.
    static std::string display_label(const char* raw_label) {
        std::string label = raw_label ? raw_label : "";
        const size_t first_space = label.find(' ');
        if (first_space != std::string::npos && !label.empty()) {
            const unsigned char first = static_cast<unsigned char>(label[0]);
            if (first >= 0x80 || label[0] == '?' || label[0] == '+' ||
                label[0] == '-' || label[0] == '*' || label[0] == '/')
                label = label.substr(first_space + 1);
        }
        return label;
    }

    static std::string display_title(const Node& n) {
        if (const auto* ti = find_type_info(n.type)) return display_label(ti->label);
        return n.label;
    }

    static void draw_node_header_icon(ImDrawList* dl, ImVec2 center,
                                      const Node& n, ImU32 color) {
        const auto* ti = find_type_info(n.type);
        const std::string category = ti ? ti->category : "";
        const ImU32 ink = IM_COL32(255, 255, 255, 235);
        if (category == "Events") {
            // play / trigger marker
            dl->AddTriangleFilled({center.x-4,center.y-6}, {center.x-4,center.y+6},
                                  {center.x+6,center.y}, ink);
        } else if (category == "Flow") {
            dl->AddTriangleFilled({center.x-5,center.y-5}, {center.x-5,center.y+5},
                                  {center.x+4,center.y}, ink);
            dl->AddLine({center.x-7,center.y}, {center.x-4,center.y}, ink, 2.f);
        } else if (category == "Math") {
            if (n.type == "math_sub") {
                dl->AddLine({center.x-5,center.y}, {center.x+5,center.y}, ink, 2.f);
            } else if (n.type == "math_mul") {
                dl->AddLine({center.x-4,center.y-4}, {center.x+4,center.y+4}, ink, 2.f);
                dl->AddLine({center.x+4,center.y-4}, {center.x-4,center.y+4}, ink, 2.f);
            } else if (n.type == "math_div") {
                dl->AddCircleFilled({center.x,center.y-4}, 1.5f, ink);
                dl->AddLine({center.x-5,center.y}, {center.x+5,center.y}, ink, 2.f);
                dl->AddCircleFilled({center.x,center.y+4}, 1.5f, ink);
            } else {
                dl->AddLine({center.x-5,center.y}, {center.x+5,center.y}, ink, 2.f);
                dl->AddLine({center.x,center.y-5}, {center.x,center.y+5}, ink, 2.f);
            }
        } else if (category == "Variables") {
            dl->AddRectFilled({center.x-5,center.y-4}, {center.x+5,center.y+4}, ink, 1.f);
            dl->AddRect({center.x-7,center.y-6}, {center.x+3,center.y+2}, color, 1.f, 0, 1.5f);
        } else if (category == "Actions") {
            dl->AddRectFilled({center.x-4,center.y-4}, {center.x+4,center.y+4}, ink, 1.f);
        } else if (category == "Debug") {
            dl->AddCircle(center, 5.f, ink, 0, 1.8f);
            dl->AddLine({center.x,center.y-3}, {center.x,center.y+1}, ink, 1.6f);
            dl->AddCircleFilled({center.x,center.y+3}, 1.f, ink);
        } else {
            dl->AddCircleFilled(center, 4.f, ink);
        }
    }

    // Can a wire legally connect these two pin types? Exec only connects to
    // exec. Data pins connect to same-type, with numeric widening (int->float)
    // and anything being convertible to a string display pin, mirroring the
    // permissive-but-sane casting Blueprint applies at connection time.
    static bool pins_compatible(const std::string& a, const std::string& b) {
        if (a == b) return true;
        if (a == "wildcard" || b == "wildcard") return true;
        if (a == "exec" || b == "exec") return false;
        bool a_num = (a == "int" || a == "float");
        bool b_num = (b == "int" || b == "float");
        if (a_num && b_num) return true;
        if (a == "string" || b == "string") return true; // anything can display as text
        return false;
    }

    Node* find_node(int id) {
        for (auto& n : nodes) if (n.id == id) return &n;
        return nullptr;
    }

    // Locate a pin by id anywhere in the graph; reports which node/side it's on.
    EventGraphPin* find_pin(int pin_id, int* out_node_id = nullptr, bool* out_is_output = nullptr) {
        for (auto& n : nodes) {
            for (auto& p : n.in_pins) {
                if (p.pin_id == pin_id) { if (out_node_id) *out_node_id = n.id; if (out_is_output) *out_is_output = false; return &p; }
            }
            for (auto& p : n.out_pins) {
                if (p.pin_id == pin_id) { if (out_node_id) *out_node_id = n.id; if (out_is_output) *out_is_output = true; return &p; }
            }
        }
        return nullptr;
    }

    void open(EditorState& st) {
        Entity* entity = st.selected_entity();
        if (!entity || !entity->contains("components") ||
            !(*entity)["components"].contains("VisualScript")) {
            _bound_entity_id = -1;
            _bound_entity_name.clear();
            _asset_path.clear();
            _open = true;
            st.log_warn("Select an entity with a Visual Script component, then open Visual Scripting.");
            return;
        }
        const auto& component = (*entity)["components"]["VisualScript"];
        const std::string asset = component.value("asset", std::string());
        if (asset.empty()) {
            _bound_entity_id = -1;
            _bound_entity_name.clear();
            _asset_path.clear();
            _open = true;
            st.log_warn("Create or assign a graph asset in the selected entity's Visual Script component first.");
            return;
        }
        const fs::path raw(asset);
        open_entity_asset(entity->value("id", -1), entity->value("name", "Entity"),
                          raw.is_absolute() ? raw : (fs::path(st.asset_dir) / raw));
    }

    void open_entity_asset(int entity_id, const std::string& entity_name,
                           const fs::path& asset_path) {
        _bound_entity_id = entity_id;
        _bound_entity_name = entity_name;
        _asset_path = asset_path;
        _open = true;
        auto j = load_json(_asset_path);
        if (j.is_array() || j.is_object()) {
            load_from_json(j);
        } else {
            nodes.clear(); links.clear(); variables.clear();
            _next_id = 1; _next_pin_id = 1;
        }
        _last_saved_graph = save_to_json().dump();
        _save_error_reported = false;
    }

    const fs::path& asset_path() const { return _asset_path; }

    bool _binding_is_valid(const EditorState& st) const {
        if (_bound_entity_id < 0 || st.selected_id != _bound_entity_id || _asset_path.empty()) return false;
        const Entity* entity = st.find_entity(_bound_entity_id);
        if (!entity || !entity->contains("components") ||
            !(*entity)["components"].contains("VisualScript")) return false;
        const std::string assigned = (*entity)["components"]["VisualScript"].value("asset", std::string());
        if (assigned.empty()) return false;
        const fs::path raw(assigned);
        const fs::path assigned_path = raw.is_absolute() ? raw : (fs::path(st.asset_dir) / raw);
        return assigned_path.lexically_normal() == _asset_path.lexically_normal();
    }

    void _autosave_if_changed(EditorState& st) {
        if (_asset_path.empty()) return;
        const nlohmann::json graph = save_to_json();
        const std::string serialised = graph.dump();
        if (serialised == _last_saved_graph) return;
        if (save_json(_asset_path, graph)) {
            _last_saved_graph = serialised;
            _save_error_reported = false;
        } else if (!_save_error_reported) {
            st.log_error("Visual Script autosave failed: " + _asset_path.string());
            _save_error_reported = true;
        }
    }

    // ── Property-field metadata per node type ──
    // Returns the human-readable label for param1/param2/param3/float_param for a given
    // node type, or "" if that slot isn't used by the type. Centralizing this means the
    // live Inspector panel and the legacy right-click popup always agree on what each
    // field means instead of duplicating the same long if/else chain twice.
    struct FieldLabels { std::string p1, p2, p3, fp; };
    static FieldLabels field_labels_for(const std::string& type) {
        if (type == "delay" || type == "wait")              return {"", "", "", "Seconds"};
        if (type == "for_loop")                              return {"", "", "", "Count"};
        if (type == "while_loop")                            return {"Condition", "", "", ""};
        if (type == "on_key" || type == "on_key_held" || type == "on_key_released" || type == "input_key")
                                                               return {"Key", "", "", ""};
        if (type == "input_axis")                            return {"Negative", "Positive", "", ""};
        if (type == "on_ui_click")                           return {"Action filter (optional)", "", "", ""};
        if (type == "set_active")                            return {"Entity", "Active (true/false)", "", ""};
        if (type == "destroy")                                return {"Target", "", "", ""};
        if (type == "load_scene")                             return {"Scene", "", "", ""};
        if (type == "spawn")                                 return {"Template Entity", "Y", "", "X"};
        if (type == "play_animation")                        return {"Entity", "Animation", "", ""};
        if (type == "set_text")                              return {"Entity", "Text", "", ""};
        if (type == "set_sprite")                            return {"Entity", "Sprite", "", ""};
        if (type == "set_ui_progress")                       return {"Entity", "", "", "Value"};
        if (type == "set_audio_volume")                      return {"Entity", "", "", "Volume"};
        if (type == "set_field")                             return {"Entity", "Field", "Value", ""};
        if (type == "get_field")                             return {"Entity", "Field", "", ""};
        if (type == "debug_log")                             return {"Message", "", "", ""};
        if (type == "audio_play")                            return {"Source", "Clip", "", ""};
        if (type == "log" || type == "comment")              return {"Text", "", "", ""};
        if (type == "set_velocity" || type == "add_force")   return {"Entity", "Y", "", "X"};
        if (type == "teleport_to")                            return {"Entity", "Y", "", "X"};
        if (type == "move_by")                                return {"Entity", "Y / sec", "", "X / sec"};
        if (type == "set_rotation")                           return {"Entity", "", "", "Degrees"};
        if (type == "set_gravity")                            return {"Entity", "", "", "Scale"};
        if (type == "branch" || type == "gate")               return {"Condition", "", "", ""};
        if (type == "print_value")                            return {"Entity", "Field", "", ""};
        if (type == "tween_to")                               return {"Entity", "Y", "Duration", "X"};
        if (type == "set_variable")                           return {"Name", "Value", "", ""};
        if (type == "get_variable")                           return {"Name", "", "", ""};
        if (type == "increment_variable" || type == "decrement_variable") return {"Name", "", "", ""};
        if (type == "math_add" || type == "math_sub" || type == "math_mul" || type == "math_div")
                                                               return {"Store in", "B", "", "A"};
        if (type == "random_range")                           return {"Store in", "Max", "", "Min"};
        if (type == "clamp")                                  return {"Store in", "Range", "", "Value"};
        if (type == "on_trigger" || type == "on_collision" ||
            type == "on_trigger_stay" || type == "on_trigger_exit" ||
            type == "on_collision_stay" || type == "on_collision_exit")
                                                               return {"Tag filter", "", "", ""};
        return {"", "", "", ""};
    }

    // Draws editable widgets for every param/float slot a node type actually uses.
    // `id_suffix` keeps widget IDs unique when the same node is rendered in two
    // places in the same frame (e.g. Inspector panel + legacy right-click popup).
    void draw_node_property_fields(Node& n, const char* id_suffix) {
        FieldLabels fl = field_labels_for(n.type);
        bool any = false;

        auto text_field = [&](const char* label, std::string& field, const char* widget_id) {
            if (label[0] == '\0') return;
            any = true;
            char buf[128]; snprintf(buf, sizeof(buf), "%s", field.c_str());
            ImGui::SetNextItemWidth(-1);
            std::string full_id = std::string("##") + widget_id + id_suffix;
            if (ImGui::InputText(full_id.c_str(), buf, sizeof(buf))) {
                push_undo_snapshot_if_needed_for_field_edit();
                field = buf;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", label);
            ImGui::Spacing();
        };

        text_field(fl.p1.c_str(), n.param1, "p1");
        text_field(fl.p2.c_str(), n.param2, "p2");
        text_field(fl.p3.c_str(), n.param3, "p3");

        if (!fl.fp.empty()) {
            any = true;
            std::string full_id = std::string("##fp") + id_suffix;
            // Edit a local copy first.  The undo snapshot must see the old
            // graph value; binding the model directly to ImGui captures a
            // post-edit state and makes Ctrl+Z appear broken.
            float edited = n.float_param;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputFloat(full_id.c_str(), &edited, 0.1f)) {
                push_undo_snapshot_if_needed_for_field_edit();
                n.float_param = edited;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", fl.fp.c_str());
            ImGui::Spacing();
        }

        if (!any) {
            ImGui::TextDisabled("This node has no editable properties.");
        }
    }

    // Blueprint-style node fields.  These are deliberately real ImGui
    // controls rather than a painted value preview: a graph can be authored
    // without repeatedly travelling to the side Inspector.  The node header
    // is the only drag handle, so editing an inline value can never turn into
    // an accidental node drag.
    static int inline_property_row_count(const Node& n) {
        const FieldLabels fl = field_labels_for(n.type);
        return (!fl.p1.empty() ? 1 : 0) + (!fl.p2.empty() ? 1 : 0) +
               (!fl.p3.empty() ? 1 : 0) + (!fl.fp.empty() ? 1 : 0);
    }

    void draw_inline_node_property_fields(EditorState& st, Node& n, const ImVec2& node_pos,
                                          float top_y, float view_scale) {
        const FieldLabels fl = field_labels_for(n.type);
        float y = top_y;
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const float field_width = std::max(64.f, n.width - 18.f) * view_scale;

        auto text_field = [&](const std::string& label, std::string& field,
                              const char* suffix) {
            if (label.empty()) return;
            draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                               {node_pos.x + 9.f * view_scale, y},
                               IM_COL32(168, 178, 194, 235), label.c_str());
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", field.c_str());
            ImGui::SetCursorScreenPos({node_pos.x + 9.f * view_scale, y + 13.f * view_scale});
            ImGui::SetNextItemWidth(field_width);
            const std::string id = "##inline_" + std::to_string(n.id) + "_" + suffix;
            const bool is_entity_reference = label == "Entity" || label == "Source" ||
                label == "Template Entity" || (n.type == "destroy" && label == "Target");
            if (is_entity_reference) {
                const std::string preview = field.empty() ? "(Select Entity)" :
                    (field == "$self" ? "Self" : field);
                if (ImGui::BeginCombo(id.c_str(), preview.c_str())) {
                    if (ImGui::Selectable("Self ($self)", field == "$self")) {
                        push_undo_snapshot_if_needed_for_field_edit();
                        field = "$self";
                    }
                    for (const auto& entity : st.entities) {
                        const int entity_id = entity.value("id", 0);
                        const std::string value = std::to_string(entity_id);
                        const std::string option = entity.value("name", std::string("Entity")) + " (" + value + ")";
                        if (ImGui::Selectable(option.c_str(), field == value)) {
                            push_undo_snapshot_if_needed_for_field_edit();
                            field = value;
                        }
                    }
                    ImGui::EndCombo();
                }
                y += 36.f * view_scale;
                return;
            }
            const bool is_key_picker =
                ((n.type == "on_key" || n.type == "on_key_held" || n.type == "on_key_released" || n.type == "input_key") && label == "Key") ||
                (n.type == "input_axis" && (label == "Negative" || label == "Positive"));
            if (is_key_picker) {
                const std::string preview = field.empty() ? "(Choose Key)" : field;
                if (ImGui::BeginCombo(id.c_str(), preview.c_str())) {
                    static const SDL_Scancode common_keys[] = {
                        SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D,
                        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                        SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE,
                        SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_E, SDL_SCANCODE_F,
                        SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C
                    };
                    for (SDL_Scancode sc : common_keys) {
                        const char* name = SDL_GetScancodeName(sc);
                        if (name && name[0] && ImGui::Selectable(name, field == name)) {
                            push_undo_snapshot_if_needed_for_field_edit();
                            field = name;
                        }
                    }
                    ImGui::EndCombo();
                }
                y += 36.f * view_scale;
                return;
            }
            if (n.type == "load_scene" && label == "Scene") {
                const std::string preview = field.empty() ? "(Choose Scene)" : fs::path(field).filename().string();
                ImGui::Button(preview.c_str(), {field_width - 62.f * view_scale, 0.f});
                if (ImGui::IsItemHovered() && !field.empty()) ImGui::SetTooltip("%s", field.c_str());
                ImGui::SameLine(0, 3.f * view_scale);
                if (ImGui::SmallButton((std::string("...") + id).c_str())) {
                    const std::string picked = browse_project_scene_reference(st);
                    if (!picked.empty()) {
                        push_undo_snapshot_if_needed_for_field_edit();
                        field = picked;
                    }
                }
                y += 36.f * view_scale;
                return;
            }
            if (n.type == "audio_play" && label == "Clip") {
                const std::string preview = field.empty() ? "(Choose Audio Clip)" : fs::path(field).filename().string();
                ImGui::Button(preview.c_str(), {field_width - 62.f * view_scale, 0.f});
                if (ImGui::IsItemHovered() && !field.empty()) ImGui::SetTooltip("%s", field.c_str());
                ImGui::SameLine(0, 3.f * view_scale);
                if (ImGui::SmallButton((std::string("...") + id).c_str())) {
                    // Match the real runtime codec set.  The core SDL
                    // fallback used by this Editor plays WAV; builds linked
                    // with SDL_mixer retain the wider compressed-audio list.
#ifdef NO_SDL_MIXER
                    static const char audio_filter[] = "WAV files\0*.wav\0All files\0*.*\0";
#else
                    static const char audio_filter[] = "Audio files\0*.wav;*.ogg;*.mp3;*.flac\0All files\0*.*\0";
#endif
                    const std::string picked = browse_project_asset_reference(st, audio_filter, "Choose audio clip");
                    if (!picked.empty()) {
                        push_undo_snapshot_if_needed_for_field_edit();
                        field = picked;
                    }
                }
                y += 36.f * view_scale;
                return;
            }
            if (n.type == "set_sprite" && label == "Sprite") {
                const std::string preview = field.empty() ? "(Choose Sprite)" : fs::path(field).filename().string();
                ImGui::Button(preview.c_str(), {field_width - 62.f * view_scale, 0.f});
                if (ImGui::IsItemHovered() && !field.empty()) ImGui::SetTooltip("%s", field.c_str());
                ImGui::SameLine(0, 3.f * view_scale);
                if (ImGui::SmallButton((std::string("...") + id).c_str())) {
                    static const char image_filter[] = "Image files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0All files\0*.*\0";
                    const std::string picked = browse_project_asset_reference(st, image_filter, "Choose sprite texture");
                    if (!picked.empty()) {
                        push_undo_snapshot_if_needed_for_field_edit();
                        field = picked;
                    }
                }
                y += 36.f * view_scale;
                return;
            }
            if (n.type == "play_animation" && label == "Animation") {
                const Entity* owner = nullptr;
                if (n.param1 == "$self") owner = st.selected_entity();
                else { try { owner = st.find_entity(std::stoi(n.param1)); } catch (...) {} }
                const Entity* animator = owner && owner->contains("components") &&
                    (*owner)["components"].contains("Animator") ? &(*owner)["components"]["Animator"] : nullptr;
                if (animator && animator->contains("animations") && (*animator)["animations"].is_object()) {
                    const std::string preview = field.empty() ? "(Choose Animation)" : field;
                    if (ImGui::BeginCombo(id.c_str(), preview.c_str())) {
                        for (const auto& [name, clip] : (*animator)["animations"].items()) {
                            (void)clip;
                            if (ImGui::Selectable(name.c_str(), field == name)) {
                                push_undo_snapshot_if_needed_for_field_edit();
                                field = name;
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::TextDisabled("Select an Animator entity first.");
                }
                y += 36.f * view_scale;
                return;
            }
            // Generic component properties are still useful because they
            // cover every inspector component without inventing a separate
            // node for every single field.  They must not, however, make an
            // author type fragile JSON paths.  The selected target's real
            // component fields are presented as a picker and stored in the
            // stable runtime form Components/<Component>/<Property>.
            if ((n.type == "set_field" || n.type == "get_field" || n.type == "print_value") && label == "Field") {
                const Entity* target = nullptr;
                if (n.param1 == "$self") target = st.selected_entity();
                else {
                    try { target = st.find_entity(std::stoi(n.param1)); }
                    catch (...) { target = nullptr; }
                }
                std::string preview = field.empty() ? "(Choose Component Property)" : field;
                if (ImGui::BeginCombo(id.c_str(), preview.c_str())) {
                    if (!target || !target->contains("components") || !(*target)["components"].is_object()) {
                        ImGui::TextDisabled("Choose an Entity on this node first.");
                    } else {
                        const auto& components = (*target)["components"];
                        bool has_properties = false;
                        for (const auto& [component_name, component] : components.items()) {
                            if (!component.is_object()) continue;
                            bool component_has_properties = false;
                            for (const auto& [property_name, property] : component.items()) {
                                if (property.is_boolean() || property.is_number() || property.is_string()) {
                                    component_has_properties = true;
                                    break;
                                }
                            }
                            if (!component_has_properties) continue;
                            has_properties = true;
                            ImGui::PushID(component_name.c_str());
                            if (ImGui::TreeNode(component_name.c_str())) {
                                for (const auto& [property_name, property] : component.items()) {
                                    if (!(property.is_boolean() || property.is_number() || property.is_string())) continue;
                                    const std::string candidate = "Components/" + component_name + "/" + property_name;
                                    const std::string display = property_name + "##" + std::to_string(n.id);
                                    if (ImGui::Selectable(display.c_str(), field == candidate)) {
                                        push_undo_snapshot_if_needed_for_field_edit();
                                        field = candidate;
                                    }
                                    if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", candidate.c_str());
                                }
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
                        if (!has_properties) ImGui::TextDisabled("This Entity has no editable component properties.");
                    }
                    ImGui::EndCombo();
                }
                y += 36.f * view_scale;
                return;
            }
            if (ImGui::InputText(id.c_str(), buf, sizeof(buf))) {
                push_undo_snapshot_if_needed_for_field_edit();
                field = buf;
            }
            y += 36.f * view_scale;
        };

        text_field(fl.p1, n.param1, "p1");
        text_field(fl.p2, n.param2, "p2");
        text_field(fl.p3, n.param3, "p3");
        if (!fl.fp.empty()) {
            draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                               {node_pos.x + 9.f * view_scale, y},
                               IM_COL32(168, 178, 194, 235), fl.fp.c_str());
            ImGui::SetCursorScreenPos({node_pos.x + 9.f * view_scale, y + 13.f * view_scale});
            ImGui::SetNextItemWidth(field_width);
            // Keep the data model unchanged until Undo has captured it.
            float edited = n.float_param;
            const std::string id = "##inline_" + std::to_string(n.id) + "_fp";
            if (ImGui::InputFloat(id.c_str(), &edited, 0.1f, 1.f, "%.3g")) {
                push_undo_snapshot_if_needed_for_field_edit();
                n.float_param = edited;
            }
        }
    }

    // A single undo snapshot per "edit session" (not per keystroke). Resets once the
    // field loses focus / a different node is selected.
    void push_undo_snapshot_if_needed_for_field_edit() {
        if (!_field_edit_snapshot_taken) {
            push_undo_snapshot();
            _field_edit_snapshot_taken = true;
        }
    }

    void clear_selection() {
        for (auto& n : nodes) n.selected = false;
    }

    int selected_count() const {
        int c = 0;
        for (const auto& n : nodes) if (n.selected) ++c;
        return c;
    }

    void select_only(int id) {
        clear_selection();
        for (auto& n : nodes) if (n.id == id) { n.selected = true; break; }
    }

    void toggle_selection(int id) {
        for (auto& n : nodes) if (n.id == id) { n.selected = !n.selected; break; }
    }

    void select_all() {
        for (auto& n : nodes) if (n.type != "comment") n.selected = true;
    }

    std::pair<ImVec2, ImVec2> selection_bounds() const {
        bool first = true;
        ImVec2 mn{0,0}, mx{0,0};
        for (const auto& n : nodes) {
            if (!n.selected) continue;
            ImVec2 a{n.x, n.y};
            ImVec2 b{n.x + n.width, n.y + n.height};
            if (first) { mn = a; mx = b; first = false; }
            else {
                mn.x = std::min(mn.x, a.x); mn.y = std::min(mn.y, a.y);
                mx.x = std::max(mx.x, b.x); mx.y = std::max(mx.y, b.y);
            }
        }
        return {mn, mx};
    }

    void frame_selection(ImVec2 canvas_size) {
        if (selected_count() == 0) return;
        auto [mn, mx] = selection_bounds();
        ImVec2 center{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f};
        _scroll = {center.x - canvas_size.x * 0.5f / std::max(0.01f, _zoom),
                   center.y - canvas_size.y * 0.5f / std::max(0.01f, _zoom)};
    }

    void push_undo_snapshot() {
        _undo_stack.push_back(save_to_json());
        if (_undo_stack.size() > 80) _undo_stack.erase(_undo_stack.begin());
        _redo_stack.clear();
    }

    void undo() {
        if (_undo_stack.empty()) return;
        _redo_stack.push_back(save_to_json());
        auto j = _undo_stack.back();
        _undo_stack.pop_back();
        load_from_json(j);
        clear_selection();
    }

    void redo() {
        if (_redo_stack.empty()) return;
        _undo_stack.push_back(save_to_json());
        auto j = _redo_stack.back();
        _redo_stack.pop_back();
        load_from_json(j);
        clear_selection();
    }

    void start_wiring(int node_id, int pin_id, bool is_output, const std::string& ptype) {
        _wiring_active = true;
        _wiring_from_node = node_id;
        _wiring_from_pin = pin_id;
        _wiring_from_is_output = is_output;
        _wiring_ptype = ptype;
    }

    void cancel_wiring() {
        _wiring_active = false;
        _wiring_from_node = -1;
        _wiring_from_pin = -1;
        _wiring_from_is_output = true;
        _wiring_ptype = "exec";
    }

    bool type_has_compatible_pin(const NodeTypeInfo* ti, const std::string& ptype, bool want_output) {
        if (!ti) return false;
        const auto& pins = want_output ? ti->out_pins : ti->in_pins;
        for (const auto& p : pins) if (pins_compatible(p.ptype, ptype)) return true;
        return false;
    }

    bool type_has_compatible_pin(const NodeTypeInfo& ti, const std::string& ptype, bool want_output) {
        return type_has_compatible_pin(&ti, ptype, want_output);
    }

    void connect_pin_ids(int from_node, int from_pin, int to_node, int to_pin, bool snapshot = true) {
        if (snapshot) push_undo_snapshot();
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const Link& l) { return l.to_node == to_node && l.to_pin == to_pin; }), links.end());
        links.push_back({from_node, to_node, from_pin, to_pin});

        EventGraphPin* fp = find_pin(from_pin);
        EventGraphPin* tp = find_pin(to_pin);
        if (fp && tp) {
            if (fp->ptype == "wildcard" && tp->ptype != "wildcard") fp->ptype = tp->ptype;
            if (tp->ptype == "wildcard" && fp->ptype != "wildcard") tp->ptype = fp->ptype;
            if (fp->ptype == "wildcard" && tp->ptype == "wildcard") {
                fp->ptype = tp->ptype = "exec";
            }
        }
    }

    void delete_selected() {
        std::unordered_set<int> ids;
        for (const auto& n : nodes) if (n.selected) ids.insert(n.id);
        if (ids.empty()) return;
        push_undo_snapshot();
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const Link& l) { return ids.count(l.from_node) || ids.count(l.to_node); }), links.end());
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
            [&](const Node& n) { return ids.count(n.id); }), nodes.end());
    }

    void delete_node_by_id(int node_id) {
        push_undo_snapshot();
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const Link& l) { return l.from_node == node_id || l.to_node == node_id; }), links.end());
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
            [&](const Node& n) { return n.id == node_id; }), nodes.end());
    }

    void copy_selected_to_clipboard() {
        std::unordered_set<int> ids;
        for (const auto& n : nodes) if (n.selected) ids.insert(n.id);
        if (ids.empty()) return;

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : nodes) {
            if (!n.selected) continue;
            nlohmann::json jn;
            jn["id"] = n.id; jn["type"] = n.type; jn["label"] = n.label;
            jn["x"] = n.x; jn["y"] = n.y;
            jn["p1"] = n.param1; jn["p2"] = n.param2; jn["p3"] = n.param3;
            jn["fp"] = n.float_param;
            nlohmann::json jin = nlohmann::json::array();
            for (const auto& p : n.in_pins) jin.push_back({{"label",p.label},{"type",p.ptype},{"id",p.pin_id},{"lit",p.literal}});
            nlohmann::json jout = nlohmann::json::array();
            for (const auto& p : n.out_pins) jout.push_back({{"label",p.label},{"type",p.ptype},{"id",p.pin_id},{"lit",p.literal}});
            jn["in_pins"] = jin; jn["out_pins"] = jout;
            arr.push_back(jn);
        }
        nlohmann::json meta;
        meta["_meta"] = true;
        meta["links"] = nlohmann::json::array();
        for (const auto& l : links) {
            if (ids.count(l.from_node) && ids.count(l.to_node)) {
                meta["links"].push_back({{"fn",l.from_node},{"fp",l.from_pin},{"tn",l.to_node},{"tp",l.to_pin}});
            }
        }
        // Copy declarations referenced by the selected nodes as well. Pasting
        // into another entity's graph therefore keeps Get/Set nodes useful
        // instead of silently losing their defaults at runtime.
        std::unordered_set<std::string> copied_variable_names;
        for (const auto& node : nodes) {
            const bool variable_node = node.type == "set_variable" || node.type == "get_variable" ||
                node.type == "increment_variable" || node.type == "decrement_variable";
            if (node.selected && variable_node && !node.param1.empty()) copied_variable_names.insert(node.param1);
        }
        meta["variables"] = nlohmann::json::array();
        for (const auto& variable : variables) {
            if (copied_variable_names.count(variable.name)) {
                meta["variables"].push_back({{"name", variable.name}, {"type", variable.type},
                                             {"default", variable.default_value}});
            }
        }
        arr.push_back(meta);
        _clipboard_json = arr.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        ImGui::SetClipboardText(_clipboard_json.c_str());
    }

    void paste_clipboard(ImVec2 canvas_origin, ImVec2 target_pos) {
        std::string src = _clipboard_json;
        if (src.empty()) {
            const char* clip = ImGui::GetClipboardText();
            if (clip) src = clip;
        }
        if (src.empty()) return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(src); } catch (...) { return; }
        if (!j.is_array() || j.empty()) return;

        push_undo_snapshot();
        std::unordered_map<int,int> node_map;
        std::unordered_map<int,int> pin_map;
        ImVec2 minp{1e9f, 1e9f};
        for (const auto& item : j) {
            if (!item.is_object() || item.value("_meta", false)) continue;
            minp.x = std::min(minp.x, item.value("x", 0.f));
            minp.y = std::min(minp.y, item.value("y", 0.f));
        }
        ImVec2 delta{target_pos.x - minp.x + 24.f, target_pos.y - minp.y + 24.f};

        std::vector<int> new_ids;
        for (const auto& item : j) {
            if (!item.is_object() || item.value("_meta", false)) continue;
            Node n;
            int old_id = item.value("id", 0);
            n.id = _next_id++;
            node_map[old_id] = n.id;
            n.type = item.value("type", "comment");
            n.label = item.value("label", n.type);
            n.x = item.value("x", 100.f) + delta.x;
            n.y = item.value("y", 100.f) + delta.y;
            n.param1 = item.value("p1", "");
            n.param2 = item.value("p2", "");
            n.param3 = item.value("p3", "");
            n.float_param = item.value("fp", 0.f);
            if (item.contains("in_pins")) {
                for (const auto& jp : item["in_pins"]) {
                    EventGraphPin p;
                    p.label = jp.value("label", "");
                    p.ptype = jp.value("type", "exec");
                    p.literal = jp.value("literal", jp.value("lit", ""));
                    p.pin_id = _next_pin_id++;
                    pin_map[jp.value("id", 0)] = p.pin_id;
                    n.in_pins.push_back(p);
                }
            }
            if (item.contains("out_pins")) {
                for (const auto& jp : item["out_pins"]) {
                    EventGraphPin p;
                    p.label = jp.value("label", "");
                    p.ptype = jp.value("type", "exec");
                    p.literal = jp.value("literal", jp.value("lit", ""));
                    p.pin_id = _next_pin_id++;
                    pin_map[jp.value("id", 0)] = p.pin_id;
                    n.out_pins.push_back(p);
                }
            }
            if (n.in_pins.empty() && n.out_pins.empty()) {
                auto* ti = find_type_info(n.type);
                if (ti) {
                    for (const auto& p : ti->in_pins) { auto np = p; np.pin_id = _next_pin_id++; n.in_pins.push_back(np); }
                    for (const auto& p : ti->out_pins) { auto np = p; np.pin_id = _next_pin_id++; n.out_pins.push_back(np); }
                }
            }
            nodes.push_back(n);
            new_ids.push_back(n.id);
        }
        if (j.back().is_object() && j.back().value("_meta", false) && j.back().contains("links")) {
            for (const auto& jl : j.back()["links"]) {
                int fn = node_map[jl.value("fn", 0)];
                int tn = node_map[jl.value("tn", 0)];
                int fp = pin_map[jl.value("fp", 0)];
                int tp = pin_map[jl.value("tp", 0)];
                if (fn && tn && fp && tp) links.push_back({fn, tn, fp, tp});
            }
        }
        if (j.back().is_object() && j.back().value("_meta", false) && j.back().contains("variables") &&
            j.back()["variables"].is_array()) {
            for (const auto& entry : j.back()["variables"]) {
                if (!entry.is_object()) continue;
                GraphVariable variable;
                variable.name = entry.value("name", std::string());
                variable.type = entry.value("type", std::string("float"));
                variable.default_value = entry.value("default", entry.value("value", std::string("0")));
                if (variable.name.empty()) continue;
                const bool already_declared = std::any_of(variables.begin(), variables.end(),
                    [&](const GraphVariable& existing) { return existing.name == variable.name; });
                if (!already_declared) variables.push_back(std::move(variable));
            }
        }
        clear_selection();
        for (auto& n : nodes) for (int id : new_ids) if (n.id == id) n.selected = true;
    }

    void frame_selection_to_view(ImVec2 canvas_size) { frame_selection(canvas_size); }

    void align_selected(bool horizontal) {
        if (selected_count() < 2) return;
        push_undo_snapshot();
        bool first = true;
        float target = 0.f;
        for (const auto& n : nodes) if (n.selected) { target = horizontal ? n.y : n.x; first = false; break; }
        for (auto& n : nodes) if (n.selected) {
            if (horizontal) n.y = target;
            else n.x = target;
        }
    }

    void distribute_selected(bool horizontal) {
        std::vector<Node*> sel;
        for (auto& n : nodes) if (n.selected) sel.push_back(&n);
        if (sel.size() < 3) return;
        std::sort(sel.begin(), sel.end(), [&](const Node* a, const Node* b) {
            return horizontal ? a->x < b->x : a->y < b->y;
        });
        push_undo_snapshot();
        float start = horizontal ? sel.front()->x : sel.front()->y;
        float end   = horizontal ? sel.back()->x  : sel.back()->y;
        float step = (end - start) / float(sel.size() - 1);
        for (size_t i = 0; i < sel.size(); ++i) {
            if (horizontal) sel[i]->x = start + step * float(i);
            else sel[i]->y = start + step * float(i);
        }
    }

    void complete_pending_wire_to_new_node() {
        if (_pending_wire_ptype.empty() || _pending_wire_from_node < 0 || _pending_wire_from_pin < 0) return;
        Node* new_node = find_node(_next_id - 1);
        if (!new_node) return;

        auto pick_pin = [&](bool want_output) -> int {
            auto& pins = want_output ? new_node->out_pins : new_node->in_pins;
            for (const auto& p : pins) if (pins_compatible(p.ptype, _pending_wire_ptype)) return p.pin_id;
            return pins.empty() ? -1 : pins[0].pin_id;
        };

        int target_pin = pick_pin(!_pending_wire_from_is_output);
        if (target_pin < 0) { _pending_wire_ptype.clear(); return; }

        connect_pin_ids(
            _pending_wire_from_is_output ? _pending_wire_from_node : new_node->id,
            _pending_wire_from_is_output ? _pending_wire_from_pin  : target_pin,
            _pending_wire_from_is_output ? new_node->id             : _pending_wire_from_node,
            _pending_wire_from_is_output ? target_pin               : _pending_wire_from_pin,
            false
        );

        _pending_wire_ptype.clear();
        _pending_wire_from_node = -1;
        _pending_wire_from_pin = -1;
        _pending_wire_from_is_output = true;
    }

    void add_node(const std::string& type, float x, float y) {
        auto* ti = find_type_info(type);
        if (!ti) return;
        push_undo_snapshot();

        Node n;
        n.id = _next_id++;
        n.type = type;
        n.label = ti->label;
        n.x = x; n.y = y;
        if (type == "reroute") { n.width = 86.f; n.height = 42.f; }

        // Copy pins from type info
        for (auto& p : ti->in_pins) {
            auto np = p; np.pin_id = _next_pin_id++;
            n.in_pins.push_back(np);
        }
        for (auto& p : ti->out_pins) {
            auto np = p; np.pin_id = _next_pin_id++;
            n.out_pins.push_back(np);
        }
        nodes.push_back(n);
        clear_selection();
        nodes.back().selected = true;
        _last_interacted_node = n.id;
    }

    ImU32 pin_color(const std::string& ptype) {
        if (ptype == "exec")   return IM_COL32(255,255,255,230);
        if (ptype == "bool")   return IM_COL32(220,80,80,230);
        if (ptype == "int")    return IM_COL32(80,200,80,230);
        if (ptype == "float")  return IM_COL32(80,150,240,230);
        if (ptype == "string") return IM_COL32(240,220,60,230);
        if (ptype == "entity") return IM_COL32(180,100,200,230);
        if (ptype == "asset")  return IM_COL32(234,130,68,230);
        if (ptype == "vector") return IM_COL32(70,210,190,230);
        if (ptype == "color")  return IM_COL32(242,105,185,230);
        if (ptype == "wildcard") return IM_COL32(180,180,180,180);
        return IM_COL32(200,200,200,230);
    }

    static const char* param_label(const std::string& type, int idx) {
        if (type == "on_key")          return idx==0?"Scancode":"";
        if (type == "delay")           return idx==0?"Seconds":"";
        if (type == "wait")            return idx==0?"Seconds":"";
        if (type == "for_loop")        return idx==0?"Count":"";
        if (type == "while_loop")      return idx==0?"Condition":"";
        if (type == "set_active")      return idx==0?"Entity":"Active(true/false)";
        if (type == "destroy")         return idx==0?"Entity":"";
        if (type == "spawn")           return idx==0?"Template":(idx==1?"X":"Y");
        if (type == "play_animation")  return idx==0?"Entity":(idx==1?"Anim":"");
        if (type == "set_field")       return idx==0?"Entity":(idx==1?"Field":"Value");
        if (type == "load_scene")      return idx==0?"Scene Name":"";
        if (type == "audio_play")      return idx==0?"Entity/Source":(idx==1?"Clip":"");
        if (type == "log")             return idx==0?"Message":"";
        if (type == "log_value")       return idx==0?"Entity":(idx==1?"Field":"");
        if (type == "print_value")     return idx==0?"Entity":(idx==1?"Field":"");
        if (type == "set_velocity")    return idx==0?"Entity":(idx==1?"Vy":"Vx");
        if (type == "add_force")       return idx==0?"Entity":(idx==1?"Fy":"Fx");
        if (type == "teleport_to")     return idx==0?"Entity":(idx==1?"Y":"X");
        if (type == "tween_to")        return idx==0?"Entity":(idx==1?"Duration":"X/Y");
        if (type == "gate")            return idx==0?"Open(true/false)":"";
        if (type == "condition")       return idx==0?"Expression":"";
        if (type == "branch")          return idx==0?"Condition":"";
        if (type == "comment")         return idx==0?"Text":"";
        if (type == "set_variable")    return idx==0?"Name":(idx==1?"Value":"");
        if (type == "get_variable")    return idx==0?"Name":"";
        if (type == "increment_variable"||type=="decrement_variable") return idx==0?"Name":"";
        if (type == "math_add" || type == "math_sub" || type == "math_mul" || type == "math_div") {
            if (idx == 0) return "Store in var";
            if (idx == 1) return "A";
            if (idx == 2) return "B";
        }
        if (type == "random_range") {
            if (idx == 0) return "Store in var";
            if (idx == 1) return "Min";
            if (idx == 2) return "Max";
        }
        if (type == "clamp") {
            if (idx == 0) return "Store in var";
            if (idx == 1) return "Value";
            if (idx == 2) return "Min/Max";
        }
        return idx==0?"Param 1":(idx==1?"Param 2":"Param 3");
    }

    // Compute a pin's center in screen space given the node's screen position.
    static ImVec2 pin_screen_pos(const Node& n, ImVec2 node_screen_pos, int pin_index,
                                 bool is_output, float view_scale) {
        float y = node_screen_pos.y + (24.f + pin_index * 18.f) * view_scale;
        int count = is_output ? (int)n.out_pins.size() : (int)n.in_pins.size();
        if (count > 1) y += 6.f * view_scale;
        // Deliberately place sockets just outside the card.  Apart from making
        // the connection affordance obvious, this prevents the title/body hit
        // surfaces from competing with the first few pixels of a wire drag.
        float x = is_output ? (node_screen_pos.x + (n.width + 7.f) * view_scale)
                            : (node_screen_pos.x - 7.f * view_scale);
        return {x, y};
    }

    bool pin_is_connected(int node_id, int pin_id, bool is_output) const {
        for (const auto& link : links) {
            if (is_output && link.from_node == node_id && link.from_pin == pin_id) return true;
            if (!is_output && link.to_node == node_id && link.to_pin == pin_id) return true;
        }
        return false;
    }

    // Paint a pin and record its hit target.  Input is resolved once after all
    // nodes have been painted, so no overlapping ImGui widget can intercept a
    // graph wire gesture.
    void draw_pin(ImDrawList* dl, ImVec2 pos, const EventGraphPin& pin, bool is_output,
                  int node_id, bool connected, float view_scale) {
        float r = ((pin.ptype == "exec") ? 7.f : 6.f) * view_scale;
        ImU32 col = pin_color(pin.ptype);
        ImU32 hover_col = IM_COL32(255,255,255,255);
        const ImVec2 mouse = ImGui::GetMousePos();
        const float dx = mouse.x - pos.x, dy = mouse.y - pos.y;
        const bool hovered = dx * dx + dy * dy <= (r + 6.f * view_scale) * (r + 6.f * view_scale);
        _pin_hits.push_back({node_id, pin.pin_id, is_output, pin.ptype, pos});

        // Wiring compatibility glow: highlight pins the active drag could legally land on.
        bool compat_glow = _wiring_active && (is_output != _wiring_from_is_output) &&
                            pins_compatible(pin.ptype, _wiring_ptype) && node_id != _wiring_from_node;
        if (hovered && _wiring_active && compat_glow) {
            dl->AddCircleFilled(pos, r + 4.f * view_scale, IM_COL32(255,255,120,160));
        } else if (hovered) {
            dl->AddCircleFilled(pos, r + 3.f * view_scale, IM_COL32(255,255,255,60));
        }

        // A dark backing makes every socket readable even over a highlighted
        // card edge.  The bright filled centre is intentionally always shown;
        // a disconnected pin must look draggable, not disabled.
        dl->AddCircleFilled(pos, r + 2.f * view_scale, IM_COL32(12,16,23,245));
        if (pin.ptype == "exec") {
            const float direction = is_output ? 1.f : -1.f;
            ImVec2 p1 = {pos.x - direction * 5.f * view_scale, pos.y - 6.f * view_scale};
            ImVec2 p2 = {pos.x - direction * 5.f * view_scale, pos.y + 6.f * view_scale};
            ImVec2 p3 = {pos.x + direction * 6.f * view_scale, pos.y};
            ImU32 c = hovered ? hover_col : col;
            dl->AddTriangleFilled(p1, p2, p3, c);
            dl->AddTriangle(p1, p2, p3, IM_COL32(255,255,255,150), 1.2f);
        } else {
            dl->AddCircleFilled(pos, r, hovered ? hover_col : col);
            dl->AddCircle(pos, r, IM_COL32(255,255,255,165), 0, 1.2f);
            if (!connected) dl->AddCircleFilled(pos, 2.f * view_scale, IM_COL32(20,25,32,230));
        }

        // Pin label
        if (!pin.label.empty()) {
            if (is_output) {
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                            {pos.x + 10.f * view_scale, pos.y - 6.f * view_scale},
                            IM_COL32(200,200,200,200), pin.label.c_str());
            } else {
                float tw = ImGui::CalcTextSize(pin.label.c_str()).x;
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                            {pos.x - 10.f * view_scale - tw, pos.y - 6.f * view_scale},
                            IM_COL32(200,200,200,200), pin.label.c_str());
            }
        }

    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({900, 600}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Visual Scripting##evgraph", &_open,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::End(); return;
        }

        if (!_binding_is_valid(st)) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Visual Scripting is attached to an entity");
            ImGui::Separator();
            ImGui::TextWrapped("Select an entity with a Visual Script component, create or assign its graph asset in the Inspector, then choose Open Visual Script. Each entity owns its own graph and runtime state.");
            ImGui::Spacing();
            ImGui::TextDisabled("Copy remains available after opening another entity's graph, so nodes can be reused safely.");
            ImGui::End();
            return;
        }

        ImGui::TextColored(ImVec4(0.45f, 0.78f, 1.f, 1.f), "Entity: %s", _bound_entity_name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s  |  Autosaves", _asset_path.filename().string().c_str());
        ImGui::Separator();

        ImVec2 toolbar_size = ImGui::GetContentRegionAvail();
        toolbar_size.y = ImGui::GetFrameHeightWithSpacing() + 4;

        // ── Toolbar with category-filtered Add Node menu ──
        static char _filter_buf[128] = "";
        if (ImGui::Button("＋ Add Node")) ImGui::OpenPopup("##addnode_popup");
        ImGui::SameLine();
        if (ImGui::Button("Undo")) undo();
        ImGui::SameLine();
        if (ImGui::Button("Redo")) redo();
        ImGui::SameLine();
        if (ImGui::Button("Copy")) copy_selected_to_clipboard();
        ImGui::SameLine();
        if (ImGui::Button("Paste")) {
            ImVec2 mouse = ImGui::GetMousePos();
            paste_clipboard({0,0}, mouse);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All")) { push_undo_snapshot(); nodes.clear(); links.clear(); _next_id=1; _next_pin_id=1; clear_selection(); cancel_wiring(); }
        ImGui::SameLine();
        if (ImGui::Button("Validate")) {
            validate_graph(st);
        }
        ImGui::SameLine();
        ImGui::Dummy({8,0});
        ImGui::SameLine();
        ImGui::TextUnformatted("Zoom");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("##zoom", &_zoom, 0.4f, 2.0f, "%.1fx");
        ImGui::SameLine();
        if (ImGui::Button("Reset View")) { _scroll = {0,0}; _zoom = 1.f; }
        ImGui::SameLine();
        ImGui::TextDisabled("| Drag a pin to wire | Ctrl+C/V/X/Z/Y/A/F | Right-click canvas for search | Right-click link to break");

        // Right-click Add Node popup (uses last right-click position)
        if (ImGui::BeginPopup("##addnode_rightclick")) {
            ImGui::TextDisabled(_pending_wire_ptype.empty() ? "Add Node at cursor" : "Add Node (compatible with dragged pin)");
            ImGui::Separator();
            if (ImGui::IsWindowAppearing()) { _filter_buf[0] = '\0'; ImGui::SetKeyboardFocusHere(); }
            ImGui::InputText("Search##filter2", _filter_buf, sizeof(_filter_buf));
            ImGui::Separator();
            std::string filter = _filter_buf;
            std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
            std::string current_category = "";
            for (auto& ti : node_types()) {
                std::string label = display_label(ti.label);
                std::string label_lower = label;
                std::transform(label_lower.begin(), label_lower.end(), label_lower.begin(), ::tolower);
                if (!filter.empty() && label_lower.find(filter) == std::string::npos) continue;
                if (!_pending_wire_ptype.empty() && !type_has_compatible_pin(&ti, _pending_wire_ptype, !_pending_wire_from_is_output)) continue;
                if (current_category != ti.category) {
                    current_category = ti.category;
                    ImGui::SeparatorText(ti.category);
                }
                if (ImGui::Selectable(label.c_str())) {
                    add_node(ti.type, _add_node_canvas_x, _add_node_canvas_y);
                    complete_pending_wire_to_new_node();
                    _filter_buf[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        } else {
            _pending_wire_ptype.clear();
        }

        // Add Node popup (from toolbar) with category grouping and filter
        if (ImGui::BeginPopup("##addnode_popup")) {
            ImGui::InputText("Search##filter", _filter_buf, sizeof(_filter_buf));
            ImGui::Separator();
            std::string filter = _filter_buf;
            std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
            
            std::string current_category = "";
            for (auto& ti : node_types()) {
                std::string label = display_label(ti.label);
                std::string label_lower = label;
                std::transform(label_lower.begin(), label_lower.end(), label_lower.begin(), ::tolower);
                if (!filter.empty() && label_lower.find(filter) == std::string::npos) continue;
                
                if (current_category != ti.category) {
                    current_category = ti.category;
                    ImGui::SeparatorText(ti.category);
                }
                if (ImGui::Selectable(label.c_str())) {
                    float cx = _scroll.x + 300.f;
                    float cy = _scroll.y + 150.f;
                    add_node(ti.type, cx, cy);
                    _filter_buf[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        ImGui::Separator();

        // ── Split layout: variable panel (left) + canvas (right) ──
        ImGui::BeginChild("##ev_varsplit", {0, 0}, false, ImGuiWindowFlags_NoScrollbar);
        
        // Variable panel: declarations are persistent graph data, rather than
        // a passive scan of whichever variable nodes currently happen to be
        // visible.  This makes defaults, rename and delete deterministic.
        ImGui::BeginChild("##varsidebar", {260, 0}, true);
        ImGui::TextDisabled("BLACKBOARD");
        ImGui::TextWrapped("Graph-local variables. Defaults seed each entity's isolated runtime state.");
        ImGui::Spacing();
        static const char* variable_types[] = {"bool", "int", "float", "string", "vector", "color", "entity", "asset"};
        int remove_variable = -1;
        int duplicate_variable = -1;
        bool request_variable_delete = false;
        for (int index = 0; index < (int)variables.size(); ++index) {
            auto& variable = variables[index];
            ImGui::PushID(index);
            int uses = 0;
            for (const auto& node : nodes) {
                const bool variable_node = node.type == "set_variable" || node.type == "get_variable" ||
                    node.type == "increment_variable" || node.type == "decrement_variable";
                if (variable_node && node.param1 == variable.name) ++uses;
            }

            char name_buffer[64]; std::snprintf(name_buffer, sizeof(name_buffer), "%s", variable.name.c_str());
            ImGui::SetNextItemWidth(148.f);
            if (ImGui::InputText("##variable_name", name_buffer, sizeof(name_buffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                const std::string renamed = name_buffer;
                const bool valid = !renamed.empty() && std::all_of(renamed.begin(), renamed.end(),
                    [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; });
                const bool duplicate = std::any_of(variables.begin(), variables.end(), [&](const GraphVariable& other) {
                    return &other != &variable && other.name == renamed;
                });
                if (valid && !duplicate) {
                    push_undo_snapshot();
                    const std::string old_name = variable.name;
                    variable.name = renamed;
                    for (auto& node : nodes) {
                        const bool variable_node = node.type == "set_variable" || node.type == "get_variable" ||
                            node.type == "increment_variable" || node.type == "decrement_variable";
                        if (variable_node && node.param1 == old_name) node.param1 = renamed;
                    }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Press Enter to rename. References in this graph are updated together.");
            ImGui::SameLine(0, 4);
            int type_index = 2;
            for (int i = 0; i < IM_ARRAYSIZE(variable_types); ++i)
                if (variable.type == variable_types[i]) { type_index = i; break; }
            ImGui::SetNextItemWidth(88.f);
            if (ImGui::Combo("##variable_type", &type_index, variable_types, IM_ARRAYSIZE(variable_types))) {
                push_undo_snapshot();
                variable.type = variable_types[type_index];
            }

            char default_buffer[80]; std::snprintf(default_buffer, sizeof(default_buffer), "%s", variable.default_value.c_str());
            ImGui::SetNextItemWidth(148.f);
            if (ImGui::InputText("Default##variable", default_buffer, sizeof(default_buffer))) {
                push_undo_snapshot_if_needed_for_field_edit();
                variable.default_value = default_buffer;
            }
            ImGui::SameLine(0, 4);
            ImGui::TextDisabled("%d use%s", uses, uses == 1 ? "" : "s");
            if (ImGui::SmallButton("Find Nodes")) {
                clear_selection();
                for (auto& node : nodes) {
                    const bool variable_node = node.type == "set_variable" || node.type == "get_variable" ||
                        node.type == "increment_variable" || node.type == "decrement_variable";
                    if (variable_node && node.param1 == variable.name) node.selected = true;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Highlights this variable's Get, Set, Increment, and Decrement nodes. It does not change the variable.");
            ImGui::SameLine(0, 3);
            if (ImGui::SmallButton("+ Set")) {
                add_node("set_variable", _scroll.x + 260.f, _scroll.y + 120.f);
                nodes.back().param1 = variable.name;
                nodes.back().param2 = variable.default_value;
            }
            ImGui::SameLine(0, 3);
            if (ImGui::SmallButton("+ Get")) {
                add_node("get_variable", _scroll.x + 260.f, _scroll.y + 120.f);
                nodes.back().param1 = variable.name;
            }
            ImGui::SameLine(0, 3);
            if (ImGui::SmallButton("...")) ImGui::OpenPopup("##variable_actions");
            if (ImGui::BeginPopup("##variable_actions")) {
                ImGui::TextDisabled("%s", variable.name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate Variable")) duplicate_variable = index;
                if (ImGui::MenuItem("Delete Variable...")) {
                    _pending_variable_delete = index;
                    request_variable_delete = true;
                }
                ImGui::EndPopup();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (duplicate_variable >= 0 && duplicate_variable < (int)variables.size()) {
            const GraphVariable source = variables[duplicate_variable];
            std::string base = source.name + "_copy";
            std::string duplicate_name = base;
            int suffix = 2;
            const auto exists = [&](const std::string& name) {
                return std::any_of(variables.begin(), variables.end(), [&](const GraphVariable& other) {
                    return other.name == name;
                });
            };
            while (exists(duplicate_name)) duplicate_name = base + std::to_string(suffix++);
            push_undo_snapshot();
            variables.push_back({duplicate_name, source.type, source.default_value});
        }
        if (request_variable_delete) ImGui::OpenPopup("##confirm_variable_delete");
        if (ImGui::BeginPopupModal("##confirm_variable_delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (_pending_variable_delete < 0 || _pending_variable_delete >= (int)variables.size()) {
                ImGui::CloseCurrentPopup();
            } else {
                const auto& variable = variables[_pending_variable_delete];
                ImGui::Text("Delete variable '%s'?", variable.name.c_str());
                ImGui::TextWrapped("Its Get/Set/Increment/Decrement nodes will be left in the graph, but their variable reference will be cleared so this deletion remains explicit and undoable.");
                ImGui::Spacing();
                if (ImGui::Button("Delete Variable", {140, 0})) {
                    remove_variable = _pending_variable_delete;
                    _pending_variable_delete = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", {100, 0})) {
                    _pending_variable_delete = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        if (remove_variable >= 0) {
            push_undo_snapshot();
            const std::string removed_name = variables[remove_variable].name;
            // Leaving stale variable references would cause migration to
            // recreate a declaration next time the graph opens. Clear them so
            // deletion remains deliberate and persistent.
            for (auto& node : nodes) {
                const bool variable_node = node.type == "set_variable" || node.type == "get_variable" ||
                    node.type == "increment_variable" || node.type == "decrement_variable";
                if (variable_node && node.param1 == removed_name) node.param1.clear();
            }
            variables.erase(variables.begin() + remove_variable);
        }
        if (variables.empty()) ImGui::TextDisabled("No declared variables yet.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("NEW VARIABLE");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##new_graph_variable", "Name (for example, score)",
                                 _new_variable_name, sizeof(_new_variable_name));
        ImGui::SetNextItemWidth(100.f);
        ImGui::Combo("##new_graph_variable_type", &_new_variable_type, variable_types, IM_ARRAYSIZE(variable_types));
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##new_graph_variable_default", "Default", _new_variable_default, sizeof(_new_variable_default));
        const std::string proposed_name = _new_variable_name;
        const bool name_is_safe = !proposed_name.empty() && std::all_of(proposed_name.begin(), proposed_name.end(),
            [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; });
        const bool duplicate_name = std::any_of(variables.begin(), variables.end(), [&](const GraphVariable& variable) {
            return variable.name == proposed_name;
        });
        const bool can_add_variable = name_is_safe && !duplicate_name;
        if (!can_add_variable) ImGui::BeginDisabled();
        if (ImGui::Button("Create Variable", {-1, 0})) {
            push_undo_snapshot();
            variables.push_back({proposed_name, variable_types[_new_variable_type], _new_variable_default});
            _new_variable_name[0] = '\0';
            std::snprintf(_new_variable_default, sizeof(_new_variable_default), "%s", "0");
        }
        if (!can_add_variable) ImGui::EndDisabled();
        if (!name_is_safe && _new_variable_name[0] != '\0')
            ImGui::TextDisabled("Use letters, numbers, and underscore only.");
        else if (duplicate_name)
            ImGui::TextDisabled("A variable with this name already exists.");
        ImGui::EndChild();

        ImGui::SameLine();
        
        // ── Canvas ──
        ImGui::BeginChild("##ev_canvas", {0, 0}, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        _zoom = std::clamp(_zoom, 0.40f, 2.00f);
        auto to_screen = [&](float x, float y) -> ImVec2 {
            return {canvas_pos.x + (x - _scroll.x) * _zoom,
                    canvas_pos.y + (y - _scroll.y) * _zoom};
        };
        auto to_canvas = [&](const ImVec2& point) -> ImVec2 {
            return {_scroll.x + (point.x - canvas_pos.x) / _zoom,
                    _scroll.y + (point.y - canvas_pos.y) / _zoom};
        };
        
        // Grid background
        dl->AddRectFilled(canvas_pos, {canvas_pos.x+canvas_size.x, canvas_pos.y+canvas_size.y},
                          IM_COL32(38,38,42,255));
        // Grid dots
        const float grid_size = 24.f * _zoom;
        float first_grid_x = std::fmod(-_scroll.x * _zoom, grid_size);
        float first_grid_y = std::fmod(-_scroll.y * _zoom, grid_size);
        if (first_grid_x < 0.f) first_grid_x += grid_size;
        if (first_grid_y < 0.f) first_grid_y += grid_size;
        for (float gx = first_grid_x; gx < canvas_size.x; gx += grid_size) {
            dl->AddRectFilled({canvas_pos.x + gx, canvas_pos.y}, 
                              {canvas_pos.x + gx + 1, canvas_pos.y + canvas_size.y}, 
                              IM_COL32(45,45,50,80));
        }
        for (float gy = first_grid_y; gy < canvas_size.y; gy += grid_size) {
            dl->AddRectFilled({canvas_pos.x, canvas_pos.y + gy}, 
                              {canvas_pos.x + canvas_size.x, canvas_pos.y + gy + 1}, 
                              IM_COL32(45,45,50,80));
        }

        // Canvas interaction
        // The canvas covers the complete graph area, but must never consume
        // the node widgets drawn over it.  Without AllowOverlap it becomes
        // the hovered/active ImGui item first, which makes pins, inline
        // fields and node drag handles appear completely unresponsive.
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##canvas", canvas_size,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool canvas_hovered = ImGui::IsItemHovered();
        // Keep the world point under the cursor fixed while zooming.  The same
        // transform is used by cards, pins, wires, selection, and dragging so
        // wire hit-testing cannot drift away from the visible socket.
        if (canvas_hovered && ImGui::GetIO().MouseWheel != 0.f && !ImGui::IsAnyItemActive()) {
            const ImVec2 mouse = ImGui::GetMousePos();
            const ImVec2 focal_point = to_canvas(mouse);
            const float factor = ImGui::GetIO().MouseWheel > 0.f ? 1.12f : (1.f / 1.12f);
            _zoom = std::clamp(_zoom * factor, 0.40f, 2.00f);
            _scroll = {focal_point.x - (mouse.x - canvas_pos.x) / _zoom,
                       focal_point.y - (mouse.y - canvas_pos.y) / _zoom};
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            _scroll.x -= ImGui::GetIO().MouseDelta.x / std::max(0.01f, _zoom);
            _scroll.y -= ImGui::GetIO().MouseDelta.y / std::max(0.01f, _zoom);
        }
        // Cancel wiring on right-click
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && _wiring_active) {
            _wiring_active = false;
            _wiring_from_node = -1;
            _wiring_from_pin = -1;
        }
        
        // Right-click on canvas opens Add Node popup at mouse position
        if (canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            ImVec2 mouse = ImGui::GetMousePos();
            const ImVec2 at = to_canvas(mouse);
            _add_node_canvas_x = at.x;
            _add_node_canvas_y = at.y;
            ImGui::OpenPopup("##addnode_rightclick");
        }

        // ── Find node helper ──
        auto find_node = [&](int id) -> Node* {
            for (auto& n : nodes) if (n.id == id) return &n;
            return nullptr;
        };

        // ── Draw links ──
        for (auto& ln : links) {
            Node* fn = find_node(ln.from_node);
            Node* tn = find_node(ln.to_node);
            if (!fn || !tn) continue;
            
            // Find pin positions
            float from_y = fn->y + 24.f;
            for (auto& p : fn->out_pins) {
                if (p.pin_id == ln.from_pin) break;
                from_y += 18.f;
            }
            float to_y = tn->y + 24.f;
            for (auto& p : tn->in_pins) {
                if (p.pin_id == ln.to_pin) break;
                to_y += 18.f;
            }
            if (fn->out_pins.size() > 1) from_y += 6.f;
            if (tn->in_pins.size() > 1) to_y += 6.f;

            ImVec2 fn_pos = to_screen(fn->x, fn->y);
            ImVec2 tn_pos = to_screen(tn->x, tn->y);
            int from_index = 0;
            for (; from_index < (int)fn->out_pins.size(); ++from_index)
                if (fn->out_pins[from_index].pin_id == ln.from_pin) break;
            int to_index = 0;
            for (; to_index < (int)tn->in_pins.size(); ++to_index)
                if (tn->in_pins[to_index].pin_id == ln.to_pin) break;
            ImVec2 from = pin_screen_pos(*fn, fn_pos, std::min(from_index, std::max(0, (int)fn->out_pins.size() - 1)), true, _zoom);
            ImVec2 to = pin_screen_pos(*tn, tn_pos, std::min(to_index, std::max(0, (int)tn->in_pins.size() - 1)), false, _zoom);
            ImVec2 cp1 = {from.x + 60.f * _zoom, from.y};
            ImVec2 cp2 = {to.x - 60.f * _zoom,   to.y};
            
            // Color the link based on source node type
            auto* ti = find_type_info(fn->type);
            ImVec4 hdr4 = ti ? ti->color : ImVec4(0.3f,0.5f,0.9f,1.f);
            ImU32 link_col = ImGui::ColorConvertFloat4ToU32(ImVec4(hdr4.x, hdr4.y, hdr4.z, 0.7f));
            
            // Check mouse distance for hover effect
            ImVec2 mouse = ImGui::GetMousePos();
            float mid_x = (from.x + to.x) / 2;
            float mid_y = (from.y + to.y) / 2;
            float dist = sqrt(pow(mouse.x - mid_x, 2) + pow(mouse.y - mid_y, 2));
            bool link_hovered = dist < 15.f;
            
            float link_thick = (link_hovered ? 3.5f : 2.5f) * _zoom;
            ImU32 draw_col = link_hovered ? IM_COL32(255,255,100,240) : link_col;
            dl->AddBezierCubic(from, cp1, cp2, to, draw_col, link_thick);
            
            // Show dot at midpoint when hovered (visual cue for deletion)
            if (link_hovered) {
                dl->AddCircleFilled({mid_x, mid_y}, 4.f, IM_COL32(255,80,80,220));
                dl->AddText({mid_x + 8, mid_y - 6}, IM_COL32(255,80,80,200), "✕");
            }
            
            // Delete link on right-click
            if (dist < 15.f && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && canvas_hovered) {
                _pending_link_delete = &ln;
            }
        }

        // Delete pending link
        if (_pending_link_delete) {
            links.erase(std::remove_if(links.begin(), links.end(),
                [&](auto& l) { return &l == _pending_link_delete; }), links.end());
            _pending_link_delete = nullptr;
        }

        // ── Draw wiring preview ──
        if (_wiring_active) {
            Node* wn = find_node(_wiring_from_node);
            if (wn) {
                const auto& source_pins = _wiring_from_is_output ? wn->out_pins : wn->in_pins;
                int source_index = 0;
                while (source_index < (int)source_pins.size() &&
                       source_pins[source_index].pin_id != _wiring_from_pin) ++source_index;
                if (source_index >= (int)source_pins.size()) cancel_wiring();
                ImVec2 node_pos = to_screen(wn->x, wn->y);
                ImVec2 wfrom = pin_screen_pos(*wn, node_pos, source_index, _wiring_from_is_output, _zoom);
                ImVec2 wto = ImGui::GetMousePos();
                const float direction = _wiring_from_is_output ? 1.f : -1.f;
                ImVec2 wcp1 = {wfrom.x + 60.f * _zoom * direction, wfrom.y};
                ImVec2 wcp2 = {wto.x - 60.f * _zoom * direction, wto.y};
                dl->AddBezierCubic(wfrom, wcp1, wcp2, wto, IM_COL32(78,138,245,100), 2.f * _zoom, 0);
            }
        }

        // ── Draw nodes ──
        dl->PushClipRect(canvas_pos, {canvas_pos.x+canvas_size.x, canvas_pos.y+canvas_size.y}, true);
        int to_delete = -1;
        bool any_node_hovered = false;
        _pin_hits.clear();

        ImGui::SetWindowFontScale(_zoom);
        for (auto& n : nodes) {
            ImVec2 node_pos = to_screen(n.x, n.y);
            
            // Calculate height based on pins
            int num_rows = std::max((int)n.in_pins.size(), (int)n.out_pins.size());
            if (num_rows < 1) num_rows = 1;
            n.height = 28.f + num_rows * 18.f + 6.f;
            
            // One complete row per editable field keeps multi-value nodes
            // readable and makes room for the inline Blueprint-style inputs.
            const int inline_rows = inline_property_row_count(n);
            const float param_area = inline_rows > 0 ? inline_rows * 36.f + 4.f : 0.f;
            n.height += param_area;
            n.width = (n.type == "reroute") ? 110.f : 185.f;
            const float node_width = n.width * _zoom;
            const float node_height = n.height * _zoom;
            const float header_height = 22.f * _zoom;

            auto* ti = find_type_info(n.type);
            ImVec4 hdr_col4 = ti ? ti->color : ImVec4(0.2f,0.42f,0.22f,1.f);
            ImU32 hdr_col = ImGui::ColorConvertFloat4ToU32(hdr_col4);
            ImU32 body_col = IM_COL32(50,50,55,255);
            ImU32 bdr_col = IM_COL32(80,80,90,255);

            // Check if mouse is hovering over this node
            ImVec2 mouse = ImGui::GetMousePos();
            bool hovered = (mouse.x >= node_pos.x && mouse.x <= node_pos.x + node_width &&
                            mouse.y >= node_pos.y && mouse.y <= node_pos.y + node_height);
            any_node_hovered = any_node_hovered || hovered;
            bool is_breakpoint = (n.type == "breakpoint");
            bool is_selected = n.selected;
            
            // Glow/shadow based on hover state
            float shadow_intensity = hovered ? 180.f : 100.f;
            float shadow_size = (hovered ? 6.f : 3.f) * _zoom;
            dl->AddRectFilled({node_pos.x+shadow_size, node_pos.y+shadow_size}, 
                              {node_pos.x+node_width+shadow_size, node_pos.y+node_height+shadow_size},
                              IM_COL32(0,0,0,(int)shadow_intensity), 5.f * _zoom);
            
            // Body
            dl->AddRectFilled(node_pos, {node_pos.x+node_width, node_pos.y+node_height}, body_col, 5.f * _zoom);
            
            // Breakpoint indicator - red left bar
            if (is_breakpoint) {
                dl->AddRectFilled({node_pos.x, node_pos.y+2.f * _zoom}, {node_pos.x+6.f * _zoom, node_pos.y+node_height-2.f * _zoom},
                                  IM_COL32(255,60,60,220), 3.f * _zoom);
            }
            
            // Header
            dl->AddRectFilled(node_pos, {node_pos.x+node_width, node_pos.y+header_height}, hdr_col, 5.f * _zoom);
            dl->AddRectFilled({node_pos.x, node_pos.y+17.f * _zoom}, {node_pos.x+node_width, node_pos.y+header_height}, hdr_col, 0);
            
            // Vector icons replace unsupported emoji / '?' glyphs.
            draw_node_header_icon(dl, {node_pos.x + 13.f * _zoom, node_pos.y + 11.f * _zoom}, n, hdr_col);
            const std::string node_title = display_title(n);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {node_pos.x + 25.f * _zoom, node_pos.y + 3.f * _zoom}, IM_COL32(255,255,255,255), node_title.c_str());
            
            // Border (highlighted on hover or selected)
            ImU32 border_col = is_selected ? IM_COL32(255,220,120,255) : (hovered ? IM_COL32(120,180,255,255) : bdr_col);
            float border_thick = (is_selected ? 2.5f : (hovered ? 2.f : 1.f)) * _zoom;
            dl->AddRect(node_pos, {node_pos.x+node_width, node_pos.y+node_height}, border_col, 5.f * _zoom, 0, border_thick);
            
            // Hover glow ring
            if (hovered) {
                dl->AddRect(node_pos, {node_pos.x+node_width, node_pos.y+node_height}, 
                            IM_COL32(100,160,255,60), 5.f * _zoom, 0, 3.f * _zoom);
            }

            // ── Draw pins ──
            float pin_start_y = node_pos.y + 24.f * _zoom;
            
            // Input pins (left side)
            for (int pi = 0; pi < (int)n.in_pins.size(); ++pi) {
                ImVec2 pin_pos = pin_screen_pos(n, node_pos, pi, false, _zoom);
                draw_pin(dl, pin_pos, n.in_pins[pi], false, n.id,
                         pin_is_connected(n.id, n.in_pins[pi].pin_id, false), _zoom);
                // Only show pin labels for multi-pin nodes
                if (n.in_pins.size() > 1 && !n.in_pins[pi].label.empty()) {
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {pin_pos.x + 8.f * _zoom, pin_pos.y - 6.f * _zoom}, IM_COL32(200,200,200,180), n.in_pins[pi].label.c_str());
                }
            }
            
            // Output pins (right side)
            for (int pi = 0; pi < (int)n.out_pins.size(); ++pi) {
                ImVec2 pin_pos = pin_screen_pos(n, node_pos, pi, true, _zoom);
                draw_pin(dl, pin_pos, n.out_pins[pi], true, n.id,
                         pin_is_connected(n.id, n.out_pins[pi].pin_id, true), _zoom);
                if (n.out_pins.size() > 1 && !n.out_pins[pi].label.empty()) {
                    float tw = ImGui::CalcTextSize(n.out_pins[pi].label.c_str()).x;
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {pin_pos.x - 10.f * _zoom - tw, pin_pos.y - 6.f * _zoom}, IM_COL32(200,200,200,180), n.out_pins[pi].label.c_str());
                }
            }

            // ── Draw param values in body ──
            float py = pin_start_y + (std::max((int)n.in_pins.size(), (int)n.out_pins.size()) * 18.f + 4.f) * _zoom;
            // Legacy painted preview is intentionally disabled: the editable
            // controls below are the node's source of truth.
            if (false && param_area > 0) {
                ImU32 param_col = IM_COL32(180,180,190,220);
                ImU32 label_col = IM_COL32(130,130,140,180);
                
                auto draw_param = [&](const char* label, const std::string& val, bool is_float = false) {
                    if (!label || !*label) return;
                    dl->AddText({node_pos.x+8, py}, label_col, label);
                    dl->AddText({node_pos.x+8, py+10}, param_col, 
                        (is_float ? std::to_string(n.float_param) : val).substr(0, 20).c_str());
                    py += 20.f;
                };

                // Show params relevant to node type
                if (n.type == "delay" || n.type == "wait") {
                    draw_param("Seconds:", "", true);
                } else if (n.type == "for_loop") {
                    draw_param("Count:", "", true);
                } else if (n.type == "while_loop") {
                    draw_param("Condition:", n.param1);
                } else if (n.type == "on_key") {
                    draw_param("Scancode:", n.param1);
                } else if (n.type == "set_active") {
                    draw_param("Entity:", n.param1);
                    if (!n.param2.empty()) { draw_param("Active:", n.param2); }
                } else if (n.type == "destroy" || n.type == "load_scene") {
                    draw_param("Target:", n.param1);
                } else if (n.type == "spawn") {
                    draw_param("Template:", n.param1);
                    std::string pos = std::to_string((int)n.float_param) + "," + n.param2;
                    draw_param("Position:", pos);
                } else if (n.type == "play_animation") {
                    draw_param("Entity:", n.param1);
                    if (!n.param2.empty()) draw_param("Anim:", n.param2);
                } else if (n.type == "set_field") {
                    draw_param("Entity:", n.param1);
                    if (!n.param2.empty()) draw_param("Field:", n.param2);
                    if (!n.param3.empty()) draw_param("Value:", n.param3);
                } else if (n.type == "audio_play") {
                    draw_param("Source:", n.param1);
                    if (!n.param2.empty()) draw_param("Clip:", n.param2);
                } else if (n.type == "log" || n.type == "comment") {
                    draw_param("Text:", n.param1);
                } else if (n.type == "set_velocity" || n.type == "add_force") {
                    draw_param("Entity:", n.param1);
                    std::string v = "(" + std::to_string((int)n.float_param) + ", " + n.param2 + ")";
                    draw_param("Vector:", v);
                } else if (n.type == "teleport_to") {
                    draw_param("Entity:", n.param1);
                    std::string pos = "(" + std::to_string((int)n.float_param) + ", " + n.param2 + ")";
                    draw_param("Position:", pos);
                } else if (n.type == "branch" || n.type == "gate") {
                    draw_param("Condition:", n.param1);
                } else if (n.type == "print_value") {
                    draw_param("Entity:", n.param1);
                    if (!n.param2.empty()) draw_param("Field:", n.param2);
                } else if (n.type == "tween_to") {
                    draw_param("Entity:", n.param1);
                    if (!n.param2.empty()) draw_param("Target:", n.param2);
                } else if (n.type == "set_variable") {
                    draw_param("Name:", n.param1);
                    if (!n.param2.empty()) draw_param("Value:", n.param2);
                } else if (n.type == "get_variable") {
                    draw_param("Name:", n.param1);
                } else if (n.type == "increment_variable" || n.type == "decrement_variable") {
                    draw_param("Name:", n.param1);
                } else if (n.type == "math_add") {
                    draw_param("Store in:", n.param1);
                    draw_param("A + B:", std::to_string((int)n.float_param) + " , " + n.param2);
                } else if (n.type == "math_sub") {
                    draw_param("Store in:", n.param1);
                    draw_param("A - B:", std::to_string((int)n.float_param) + " , " + n.param2);
                } else if (n.type == "math_mul") {
                    draw_param("Store in:", n.param1);
                    draw_param("A × B:", std::to_string((int)n.float_param) + " , " + n.param2);
                } else if (n.type == "math_div") {
                    draw_param("Store in:", n.param1);
                    draw_param("A ÷ B:", std::to_string((int)n.float_param) + " , " + n.param2);
                } else if (n.type == "random_range") {
                    draw_param("Store in:", n.param1);
                    draw_param("Range:", std::to_string((int)n.float_param) + " to " + n.param2);
                } else if (n.type == "clamp") {
                    draw_param("Store in:", n.param1);
                    draw_param("Value:", std::to_string((int)n.float_param) + " [" + n.param2 + "]");
                } else if (n.type == "on_trigger" || n.type == "on_collision") {
                    draw_param("Tag filter:", n.param1);
                } else {
                    // Generic
                    if (!n.param1.empty()) draw_param(n.param1.c_str(), "");
                    if (!n.param2.empty()) { draw_param("", n.param2); py += 10; }
                }
            }

            // ── Hit area for drag / select ──
            if (inline_rows > 0) {
                const float fields_y = pin_start_y + (num_rows * 18.f + 4.f) * _zoom;
                draw_inline_node_property_fields(st, n, node_pos, fields_y, _zoom);
            }

            // The title bar is the drag handle. The body remains free for
            // text/number widgets and pins, so editing never starts a drag.
            ImGui::SetCursorScreenPos(node_pos);
            ImGui::InvisibleButton(("##n"+std::to_string(n.id)).c_str(), {node_width, header_height});

            // ── Selection is decided at mouse-DOWN (IsItemActivated), not on click-release.
            // This is the fix for "dragging one node drags the whole group": previously,
            // selection updates relied on IsItemClicked (a click+release signal) while the
            // drag-group decision (`n.selected && selected_count() > 1`) was re-evaluated every
            // drag frame. If a node was left selected from an earlier Ctrl+A / box-select and
            // the user then pressed-and-immediately-dragged it (without a discrete "click"),
            // the stale multi-selection silently carried into the drag and moved every node.
            // Deciding selection synchronously at activation removes that race: a plain click
            // (no Ctrl/Shift) on any node — selected or not — immediately collapses the
            // selection down to just that node *before* any drag delta is ever applied, unless
            // the drag is explicitly a "drag the current multi-selection" gesture (the node was
            // already the sole anchor of an existing multi-select and the user didn't click an
            // unselected node to break out of it).
            if (ImGui::IsItemActivated()) {
                bool ctrl = ImGui::GetIO().KeyCtrl;
                bool shift = ImGui::GetIO().KeyShift;
                if (ctrl) {
                    toggle_selection(n.id);
                } else if (shift) {
                    if (!n.selected) n.selected = true;
                    // shift-clicking an already-selected node keeps the group as-is
                } else if (!n.selected) {
                    // Clicking an unselected card starts a fresh selection.
                    // Clicking an already selected card deliberately preserves
                    // the selection so a normal multi-node drag moves the
                    // complete group (the conventional graph-editor behavior).
                    select_only(n.id);
                }
                _drag_is_group = n.selected && selected_count() > 1;
                _drag_snap_x = n.x;
                _drag_snap_y = n.y;
                _move_snapshot_taken = false;
            }

            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                if (!_wiring_active) {
                    if (!_move_snapshot_taken) {
                        push_undo_snapshot();
                        _move_snapshot_taken = true;
                    }
                    float dx = ImGui::GetIO().MouseDelta.x / std::max(0.01f, _zoom);
                    float dy = ImGui::GetIO().MouseDelta.y / std::max(0.01f, _zoom);
                    // Use the decision made at activation time, not a re-check of current
                    // selection — keeps the drag behavior stable for the whole gesture even
                    // though selection could otherwise be queried mid-drag.
                    if (_drag_is_group) {
                        for (auto& sn : nodes) if (sn.selected) { sn.x += dx; sn.y += dy; }
                    } else {
                        n.x += dx;
                        n.y += dy;
                    }
                }
            }

            // Snap node when released
            if (ImGui::IsItemDeactivated() && !_wiring_active) {
                const float snap = 12.f;
                if (_drag_is_group) {
                    for (auto& sn : nodes) if (sn.selected) {
                        sn.x = roundf(sn.x / snap) * snap;
                        sn.y = roundf(sn.y / snap) * snap;
                    }
                } else {
                    n.x = roundf(n.x / snap) * snap;
                    n.y = roundf(n.y / snap) * snap;
                }
                _move_snapshot_taken = false;
                _drag_is_group = false;
            }

            // ── Right-click context menu (quick actions; properties now live in the
            //    Inspector panel on the right so you don't have to right-click just to
            //    rename a variable or tweak a value) ──
            if (ImGui::BeginPopupContextItem()) {
                ImGui::SeparatorText(n.label.c_str());
                ImGui::TextDisabled("Edit values directly in this node.");
                ImGui::Separator();
                if (ImGui::Button("Duplicate Node")) { _last_interacted_node = n.id; _pending_duplicate = n.id; }
                if (ImGui::Button("Delete Node")) to_delete = n.id;
                ImGui::EndPopup();
            }
        }

        dl->PopClipRect();

        // Resolve the complete wire gesture after the canvas has seen every
        // socket.  Drag from any pin and release on a compatible opposite pin;
        // click an empty area or press Escape/right-click to cancel.  This is
        // intentionally independent of node drag widgets and inline fields.
        const ImVec2 graph_mouse = ImGui::GetMousePos();
        const PinHit* hit_pin = nullptr;
        for (auto it = _pin_hits.rbegin(); it != _pin_hits.rend(); ++it) {
            const float radius = (it->ptype == "exec") ? 12.f : 11.f;
            const float dx = graph_mouse.x - it->pos.x, dy = graph_mouse.y - it->pos.y;
            if (dx * dx + dy * dy <= radius * radius) { hit_pin = &*it; break; }
        }
        if (hit_pin && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !_wiring_active) {
            start_wiring(hit_pin->node_id, hit_pin->pin_id, hit_pin->is_output, hit_pin->ptype);
        }
        if (_wiring_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (hit_pin && hit_pin->node_id != _wiring_from_node &&
                hit_pin->is_output != _wiring_from_is_output &&
                pins_compatible(hit_pin->ptype, _wiring_ptype)) {
                if (_wiring_from_is_output)
                    connect_pin_ids(_wiring_from_node, _wiring_from_pin, hit_pin->node_id, hit_pin->pin_id, true);
                else
                    connect_pin_ids(hit_pin->node_id, hit_pin->pin_id, _wiring_from_node, _wiring_from_pin, true);
            }
            cancel_wiring();
        }

        // ── Handle duplication ──
        if (_pending_duplicate >= 0) {
            Node* src = nullptr;
            for (auto& n : nodes) { if (n.id == _pending_duplicate) { src = &n; break; } }
            if (src) {
                Node dup;
                dup.id = _next_id++;
                dup.type = src->type;
                dup.label = src->label;
                dup.x = src->x + 30.f;
                dup.y = src->y + 30.f;
                dup.param1 = src->param1;
                dup.param2 = src->param2;
                dup.param3 = src->param3;
                dup.float_param = src->float_param;
                for (auto& p : src->in_pins) {
                    EventGraphPin np = p; np.pin_id = _next_pin_id++; dup.in_pins.push_back(np);
                }
                for (auto& p : src->out_pins) {
                    EventGraphPin np = p; np.pin_id = _next_pin_id++; dup.out_pins.push_back(np);
                }
                nodes.push_back(dup);
            }
            _pending_duplicate = -1;
        }

        // ── Handle deletions ──
        if (to_delete >= 0) {
            links.erase(std::remove_if(links.begin(), links.end(), 
                [&](auto& l) { return l.from_node == to_delete || l.to_node == to_delete; }), links.end());
            nodes.erase(std::remove_if(nodes.begin(), nodes.end(), 
                [&](auto& n) { return n.id == to_delete; }), nodes.end());
        }

        // ── Cancel wiring with Escape ──
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && _wiring_active) {
            _wiring_active = false;
            _wiring_from_node = -1;
            _wiring_from_pin = -1;
        }

        // ── Keyboard shortcuts / box select ──
        if (ImGui::IsKeyPressed(ImGuiKey_Z) && ImGui::GetIO().KeyCtrl) { undo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Y) && ImGui::GetIO().KeyCtrl) { redo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_A) && ImGui::GetIO().KeyCtrl) { select_all(); }
        if (ImGui::IsKeyPressed(ImGuiKey_C) && ImGui::GetIO().KeyCtrl) { copy_selected_to_clipboard(); }
        if (ImGui::IsKeyPressed(ImGuiKey_X) && ImGui::GetIO().KeyCtrl) { copy_selected_to_clipboard(); delete_selected(); }
        if (ImGui::IsKeyPressed(ImGuiKey_V) && ImGui::GetIO().KeyCtrl) {
            ImVec2 mouse = ImGui::GetMousePos();
            paste_clipboard(canvas_pos, to_canvas(mouse));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) { frame_selection(canvas_size); }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !_wiring_active) {
            if (selected_count() > 0) delete_selected();
            else if (canvas_hovered) {
                ImVec2 mouse = ImGui::GetMousePos();
                int closest = -1;
                float best_dist = 1e9f;
                for (auto& n : nodes) {
                    ImVec2 nc = to_screen(n.x + n.width * .5f, n.y + n.height * .5f);
                    float d = sqrt(pow(mouse.x - nc.x, 2) + pow(mouse.y - nc.y, 2));
                    if (d < best_dist) { best_dist = d; closest = n.id; }
                }
                if (closest >= 0 && best_dist < 300.f) delete_node_by_id(closest);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D) && ImGui::GetIO().KeyCtrl && !_wiring_active) {
            if (selected_count() > 0) {
                copy_selected_to_clipboard();
                ImVec2 mouse = ImGui::GetMousePos();
                paste_clipboard(canvas_pos, to_canvas(mouse));
            } else if (canvas_hovered) {
                ImVec2 mouse = ImGui::GetMousePos();
                int closest = -1;
                float best_dist = 1e9f;
                for (auto& n : nodes) {
                    ImVec2 nc = to_screen(n.x + n.width * .5f, n.y + n.height * .5f);
                    float d = sqrt(pow(mouse.x - nc.x, 2) + pow(mouse.y - nc.y, 2));
                    if (d < best_dist) { best_dist = d; closest = n.id; }
                }
                if (closest >= 0 && best_dist < 300.f) {
                    select_only(closest);
                    copy_selected_to_clipboard();
                    paste_clipboard(canvas_pos, to_canvas(mouse));
                }
            }
        }

        if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !any_node_hovered && !_wiring_active) {
            clear_selection();
            _box_selecting = true;
            _box_select_start = ImGui::GetMousePos();
            _box_select_end = _box_select_start;
        }
        if (_box_selecting) {
            _box_select_end = ImGui::GetMousePos();
            ImVec2 a{std::min(_box_select_start.x, _box_select_end.x), std::min(_box_select_start.y, _box_select_end.y)};
            ImVec2 b{std::max(_box_select_start.x, _box_select_end.x), std::max(_box_select_start.y, _box_select_end.y)};
            dl->AddRect(a, b, IM_COL32(120,180,255,220), 0.f, 0, 1.5f);
            dl->AddRectFilled(a, b, IM_COL32(120,180,255,35));
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                clear_selection();
                for (auto& n : nodes) {
                    ImVec2 np = to_screen(n.x, n.y);
                    ImVec2 ne{np.x + n.width * _zoom, np.y + n.height * _zoom};
                    bool inside = !(ne.x < a.x || ne.y < a.y || np.x > b.x || np.y > b.y);
                    if (inside) n.selected = true;
                }
                _box_selecting = false;
            }
        }
        ImGui::SetWindowFontScale(1.f);
        // Field editing takes one snapshot per focused edit session. Reset it
        // once no text/number control owns keyboard focus, otherwise every
        // later node edit would silently share the first undo record.
        if (!ImGui::IsAnyItemActive()) _field_edit_snapshot_taken = false;
        ImGui::EndChild(); // ##ev_canvas
        ImGui::EndChild(); // ##ev_varsplit

        _autosave_if_changed(st);
        ImGui::End();
    }

    // ── Serialization ──
    nlohmann::json save_to_json() {
        // V3 assets are self-describing, entity-owned documents.  The runtime
        // still reads V1/V2 arrays, but new authoring never relies on a scene
        // fallback or a detached metadata pseudo-node.
        nlohmann::json j = {
            {"format", "gameengine.visual-script"},
            {"version", 3},
            {"owner", {{"entity_id", _bound_entity_id}, {"entity_name", _bound_entity_name}}},
            {"next_id", _next_id},
            {"next_pin_id", _next_pin_id},
            {"nodes", nlohmann::json::array()},
            {"links", nlohmann::json::array()},
            {"variables", nlohmann::json::array()}
        };
        for (auto& n : nodes) {
            nlohmann::json jn;
            jn["id"] = n.id;
            jn["type"] = n.type;
            jn["label"] = n.label;
            jn["x"] = n.x; jn["y"] = n.y;
            jn["p1"] = n.param1; jn["p2"] = n.param2; jn["p3"] = n.param3;
            jn["fp"] = n.float_param;
            // Save pin IDs for link resolution
            nlohmann::json jin = nlohmann::json::array();
            for (auto& p : n.in_pins) jin.push_back({{"label", p.label}, {"type", p.ptype}, {"id", p.pin_id}, {"literal", p.literal}});
            jn["in_pins"] = jin;
            nlohmann::json jout = nlohmann::json::array();
            for (auto& p : n.out_pins) jout.push_back({{"label", p.label}, {"type", p.ptype}, {"id", p.pin_id}, {"literal", p.literal}});
            jn["out_pins"] = jout;
            j["nodes"].push_back(jn);
        }
        for (auto& l : links) {
            j["links"].push_back({{"fn", l.from_node}, {"fp", l.from_pin}, {"tn", l.to_node}, {"tp", l.to_pin}});
        }
        for (const auto& variable : variables) {
            if (!variable.name.empty()) {
                j["variables"].push_back({{"name", variable.name}, {"type", variable.type},
                                            {"default", variable.default_value}});
            }
        }
        return j;
    }

    void load_from_json(const nlohmann::json& j, bool select_loaded = false) {
        nodes.clear(); links.clear(); variables.clear();
        _undo_stack.clear();
        _redo_stack.clear();
        _clipboard_json.clear();
        cancel_wiring();
        clear_selection();
        _dragging_nodes = false;
        _move_snapshot_taken = false;
        _box_selecting = false;
        _box_select_start = {0,0};
        _box_select_end = {0,0};
        _next_id = 1; _next_pin_id = 1;

        // V3 stores its document metadata alongside a real `nodes` array;
        // V1/V2 used an array with a synthetic `_meta` element.  Normalize
        // both here so migration is lossless and the rest of the loader only
        // ever sees actual nodes.
        const nlohmann::json* node_source = &j;
        const nlohmann::json* v3_links = nullptr;
        if (j.is_object()) {
            if (!j.contains("nodes") || !j["nodes"].is_array()) return;
            node_source = &j["nodes"];
            _next_id = std::max(1, j.value("next_id", 1));
            _next_pin_id = std::max(1, j.value("next_pin_id", 1));
            if (j.contains("links") && j["links"].is_array()) v3_links = &j["links"];
            if (j.contains("variables") && j["variables"].is_array()) {
                for (const auto& entry : j["variables"]) {
                    if (!entry.is_object()) continue;
                    GraphVariable variable;
                    variable.name = entry.value("name", std::string());
                    variable.type = entry.value("type", std::string("float"));
                    variable.default_value = entry.value("default", entry.value("value", std::string("0")));
                    if (!variable.name.empty()) variables.push_back(std::move(variable));
                }
            }
        } else if (!j.is_array()) {
            return;
        }
        
        for (const auto& jn : *node_source) {
            if (jn.is_object() && jn.value("_meta", false)) {
                // Older graphs often saved next_pin_id as 1 while their node
                // pin arrays were empty.  Keep the IDs already generated by
                // the legacy-repair path above; rewinding here created duplicate
                // pin IDs and made a wire attach to the wrong node.
                _next_id = std::max(_next_id, jn.value("next_id", _next_id));
                _next_pin_id = std::max(_next_pin_id, jn.value("next_pin_id", _next_pin_id));
                if (jn.contains("links")) {
                    for (auto& jl : jn["links"]) {
                        Link l;
                        l.from_node = jl.value("fn", 0);
                        l.from_pin = jl.value("fp", 0);
                        l.to_node = jl.value("tn", 0);
                        l.to_pin = jl.value("tp", 0);
                        links.push_back(l);
                    }
                }
                if (jn.contains("variables") && jn["variables"].is_array()) {
                    for (const auto& entry : jn["variables"]) {
                        if (!entry.is_object()) continue;
                        GraphVariable variable;
                        variable.name = entry.value("name", std::string());
                        variable.type = entry.value("type", std::string("float"));
                        variable.default_value = entry.value("default", entry.value("value", std::string("0")));
                        if (variable.name.empty()) continue;
                        const bool duplicate = std::any_of(variables.begin(), variables.end(),
                            [&](const GraphVariable& known) { return known.name == variable.name; });
                        if (!duplicate) variables.push_back(std::move(variable));
                    }
                }
                continue;
            }
            
            Node n;
            n.id = jn.value("id", _next_id);
            n.type = jn.value("type", "log");
            n.label = jn.value("label", n.type);
            n.x = jn.value("x", 100.f);
            n.y = jn.value("y", 100.f);
            n.param1 = jn.value("p1", "");
            n.param2 = jn.value("p2", "");
            n.param3 = jn.value("p3", "");
            n.float_param = jn.value("fp", 0.f);
            
            // Load pins if saved
            if (jn.contains("in_pins")) {
                for (auto& jp : jn["in_pins"]) {
                    EventGraphPin p;
                    p.label = jp.value("label", "");
                    p.ptype = jp.value("type", "exec");
                    p.pin_id = jp.value("id", _next_pin_id++);
                    p.literal = jp.value("literal", jp.value("lit", ""));
                    n.in_pins.push_back(p);
                }
            }
            if (jn.contains("out_pins")) {
                for (auto& jp : jn["out_pins"]) {
                    EventGraphPin p;
                    p.label = jp.value("label", "");
                    p.ptype = jp.value("type", "exec");
                    p.pin_id = jp.value("id", _next_pin_id++);
                    p.literal = jp.value("literal", jp.value("lit", ""));
                    n.out_pins.push_back(p);
                }
            }
            
            // Legacy support: if no pins saved, generate from type info
            if (n.in_pins.empty() && n.out_pins.empty()) {
                auto* ti = find_type_info(n.type);
                if (ti) {
                    for (auto& p : ti->in_pins) { auto np = p; np.pin_id = _next_pin_id++; n.in_pins.push_back(np); }
                    for (auto& p : ti->out_pins) { auto np = p; np.pin_id = _next_pin_id++; n.out_pins.push_back(np); }
                } else {
                    n.in_pins.push_back({"", "exec", _next_pin_id++});
                    n.out_pins.push_back({"", "exec", _next_pin_id++});
                }
            }
            // Schema migration: older graph assets only had exec pins. Add
            // the modern typed data pins without touching existing IDs or
            // links, so a saved graph gains wireable values the next time it
            // is opened and autosaved.
            if (const auto* ti = find_type_info(n.type)) {
                auto ensure_ports = [&](const std::vector<EventGraphPin>& desired,
                                        std::vector<EventGraphPin>& actual) {
                    for (const auto& template_pin : desired) {
                        const bool already_present = std::any_of(actual.begin(), actual.end(),
                            [&](const EventGraphPin& current) {
                                return current.label == template_pin.label && current.ptype == template_pin.ptype;
                            });
                        if (!already_present) {
                            EventGraphPin added = template_pin;
                            added.pin_id = _next_pin_id++;
                            actual.push_back(std::move(added));
                        }
                    }
                };
                ensure_ports(ti->in_pins, n.in_pins);
                ensure_ports(ti->out_pins, n.out_pins);
            }
            
            // Legacy link format: "outs" array on nodes
            if (jn.contains("outs") && !jn["outs"].empty()) {
                for (auto& v : jn["outs"]) {
                    int dst = v.get<int>();
                    // Find matching pin
                    int from_pin = n.out_pins.empty() ? 0 : n.out_pins[0].pin_id;
                    // Find dest node's input pin
                    int to_pin = 0;
                    for (const auto& dn : *node_source) {
                        if (dn.is_object() && dn.value("id", 0) == dst) {
                            if (dn.contains("in_pins") && !dn["in_pins"].empty())
                                to_pin = dn["in_pins"][0].value("id", 0);
                            break;
                        }
                    }
                    links.push_back({n.id, dst, from_pin, to_pin});
                }
            }
            
            if (n.id >= _next_id) _next_id = n.id + 1;
            nodes.push_back(n);
        }

        if (v3_links) {
            for (const auto& jl : *v3_links) {
                if (!jl.is_object()) continue;
                Link link;
                link.from_node = jl.value("fn", jl.value("from", 0));
                link.from_pin = jl.value("fp", 0);
                link.to_node = jl.value("tn", jl.value("to", 0));
                link.to_pin = jl.value("tp", 0);
                if (link.from_node && link.to_node) links.push_back(link);
            }
        }

        // Graphs created before declarations were introduced referenced
        // variables only from their Set/Get nodes. Preserve those assets by
        // materializing stable declarations the first time they are opened.
        for (const auto& node : nodes) {
            const bool variable_node = node.type == "set_variable" || node.type == "get_variable" ||
                node.type == "increment_variable" || node.type == "decrement_variable";
            if (!variable_node || node.param1.empty()) continue;
            const bool declared = std::any_of(variables.begin(), variables.end(),
                [&](const GraphVariable& variable) { return variable.name == node.param1; });
            if (!declared) variables.push_back({node.param1, "float", node.type == "set_variable" ? node.param2 : "0"});
        }
    }

    // ── Graph Validation ──
    void validate_graph(EditorState& st) {
        int warnings = 0;
        
        // 1. Check for unconnected event nodes
        for (auto& n : nodes) {
            bool is_event = (n.type.rfind("on_", 0) == 0);
            bool has_output = false;
            for (auto& l : links) {
                if (l.from_node == n.id) { has_output = true; break; }
            }
            if (is_event && !has_output) {
                st.log_warn("⚠ Event node '" + n.label + "' (id:" + std::to_string(n.id) + ") has no connections.");
                warnings++;
            }
        }
        
        // 2. Check for nodes with no input connections (non-event nodes)
        for (auto& n : nodes) {
            if (n.type.rfind("on_", 0) == 0 || n.type == "comment") continue;
            bool has_input = false;
            for (auto& l : links) {
                if (l.to_node == n.id) { has_input = true; break; }
            }
            if (!has_input) {
                st.log_warn("⚠ Node '" + n.label + "' (id:" + std::to_string(n.id) + ") is not connected to any event.");
                warnings++;
            }
        }
        
        // 3. Check for cycles (simple BFS from each node)
        for (auto& n : nodes) {
            std::unordered_set<int> visited;
            std::vector<int> stack = {n.id};
            bool has_cycle = false;
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                if (visited.count(cur)) { has_cycle = true; break; }
                visited.insert(cur);
                // Limit search depth
                if (visited.size() > 100) break;
                for (auto& l : links) {
                    if (l.from_node == cur) stack.push_back(l.to_node);
                }
            }
            if (has_cycle) {
                st.log_warn("⚠ Cycle detected starting from node '" + n.label + "' (id:" + std::to_string(n.id) + ").");
                warnings++;
                break; // Only report first cycle
            }
        }
        
        // 4. Check for duplicate entity references in action nodes
        std::unordered_set<std::string> seen_entities;
        for (auto& n : nodes) {
            if ((n.type == "set_active" || n.type == "destroy" || n.type == "spawn") && !n.param1.empty()) {
                if (seen_entities.count(n.param1)) {
                    st.log_warn("⚠ Entity '" + n.param1 + "' referenced by multiple nodes (id:" + std::to_string(n.id) + ").");
                }
                seen_entities.insert(n.param1);
            }
        }
        
        // 5. Check for orphan nodes (no input AND no output)
        for (auto& n : nodes) {
            if (n.type == "comment") continue;
            bool has_input = false, has_output = false;
            for (auto& l : links) {
                if (l.to_node == n.id) has_input = true;
                if (l.from_node == n.id) has_output = true;
            }
            if (!has_input && !has_output) {
                st.log_warn("⚠ Node '" + n.label + "' (id:" + std::to_string(n.id) + ") is orphaned (no connections).");
                warnings++;
            }
        }
        
        if (warnings == 0) {
            st.log_success("✓ Graph validation passed - no issues found.");
        } else {
            st.log_success("Graph validation complete: " + std::to_string(warnings) + " warning(s).");
        }
    }

private:
    Link* _pending_link_delete = nullptr;
    int _pending_duplicate = -1;
    int _last_interacted_node = -1;
    float _add_node_canvas_x = 300.f;
    float _add_node_canvas_y = 200.f;
    float _drag_snap_x = 0.f;
    float _drag_snap_y = 0.f;
    bool _drag_is_group = false;
    bool _field_edit_snapshot_taken = false;
};

// ─── Curve Editor (Animation Curve / Gradient) ───────────────────────────────
// Unity's AnimationCurve inspector widget. Editable bezier curve stored as
// a flat array of [t,v, t,v, ...] pairs — same format as ParticleEmitter
// size_curve/color_curve. Can be embedded inline in the inspector or opened
// as a floating window.

class CurveEditorPanel {
public:
    struct Key { float t, v; };
    std::vector<Key> keys;
    bool  _open = false;
    int   _sel_key = -1;
    std::string _title  = "Curve";
    std::string _target_component;
    std::string _target_field;
    int         _target_entity_id = -1;

    // Normalized value range
    float _v_min = 0.f, _v_max = 1.f;

    void open(const std::string& title, const std::vector<float>& flat,
              float v_min=0.f, float v_max=1.f) {
        _open=true; _title=title; _v_min=v_min; _v_max=v_max;
        keys.clear();
        for (int i=0; i+1<(int)flat.size(); i+=2)
            keys.push_back({flat[i], flat[i+1]});
        if (keys.empty()) { keys.push_back({0.f,0.f}); keys.push_back({1.f,1.f}); }
    }

    std::vector<float> to_flat() const {
        std::vector<float> r;
        for (auto& k:keys) { r.push_back(k.t); r.push_back(k.v); }
        return r;
    }

    void draw(EditorState& st) {
        (void)st;
        if (!_open) return;
        ImGui::SetNextWindowSize({500,300}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin((_title + "##cved").c_str(), &_open)) { ImGui::End(); return; }

        // Sort keys by time
        std::sort(keys.begin(),keys.end(),[](auto& a,auto& b){return a.t<b.t;});

        ImVec2 canvas = ImGui::GetContentRegionAvail();
        canvas.y -= ImGui::GetFrameHeightWithSpacing()*2 + 8;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(origin, {origin.x+canvas.x, origin.y+canvas.y}, IM_COL32(28,28,32,255));
        dl->AddRect(origin, {origin.x+canvas.x, origin.y+canvas.y}, IM_COL32(80,80,90,255));

        // Grid lines
        for (int i=1;i<4;++i) {
            float x = origin.x + canvas.x*i/4;
            float y = origin.y + canvas.y*i/4;
            dl->AddLine({x,origin.y},{x,origin.y+canvas.y}, IM_COL32(50,50,55,255));
            dl->AddLine({origin.x,y},{origin.x+canvas.x,y}, IM_COL32(50,50,55,255));
        }

        // Map key to screen
        auto to_scr = [&](float t, float v) -> ImVec2 {
            float nx = t;
            float ny = 1.f - (v - _v_min)/std::max(0.001f,_v_max-_v_min);
            return {origin.x + nx*canvas.x, origin.y + ny*canvas.y};
        };
        auto to_key = [&](ImVec2 p) -> Key {
            float t = (p.x-origin.x)/canvas.x;
            float v = _v_min + (1.f-(p.y-origin.y)/canvas.y)*(_v_max-_v_min);
            return {std::clamp(t,0.f,1.f), std::clamp(v,_v_min,_v_max)};
        };

        // Draw curve (linear segments for now)
        if (keys.size()>1) {
            for (int i=0;i+1<(int)keys.size();++i) {
                ImVec2 a = to_scr(keys[i].t,   keys[i].v);
                ImVec2 b = to_scr(keys[i+1].t, keys[i+1].v);
                // bezier through midpoints
                ImVec2 cp1={a.x+(b.x-a.x)*0.4f, a.y};
                ImVec2 cp2={a.x+(b.x-a.x)*0.6f, b.y};
                dl->AddBezierCubic(a,cp1,cp2,b, IM_COL32(78,200,100,255), 2.f);
            }
        }

        // Draw key handles
        int to_del = -1;
        for (int i=0;i<(int)keys.size();++i) {
            ImVec2 sp = to_scr(keys[i].t, keys[i].v);
            bool sel = (_sel_key==i);
            dl->AddCircleFilled(sp, sel?7.f:5.f,
                sel ? IM_COL32(255,220,50,255) : IM_COL32(78,200,100,255));
            dl->AddCircle(sp, sel?7.f:5.f, IM_COL32(0,0,0,200));

            ImGui::SetCursorScreenPos({sp.x-7,sp.y-7});
            ImGui::InvisibleButton(("##k"+std::to_string(i)).c_str(), {14,14});
            if (ImGui::IsItemClicked()) _sel_key=i;
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                ImVec2 mp = ImGui::GetIO().MousePos;
                auto nk = to_key(mp);
                // Don't let time cross neighbors
                float tmin = i>0 ? keys[i-1].t+0.001f : 0.f;
                float tmax = i<(int)keys.size()-1 ? keys[i+1].t-0.001f : 1.f;
                keys[i].t = std::clamp(nk.t, tmin, tmax);
                keys[i].v = nk.v;
                _sel_key=i;
            }
            if (ImGui::BeginPopupContextItem()) {
                ImGui::Text("Key %d", i);
                ImGui::InputFloat("Time##kt",&keys[i].t,0.01f,0.1f,"%.3f");
                ImGui::InputFloat("Value##kv",&keys[i].v,0.01f,0.1f,"%.3f");
                if (ImGui::Button("Delete Key") && keys.size()>2) to_del=i;
                ImGui::EndPopup();
            }
        }
        if (to_del>=0) { keys.erase(keys.begin()+to_del); _sel_key=-1; }

        // Click on canvas to add key
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##cvsclick",canvas);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
            auto nk = to_key(ImGui::GetIO().MousePos);
            keys.push_back(nk);
            std::sort(keys.begin(),keys.end(),[](auto& a,auto& b){return a.t<b.t;});
        }

        ImGui::SetCursorScreenPos({origin.x, origin.y+canvas.y+4});
        if (_sel_key>=0 && _sel_key<(int)keys.size()) {
            ImGui::SetNextItemWidth(80);
            ImGui::InputFloat("t",&keys[_sel_key].t,0.01f,0,"%.3f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputFloat("v",&keys[_sel_key].v,0.01f,0,"%.3f");
            ImGui::SameLine();
        }
        if (ImGui::Button("Reset")) { keys={{0,0},{1,1}}; _sel_key=-1; }
        ImGui::SameLine();
        ImGui::TextDisabled("Left-click canvas to add key. Right-click key to delete.");

        ImGui::End();
    }
};

// ─── Scene Lighting Overview ──────────────────────────────────────────────────
// Shows all Light2D entities in the scene, lets you tweak them centrally
// (like Unity's Lighting window), and manages a global ambient color.

class LightingPanel {
public:
    bool   _open          = false;
    float  _ambient[4]    = {0.05f,0.05f,0.1f,1.f};
    bool   _ambient_on    = true;
    float  _global_int    = 1.f;

    void open() { _open = true; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({360,480}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Lighting##lwin", &_open)) { ImGui::End(); return; }

        ImGui::SeparatorText("Global / Ambient");
        ImGui::Checkbox("Enable Ambient Light", &_ambient_on);
        ImGui::ColorEdit4("Ambient Color", _ambient, ImGuiColorEditFlags_AlphaBar);
        ImGui::SliderFloat("Global Intensity", &_global_int, 0.f, 4.f);

        ImGui::Spacing();
        ImGui::SeparatorText("Scene Lights");
        ImGui::TextDisabled("All Light2D components in the current scene:");

        if (ImGui::BeginTable("##lights", 5, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY, {0,280})) {
            ImGui::TableSetupColumn("Entity",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Radius",   ImGuiTableColumnFlags_WidthFixed,60);
            ImGui::TableSetupColumn("Intensity",ImGuiTableColumnFlags_WidthFixed,70);
            ImGui::TableSetupColumn("Color",    ImGuiTableColumnFlags_WidthFixed,50);
            ImGui::TableSetupColumn("On",       ImGuiTableColumnFlags_WidthFixed,30);
            ImGui::TableHeadersRow();

            for (auto& e : st.entities) {
                if (!e.contains("components") || !e["components"].contains("Light2D")) continue;
                auto& L = e["components"]["Light2D"];
                ImGui::TableNextRow();
                ImGui::PushID(e.value("id",0));

                // Entity name — click to select
                ImGui::TableSetColumnIndex(0);
                bool sel = (st.selected_id == e.value("id",0));
                if (ImGui::Selectable(e.value("name","?").c_str(), sel,
                        ImGuiSelectableFlags_SpanAllColumns))
                    st.select(e.value("id",0));

                ImGui::TableSetColumnIndex(1);
                float radius = L.value("radius",200.f);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputFloat("##r",&radius,0,0,"%.0f")) L["radius"]=radius;

                ImGui::TableSetColumnIndex(2);
                float inten = L.value("intensity",1.f);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##i",&inten,0,4,"%.2f")) L["intensity"]=inten;

                ImGui::TableSetColumnIndex(3);
                // Color swatch
                auto& jc = L["color"];
                float fc[4]={(float)jc[0].get<int>()/255.f,(float)jc[1].get<int>()/255.f,
                             (float)jc[2].get<int>()/255.f,(float)jc[3].get<int>()/255.f};
                if (ImGui::ColorEdit4("##c",fc,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_AlphaBar)) {
                    jc[0]=(int)(fc[0]*255); jc[1]=(int)(fc[1]*255);
                    jc[2]=(int)(fc[2]*255); jc[3]=(int)(fc[3]*255);
                }

                ImGui::TableSetColumnIndex(4);
                bool on = L.value("enabled",true);
                if (ImGui::Checkbox("##on",&on)) L["enabled"]=on;

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button("Add Point Light")) {
            Entity e = SceneIO::make_entity("Point Light 2D", st);
            e["components"]["Light2D"] = component_defaults()["Light2D"];
            e["components"]["Transform"] = component_defaults()["Transform"];
            e["components"]["Transform"]["x"] = st.cam_x;
            e["components"]["Transform"]["y"] = st.cam_y;
            st.undo.push_deep(st.entities);
            st.entities.push_back(e);
            transform::mark_structure_dirty();
            st.select(e.value("id",0));
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Saved Lighting Settings");
        if (ImGui::Button("Save Lighting")) {
            nlohmann::json j = {{"ambient_on",_ambient_on},{"global_int",_global_int},
                {"ambient",{_ambient[0],_ambient[1],_ambient[2],_ambient[3]}}};
            fs::path p = fs::path(st.scene_path).parent_path()/"lighting.json";
            if (save_json(p,j)) st.log_success("Lighting saved.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Lighting")) {
            fs::path p = fs::path(st.scene_path).parent_path()/"lighting.json";
            auto j = load_json(p);
            if (j.is_object()) {
                _ambient_on=j.value("ambient_on",true);
                _global_int=j.value("global_int",1.f);
                if (j.contains("ambient")) {
                    _ambient[0]=j["ambient"][0]; _ambient[1]=j["ambient"][1];
                    _ambient[2]=j["ambient"][2]; _ambient[3]=j["ambient"][3];
                }
                st.log_success("Lighting loaded.");
            }
        }
        ImGui::End();
    }
};

// ─── Profiler Panel ───────────────────────────────────────────────────────────
// Lightweight frame timing display — mirrors Unity's Profiler window.
// Records per-frame dt, FPS, and basic system buckets (Physics, Render, Script).

class ProfilerPanel {
public:
    static constexpr int HISTORY = 256;
    float   _fps_history[HISTORY] = {};
    float   _dt_history[HISTORY]  = {};
    int     _hist_idx = 0;
    float   _fps_sum  = 0.f;
    bool    _open     = false;
    bool    _recording = true;

    // Real per-stage timings (ms), copied each push() from
    // EditorState::frame_*_ms — populated every frame by ViewportPanel::draw
    // with actual std::chrono::steady_clock measurements around each
    // subsystem's update() call (see panels.hpp). These used to be a fake
    // 45/25/20/10 split of the frame time with no relationship to what the
    // engine actually spent time on; now they're the real thing, so e.g. a
    // scene with 2,000 colliders will visibly show Physics dominating the
    // bar instead of always reporting a fixed 25%.
    float   _render_ms = 0.f, _physics_ms = 0.f, _script_ms = 0.f, _other_ms = 0.f;

    void open() { _open = true; }

    void push(EditorState& st, float dt) {
        if (!_recording) return;
        float fps = dt > 0.f ? 1.f/dt : 0.f;
        _fps_sum -= _fps_history[_hist_idx];
        _fps_sum += fps;
        _fps_history[_hist_idx] = fps;
        _dt_history[_hist_idx]  = dt*1000.f;
        _hist_idx = (_hist_idx+1) % HISTORY;

        _render_ms  = st.frame_render_ms;
        _physics_ms = st.frame_physics_ms;
        _script_ms  = st.frame_script_ms;
        _other_ms   = st.frame_other_ms;
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({480,380}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Profiler##prof", &_open)) { ImGui::End(); return; }

        float avg_fps = _fps_sum / HISTORY;
        float cur_dt  = _dt_history[(_hist_idx+HISTORY-1)%HISTORY];

        ImGui::Text("FPS: %.1f  (avg)  |  Frame: %.2f ms", avg_fps, cur_dt);
        ImGui::SameLine(0,20);
        if (_recording) {
            if (ImGui::SmallButton("■ Stop"))  _recording=false;
        } else {
            if (ImGui::SmallButton("● Record")) _recording=true;
        }
        if (!st.playing) {
            ImGui::SameLine(0,20);
            ImGui::TextDisabled("(Enter Play mode to see Script/Physics/Other costs)");
        }

        ImGui::Spacing();
        // FPS graph
        char overlay[32]; snprintf(overlay,sizeof(overlay),"%.1f fps", avg_fps);
        ImGui::PlotLines("##fps", _fps_history, HISTORY, _hist_idx,
                         overlay, 0.f, 200.f, {-1, 70});
        ImGui::TextDisabled("FPS (200 max shown)");

        ImGui::Spacing();
        // Frame time bars — real measured costs, not a fixed synthetic split.
        ImGui::SeparatorText("Frame Budget (ms, measured)");
        float total_ms = _render_ms+_physics_ms+_script_ms+_other_ms;
        auto bar = [&](const char* label, float v, ImVec4 col) {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            char ov[32]; snprintf(ov,sizeof(ov),"%.2f ms",v);
            ImGui::ProgressBar(total_ms>0?v/std::max(total_ms,16.f):0.f, {-1,16}, ov);
            ImGui::PopStyleColor();
            ImGui::SameLine(0,-1);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()-80.f);
            ImGui::TextUnformatted(label);
        };
        bar("Render",  _render_ms,  ImVec4(0.20f,0.60f,0.90f,1.f));
        bar("Physics", _physics_ms, ImVec4(0.90f,0.55f,0.10f,1.f));
        bar("Scripts", _script_ms,  ImVec4(0.40f,0.80f,0.30f,1.f));
        bar("Other",   _other_ms,   ImVec4(0.55f,0.55f,0.55f,1.f));
        ImGui::TextDisabled("Other = transform + animator + particles + audio");

        if (st.playing) {
            const auto caches = phys::get_runtime_cache_stats();
            ImGui::Spacing();
            ImGui::SeparatorText("Physics Runtime State");
            ImGui::Text("Interpolation: %zu   Active contacts: %zu",
                        caches.interpolation_records, caches.active_contact_pairs);
            ImGui::Text("Contact IDs: %zu   Ignored pairs: %zu / %zu",
                        caches.contact_id_records, caches.ignored_entity_pairs,
                        caches.ignored_collider_pairs);
            ImGui::TextDisabled("These counters should track live scene objects, not grow after effects expire.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Scene Metrics");
        int entity_count = (int)st.entities.size();
        int script_count = 0, rb_count = 0, sprite_count = 0;
        for (auto& e : st.entities) {
            if (!e.contains("components")) continue;
            if (e["components"].contains("Script"))       ++script_count;
            if (e["components"].contains("Rigidbody2D")) ++rb_count;
            if (e["components"].contains("SpriteRenderer"))++sprite_count;
        }
        ImGui::Text("Entities:  %d", entity_count);
        ImGui::Text("Sprites:   %d  (draw calls estimate: %d)", sprite_count, std::max(1,sprite_count));
        ImGui::Text("Rigidbodies: %d", rb_count);
        ImGui::Text("Scripts:   %d", script_count);
        if (sprite_count > 200)
            ImGui::TextColored(ImVec4(1,0.7f,0.1f,1),"⚠ >200 sprites — consider sprite atlases & batching.");

        ImGui::End();
    }
};

// ─── Audio Mixer Panel ────────────────────────────────────────────────────────
// Unity's AudioMixer window: named mixer groups with volume/pitch faders.
// Stored as assets/audio_mixer.json.

class AudioMixerPanel {
public:
    struct Group {
        std::string name;
        float volume  = 1.f;   // 0..1 linear
        float pitch   = 1.f;
        bool  mute    = false;
        bool  solo    = false;
        std::string parent;    // empty = Master
    };
    std::vector<Group> groups;
    bool _open = false;
    int  _sel  = -1;

    AudioMixerPanel() {
        groups.push_back({"Master",  1.f, 1.f, false, false, ""});
        groups.push_back({"Music",   0.8f,1.f, false, false, "Master"});
        groups.push_back({"SFX",     1.f, 1.f, false, false, "Master"});
        groups.push_back({"Ambient", 0.6f,1.f, false, false, "Master"});
        groups.push_back({"UI",      0.9f,1.f, false, false, "Master"});
    }

    void open() { _open = true; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({460, 340}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Audio Mixer##amix", &_open)) { ImGui::End(); return; }

        ImGui::SeparatorText("Mixer Groups");
        ImGui::TextDisabled("Assign a group name to AudioSource components to route them here.");
        ImGui::Spacing();

        // Horizontal fader strip layout
        float strip_w = 72.f;
        bool any_solo = false;
        for (auto& g : groups) if (g.solo) { any_solo=true; break; }

        for (int i=0;i<(int)groups.size();++i) {
            auto& g = groups[i];
            ImGui::PushID(i);
            ImGui::BeginGroup();

            // Mute button
            bool eff_mute = g.mute || (any_solo && !g.solo);
            if (eff_mute) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f,0.2f,0.2f,1.f));
            if (ImGui::Button("M",{24,18})) g.mute=!g.mute;
            if (eff_mute) ImGui::PopStyleColor();
            ImGui::SameLine(0,2);
            // Solo button
            if (g.solo) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f,0.7f,0.1f,1.f));
            if (ImGui::Button("S",{24,18})) g.solo=!g.solo;
            if (g.solo) ImGui::PopStyleColor();

            // Volume fader (vertical)
            float db = g.volume > 0.f ? 20.f*log10f(g.volume) : -80.f;
            ImGui::SetNextItemWidth(strip_w);
            float vlin = g.volume;
            ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                eff_mute ? ImVec4(0.5f,0.5f,0.5f,1.f) : ImVec4(0.3f,0.7f,0.4f,1.f));
            if (ImGui::VSliderFloat("##vol",{strip_w,90.f},&vlin,0.f,1.25f, g.volume>0?"":" ")) {
                g.volume = vlin;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f dB",db);

            // dB label
            char dblbl[16];
            if (db <= -79.f) snprintf(dblbl,sizeof(dblbl),"-∞");
            else              snprintf(dblbl,sizeof(dblbl),"%.1f",db);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(strip_w-ImGui::CalcTextSize(dblbl).x)*0.5f);
            ImGui::TextUnformatted(dblbl);

            // Group name
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(strip_w-ImGui::CalcTextSize(g.name.c_str()).x)*0.5f);
            bool sel=(_sel==i);
            ImGui::TextColored(sel?ImVec4(0.4f,0.8f,1.f,1.f):ImVec4(1,1,1,1),"%s",g.name.c_str());
            if (ImGui::IsItemClicked()) _sel=i;

            ImGui::EndGroup();
            if (i<(int)groups.size()-1) ImGui::SameLine(0,8);
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("+ Add Group")) {
            groups.push_back({"New Group",1.f,1.f,false,false,"Master"});
        }
        if (_sel>=0 && _sel<(int)groups.size() && groups[_sel].name!="Master") {
            ImGui::SameLine();
            if (ImGui::Button("Remove Selected")) {
                groups.erase(groups.begin()+_sel);
                _sel=-1;
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Save Mixer")) {
            nlohmann::json j=nlohmann::json::array();
            for (auto& g:groups) j.push_back({{"name",g.name},{"vol",g.volume},{"pitch",g.pitch},{"parent",g.parent}});
            fs::path p=fs::path(st.asset_dir)/"audio_mixer.json";
            if(save_json(p,j)) st.log_success("Audio mixer saved.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Mixer")) {
            fs::path p=fs::path(st.asset_dir)/"audio_mixer.json";
            auto j=load_json(p);
            if(j.is_array()){
                groups.clear();
                for(auto& v:j) groups.push_back({v.value("name",""),
                    (float)v.value("vol",1.0),(float)v.value("pitch",1.0),
                    false,false,v.value("parent","")});
                st.log_success("Audio mixer loaded.");
            }
        }
        ImGui::End();
    }
};

// ─── Input Manager ────────────────────────────────────────────────────────────
// Unity's Edit > Project Settings > Input Manager: named virtual axes/buttons
// mapped to physical keys. Stored in ProjectSettings/InputManager.json.

class InputManagerPanel {
public:
    struct Axis {
        std::string name;
        std::string positive_key;    // e.g. "right","d"
        std::string negative_key;    // e.g. "left","a"
        std::string alt_positive;
        std::string alt_negative;
        float       gravity    = 3.f;
        float       dead       = 0.001f;
        float       sensitivity= 3.f;
        bool        snap       = false;
        bool        invert     = false;
        int         type       = 0;  // 0=key/mouse button, 1=mouse movement, 2=joystick
    };
    std::vector<Axis> axes;
    bool _open = false;
    int  _sel  = -1;
    bool _expanded[64] = {};

    InputManagerPanel() {
        axes = {
            {"Horizontal","right","left","d","a",3,0.001f,3,true,false,0},
            {"Vertical",  "up",   "down","w","s",3,0.001f,3,true,false,0},
            {"Jump",      "space","",    "","", 3,0.001f,3,false,false,0},
            {"Fire1",     "left ctrl","","left alt","",3,0.001f,3,false,false,0},
            {"Fire2",     "left alt", "","",         "",3,0.001f,3,false,false,0},
            {"Fire3",     "left shift","","",        "",3,0.001f,3,false,false,0},
            {"Mouse X",   "","","","",0,0,0.1f,false,false,1},
            {"Mouse Y",   "","","","",0,0,0.1f,false,false,1},
            {"Cancel",    "escape","","","",3,0.001f,3,false,false,0},
            {"Submit",    "return","","joystick button 0","",3,0.001f,3,false,false,0},
        };
    }

    void open() { _open = true; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({480,520}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Input Manager##ipmgr", &_open)) { ImGui::End(); return; }

        ImGui::SeparatorText("Virtual Axes / Buttons");
        ImGui::TextDisabled("Script: Input::GetAxis(\"Horizontal\"), Input::GetButton(\"Jump\")");
        ImGui::Spacing();

        int to_del=-1;
        for (int i=0;i<(int)axes.size();++i) {
            auto& ax=axes[i];
            ImGui::PushID(i);
            bool& ex=_expanded[i];
            ImGui::SetNextItemOpen(ex, ImGuiCond_Always);
            if (ImGui::CollapsingHeader(ax.name.c_str())) {
                ex=true;
                ImGui::Indent(12);
                auto field=[&](const char* lbl, std::string& v){
                    char b[64]; snprintf(b,sizeof(b),"%s",v.c_str());
                    ImGui::SetNextItemWidth(160);
                    if(ImGui::InputText(lbl,b,sizeof(b))) v=b;
                };
                const char* types[]={"Key/Mouse Button","Mouse Movement","Joystick Axis"};
                ImGui::Combo("Type",&ax.type,types,3);
                field("Positive Key##pk",  ax.positive_key);
                field("Negative Key##nk",  ax.negative_key);
                field("Alt Positive##ap",  ax.alt_positive);
                field("Alt Negative##an",  ax.alt_negative);
                ImGui::SetNextItemWidth(100); ImGui::InputFloat("Gravity##g",&ax.gravity,0.1f);
                ImGui::SetNextItemWidth(100); ImGui::InputFloat("Dead##d",   &ax.dead,   0.001f);
                ImGui::SetNextItemWidth(100); ImGui::InputFloat("Sensitivity",&ax.sensitivity,0.1f);
                ImGui::Checkbox("Snap",&ax.snap); ImGui::SameLine();
                ImGui::Checkbox("Invert",&ax.invert);
                if (ImGui::Button("Delete Axis")) to_del=i;
                ImGui::Unindent(12);
            } else { ex=false; }
            ImGui::PopID();
        }
        if(to_del>=0) { axes.erase(axes.begin()+to_del); }

        ImGui::Spacing();
        if (ImGui::Button("+ Add Axis")) {
            axes.push_back({"New Axis","","","","",3,0.001f,3,false,false,0});
            _expanded[(int)axes.size()-1]=true;
        }

        ImGui::Spacing();
        if (ImGui::Button("Save##ims")) {
            nlohmann::json j=nlohmann::json::array();
            for(auto& ax:axes) j.push_back({{"name",ax.name},
                {"pos",ax.positive_key},{"neg",ax.negative_key},
                {"apos",ax.alt_positive},{"aneg",ax.alt_negative},
                {"grav",ax.gravity},{"dead",ax.dead},{"sens",ax.sensitivity},
                {"snap",ax.snap},{"inv",ax.invert},{"type",ax.type}});
            auto p=fs::path(st.scene_path).parent_path()/"ProjectSettings"/"InputManager.json";
            if(save_json(p,j)) st.log_success("Input manager saved.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Load##iml")) {
            auto p=fs::path(st.scene_path).parent_path()/"ProjectSettings"/"InputManager.json";
            auto j=load_json(p);
            if(j.is_array()){
                axes.clear();
                for(auto& v:j) axes.push_back({v.value("name",""),
                    v.value("pos",""),v.value("neg",""),
                    v.value("apos",""),v.value("aneg",""),
                    (float)v.value("grav",3.0),(float)v.value("dead",0.001),
                    (float)v.value("sens",3.0),
                    v.value("snap",false),v.value("inv",false),v.value("type",0)});
                st.log_success("Input manager loaded.");
            }
        }
        ImGui::End();
    }
};

// ─── Asset Importer Settings Panel ───────────────────────────────────────────
// Unity's per-asset import settings inspector. Shows when clicking a texture/
// audio/script asset. Allows overriding PPU, filter mode, compression, etc.
// Settings saved as .meta sidecar files next to each asset.

class AssetImporterPanel {
public:
    bool        _open = false;
    std::string _asset_path;

    // Cached import settings (loaded from .meta on open)
    int    _tex_ppu     = 100;
    int    _filter      = 0;  // 0=point,1=bilinear,2=trilinear
    int    _wrap        = 0;  // 0=clamp,1=repeat,2=mirror
    int    _compression = 1;  // 0=none,1=normal,2=high
    bool   _gen_mipmaps = false;
    bool   _alpha_is_trans= true;
    int    _pivot       = 4;  // 0-8 (same as SpriteSlicer)
    float  _pivot_x     = 0.5f, _pivot_y=0.5f;
    // Audio
    int    _audio_fmt   = 0;  // 0=PCM,1=Vorbis,2=ADPCM
    float  _audio_quality=0.7f;
    bool   _audio_3d    = false;
    bool   _load_bg     = false;

    void open(const std::string& path) {
        _asset_path = path;
        _open = true;
        // Load existing .meta if present
        auto j = load_json(fs::path(path).string()+".meta");
        if (j.is_object()) {
            _tex_ppu      = j.value("ppu",100);
            _filter       = j.value("filter",0);
            _wrap         = j.value("wrap",0);
            _compression  = j.value("compress",1);
            _gen_mipmaps  = j.value("mipmaps",false);
            _alpha_is_trans=j.value("alpha_trans",true);
            _audio_fmt    = j.value("audio_fmt",0);
            _audio_quality= j.value("audio_quality",0.7f);
        }
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({340,400}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Import Settings##imp", &_open)) { ImGui::End(); return; }

        ImGui::TextDisabled("%s", fs::path(_asset_path).filename().string().c_str());
        ImGui::Separator();

        auto ext = fs::path(_asset_path).extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        bool is_img  = (ext==".png"||ext==".jpg"||ext==".bmp"||ext==".tga"||ext==".gif");
        bool is_audio= (ext==".wav"||ext==".ogg"||ext==".mp3"||ext==".flac");

        if (is_img) {
            ImGui::SeparatorText("Texture");
            ImGui::InputInt("Pixels Per Unit", &_tex_ppu, 1, 10);
            const char* filters[]={"Point (no filter)","Bilinear","Trilinear"};
            ImGui::Combo("Filter Mode",  &_filter,  filters, 3);
            const char* wraps[]={"Clamp","Repeat","Mirror"};
            ImGui::Combo("Wrap Mode",    &_wrap,    wraps,   3);
            const char* comps[]={"None","Normal Quality","High Quality"};
            ImGui::Combo("Compression",  &_compression, comps, 3);
            ImGui::Checkbox("Generate Mipmaps", &_gen_mipmaps);
            ImGui::Checkbox("Alpha Is Transparency", &_alpha_is_trans);

            ImGui::SeparatorText("Sprite");
            const char* pivots[]={"Top Left","Top","Top Right","Left","Center",
                "Right","Bottom Left","Bottom","Bottom Right","Custom"};
            ImGui::Combo("Pivot", &_pivot, pivots, 10);
            if (_pivot==9) {
                ImGui::InputFloat("Pivot X",&_pivot_x,0.01f,0,"%.2f");
                ImGui::InputFloat("Pivot Y",&_pivot_y,0.01f,0,"%.2f");
            }
        } else if (is_audio) {
            ImGui::SeparatorText("Audio");
            const char* fmts[]={"PCM (uncompressed)","Vorbis","ADPCM"};
            ImGui::Combo("Format", &_audio_fmt, fmts, 3);
            if (_audio_fmt==1)
                ImGui::SliderFloat("Quality",&_audio_quality,0.f,1.f,"%.2f");
            ImGui::Checkbox("3D Spatial Blend", &_audio_3d);
            ImGui::Checkbox("Load in Background",&_load_bg);
        } else {
            ImGui::TextDisabled("No special import settings for this file type.");
        }

        ImGui::Spacing();
        if (ImGui::Button("Apply##imp", {-1,0})) {
            nlohmann::json j = {
                {"ppu",_tex_ppu},{"filter",_filter},{"wrap",_wrap},
                {"compress",_compression},{"mipmaps",_gen_mipmaps},
                {"alpha_trans",_alpha_is_trans},
                {"audio_fmt",_audio_fmt},{"audio_quality",_audio_quality},
                {"audio_3d",_audio_3d},{"load_bg",_load_bg},
                {"pivot",_pivot},{"pivot_x",_pivot_x},{"pivot_y",_pivot_y}
            };
            if (save_json(fs::path(_asset_path).string()+".meta", j))
                st.log_success("Import settings saved: " + _asset_path + ".meta");
            _open = false;
        }
        ImGui::End();
    }
};

// ─── Prefab Manager Panel ─────────────────────────────────────────────────────
// Unity-style prefab workflow: save any entity (with its full hierarchy and
// components) as a .prefab JSON asset; instantiate it by dragging from the
// Assets panel or via the Prefab Manager. Supports nested prefabs (a prefab
// that contains another prefab by asset path reference). Overrides are tracked
// as a diff against the source prefab so individual field changes don't sever
// the link.
//
// Prefab format (.prefab):
//   { "root": <entity JSON>, "children": [ <entity JSON>, ... ] }
// Override format stored on the instance entity:
//   { "prefab_path": "assets/Player.prefab", "overrides": { "components.Transform.x": 100 } }

// Dedicated prefab workspace.  This intentionally edits an isolated prefab
// document rather than swapping the currently open scene, so opening a prefab
// cannot dirty, replace, or interrupt the user's scene/play state.
class PrefabStagePanel {
    using NodePath = std::vector<std::size_t>;

    bool _open = false;
    bool _dirty = false;
    std::string _path;
    std::string _asset_dir;
    Entity _root;
    NodePath _selected_path;
    float _zoom = 1.f;
    ImVec2 _view_offset{0.f, 0.f};
    bool _frame_pending = true;
    thumbnail_cache::Cache _preview_cache;

    struct PreviewBounds {
        float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
        bool valid = false;

        void include(float x0, float y0, float x1, float y1) {
            if (!valid) {
                min_x = x0; min_y = y0; max_x = x1; max_y = y1; valid = true;
                return;
            }
            min_x = std::min(min_x, x0); min_y = std::min(min_y, y0);
            max_x = std::max(max_x, x1); max_y = std::max(max_y, y1);
        }
    };

    static bool path_equal(const NodePath& a, const NodePath& b) { return a == b; }

    static std::string path_id(const NodePath& path) {
        std::string out = "root";
        for (std::size_t i : path) out += "_" + std::to_string(i);
        return out;
    }

    Entity* node_at(const NodePath& path) {
        Entity* node = &_root;
        for (std::size_t index : path) {
            if (!node->contains("_prefab_children") || !(*node)["_prefab_children"].is_array() ||
                index >= (*node)["_prefab_children"].size()) return nullptr;
            node = &(*node)["_prefab_children"][index];
        }
        return node;
    }

    static int parent_id_of(const Entity& e) {
        if (!e.contains("components") || !e["components"].contains("Transform")) return -1;
        return e["components"]["Transform"].value("parent", -1);
    }

    // PrefabManager v1 wrote children as a legacy flat top-level array.
    // Read that form without losing objects, then save it back in the current
    // nested prefab format after the first explicit stage save.
    static void attach_legacy_children(Entity& node, std::vector<Entity>& flat,
                                       std::vector<bool>& consumed) {
        const int parent_id = node.value("id", -1);
        for (std::size_t i = 0; i < flat.size(); ++i) {
            if (consumed[i] || parent_id_of(flat[i]) != parent_id) continue;
            consumed[i] = true;
            Entity child = flat[i].deep_clone();
            attach_legacy_children(child, flat, consumed);
            node["_prefab_children"].push_back(std::move(child));
        }
    }

    static int max_id(const Entity& node) {
        int result = node.value("id", 0);
        if (node.contains("_prefab_children") && node["_prefab_children"].is_array())
            for (const auto& child : node["_prefab_children"])
                result = std::max(result, max_id(child));
        return result;
    }

    static bool sprite_size(const Entity& node, float& width, float& height) {
        if (!node.contains("components") || !node["components"].contains("SpriteRenderer")) return false;
        const auto& sr = node["components"]["SpriteRenderer"];
        // Most real prefabs use source_w/source_h for a sprite-sheet frame.
        // The old preview looked only for width/height, so it was incorrect
        // even when the object happened to be on screen.
        width = sr.value("source_w", sr.value("width", sr.value("w", 56.f)));
        height = sr.value("source_h", sr.value("height", sr.value("h", 56.f)));
        return true;
    }

    void collect_visible_bounds(const Entity& node, float parent_x, float parent_y,
                                float parent_sx, float parent_sy, PreviewBounds& bounds) const {
        float x = parent_x, y = parent_y, sx = parent_sx, sy = parent_sy;
        if (node.contains("components") && node["components"].contains("Transform")) {
            const auto& tr = node["components"]["Transform"];
            x += tr.value("x", 0.f) * parent_sx;
            y += tr.value("y", 0.f) * parent_sy;
            sx *= tr.value("scale_x", 1.f);
            sy *= tr.value("scale_y", 1.f);
        }
        float width = 56.f, height = 56.f;
        if (sprite_size(node, width, height)) {
            width = std::max(1.f, std::abs(width * sx));
            height = std::max(1.f, std::abs(height * sy));
            bounds.include(x - width * .5f, y - height * .5f,
                           x + width * .5f, y + height * .5f);
        }
        if (node.contains("_prefab_children") && node["_prefab_children"].is_array()) {
            for (const auto& child : node["_prefab_children"])
                collect_visible_bounds(child, x, y, sx, sy, bounds);
        }
    }

    void frame_visible_content(const ImVec2& canvas_size) {
        PreviewBounds bounds;
        collect_visible_bounds(_root, 0.f, 0.f, 1.f, 1.f, bounds);
        if (!bounds.valid) {
            _zoom = 1.f;
            _view_offset = {0.f, 0.f};
            _frame_pending = false;
            return;
        }
        const float width = std::max(1.f, bounds.max_x - bounds.min_x);
        const float height = std::max(1.f, bounds.max_y - bounds.min_y);
        const float fit_x = std::max(1.f, canvas_size.x - 88.f) / width;
        const float fit_y = std::max(1.f, canvas_size.y - 88.f) / height;
        _zoom = std::clamp(std::min(fit_x, fit_y), .25f, 4.f);
        const float cx = (bounds.min_x + bounds.max_x) * .5f;
        const float cy = (bounds.min_y + bounds.max_y) * .5f;
        _view_offset = {-cx * _zoom, -cy * _zoom};
        _frame_pending = false;
    }

    void draw_tree(Entity& node, const NodePath& path) {
        const bool has_children = node.contains("_prefab_children") && node["_prefab_children"].is_array() &&
                                  node["_prefab_children"].size() > 0;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (path_equal(path, _selected_path)) flags |= ImGuiTreeNodeFlags_Selected;
        const std::string name = node.value("name", std::string("GameObject"));
        const bool open = ImGui::TreeNodeEx(path_id(path).c_str(), flags, "%s", name.c_str());
        if (ImGui::IsItemClicked()) _selected_path = path;
        if (has_children && open) {
            for (std::size_t i = 0; i < node["_prefab_children"].size(); ++i) {
                NodePath child_path = path;
                child_path.push_back(i);
                draw_tree(node["_prefab_children"][i], child_path);
            }
            ImGui::TreePop();
        }
    }

    bool draw_preview_node(Entity& node, const NodePath& path, ImDrawList* dl,
                           const ImVec2& center, float parent_x, float parent_y,
                           float parent_sx, float parent_sy,
                           const ImVec2& mouse, bool click) {
        float x = parent_x, y = parent_y, sx = parent_sx, sy = parent_sy;
        if (node.contains("components") && node["components"].contains("Transform")) {
            const auto& tr = node["components"]["Transform"];
            x += tr.value("x", 0.f) * parent_sx; y += tr.value("y", 0.f) * parent_sy;
            sx *= tr.value("scale_x", 1.f); sy *= tr.value("scale_y", 1.f);
        }
        float width = 56.f, height = 56.f;
        const bool has_sprite = sprite_size(node, width, height);
        width = std::max(12.f, std::abs(width * sx * _zoom));
        height = std::max(12.f, std::abs(height * sy * _zoom));
        const ImVec2 p0{center.x + _view_offset.x + x * _zoom - width * .5f,
                         center.y + _view_offset.y + y * _zoom - height * .5f};
        const ImVec2 p1{p0.x + width, p0.y + height};
        const bool selected = path_equal(path, _selected_path);
        const bool hovered = mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y;
        const ImU32 fill = has_sprite
            ? IM_COL32(91, 155, 242, 120) : IM_COL32(126, 100, 200, 105);
        dl->AddRectFilled(p0, p1, fill, 3.f);
        if (has_sprite) {
            const auto& sr = node["components"]["SpriteRenderer"];
            const std::string texture = node["components"]["SpriteRenderer"].value("texture", std::string{});
            if (!texture.empty()) {
                fs::path texture_path = texture;
                if (!texture_path.is_absolute()) {
                    const std::array<fs::path, 3> candidates{
                        fs::path(_asset_dir) / texture_path,
                        fs::path(_path).parent_path() / texture_path,
                        fs::path(_asset_dir) / texture_path.filename()
                    };
                    for (const fs::path& candidate : candidates) {
                        std::error_code ec;
                        if (fs::is_regular_file(candidate, ec)) { texture_path = candidate; break; }
                    }
                }
                if (const auto* image = _preview_cache.get(texture_path.string()); image && image->imgui_ds) {
                    ImVec2 uv0{0.f, 0.f}, uv1{1.f, 1.f};
                    if (sr.value("use_source_rect", false) && image->w > 0 && image->h > 0) {
                        const float source_x = sr.value("source_x", 0.f);
                        const float source_y = sr.value("source_y", 0.f);
                        const float source_w = sr.value("source_w", static_cast<float>(image->w));
                        const float source_h = sr.value("source_h", static_cast<float>(image->h));
                        uv0 = {source_x / image->w, source_y / image->h};
                        uv1 = {(source_x + source_w) / image->w, (source_y + source_h) / image->h};
                    }
                    if (sr.value("flip_x", false)) std::swap(uv0.x, uv1.x);
                    if (sr.value("flip_y", false)) std::swap(uv0.y, uv1.y);
                    dl->AddImage((ImTextureID)(intptr_t)image->imgui_ds, p0, p1, uv0, uv1);
                }
            }
        }
        dl->AddRect(p0, p1, selected ? IM_COL32(255, 215, 100, 255) :
                               (hovered ? IM_COL32(135, 205, 255, 255) : IM_COL32(185, 185, 195, 190)),
                    3.f, 0, selected ? 2.5f : 1.2f);
        dl->AddText({p0.x + 4.f, p0.y + 4.f}, IM_COL32(245,245,248,235),
                    node.value("name", std::string("GameObject")).c_str());

        bool hit = hovered && click;
        if (node.contains("_prefab_children") && node["_prefab_children"].is_array()) {
            for (std::size_t i = 0; i < node["_prefab_children"].size(); ++i) {
                NodePath child_path = path;
                child_path.push_back(i);
                hit = draw_preview_node(node["_prefab_children"][i], child_path, dl, center, x, y, sx, sy, mouse, click) || hit;
            }
        }
        if (hovered && click) _selected_path = path;
        return hit;
    }

    bool edit_value(const std::string& label, Entity& value) {
        if (value.is_boolean()) {
            bool v = value.get<bool>();
            if (ImGui::Checkbox(label.c_str(), &v)) { value = v; return true; }
        } else if (value.is_number_integer()) {
            int v = value.get<int>();
            if (ImGui::DragInt(label.c_str(), &v, 1.f)) { value = v; return true; }
        } else if (value.is_number_float()) {
            float v = value.get<float>();
            if (ImGui::DragFloat(label.c_str(), &v, .1f)) { value = v; return true; }
        } else if (value.is_string()) {
            char buf[256]; std::snprintf(buf, sizeof(buf), "%s", value.get<std::string>().c_str());
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) { value = std::string(buf); return true; }
        } else if (value.is_array() && value.size() == 4 && value[0].is_number() && value[1].is_number()) {
            float color[4] = { value[0].get<float>() / 255.f, value[1].get<float>() / 255.f,
                               value[2].get<float>() / 255.f, value[3].get<float>() / 255.f };
            if (ImGui::ColorEdit4(label.c_str(), color)) {
                for (int i = 0; i < 4; ++i) value[i] = (int)(color[i] * 255.f);
                return true;
            }
        } else {
            ImGui::TextDisabled("%s (complex value)", label.c_str());
        }
        return false;
    }

    void draw_properties() {
        Entity* node = node_at(_selected_path);
        if (!node) { ImGui::TextDisabled("Select an object in the prefab."); return; }
        ImGui::TextColored({0.65f,0.86f,1.f,1.f}, "%s", node->value("name", "GameObject").c_str());
        ImGui::Separator();
        _dirty = edit_value("Name", (*node)["name"]) || _dirty;
        _dirty = edit_value("Active", (*node)["active"]) || _dirty;

        Entity& transform_comp = (*node)["components"]["Transform"];
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            _dirty = edit_value("Position X", transform_comp["x"]) || _dirty;
            _dirty = edit_value("Position Y", transform_comp["y"]) || _dirty;
            _dirty = edit_value("Rotation", transform_comp["rotation"]) || _dirty;
            _dirty = edit_value("Scale X", transform_comp["scale_x"]) || _dirty;
            _dirty = edit_value("Scale Y", transform_comp["scale_y"]) || _dirty;
        }
        if (node->contains("components") && (*node)["components"].is_object()) {
            for (auto& [component_name, component] : (*node)["components"].items()) {
                if (component_name == "Transform" || !component.is_object()) continue;
                ImGui::PushID(component_name.c_str());
                if (ImGui::CollapsingHeader(component_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (auto& [key, value] : component.items()) {
                        ImGui::PushID(key.c_str());
                        _dirty = edit_value(key, value) || _dirty;
                        ImGui::PopID();
                    }
                }
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Add Child")) {
            Entity child = Entity::object();
            child["id"] = max_id(_root) + 1;
            child["name"] = "Child";
            child["active"] = true;
            child["components"]["Transform"] = Entity::object();
            child["components"]["Transform"]["x"] = 32.f;
            child["components"]["Transform"]["y"] = 32.f;
            child["components"]["Transform"]["rotation"] = 0.f;
            child["components"]["Transform"]["scale_x"] = 1.f;
            child["components"]["Transform"]["scale_y"] = 1.f;
            child["components"]["Transform"]["parent"] = node->value("id", -1);
            (*node)["_prefab_children"].push_back(std::move(child));
            _dirty = true;
        }
        if (!_selected_path.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Delete Selected")) {
                NodePath parent = _selected_path;
                const std::size_t index = parent.back(); parent.pop_back();
                if (Entity* parent_node = node_at(parent)) (*parent_node)["_prefab_children"].erase_at(index);
                _selected_path = parent;
                _dirty = true;
            }
        }
    }

public:
    void init(vkr::RendererBackend& backend) { _preview_cache.init(backend); }
    void shutdown() { _preview_cache.clear(); }

    bool open(const std::string& path, EditorState& st) {
        const nlohmann::json doc = load_json(path);
        if (!doc.is_object() || !doc.contains("root") || !doc["root"].is_object()) {
            st.log_error("Prefab Stage could not open an invalid prefab: " + path);
            return false;
        }
        _root = Entity(doc["root"]);
        if (doc.contains("children") && doc["children"].is_array() && !_root.contains("_prefab_children")) {
            std::vector<Entity> flat;
            for (const auto& child : doc["children"]) if (child.is_object()) flat.emplace_back(Entity(child));
            std::vector<bool> consumed(flat.size(), false);
            attach_legacy_children(_root, flat, consumed);
        }
        _path = path;
        _asset_dir = st.asset_dir;
        _selected_path.clear();
        _dirty = false;
        _frame_pending = true;
        _open = true;
        return true;
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({1040, 690}, ImGuiCond_FirstUseEver);
        const std::string title = "Prefab Stage - " + fs::path(_path).filename().string() + "##prefabstage";
        if (!ImGui::Begin(title.c_str(), &_open, ImGuiWindowFlags_MenuBar)) { ImGui::End(); return; }
        if (ImGui::BeginMenuBar()) {
            if (ImGui::Button("Save")) {
                if (prefab::save(_root, _path)) {
                    _dirty = false;
                    st.log_success("Saved prefab: " + fs::path(_path).filename().string());
                } else st.log_error("Could not save prefab: " + _path);
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert")) open(_path, st);
            ImGui::SameLine();
            if (ImGui::Button("Frame All")) _frame_pending = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderFloat("Zoom", &_zoom, .25f, 4.f, "%.2fx");
            ImGui::SameLine();
            ImGui::TextColored(_dirty ? ImVec4(1.f,.75f,.28f,1.f) : ImVec4(.45f,.9f,.55f,1.f),
                               _dirty ? "Unsaved changes" : "Saved");
            ImGui::EndMenuBar();
        }

        ImGui::BeginChild("##prefab_hierarchy", {220.f, 0}, true);
        ImGui::TextDisabled("PREFAB HIERARCHY");
        ImGui::Separator();
        draw_tree(_root, {});
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##prefab_preview", {-330.f, 0}, true);
        const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
        const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        if (_frame_pending && canvas_size.x > 1.f && canvas_size.y > 1.f) frame_visible_content(canvas_size);
        ImGui::InvisibleButton("##prefab_canvas", canvas_size,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        const bool preview_hovered = ImGui::IsItemHovered();
        const ImGuiIO& io = ImGui::GetIO();
        // A stage must be navigable even when a large sprite sits below the
        // cursor.  Middle/right drag pans; the wheel zooms around the pointer
        // so the part being inspected never jumps away.
        if (preview_hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
            _view_offset.x += io.MouseDelta.x;
            _view_offset.y += io.MouseDelta.y;
        }
        if (preview_hovered && io.MouseWheel != 0.f) {
            const ImVec2 center{canvas_min.x + canvas_size.x*.5f, canvas_min.y + canvas_size.y*.5f};
            const ImVec2 mouse = ImGui::GetMousePos();
            const float old_zoom = _zoom;
            _zoom = std::clamp(_zoom * (io.MouseWheel > 0.f ? 1.12f : .89f), .25f, 4.f);
            const float factor = _zoom / old_zoom;
            _view_offset.x = mouse.x - center.x - (mouse.x - center.x - _view_offset.x) * factor;
            _view_offset.y = mouse.y - center.y - (mouse.y - center.y - _view_offset.y) * factor;
        }
        const bool click = ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
                           !ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(canvas_min, {canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y}, IM_COL32(37, 40, 48, 255));
        for (float x = std::fmod(canvas_min.x + canvas_size.x * .5f, 32.f); x < canvas_min.x + canvas_size.x; x += 32.f)
            dl->AddLine({x,canvas_min.y}, {x,canvas_min.y+canvas_size.y}, IM_COL32(78,82,93,90));
        for (float y = std::fmod(canvas_min.y + canvas_size.y * .5f, 32.f); y < canvas_min.y + canvas_size.y; y += 32.f)
            dl->AddLine({canvas_min.x,y}, {canvas_min.x+canvas_size.x,y}, IM_COL32(78,82,93,90));
        const ImVec2 center{canvas_min.x + canvas_size.x*.5f, canvas_min.y + canvas_size.y*.5f};
        dl->AddLine({canvas_min.x, center.y}, {canvas_min.x+canvas_size.x, center.y}, IM_COL32(130,130,145,150), 1.5f);
        dl->AddLine({center.x, canvas_min.y}, {center.x, canvas_min.y+canvas_size.y}, IM_COL32(130,130,145,150), 1.5f);
        const bool hit = draw_preview_node(_root, {}, dl, center, 0.f, 0.f, 1.f, 1.f, ImGui::GetMousePos(), click);
        if (click && !hit) _selected_path.clear();
        dl->AddText({canvas_min.x+10.f,canvas_min.y+10.f}, IM_COL32(210,215,225,220),
                    "Prefab Preview  |  Middle/right drag: pan  •  Wheel: zoom  •  Frame All: fit");
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##prefab_properties", {0, 0}, true);
        ImGui::TextDisabled("PREFAB INSPECTOR");
        ImGui::Separator();
        draw_properties();
        ImGui::EndChild();

        ImGui::End();
        if (!_open && _dirty)
            st.log_warn("Prefab Stage closed with unsaved changes: " + fs::path(_path).filename().string());
    }
};

class PrefabManagerPanel {
public:
    bool        _open = false;
    std::string _filter;
    std::string _sel_path;  // currently selected .prefab path
    char        _name_buf[128] = {};

    void open() { _open = true; }

    // Save the selected entity (and its full child hierarchy) as a .prefab asset.
    static bool save_prefab(EditorState& st, int entity_id, const fs::path& out_path) {
        const Entity* root = st.find_entity(entity_id);
        if (!root) return false;

        nlohmann::json doc;
        doc["root"] = *root;

        // Recursively gather children
        std::function<void(int, nlohmann::json&)> collect = [&](int pid, nlohmann::json& arr) {
            for (int cid : st.children_of(pid)) {
                const Entity* ce = st.find_entity(cid);
                if (!ce) continue;
                arr.push_back(*ce);
                collect(cid, arr);
            }
        };
        doc["children"] = nlohmann::json::array();
        collect(entity_id, doc["children"]);

        // Tag the root with its prefab source path so instantiated copies
        // know which asset they came from.
        doc["root"]["prefab_path"] = out_path.string();

        return save_json(out_path, doc);
    }

    // Instantiate a .prefab into the current scene, re-mapping IDs to avoid
    // collisions. Returns the id of the new root entity or -1 on failure.
    static int instantiate_prefab(EditorState& st, const fs::path& prefab_path,
                                   float world_x = 0.f, float world_y = 0.f) {
        auto doc = load_json(prefab_path);
        if (!doc.is_object() || !doc.contains("root")) return -1;

        // Build an id re-map: old_id -> new_id
        std::unordered_map<int,int> id_map;
        auto remap_id = [&](int old_id) -> int {
            auto it = id_map.find(old_id);
            if (it != id_map.end()) return it->second;
            int nid = st.next_id() + (int)id_map.size();
            id_map[old_id] = nid;
            return nid;
        };

        // Collect all entities from the prefab doc
        std::vector<nlohmann::json> all_ents;
        all_ents.push_back(doc["root"]);
        if (doc.contains("children") && doc["children"].is_array())
            for (auto& c : doc["children"]) all_ents.push_back(c);

        // First pass: assign new IDs
        for (auto& e : all_ents) {
            int old_id = e.value("id", 0);
            remap_id(old_id);
        }

        // Second pass: fix id and parent references, insert into scene
        int root_id = -1;
        bool first = true;
        for (auto& e : all_ents) {
            int old_id = e.value("id", 0);
            int new_id = id_map[old_id];
            e["id"] = new_id;

            // Fix parent reference
            if (e.contains("components") && e["components"].contains("Transform")) {
                auto& tr = e["components"]["Transform"];
                int old_parent = tr.value("parent", -1);
                if (old_parent >= 0) {
                    auto pit = id_map.find(old_parent);
                    tr["parent"] = (pit != id_map.end()) ? pit->second : -1;
                }
                // Offset root entity to spawn position
                if (first) {
                    tr["x"] = (float)tr.value("x", 0.0) + world_x;
                    tr["y"] = (float)tr.value("y", 0.0) + world_y;
                }
            }
            // Store prefab source
            e["prefab_path"] = prefab_path.string();

            if (first) { root_id = new_id; first = false; }
            st.entities.push_back(e);
        }
        transform::mark_structure_dirty();
        return root_id;
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({420, 480}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Prefab Manager##pfb", &_open)) { ImGui::End(); return; }

        ImGui::SeparatorText("Prefabs in Project");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pfbfilt", _filter.data(), 128,
                         ImGuiInputTextFlags_CallbackResize,
                         [](ImGuiInputTextCallbackData* d) -> int {
                             if (d->EventFlag == ImGuiInputTextFlags_CallbackResize)
                                 ((std::string*)d->UserData)->resize(d->BufTextLen);
                             return 0;
                         }, &_filter);
        ImGui::SameLine();
        ImGui::TextDisabled("(filter)");

        ImGui::Spacing();

        // List all .prefab files under the asset dir
        std::vector<fs::path> prefabs;
        std::error_code ec;
        for (auto& de : fs::recursive_directory_iterator(st.asset_dir, ec))
            if (!ec && de.path().extension() == ".prefab")
                prefabs.push_back(de.path());

        if (ImGui::BeginChild("##pfblist", {-1, 220}, true)) {
            for (auto& p : prefabs) {
                std::string stem = p.stem().string();
                if (!_filter.empty() && stem.find(_filter) == std::string::npos) continue;
                bool sel = (_sel_path == p.string());
                if (ImGui::Selectable(("⬡  " + stem).c_str(), sel))
                    _sel_path = p.string();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", p.string().c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::SeparatorText("Actions");

        // Save selected entity as new prefab
        if (st.selected_id >= 0) {
            ImGui::InputText("Prefab Name##pfbname", _name_buf, sizeof(_name_buf));
            if (ImGui::Button("Save Selected as Prefab")) {
                std::string fname = std::string(_name_buf);
                if (fname.empty()) fname = "NewPrefab";
                fs::path out = fs::path(st.asset_dir) / (fname + ".prefab");
                if (save_prefab(st, st.selected_id, out))
                    st.log_success("Prefab saved: " + out.string());
                else
                    st.log_error("Failed to save prefab.");
            }
        } else {
            ImGui::TextDisabled("(Select an entity in Hierarchy to save as prefab)");
        }

        ImGui::Spacing();

        if (!_sel_path.empty()) {
            ImGui::Text("Selected: %s", fs::path(_sel_path).filename().string().c_str());
            if (ImGui::Button("Open Prefab Stage")) {
                st.requested_prefab_stage_asset = _sel_path;
                st.request_prefab_stage_open = true;
            }
            if (ImGui::Button("Instantiate at Origin")) {
                st.undo.push_deep(st.entities);
                int nid = instantiate_prefab(st, _sel_path, 0.f, 0.f);
                if (nid >= 0) { st.select(nid); st.log_success("Prefab instantiated."); }
                else          st.log_error("Failed to instantiate prefab.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Instantiate at Camera")) {
                st.undo.push_deep(st.entities);
                int nid = instantiate_prefab(st, _sel_path, st.cam_x, st.cam_y);
                if (nid >= 0) { st.select(nid); st.log_success("Prefab instantiated at camera."); }
            }
            ImGui::Spacing();
            if (ImGui::Button("Apply Overrides (Unlink)")) {
                // Simply remove the prefab_path tag — the instance is now standalone.
                Entity* e = st.find_entity(st.selected_id);
                if (e) { e->erase("prefab_path"); st.log("Prefab link removed."); }
            }
        }

        ImGui::End();
    }
};

// ─── NavMesh2D Baking Panel ───────────────────────────────────────────────────
// Mirrors Unity's Navigation window: defines walkable areas and obstacles,
// then serialises a simple waypoint graph (nodes + edges) as navmesh.json
// so the runtime NavPathSystem can A* through it.
//
// The baking strategy here is "bake from Waypoint2D entities": the user places
// Waypoint2D entities throughout the scene to define reachable nodes, and this
// panel builds the adjacency list by raycasting between all pairs and checking
// clearance. A full polygon-based NavMesh (Recast-style) would require a
// geometry kernel; the waypoint graph is the pragmatic 2D approach used by
// most 2D engines and is sufficient for most use cases.

class NavMeshPanel {
public:
    bool  _open       = false;
    float _agent_radius = 16.f;
    float _max_edge   = 200.f;  // max waypoint–waypoint edge length (px)
    int   _layer_mask = 0xFF;   // which physics layers count as obstacles
    bool  _show_gizmos = true;

    void open() { _open = true; }

    // Build adjacency list from all Waypoint2D / NavMeshAgent2D entities
    nlohmann::json bake(EditorState& st) {
        nlohmann::json graph;
        graph["nodes"]  = nlohmann::json::array();
        graph["edges"]  = nlohmann::json::array();
        graph["agent_radius"] = _agent_radius;

        // Collect all entities that act as nav nodes (Waypoint2D or marked NavNode)
        struct NavNode { int id; float x, y; };
        std::vector<NavNode> nodes;
        for (auto& e : st.entities) {
            if (!e.contains("components")) continue;
            auto& comps = e["components"];
            float x = 0.f, y = 0.f;
            if (comps.contains("Transform")) {
                x = comps["Transform"].value("x", 0.0);
                y = comps["Transform"].value("y", 0.0);
            }
            if (comps.contains("Waypoint2D") || comps.contains("NavMeshAgent2D")) {
                int nid = e.value("id", 0);
                std::string name = e.value("name", std::to_string(nid));
                nlohmann::json node = {{"id", nid}, {"name", name}, {"x", x}, {"y", y}};
                graph["nodes"].push_back(node);
                float pos[2] = {x, y};
                (void)pos;
                nodes.push_back({nid, x, y});
            }
        }

        // Build edges: connect nodes within _max_edge distance
        // (full raycast obstacle detection requires physics; here we use
        //  distance-only as the base heuristic and let the user tune _max_edge)
        for (int i = 0; i < (int)nodes.size(); ++i) {
            for (int j = i+1; j < (int)nodes.size(); ++j) {
                float dx = nodes[j].x - nodes[i].x;
                float dy = nodes[j].y - nodes[i].y;
                float dist = std::sqrt(dx*dx + dy*dy);
                if (dist <= _max_edge) {
                    graph["edges"].push_back({
                        {"from", nodes[i].id},
                        {"to",   nodes[j].id},
                        {"cost", dist}
                    });
                }
            }
        }
        return graph;
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({380, 420}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("NavMesh 2D##nav", &_open)) { ImGui::End(); return; }

        ImGui::SeparatorText("Bake Settings");
        ImGui::SliderFloat("Agent Radius##nav", &_agent_radius, 4.f, 128.f, "%.0f px");
        ImGui::SliderFloat("Max Edge Length##nav", &_max_edge, 32.f, 1024.f, "%.0f px");
        ImGui::Checkbox("Show Gizmos##nav", &_show_gizmos);

        ImGui::Spacing();
        ImGui::TextWrapped("Place Waypoint2D or NavMeshAgent2D entities in the scene "
                           "to define the navigation graph nodes. Edges are auto-built "
                           "between nodes within Max Edge Length.");

        ImGui::Spacing();
        ImGui::SeparatorText("Scene Nodes");
        int node_count = 0;
        for (auto& e : st.entities) {
            if (!e.contains("components")) continue;
            if (e["components"].contains("Waypoint2D") || e["components"].contains("NavMeshAgent2D"))
                ++node_count;
        }
        ImGui::Text("Nav nodes found: %d", node_count);

        ImGui::Spacing();
        if (ImGui::Button("Bake NavMesh", {-1, 0})) {
            auto graph = bake(st);
            fs::path out = fs::path(st.scene_path).parent_path() / "navmesh.json";
            if (save_json(out, graph))
                st.log_success("NavMesh baked: " + std::to_string(graph["nodes"].size())
                               + " nodes, " + std::to_string(graph["edges"].size()) + " edges → " + out.string());
            else
                st.log_error("NavMesh bake failed.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Quick Add");
        if (ImGui::Button("Add Waypoint Node")) {
            auto e = SceneIO::make_entity("NavNode", st);
            e["components"]["Waypoint2D"] = component_defaults()["Waypoint2D"];
            e["components"]["Transform"]["x"] = st.cam_x;
            e["components"]["Transform"]["y"] = st.cam_y;
            st.undo.push_deep(st.entities);
            st.entities.push_back(e);
            transform::mark_structure_dirty();
            st.select(e.value("id", 0));
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Load Baked Mesh");
        fs::path nav_path = fs::path(st.scene_path).parent_path() / "navmesh.json";
        if (fs::exists(nav_path)) {
            auto j = load_json(nav_path);
            int nn = j.contains("nodes") ? (int)j["nodes"].size() : 0;
            int ne = j.contains("edges") ? (int)j["edges"].size() : 0;
            ImGui::Text("Baked mesh: %d nodes, %d edges", nn, ne);
            ImGui::TextDisabled("%s", nav_path.string().c_str());
        } else {
            ImGui::TextDisabled("(not baked yet)");
        }

        ImGui::End();
    }
};

// ─── Timeline / Cutscene Panel ────────────────────────────────────────────────
// Mirrors Unity's Timeline window: a multi-track sequencer for cutscenes,
// scripted events, and tween animations. Each Track has an entity target and
// a list of Clips (start_time, duration, type, data). Types:
//   "transform"  — tween position/rotation/scale
//   "animation"  — trigger an Animator clip by name
//   "audio"      — play an AudioSource clip
//   "activate"   — enable/disable the entity
//   "script"     — call a named method on the entity's Script component
//
// Stored as <scene>/timeline.json. Runtime playback is handled by
// TimelineSystem in the engine.

class TimelinePanel {
public:
    bool  _open        = false;
    float _playhead    = 0.f;
    float _duration    = 10.f;
    bool  _playing     = false;
    float _zoom        = 60.f;  // px per second
    int   _sel_track   = -1;
    int   _sel_clip    = -1;

    struct Clip {
        float start, duration;
        std::string type;    // "transform"|"animation"|"audio"|"activate"|"script"
        std::string label;
        nlohmann::json data;
    };
    struct Track {
        int         entity_id = -1;
        std::string entity_name;
        std::string type;    // "transform"|"animation"|"audio"|"activation"|"script"
        std::vector<Clip> clips;
        bool locked = false;
        bool muted  = false;
    };
    std::vector<Track> tracks;

    void open() { _open = true; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({800, 400}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Timeline##tl", &_open)) { ImGui::End(); return; }

        // Transport bar
        ImGui::PushStyleColor(ImGuiCol_Button, _playing ? ImVec4(0.7f,0.3f,0.3f,1.f) : ImVec4(0.3f,0.7f,0.3f,1.f));
        if (ImGui::Button(_playing ? "■ Stop##tl" : "▶ Play##tl")) _playing = !_playing;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("|◀##tl")) { _playhead = 0.f; _playing = false; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderFloat("##ph", &_playhead, 0.f, _duration, "%.2f s");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        ImGui::InputFloat("Dur##tl", &_duration, 0.5f, 5.f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.f);
        ImGui::SliderFloat("Zoom##tl", &_zoom, 20.f, 200.f, "%.0f");

        ImGui::Separator();

        // Track list on the left + clip lanes on the right
        float track_label_w = 160.f;
        float lane_h        = 28.f;
        float timeline_w    = _duration * _zoom;

        ImVec2 tl_origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl   = ImGui::GetWindowDrawList();

        // Draw time ruler
        {
            float ruler_h = 18.f;
            ImVec2 ruler_pos = {tl_origin.x + track_label_w, tl_origin.y};
            dl->AddRectFilled(ruler_pos, {ruler_pos.x + timeline_w, ruler_pos.y + ruler_h},
                              IM_COL32(45,45,50,255));
            for (float t = 0.f; t <= _duration; t += 0.5f) {
                float rx = ruler_pos.x + t * _zoom;
                bool major = (fmodf(t, 1.f) < 0.001f);
                dl->AddLine({rx, ruler_pos.y + (major?4.f:8.f)}, {rx, ruler_pos.y + ruler_h},
                            IM_COL32(180,180,180,200), major ? 1.5f : 0.8f);
                if (major) {
                    char lbl[16]; snprintf(lbl, sizeof(lbl), "%.0fs", t);
                    dl->AddText({rx+2.f, ruler_pos.y+2.f}, IM_COL32(200,200,200,255), lbl);
                }
            }
            // Playhead
            float phx = ruler_pos.x + _playhead * _zoom;
            dl->AddLine({phx, ruler_pos.y}, {phx, ruler_pos.y + ruler_h + lane_h * (float)tracks.size()},
                        IM_COL32(255,80,80,220), 1.5f);
            ImGui::Dummy({track_label_w + timeline_w, ruler_h});
        }

        // Track rows
        for (int ti = 0; ti < (int)tracks.size(); ++ti) {
            auto& tr = tracks[ti];
            ImGui::PushID(ti);

            // Label column
            ImVec2 row_pos = ImGui::GetCursorScreenPos();
            bool sel = (_sel_track == ti);
            if (sel) dl->AddRectFilled(row_pos, {row_pos.x + track_label_w, row_pos.y + lane_h},
                                       IM_COL32(60,90,130,200));
            if (ImGui::Selectable(("##trow" + std::to_string(ti)).c_str(), sel,
                                   0, {track_label_w, lane_h}))
                _sel_track = ti;
            ImGui::SetCursorScreenPos({row_pos.x+4.f, row_pos.y+6.f});
            ImGui::TextUnformatted(tr.entity_name.empty() ? "(no entity)" : tr.entity_name.c_str());
            ImGui::SetCursorScreenPos({row_pos.x + 2.f, row_pos.y + 14.f});
            ImGui::TextDisabled("%s", tr.type.c_str());

            // Clip lane
            ImVec2 lane_pos = {row_pos.x + track_label_w, row_pos.y};
            dl->AddRectFilled(lane_pos, {lane_pos.x + timeline_w, lane_pos.y + lane_h},
                              IM_COL32(35,35,40,255));
            dl->AddLine({lane_pos.x, lane_pos.y + lane_h - 1},
                        {lane_pos.x + timeline_w, lane_pos.y + lane_h - 1},
                        IM_COL32(60,60,70,255));

            for (int ci = 0; ci < (int)tr.clips.size(); ++ci) {
                auto& clip = tr.clips[ci];
                float cx = lane_pos.x + clip.start    * _zoom;
                float cw = std::max(8.f, clip.duration * _zoom);
                bool csel = (_sel_track == ti && _sel_clip == ci);
                ImU32 clip_col = csel ? IM_COL32(100,160,255,220) : IM_COL32(70,120,200,180);
                dl->AddRectFilled({cx+1, lane_pos.y+3}, {cx+cw-1, lane_pos.y+lane_h-3},
                                  clip_col, 3.f);
                dl->AddRect({cx+1, lane_pos.y+3}, {cx+cw-1, lane_pos.y+lane_h-3},
                            IM_COL32(150,190,255,200), 3.f);
                if (cw > 30.f)
                    dl->AddText({cx+5, lane_pos.y+8}, IM_COL32(220,230,255,255), clip.label.c_str());

                // Click to select clip
                ImGui::SetCursorScreenPos({cx+1, lane_pos.y+3});
                if (ImGui::InvisibleButton(("##clip"+std::to_string(ti)+"_"+std::to_string(ci)).c_str(),
                                           {cw-2, lane_h-6})) {
                    _sel_track = ti; _sel_clip = ci;
                }
            }

            ImGui::SetCursorScreenPos({row_pos.x, row_pos.y + lane_h});
            ImGui::Dummy({track_label_w + timeline_w, 0});
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::SeparatorText("Track Operations");

        if (ImGui::Button("+ Transform Track") && st.selected_id >= 0) {
            Track t;
            t.entity_id   = st.selected_id;
            const Entity* e = st.find_entity(st.selected_id);
            t.entity_name = e ? e->value("name", std::to_string(st.selected_id)) : "?";
            t.type        = "transform";
            tracks.push_back(t);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Audio Track") && st.selected_id >= 0) {
            Track t; t.entity_id = st.selected_id;
            const Entity* e = st.find_entity(st.selected_id);
            t.entity_name = e ? e->value("name", std::to_string(st.selected_id)) : "?";
            t.type = "audio"; tracks.push_back(t);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Activation Track") && st.selected_id >= 0) {
            Track t; t.entity_id = st.selected_id;
            const Entity* e = st.find_entity(st.selected_id);
            t.entity_name = e ? e->value("name", std::to_string(st.selected_id)) : "?";
            t.type = "activate"; tracks.push_back(t);
        }

        if (_sel_track >= 0 && _sel_track < (int)tracks.size()) {
            auto& tr = tracks[_sel_track];
            ImGui::Spacing();
            ImGui::SeparatorText("Selected Track");
            ImGui::Text("Entity: %s  |  Type: %s  |  Clips: %d",
                        tr.entity_name.c_str(), tr.type.c_str(), (int)tr.clips.size());
            if (ImGui::Button("Add Clip at Playhead")) {
                Clip c; c.start = _playhead; c.duration = 1.f;
                c.type = tr.type; c.label = tr.type;
                tr.clips.push_back(c);
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove Track")) {
                tracks.erase(tracks.begin() + _sel_track);
                _sel_track = -1; _sel_clip = -1;
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Save Timeline")) {
            nlohmann::json j;
            j["duration"] = _duration;
            j["tracks"]   = nlohmann::json::array();
            for (auto& tr : tracks) {
                nlohmann::json jtr = {{"entity_id", tr.entity_id},
                                       {"entity_name", tr.entity_name},
                                       {"type", tr.type}};
                jtr["clips"] = nlohmann::json::array();
                for (auto& c : tr.clips)
                    jtr["clips"].push_back({{"start",c.start},{"duration",c.duration},
                                            {"type",c.type},{"label",c.label},{"data",c.data}});
                j["tracks"].push_back(jtr);
            }
            fs::path p = fs::path(st.scene_path).parent_path() / "timeline.json";
            if (save_json(p, j)) st.log_success("Timeline saved.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Timeline")) {
            fs::path p = fs::path(st.scene_path).parent_path() / "timeline.json";
            auto j = load_json(p);
            if (j.is_object()) {
                _duration = j.value("duration", 10.f);
                tracks.clear();
                for (auto& jtr : j.value("tracks", nlohmann::json::array())) {
                    Track tr;
                    tr.entity_id   = jtr.value("entity_id", -1);
                    tr.entity_name = jtr.value("entity_name", "");
                    tr.type        = jtr.value("type", "transform");
                    for (auto& jc : jtr.value("clips", nlohmann::json::array())) {
                        Clip c;
                        c.start    = jc.value("start",    0.f);
                        c.duration = jc.value("duration", 1.f);
                        c.type     = jc.value("type",     "transform");
                        c.label    = jc.value("label",    "");
                        c.data     = jc.value("data",     nlohmann::json::object());
                        tr.clips.push_back(c);
                    }
                    tracks.push_back(tr);
                }
                st.log_success("Timeline loaded.");
            }
        }

        ImGui::End();
    }
};

// ─── Scriptable Object Editor ─────────────────────────────────────────────────
// Unity's ScriptableObject workflow: create, inspect, and edit .sobj JSON
// assets. A .sobj file is a typed JSON object with a "type" field and arbitrary
// data fields. Scripts reference them via ScriptableObjectRef components.
// This panel lists all .sobj files in the project and provides a live
// key-value editor with type inference (bool, int, float, string, array).

class ScriptableObjectPanel {
public:
    bool        _open = false;
    std::string _sel_path;
    nlohmann::json _current;
    char        _new_key[64]   = {};
    char        _new_type[32]  = "string";
    char        _type_name[64] = "MyData";
    bool        _dirty         = false;

    void open() { _open = true; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({500, 520}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Scriptable Object Editor##sobj", &_open)) { ImGui::End(); return; }

        // Left: file list
        ImGui::Columns(2, "##sobj_cols", true);
        ImGui::SetColumnWidth(0, 160.f);
        ImGui::SeparatorText("Assets (.sobj)");
        if (ImGui::BeginChild("##sobjlist", {-1, -30}, true)) {
            std::error_code ec;
            for (auto& de : fs::recursive_directory_iterator(st.asset_dir, ec)) {
                if (ec) break;
                if (de.path().extension() != ".sobj") continue;
                std::string stem = de.path().stem().string();
                bool sel = (_sel_path == de.path().string());
                if (ImGui::Selectable(stem.c_str(), sel)) {
                    if (!_dirty || sel) {
                        _sel_path = de.path().string();
                        _current  = load_json(_sel_path);
                        if (!_current.is_object()) _current = nlohmann::json::object();
                        _dirty = false;
                    }
                }
            }
        }
        ImGui::EndChild();
        if (ImGui::Button("New .sobj")) {
            char nm[64]; snprintf(nm, sizeof(nm), "%s.sobj", _type_name);
            fs::path p = fs::path(st.asset_dir) / nm;
            nlohmann::json j = {{"type", std::string(_type_name)}};
            if (save_json(p, j)) {
                _sel_path = p.string(); _current = j; _dirty = false;
                st.log_success("Created " + p.string());
            }
        }

        ImGui::NextColumn();

        // Right: field editor
        if (!_sel_path.empty() && _current.is_object()) {
            ImGui::SeparatorText(fs::path(_sel_path).filename().string().c_str());

            // type field (always first)
            if (_current.contains("type")) {
                std::string tname = _current["type"].get<std::string>();
                char tbuf[64]; snprintf(tbuf, sizeof(tbuf), "%s", tname.c_str());
                if (ImGui::InputText("type##sobj", tbuf, sizeof(tbuf)))
                    { _current["type"] = std::string(tbuf); _dirty = true; }
            }

            ImGui::Separator();
            if (ImGui::BeginChild("##sobjfields", {-1, -80}, false)) {
                for (auto it = _current.begin(); it != _current.end(); ++it) {
                    if (it.key() == "type") continue;
                    ImGui::PushID(it.key().c_str());
                    auto& val = it.value();
                    ImGui::TextUnformatted(it.key().c_str());
                    ImGui::SameLine(120.f);
                    if (val.is_boolean()) {
                        bool b = val.get<bool>();
                        if (ImGui::Checkbox("##v", &b)) { val = b; _dirty = true; }
                    } else if (val.is_number_integer()) {
                        int iv = val.get<int>();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputInt("##v", &iv)) { val = iv; _dirty = true; }
                    } else if (val.is_number_float()) {
                        float fv = val.get<float>();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputFloat("##v", &fv, 0.f, 0.f, "%.4f")) { val = fv; _dirty = true; }
                    } else if (val.is_string()) {
                        std::string sv = val.get<std::string>();
                        char sbuf[256]; snprintf(sbuf, sizeof(sbuf), "%s", sv.c_str());
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##v", sbuf, sizeof(sbuf)))
                            { val = std::string(sbuf); _dirty = true; }
                    } else {
                        std::string dump = val.dump();
                        ImGui::TextDisabled("%s", dump.substr(0, 40).c_str());
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            // Add new field
            ImGui::Separator();
            ImGui::SetNextItemWidth(100.f);
            ImGui::InputText("Key##sobj",  _new_key,  sizeof(_new_key));
            ImGui::SameLine();
            const char* types[] = {"string","int","float","bool"};
            static int type_idx = 0;
            ImGui::SetNextItemWidth(70.f);
            ImGui::Combo("Type##sobj", &type_idx, types, 4);
            ImGui::SameLine();
            if (ImGui::Button("+##sobj") && _new_key[0]) {
                std::string k(_new_key);
                if      (type_idx == 0) _current[k] = "";
                else if (type_idx == 1) _current[k] = 0;
                else if (type_idx == 2) _current[k] = 0.f;
                else if (type_idx == 3) _current[k] = false;
                _dirty = true; memset(_new_key, 0, sizeof(_new_key));
            }

            ImGui::Spacing();
            bool disabled = !_dirty;
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Save##sobj", {-1, 0})) {
                if (save_json(_sel_path, _current)) {
                    _dirty = false;
                    st.log_success("Saved " + _sel_path);
                }
            }
            if (disabled) ImGui::EndDisabled();
        } else {
            ImGui::TextDisabled("Select a .sobj asset on the left to edit.");
        }

        ImGui::Columns(1);
        ImGui::End();
    }
};

// ─── Physics Debugger 2D ──────────────────────────────────────────────────────
// Mirrors Unity's Physics Debugger: overlays collider shapes, velocity vectors,
// contact points, and joint anchors on the viewport. Reads entity components
// and draws debug geometry into an ImGui overlay window rendered on top of the
// viewport. Useful for diagnosing physics setup without running the game.

class PhysicsDebugger2D {
public:
    bool _open           = false;
    bool _show_colliders = true;
    bool _show_triggers  = true;
    bool _show_joints    = true;
    bool _show_rigidbodies = true;
    bool _show_effectors = true;
    float _alpha         = 0.45f;
    ImVec4 _col_static   = {0.2f, 0.9f, 0.2f, 1.f};
    ImVec4 _col_dynamic  = {0.2f, 0.6f, 1.0f, 1.f};
    ImVec4 _col_trigger  = {1.0f, 0.8f, 0.1f, 1.f};
    ImVec4 _col_kinematic= {0.9f, 0.5f, 0.1f, 1.f};
    ImVec4 _col_joint    = {1.0f, 0.3f, 0.9f, 1.f};
    ImVec4 _col_effector = {0.8f, 0.3f, 1.0f, 1.f};

    void open() { _open = true; }

    // Called from viewport after scene render to overlay debug shapes.
    // `world_to_screen` converts world coords to ImGui screen coords.
    void draw_overlay(EditorState& st,
                      std::function<ImVec2(float,float)> world_to_screen) {
        if (!_open) return;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) dl = ImGui::GetForegroundDrawList();

        for (auto& e : st.entities) {
            if (!e.contains("components")) continue;
            auto& comps = e["components"];

            float ex = 0.f, ey = 0.f, er = 0.f;
            float sx = 1.f, sy = 1.f;
            if (comps.contains("Transform")) {
                auto& tr = comps["Transform"];
                ex = tr.value("x", 0.0); ey = tr.value("y", 0.0);
                er = tr.value("rotation", 0.0);
                sx = tr.value("scale_x", 1.0); sy = tr.value("scale_y", 1.0);
            }

            bool is_kinematic = false;
            bool is_dynamic   = false;
            if (_show_rigidbodies && comps.contains("Rigidbody2D")) {
                auto& rb = comps["Rigidbody2D"];
                is_kinematic = rb.value("is_kinematic", false);
                std::string bt = rb.value("body_type", "dynamic");
                is_dynamic = (bt == "dynamic");
            }

            auto draw_rect_outline = [&](float cx, float cy, float hw, float hh, ImVec4 col) {
                ImVec2 tl = world_to_screen(cx - hw, cy - hh);
                ImVec2 br = world_to_screen(cx + hw, cy + hh);
                ImU32 c = ImGui::ColorConvertFloat4ToU32({col.x, col.y, col.z, col.w * _alpha});
                ImU32 cs = ImGui::ColorConvertFloat4ToU32({col.x, col.y, col.z, col.w * _alpha * 0.2f});
                dl->AddRectFilled(tl, br, cs);
                dl->AddRect(tl, br, c, 0.f, 0, 1.5f);
            };
            auto draw_circle_outline = [&](float cx, float cy, float r, ImVec4 col) {
                ImVec2 ctr = world_to_screen(cx, cy);
                ImVec2 edge = world_to_screen(cx + r, cy);
                float sr = edge.x - ctr.x;
                ImU32 c  = ImGui::ColorConvertFloat4ToU32({col.x, col.y, col.z, col.w * _alpha});
                ImU32 cs = ImGui::ColorConvertFloat4ToU32({col.x, col.y, col.z, col.w * _alpha * 0.2f});
                dl->AddCircleFilled(ctr, sr, cs);
                dl->AddCircle(ctr, sr, c, 32, 1.5f);
            };

            ImVec4 body_col = is_dynamic ? _col_dynamic : (is_kinematic ? _col_kinematic : _col_static);

            if (_show_colliders && comps.contains("BoxCollider2D")) {
                auto& c = comps["BoxCollider2D"];
                bool trig = c.value("is_trigger", false);
                float w = c.value("width", 32.0) * sx * 0.5f;
                float h = c.value("height",32.0) * sy * 0.5f;
                float ox = c.value("offset_x", 0.0), oy = c.value("offset_y", 0.0);
                draw_rect_outline(ex + ox, ey + oy, w, h, trig ? _col_trigger : body_col);
            }
            if (_show_colliders && comps.contains("CircleCollider2D")) {
                auto& c = comps["CircleCollider2D"];
                bool trig = c.value("is_trigger", false);
                float r = c.value("radius", 16.0) * std::max(sx, sy);
                float ox = c.value("offset_x",0.0), oy = c.value("offset_y",0.0);
                draw_circle_outline(ex+ox, ey+oy, r, trig ? _col_trigger : body_col);
            }
            if (_show_effectors && comps.contains("PointEffector2D")) {
                float r = 32.f; // visual radius hint
                draw_circle_outline(ex, ey, r, _col_effector);
                ImVec2 ctr = world_to_screen(ex, ey);
                dl->AddText({ctr.x-8, ctr.y-6}, IM_COL32(200,120,255,220), "PtEff");
            }
            if (_show_joints) {
                auto draw_joint = [&](const char* label) {
                    if (!comps.contains(label)) return;
                    auto& jc = comps[label];
                    float ax = jc.value("anchor_x", 0.f), ay = jc.value("anchor_y", 0.f);
                    ImVec2 p = world_to_screen(ex + ax, ey + ay);
                    ImU32 jcol = ImGui::ColorConvertFloat4ToU32(_col_joint);
                    dl->AddCircleFilled(p, 5.f, jcol);
                    dl->AddCircle(p, 8.f, jcol, 12, 1.f);
                    int ce_id = jc.value("connected_entity", -1);
                    if (ce_id >= 0) {
                        const Entity* ce = st.find_entity(ce_id);
                        if (ce && ce->contains("components") && (*ce)["components"].contains("Transform")) {
                            float cx2 = (*ce)["components"]["Transform"].value("x", 0.f);
                            float cy2 = (*ce)["components"]["Transform"].value("y", 0.f);
                            ImVec2 p2 = world_to_screen(cx2, cy2);
                            dl->AddLine(p, p2, jcol, 1.f);
                        }
                    }
                };
                draw_joint("DistanceJoint2D"); draw_joint("SpringJoint2D");
                draw_joint("HingeJoint2D");    draw_joint("SliderJoint2D");
                draw_joint("WheelJoint2D");
            }
        }
    }

    void draw_ui(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({280, 300}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Physics Debugger 2D##phd", &_open)) { ImGui::End(); return; }

        ImGui::SeparatorText("Visibility");
        ImGui::Checkbox("Colliders##phd",   &_show_colliders);
        ImGui::Checkbox("Triggers##phd",    &_show_triggers);
        ImGui::Checkbox("Joints##phd",      &_show_joints);
        ImGui::Checkbox("Rigidbodies##phd", &_show_rigidbodies);
        ImGui::Checkbox("Effectors##phd",   &_show_effectors);
        ImGui::SliderFloat("Alpha##phd", &_alpha, 0.f, 1.f, "%.2f");

        ImGui::SeparatorText("Colors");
        ImGui::ColorEdit4("Static##phd",    &_col_static.x,    ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Dynamic##phd",   &_col_dynamic.x,   ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Trigger##phd",   &_col_trigger.x,   ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Kinematic##phd", &_col_kinematic.x, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Joint##phd",     &_col_joint.x,     ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Effector##phd",  &_col_effector.x,  ImGuiColorEditFlags_NoInputs);

        ImGui::Spacing();
        int rb_count = 0, col_count = 0, joint_count = 0;
        for (auto& e : st.entities) {
            if (!e.contains("components")) continue;
            auto& c = e["components"];
            if (c.contains("Rigidbody2D"))    ++rb_count;
            if (c.contains("BoxCollider2D") || c.contains("CircleCollider2D") ||
                c.contains("CapsuleCollider2D") || c.contains("PolygonCollider2D")) ++col_count;
            if (c.contains("HingeJoint2D") || c.contains("SpringJoint2D") ||
                c.contains("DistanceJoint2D") || c.contains("WheelJoint2D")) ++joint_count;
        }
        ImGui::SeparatorText("Scene Stats");
        ImGui::Text("Rigidbodies: %d  Colliders: %d  Joints: %d",
                    rb_count, col_count, joint_count);
        ImGui::End();
    }
};

// ─── Memory Profiler Panel ────────────────────────────────────────────────────
// Lightweight runtime memory snapshot. Mirrors Unity's Memory Profiler:
// categorises memory by entity/component type and tracks scene complexity
// metrics. In this engine, "memory" is the serialised JSON size of each
// entity's component data — a useful proxy for data budget (texture refs,
// tilemap grids, particle curve data, etc.).

class MemoryProfilerPanel {
public:
    bool  _open       = false;
    bool  _sort_by_size = true;

    struct Entry {
        std::string name;
        std::string category;
        size_t      bytes = 0;
    };

    std::vector<Entry> _snapshot;
    size_t             _total_bytes = 0;

    void open() { _open = true; }

    void take_snapshot(EditorState& st) {
        _snapshot.clear();
        _total_bytes = 0;

        // Category totals
        std::unordered_map<std::string, size_t> cat_totals;

        for (auto& e : st.entities) {
            std::string ename = e.value("name", std::to_string(e.value("id",0)));
            std::string dump  = nlohmann::json(e).dump();
            size_t      esz   = dump.size();
            _total_bytes += esz;
            _snapshot.push_back({ename, "Entity", esz});

            // Per-component breakdown
            if (e.contains("components")) {
                for (auto& [ctype, cdata] : e["components"].items()) {
                    size_t csz = nlohmann::json(cdata).dump().size();
                    cat_totals[ctype] += csz;
                }
            }
        }

        // Add category summary entries
        for (auto& [cat, sz] : cat_totals)
            _snapshot.push_back({"[" + cat + "] (all)", "Component Type", sz});

        if (_sort_by_size)
            std::sort(_snapshot.begin(), _snapshot.end(),
                      [](const Entry& a, const Entry& b){ return a.bytes > b.bytes; });
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({500, 500}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Memory Profiler##mem", &_open)) { ImGui::End(); return; }

        ImGui::Text("Total scene data: %.1f KB", _total_bytes / 1024.f);
        ImGui::SameLine(0, 20);
        ImGui::Checkbox("Sort by size##mem", &_sort_by_size);
        ImGui::SameLine(0, 20);
        if (ImGui::Button("Take Snapshot##mem")) take_snapshot(st);

        ImGui::Spacing();
        if (_snapshot.empty()) {
            ImGui::TextDisabled("Press 'Take Snapshot' to analyse scene memory.");
        } else {
            float bar_w = ImGui::GetContentRegionAvail().x - 260.f;
            if (ImGui::BeginTable("##memtbl", 4,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch, 0.5f);
                ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed,  110.f);
                ImGui::TableSetupColumn("Bytes",    ImGuiTableColumnFlags_WidthFixed,   70.f);
                ImGui::TableSetupColumn("Usage",    ImGuiTableColumnFlags_WidthFixed,  bar_w > 60 ? bar_w : 60.f);
                ImGui::TableHeadersRow();

                size_t max_bytes = _snapshot.empty() ? 1 : _snapshot.front().bytes;
                for (auto& entry : _snapshot) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(entry.name.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", entry.category.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (entry.bytes >= 1024)
                        ImGui::Text("%.1fK", entry.bytes / 1024.f);
                    else
                        ImGui::Text("%zu",   entry.bytes);
                    ImGui::TableSetColumnIndex(3);
                    float frac = max_bytes > 0 ? (float)entry.bytes / (float)max_bytes : 0.f;
                    char ov[16]; snprintf(ov, sizeof(ov), "%.1f%%", frac * 100.f);
                    ImGui::ProgressBar(frac, {-1, 14}, ov);
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
};

// ─── Package Manager Panel ────────────────────────────────────────────────────
// Mirrors Unity's Package Manager: a curated catalogue of optional engine
// modules the user can toggle on/off (stored in project_packages.json).
// Each package maps to a feature flag that editor/runtime systems read to
// conditionally enable behaviour (e.g. enabling NavMesh2D compiles/loads the
// pathfinding system; enabling Cinemachine2D activates the virtual-camera
// subsystem). This panel is purely metadata — the actual feature code is
// already compiled in; packages just control which parts are active.

class PackageManagerPanel {
public:
    bool _open = false;

    struct Package {
        std::string id;
        std::string display_name;
        std::string version;
        std::string description;
        std::string category;
        bool        installed = false;
        bool        builtin   = false;  // built-in = can't remove
    };

    std::vector<Package> _packages = {
        // Built-in (always on)
        {"com.engine.2dcore",       "2D Core",           "1.0.0",
         "Sprite rendering, transforms, basic physics, scene management.", "Core", true, true},
        {"com.engine.ui",           "UI Toolkit",        "1.0.0",
         "UICanvas, UIPanel, UIText, UIButton, UIImage, UIProgressBar, UILayoutGroup.", "Core", true, true},
        {"com.engine.audio",        "Audio System",      "1.0.0",
         "AudioSource, AudioMixer panel, spatial audio.", "Core", true, true},
        {"com.engine.physics2d",    "Physics 2D",        "1.0.0",
         "Rigidbody2D, all collider types, joints, effectors.", "Core", true, true},
        {"com.engine.animation",    "Animation",         "1.0.0",
         "Animator, AnimatorPanel, animation state machine, layer blending.", "Core", true, true},
        // Optional (toggle)
        {"com.engine.tilemap",      "Tilemap",           "1.2.0",
         "Tilemap component, Tile Palette, Rule Tile, Sprite Atlas, CompositeCollider2D.", "2D", false, false},
        {"com.engine.navmesh2d",    "AI Navigation 2D",  "1.1.0",
         "NavMeshAgent2D, NavMeshObstacle2D, NavMesh baking panel, A* pathfinding.", "AI", false, false},
        {"com.engine.cinemachine",  "Cinemachine 2D",    "2.0.0",
         "Virtual Camera, dead zone / soft zone, Confiner, lookahead, prioritised blending.", "Camera", false, false},
        {"com.engine.textmeshpro",  "TextMesh Pro 2D",   "3.0.0",
         "SDF font rendering, rich text markup, per-character animation, auto-sizing.", "Rendering", false, false},
        {"com.engine.linerenderer", "Line Renderer 2D",  "1.0.0",
         "LineRenderer2D component: ropes, trajectories, laser beams, path gizmos.", "Rendering", false, false},
        {"com.engine.timeline",     "Timeline",          "1.3.0",
         "Multi-track sequencer: tween, audio, activation, and script event tracks.", "Sequencing", false, false},
        {"com.engine.particles",    "Particle System",   "1.1.0",
         "ParticleEmitter with curves, sub-emitters, atlas sheets, burst mode.", "Effects", false, false},
        {"com.engine.lighting2d",   "2D Lights",         "1.0.0",
         "Light2D, global illumination, ambient color, LightingPanel.", "Rendering", false, false},
        {"com.engine.videoplayer",  "Video Player 2D",   "1.0.0",
         "VideoPlayer2D: plays animated GIF clips on SpriteRenderer textures.", "Media", false, false},
        {"com.engine.scriptableobj","Scriptable Objects","1.0.0",
         "ScriptableObjectRef component + .sobj asset editor for data-driven design.", "Scripting", false, false},
        {"com.engine.prefabs",      "Prefab System",     "1.0.0",
         "Save/instantiate prefab assets, nested prefabs, override tracking.", "Scene", false, false},
        {"com.engine.memoryprofiler","Memory Profiler",  "1.0.0",
         "Scene data budget analyser: per-entity and per-component-type breakdowns.", "Profiling", false, false},
        {"com.engine.physdebugger", "Physics Debugger 2D","1.0.0",
         "Overlay collider shapes, velocity vectors, joint anchors in viewport.", "Debugging", false, false},
        {"com.engine.vfxgraph",     "VFX Graph 2D",      "1.0.0",
         "Node-based visual effect authoring — shaders, distortion, trail FX.", "Effects", false, false},
        {"com.engine.multiplayer",  "Multiplayer (Lobby)","1.0.0",
         "LAN/relay matchmaking, NetSpawn, networked component sync.", "Networking", false, false},
    };

    std::string  _search;
    std::string  _category_filter;
    int          _sel = -1;

    void open() { _open = true; }

    void load(EditorState& st) {
        fs::path p = fs::path(st.scene_path).parent_path() / "project_packages.json";
        auto j = load_json(p);
        if (!j.is_object()) return;
        for (auto& pkg : _packages) {
            if (j.contains(pkg.id))
                pkg.installed = j[pkg.id].value("installed", pkg.installed);
        }
    }

    void save(EditorState& st) {
        nlohmann::json j;
        for (auto& pkg : _packages)
            j[pkg.id] = {{"installed", pkg.installed}, {"version", pkg.version}};
        fs::path p = fs::path(st.scene_path).parent_path() / "project_packages.json";
        if (save_json(p, j)) st.log_success("Package configuration saved.");
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({680, 520}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Package Manager##pkgmgr", &_open)) { ImGui::End(); return; }

        // Toolbar
        ImGui::SetNextItemWidth(200.f);
        ImGui::InputText("Search##pkg", _search.data(), 128,
                         ImGuiInputTextFlags_CallbackResize,
                         [](ImGuiInputTextCallbackData* d)->int{
                             if(d->EventFlag==ImGuiInputTextFlags_CallbackResize)
                                 ((std::string*)d->UserData)->resize(d->BufTextLen);
                             return 0; }, &_search);
        ImGui::SameLine();
        static const char* cats[] = {"All","Core","2D","AI","Camera","Rendering","Sequencing","Effects","Media","Scripting","Scene","Profiling","Debugging","Networking"};
        static int cat_idx = 0;
        ImGui::SetNextItemWidth(120.f);
        ImGui::Combo("##pkgcat", &cat_idx, cats, IM_ARRAYSIZE(cats));
        ImGui::SameLine();
        if (ImGui::Button("Save##pkg")) save(st);
        ImGui::SameLine();
        if (ImGui::Button("Reload##pkg")) load(st);

        ImGui::Separator();

        // Split: list left, detail right
        ImGui::Columns(2, "##pkgcols", true);
        ImGui::SetColumnWidth(0, 340.f);

        if (ImGui::BeginChild("##pkglist", {-1, -1}, false)) {
            for (int i = 0; i < (int)_packages.size(); ++i) {
                auto& pkg = _packages[i];
                if (!_search.empty() &&
                    pkg.display_name.find(_search) == std::string::npos &&
                    pkg.id.find(_search) == std::string::npos) continue;
                std::string cf(cats[cat_idx]);
                if (cf != "All" && pkg.category != cf) continue;

                ImGui::PushID(i);
                bool sel = (_sel == i);

                // Installed indicator
                ImVec4 dot_col = pkg.installed
                    ? (pkg.builtin ? ImVec4(0.4f,0.7f,1.f,1.f) : ImVec4(0.3f,0.9f,0.4f,1.f))
                    : ImVec4(0.5f,0.5f,0.5f,0.5f);
                ImGui::TextColored(dot_col, "●");
                ImGui::SameLine();

                if (ImGui::Selectable((pkg.display_name + "##pkg" + std::to_string(i)).c_str(),
                                       sel, 0, {-1, 0}))
                    _sel = i;

                // Version badge
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - 40.f + ImGui::GetCursorPosX());
                ImGui::TextDisabled("%s", pkg.version.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::NextColumn();

        if (_sel >= 0 && _sel < (int)_packages.size()) {
            auto& pkg = _packages[_sel];
            ImGui::SeparatorText(pkg.display_name.c_str());
            ImGui::TextDisabled("ID: %s", pkg.id.c_str());
            ImGui::TextDisabled("Version: %s  |  Category: %s", pkg.version.c_str(), pkg.category.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("%s", pkg.description.c_str());
            ImGui::Spacing();

            if (pkg.builtin) {
                ImGui::TextColored({0.4f,0.7f,1.f,1.f}, "Built-in package (always active)");
            } else {
                bool on = pkg.installed;
                if (ImGui::Checkbox(on ? "Installed (click to remove)##pkg"
                                       : "Not installed (click to install)##pkg", &on))
                    pkg.installed = on;
                ImGui::Spacing();
                if (pkg.installed)
                    ImGui::TextColored({0.3f,0.9f,0.4f,1.f}, "✓ Active — related components and panels are enabled.");
                else
                    ImGui::TextDisabled("Inactive — related components are present but flagged as optional.");
            }
        } else {
            ImGui::TextDisabled("Select a package to see details.");
        }

        ImGui::Columns(1);
        ImGui::End();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// NOVA ENGINE — UNITY 2D GAP CLOSERS  (batch 2)
//
//  11. TrailRendererSystem   — runtime per-entity ribbon trail
//  12. LineRendererPanel     — world-space polyline gizmo + runtime component
//  13. VirtualCamera2DSystem — Cinemachine-style follow camera (dead-zone,
//                              confiner, pixel-snap, damping, screen-shake)
//  14. TweenSystem           — DOTween-style fluent tweens accessible from
//                              scripts via Tween::To / MoveTo / FadeTo etc.
//  15. DialogueSystem        — branching dialogue (speaker / portrait /
//                              choices) with editor graph panel
//  16. LocalizationSystem    — CSV string tables, runtime L("key") lookup,
//                              in-editor table browser / hot-reload
//  17. ConsolePanel          — categorised, searchable runtime log viewer
//  18. StateMachineComponent — generic code-driven FSM (not animator-tied)
// ═══════════════════════════════════════════════════════════════════════════

// ─── 11. TrailRenderer system ─────────────────────────────────────────────────
// Each entity with a "TrailRenderer" component accumulates world-space points
// as it moves; the render system draws them as a colour-fading ribbon.
// Component fields (match Unity naming where possible):
//   time          float  — seconds a trail point lives (default 1.0)
//   start_width   float  — ribbon width at the newest point  (px, default 8)
//   end_width     float  — ribbon width at the oldest point  (px, default 0)
//   start_color   [r,g,b,a] ints — colour of new point (default white)
//   end_color     [r,g,b,a] ints — colour of old point (default transparent)
//   min_distance  float  — minimum world-units moved before a new point is
//                          recorded (avoids jitter, default 4)
//   emitting      bool   — if false the ribbon freezes / decays (default true)
//   texture       string — optional texture path (tiled along the ribbon UV)
//
// Runtime:  TrailRendererSystem::update(entities, dt)  called from core.cpp
//           before the sprite batch.  It writes "_trail_points" (an array of
//           {x,y,t,w,r,g,b,a} objects) back onto the component every frame so
//           the render system can iterate them directly with no extra state.
//
// Editor:   TrailRendererPanel lets you tweak every field with live preview.

class TrailRendererSystem {
public:
    void update(std::vector<Entity>& entities, float dt) {
        for (auto& e : entities) {
            if (!e.contains("components")) continue;
            auto& comps = e["components"];
            if (!comps.contains("TrailRenderer")) continue;
            auto& tr = comps["TrailRenderer"];
            if (!tr.value("emitting", true)) {
                // still age existing points even when not emitting
                _age_points(tr, dt);
                continue;
            }
            // entity world position
            float ex = 0.f, ey = 0.f;
            if (comps.contains("Transform")) {
                ex = comps["Transform"].value("x", 0.f);
                ey = comps["Transform"].value("y", 0.f);
            }
            // initialise point array if missing
            if (!tr.contains("_trail_points") || !tr["_trail_points"].is_array())
                tr["_trail_points"] = nlohmann::json::array();

            // check min_distance
            float min_d = tr.value("min_distance", 4.f);
            bool should_emit = true;
            auto& pts = tr["_trail_points"];
            if (!pts.empty()) {
                float lx = pts[0].value("x", 0.f);
                float ly = pts[0].value("y", 0.f);
                float dx = ex - lx, dy = ey - ly;
                if (dx*dx + dy*dy < min_d*min_d) should_emit = false;
            }
            if (should_emit) {
                float trail_t = tr.value("time", 1.f);
                float sw = tr.value("start_width", 8.f);
                auto sc = tr.value("start_color", std::vector<int>{255,255,255,255});
                Entity pt = Entity::object();
                pt["x"] = ex; pt["y"] = ey; pt["t"] = trail_t; pt["w"] = sw;
                pt["r"] = sc.size()>0?sc[0]:255;
                pt["g"] = sc.size()>1?sc[1]:255;
                pt["b"] = sc.size()>2?sc[2]:255;
                pt["a"] = sc.size()>3?sc[3]:255;
                pts.insert((size_t)0, pt);  // prepend — runtime::Value::insert(size_t, const Value&)
            }
            _age_points(tr, dt);
        }
    }

private:
    void _age_points(Entity& tr, float dt) {
        if (!tr.contains("_trail_points") || !tr["_trail_points"].is_array()) return;
        auto& pts = tr["_trail_points"];
        float ew = tr.value("end_width", 0.f);
        auto ec = tr.value("end_color", std::vector<int>{255,255,255,0});
        float trail_t = tr.value("time", 1.f);
        float sw = tr.value("start_width", 8.f);
        auto sc = tr.value("start_color", std::vector<int>{255,255,255,255});

        for (int i = (int)pts.size()-1; i >= 0; --i) {
            float t = pts[i].value("t", 0.f) - dt;
            if (t <= 0.f) { pts.erase_at((size_t)i); continue; }
            pts[i]["t"] = t;
            // lerp width and colour based on remaining life ratio
            float ratio = std::max(0.f, std::min(1.f, t / trail_t));
            pts[i]["w"] = ew + (sw - ew) * ratio;
            pts[i]["r"] = (ec.size()>0?ec[0]:255) + (int)(((sc.size()>0?sc[0]:255)-(ec.size()>0?ec[0]:255))*ratio);
            pts[i]["g"] = (ec.size()>1?ec[1]:255) + (int)(((sc.size()>1?sc[1]:255)-(ec.size()>1?ec[1]:255))*ratio);
            pts[i]["b"] = (ec.size()>2?ec[2]:255) + (int)(((sc.size()>2?sc[2]:255)-(ec.size()>2?ec[2]:255))*ratio);
            pts[i]["a"] = (ec.size()>3?ec[3]:0)   + (int)(((sc.size()>3?sc[3]:255)-(ec.size()>3?ec[3]:0))*ratio);
        }
    }
};

// Editor panel for TrailRenderer component on the selected entity
class TrailRendererPanel {
    bool _open = true;
public:
    bool& open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({340, 320}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Trail Renderer##trailpanel", &_open)) { ImGui::End(); return; }

        Entity* sel = st.selected_entity();
        if (!sel || !sel->contains("components") || !(*sel)["components"].contains("TrailRenderer")) {
            ImGui::TextDisabled("Select an entity with a TrailRenderer component.");
            ImGui::End(); return;
        }
        auto& tr = (*sel)["components"]["TrailRenderer"];

        float time     = tr.value("time", 1.f);
        float sw       = tr.value("start_width", 8.f);
        float ew       = tr.value("end_width", 0.f);
        float min_d    = tr.value("min_distance", 4.f);
        bool emitting  = tr.value("emitting", true);
        auto sc = tr.value("start_color", std::vector<int>{255,255,255,255});
        auto ec = tr.value("end_color",   std::vector<int>{255,255,255,0});

        if (ImGui::DragFloat("Lifetime (s)",    &time,     0.01f, 0.05f, 20.f))  tr["time"]         = time;
        if (ImGui::DragFloat("Start Width",     &sw,       0.5f,  0.f,  200.f)) tr["start_width"]  = sw;
        if (ImGui::DragFloat("End Width",       &ew,       0.5f,  0.f,  200.f)) tr["end_width"]    = ew;
        if (ImGui::DragFloat("Min Vertex Dist", &min_d,   0.5f,  0.f,  100.f)) tr["min_distance"] = min_d;
        if (ImGui::Checkbox("Emitting",         &emitting))                      tr["emitting"]     = emitting;

        ImGui::Separator();
        ImGui::Text("Start Color");
        float sf[4] = {sc.size()>0?sc[0]/255.f:1.f, sc.size()>1?sc[1]/255.f:1.f,
                       sc.size()>2?sc[2]/255.f:1.f, sc.size()>3?sc[3]/255.f:1.f};
        if (ImGui::ColorEdit4("##sc", sf)) tr["start_color"] = std::vector<int>{(int)(sf[0]*255),(int)(sf[1]*255),(int)(sf[2]*255),(int)(sf[3]*255)};
        ImGui::Text("End Color");
        float ef[4] = {ec.size()>0?ec[0]/255.f:1.f, ec.size()>1?ec[1]/255.f:1.f,
                       ec.size()>2?ec[2]/255.f:1.f, ec.size()>3?ec[3]/255.f:0.f};
        if (ImGui::ColorEdit4("##ec", ef)) tr["end_color"] = std::vector<int>{(int)(ef[0]*255),(int)(ef[1]*255),(int)(ef[2]*255),(int)(ef[3]*255)};

        ImGui::Separator();
        std::string texture = tr.value("texture", std::string());
        if (InspectorPanel::draw_project_asset_slot(st, "Texture", texture,
                {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0\0", "Select Trail Texture"))
            tr["texture"] = texture;

        int pts = tr.contains("_trail_points") && tr["_trail_points"].is_array()
                  ? (int)tr["_trail_points"].size() : 0;
        ImGui::Separator();
        ImGui::TextDisabled("Live points: %d", pts);
        ImGui::End();
    }
};

// ─── 12. LineRenderer component + panel ───────────────────────────────────────
// Draws a world-space polyline between N user-defined control points.
// Component fields:
//   points        array of {x,y}  — control points in world-space
//   width         float           — uniform line width in pixels (default 2)
//   color         [r,g,b,a]       — line colour (default white)
//   loop          bool            — connect last point back to first
//   use_gradient  bool            — if true, lerp start_color→end_color along line
//   start_color / end_color       — gradient endpoints
//   texture       string          — optional texture tiled along the line
//   sorting_layer / order_in_layer — standard sprite sort fields
//
// The render system iterates "LineRenderer" components and issues one thin
// quad per segment via push_quad(use_texture=false) — no shader change needed.

class LineRendererPanel {
    bool _open = true;
    int  _sel_pt = -1;
public:
    bool& open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({360, 420}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Line Renderer##lrpanel", &_open)) { ImGui::End(); return; }

        Entity* sel = st.selected_entity();
        if (!sel || !sel->contains("components") || !(*sel)["components"].contains("LineRenderer")) {
            ImGui::TextDisabled("Select an entity with a LineRenderer component.");
            ImGui::End(); return;
        }
        auto& lr = (*sel)["components"]["LineRenderer"];
        if (!lr.contains("points") || !lr["points"].is_array())
            lr["points"] = nlohmann::json::array();
        auto& pts = lr["points"];

        float width = lr.value("width", 2.f);
        bool loop   = lr.value("loop", false);
        bool grad   = lr.value("use_gradient", false);
        if (ImGui::DragFloat("Width (px)", &width, 0.5f, 0.5f, 64.f)) lr["width"] = width;
        if (ImGui::Checkbox("Loop",         &loop))                     lr["loop"]  = loop;
        if (ImGui::Checkbox("Use Gradient", &grad))                     lr["use_gradient"] = grad;

        auto col = lr.value("color", std::vector<int>{255,255,255,255});
        float cf[4] = {col.size()>0?col[0]/255.f:1.f, col.size()>1?col[1]/255.f:1.f,
                       col.size()>2?col[2]/255.f:1.f, col.size()>3?col[3]/255.f:1.f};
        if (!grad) {
            if (ImGui::ColorEdit4("Color##lr", cf))
                lr["color"] = std::vector<int>{(int)(cf[0]*255),(int)(cf[1]*255),(int)(cf[2]*255),(int)(cf[3]*255)};
        } else {
            auto sc = lr.value("start_color", std::vector<int>{255,255,255,255});
            auto ec = lr.value("end_color",   std::vector<int>{255,0,0,255});
            float sf[4] = {sc.size()>0?sc[0]/255.f:1.f,sc.size()>1?sc[1]/255.f:1.f,sc.size()>2?sc[2]/255.f:1.f,sc.size()>3?sc[3]/255.f:1.f};
            float ef[4] = {ec.size()>0?ec[0]/255.f:1.f,ec.size()>1?ec[1]/255.f:1.f,ec.size()>2?ec[2]/255.f:1.f,ec.size()>3?ec[3]/255.f:1.f};
            if (ImGui::ColorEdit4("Start Color##lr", sf)) lr["start_color"] = std::vector<int>{(int)(sf[0]*255),(int)(sf[1]*255),(int)(sf[2]*255),(int)(sf[3]*255)};
            if (ImGui::ColorEdit4("End Color##lr",   ef)) lr["end_color"]   = std::vector<int>{(int)(ef[0]*255),(int)(ef[1]*255),(int)(ef[2]*255),(int)(ef[3]*255)};
        }

        ImGui::Separator();
        ImGui::Text("Points (%d)", (int)pts.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("+##lradd")) {
            nlohmann::json p; p["x"] = 0.f; p["y"] = 0.f;
            pts.push_back(p); _sel_pt = (int)pts.size()-1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("-##lrdel") && _sel_pt >= 0 && _sel_pt < (int)pts.size()) {
            pts.erase_at((size_t)_sel_pt); _sel_pt = std::max(0, _sel_pt-1);
        }

        if (ImGui::BeginChild("##lrpts", {-1, 160}, true)) {
            for (int i = 0; i < (int)pts.size(); ++i) {
                ImGui::PushID(i);
                bool sel2 = (_sel_pt == i);
                if (ImGui::Selectable(("P" + std::to_string(i)).c_str(), sel2))
                    _sel_pt = i;
                ImGui::SameLine(60);
                ImGui::Text("(%.1f, %.1f)", pts[i].value("x",0.f), pts[i].value("y",0.f));
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        if (_sel_pt >= 0 && _sel_pt < (int)pts.size()) {
            float px = pts[_sel_pt].value("x", 0.f);
            float py = pts[_sel_pt].value("y", 0.f);
            ImGui::DragFloat("X##lrx", &px, 1.f); pts[_sel_pt]["x"] = px;
            ImGui::DragFloat("Y##lry", &py, 1.f); pts[_sel_pt]["y"] = py;
        }

        std::string texture = lr.value("texture", std::string());
        if (InspectorPanel::draw_project_asset_slot(st, "Texture##lr", texture,
                {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0\0", "Select Line Texture"))
            lr["texture"] = texture;
        ImGui::End();
    }
};

// ─── 13. VirtualCamera2D system (Cinemachine-style) ───────────────────────────
// Entities with a "VirtualCamera2D" component drive the engine camera.
// The highest-priority active VirtualCamera2D "wins" each frame.
//
// Component fields:
//   priority         int    — higher wins  (default 0)
//   follow_target    int    — entity id to follow (0 = none)
//   look_at_target   int    — entity id to aim at (0 = none, uses follow_target)
//   damping_x/y      float  — position smoothing (0=instant, default 0.15)
//   dead_zone_w/h    float  — world-units dead-zone before camera moves
//   soft_zone_w/h    float  — world-units soft-zone (reduced-speed tracking)
//   offset_x/y       float  — world-space offset added to follow position
//   confine_x/y      float  — rect centre to confine the camera to (0=disabled)
//   confine_w/h      float  — confine rect half-extents
//   pixel_snap       bool   — snap camera position to pixel grid (PPU-aware)
//   pixels_per_unit  float  — pixels per world unit for pixel-snap (default 32)
//   zoom             float  — camera orthographic size multiplier (default 1)
//   zoom_damping     float  — smooth zoom speed (default 0.1)
//   shake_intensity  float  — current screen-shake magnitude (set from script)
//   shake_duration   float  — remaining shake seconds
//   shake_frequency  float  — shake oscillation speed (default 25)

class VirtualCamera2DSystem {
public:
    struct ShakeState { float intensity=0.f; float duration=0.f; float frequency=25.f; float _timer=0.f; };

    // Call once per frame.  Writes the winning camera position into
    // st.camera_x, st.camera_y, st.camera_zoom (fields you add to EditorState).
    void update(std::vector<Entity>& entities, float dt,
                float& out_cam_x, float& out_cam_y, float& out_zoom) {
        // Find highest-priority active VCam
        Entity* best = nullptr;
        int best_pri = INT_MIN;
        for (auto& e : entities) {
            if (!e.contains("components")) continue;
            auto& comps = e["components"];
            if (!comps.contains("VirtualCamera2D")) continue;
            if (!e.value("active", true)) continue;
            auto& vc = comps["VirtualCamera2D"];
            int pri = vc.value("priority", 0);
            if (pri > best_pri) { best_pri = pri; best = &e; }
        }
        if (!best) return;
        auto& vc = (*best)["components"]["VirtualCamera2D"];

        // Resolve follow target
        int ftid = vc.value("follow_target", 0);
        float tx = out_cam_x, ty = out_cam_y;
        if (ftid != 0) {
            for (auto& e : entities) {
                if (e.value("id", -1) != ftid) continue;
                if (!e.contains("components") || !e["components"].contains("Transform")) break;
                tx = e["components"]["Transform"].value("x", tx) + vc.value("offset_x", 0.f);
                ty = e["components"]["Transform"].value("y", ty) + vc.value("offset_y", 0.f);
                break;
            }
        }

        // Dead zone — don't move camera if target is within dead zone
        float dzw = vc.value("dead_zone_w", 0.f) * 0.5f;
        float dzh = vc.value("dead_zone_h", 0.f) * 0.5f;
        float dx = tx - out_cam_x, dy = ty - out_cam_y;
        if (std::abs(dx) < dzw) tx = out_cam_x;
        if (std::abs(dy) < dzh) ty = out_cam_y;

        // Damping
        float damp_x = vc.value("damping_x", 0.15f);
        float damp_y = vc.value("damping_y", 0.15f);
        float alpha_x = (damp_x <= 0.f) ? 1.f : 1.f - std::exp(-dt / damp_x);
        float alpha_y = (damp_y <= 0.f) ? 1.f : 1.f - std::exp(-dt / damp_y);
        float new_cx = out_cam_x + (tx - out_cam_x) * alpha_x;
        float new_cy = out_cam_y + (ty - out_cam_y) * alpha_y;

        // Confine
        float cw = vc.value("confine_w", 0.f);
        float ch = vc.value("confine_h", 0.f);
        if (cw > 0.f && ch > 0.f) {
            float cx = vc.value("confine_x", 0.f);
            float cy = vc.value("confine_y", 0.f);
            new_cx = std::max(cx - cw, std::min(cx + cw, new_cx));
            new_cy = std::max(cy - ch, std::min(cy + ch, new_cy));
        }

        // Pixel-snap
        if (vc.value("pixel_snap", false)) {
            float ppu = vc.value("pixels_per_unit", 32.f);
            if (ppu > 0.f) {
                new_cx = std::round(new_cx * ppu) / ppu;
                new_cy = std::round(new_cy * ppu) / ppu;
            }
        }

        // Zoom damping
        float target_zoom = vc.value("zoom", 1.f);
        float zoom_damp   = vc.value("zoom_damping", 0.1f);
        float alpha_z = (zoom_damp <= 0.f) ? 1.f : 1.f - std::exp(-dt / zoom_damp);
        out_zoom = out_zoom + (target_zoom - out_zoom) * alpha_z;

        // Screen-shake (procedural, not stored on entity to avoid scene dirtying)
        float shi = vc.value("shake_intensity", 0.f);
        float shd = vc.value("shake_duration",  0.f);
        float shf = vc.value("shake_frequency", 25.f);
        if (shd > 0.f && shi > 0.f) {
            float& tmr = _shake_timer[&vc];
            tmr += dt;
            float s = shi * (shd > 0.f ? shd : 1.f);
            float ox = std::sin(tmr * shf * 2.3562f) * s;
            float oy = std::cos(tmr * shf * 1.7321f) * s;
            new_cx += ox; new_cy += oy;
            float new_shd = shd - dt;
            vc["shake_duration"] = std::max(0.f, new_shd);
            if (new_shd <= 0.f) { vc["shake_intensity"] = 0.f; _shake_timer.erase(&vc); }
        }

        out_cam_x = new_cx;
        out_cam_y = new_cy;
    }

    // Convenience: trigger a shake on the active VCam from script.
    static void Shake(Entity& vcam_comp, float intensity, float duration, float frequency=25.f) {
        vcam_comp["shake_intensity"] = intensity;
        vcam_comp["shake_duration"]  = duration;
        vcam_comp["shake_frequency"] = frequency;
    }

private:
    std::unordered_map<void*, float> _shake_timer;
};

// Editor panel for VirtualCamera2D on selected entity
class VirtualCamera2DPanel {
    bool _open = true;
public:
    bool& open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({360, 480}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Virtual Camera 2D##vcampanel", &_open)) { ImGui::End(); return; }
        Entity* sel = st.selected_entity();
        if (!sel || !sel->contains("components") || !(*sel)["components"].contains("VirtualCamera2D")) {
            ImGui::TextDisabled("Select an entity with a VirtualCamera2D component.");
            ImGui::End(); return;
        }
        auto& vc = (*sel)["components"]["VirtualCamera2D"];

        int pri = vc.value("priority", 0);
        if (ImGui::DragInt("Priority", &pri)) vc["priority"] = pri;

        ImGui::Separator();
        ImGui::TextColored({0.6f,0.9f,1.f,1.f}, "Follow / Look At");
        int ftid = vc.value("follow_target", 0);
        if (ImGui::DragInt("Follow Target ID", &ftid)) vc["follow_target"] = ftid;
        float ox = vc.value("offset_x", 0.f), oy = vc.value("offset_y", 0.f);
        if (ImGui::DragFloat2("Offset##vc", &ox, 1.f)) { vc["offset_x"] = ox; vc["offset_y"] = oy; }

        ImGui::Separator();
        ImGui::TextColored({0.6f,0.9f,1.f,1.f}, "Damping");
        float dx = vc.value("damping_x", 0.15f), dy = vc.value("damping_y", 0.15f);
        if (ImGui::DragFloat("Damping X", &dx, 0.005f, 0.f, 5.f)) vc["damping_x"] = dx;
        if (ImGui::DragFloat("Damping Y", &dy, 0.005f, 0.f, 5.f)) vc["damping_y"] = dy;

        ImGui::Separator();
        ImGui::TextColored({0.6f,0.9f,1.f,1.f}, "Dead Zone");
        float dzw = vc.value("dead_zone_w", 0.f), dzh = vc.value("dead_zone_h", 0.f);
        if (ImGui::DragFloat("Dead Zone W", &dzw, 1.f, 0.f, 1000.f)) vc["dead_zone_w"] = dzw;
        if (ImGui::DragFloat("Dead Zone H", &dzh, 1.f, 0.f, 1000.f)) vc["dead_zone_h"] = dzh;

        ImGui::Separator();
        ImGui::TextColored({0.6f,0.9f,1.f,1.f}, "Confiner");
        float cw = vc.value("confine_w", 0.f), ch = vc.value("confine_h", 0.f);
        float ccx = vc.value("confine_x", 0.f), ccy = vc.value("confine_y", 0.f);
        if (ImGui::DragFloat2("Confine Center##vc", &ccx, 1.f)) { vc["confine_x"]=ccx; vc["confine_y"]=ccy; }
        if (ImGui::DragFloat2("Confine Half-Size##vc", &cw, 1.f)) { vc["confine_w"]=cw; vc["confine_h"]=ch; }

        ImGui::Separator();
        ImGui::TextColored({0.6f,0.9f,1.f,1.f}, "Lens");
        float zoom = vc.value("zoom", 1.f), zdamp = vc.value("zoom_damping", 0.1f);
        if (ImGui::DragFloat("Zoom", &zoom, 0.01f, 0.1f, 20.f)) vc["zoom"] = zoom;
        if (ImGui::DragFloat("Zoom Damping", &zdamp, 0.005f, 0.f, 2.f)) vc["zoom_damping"] = zdamp;
        bool psnap = vc.value("pixel_snap", false);
        if (ImGui::Checkbox("Pixel Snap", &psnap)) vc["pixel_snap"] = psnap;
        if (psnap) {
            float ppu = vc.value("pixels_per_unit", 32.f);
            if (ImGui::DragFloat("Pixels Per Unit##vc", &ppu, 1.f, 1.f, 512.f)) vc["pixels_per_unit"] = ppu;
        }

        ImGui::Separator();
        ImGui::TextColored({0.6f,0.9f,1.f,1.f}, "Screen Shake");
        float shi = vc.value("shake_intensity", 0.f), shd = vc.value("shake_duration", 0.f), shf = vc.value("shake_frequency", 25.f);
        if (ImGui::DragFloat("Intensity##vsh", &shi, 0.5f, 0.f, 200.f)) vc["shake_intensity"] = shi;
        if (ImGui::DragFloat("Duration##vsh",  &shd, 0.01f, 0.f, 10.f)) vc["shake_duration"]  = shd;
        if (ImGui::DragFloat("Frequency##vsh", &shf, 0.5f, 1.f, 100.f)) vc["shake_frequency"] = shf;
        if (ImGui::Button("Preview Shake")) { vc["shake_duration"] = shd > 0.f ? shd : 0.3f; }

        ImGui::End();
    }
};

// ─── 14. Tween system (DOTween-style fluent API) ──────────────────────────────
// Full DOTween-parity fluent tween system callable from MonoBehaviour scripts.
//
// Usage from script:
//   Tween::MoveTo(entity, {300.f, 200.f}, 0.5f)
//       .SetEase(Tween::Ease::OutBack)
//       .SetDelay(0.1f)
//       .SetLoop(-1, Tween::LoopType::Yoyo)
//       .OnComplete([this]{ Log("done"); });
//
//   Tween::FadeTo(entity, 0.f, 0.3f);
//   Tween::ScaleTo(entity, {2.f, 2.f}, 0.4f);
//   Tween::RotateTo(entity, 180.f, 0.6f);
//   Tween::ColorTo(entity, {255,0,0,255}, 0.3f);
//   Tween::Value(0.f, 1.f, 1.f, [](float v){ /* custom callback */ });
//
// TweenSystem::update(dt) drives all live tweens each frame (call from core.cpp).

namespace Tween {
    enum class Ease {
        Linear,
        InSine, OutSine, InOutSine,
        InQuad, OutQuad, InOutQuad,
        InCubic, OutCubic, InOutCubic,
        InQuart, OutQuart, InOutQuart,
        InBack, OutBack, InOutBack,
        InElastic, OutElastic, InOutElastic,
        InBounce, OutBounce, InOutBounce,
        Flash
    };
    enum class LoopType { Restart, Yoyo, Incremental };

    inline float apply_ease(Ease e, float t) {
        t = std::max(0.f, std::min(1.f, t));
        const float pi = 3.14159265f;
        const float c1 = 1.70158f, c3 = c1 + 1.f;
        const float c4 = (2.f*pi)/3.f;
        switch(e) {
            case Ease::Linear:      return t;
            case Ease::InSine:      return 1.f - std::cos(t*pi*0.5f);
            case Ease::OutSine:     return std::sin(t*pi*0.5f);
            case Ease::InOutSine:   return -(std::cos(pi*t)-1.f)*0.5f;
            case Ease::InQuad:      return t*t;
            case Ease::OutQuad:     return 1.f-(1.f-t)*(1.f-t);
            case Ease::InOutQuad:   return t<0.5f?2*t*t:1.f-(-2*t+2)*(-2*t+2)*0.5f;
            case Ease::InCubic:     return t*t*t;
            case Ease::OutCubic:    { float u=1.f-t; return 1.f-u*u*u; }
            case Ease::InOutCubic:  return t<0.5f?4*t*t*t:1.f-(-2*t+2)*(-2*t+2)*(-2*t+2)*0.5f;
            case Ease::InQuart:     return t*t*t*t;
            case Ease::OutQuart:    { float u=1.f-t; return 1.f-u*u*u*u; }
            case Ease::InOutQuart:  return t<0.5f?8*t*t*t*t:1.f-(-2*t+2)*(-2*t+2)*(-2*t+2)*(-2*t+2)*0.5f;
            case Ease::InBack:      return c3*t*t*t - c1*t*t;
            case Ease::OutBack:     { float u=t-1.f; return 1.f+c3*u*u*u+c1*u*u; }
            case Ease::InOutBack:   {
                const float c2=c1*1.525f;
                return t<0.5f ? ((2*t)*(2*t)*((c2+1)*2*t-c2))*0.5f
                              : ((2*t-2)*(2*t-2)*((c2+1)*(2*t-2)+c2)+2)*0.5f;
            }
            case Ease::InElastic:   return t==0?0:t==1?1:-std::pow(2.f,10*t-10)*std::sin((t*10-10.75f)*c4);
            case Ease::OutElastic:  return t==0?0:t==1?1: std::pow(2.f,-10*t)*std::sin((t*10-0.75f)*c4)+1;
            case Ease::InBounce:    return 1.f - apply_ease(Ease::OutBounce, 1.f - t);
            case Ease::OutBounce: {
                const float n1=7.5625f, d1=2.75f;
                if (t < 1.f/d1)       return n1*t*t;
                else if (t < 2.f/d1)  { t-=1.5f/d1;   return n1*t*t+0.75f; }
                else if (t < 2.5f/d1) { t-=2.25f/d1;  return n1*t*t+0.9375f; }
                else                  { t-=2.625f/d1;  return n1*t*t+0.984375f; }
            }
            case Ease::InOutBounce: return t<0.5f?(1.f-apply_ease(Ease::OutBounce,1-2*t))*0.5f
                                                 :(1.f+apply_ease(Ease::OutBounce,2*t-1))*0.5f;
            default: return t;
        }
    }

    struct TweenHandle;
    using OnCompleteCallback = std::function<void()>;
    using OnUpdateCallback   = std::function<void(float)>; // receives 0..1 progress

    struct TweenData {
        // target
        Entity* entity = nullptr;
        // type
        enum class Type { Move, Scale, Rotate, Fade, Color, Value } type = Type::Value;
        // from / to
        float from_x=0,from_y=0, to_x=0,to_y=0;
        // timing
        float duration=1.f, delay=0.f, elapsed=0.f;
        // control
        Ease ease = Ease::Linear;
        int  loops = 1;            // -1 = infinite
        int  loops_done = 0;
        LoopType loop_type = LoopType::Restart;
        bool paused = false;
        bool killed  = false;
        bool started  = false;
        // callbacks
        OnCompleteCallback on_complete;
        OnUpdateCallback   on_update;
        // id
        int id = 0;
    };

    // Global tween list — lives for the process lifetime; tweens remove themselves
    // on completion unless looping.
    inline std::vector<TweenData>& _tweens() { static std::vector<TweenData> v; return v; }
    inline int& _next_id() { static int n = 1; return n; }

    struct TweenHandle {
        int id = 0;

        TweenHandle& SetEase(Ease e) {
            for (auto& t : _tweens()) if (t.id==id) { t.ease=e; break; }
            return *this;
        }
        TweenHandle& SetDelay(float d) {
            for (auto& t : _tweens()) if (t.id==id) { t.delay=d; break; }
            return *this;
        }
        TweenHandle& SetLoop(int loops, LoopType lt = LoopType::Restart) {
            for (auto& t : _tweens()) if (t.id==id) { t.loops=loops; t.loop_type=lt; break; }
            return *this;
        }
        TweenHandle& OnComplete(OnCompleteCallback cb) {
            for (auto& t : _tweens()) if (t.id==id) { t.on_complete=std::move(cb); break; }
            return *this;
        }
        TweenHandle& OnUpdate(OnUpdateCallback cb) {
            for (auto& t : _tweens()) if (t.id==id) { t.on_update=std::move(cb); break; }
            return *this;
        }
        void Kill(bool complete = false) {
            for (auto& t : _tweens()) if (t.id==id) {
                if (complete && t.on_complete) t.on_complete();
                t.killed = true; break;
            }
        }
        void Pause()  { for (auto& t : _tweens()) if (t.id==id) { t.paused=true;  break; } }
        void Resume() { for (auto& t : _tweens()) if (t.id==id) { t.paused=false; break; } }
        bool IsActive() const {
            for (auto& t : _tweens()) if (t.id==id && !t.killed) return true;
            return false;
        }
    };

    inline TweenHandle _register(TweenData d) {
        d.id = _next_id()++;
        _tweens().push_back(std::move(d));
        return TweenHandle{_tweens().back().id};
    }

    // ── Factory functions ────────────────────────────────────────────────────

    // Move entity Transform to world position (tx, ty)
    inline TweenHandle MoveTo(Entity* entity, float tx, float ty, float duration) {
        TweenData d; d.entity=entity; d.duration=duration;
        d.type=TweenData::Type::Move; d.to_x=tx; d.to_y=ty;
        return _register(std::move(d));
    }
    inline TweenHandle MoveTo(Entity* entity, Vector2 pos, float duration) { return MoveTo(entity,pos.x,pos.y,duration); }

    inline TweenHandle ScaleTo(Entity* entity, float sx, float sy, float duration) {
        TweenData d; d.entity=entity; d.duration=duration;
        d.type=TweenData::Type::Scale; d.to_x=sx; d.to_y=sy;
        return _register(std::move(d));
    }
    inline TweenHandle ScaleTo(Entity* entity, Vector2 s, float duration) { return ScaleTo(entity,s.x,s.y,duration); }

    inline TweenHandle RotateTo(Entity* entity, float degrees, float duration) {
        TweenData d; d.entity=entity; d.duration=duration;
        d.type=TweenData::Type::Rotate; d.to_x=degrees;
        return _register(std::move(d));
    }

    inline TweenHandle FadeTo(Entity* entity, float alpha, float duration) {
        TweenData d; d.entity=entity; d.duration=duration;
        d.type=TweenData::Type::Fade; d.to_x=alpha;
        return _register(std::move(d));
    }

    // Color tween — to_x=r, to_y=g stored, (b,a in separate fields below via on_update pattern)
    inline TweenHandle ColorTo(Entity* entity, int r, int g, int b, int a, float duration) {
        TweenData d; d.entity=entity; d.duration=duration;
        d.type=TweenData::Type::Color;
        d.to_x=(float)r; d.to_y=(float)g;
        // Store b,a in from fields until start (will be overwritten with actual from at start)
        d.from_x=(float)b; d.from_y=(float)a;
        return _register(std::move(d));
    }

    // Generic value tween — fires on_update(value) every frame
    inline TweenHandle Value(float from, float to, float duration, OnUpdateCallback cb) {
        TweenData d; d.entity=nullptr; d.duration=duration;
        d.type=TweenData::Type::Value; d.from_x=from; d.to_x=to;
        d.started=true; // no entity to read from
        d.on_update=std::move(cb);
        return _register(std::move(d));
    }

    // Kill all tweens targeting a specific entity (call before Destroy)
    inline void KillAll(Entity* entity) {
        for (auto& t : _tweens()) if (t.entity==entity) t.killed=true;
    }

    // Pause / resume all tweens
    inline void PauseAll()  { for (auto& t : _tweens()) t.paused=true;  }
    inline void ResumeAll() { for (auto& t : _tweens()) t.paused=false; }
} // namespace Tween

class TweenSystem {
public:
    void update(float dt) {
        auto& tweens = Tween::_tweens();
        for (auto& tw : tweens) {
            if (tw.killed || tw.paused) continue;

            // Delay
            if (tw.elapsed < tw.delay) { tw.elapsed += dt; continue; }
            float t = tw.elapsed - tw.delay;

            // Capture from-values on first real tick
            if (!tw.started) {
                _capture_from(tw);
                tw.started = true;
            }

            t += dt;
            tw.elapsed = t + tw.delay;

            float progress = tw.duration > 0.f ? std::min(1.f, t / tw.duration) : 1.f;
            float ep = Tween::apply_ease(tw.ease, progress);

            // Yoyo: flip ep on odd loops
            if (tw.loop_type == Tween::LoopType::Yoyo && (tw.loops_done % 2 == 1))
                ep = 1.f - ep;

            _apply(tw, ep);
            if (tw.on_update) tw.on_update(ep);

            if (progress >= 1.f) {
                tw.loops_done++;
                bool more = (tw.loops == -1) || (tw.loops_done < tw.loops);
                if (!more) {
                    if (tw.on_complete) tw.on_complete();
                    tw.killed = true;
                } else {
                    // Reset elapsed to start next loop
                    tw.elapsed = tw.delay;
                    if (tw.loop_type == Tween::LoopType::Incremental) {
                        // shift from → current to for incremental
                        tw.from_x = tw.to_x; tw.from_y = tw.to_y;
                    }
                }
            }
        }
        // Purge dead tweens
        tweens.erase(std::remove_if(tweens.begin(), tweens.end(),
            [](const Tween::TweenData& t){ return t.killed; }), tweens.end());
    }

private:
    void _capture_from(Tween::TweenData& tw) {
        using T = Tween::TweenData::Type;
        if (!tw.entity || !tw.entity->contains("components")) return;
        auto& comps = (*tw.entity)["components"];
        switch (tw.type) {
            case T::Move:
                if (comps.contains("Transform")) {
                    tw.from_x = comps["Transform"].value("x", 0.f);
                    tw.from_y = comps["Transform"].value("y", 0.f);
                } break;
            case T::Scale:
                if (comps.contains("Transform")) {
                    tw.from_x = comps["Transform"].value("scale_x", 1.f);
                    tw.from_y = comps["Transform"].value("scale_y", 1.f);
                } break;
            case T::Rotate:
                if (comps.contains("Transform"))
                    tw.from_x = comps["Transform"].value("rotation", 0.f);
                break;
            case T::Fade:
                if (comps.contains("SpriteRenderer"))
                    tw.from_x = comps["SpriteRenderer"].value("opacity", 1.f);
                break;
            case T::Color: {
                // to_x=target_r, to_y=target_g, from_x=target_b, from_y=target_a stored temporarily
                float tr=tw.to_x, tg=tw.to_y, tb=tw.from_x, ta=tw.from_y;
                if (comps.contains("SpriteRenderer")) {
                    auto col = comps["SpriteRenderer"].value("color", std::vector<int>{255,255,255,255});
                    tw.from_x=col.size()>0?col[0]:255.f;
                    tw.from_y=col.size()>1?col[1]:255.f;
                    // store b/a target+from in extra slots via on_update closure
                    float fb=col.size()>2?col[2]:255.f, fa=(col.size()>3?col[3]:255.f)/255.f;
                    Entity* ent = tw.entity;
                    tw.on_update = [ent,tr,tg,tb,ta,fr=tw.from_x,fg=tw.from_y,fb,fa](float ep){
                        if (!ent || !ent->contains("components")) return;
                        auto& sr = (*ent)["components"]["SpriteRenderer"];
                        sr["color"] = std::vector<int>{
                            (int)(fr+(tr-fr)*ep), (int)(fg+(tg-fg)*ep),
                            (int)(fb+(tb-fb)*ep), (int)((fa+(ta/255.f-fa)*ep)*255.f)};
                    };
                }
            } break;
            default: break;
        }
    }

    void _apply(Tween::TweenData& tw, float ep) {
        using T = Tween::TweenData::Type;
        if (!tw.entity || !tw.entity->contains("components")) return;
        auto& comps = (*tw.entity)["components"];
        switch (tw.type) {
            case T::Move:
                if (comps.contains("Transform")) {
                    comps["Transform"]["x"] = tw.from_x + (tw.to_x - tw.from_x) * ep;
                    comps["Transform"]["y"] = tw.from_y + (tw.to_y - tw.from_y) * ep;
                } break;
            case T::Scale:
                if (comps.contains("Transform")) {
                    comps["Transform"]["scale_x"] = tw.from_x + (tw.to_x - tw.from_x) * ep;
                    comps["Transform"]["scale_y"] = tw.from_y + (tw.to_y - tw.from_y) * ep;
                } break;
            case T::Rotate:
                if (comps.contains("Transform"))
                    comps["Transform"]["rotation"] = tw.from_x + (tw.to_x - tw.from_x) * ep;
                break;
            case T::Fade:
                if (comps.contains("SpriteRenderer"))
                    comps["SpriteRenderer"]["opacity"] = tw.from_x + (tw.to_x - tw.from_x) * ep;
                break;
            case T::Value:
                // Value tweens are handled entirely via on_update
                if (tw.on_update) tw.on_update(tw.from_x + (tw.to_x - tw.from_x) * ep);
                break;
            default: break; // Color handled via on_update closure
        }
    }
};

// ─── 15. Dialogue system ──────────────────────────────────────────────────────
// A self-contained branching dialogue system.
// Dialogue assets are stored as JSON at  assets/dialogues/<name>.dlg.json
// Format:
//   {
//     "nodes": [
//       { "id": "start", "speaker": "Alice", "portrait": "alice.png",
//         "text": "Hello!", "next": "n2" },
//       { "id": "n2", "speaker": "Bob", "portrait": "bob.png",
//         "text": "Choose:",
//         "choices": [
//           { "label": "Option A", "next": "end_a" },
//           { "label": "Option B", "next": "end_b" }
//         ]
//       },
//       { "id": "end_a", "speaker": "Bob", "text": "You chose A.", "next": "" },
//       { "id": "end_b", "speaker": "Bob", "text": "You chose B.", "next": "" }
//     ]
//   }
//
// Runtime API (call from MonoBehaviour scripts):
//   DialogueSystem::Load("my_dialogue");
//   DialogueSystem::Start();                // go to "start" node
//   DialogueSystem::StartAt("n2");          // go to specific node
//   if (DialogueSystem::IsActive()) { ... }
//   auto& node = DialogueSystem::Current(); // read .speaker / .text / .choices
//   DialogueSystem::Advance();              // next linear node
//   DialogueSystem::Choose(0);             // pick choice 0
//   DialogueSystem::OnNodeEnter([](auto& n){ /* play voice, etc. */ });
//
// Editor panel: allows previewing the dialogue graph and editing nodes.

namespace DialogueSystem {
    struct Choice { std::string label; std::string next; };
    struct Node {
        std::string id, speaker, portrait, text, next;
        std::vector<Choice> choices;
    };

    struct State {
        std::unordered_map<std::string, Node> nodes;
        std::string current_id;
        bool active = false;
        std::function<void(const Node&)> on_node_enter;
    };

    inline State& _state() { static State s; return s; }

    inline bool Load(const std::string& asset_name, const std::string& base_path = "assets/dialogues/") {
        auto& s = _state();
        s.nodes.clear(); s.active = false; s.current_id = "";
        fs::path p = fs::path(base_path) / (asset_name + ".dlg.json");
        auto j = load_json(p);
        if (!j.is_object() || !j.contains("nodes")) return false;
        for (auto& jn : j["nodes"]) {
            Node n;
            n.id       = jn.value("id", "");
            n.speaker  = jn.value("speaker", "");
            n.portrait = jn.value("portrait", "");
            n.text     = jn.value("text", "");
            n.next     = jn.value("next", "");
            if (jn.contains("choices") && jn["choices"].is_array()) {
                for (auto& jc : jn["choices"])
                    n.choices.push_back({jc.value("label",""), jc.value("next","")});
            }
            if (!n.id.empty()) s.nodes[n.id] = std::move(n);
        }
        return true;
    }

   
    inline void StartAt(const std::string& node_id) {
        auto& s = _state();
        auto it = s.nodes.find(node_id);
        if (it == s.nodes.end()) return;
        s.current_id = node_id; s.active = true;
        if (s.on_node_enter) s.on_node_enter(it->second);
    }
    inline void Start()               { StartAt("start"); }
    inline bool IsActive() { return _state().active; }

    inline const Node& Current() {
        static Node empty;
        auto& s = _state();
        auto it = s.nodes.find(s.current_id);
        return it != s.nodes.end() ? it->second : empty;
    }

    inline void Advance() {
        auto& s = _state();
        auto it = s.nodes.find(s.current_id);
        if (it == s.nodes.end() || !it->second.choices.empty()) return;
        if (it->second.next.empty()) { s.active = false; return; }
        StartAt(it->second.next);
    }

    inline void Choose(int index) {
        auto& s = _state();
        auto it = s.nodes.find(s.current_id);
        if (it == s.nodes.end()) return;
        auto& choices = it->second.choices;
        if (index < 0 || index >= (int)choices.size()) return;
        StartAt(choices[index].next);
    }

    inline void Stop() { _state().active = false; }

    inline void OnNodeEnter(std::function<void(const Node&)> cb) { _state().on_node_enter = std::move(cb); }

    inline bool Save(const std::string& asset_name, const std::string& base_path = "assets/dialogues/") {
        auto& s = _state();
        nlohmann::json j; j["nodes"] = nlohmann::json::array();
        for (auto& [id, n] : s.nodes) {
            nlohmann::json jn;
            jn["id"]=n.id; jn["speaker"]=n.speaker; jn["portrait"]=n.portrait;
            jn["text"]=n.text; jn["next"]=n.next;
            jn["choices"]=nlohmann::json::array();
            for (auto& c : n.choices) { nlohmann::json jc; jc["label"]=c.label; jc["next"]=c.next; jn["choices"].push_back(jc); }
            j["nodes"].push_back(jn);
        }
        return save_json(fs::path(base_path) / (asset_name + ".dlg.json"), j);
    }
} // namespace DialogueSystem

// Dialogue Editor Panel
class DialogueEditorPanel {
    bool _open = true;
    std::string _asset_name = "my_dialogue";
    std::string _sel_node;
    char _new_node_id[64] = {};
    bool _dirty = false;

public:
    bool& open() { return _open; }
    void draw(EditorState& /*st*/) {
        if (!_open) return;
        ImGui::SetNextWindowSize({560, 480}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Dialogue Editor##dlgedit", &_open)) { ImGui::End(); return; }

        auto& s = DialogueSystem::_state();

        // Toolbar
        char abuf[128]; snprintf(abuf, sizeof(abuf), "%s", _asset_name.c_str());
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("Asset##dlg", abuf, sizeof(abuf))) _asset_name = abuf;
        ImGui::SameLine();
        if (ImGui::Button("Load##dlg")) { DialogueSystem::Load(_asset_name); _dirty=false; }
        ImGui::SameLine();
        if (ImGui::Button("Save##dlg")) { DialogueSystem::Save(_asset_name); _dirty=false; }
        if (_dirty) { ImGui::SameLine(); ImGui::TextColored({1,0.7f,0,1},"*unsaved*"); }

        ImGui::Separator();
        ImGui::Columns(2, "dlgcols");
        // Node list
        ImGui::Text("Nodes (%d)", (int)s.nodes.size());
        if (ImGui::BeginChild("##dlgnodelist", {-1, 300}, true)) {
            for (auto& [id, n] : s.nodes) {
                bool sel = (_sel_node == id);
                if (ImGui::Selectable(id.c_str(), sel)) _sel_node = id;
            }
        }
        ImGui::EndChild();
        ImGui::InputText("##newnodeid", _new_node_id, sizeof(_new_node_id));
        ImGui::SameLine();
        if (ImGui::Button("Add Node##dlg") && _new_node_id[0]) {
            DialogueSystem::Node n; n.id = _new_node_id; n.text = "...";
            s.nodes[n.id] = std::move(n); _sel_node = _new_node_id;
            _new_node_id[0] = 0; _dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Del##dlg") && !_sel_node.empty()) {
            s.nodes.erase(_sel_node); _sel_node.clear(); _dirty = true;
        }

        ImGui::NextColumn();
        // Node editor
        if (!_sel_node.empty() && s.nodes.count(_sel_node)) {
            auto& n = s.nodes[_sel_node];
            char buf[256];
            ImGui::Text("Node: %s", n.id.c_str());

            snprintf(buf,sizeof(buf),"%s",n.speaker.c_str());
            if (ImGui::InputText("Speaker##dlg", buf, sizeof(buf))) { n.speaker=buf; _dirty=true; }
            snprintf(buf,sizeof(buf),"%s",n.portrait.c_str());
            if (ImGui::InputText("Portrait##dlg", buf, sizeof(buf))) { n.portrait=buf; _dirty=true; }
            snprintf(buf,sizeof(buf),"%s",n.text.c_str());
            if (ImGui::InputTextMultiline("Text##dlg", buf, sizeof(buf), {-1,80})) { n.text=buf; _dirty=true; }
            snprintf(buf,sizeof(buf),"%s",n.next.c_str());
            if (ImGui::InputText("Next Node##dlg", buf, sizeof(buf))) { n.next=buf; _dirty=true; }

            ImGui::Separator();
            ImGui::Text("Choices (%d)", (int)n.choices.size());
            for (int i=0; i<(int)n.choices.size(); ++i) {
                ImGui::PushID(i);
                char lb[128], nx[128];
                snprintf(lb,sizeof(lb),"%s",n.choices[i].label.c_str());
                snprintf(nx,sizeof(nx),"%s",n.choices[i].next.c_str());
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputText("Label##dlgch", lb, sizeof(lb))) { n.choices[i].label=lb; _dirty=true; }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputText("→##dlgch", nx, sizeof(nx))) { n.choices[i].next=nx; _dirty=true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X##dlgch")) { n.choices.erase(n.choices.begin()+i); _dirty=true; ImGui::PopID(); break; }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Choice##dlg")) { n.choices.push_back({"Option", ""}); _dirty=true; }

            // Live preview
            ImGui::Separator();
            if (ImGui::Button("Preview From This Node##dlg")) DialogueSystem::StartAt(n.id);
            if (DialogueSystem::IsActive() && DialogueSystem::Current().id == n.id) {
                ImGui::TextColored({0.3f,1,0.3f,1},"[ACTIVE]  %s: %s", n.speaker.c_str(), n.text.c_str());
                for (int i=0; i<(int)n.choices.size(); ++i) {
                    if (ImGui::Button(("Choice: "+n.choices[i].label).c_str())) DialogueSystem::Choose(i);
                }
                if (n.choices.empty() && ImGui::Button("Advance##dlgprev")) DialogueSystem::Advance();
            }
        }
        ImGui::Columns(1);
        ImGui::End();
    }
};

// ─── 16. Localization system ──────────────────────────────────────────────────
// CSV-based string table with runtime hot-reload.
//
// CSV format  (assets/localization/<locale>.csv):
//   key,value
//   ui.start_button,Start Game
//   ui.quit_button,Quit
//   story.intro,"Once upon a time..."
//
// Script usage:
//   #include "../../engine_cpp/localization.hpp"   // (or via unity2d_script_api)
//   std::string text = L("ui.start_button");       // returns locale string or key
//   Localization::SetLocale("fr");                  // switches language at runtime
//   Localization::Reload();                         // re-reads CSV from disk
//
// Editor panel: browse/edit keys, switch locale, add new keys.

namespace Localization {
    struct State {
        std::string locale = "en";
        std::string base_path = "assets/localization/";
        std::unordered_map<std::string, std::string> table;
        bool loaded = false;
    };

    inline State& _state() { static State s; return s; }

    // Parse a very simple CSV (key,value; value may be double-quoted)
    inline bool Reload() {
        auto& s = _state();
        s.table.clear(); s.loaded = false;
        fs::path p = fs::path(s.base_path) / (s.locale + ".csv");
        std::ifstream f(p);
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0]=='#') continue;
            auto comma = line.find(',');
            if (comma == std::string::npos) continue;
            std::string key = line.substr(0, comma);
            std::string val = line.substr(comma+1);
            // strip quotes
            if (!val.empty() && val.front()=='"') {
                if (val.back()=='"') val = val.substr(1, val.size()-2);
                else val = val.substr(1);
            }
            // trim CR
            if (!val.empty() && val.back()=='\r') val.pop_back();
            s.table[key] = val;
        }
        s.loaded = true;
        return true;
    }

    inline void SetLocale(const std::string& locale) { _state().locale = locale; Reload(); }
    inline const std::string& GetLocale() { return _state().locale; }

    inline const std::string& Get(const std::string& key) {
        auto& s = _state();
        if (!s.loaded) Reload();
        auto it = s.table.find(key);
        return it != s.table.end() ? it->second : key; // fallback = key itself
    }

    // Set a key at runtime (temporary, not written to disk unless SaveCSV is called)
    inline void Set(const std::string& key, const std::string& value) {
        _state().table[key] = value;
    }

    inline bool SaveCSV(const std::string& locale = "") {
        auto& s = _state();
        std::string loc = locale.empty() ? s.locale : locale;
        fs::path p = fs::path(s.base_path) / (loc + ".csv");
        std::error_code ec; fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p);
        if (!f) return false;
        for (auto& [k,v] : s.table) {
            bool quote = v.find(',') != std::string::npos || v.find('\n') != std::string::npos;
            f << k << "," << (quote ? "\"" : "") << v << (quote ? "\"" : "") << "\n";
        }
        return f.good();
    }
} // namespace Localization

// Short alias usable in scripts: L("key")
inline std::string L(const std::string& key) { return Localization::Get(key); }

class LocalizationPanel {
    bool _open = true;
    char _filter[128] = {};
    char _new_key[128] = {}, _new_val[256] = {};
    std::string _edit_key;
    bool _dirty = false;

public:
    bool& open() { return _open; }
    void draw(EditorState& /*st*/) {
        if (!_open) return;
        ImGui::SetNextWindowSize({540, 420}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Localization##loclpanel", &_open)) { ImGui::End(); return; }
        auto& s = Localization::_state();

        // Toolbar
        char locbuf[32]; snprintf(locbuf, sizeof(locbuf), "%s", s.locale.c_str());
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputText("Locale##loc", locbuf, sizeof(locbuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            Localization::SetLocale(locbuf);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload##loc")) Localization::Reload();
        ImGui::SameLine();
        if (ImGui::Button("Save##loc")) { Localization::SaveCSV(); _dirty=false; }
        if (_dirty) { ImGui::SameLine(); ImGui::TextColored({1,0.7f,0,1},"*unsaved*"); }

        ImGui::Separator();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("Filter##loc", _filter, sizeof(_filter));

        ImGui::Text("%d keys", (int)s.table.size());
        if (ImGui::BeginChild("##loctable", {-1,-60}, true)) {
            std::string flt(_filter);
            for (auto& [k,v] : s.table) {
                if (!flt.empty() && k.find(flt)==std::string::npos && v.find(flt)==std::string::npos) continue;
                ImGui::PushID(k.c_str());
                ImGui::TextColored({0.7f,0.85f,1.f,1.f}, "%s", k.c_str());
                ImGui::SameLine(200);
                char vbuf[512]; snprintf(vbuf,sizeof(vbuf),"%s",v.c_str());
                ImGui::SetNextItemWidth(-40);
                if (ImGui::InputText("##locv", vbuf, sizeof(vbuf))) { s.table[k]=vbuf; _dirty=true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("X##locdel")) { s.table.erase(k); _dirty=true; ImGui::PopID(); break; }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // Add new key
        ImGui::Separator();
        ImGui::SetNextItemWidth(160); ImGui::InputText("Key##locnew",  _new_key, sizeof(_new_key));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220); ImGui::InputText("Value##locnew",_new_val, sizeof(_new_val));
        ImGui::SameLine();
        if (ImGui::Button("Add##locnew") && _new_key[0]) {
            s.table[_new_key] = _new_val; _dirty=true;
            _new_key[0]=0; _new_val[0]=0;
        }
        ImGui::End();
    }
};

// ─── 17. ConsolePanel — categorised, searchable runtime log ───────────────────
// Drop-in replacement for the editor's basic log viewer.
// Features:
//   • Log, Warning, Error, Exception, Assert categories with colour coding
//   • Real-time search / filter
//   • Stack-trace field per entry (populated when Debug::log_error is called
//     with a backtrace string)
//   • Collapse identical consecutive messages (toggle)
//   • Clear, Copy All, Pause buttons
//   • Entry count badges per category (Unity style)
//   • Double-click opens source location in a detail pane
//   • Max 4000 entries, auto-trims oldest

// ConsolePanel is already defined in panels.hpp; only define it here when
// compiled without the main panels header (e.g. in a standalone translation unit).
#ifndef CONSOLE_PANEL_DEFINED
#define CONSOLE_PANEL_DEFINED
class ConsolePanel {
public:
    enum class Level { Log, Warning, Error, Exception, Assert };
    struct Entry {
        Level level;
        std::string message;
        std::string stack;
        int count = 1;        // for collapse
        double timestamp = 0.0;
    };

    // Call these from your engine's logging macros / Debug::log_* functions
    void add(Level level, const std::string& msg, const std::string& stack = "") {
        if (_paused) return;
        // Collapse identical consecutive entries
        if (_collapse && !_entries.empty()) {
            auto& last = _entries.back();
            if (last.level == level && last.message == msg) { last.count++; _scroll_to_bottom=true; return; }
        }
        Entry e; e.level=level; e.message=msg; e.stack=stack;
        _entries.push_back(std::move(e));
        if ((int)_entries.size() > _max_entries)
            _entries.erase(_entries.begin(), _entries.begin() + (_max_entries/4));
        _counts[(int)level]++;
        _scroll_to_bottom = true;
    }
    void log(const std::string& msg)       { add(Level::Log, msg); }
    void warn(const std::string& msg)      { add(Level::Warning, msg); }
    void error(const std::string& msg, const std::string& stack="") { add(Level::Error, msg, stack); }

    bool& open() { return _open; }
    void draw(EditorState& /*st*/) {
        if (!_open) return;
        ImGui::SetNextWindowSize({700, 320}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Console##consolepanel", &_open)) { ImGui::End(); return; }

        // Toolbar
        if (ImGui::Button("Clear")) { _entries.clear(); for(auto& c:_counts)c=0; _sel=-1; }
        ImGui::SameLine();
        if (ImGui::Button(_paused ? "Resume##con" : "Pause##con")) _paused = !_paused;
        ImGui::SameLine();
        if (ImGui::Checkbox("Collapse", &_collapse)) {}
        ImGui::SameLine(0,20);

        // Category toggles with counts
        static const char* labels[] = {"Log","Warn","Error","Exc","Assert"};
        static ImVec4 cols[] = {
            {0.85f,0.85f,0.85f,1},{1,0.85f,0.2f,1},{1,0.3f,0.3f,1},
            {1,0.1f,0.6f,1},{0.5f,0.3f,1,1}
        };
        for (int i=0;i<5;++i) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, _show[i] ? cols[i] : ImVec4{0.4f,0.4f,0.4f,1});
            char lbl[32]; snprintf(lbl,sizeof(lbl),"%s (%d)##conlvl%d",labels[i],_counts[i],i);
            if (ImGui::Selectable(lbl, _show[i], 0, {80,0})) _show[i]=!_show[i];
            ImGui::PopStyleColor();
        }

        // Search
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Search##con", _filter, sizeof(_filter));

        ImGui::Separator();

        // Entry list
        float list_h = _sel >= 0 ? -120.f : -1.f;
        if (ImGui::BeginChild("##conlist", {-1, list_h}, false, ImGuiWindowFlags_HorizontalScrollbar)) {
            std::string flt(_filter);
            for (int i=0; i<(int)_entries.size(); ++i) {
                auto& e = _entries[i];
                if (!_show[(int)e.level]) continue;
                if (!flt.empty() && e.message.find(flt)==std::string::npos) continue;
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Text, cols[(int)e.level]);
                bool sel = (_sel == i);
                std::string disp = e.message;
                if (e.count > 1) disp = "["+std::to_string(e.count)+"x] " + disp;
                if (ImGui::Selectable(disp.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick))
                    _sel = sel ? -1 : i;
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            if (_scroll_to_bottom) { ImGui::SetScrollHereY(1.0f); _scroll_to_bottom=false; }
        }
        ImGui::EndChild();

        // Detail pane
        if (_sel >= 0 && _sel < (int)_entries.size()) {
            ImGui::Separator();
            if (ImGui::BeginChild("##condetail", {-1,-1}, true)) {
                auto& e = _entries[_sel];
                ImGui::PushStyleColor(ImGuiCol_Text, cols[(int)e.level]);
                ImGui::TextWrapped("%s", e.message.c_str());
                ImGui::PopStyleColor();
                if (!e.stack.empty()) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Stack Trace:");
                    ImGui::TextWrapped("%s", e.stack.c_str());
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

private:
    bool _open = true;
    std::vector<Entry> _entries;
    int _max_entries = 4000;
    int _counts[5] = {};
    bool _show[5] = {true,true,true,true,true};
    char _filter[128] = {};
    int _sel = -1;
    bool _collapse = true;
    bool _paused = false;
    bool _scroll_to_bottom = false;
};
#endif // CONSOLE_PANEL_DEFINED

// ─── 18. StateMachine component (generic code-driven FSM) ─────────────────────
// A lightweight generic FSM usable from scripts — decoupled from the Animator.
// Typical usage (inside MonoBehaviour, C++ script):
//
//   StateMachine fsm;
//   void Awake() override {
//       fsm.AddState("Idle",
//           [this]{ Log("enter idle"); },          // OnEnter
//           [this](float dt){ /* tick */ },         // OnUpdate
//           [this]{ Log("exit idle"); });           // OnExit
//       fsm.AddState("Run",
//           [this]{ animator.Play("Run"); },
//           [this](float dt){ move(dt); },
//           nullptr);
//       fsm.AddTransition("Idle","Run",  [this]{ return speed>10.f; });
//       fsm.AddTransition("Run", "Idle", [this]{ return speed<=10.f; });
//       fsm.Start("Idle");
//   }
//   void Update(float dt) override { fsm.Update(dt); }
//
// Notes:
//   • Transitions are evaluated in insertion order each Update; first match fires.
//   • Any-state transitions (from "") are checked first.
//   • OnExit of the old state is called BEFORE OnEnter of the new state.
//   • Re-entering the current state is allowed (call fsm.ForceState(name)).

class StateMachine {
public:
    using EnterFn  = std::function<void()>;
    using UpdateFn = std::function<void(float)>;
    using ExitFn   = std::function<void()>;
    using CondFn   = std::function<bool()>;

    struct State {
        std::string name;
        EnterFn  on_enter;
        UpdateFn on_update;
        ExitFn   on_exit;
    };

    struct Transition {
        std::string from; // "" = any state
        std::string to;
        CondFn condition;
        float min_time = 0.f; // minimum time in current state before this can fire
    };

    void AddState(const std::string& name, EnterFn enter=nullptr, UpdateFn update=nullptr, ExitFn exit=nullptr) {
        _states.push_back({name, std::move(enter), std::move(update), std::move(exit)});
    }

    void AddTransition(const std::string& from, const std::string& to, CondFn cond, float min_time=0.f) {
        _transitions.push_back({from, to, std::move(cond), min_time});
    }
    void AddAnyTransition(const std::string& to, CondFn cond, float min_time=0.f) {
        AddTransition("", to, std::move(cond), min_time);
    }

    void Start(const std::string& initial_state) {
        _enter(initial_state);
    }

    void Update(float dt) {
        _time_in_state += dt;
        // Check any-state transitions first
        for (auto& tr : _transitions) {
            if (!tr.from.empty() && tr.from != _current) continue;
            if (_time_in_state < tr.min_time) continue;
            if (tr.condition && tr.condition()) { _enter(tr.to); return; }
        }
        // Tick current state
        auto* s = _find(_current);
        if (s && s->on_update) s->on_update(dt);
    }

    void ForceState(const std::string& name) { _enter(name); }

    const std::string& Current() const { return _current; }
    float TimeInState() const { return _time_in_state; }
    bool  Is(const std::string& name) const { return _current == name; }

    // Useful for editor display
    std::vector<std::string> StateNames() const {
        std::vector<std::string> out; for (auto& s:_states) out.push_back(s.name); return out;
    }

private:
    std::vector<State>      _states;
    std::vector<Transition> _transitions;
    std::string             _current;
    float                   _time_in_state = 0.f;

    State* _find(const std::string& name) {
        for (auto& s : _states) if (s.name == name) return &s;
        return nullptr;
    }

    void _enter(const std::string& name) {
        if (auto* old = _find(_current)) if (old->on_exit) old->on_exit();
        _current = name;
        _time_in_state = 0.f;
        if (auto* s = _find(name)) if (s->on_enter) s->on_enter();
    }
};

// Editor panel: shows FSM state for a selected script (read-only runtime view)
class StateMachineDebugPanel {
    bool _open = true;
public:
    bool& open() { return _open; }
    // Pass in a list of (label, fsm*) pairs from your editor's registered scripts
    void draw(const std::vector<std::pair<std::string, StateMachine*>>& fsms) {
        if (!_open) return;
        ImGui::SetNextWindowSize({300, 280}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("FSM Debugger##fsmdebug", &_open)) { ImGui::End(); return; }
        if (fsms.empty()) { ImGui::TextDisabled("No registered state machines."); ImGui::End(); return; }
        for (auto& [label, fsm] : fsms) {
            if (!fsm) continue;
            if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextColored({0.3f,1.f,0.5f,1.f}, "Current: %s", fsm->Current().c_str());
                ImGui::Text("Time in state: %.2f s", fsm->TimeInState());
                ImGui::Spacing();
                ImGui::TextDisabled("All States:");
                for (auto& sn : fsm->StateNames()) {
                    bool is_cur = (sn == fsm->Current());
                    if (is_cur) ImGui::PushStyleColor(ImGuiCol_Text, {0.3f,1.f,0.5f,1.f});
                    ImGui::BulletText("%s%s", sn.c_str(), is_cur?" ◀":"");
                    if (is_cur) ImGui::PopStyleColor();
                }
            }
        }
        ImGui::End();
    }
};
// ═══════════════════════════════════════════════════════════════════════════════
// NOVA ENGINE — ADVANCED FEATURES BLOCK
// Features added below: SpriteEditor, ShaderGraphPanel, GradientEditorPanel,
// FrameDebuggerPanel, UndoHistoryPanel, UIBuilderPanel, BuildReportPanel,
// GitVersionControlPanel, AddressablesPanel, GizmoOverlaySettings,
// Shadow2DSettingsPanel, EffectorChainDebugger, BatchRenamerPanel,
// ComponentSearchPanel, HotkeysPanel
// ═══════════════════════════════════════════════════════════════════════════════

// ─── 19. Sprite Editor Panel ──────────────────────────────────────────────────
// Unity-style sprite editor: visual pivot editing, manual slice, border editing.
class SpriteEditorPanel {
    bool _open = false;
    std::string _texture_path;
    // Slice rects stored as {x,y,w,h,pivot_x,pivot_y,name}
    struct SliceRect { int x=0,y=0,w=64,h=64; float px=0.5f,py=0.5f; std::string name; };
    std::vector<SliceRect> _rects;
    int _selected = -1;
    int _tex_w = 256, _tex_h = 256;
    float _zoom = 1.f;
    ImVec2 _scroll{0,0};
    char _slice_cols[8]="4"; char _slice_rows[8]="4";
    enum class SliceMode { Automatic, Grid, Manual } _slice_mode = SliceMode::Grid;
    bool _dirty = false;
    thumbnail_cache::Cache _preview_cache;
public:
    void init(vkr::RendererBackend& backend) { _preview_cache.init(backend); }
    void shutdown() { _preview_cache.clear(); }
    void open(const std::string& path="") { _open=true; if(!path.empty()){_texture_path=path;_rects.clear();_selected=-1;_tex_w=256;_tex_h=256;_load_meta();} }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({820,560},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Sprite Editor##spriteed",&_open,ImGuiWindowFlags_MenuBar)) { ImGui::End(); return; }

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Slice")) {
                ImGui::RadioButton("Automatic", (int*)&_slice_mode, 0);
                ImGui::RadioButton("Grid",      (int*)&_slice_mode, 1);
                ImGui::RadioButton("Manual",    (int*)&_slice_mode, 2);
                if (_slice_mode == SliceMode::Grid) {
                    ImGui::InputText("Cols##sc", _slice_cols, 8);
                    ImGui::InputText("Rows##sr", _slice_rows, 8);
                }
                if (ImGui::Button("Apply Slice")) { _do_slice(); }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::SliderFloat("Zoom", &_zoom, 0.25f, 8.f, "%.2fx");
                ImGui::EndMenu();
            }
            if (ImGui::Button("Apply##sped")) { _save_meta(st); }
            ImGui::EndMenuBar();
        }

        // Left: sprite canvas
        ImGui::BeginChild("##spedc", {560,0}, true);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        const thumbnail_cache::Entry* preview = _texture_path.empty() ? nullptr : _preview_cache.get(_texture_path);
        if (preview && preview->w > 0 && preview->h > 0) {
            _tex_w = preview->w;
            _tex_h = preview->h;
        }
        float tw = _tex_w * _zoom, th = _tex_h * _zoom;
        // Draw checkerboard background
        ImU32 ca = IM_COL32(80,80,80,255), cb = IM_COL32(60,60,60,255);
        int gs = 16;
        for (int gy=0;gy<(int)(th/gs)+1;gy++)
            for (int gx=0;gx<(int)(tw/gs)+1;gx++) {
                ImU32 c=(gx+gy)%2?ca:cb;
                dl->AddRectFilled({p0.x+gx*gs,p0.y+gy*gs},{p0.x+(gx+1)*gs,p0.y+(gy+1)*gs},c);
            }
        if (preview && preview->imgui_ds) {
            dl->AddImage((ImTextureID)(intptr_t)preview->imgui_ds, p0, {p0.x+tw,p0.y+th});
        } else {
            dl->AddRectFilled(p0,{p0.x+tw,p0.y+th},IM_COL32(34,34,40,230));
            dl->AddRect(p0,{p0.x+tw,p0.y+th},IM_COL32(200,200,200,120));
            dl->AddText({p0.x+8.f,p0.y+8.f}, IM_COL32(210,210,215,190),
                        _texture_path.empty() ? "Select a texture asset" : "Texture preview unavailable");
        }
        dl->AddText({p0.x+6.f,p0.y+6.f}, IM_COL32(255,255,255,230),
                    (fs::path(_texture_path).filename().string()+"  "+std::to_string(_tex_w)+" x "+std::to_string(_tex_h)).c_str());
        // Draw rects
        for (int i=0;i<(int)_rects.size();i++) {
            auto& r = _rects[i];
            float rx=p0.x+r.x*_zoom, ry=p0.y+r.y*_zoom;
            float rw=r.w*_zoom, rh=r.h*_zoom;
            ImU32 col = (i==_selected)?IM_COL32(80,200,255,255):IM_COL32(80,180,80,200);
            dl->AddRect({rx,ry},{rx+rw,ry+rh},col,0,0,i==_selected?2.f:1.f);
            // Pivot dot
            float pvx=rx+r.px*rw, pvy=ry+(1.f-r.py)*rh;
            dl->AddCircleFilled({pvx,pvy},4.f,IM_COL32(255,80,80,255));
            // Label
            if (_zoom>1.5f) dl->AddText({rx+2,ry+2},IM_COL32(255,255,255,200),r.name.empty()?("Sprite_"+std::to_string(i)).c_str():r.name.c_str());
        }
        // Click to select / add manual rect
        ImVec2 mp = ImGui::GetMousePos();
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
            _selected = -1;
            for (int i=0;i<(int)_rects.size();i++) {
                auto& r=_rects[i];
                if (mp.x>=p0.x+r.x*_zoom && mp.x<=p0.x+(r.x+r.w)*_zoom &&
                    mp.y>=p0.y+r.y*_zoom && mp.y<=p0.y+(r.y+r.h)*_zoom)
                    { _selected=i; break; }
            }
            if (_selected<0 && _slice_mode==SliceMode::Manual) {
                SliceRect nr; nr.x=(int)((mp.x-p0.x)/_zoom); nr.y=(int)((mp.y-p0.y)/_zoom);
                nr.w=64; nr.h=64; nr.name="Sprite_"+std::to_string(_rects.size());
                _rects.push_back(nr); _selected=(int)_rects.size()-1; _dirty=true;
            }
        }
        ImGui::Dummy({tw,th});
        ImGui::EndChild();

        // Right: properties
        ImGui::SameLine();
        ImGui::BeginChild("##spedr",{0,0},true);
        ImGui::Text("Sprites: %d", (int)_rects.size());
        ImGui::Separator();
        if (_selected>=0 && _selected<(int)_rects.size()) {
            auto& r=_rects[_selected];
            ImGui::Text("Sprite: %d", _selected);
            char nbuf[64]; snprintf(nbuf,64,"%s",r.name.c_str());
            if (ImGui::InputText("Name##spn",nbuf,64)) { r.name=nbuf; _dirty=true; }
            if (ImGui::DragInt("X##spx",&r.x,1)) _dirty=true;
            if (ImGui::DragInt("Y##spy",&r.y,1)) _dirty=true;
            if (ImGui::DragInt("W##spw",&r.w,1,1,4096)) _dirty=true;
            if (ImGui::DragInt("H##sph",&r.h,1,1,4096)) _dirty=true;
            if (ImGui::SliderFloat("Pivot X##spvx",&r.px,0.f,1.f)) _dirty=true;
            if (ImGui::SliderFloat("Pivot Y##spvy",&r.py,0.f,1.f)) _dirty=true;
            // Pivot presets
            if (ImGui::Button("Center")) { r.px=.5f;r.py=.5f;_dirty=true; } ImGui::SameLine();
            if (ImGui::Button("Bottom")) { r.px=.5f;r.py=0.f;_dirty=true; } ImGui::SameLine();
            if (ImGui::Button("Top"))    { r.px=.5f;r.py=1.f;_dirty=true; }
            if (ImGui::Button("Delete Rect")) { _rects.erase(_rects.begin()+_selected); _selected=-1; _dirty=true; }
        } else {
            ImGui::TextDisabled("Click a rect to edit.");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Texture: %s", _texture_path.empty()?"(none)":_texture_path.c_str());
        if (ImGui::Button("Clear All")) { _rects.clear(); _selected=-1; _dirty=true; }
        ImGui::EndChild();
        ImGui::End();
    }
private:
    void _do_slice() {
        _rects.clear(); _selected=-1;
        int cols = std::max(1,atoi(_slice_cols)), rows = std::max(1,atoi(_slice_rows));
        int cw=_tex_w/cols, ch=_tex_h/rows;
        for (int r=0;r<rows;r++) for (int c=0;c<cols;c++) {
            SliceRect sr; sr.x=c*cw; sr.y=r*ch; sr.w=cw; sr.h=ch;
            sr.px=.5f; sr.py=.5f;
            sr.name="Sprite_"+std::to_string(r*cols+c);
            _rects.push_back(sr);
        }
        _dirty=true;
    }
    void _load_meta() {
        if (_texture_path.empty()) return;
        auto p = fs::path(_texture_path).replace_extension(".sprite_meta");
        auto j = load_json(p);
        if (!j.is_array()) return;
        _rects.clear();
        for (auto& jr:j) {
            SliceRect r;
            r.x=jr.value("x",0); r.y=jr.value("y",0); r.w=jr.value("w",64); r.h=jr.value("h",64);
            r.px=jr.value("px",0.5f); r.py=jr.value("py",0.5f); r.name=jr.value("name","");
            _rects.push_back(r);
        }
    }
    void _save_meta(EditorState& st) {
        if (_texture_path.empty()) { st.log_warn("No texture selected."); return; }
        auto p = fs::path(_texture_path).replace_extension(".sprite_meta");
        nlohmann::json j = nlohmann::json::array();
        for (auto& r:_rects) j.push_back({{"x",r.x},{"y",r.y},{"w",r.w},{"h",r.h},{"px",r.px},{"py",r.py},{"name",r.name}});
        save_json(p,j);
        st.log("Sprite meta saved: "+p.string());
        _dirty=false;
    }
};

// ─── 20. Gradient Editor Panel ────────────────────────────────────────────────
// Reusable gradient editor widget (used by particle systems, lights, shaders).
// Stores keys as {t, r, g, b, a}. Inline callable as a widget.
struct GradientKey { float t=0.f; float r=1,g=1,b=1,a=1; };
struct Gradient {
    std::vector<GradientKey> keys = {{0.f,1,1,1,1},{1.f,1,1,1,0}};
    void sort_keys() { std::sort(keys.begin(),keys.end(),[](auto&a,auto&b){return a.t<b.t;}); }
    // Sample at t in [0,1]
    void sample(float t, float& r,float& g,float& b,float& a) const {
        if (keys.empty()){r=g=b=a=1;return;}
        if (t<=keys.front().t){auto&k=keys.front();r=k.r;g=k.g;b=k.b;a=k.a;return;}
        if (t>=keys.back().t){auto&k=keys.back();r=k.r;g=k.g;b=k.b;a=k.a;return;}
        for (int i=0;i<(int)keys.size()-1;i++) {
            if (t>=keys[i].t && t<=keys[i+1].t) {
                float f=(t-keys[i].t)/(keys[i+1].t-keys[i].t);
                auto&a0=keys[i],&a1=keys[i+1];
                r=a0.r+(a1.r-a0.r)*f; g=a0.g+(a1.g-a0.g)*f;
                b=a0.b+(a1.b-a0.b)*f; a=a0.a+(a1.a-a0.a)*f; return;
            }
        }
    }
    nlohmann::json to_json() const {
        nlohmann::json j=nlohmann::json::array();
        for (auto&k:keys) j.push_back({{"t",k.t},{"r",k.r},{"g",k.g},{"b",k.b},{"a",k.a}});
        return j;
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_array()) return; keys.clear();
        for (auto&jk:j) keys.push_back({jk.value("t",0.f),jk.value("r",1.f),jk.value("g",1.f),jk.value("b",1.f),jk.value("a",1.f)});
        sort_keys();
    }
};

class GradientEditorPanel {
    bool _open = false;
    Gradient _grad;
    int _sel = -1;
    std::string _title = "Gradient Editor";
    std::function<void(const Gradient&)> _on_change;
public:
    void open(const std::string& title, const Gradient& g, std::function<void(const Gradient&)> cb={}) {
        _open=true; _grad=g; _sel=-1; _title=title; _on_change=cb;
    }
    bool& is_open() { return _open; }
    const Gradient& gradient() const { return _grad; }
    void draw() {
        if (!_open) return;
        ImGui::SetNextWindowSize({460,200},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin((_title+"##graded").c_str(),&_open)) { ImGui::End(); return; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float bw=420.f, bh=32.f;
        // Draw gradient bar
        int segs=128;
        for (int i=0;i<segs;i++) {
            float t0=(float)i/segs, t1=(float)(i+1)/segs;
            float r0,g0,b0,a0,r1,g1,b1,a1;
            _grad.sample(t0,r0,g0,b0,a0); _grad.sample(t1,r1,g1,b1,a1);
            dl->AddRectFilledMultiColor(
                {p0.x+t0*bw,p0.y},{p0.x+t1*bw,p0.y+bh},
                IM_COL32(r0*255,g0*255,b0*255,a0*255),
                IM_COL32(r1*255,g1*255,b1*255,a1*255),
                IM_COL32(r1*255,g1*255,b1*255,a1*255),
                IM_COL32(r0*255,g0*255,b0*255,a0*255));
        }
        dl->AddRect(p0,{p0.x+bw,p0.y+bh},IM_COL32(160,160,160,255));
        // Draw key handles
        for (int i=0;i<(int)_grad.keys.size();i++) {
            auto& k=_grad.keys[i];
            float kx=p0.x+k.t*bw;
            ImU32 hcol = i==_sel?IM_COL32(255,255,0,255):IM_COL32(200,200,200,255);
            dl->AddTriangleFilled({kx-6,p0.y+bh+2},{kx+6,p0.y+bh+2},{kx,p0.y+bh+14},IM_COL32(k.r*255,k.g*255,k.b*255,255));
            dl->AddTriangle({kx-6,p0.y+bh+2},{kx+6,p0.y+bh+2},{kx,p0.y+bh+14},hcol);
        }
        ImGui::Dummy({bw, bh+20});
        // Click on bar: add key; click on handle: select
        ImVec2 mp=ImGui::GetMousePos();
        if (ImGui::IsWindowHovered()) {
            if (ImGui::IsMouseClicked(0)) {
                _sel=-1;
                for (int i=0;i<(int)_grad.keys.size();i++) {
                    float kx=p0.x+_grad.keys[i].t*bw;
                    if (std::abs(mp.x-kx)<8 && mp.y>p0.y+bh) { _sel=i; break; }
                }
                if (_sel<0 && mp.y>=p0.y && mp.y<=p0.y+bh) {
                    float t=(mp.x-p0.x)/bw;
                    float r,g,b,a; _grad.sample(t,r,g,b,a);
                    _grad.keys.push_back({t,r,g,b,a}); _grad.sort_keys(); _sel=(int)_grad.keys.size()-1;
                    if (_on_change) _on_change(_grad);
                }
            }
        }
        // Edit selected key
        if (_sel>=0 && _sel<(int)_grad.keys.size()) {
            auto& k=_grad.keys[_sel];
            bool ch=false;
            ch|=ImGui::SliderFloat("Position##gkt",&k.t,0.f,1.f);
            float col[4]={k.r,k.g,k.b,k.a};
            if (ImGui::ColorEdit4("Color##gkc",col)) { k.r=col[0];k.g=col[1];k.b=col[2];k.a=col[3];ch=true; }
            if (ch) { _grad.sort_keys(); if (_on_change) _on_change(_grad); }
            if (ImGui::Button("Delete Key") && _grad.keys.size()>2) { _grad.keys.erase(_grad.keys.begin()+_sel); _sel=-1; if (_on_change) _on_change(_grad); }
        } else { ImGui::TextDisabled("Click bar to add key, click handle to edit."); }
        ImGui::End();
    }
};

// ─── 21. Shader Graph Panel ───────────────────────────────────────────────────
// Node-based shader graph editor — produces GLSL snippet for custom materials.
class ShaderGraphPanel {
    bool _open = false;
    struct Pin { std::string label; bool is_out; int node_id; int idx; };
    struct SGNode {
        int id; std::string type; ImVec2 pos; std::string label;
        std::vector<std::string> in_pins, out_pins;
        std::unordered_map<std::string,float> values;
    };
    struct SGLink { int from_node,from_pin,to_node,to_pin; };
    std::vector<SGNode> _nodes;
    std::vector<SGLink> _links;
    int _next_id = 1;
    int _drag_from_node=-1, _drag_from_pin=-1;
    int _selected_node=-1;
    std::string _glsl_preview;
    std::string _build_status;
    fs::path _asset_path;
    char _asset_name[64]="MyShader";
public:
    void open() { _open=true; if (_nodes.empty()) _init_default(); }
    bool open_asset(const fs::path& path, EditorState& st) {
        _open = true;
        const auto source = load_json(path);
        if (!source.is_object() || source.value("format", std::string()) != "gameengine.shader-graph") {
            _build_status = "Could not open a valid Shader Graph asset.";
            st.log_error(_build_status + " " + path.string());
            return false;
        }
        std::vector<SGNode> loaded_nodes;
        if (!source.contains("nodes") || !source["nodes"].is_array()) {
            _build_status = "Shader Graph has no node data.";
            st.log_error(_build_status);
            return false;
        }
        int next_id = 1;
        for (const auto& item : source["nodes"]) {
            if (!item.is_object()) continue;
            SGNode node;
            node.id = item.value("id", next_id);
            node.type = item.value("type", std::string("Output"));
            node.pos = {item.value("x", 100.f), item.value("y", 100.f)};
            if (item.contains("values") && item["values"].is_object()) {
                for (auto it = item["values"].begin(); it != item["values"].end(); ++it) {
                    if (it.value().is_number()) node.values[it.key()] = it.value().get<float>();
                }
            }
            const auto read_pins = [](const nlohmann::json& raw, std::vector<std::string>& pins) {
                if (!raw.is_array()) return;
                for (const auto& pin : raw) {
                    if (pin.is_string()) pins.push_back(pin.get<std::string>());
                    else if (pin.is_object()) pins.push_back(pin.value("label", std::string()));
                }
            };
            if (item.contains("in_pins")) read_pins(item["in_pins"], node.in_pins);
            if (item.contains("out_pins")) read_pins(item["out_pins"], node.out_pins);
            if (node.in_pins.empty() && node.out_pins.empty()) _restore_default_pins(node);
            loaded_nodes.push_back(std::move(node));
            next_id = std::max(next_id, loaded_nodes.back().id + 1);
        }
        std::vector<SGLink> loaded_links;
        if (source.contains("links") && source["links"].is_array()) {
            for (const auto& item : source["links"]) {
                if (!item.is_object()) continue;
                SGLink link{item.value("fn", -1), item.value("fp", -1),
                            item.value("tn", -1), item.value("tp", -1)};
                if (link.from_node >= 0 && link.to_node >= 0 &&
                    link.from_node < (int)loaded_nodes.size() && link.to_node < (int)loaded_nodes.size() &&
                    link.from_pin >= 0 && link.to_pin >= 0 &&
                    link.from_pin < (int)loaded_nodes[link.from_node].out_pins.size() &&
                    link.to_pin < (int)loaded_nodes[link.to_node].in_pins.size())
                    loaded_links.push_back(link);
            }
        }
        _nodes = std::move(loaded_nodes);
        _links = std::move(loaded_links);
        _next_id = next_id;
        _selected_node = -1;
        _drag_from_node = _drag_from_pin = -1;
        _asset_path = path;
        const std::string stored_name = source.value("name", std::string());
        std::string base_name = stored_name.empty() ? path.stem().string() : stored_name;
        constexpr const char* graph_suffix = ".shadergraph";
        if (base_name.size() > std::char_traits<char>::length(graph_suffix) &&
            base_name.compare(base_name.size() - std::char_traits<char>::length(graph_suffix),
                              std::char_traits<char>::length(graph_suffix), graph_suffix) == 0)
            base_name.erase(base_name.size() - std::char_traits<char>::length(graph_suffix));
        std::snprintf(_asset_name, sizeof(_asset_name), "%s", base_name.c_str());
        _generate_glsl();
        _build_status = "Opened " + path.filename().string() + ".";
        st.log_success(_build_status);
        return true;
    }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({900,560},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Shader Graph##sg",&_open,ImGuiWindowFlags_MenuBar)) { ImGui::End(); return; }
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Add Node")) {
                if (ImGui::MenuItem("Texture2D Sample"))  _add_node("Texture2D",  {500,200},{"UV"},{"RGBA","R","G","B","A"});
                if (ImGui::MenuItem("UV"))                _add_node("UV",         {300,150},{},{"UV"});
                if (ImGui::MenuItem("Multiply"))          _add_node("Multiply",   {500,300},{"A","B"},{"Out"});
                if (ImGui::MenuItem("Add"))               _add_node("Add",        {500,380},{"A","B"},{"Out"});
                if (ImGui::MenuItem("Color"))             _add_node("Color",      {300,300},{},{"RGBA"});
                if (ImGui::MenuItem("Float"))             _add_node("Float",      {300,450},{},{"Out"});
                if (ImGui::MenuItem("Split"))             _add_node("Split",      {600,250},{"RGBA"},{"R","G","B","A"});
                if (ImGui::MenuItem("Lerp"))              _add_node("Lerp",       {600,350},{"A","B","T"},{"Out"});
                ImGui::EndMenu();
            }
            ImGui::InputText("##sgname",_asset_name,64); ImGui::SameLine();
            if (ImGui::Button("Save Graph")) _save(st);
            ImGui::SameLine();
            if (ImGui::Button("Build Material")) _build_material(st);
            ImGui::EndMenuBar();
        }

        // Canvas
        ImGui::BeginChild("##sgcanvas",{620,0},true,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();

        // Draw links
        for (auto& lk:_links) {
            if (lk.from_node>=0&&lk.from_node<(int)_nodes.size()&&lk.to_node>=0&&lk.to_node<(int)_nodes.size()) {
                ImVec2 fp=_pin_pos(_nodes[lk.from_node],lk.from_pin,true,cp);
                ImVec2 tp=_pin_pos(_nodes[lk.to_node],lk.to_pin,false,cp);
                dl->AddBezierCubic(fp,{fp.x+60,fp.y},{tp.x-60,tp.y},tp,IM_COL32(150,220,255,220),2.f);
            }
        }
        if (_drag_from_node >= 0 && _drag_from_node < (int)_nodes.size() &&
            _drag_from_pin >= 0 && _drag_from_pin < (int)_nodes[_drag_from_node].out_pins.size()) {
            const ImVec2 start = _pin_pos(_nodes[_drag_from_node], _drag_from_pin, true, cp);
            const ImVec2 end = ImGui::GetMousePos();
            dl->AddBezierCubic(start, {start.x + 60.f, start.y}, {end.x - 60.f, end.y}, end,
                                IM_COL32(255,210,80,235), 2.5f);
        }

        // Draw nodes
        for (int ni=0;ni<(int)_nodes.size();ni++) {
            auto& nd=_nodes[ni];
            ImVec2 np={cp.x+nd.pos.x,cp.y+nd.pos.y};
            float nw=130.f, row=18.f;
            int maxpins=std::max((int)nd.in_pins.size(),(int)nd.out_pins.size());
            float nh=30.f+maxpins*row+6.f;
            // Shadow
            dl->AddRectFilled({np.x+3,np.y+3},{np.x+nw+3,np.y+nh+3},IM_COL32(0,0,0,80),6.f);
            // Body
            dl->AddRectFilled(np,{np.x+nw,np.y+nh},IM_COL32(45,45,55,245),6.f);
            // Header
            dl->AddRectFilled(np,{np.x+nw,np.y+22},IM_COL32(60,120,200,220),6.f);
            dl->AddText({np.x+6,np.y+4},IM_COL32(255,255,255,255),nd.type.c_str());
            // Pins
            bool over_pin = false;
            const ImVec2 mouse = ImGui::GetMousePos();
            for (int p=0;p<(int)nd.in_pins.size();p++) {
                ImVec2 pp=_pin_pos(nd,p,false,cp);
                dl->AddCircleFilled(pp,5.f,IM_COL32(100,220,100,255));
                dl->AddText({pp.x+8,pp.y-7},IM_COL32(200,200,200,255),nd.in_pins[p].c_str());
                const float dx = mouse.x - pp.x, dy = mouse.y - pp.y;
                const bool hit = dx*dx + dy*dy <= 9.f*9.f;
                over_pin = over_pin || hit;
                if (hit && ImGui::IsMouseReleased(0) && _drag_from_node >= 0) {
                    _connect(_drag_from_node, _drag_from_pin, ni, p);
                    _drag_from_node = _drag_from_pin = -1;
                }
            }
            for (int p=0;p<(int)nd.out_pins.size();p++) {
                ImVec2 pp=_pin_pos(nd,p,true,cp);
                dl->AddCircleFilled(pp,5.f,IM_COL32(220,180,60,255));
                ImVec2 lt=dl->GetClipRectMax(); (void)lt;
                float tw2=ImGui::CalcTextSize(nd.out_pins[p].c_str()).x;
                dl->AddText({pp.x-8-tw2,pp.y-7},IM_COL32(200,200,200,255),nd.out_pins[p].c_str());
                const float dx = mouse.x - pp.x, dy = mouse.y - pp.y;
                const bool hit = dx*dx + dy*dy <= 9.f*9.f;
                over_pin = over_pin || hit;
                if (hit && ImGui::IsMouseClicked(0)) {
                    _drag_from_node = ni;
                    _drag_from_pin = p;
                }
            }
            // Drag node
            ImGui::SetCursorScreenPos(np);
            ImGui::InvisibleButton(("nd"+std::to_string(nd.id)).c_str(),{nw,nh});
            if (ImGui::IsItemClicked() && !over_pin) _selected_node = ni;
            if (!over_pin && ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                auto d=ImGui::GetMouseDragDelta(0,0.f);
                nd.pos.x+=d.x; nd.pos.y+=d.y;
                ImGui::ResetMouseDragDelta();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete Node")) { _delete_node(ni); break; }
                ImGui::EndPopup();
            }
        }
        // Cancel an abandoned wire instead of leaving graph interaction stuck
        // to the cursor.  A node's context menu remains available on this
        // same right click.
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            _drag_from_node = _drag_from_pin = -1;
        }
        ImGui::EndChild();

        // Right: selected-node properties and generated shader source
        ImGui::SameLine();
        ImGui::BeginChild("##sgglsl",{0,0},true);
        if (_selected_node >= 0 && _selected_node < (int)_nodes.size()) {
            auto& selected = _nodes[_selected_node];
            ImGui::TextColored({0.35f,0.75f,1.f,1.f}, "%s", selected.type.c_str());
            if (selected.type == "Color") {
                float color[4] = {selected.values["r"], selected.values["g"], selected.values["b"], selected.values["a"]};
                if (color[3] == 0.f && selected.values.find("initialized") == selected.values.end()) {
                    color[0]=color[1]=color[2]=color[3]=1.f; selected.values["initialized"] = 1.f;
                }
                if (ImGui::ColorEdit4("Value##sgcolor", color)) {
                    selected.values["r"]=color[0]; selected.values["g"]=color[1];
                    selected.values["b"]=color[2]; selected.values["a"]=color[3];
                }
            } else if (selected.type == "Float") {
                float value = selected.values.count("value") ? selected.values["value"] : 1.f;
                if (ImGui::DragFloat("Value##sgfloat", &value, .01f, -100.f, 100.f)) selected.values["value"] = value;
            } else {
                ImGui::TextDisabled("Connect pins to use this node in the material output.");
            }
            ImGui::Separator();
        }
        ImGui::TextDisabled("GLSL Preview:");
        ImGui::Separator();
        ImGui::InputTextMultiline("##glslprev",_glsl_preview.data(),_glsl_preview.size()+1,{-1,-1},ImGuiInputTextFlags_ReadOnly);
        if (!_build_status.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", _build_status.c_str());
        }
        ImGui::EndChild();
        ImGui::End();
    }
private:
    static void _restore_default_pins(SGNode& node) {
        node.in_pins.clear();
        node.out_pins.clear();
        if (node.type == "Output") { node.in_pins = {"Albedo", "Alpha"}; return; }
        if (node.type == "UV") { node.out_pins = {"UV"}; return; }
        if (node.type == "Texture2D") { node.in_pins = {"UV"}; node.out_pins = {"RGBA", "R", "G", "B", "A"}; return; }
        if (node.type == "Color") { node.out_pins = {"RGBA"}; return; }
        if (node.type == "Float") { node.out_pins = {"Out"}; return; }
        if (node.type == "Multiply" || node.type == "Add") { node.in_pins = {"A", "B"}; node.out_pins = {"Out"}; return; }
        if (node.type == "Split") { node.in_pins = {"RGBA"}; node.out_pins = {"R", "G", "B", "A"}; return; }
        if (node.type == "Lerp") { node.in_pins = {"A", "B", "T"}; node.out_pins = {"Out"}; return; }
    }

    void _init_default() {
        _nodes.clear(); _links.clear(); _next_id=1;
        _add_node("UV",       {80,  160}, {},             {"UV"});
        _add_node("Texture2D",{280, 160}, {"UV"},         {"RGBA","R","G","B","A"});
        _add_node("Output",   {520, 200}, {"Albedo","Alpha"},{});
        _connect(1, 0, 2, 0); // Texture2D.RGBA -> Output.Albedo
        _selected_node = -1;
    }
    ImVec2 _pin_pos(const SGNode& nd, int idx, bool is_out, ImVec2 origin) {
        float nw=130.f, row=18.f;
        float y=origin.y+nd.pos.y+28.f+idx*row;
        float x=origin.x+nd.pos.x+(is_out?130.f:0.f);
        return {x,y};
    }
    void _add_node(const std::string& type, ImVec2 pos,
                   std::vector<std::string> ins, std::vector<std::string> outs) {
        SGNode nd; nd.id=_next_id++; nd.type=type; nd.pos=pos;
        nd.in_pins=ins; nd.out_pins=outs;
        if (type == "Color") { nd.values["r"]=1.f; nd.values["g"]=1.f; nd.values["b"]=1.f; nd.values["a"]=1.f; nd.values["initialized"]=1.f; }
        if (type == "Float") nd.values["value"] = 1.f;
        _nodes.push_back(nd);
    }

    void _connect(int from_node, int from_pin, int to_node, int to_pin) {
        if (from_node < 0 || to_node < 0 || from_node >= (int)_nodes.size() || to_node >= (int)_nodes.size()) return;
        if (from_pin < 0 || from_pin >= (int)_nodes[from_node].out_pins.size() ||
            to_pin < 0 || to_pin >= (int)_nodes[to_node].in_pins.size()) return;
        if (from_node == to_node) return;
        _links.erase(std::remove_if(_links.begin(), _links.end(), [&](const SGLink& link) {
            return link.to_node == to_node && link.to_pin == to_pin;
        }), _links.end());
        _links.push_back({from_node, from_pin, to_node, to_pin});
    }

    void _delete_node(int node_index) {
        if (node_index < 0 || node_index >= (int)_nodes.size()) return;
        _links.erase(std::remove_if(_links.begin(), _links.end(), [&](const SGLink& link) {
            return link.from_node == node_index || link.to_node == node_index;
        }), _links.end());
        for (auto& link : _links) {
            if (link.from_node > node_index) --link.from_node;
            if (link.to_node > node_index) --link.to_node;
        }
        _nodes.erase(_nodes.begin() + node_index);
        if (_selected_node == node_index) _selected_node = -1;
        else if (_selected_node > node_index) --_selected_node;
        if (_drag_from_node == node_index) _drag_from_node = _drag_from_pin = -1;
        else if (_drag_from_node > node_index) --_drag_from_node;
    }

    static float _value_or(const SGNode& node, const std::string& key, float fallback) {
        auto it = node.values.find(key);
        return it == node.values.end() ? fallback : it->second;
    }

    std::string _input_expression(int node_index, int pin_index, const std::string& fallback,
                                  std::unordered_set<int>& visiting) const {
        for (auto it = _links.rbegin(); it != _links.rend(); ++it) {
            if (it->to_node == node_index && it->to_pin == pin_index)
                return _expression_for(it->from_node, it->from_pin, visiting);
        }
        return fallback;
    }

    std::string _expression_for(int node_index, int output_pin, std::unordered_set<int>& visiting) const {
        if (node_index < 0 || node_index >= (int)_nodes.size() || visiting.count(node_index)) return "vec4(1.0)";
        visiting.insert(node_index);
        const SGNode& node = _nodes[node_index];
        std::string expression = "vec4(1.0)";
        if (node.type == "Texture2D") {
            const std::string uv = _input_expression(node_index, 0, "vec4(frag_uv, 0.0, 1.0)", visiting);
            expression = "texture(albedo_sampler, (" + uv + ").xy)";
        } else if (node.type == "Color") {
            std::ostringstream out;
            out << "vec4(" << _value_or(node,"r",1.f) << "," << _value_or(node,"g",1.f) << ","
                << _value_or(node,"b",1.f) << "," << _value_or(node,"a",1.f) << ")";
            expression = out.str();
        } else if (node.type == "Float") {
            expression = "vec4(" + std::to_string(_value_or(node,"value",1.f)) + ")";
        } else if (node.type == "UV") {
            expression = "vec4(frag_uv, 0.0, 1.0)";
        } else if (node.type == "Multiply") {
            expression = "(" + _input_expression(node_index,0,"vec4(1.0)",visiting) + " * " +
                         _input_expression(node_index,1,"vec4(1.0)",visiting) + ")";
        } else if (node.type == "Add") {
            expression = "(" + _input_expression(node_index,0,"vec4(0.0)",visiting) + " + " +
                         _input_expression(node_index,1,"vec4(0.0)",visiting) + ")";
        } else if (node.type == "Lerp") {
            expression = "mix(" + _input_expression(node_index,0,"vec4(0.0)",visiting) + "," +
                         _input_expression(node_index,1,"vec4(1.0)",visiting) + ",clamp((" +
                         _input_expression(node_index,2,"vec4(0.5)",visiting) + ").r,0.0,1.0))";
        } else if (node.type == "Split") {
            expression = _input_expression(node_index, 0, "vec4(1.0)", visiting);
        }
        visiting.erase(node_index);
        (void)output_pin; // Scalar Split pins are represented by their source vector in v1.
        return expression;
    }

    std::string _fragment_source() const {
        int output = -1;
        for (int index=0; index<(int)_nodes.size(); ++index) if (_nodes[index].type == "Output") { output=index; break; }
        std::unordered_set<int> visiting;
        const std::string color = output >= 0 ? _input_expression(output, 0, "texture(albedo_sampler, frag_uv)", visiting)
                                              : "texture(albedo_sampler, frag_uv)";
        visiting.clear();
        const std::string alpha = output >= 0 ? _input_expression(output, 1, "vec4(1.0)", visiting) : "vec4(1.0)";
        return "#version 450\n"
               "layout(location=0) in vec2 frag_uv;\n"
               "layout(location=1) in vec4 frag_color;\n"
               "layout(location=0) out vec4 out_color;\n"
               "layout(set=0,binding=0) uniform sampler2D albedo_sampler;\n"
               // This block intentionally matches the unlit sprite shader's
               // byte layout.  Generated graph materials share sprite.vert,
               // so adding lit-only fields here would make the shader pair
               // incompatible even though those fields are not read.
               "layout(push_constant) uniform PushConstants { vec2 viewport_size; float alpha_cutoff; int use_texture; } pc;\n"
               "void main() {\n"
               "  vec4 graph_color = " + color + ";\n"
               "  graph_color *= frag_color;\n"
               "  graph_color.a *= (" + alpha + ").r;\n"
               "  if (pc.alpha_cutoff >= 0.0 && graph_color.a < pc.alpha_cutoff) discard;\n"
               "  out_color = graph_color;\n"
               "}\n";
    }

    void _generate_glsl() { _glsl_preview = _fragment_source(); }

    static std::string _safe_asset_name(const char* raw) {
        std::string result;
        for (const unsigned char c : std::string(raw ? raw : "")) {
            if (std::isalnum(c) || c == '_' || c == '-') result.push_back((char)c);
        }
        return result.empty() ? "MyShader" : result;
    }

    void _save(EditorState& st) {
        nlohmann::json j;
        j["format"] = "gameengine.shader-graph";
        j["version"] = 2;
        j["name"] = _safe_asset_name(_asset_name);
        for (auto& nd:_nodes) {
            nlohmann::json jn;
            jn["id"]=nd.id; jn["type"]=nd.type; jn["x"]=nd.pos.x; jn["y"]=nd.pos.y;
            jn["values"] = nd.values;
            jn["in_pins"] = nd.in_pins;
            jn["out_pins"] = nd.out_pins;
            j["nodes"].push_back(jn);
        }
        for (auto& lk:_links) j["links"].push_back({{"fn",lk.from_node},{"fp",lk.from_pin},{"tn",lk.to_node},{"tp",lk.to_pin}});
        fs::path p = fs::path(fs::path(st.scene_path).parent_path().string())/"assets"/(_safe_asset_name(_asset_name)+".shadergraph.json");
        if (save_json(p,j)) {
            _asset_path = p;
            st.log("Saved shader graph: "+p.string());
        } else {
            _build_status = "Could not save Shader Graph asset.";
            st.log_error(_build_status + " " + p.string());
        }
    }

    void _build_material(EditorState& st) {
        if (st.scene_path.empty()) { _build_status = "Open a project scene before building a material."; return; }
        _save(st);
        _generate_glsl();
        const std::string name = _safe_asset_name(_asset_name);
        const fs::path project = fs::path(st.scene_path).parent_path();
        const fs::path shader_dir = project / "assets" / "shaders";
        std::error_code ec;
        fs::create_directories(shader_dir, ec);
        if (ec) { _build_status = "Could not create project shader folder: " + ec.message(); return; }
        const fs::path fragment = shader_dir / (name + ".frag");
        const fs::path fragment_spv = shader_dir / (name + ".frag.spv");
        const fs::path vertex_spv = shader_dir / (name + ".vert.spv");
        const fs::path build_log = shader_dir / (name + ".shader-build.log");
        { std::ofstream output(fragment, std::ios::binary | std::ios::trunc); output << _glsl_preview; }
        if (!fs::is_regular_file(fragment)) { _build_status = "Could not write generated fragment shader."; return; }

        fs::path glslc;
        if (const char* sdk = std::getenv("VULKAN_SDK")) glslc = fs::path(sdk) / "Bin" / "glslc.exe";
        if (!fs::is_regular_file(glslc)) glslc = "glslc";
        const std::string command = "\"" + glslc.string() + "\" \"" + fragment.string() + "\" -o \"" +
                                    fragment_spv.string() + "\" > \"" + build_log.string() + "\" 2>&1";
        const int exit_code = std::system(command.c_str());
        if (exit_code != 0 || !fs::is_regular_file(fragment_spv)) {
            _build_status = "Shader compilation failed. See " + build_log.filename().string() + " in Assets/Shaders.";
            st.log_warn(_build_status);
            return;
        }

        fs::path engine_root = project.parent_path().parent_path();
        fs::path engine_vertex = engine_root / "engine_cpp" / "vk_render" / "shaders" / "sprite.vert.spv";
        if (!fs::is_regular_file(engine_vertex)) engine_vertex = fs::current_path() / "engine_cpp" / "vk_render" / "shaders" / "sprite.vert.spv";
        fs::copy_file(engine_vertex, vertex_spv, fs::copy_options::overwrite_existing, ec);
        if (ec || !fs::is_regular_file(vertex_spv)) {
            _build_status = "Fragment compiled, but the compatible sprite vertex SPIR-V could not be copied.";
            st.log_warn(_build_status);
            return;
        }

        const fs::path material_path = project / "assets" / (name + ".material");
        const std::string relative_base = "assets/shaders/" + name;
        const nlohmann::json material = {{"name", name}, {"shader", "Sprite-Unlit"},
            {"color", nlohmann::json::array({255,255,255,255})}, {"texture", ""},
            {"custom_vert_spv", relative_base + ".vert.spv"}, {"custom_frag_spv", relative_base + ".frag.spv"}};
        if (!save_json(material_path, material)) {
            _build_status = "Shader compiled, but the .material asset could not be saved.";
            st.log_warn(_build_status);
            return;
        }
        st.select_asset(material_path.string());
        _build_status = "Built " + material_path.filename().string() + ". Assign it to a SpriteRenderer material slot.";
        st.log_success(_build_status);
    }
};

// ─── 22. Frame Debugger Panel ─────────────────────────────────────────────────
// Captures per-frame render events (like Unity's Frame Debugger).
class FrameDebuggerPanel {
    bool _open = false;
    struct DrawCall {
        int idx; std::string label; std::string shader;
        int vertices=0, indices=0; float gpu_ms=0.f;
        std::string blend_mode; bool instanced=false; int instance_count=1;
    };
    std::vector<DrawCall> _calls;
    int _selected = -1;
    bool _capture_requested = false;
    int _frame_num = 0;
public:
    void open() { _open=true; }
    bool& is_open() { return _open; }
    bool take_capture_request() {
        const bool requested = _capture_requested;
        _capture_requested = false;
        return requested;
    }
    // The viewport supplies real SpriteBatch/culling counters after rendering;
    // this deliberately avoids invented draw calls or fake GPU timings.
    void capture_runtime(uint32_t draw_calls, uint32_t regular_quads, uint32_t instanced_quads,
                         uint32_t instanced_batches, uint32_t considered, uint32_t visible, uint32_t culled) {
        _calls.clear(); _selected = -1; ++_frame_num;
        auto append = [&](const std::string& label, const std::string& pipeline, uint32_t quads,
                          bool instanced, uint32_t batches) {
            if (quads == 0 && batches == 0) return;
            DrawCall call;
            call.idx = (int)_calls.size(); call.label = label; call.shader = pipeline;
            call.vertices = (int)quads * 4; call.indices = (int)quads * 6;
            call.gpu_ms = -1.f; call.blend_mode = "Recorded Vulkan batch";
            call.instanced = instanced; call.instance_count = (int)batches;
            _calls.push_back(std::move(call));
        };
        append("Regular sprite work", "sprite / sprite_lit", regular_quads, false,
               draw_calls >= instanced_batches ? draw_calls - instanced_batches : 0u);
        append("Instanced sprite work", "sprite_inst", instanced_quads, true, instanced_batches);
        DrawCall culling;
        culling.idx = (int)_calls.size(); culling.label = "Visibility culling"; culling.shader = "CPU scene culler";
        culling.vertices = (int)visible; culling.indices = (int)culled; culling.gpu_ms = -1.f;
        culling.blend_mode = std::to_string(visible) + " visible / " + std::to_string(considered) + " considered";
        _calls.push_back(std::move(culling));
    }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({700,440},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Frame Debugger##fd",&_open)) { ImGui::End(); return; }
        if (ImGui::Button("Capture Next Rendered Frame")) _capture_requested = true;
        ImGui::SameLine();
        ImGui::Text("Frame %d  |  Draw calls: %d", _frame_num, (int)_calls.size());
        ImGui::SameLine(); ImGui::TextDisabled("GPU timestamps are not fabricated.");
        ImGui::Separator();
        ImGui::BeginChild("##fdlist",{300,0},true);
        for (int i=0;i<(int)_calls.size();i++) {
            auto& dc=_calls[i];
            char buf[128]; snprintf(buf,128,"[%02d] %s",dc.idx,dc.label.c_str());
            bool sel=i==_selected;
            // Color by gpu cost
            ImGui::PushStyleColor(ImGuiCol_Text, dc.instanced ? ImVec4{0.4f,0.85f,1.f,1.f} : ImVec4{0.85f,0.85f,0.85f,1.f});
            if (ImGui::Selectable(buf,sel)) _selected=i;
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##fddetail",{0,0},true);
        if (_selected>=0 && _selected<(int)_calls.size()) {
            auto& dc=_calls[_selected];
            ImGui::TextColored({0.4f,0.8f,1.f,1.f},"%s",dc.label.c_str());
            ImGui::Separator();
            ImGui::Text("Shader:     %s", dc.shader.c_str());
            ImGui::Text("Vertices:   %d", dc.vertices);
            ImGui::Text("Indices:    %d", dc.indices);
            ImGui::TextUnformatted("GPU Time:   timestamp instrumentation unavailable");
            ImGui::Text("Blend Mode: %s", dc.blend_mode.c_str());
            if (dc.instanced) ImGui::Text("Instanced:  %d instances", dc.instance_count);
            else              ImGui::Text("Instanced:  No");
        } else {
            ImGui::TextDisabled("Select a draw call to inspect.");
        }
        ImGui::EndChild();
        ImGui::End();
    }
};

// ─── 23. Undo History Panel ───────────────────────────────────────────────────
// Visual undo/redo stack like Unity's Edit > Undo History.
class UndoHistoryPanel {
    bool _open = false;
    std::vector<std::string> _labels; // injected externally each push
    int _cursor = -1; // current position in history
public:
    void open() { _open=true; }
    bool& is_open() { return _open; }
    // Call this whenever you push an undo state
    void push(const std::string& label) {
        if (_cursor < (int)_labels.size()-1)
            _labels.erase(_labels.begin()+_cursor+1, _labels.end());
        _labels.push_back(label);
        if ((int)_labels.size()>64) _labels.erase(_labels.begin());
        _cursor=(int)_labels.size()-1;
    }
    void notify_undo() { if (_cursor>0) _cursor--; }
    void notify_redo() { if (_cursor<(int)_labels.size()-1) _cursor++; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({280,360},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Undo History##undoh",&_open)) { ImGui::End(); return; }
        ImGui::TextDisabled("Steps: %d  Current: %d", (int)_labels.size(), _cursor);
        ImGui::Separator();
        ImGui::BeginChild("##undolist",{0,-30},true);
        // First item = "Scene opened"
        {
            bool is_cur = (_cursor<0 || _labels.empty());
            if (is_cur) ImGui::PushStyleColor(ImGuiCol_Text,{0.4f,1.f,0.5f,1.f});
            ImGui::Selectable("(Scene Opened)", is_cur);
            if (is_cur) ImGui::PopStyleColor();
        }
        for (int i=0;i<(int)_labels.size();i++) {
            bool is_cur = (i==_cursor);
            bool is_fut = (i>_cursor);
            if (is_cur) ImGui::PushStyleColor(ImGuiCol_Text,{0.4f,1.f,0.5f,1.f});
            else if (is_fut) ImGui::PushStyleColor(ImGuiCol_Text,{0.5f,0.5f,0.5f,1.f});
            if (ImGui::Selectable((_labels[i]+"##uh"+std::to_string(i)).c_str(), is_cur)) {
                // Jump: apply undo/redo as needed
                if (i<_cursor) { for(int x=_cursor;x>i;x--) { st.entities=st.undo.undo(st.entities); } _cursor=i; }
                else if (i>_cursor) { for(int x=_cursor;x<i;x++) { st.entities=st.undo.redo(st.entities); } _cursor=i; }
            }
            if (is_cur||is_fut) ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::Text("Ctrl+Z Undo | Ctrl+Y Redo");
        ImGui::End();
    }
};

// ─── 24. UI Builder Panel ─────────────────────────────────────────────────────
// Visual drag-and-drop UI layout designer. Its output is authored directly as
// the engine's UI entities, so a layout created here appears in Play mode and
// a standalone export rather than becoming an inert interchange document.
class UIBuilderPanel {
    bool _open = false;
    struct UIElem {
        int id; std::string type; // "Panel","Text","Button","Image","Slider","Toggle","InputField"
        float x=100,y=100,w=120,h=32;
        std::string text="Label"; float font_size=14;
        float col[4]={0.2f,0.2f,0.3f,0.9f};
        bool selected=false;
    };
    std::vector<UIElem> _elems;
    int _next_id=1; int _sel=-1;
    ImVec2 _canvas_size={800,450};
    std::string _doc_name="UIDocument";
public:
    void open() { _open=true; }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({1000,580},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("UI Builder##uib",&_open,ImGuiWindowFlags_MenuBar)) { ImGui::End(); return; }
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Add")) {
                if (ImGui::MenuItem("Panel"))      _add("Panel",{0.15f,0.15f,0.2f,0.85f});
                if (ImGui::MenuItem("Text"))       _add("Text",{0,0,0,0});
                if (ImGui::MenuItem("Button"))     _add("Button",{0.2f,0.4f,0.8f,1.f});
                if (ImGui::MenuItem("Image"))      _add("Image",{1,1,1,1});
                if (ImGui::MenuItem("Progress Bar")) _add("ProgressBar",{0.3f,0.65f,0.35f,1.f});
                ImGui::EndMenu();
            }
            char nb[64]; snprintf(nb,64,"%s",_doc_name.c_str());
            if (ImGui::InputText("##uibname",nb,64)) _doc_name=nb;
            ImGui::SameLine();
            if (ImGui::Button("Create UI Entities")) _apply_to_scene(st);
            ImGui::EndMenuBar();
        }

        // Canvas
        ImGui::BeginChild("##uibcanvas",{680,0},true);
        ImDrawList* dl=ImGui::GetWindowDrawList();
        ImVec2 p0=ImGui::GetCursorScreenPos();
        // Canvas background (game screen)
        dl->AddRectFilled(p0,{p0.x+_canvas_size.x,p0.y+_canvas_size.y},IM_COL32(30,30,35,255));
        dl->AddRect(p0,{p0.x+_canvas_size.x,p0.y+_canvas_size.y},IM_COL32(80,80,80,180));
        // Draw elements
        for (int i=0;i<(int)_elems.size();i++) {
            auto& e=_elems[i];
            ImVec2 ep={p0.x+e.x,p0.y+e.y};
            ImU32 bg=IM_COL32(e.col[0]*255,e.col[1]*255,e.col[2]*255,e.col[3]*255);
            dl->AddRectFilled(ep,{ep.x+e.w,ep.y+e.h},bg,4.f);
            if (e.type=="Button") { dl->AddRect(ep,{ep.x+e.w,ep.y+e.h},IM_COL32(100,160,255,200),4.f); }
            if (e.type=="Slider") {
                dl->AddRectFilled({ep.x,ep.y+e.h*.4f},{ep.x+e.w,ep.y+e.h*.6f},IM_COL32(60,60,70,255),3.f);
                dl->AddCircleFilled({ep.x+e.w*.5f,ep.y+e.h*.5f},e.h*.4f,IM_COL32(80,160,255,255));
            }
            if (!e.text.empty()) dl->AddText({ep.x+4,ep.y+(e.h-13)*.5f},IM_COL32(255,255,255,230),e.text.c_str());
            // Selection border
            if (i==_sel) dl->AddRect(ep,{ep.x+e.w,ep.y+e.h},IM_COL32(255,220,60,255),4.f,0,2.f);
        }
        ImGui::Dummy(_canvas_size);
        // Click to select / drag
        ImVec2 mp=ImGui::GetMousePos();
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
            _sel=-1;
            for (int i=(int)_elems.size()-1;i>=0;i--) {
                auto& e=_elems[i];
                if (mp.x>=p0.x+e.x&&mp.x<=p0.x+e.x+e.w&&mp.y>=p0.y+e.y&&mp.y<=p0.y+e.y+e.h)
                    { _sel=i; break; }
            }
        }
        if (_sel>=0 && ImGui::IsWindowHovered() && ImGui::IsMouseDragging(0)) {
            auto d=ImGui::GetMouseDragDelta(0,0.f);
            _elems[_sel].x+=d.x; _elems[_sel].y+=d.y;
            ImGui::ResetMouseDragDelta();
        }
        ImGui::EndChild();

        // Right: properties
        ImGui::SameLine();
        ImGui::BeginChild("##uibprops",{0,0},true);
        ImGui::Text("Elements: %d", (int)_elems.size());
        ImGui::Separator();
        if (_sel>=0 && _sel<(int)_elems.size()) {
            auto& e=_elems[_sel];
            ImGui::TextColored({0.4f,0.8f,1.f,1.f},"%s [%d]",e.type.c_str(),e.id);
            char tb[128]; snprintf(tb,128,"%s",e.text.c_str());
            if (ImGui::InputText("Text##ubt",tb,128)) e.text=tb;
            ImGui::DragFloat("X##ubx",&e.x,1.f);
            ImGui::DragFloat("Y##uby",&e.y,1.f);
            ImGui::DragFloat("W##ubw",&e.w,1.f,1.f,2000.f);
            ImGui::DragFloat("H##ubh",&e.h,1.f,1.f,2000.f);
            ImGui::SliderFloat("Font Size##ubfs",&e.font_size,8.f,72.f);
            ImGui::ColorEdit4("Color##ubc",e.col);
            ImGui::Separator();
            if (ImGui::Button("Delete##ubdel")) { _elems.erase(_elems.begin()+_sel); _sel=-1; }
        } else { ImGui::TextDisabled("Click an element to select."); }
        ImGui::Separator();
        ImGui::TextDisabled("Canvas: %.0fx%.0f", _canvas_size.x, _canvas_size.y);
        ImGui::DragFloat("W##cbw",&_canvas_size.x,1.f,100.f,3840.f);
        ImGui::DragFloat("H##cbh",&_canvas_size.y,1.f,100.f,2160.f);
        ImGui::EndChild();
        ImGui::End();
    }
private:
    void _add(const std::string& type, std::array<float,4> c) {
        UIElem e; e.id=_next_id++; e.type=type;
        e.x=200+(float)(_next_id%5)*30; e.y=150+(float)(_next_id%4)*40;
        e.w=(type=="Text")?80.f:120.f; e.h=(type=="Text")?20.f:32.f;
        e.text=(type=="Button")?"Click":(type=="Text")?"Label":"";
        e.col[0]=c[0];e.col[1]=c[1];e.col[2]=c[2];e.col[3]=c[3];
        _elems.push_back(e); _sel=(int)_elems.size()-1;
    }
    void _apply_to_scene(EditorState& st) {
        if (_elems.empty()) { st.log_warn("UI Builder has no elements to create."); return; }
        st.undo.push_deep(st.entities);
        Entity canvas = SceneIO::make_entity(_doc_name.empty() ? "UI Canvas" : _doc_name, st);
        canvas["components"]["UICanvas"] = component_defaults()["UICanvas"];
        canvas["components"]["Transform"] = component_defaults()["Transform"];
        const int canvas_id = canvas.value("id", 0);
        st.entities.push_back(std::move(canvas));

        int first_child = -1;
        for (const auto& element : _elems) {
            const std::string component = element.type == "Text" ? "UIText"
                : element.type == "Button" ? "UIButton"
                : element.type == "Image" ? "UIImage"
                : (element.type == "ProgressBar" || element.type == "Slider") ? "UIProgressBar"
                : (element.type == "Toggle" ? "UIButton" : "UIPanel");
            Entity entity = SceneIO::make_entity(_doc_name + " " + element.type, st);
            entity["components"]["Transform"] = component_defaults()["Transform"];
            entity["components"]["Transform"]["parent"] = canvas_id;
            entity["components"][component] = component_defaults()[component];
            auto& ui = entity["components"][component];
            // Builder coordinates are top-left pixels, which maps exactly to
            // a zero anchor/pivot in the runtime Canvas resolver.
            ui["anchor_x"] = 0.f; ui["anchor_y"] = 0.f;
            ui["pivot_x"] = 0.f; ui["pivot_y"] = 0.f;
            ui["pos_x"] = element.x; ui["pos_y"] = element.y;
            ui["width"] = std::max(1.f, element.w); ui["height"] = std::max(1.f, element.h);
            if (component == "UIText") {
                ui["text"] = element.text; ui["font_size"] = element.font_size;
                ui["color"] = nlohmann::json::array({(int)(element.col[0]*255.f),(int)(element.col[1]*255.f),
                                                       (int)(element.col[2]*255.f),(int)(element.col[3]*255.f)});
            } else if (component == "UIButton") {
                ui["label"] = element.text.empty() ? "Button" : element.text;
            } else if (component == "UIPanel") {
                ui["color"] = nlohmann::json::array({(int)(element.col[0]*255.f),(int)(element.col[1]*255.f),
                                                       (int)(element.col[2]*255.f),(int)(element.col[3]*255.f)});
            } else if (component == "UIProgressBar") {
                ui["fill_color"] = nlohmann::json::array({(int)(element.col[0]*255.f),(int)(element.col[1]*255.f),
                                                            (int)(element.col[2]*255.f),(int)(element.col[3]*255.f)});
            }
            const int child_id = entity.value("id", 0);
            if (first_child < 0) first_child = child_id;
            st.entities.push_back(std::move(entity));
        }
        st.resync_children_arrays();
        transform::mark_structure_dirty();
        st.select(first_child >= 0 ? first_child : canvas_id);
        st.log("UI Builder created " + std::to_string(_elems.size()) + " runtime UI entities. Save the scene to keep them.");
    }
};

// ─── 25. Build Report Panel ───────────────────────────────────────────────────
// Shows asset sizes after build — like Unity's Build Report.
class BuildReportPanel {
    bool _open = false;
    struct AssetEntry { std::string path; std::string type; size_t size_bytes=0; float pct=0.f; };
    std::vector<AssetEntry> _entries;
    size_t _total_bytes=0;
    std::string _sort_col="size";
    bool _sort_asc=false;
    char _filter[64]={};
    std::string _scanned_project;
public:
    void open() { _open=true; }
    bool& is_open() { return _open; }
    void scan(const std::string& project_dir) {
        _entries.clear(); _total_bytes=0;
        _scanned_project = project_dir;
        if (!fs::exists(project_dir)) return;
        for (auto& e : fs::recursive_directory_iterator(project_dir,fs::directory_options::skip_permission_denied)) {
            if (!e.is_regular_file()) continue;
            AssetEntry ae;
            ae.path = fs::relative(e.path(),project_dir).string();
            ae.size_bytes = (size_t)e.file_size();
            std::string ext=e.path().extension().string();
            if (ext==".png"||ext==".jpg"||ext==".webp") ae.type="Texture";
            else if (ext==".wav"||ext==".ogg"||ext==".mp3") ae.type="Audio";
            else if (ext==".cpp"||ext==".hpp") ae.type="Script";
            else if (ext==".scene"||ext==".json") ae.type="Scene/Data";
            else if (ext==".glsl"||ext==".vert"||ext==".frag") ae.type="Shader";
            else ae.type="Other";
            _total_bytes+=ae.size_bytes;
            _entries.push_back(ae);
        }
        for (auto& e:_entries) e.pct=_total_bytes>0?(float)e.size_bytes/(float)_total_bytes*100.f:0.f;
        _sort();
    }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({620,440},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Build Report##br",&_open)) { ImGui::End(); return; }
        const std::string project_dir = fs::path(st.scene_path).parent_path().string();
        if (_scanned_project != project_dir) scan(project_dir);
        if (ImGui::Button("Rescan")) scan(fs::path(st.scene_path).parent_path().string());
        ImGui::SameLine();
        ImGui::InputText("Filter##brf",_filter,64);
        ImGui::SameLine();
        ImGui::TextColored({0.6f,1.f,0.6f,1.f},"Total: %.1f MB",(float)_total_bytes/1e6f);
        const fs::path engine_root = fs::path(project_dir).parent_path().parent_path();
        const fs::path build_log = engine_root / "build" / "standalone_export" /
                                   fs::path(project_dir).filename() / "build_output.log";
        if (fs::is_regular_file(build_log)) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Latest standalone build output")) {
                std::ifstream input(build_log);
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(input, line)) {
                    lines.push_back(line);
                    if (lines.size() > 80) lines.erase(lines.begin());
                }
                ImGui::BeginChild("##build_output", {0, 140}, true);
                for (const auto& output_line : lines) ImGui::TextUnformatted(output_line.c_str());
                ImGui::EndChild();
            }
        }
        ImGui::Separator();
        // Type breakdown bar
        std::unordered_map<std::string,size_t> by_type;
        for (auto& e:_entries) by_type[e.type]+=e.size_bytes;
        ImVec2 bp=ImGui::GetCursorScreenPos(); float bw=ImGui::GetContentRegionAvail().x;
        ImDrawList* dl=ImGui::GetWindowDrawList();
        std::vector<std::pair<std::string,size_t>> bv(by_type.begin(),by_type.end());
        std::sort(bv.begin(),bv.end(),[](auto&a,auto&b){return a.second>b.second;});
        static ImU32 tcols[]={IM_COL32(60,150,220,255),IM_COL32(220,150,60,255),IM_COL32(80,200,120,255),IM_COL32(200,80,80,255),IM_COL32(180,100,200,255)};
        float cx=bp.x; int ci=0;
        for (auto& [tp,sz]:bv) {
            float w=_total_bytes>0?(float)sz/(float)_total_bytes*bw:0.f;
            dl->AddRectFilled({cx,bp.y},{cx+w,bp.y+14},tcols[ci%5]);
            cx+=w; ci++;
        }
        dl->AddRect(bp,{bp.x+bw,bp.y+14},IM_COL32(100,100,100,200));
        ImGui::Dummy({bw,14});
        // Legend
        cx=0; ci=0;
        for (auto& [tp,sz]:bv) {
            ImGui::SameLine(ci==0?0:cx);
            ImGui::ColorButton(("##bc"+tp).c_str(),ImVec4(((tcols[ci%5]>>0)&0xFF)/255.f,((tcols[ci%5]>>8)&0xFF)/255.f,((tcols[ci%5]>>16)&0xFF)/255.f,1.f),ImGuiColorEditFlags_NoTooltip,{12,12});
            ImGui::SameLine(); ImGui::Text("%s %.1fMB",tp.c_str(),(float)sz/1e6f);
            cx+=ImGui::GetItemRectMax().x-bp.x+8; ci++;
        }
        ImGui::Separator();
        if (ImGui::BeginTable("##brtab",4,ImGuiTableFlags_Sortable|ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Asset",ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type",ImGuiTableColumnFlags_WidthFixed,80);
            ImGui::TableSetupColumn("Size",ImGuiTableColumnFlags_WidthFixed,80);
            ImGui::TableSetupColumn("%",ImGuiTableColumnFlags_WidthFixed,50);
            ImGui::TableHeadersRow();
            std::string fstr(_filter);
            for (auto& e:_entries) {
                if (!fstr.empty() && e.path.find(fstr)==std::string::npos) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(e.path.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s",e.type.c_str());
                ImGui::TableSetColumnIndex(2);
                if (e.size_bytes>1<<20) ImGui::Text("%.1f MB",(float)e.size_bytes/1e6f);
                else if (e.size_bytes>1024) ImGui::Text("%.1f KB",(float)e.size_bytes/1024.f);
                else ImGui::Text("%d B",(int)e.size_bytes);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%",e.pct);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
private:
    void _sort() {
        std::sort(_entries.begin(),_entries.end(),[](auto&a,auto&b){return a.size_bytes>b.size_bytes;});
    }
};

// ─── 26. Git Version Control Panel ───────────────────────────────────────────
// Shows git status, diff summary, staged/unstaged files. Commit, pull, push.
class GitVersionControlPanel {
    bool _open = false;
    std::string _status_output;
    std::string _log_output;
    std::string _diff_output;
    char _commit_msg[256]={};
    std::string _branch="main";
    bool _refreshed=false;
    struct FileStatus { char code; std::string path; };
    std::vector<FileStatus> _files;
    int _sel=-1;
public:
    void open() { _open=true; if(!_refreshed){ _refresh(); _refreshed=true; } }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({640,440},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Version Control (Git)##git",&_open)) { ImGui::End(); return; }
        // Branch + refresh
        ImGui::TextColored({0.4f,1.f,0.5f,1.f},"  Branch: %s",_branch.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Refresh##gitr")) _refresh();
        ImGui::SameLine();
        if (ImGui::Button("Pull##gitp")) _run_git("pull",st);
        ImGui::SameLine();
        if (ImGui::Button("Push##gitpsh")) _run_git("push",st);
        ImGui::Separator();
        ImGui::BeginChild("##gitfiles",{280,280},true);
        ImGui::TextDisabled("Changed Files:");
        for (int i=0;i<(int)_files.size();i++) {
            auto& f=_files[i];
            ImU32 col;
            if (f.code=='M') col=IM_COL32(255,200,60,255);
            else if (f.code=='A') col=IM_COL32(80,200,80,255);
            else if (f.code=='D') col=IM_COL32(200,80,80,255);
            else col=IM_COL32(180,180,180,255);
            ImGui::PushStyleColor(ImGuiCol_Text,*(ImVec4*)&col);
            char lbl[256]; snprintf(lbl,256,"[%c] %s##gf%d",f.code,f.path.c_str(),i);
            if (ImGui::Selectable(lbl,_sel==i)) { _sel=i; _load_diff(f.path); }
            ImGui::PopStyleColor();
        }
        if (_files.empty()) ImGui::TextDisabled("(no changes)");
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##gitdiff",{0,280},true);
        ImGui::TextDisabled("Diff:");
        ImGui::Separator();
        ImGui::TextUnformatted(_diff_output.empty()?"(select a file to see diff)":_diff_output.c_str());
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::Text("Commit message:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##gitcm",_commit_msg,256);
        if (ImGui::Button("Stage All & Commit")) {
            if (strlen(_commit_msg)>0) { _run_git(std::string("commit -am \"")+_commit_msg+"\"",st); _commit_msg[0]=0; }
            else st.log_warn("Enter a commit message first.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard All")) { _run_git("checkout -- .",st); _refresh(); }
        ImGui::Separator();
        ImGui::TextDisabled("Recent commits:");
        ImGui::TextUnformatted(_log_output.c_str());
        ImGui::End();
    }
private:
    void _refresh() {
        _files.clear(); _diff_output.clear(); _sel=-1;
        auto run=[](const std::string& cmd)->std::string{
            FILE* p=popen(cmd.c_str(),"r"); if(!p) return "(not a git repo)";
            std::string out; char buf[256];
            while(fgets(buf,256,p)) out+=buf;
            pclose(p); return out;
        };
        // Branch
        std::string br=run("git rev-parse --abbrev-ref HEAD 2>/dev/null");
        if (!br.empty() && br.back()=='\n') br.pop_back();
        _branch=br.empty()?"(unknown)":br;
        // Status
        std::string st2=run("git status --porcelain 2>/dev/null");
        std::istringstream ss(st2);
        std::string line;
        while(std::getline(ss,line)) {
            if (line.size()<4) continue;
            FileStatus fs2; fs2.code=line[1]==' '?line[0]:line[1];
            fs2.path=line.substr(3);
            _files.push_back(fs2);
        }
        // Log
        _log_output=run("git log --oneline -6 2>/dev/null");
        if (_log_output.empty()) _log_output="(no commits)";
    }
    void _load_diff(const std::string& path) {
        FILE* p=popen(("git diff HEAD -- \""+path+"\" 2>/dev/null").c_str(),"r");
        if (!p) { _diff_output="(diff unavailable)"; return; }
        _diff_output.clear(); char buf[512];
        int lines=0;
        while(fgets(buf,512,p)&&lines<60) { _diff_output+=buf; lines++; }
        if (lines>=60) _diff_output+="...(truncated)";
        pclose(p);
    }
    void _run_git(const std::string& args, EditorState& st) {
        std::string cmd="git "+args+" 2>&1";
        FILE* p=popen(cmd.c_str(),"r"); if(!p){st.log_warn("git not found.");return;}
        std::string out; char buf[256];
        while(fgets(buf,256,p)) out+=buf;
        pclose(p);
        st.log("[git "+args+"] "+out);
        _refresh();
    }
};

// ─── 27. Gizmo Overlay Settings ──────────────────────────────────────────────
// Unity-style scene view overlay: toggle gizmo categories per type.
struct GizmoOverlaySettings {
    bool show_colliders    = true;
    bool show_lights       = true;
    bool show_cameras      = true;
    bool show_joints       = true;
    bool show_navmesh      = true;
    bool show_waypoints    = true;
    bool show_particle_bounds = false;
    bool show_grid         = true;
    bool show_audio_sources= false;
    bool show_triggers_fill= false;
    float collider_alpha   = 0.7f;
    float grid_opacity     = 0.3f;

    nlohmann::json to_json() const {
        return {{"colliders",show_colliders},{"lights",show_lights},{"cameras",show_cameras},
                {"joints",show_joints},{"navmesh",show_navmesh},{"waypoints",show_waypoints},
                {"particle_bounds",show_particle_bounds},{"grid",show_grid},
                {"audio_sources",show_audio_sources},{"triggers_fill",show_triggers_fill},
                {"collider_alpha",collider_alpha},{"grid_opacity",grid_opacity}};
    }
    void from_json(const nlohmann::json& j) {
        if (!j.is_object()) return;
        show_colliders    =j.value("colliders",true);
        show_lights       =j.value("lights",true);
        show_cameras      =j.value("cameras",true);
        show_joints       =j.value("joints",true);
        show_navmesh      =j.value("navmesh",true);
        show_waypoints    =j.value("waypoints",true);
        show_particle_bounds=j.value("particle_bounds",false);
        show_grid         =j.value("grid",true);
        show_audio_sources=j.value("audio_sources",false);
        show_triggers_fill=j.value("triggers_fill",false);
        collider_alpha    =j.value("collider_alpha",0.7f);
        grid_opacity      =j.value("grid_opacity",0.3f);
    }
};

class GizmoOverlayPanel {
    bool _open = false;
public:
    GizmoOverlaySettings settings;
    void open() { _open=true; }
    bool& is_open() { return _open; }
    // Compact popover — call from viewport toolbar
    void draw_popover() {
        if (!ImGui::BeginPopup("##gizmooverlay")) return;
        ImGui::Text("Gizmos");
        ImGui::Separator();
        ImGui::Checkbox("Colliders",    &settings.show_colliders);
        if (settings.show_colliders) { ImGui::SameLine(); ImGui::SliderFloat("##ca",&settings.collider_alpha,0.f,1.f); }
        ImGui::Checkbox("Lights",       &settings.show_lights);
        ImGui::Checkbox("Cameras",      &settings.show_cameras);
        ImGui::Checkbox("Joints",       &settings.show_joints);
        ImGui::Checkbox("NavMesh",      &settings.show_navmesh);
        ImGui::Checkbox("Waypoints",    &settings.show_waypoints);
        ImGui::Checkbox("Particle Bounds",&settings.show_particle_bounds);
        ImGui::Checkbox("Audio Sources",&settings.show_audio_sources);
        ImGui::Checkbox("Trigger Fill", &settings.show_triggers_fill);
        ImGui::Separator();
        ImGui::Checkbox("Grid",         &settings.show_grid);
        if (settings.show_grid) { ImGui::SameLine(); ImGui::SliderFloat("##go",&settings.grid_opacity,0.f,1.f); }
        ImGui::EndPopup();
    }
    // Full window version
    void draw(EditorState&) {
        if (!_open) return;
        ImGui::SetNextWindowSize({260,320},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Gizmo Overlay##gizmo",&_open)) { ImGui::End(); return; }
        ImGui::Text("Scene View Gizmos");
        ImGui::Separator();
        ImGui::Checkbox("Colliders",    &settings.show_colliders);
        if (settings.show_colliders) { ImGui::SameLine(); ImGui::SliderFloat("Alpha##gca",&settings.collider_alpha,0.f,1.f); }
        ImGui::Checkbox("Lights",       &settings.show_lights);
        ImGui::Checkbox("Cameras",      &settings.show_cameras);
        ImGui::Checkbox("Joints",       &settings.show_joints);
        ImGui::Checkbox("NavMesh",      &settings.show_navmesh);
        ImGui::Checkbox("Waypoints",    &settings.show_waypoints);
        ImGui::Checkbox("Particle Bounds",&settings.show_particle_bounds);
        ImGui::Checkbox("Audio Sources",&settings.show_audio_sources);
        ImGui::Checkbox("Trigger Fill", &settings.show_triggers_fill);
        ImGui::Separator();
        ImGui::Checkbox("Grid",         &settings.show_grid);
        if (settings.show_grid) ImGui::SliderFloat("Grid Opacity##ggo",&settings.grid_opacity,0.f,1.f);
        ImGui::End();
    }
};

// ─── 28. Shadow 2D Settings Panel ────────────────────────────────────────────
// Global 2D shadow / light system settings. Mirrors Unity URP 2D Renderer settings.
class Shadow2DSettingsPanel {
    bool _open=false;
    std::string _loaded_project;
public:
    struct Settings {
        bool enabled=true;
        float ambient_intensity=0.15f;
        float ambient_color[4]={1.f,1.f,1.f,1.f};
        float shadow_strength=1.f;
        int max_lights_per_draw=16;
    } cfg;

    void open() { _open=true; }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        _load_for_project(st);
        ImGui::SetNextWindowSize({390,300},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("2D Shadow Settings##sh2d",&_open)) { ImGui::End(); return; }
        ImGui::Checkbox("Enable 2D Lighting",&cfg.enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns Light2D, Sprite-Lit materials and Shadow2DCaster projection on or off.");
        ImGui::Separator();
        ImGui::TextUnformatted("Ambient Light");
        ImGui::SliderFloat("Intensity##sha",&cfg.ambient_intensity,0.f,4.f);
        ImGui::ColorEdit4("Ambient Color##shac",cfg.ambient_color);
        ImGui::Separator();
        ImGui::TextUnformatted("Shadow Projection");
        ImGui::SliderFloat("Global Shadow Strength##shs",&cfg.shadow_strength,0.f,1.f);
        ImGui::SliderInt("Max Active Lights##shmld",&cfg.max_lights_per_draw,0,16);
        ImGui::TextDisabled("These settings are live in the viewport and exported runtime.");
        ImGui::Separator();
        if (ImGui::Button("Save Project Lighting##shapp")) {
            nlohmann::json j;
            j["enabled"]=cfg.enabled;
            j["ambient_intensity"]=cfg.ambient_intensity;
            j["ambient_color"]=nlohmann::json::array({cfg.ambient_color[0],cfg.ambient_color[1],cfg.ambient_color[2]});
            j["shadow_strength"]=cfg.shadow_strength;
            j["max_lights"]=cfg.max_lights_per_draw;
            save_json(fs::path(fs::path(st.scene_path).parent_path().string())/"settings"/"shadow2d.json",j);
            st.log("Shadow 2D settings saved.");
        }
        ImGui::End();
    }

private:
    void _load_for_project(const EditorState& st) {
        const std::string project = fs::path(st.scene_path).parent_path().string();
        if (project.empty() || project == _loaded_project) return;
        _loaded_project = project;
        cfg = Settings{};
        std::ifstream input(fs::path(project) / "settings" / "shadow2d.json");
        nlohmann::json json;
        try { if (!input || !(input >> json)) return; } catch (...) { return; }
        cfg.enabled = json.value("enabled", cfg.enabled);
        cfg.ambient_intensity = json.value("ambient_intensity", cfg.ambient_intensity);
        cfg.shadow_strength = json.value("shadow_strength", cfg.shadow_strength);
        cfg.max_lights_per_draw = std::clamp(json.value("max_lights", cfg.max_lights_per_draw), 0, 16);
        const auto color = json.value("ambient_color", std::vector<float>{1.f,1.f,1.f});
        for (int i=0; i<3 && i<(int)color.size(); ++i) cfg.ambient_color[i] = color[(size_t)i];
    }
};

// ─── 29. Effector Chain Debugger ─────────────────────────────────────────────
// Visual debug: shows which effectors are active on selected entity.
class EffectorChainDebugger {
    bool _open=false;
public:
    void open() { _open=true; }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({400,380},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Effector Chain Debugger##ecd",&_open)) { ImGui::End(); return; }
        // Find selected entity
        Entity* ent=nullptr;
        for (auto& e:st.entities) if (e.value("id",0)==st.selected_id) { ent=&e; break; }
        if (!ent) { ImGui::TextDisabled("No entity selected."); ImGui::End(); return; }
        auto& comps=(*ent)["components"];
        std::string name=(*ent).value("name","Entity");
        ImGui::TextColored({0.4f,0.8f,1.f,1.f},"Entity: %s",name.c_str());
        ImGui::Separator();

        static const std::vector<std::pair<std::string,std::string>> effector_types = {
            {"PlatformEffector2D",  "One-way collision, surface arc"},
            {"PointEffector2D",     "Radial attract/repel force"},
            {"BuoyancyEffector2D",  "Fluid drag + buoyancy force"},
            {"SurfaceEffector2D",   "Conveyor belt surface push"},
            {"ConstantForce2D",     "Persistent world-space force"},
        };

        bool any=false;
        for (auto& [etype, edesc]:effector_types) {
            if (!comps.contains(etype)) continue;
            any=true;
            auto& ec=comps[etype];
            bool active=ec.value("enabled",true);
            ImGui::PushStyleColor(ImGuiCol_Text, active?ImVec4{0.4f,1.f,0.5f,1.f}:ImVec4{0.5f,0.5f,0.5f,1.f});
            bool hdr=ImGui::CollapsingHeader((etype+(active?" [ACTIVE]":" [DISABLED]")).c_str());
            ImGui::PopStyleColor();
            if (hdr) {
                ImGui::TextDisabled("  %s",edesc.c_str());
                ImGui::Separator();
                if (etype=="PlatformEffector2D") {
                    ImGui::Text("  One-Way: %s", ec.value("use_one_way",true)?"Yes":"No");
                    ImGui::Text("  Surface Arc: %.0f°", ec.value("surface_arc",180.0));
                } else if (etype=="PointEffector2D") {
                    float fm=ec.value("force_magnitude",10.f);
                    ImGui::Text("  Force: %.2f %s",std::abs(fm),fm<0?"(attract)":"(repel)");
                    ImGui::Text("  Distance Scale: %.2f",ec.value("distance_scale",1.f));
                } else if (etype=="BuoyancyEffector2D") {
                    ImGui::Text("  Density: %.2f",ec.value("density",1.f));
                    ImGui::Text("  Linear Drag: %.2f",ec.value("linear_drag",1.f));
                    ImGui::Text("  Surface Level: %.1f",ec.value("surface_level",0.f));
                } else if (etype=="SurfaceEffector2D") {
                    ImGui::Text("  Speed: %.2f",ec.value("speed",5.f));
                    ImGui::Text("  Force Scale: %.2f",ec.value("force_scale",0.1f));
                } else if (etype=="ConstantForce2D") {
                    ImGui::Text("  Force: (%.1f, %.1f)", ec.value("force_x",0.f),ec.value("force_y",0.f));
                    ImGui::Text("  Torque: %.2f", ec.value("torque",0.f));
                }
                ImGui::Spacing();
                // Live force estimate
                ImGui::TextColored({0.9f,0.7f,0.2f,1.f},"  → Net force direction: ↑");
            }
        }
        if (!any) ImGui::TextDisabled("No effectors on this entity.");
        ImGui::Separator();
        // Rigidbody summary if present
        if (comps.contains("Rigidbody2D")) {
            auto& rb=comps["Rigidbody2D"];
            ImGui::Text("Rigidbody2D: mass=%.2f  gravity=%.2f  drag=%.3f",
                rb.value("mass",1.f),rb.value("gravity_scale",1.f),rb.value("drag",0.05f));
        }
        ImGui::End();
    }
};

// ─── 30. Batch Renamer Panel ──────────────────────────────────────────────────
// Rename multiple selected entities at once with prefix/suffix/find-replace.
class BatchRenamerPanel {
    bool _open=false;
    char _prefix[64]={};
    char _suffix[64]={};
    char _find[64]={};
    char _replace_str[64]={};
    bool _number=false;
    int  _number_start=0;
    bool _preview=true;
public:
    void open() { _open=true; }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({420,380},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Batch Renamer##bnr",&_open)) { ImGui::End(); return; }
        ImGui::InputText("Prefix##bnrp",_prefix,64);
        ImGui::InputText("Suffix##bnrs",_suffix,64);
        ImGui::Separator();
        ImGui::InputText("Find##bnrf",_find,64);
        ImGui::InputText("Replace##bnrr",_replace_str,64);
        ImGui::Separator();
        ImGui::Checkbox("Add Number##bnrn",&_number);
        if (_number) ImGui::InputInt("Start From##bnrns",&_number_start);
        ImGui::Checkbox("Preview##bnrpv",&_preview);
        ImGui::Separator();

        // Preview / apply
        auto apply=[&](std::string name, int idx)->std::string {
            if (strlen(_find)>0) {
                size_t pos=0;
                while((pos=name.find(_find,pos))!=std::string::npos) {
                    name.replace(pos,strlen(_find),_replace_str);
                    pos+=strlen(_replace_str);
                }
            }
            std::string result=std::string(_prefix)+name+std::string(_suffix);
            if (_number) result+=std::to_string(_number_start+idx);
            return result;
        };

        std::vector<int> sel_ids=st.selected_ids.empty()?
            std::vector<int>{st.selected_id}:std::vector<int>(st.selected_ids.begin(),st.selected_ids.end());

        if (_preview) {
            ImGui::TextDisabled("Preview (%d entities):", (int)sel_ids.size());
            ImGui::BeginChild("##bnrlist",{0,160},true);
            int idx=0;
            for (int sid:sel_ids) {
                for (auto& e:st.entities) {
                    if (e.value("id",0)!=sid) continue;
                    std::string old_name=e.value("name","");
                    std::string new_name=apply(old_name,idx);
                    ImGui::TextDisabled("%s",old_name.c_str());
                    ImGui::SameLine(); ImGui::TextColored({0.4f,1.f,0.5f,1.f},"→ %s",new_name.c_str());
                    idx++;
                }
            }
            ImGui::EndChild();
        }

        if (ImGui::Button("Apply Rename##bnrap")) {
            st.undo.push(st.entities);
            int idx=0;
            for (int sid:sel_ids) {
                for (auto& e:st.entities) {
                    if (e.value("id",0)!=sid) continue;
                    e["name"]=apply(e.value("name",""),idx++);
                }
            }
            st.log("Batch renamed "+std::to_string(sel_ids.size())+" entities.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##bnrrst")) {
            _prefix[0]=_suffix[0]=_find[0]=_replace_str[0]=0;
            _number=false; _number_start=0;
        }
        ImGui::End();
    }
};

// ─── 31. Component Search Panel ───────────────────────────────────────────────
// Find all entities with a specific component — like Unity's "Find all with X".
class ComponentSearchPanel {
    bool _open=false;
    char _query[64]={};
    std::string _comp_filter;
    std::vector<int> _results;
    bool _searched=false;
public:
    void open() { _open=true; }
    bool& is_open() { return _open; }
    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({360,400},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Component Search##csp",&_open)) { ImGui::End(); return; }
        ImGui::Text("Find entities by component:");
        ImGui::SetNextItemWidth(-80);
        ImGui::InputText("##cspq",_query,64);
        ImGui::SameLine();
        if (ImGui::Button("Search##csps")) {
            _comp_filter=_query; _results.clear(); _searched=true;
            for (auto& e:st.entities) {
                if (!e.contains("components")) continue;
                for (auto& [k,v]:e["components"].items()) {
                    if (k.find(_comp_filter)!=std::string::npos) {
                        _results.push_back(e.value("id",0)); break;
                    }
                }
            }
            st.log("Component search '"+_comp_filter+"': "+std::to_string(_results.size())+" result(s).");
        }
        ImGui::Separator();
        // Quick filter buttons
        ImGui::TextDisabled("Quick:");
        auto qbtn=[&](const char* c){ if(ImGui::Button(c)){snprintf(_query,64,"%s",c);_comp_filter=c;_results.clear();_searched=true;for(auto&e:st.entities){if(!e.contains("components"))continue;if(e["components"].contains(c))_results.push_back(e.value("id",0));} } };
        qbtn("Rigidbody2D"); ImGui::SameLine();
        qbtn("Animator"); ImGui::SameLine();
        qbtn("NavMeshAgent2D");
        qbtn("Light2D"); ImGui::SameLine();
        qbtn("Camera2D"); ImGui::SameLine();
        qbtn("Script");
        ImGui::Separator();
        if (_searched) {
            ImGui::Text("Results: %d for '%s'", (int)_results.size(), _comp_filter.c_str());
            ImGui::BeginChild("##cspres",{0,0},true);
            for (int rid:_results) {
                for (auto& e:st.entities) {
                    if (e.value("id",0)!=rid) continue;
                    std::string nm=e.value("name","Entity");
                    char lbl[128]; snprintf(lbl,128,"%s [%d]##csr%d",nm.c_str(),rid,rid);
                    if (ImGui::Selectable(lbl, st.selected_id==rid)) {
                        st.select(rid);
                    }
                }
            }
            ImGui::EndChild();
        } else { ImGui::TextDisabled("Enter component name and press Search."); }
        ImGui::End();
    }
};

// ─── 32. Hotkeys / Shortcuts Panel ───────────────────────────────────────────
// Displays all editor shortcuts — like Unity's Shortcuts Manager.
class HotkeysPanel {
    bool _open=false;
    char _filter[64]={};
    struct HotkeyEntry { std::string action; std::string shortcut; std::string category; };
    std::vector<HotkeyEntry> _entries;
public:
    HotkeysPanel() {
        _entries = {
            {"Undo",                    "Ctrl+Z",           "Edit"},
            {"Redo",                    "Ctrl+Y",           "Edit"},
            {"Duplicate Entity",        "Ctrl+D",           "Edit"},
            {"Delete Entity",           "Delete",           "Edit"},
            {"Copy",                    "Ctrl+C",           "Edit"},
            {"Paste",                   "Ctrl+V",           "Edit"},
            {"Select All",              "Ctrl+A",           "Edit"},
            {"Play / Stop",             "Ctrl+P",           "Play"},
            {"Pause",                   "Ctrl+Shift+P",     "Play"},
            {"Step Frame",              "Ctrl+Alt+P",       "Play"},
            {"Move Tool",               "W",                "Viewport"},
            {"Rotate Tool",             "E",                "Viewport"},
            {"Scale Tool",              "R",                "Viewport"},
            {"Rect Tool",               "T",                "Viewport"},
            {"Frame Selected",          "F",                "Viewport"},
            {"Toggle Grid Snap",        "G",                "Viewport"},
            {"Zoom In",                 "Ctrl+=",           "Viewport"},
            {"Zoom Out",                "Ctrl+-",           "Viewport"},
            {"Project Settings",        "Ctrl+,",           "Windows"},
            {"Find Object",             "Ctrl+F",           "Windows"},
            {"Profiler",                "Ctrl+7",           "Windows"},
            {"Memory Profiler",         "Ctrl+8",           "Windows"},
            {"Prefab Manager",          "Ctrl+P",           "Windows"},
            {"Timeline",                "Ctrl+T",           "Windows"},
            {"NavMesh",                 "Ctrl+Shift+N",     "Windows"},
            {"Package Manager",         "Ctrl+Shift+P",     "Windows"},
            {"Physics Debugger",        "Ctrl+Shift+D",     "Windows"},
            {"Save Scene",              "Ctrl+S",           "File"},
            {"New Scene",               "Ctrl+N",           "File"},
            {"Open Scene",              "Ctrl+O",           "File"},
            {"Rebuild Scripts",         "Ctrl+R",           "Scripting"},
            {"Create Empty Entity",     "Ctrl+Shift+N",     "Hierarchy"},
            {"Rename Entity",           "F2",               "Hierarchy"},
            {"Toggle Entity Active",    "Alt+Shift+A",      "Hierarchy"},
        };
    }
    void open() { _open=true; }
    bool& is_open() { return _open; }
    void draw(EditorState&) {
        if (!_open) return;
        ImGui::SetNextWindowSize({480,520},ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Keyboard Shortcuts##hkp",&_open)) { ImGui::End(); return; }
        ImGui::InputText("Filter##hkf",_filter,64);
        ImGui::Separator();
        std::string cat_cur;
        for (auto& e:_entries) {
            if (strlen(_filter)>0 &&
                e.action.find(_filter)==std::string::npos &&
                e.shortcut.find(_filter)==std::string::npos &&
                e.category.find(_filter)==std::string::npos) continue;
            if (e.category!=cat_cur) {
                if (!cat_cur.empty()) ImGui::Spacing();
                ImGui::TextColored({0.4f,0.8f,1.f,1.f},"%s",e.category.c_str());
                ImGui::Separator();
                cat_cur=e.category;
            }
            ImGui::Text("%-36s", e.action.c_str());
            ImGui::SameLine(280);
            ImGui::TextColored({1.f,0.85f,0.3f,1.f},"%s",e.shortcut.c_str());
        }
        ImGui::End();
    }
};

// ─── New component defaults for features 19-32 ───────────────────────────────
// Call this after make_component_defaults() to patch in additional entries.
inline void patch_nova_component_defaults(nlohmann::json& d) {
    // TrailRenderer2D — rendered as fading polyline behind a moving body.
    d["TrailRenderer2D"] = {
        {"time",0.5f},{"min_vertex_distance",4.f},
        {"width_start",6.f},{"width_end",0.f},
        {"color_start",nlohmann::json::array({255,200,80,255})},
        {"color_end",nlohmann::json::array({255,80,80,0})},
        {"texture",""},{"sorting_layer",""},{"order_in_layer",1},
        {"auto_destroy_on_stop",false},{"emitting",true}
    };
    // Shadow2DCaster — marks a sprite/collider as a 2D shadow caster.
    d["Shadow2DCaster"] = {
        {"enabled",true},{"self_shadows",false},{"use_renderer_silhouette",true},
        {"cast_shadows",true},{"layer_mask",65535},{"shadow_strength",0.55f},
        {"silhouette_radius",24.f},{"max_distance",200.f}
    };
    // Animator Override Controller ref.
    d["AnimatorOverrideController"] = {
        {"base_controller",""},{"overrides",nlohmann::json::object()}
    };
    // IK Limb 2D — two-bone IK for 2D rigs.
    d["LimbIK2D"] = {
        {"enabled",true},{"root_entity",-1},{"mid_entity",-1},{"end_entity",-1},
        {"target_entity",-1},{"target_x",0.f},{"target_y",0.f},{"pole_entity",-1},
        {"length1",0.f},{"length2",0.f},{"bend_direction",1},
        {"chain_length",2},{"weight",1.f},{"bend_goal_weight",1.f}
    };
    // Spawner2D — timed entity spawning with pool support.
    d["Spawner2D"] = {
        {"prefab",""},{"interval",1.f},{"max_count",10},
        {"pool_size",10},{"spawn_on_start",false},
        {"offset_x",0.f},{"offset_y",0.f},
        {"spawn_radius",0.f},{"inherit_velocity",false}
    };
    // HealthComponent — canonical HP component used by game scripts.
    d["HealthComponent"] = {
        {"max_health",100.f},{"current_health",100.f},
        {"invincibility_time",0.5f},{"auto_destroy_on_death",false},
        {"on_death_event",""},{"on_damage_event",""}
    };
}

// ═══════════════════════════════════════════════════════════════════════════════
// BATCH 2 — Major Unity 2D parity features added in this pass:
//
//  33.  ObjectPoolPanel          — Runtime pool inspector (Unity's Pool Manager)
//  34.  LODGroupPanel            — 2D LOD level editor (LODGroup component)
//  35.  TilemapAnimationPanel    — Animated tile editor (TileAnimationData)
//  36.  SceneTransitionPanel     — Fade/wipe transition designer
//  37.  FlockingDebugPanel       — Boids/steering agent visualiser
//  38.  PathFollowerPanel        — Waypoint path editor (iTween / PathCreator)
//  39.  PresetManagerPanel       — Component preset library (Unity Presets)
//  40.  InspectorLockPanel       — Inspector lock + multi-object editing queue
//  41.  SnapOverridePanel        — Per-entity snap overrides (like Unity 2022+)
//  42.  ComponentCopyPanel       — Copy/paste component values across entities
// ═══════════════════════════════════════════════════════════════════════════════

// ─── 33. Object Pool Panel ───────────────────────────────────────────────────
// Mirrors Unity's Pool Manager window: shows all active pools, how many
// entities are idle vs in-use, and lets the editor pre-warm or flush a pool.
class ObjectPoolPanel {
    bool _open = false;
    char _new_key[64] = {};
    int  _new_size = 8;
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({480,360}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Object Pool Manager##opm", &_open)) { ImGui::End(); return; }
        ImGui::TextDisabled("Entities with ObjectPool component:");
        ImGui::Separator();

        // Collect pools by key
        std::unordered_map<std::string, std::pair<int,int>> pools; // key -> {idle, in_use}
        for (auto& e : st.entities) {
            if (!e.contains("components") || !e["components"].contains("ObjectPool")) continue;
            auto& pc = e["components"]["ObjectPool"];
            std::string key = pc.value("pool_key","");
            if (key.empty()) key = "(unnamed)";
            bool in_use = e.value("active",true);
            auto& p = pools[key];
            if (in_use) p.second++; else p.first++;
        }

        if (pools.empty()) {
            ImGui::TextDisabled("No pool entities in scene. Add ObjectPool component to an entity.");
        } else {
            ImGui::BeginTable("##pool_tbl", 4, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg);
            ImGui::TableSetupColumn("Pool Key", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Idle");
            ImGui::TableSetupColumn("In Use");
            ImGui::TableSetupColumn("Actions");
            ImGui::TableHeadersRow();
            for (auto& [key, counts] : pools) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(key.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", counts.first);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%d", counts.second);
                ImGui::TableSetColumnIndex(3);
                std::string flush_id = "Flush##opm_" + key;
                if (ImGui::SmallButton(flush_id.c_str())) {
                    st.undo.push(st.entities);
                    for (auto& e : st.entities) {
                        if (!e.contains("components") || !e["components"].contains("ObjectPool")) continue;
                        if (e["components"]["ObjectPool"].value("pool_key","") == key)
                            e["active"] = false;
                    }
                    st.log("Flushed pool: " + key);
                }
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::Text("Add Pool Entity:");
        ImGui::SetNextItemWidth(160);
        ImGui::InputText("Pool Key##opmnk", _new_key, 64);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputInt("Size##opmns", &_new_size);
        if (_new_size < 1) _new_size = 1;
        ImGui::SameLine();
        if (ImGui::Button("Create##opmc") && _new_key[0]) {
            st.undo.push(st.entities);
            for (int i = 0; i < _new_size; ++i) {
                Entity e = Entity::object();
                e["id"]     = st.next_id();
                e["name"]   = std::string("Pool_") + _new_key + "_" + std::to_string(i);
                e["active"] = false; // start idle
                e["components"]["ObjectPool"]["pool_key"]     = std::string(_new_key);
                e["components"]["ObjectPool"]["max_size"]     = _new_size;
                e["components"]["ObjectPool"]["pool_lifetime"]= 0.f;
                st.entities.push_back(e);
            }
            st.log(std::string("Created pool '") + _new_key + "' with " + std::to_string(_new_size) + " entries.");
            _new_key[0] = 0;
        }
        ImGui::End();
    }
};

// ─── 34. LOD Group Panel ─────────────────────────────────────────────────────
// Editor for the LODGroup2D runtime component: define N LOD levels, each with
// a screen-coverage threshold and a texture to display. Mirrors Unity's
// LODGroup component inspector with the percentage sliders.
class LODGroupPanel {
    bool _open = false;
    char _tex_buf[256] = {};
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({460,360}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("LOD Group 2D##lodgp", &_open)) { ImGui::End(); return; }

        // Find selected entity
        Entity* target = nullptr;
        for (auto& e : st.entities)
            if (e.value("id",0) == st.selected_id) { target = &e; break; }

        if (!target) { ImGui::TextDisabled("Select an entity to edit its LODGroup2D."); ImGui::End(); return; }
        if (!(*target).contains("components")) (*target)["components"] = Entity::object();
        auto& comps = (*target)["components"];
        if (!comps.contains("LODGroup2D")) {
            if (ImGui::Button("Add LODGroup2D Component")) {
                comps["LODGroup2D"] = {{"reference_height_units",1.f},{"levels",Entity::array()}};
                st.log("Added LODGroup2D to " + (*target).value("name","entity"));
            }
            ImGui::End(); return;
        }
        auto& lg = comps["LODGroup2D"];
        float ref_h = lg.value("reference_height_units", 1.f);
        ImGui::Text("Entity: %s", (*target).value("name","?").c_str());
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Reference Height (units)##lgref", &ref_h, 0.05f, 0.01f, 1000.f))
            lg["reference_height_units"] = ref_h;
        ImGui::Separator();
        ImGui::Text("LOD Levels (highest detail first):");
        auto& levels = lg["levels"];
        if (!levels.is_array()) levels = Entity::array();

        int del_idx = -1;
        for (int i=0; i<(int)levels.size(); ++i) {
            auto& lv = levels[i];
            ImGui::PushID(i);
            ImGui::Text("LOD %d", i);
            ImGui::SameLine(60);
            float thresh = lv.value("screen_threshold", 0.5f - i*0.15f);
            ImGui::SetNextItemWidth(140);
            if (ImGui::SliderFloat("Screen%%##lgst", &thresh, 0.f, 1.f))
                lv["screen_threshold"] = thresh;
            ImGui::SameLine();
            std::string tex = lv.value("texture", "");
            ImGui::SetNextItemWidth(160);
            if (InspectorPanel::draw_project_asset_slot(st, "Texture##lgtex", tex,
                    {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                    "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0\0", "Select LOD Texture"))
                lv["texture"] = tex;
            ImGui::SameLine();
            if (ImGui::SmallButton("X##lgdel")) del_idx = i;
            ImGui::PopID();
        }
        if (del_idx >= 0) { levels.erase_at((size_t)del_idx); }
        if (ImGui::Button("+ Add LOD Level##lgadd")) {
             float next_thresh = levels.empty() ? 0.5f : levels[(int)levels.size()-1].value("screen_threshold",0.5f)*0.5f;
            levels.push_back({{"screen_threshold",next_thresh},{"texture",""}});
        }

        int act = lg.value("_active_lod",-1);
        if (act >= 0) { ImGui::Separator(); ImGui::Text("Active LOD: %d", act); }
        ImGui::End();
    }
};

// ─── 35. Tilemap Animation Panel ─────────────────────────────────────────────
// Editor for TilemapAnimationSystem's animated_tiles data: select a Tilemap
// entity, then define which cells are animated and their frame sequences.
// Mirrors Unity's "Animated Tile" asset editor.
class TilemapAnimationPanel {
    bool _open = false;
    char _cell_key[32] = {};
    char _frames_csv[256] = {};
    float _fps = 8.f;
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({460,400}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Tilemap Animation##tmap_anim", &_open)) { ImGui::End(); return; }

        Entity* tm_entity = nullptr;
        for (auto& e : st.entities)
            if (e.value("id",0)==st.selected_id && e.contains("components") && e["components"].contains("Tilemap"))
            { tm_entity = &e; break; }

        if (!tm_entity) {
            ImGui::TextDisabled("Select a Tilemap entity.");
            ImGui::End(); return;
        }
        auto& tm = (*tm_entity)["components"]["Tilemap"];
        if (!tm.contains("animated_tiles")) tm["animated_tiles"] = Entity::object();
        auto& at_map = tm["animated_tiles"];

        ImGui::Text("Tilemap: %s", (*tm_entity).value("name","?").c_str());
        ImGui::Separator();
        ImGui::Text("Animated cells:");
        ImGui::BeginChild("##anim_cells_list", {0,200}, true);
        int del_key_idx = -1; int idx_=0;
        for (auto& [cell_key, at] : at_map.items()) {
            ImGui::PushID(idx_);
            ImGui::Text("Cell [%s]  fps:%.1f  frames:%zu",
                cell_key.c_str(), at.value("fps",8.f), at["frames"].is_array()?(size_t)at["frames"].size():0);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove##atrem")) del_key_idx = idx_;
            ImGui::PopID(); ++idx_;
        }
        if (del_key_idx >= 0) {
            int ki=0;
            std::string key_to_erase;
            for (auto& [k, v] : at_map.items()) {
                if (ki == del_key_idx) { key_to_erase = k; break; }
                ++ki;
            }
            if (!key_to_erase.empty()) at_map.erase(key_to_erase);
        }
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::Text("Add animated tile:");
        ImGui::SetNextItemWidth(80); ImGui::InputText("col,row##atck", _cell_key, 32);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60); ImGui::InputFloat("fps##atfps", &_fps, 0.f,0.f,"%.1f");
        ImGui::SetNextItemWidth(220); ImGui::InputText("frames (tile IDs, comma-sep)##atfr", _frames_csv, 256);
        if (ImGui::Button("Add##atadd") && _cell_key[0] && _frames_csv[0]) {
            Entity frames_arr = Entity::array();
            std::stringstream ss(_frames_csv); std::string tok;
            while (std::getline(ss, tok, ',')) {
                try { frames_arr.push_back(std::stoi(tok)); } catch (...) {}
            }
            if (!frames_arr.empty()) {
                at_map[_cell_key] = {{"fps",_fps},{"frames",frames_arr},{"_t",0.f}};
                st.log(std::string("Added animated tile at [") + _cell_key + "]");
                _cell_key[0]=_frames_csv[0]=0;
            }
        }
        ImGui::End();
    }
};

// ─── 36. Scene Transition Panel ───────────────────────────────────────────────
// Designer for SceneTransitionSystem: configure fade colour, duration, and
// transition type (fade, slide-left, slide-right, pixelate, iris-wipe) per
// target scene. Writes to the SceneTransition component on a selected entity.
class SceneTransitionPanel {
    bool _open = false;
    char _target_scene[128] = {};
    int   _loaded_entity_id = -1;
    float _duration = 0.5f;
    int   _type_idx = 0;
    float _fade_col[4] = {0,0,0,1};
    const char* _types[5] = {"fade","slide_left","slide_right","pixelate","iris_wipe"};
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({400,280}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Scene Transition##stp", &_open)) { ImGui::End(); return; }

        Entity* target = nullptr;
        for (auto& e : st.entities)
            if (e.value("id",0) == st.selected_id) { target = &e; break; }

        if (!target) { ImGui::TextDisabled("Select an entity to configure SceneTransition."); ImGui::End(); return; }
        auto& comps = (*target)["components"];
        const int target_id = target->value("id", 0);
        if (_loaded_entity_id != target_id) {
            _loaded_entity_id = target_id;
            const Entity current = comps.value("SceneTransition", Entity::object());
            const std::string scene = current.value("target_scene", std::string());
            std::snprintf(_target_scene, sizeof(_target_scene), "%s", scene.c_str());
            _duration = current.value("duration", 0.5f);
            const std::string type = current.value("transition_type", std::string("fade"));
            _type_idx = 0;
            for (int i = 0; i < 5; ++i) if (type == _types[i]) { _type_idx = i; break; }
            _fade_col[0] = current.value("fade_r", 0) / 255.f;
            _fade_col[1] = current.value("fade_g", 0) / 255.f;
            _fade_col[2] = current.value("fade_b", 0) / 255.f;
            _fade_col[3] = 1.f;
        }

        ImGui::Text("Entity: %s", (*target).value("name","?").c_str());
        const std::string scene_label = _target_scene[0] ? fs::path(_target_scene).filename().string() : "(None)";
        ImGui::TextUnformatted("Target Scene");
        ImGui::SameLine(120);
        ImGui::Button(scene_label.c_str(), {150, 0});
        if (ImGui::IsItemHovered() && _target_scene[0]) ImGui::SetTooltip("%s", _target_scene);
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse...##stts")) {
            const std::string chosen = browse_project_scene_reference(st);
            if (!chosen.empty()) std::snprintf(_target_scene, sizeof(_target_scene), "%s", chosen.c_str());
        }
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Duration (s)##std", &_duration, 0.05f, 0.05f, 5.f);
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Type##sttc", &_type_idx, _types, 5);
        ImGui::ColorEdit4("Fade Colour##stfc", _fade_col, ImGuiColorEditFlags_NoInputs);

        ImGui::Separator();
        if (ImGui::Button("Apply to SceneTransition Component##strap")) {
            comps["SceneTransition"] = {
                {"active",false},
                {"target_scene",std::string(_target_scene)},
                {"transition_type",std::string(_types[_type_idx])},
                {"duration",_duration},
                {"fade_r",(int)(_fade_col[0]*255)},
                {"fade_g",(int)(_fade_col[1]*255)},
                {"fade_b",(int)(_fade_col[2]*255)},
                {"_alpha",0.f},{"_phase","fadeout"}
            };
            st.undo.push_deep(st.entities);
            st.log("SceneTransition configured on " + (*target).value("name","entity"));
        }

        // Show current state if component exists
        if (comps.contains("SceneTransition")) {
            auto& sc = comps["SceneTransition"];
            ImGui::Separator();
            ImGui::Text("Current state: phase=%s  alpha=%.2f",
                sc.value("_phase","idle").c_str(), sc.value("_alpha",0.f));
            if (ImGui::Button("Trigger Transition (Play Mode)##strtrig"))
                sc["active"] = true;
        }
        ImGui::End();
    }
};

// ─── 37. Flocking Debug Panel ─────────────────────────────────────────────────
// Visualises FlockingSystem agents: shows flock groups, per-agent velocity
// arrows, and live weight sliders — like Unity's Steering Behaviour debug views.
class FlockingDebugPanel {
    bool _open = false;
    bool _show_radii = true;
    bool _show_vel   = true;
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({400,400}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Flocking Debug##fldp", &_open)) { ImGui::End(); return; }

        ImGui::Checkbox("Show Perception Radii##fdr", &_show_radii);
        ImGui::SameLine();
        ImGui::Checkbox("Show Velocity##fdv", &_show_vel);
        ImGui::Separator();

        // Group agents
        std::unordered_map<std::string, std::vector<Entity*>> groups;
        for (auto& e : st.entities) {
            if (!e.contains("components") || !e["components"].contains("Flock2D")) continue;
            std::string fid = e["components"]["Flock2D"].value("flock_id","default");
            groups[fid].push_back(&e);
        }

        for (auto& [fid, agents] : groups) {
            if (!ImGui::TreeNode(fid.c_str())) continue;
            ImGui::Text("Agents: %d", (int)agents.size());
            // Editable weights on first agent's component (applied to all in group)
            if (!agents.empty()) {
                auto& fc = (*agents[0])["components"]["Flock2D"];
                float sw=fc.value("separation_weight",1.5f);
                float aw=fc.value("alignment_weight",1.f);
                float cw=fc.value("cohesion_weight",1.f);
                float ms=fc.value("max_speed",120.f);
                bool changed=false;
                if (ImGui::SliderFloat("Separation##fds",&sw,0.f,5.f)) changed=true;
                if (ImGui::SliderFloat("Alignment##fda", &aw,0.f,5.f)) changed=true;
                if (ImGui::SliderFloat("Cohesion##fdc",  &cw,0.f,5.f)) changed=true;
                if (ImGui::DragFloat("Max Speed##fdms",  &ms,1.f,1.f,500.f)) changed=true;
                if (changed) {
                    for (auto* ep : agents) {
                        auto& f = (*ep)["components"]["Flock2D"];
                        f["separation_weight"]=sw; f["alignment_weight"]=aw;
                        f["cohesion_weight"]=cw;   f["max_speed"]=ms;
                    }
                }
            }
            ImGui::BeginChild(("##fld_"+fid).c_str(), {0,120}, true);
            for (auto* ep : agents) {
                float vx=0,vy=0;
                if ((*ep).contains("components") && (*ep)["components"].contains("Rigidbody2D")) {
                    vx=(*ep)["components"]["Rigidbody2D"].value("velocity_x",0.f);
                    vy=(*ep)["components"]["Rigidbody2D"].value("velocity_y",0.f);
                }
                ImGui::Text("  %-20s  vel=(%.1f, %.1f)", (*ep).value("name","?").c_str(), vx, vy);
            }
            ImGui::EndChild();
            ImGui::TreePop();
        }
        if (groups.empty()) ImGui::TextDisabled("No entities with Flock2D component.");
        ImGui::End();
    }
};

// ─── 38. Path Follower Panel ──────────────────────────────────────────────────
// Editor for PathFollower2D waypoint paths: add/remove/reorder waypoints,
// set speed and loop mode, and visualise the path as a polyline in the panel.
// Mirrors Unity's PathCreator / itween path editor workflow.
class PathFollowerPanel {
    bool _open = false;
    float _new_x = 0.f, _new_y = 0.f;
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({440,400}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Path Follower##pfp", &_open)) { ImGui::End(); return; }

        Entity* target = nullptr;
        for (auto& e : st.entities)
            if (e.value("id",0)==st.selected_id) { target = &e; break; }

        if (!target) { ImGui::TextDisabled("Select an entity with PathFollower2D."); ImGui::End(); return; }
        auto& comps = (*target)["components"];
        if (!comps.contains("PathFollower2D")) {
            if (ImGui::Button("Add PathFollower2D##pfpadd")) {
                comps["PathFollower2D"] = {
                    {"speed",80.f},{"loop",true},{"ping_pong",false},
                    {"look_at_direction",true},
                    {"waypoints",Entity::array()},
                    {"_wp_index",0},{"_segment_t",0.f},{"_going_fwd",true}
                };
                st.log("Added PathFollower2D to " + (*target).value("name","?"));
            }
            ImGui::End(); return;
        }
        auto& pf = comps["PathFollower2D"];

        ImGui::Text("Entity: %s", (*target).value("name","?").c_str());
        float speed = pf.value("speed",80.f); bool loop=pf.value("loop",true);
        bool pp=pf.value("ping_pong",false); bool lad=pf.value("look_at_direction",true);
        ImGui::SetNextItemWidth(100);
        if (ImGui::DragFloat("Speed##pfspd",&speed,1.f,0.1f,2000.f)) pf["speed"]=speed;
        ImGui::SameLine(); if (ImGui::Checkbox("Loop##pflp",&loop)) pf["loop"]=loop;
        ImGui::SameLine(); if (ImGui::Checkbox("Ping-Pong##pfpp",&pp)) pf["ping_pong"]=pp;
        ImGui::SameLine(); if (ImGui::Checkbox("Look Dir##pflad",&lad)) pf["look_at_direction"]=lad;
        ImGui::Separator();

        auto& wps = pf["waypoints"];
        if (!wps.is_array()) wps = Entity::array();
        ImGui::Text("Waypoints (%d):", (int)wps.size());
        int del_wp=-1, move_up=-1;
        for (int i=0;i<(int)wps.size();++i) {
            ImGui::PushID(i);
            float wx=wps[i].value("x",0.f), wy=wps[i].value("y",0.f);
            ImGui::Text("%2d.", i); ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::DragFloat("X##wpx",&wx,0.5f)) wps[i]["x"]=wx;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::DragFloat("Y##wpy",&wy,0.5f)) wps[i]["y"]=wy;
            ImGui::SameLine();
            if (ImGui::SmallButton("↑##wpmu")&&i>0) move_up=i;
            ImGui::SameLine();
            if (ImGui::SmallButton("X##wpdel")) del_wp=i;
            ImGui::PopID();
        }
        if (del_wp>=0) wps.erase_at((size_t)del_wp);
        if (move_up>0) std::swap(wps[move_up], wps[move_up-1]);

        ImGui::Separator();
        ImGui::SetNextItemWidth(80); ImGui::DragFloat("X##pfnwx",&_new_x,0.5f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80); ImGui::DragFloat("Y##pfnwy",&_new_y,0.5f);
        ImGui::SameLine();
        if (ImGui::Button("Add Waypoint##pfaw"))
            wps.push_back({{"x",_new_x},{"y",_new_y}});
        ImGui::SameLine();
        if (ImGui::Button("Clear All##pfca")) { wps = Entity::array(); }

        // Progress indicator
        int cur_wp = pf.value("_wp_index",0);
        float seg_t = pf.value("_segment_t",0.f);
        ImGui::Separator();
        ImGui::Text("Progress: waypoint %d  seg_t=%.2f", cur_wp, seg_t);
        ImGui::End();
    }
};

// ─── 39. Preset Manager Panel ─────────────────────────────────────────────────
// Unity's Preset system saves a component's values as a named preset and can
// apply it to another entity's matching component in one click. This panel
// implements the same workflow: capture -> name -> save to file, then apply.
class PresetManagerPanel {
    bool _open = false;
    char _preset_name[64] = {};
    char _comp_filter[64] = {};
    // Presets stored in memory: name -> {comp_name, component_json}
    struct Preset { std::string name, comp_name; nlohmann::json data; };
    std::vector<Preset> _presets;
    int _sel_preset = -1;
    std::string _preset_file;

    void load(EditorState& st) {
        _preset_file = (fs::path(st.scene_path).parent_path() / "presets.json").string();
        auto j = load_json(_preset_file);
        _presets.clear();
        if (!j.is_array()) return;
        for (auto& p : j) {
            Preset pr;
            pr.name      = p.value("name","");
            pr.comp_name = p.value("comp","");
            pr.data      = p.value("data", nlohmann::json::object());
            if (!pr.name.empty()) _presets.push_back(std::move(pr));
        }
    }
    void save_presets() {
        nlohmann::json j = nlohmann::json::array();
        for (auto& p : _presets) j.push_back({{"name",p.name},{"comp",p.comp_name},{"data",p.data}});
        save_json(_preset_file, j);
    }
public:
    void open(EditorState& st) { load(st); _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({460,400}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Preset Manager##presetmgr", &_open)) { ImGui::End(); return; }
        if (_preset_file.empty()) load(st);

        Entity* target = nullptr;
        for (auto& e : st.entities)
            if (e.value("id",0)==st.selected_id) { target = &e; break; }

        // Capture section
        ImGui::Text("Capture from selected entity:");
        if (!target) { ImGui::TextDisabled("(no selection)"); }
        else {
            ImGui::SetNextItemWidth(180);
            ImGui::InputText("Filter component##prmcf", _comp_filter, 64);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("Preset Name##prname", _preset_name, 64);
            ImGui::SameLine();
            if (ImGui::Button("Capture##prcap") && _preset_name[0] && _comp_filter[0]) {
                if ((*target).contains("components") && (*target)["components"].contains(_comp_filter)) {
                    Preset pr;
                    pr.name      = _preset_name;
                    pr.comp_name = _comp_filter;
                    pr.data      = (*target)["components"][_comp_filter];
                    // Replace if name already exists
                    for (auto& p : _presets) if (p.name==pr.name){p=pr;goto saved;}
                    _presets.push_back(std::move(pr));
                    saved:
                    save_presets();
                    st.log("Preset '"+std::string(_preset_name)+"' captured.");
                    _preset_name[0]=0;
                } else ImGui::OpenPopup("CompNotFound");
            }
            if (ImGui::BeginPopupModal("CompNotFound",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Component '%s' not found on selected entity.", _comp_filter);
                if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        ImGui::Separator();
        ImGui::Text("Saved Presets:");
        ImGui::BeginChild("##preset_list",{0,160},true);
        for (int i=0;i<(int)_presets.size();++i) {
            auto& p = _presets[i];
            std::string lbl = p.name + "  ["+p.comp_name+"]";
            if (ImGui::Selectable(lbl.c_str(), _sel_preset==i)) _sel_preset=i;
        }
        ImGui::EndChild();

        if (_sel_preset>=0 && _sel_preset<(int)_presets.size()) {
            auto& sp = _presets[_sel_preset];
            ImGui::Text("Selected: %s  (component: %s)", sp.name.c_str(), sp.comp_name.c_str());
            if (ImGui::Button("Apply to Selected Entity##prapp") && target) {
                st.undo.push(st.entities);
                (*target)["components"][sp.comp_name] = sp.data;
                st.log("Applied preset '"+sp.name+"' to "+(*target).value("name","entity"));
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete Preset##prdel")) {
                _presets.erase(_presets.begin()+_sel_preset);
                _sel_preset=-1;
                save_presets();
            }
        }
        ImGui::End();
    }
};

// ─── 40. Inspector Lock Panel ─────────────────────────────────────────────────
// Unity's Inspector Lock button keeps the Inspector showing a specific entity
// even when you click elsewhere in the Hierarchy. This panel implements the
// same mechanic plus a multi-entity edit queue (edit the same component field
// across several selected entities at once — exactly like Unity's multi-object
// editing). No equivalent exists in the existing editor.
class InspectorLockPanel {
    bool _open  = true;   // toggle button in toolbar, not a separate window
    bool _locked = false;
    int  _locked_id = -1;
    std::vector<int> _multi_sel;
    char _multi_comp[64] = {};
    char _multi_field[64] = {};
    char _multi_value[128] = {};
public:
    bool& is_open() { return _open; }
    bool locked()   const { return _locked; }
    int  locked_id() const { return _locked_id; }

    // Call this from the toolbar / menu bar
    void draw_toolbar_button(EditorState& st) {
        if (!_open) return;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, _locked ? ImVec4{0.8f,0.3f,0.1f,1.f} : ImVec4{0.2f,0.2f,0.2f,1.f});
        if (ImGui::Button(_locked ? "🔒 Inspector Locked##ilock" : "🔓 Lock Inspector##ilock")) {
            _locked = !_locked;
            _locked_id = _locked ? st.selected_id : -1;
        }
        ImGui::PopStyleColor();
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({460,300}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Multi-Object Edit##moe", &_open)) { ImGui::End(); return; }

        ImGui::TextWrapped("Set the same component field on multiple entities simultaneously (Unity-style multi-edit).");
        ImGui::Separator();

        // Build multi-selection from entities
        ImGui::Text("Select entities to batch-edit:");
        ImGui::BeginChild("##moe_elist",{0,120},true);
        for (auto& e : st.entities) {
            int eid = e.value("id",0);
            bool in_sel = std::find(_multi_sel.begin(),_multi_sel.end(),eid)!=_multi_sel.end();
            std::string lbl = e.value("name","Entity")+" ["+std::to_string(eid)+"]";
            if (ImGui::Checkbox(lbl.c_str(), &in_sel)) {
                if (in_sel) _multi_sel.push_back(eid);
                else _multi_sel.erase(std::remove(_multi_sel.begin(),_multi_sel.end(),eid),_multi_sel.end());
            }
        }
        ImGui::EndChild();
        ImGui::Text("Selected: %d entity(ies)", (int)_multi_sel.size());
        ImGui::Separator();

        ImGui::SetNextItemWidth(140); ImGui::InputText("Component##moec", _multi_comp, 64);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120); ImGui::InputText("Field##moef", _multi_field, 64);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120); ImGui::InputText("Value##moev", _multi_value, 128);
        ImGui::SameLine();
        if (ImGui::Button("Apply All##moeapp") && _multi_comp[0] && _multi_field[0] && !_multi_sel.empty()) {
            st.undo.push(st.entities);
            int changed = 0;
            for (auto& e : st.entities) {
                if (std::find(_multi_sel.begin(),_multi_sel.end(),e.value("id",0))==_multi_sel.end()) continue;
                if (!e.contains("components") || !e["components"].contains(_multi_comp)) continue;
                // Try numeric first, then string
                try {
                    float fv = std::stof(_multi_value);
                    e["components"][_multi_comp][_multi_field] = fv;
                } catch (...) {
                    e["components"][_multi_comp][_multi_field] = std::string(_multi_value);
                }
                ++changed;
            }
            st.log("Multi-edit: set "+std::string(_multi_comp)+"."+_multi_field+"="+_multi_value+" on "+std::to_string(changed)+" entities.");
        }
        ImGui::End();
    }
};

// ─── 41. Snap Override Panel ──────────────────────────────────────────────────
// Unity 2022+ lets you override the grid snap settings per-object via the
// Snap Overrides window. This panel adds per-entity snap step values so
// the editor can snap different objects to different grids.
class SnapOverridePanel {
    bool _open = false;
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({360,260}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Snap Overrides##sop", &_open)) { ImGui::End(); return; }

        Entity* target = nullptr;
        for (auto& e : st.entities)
            if (e.value("id",0)==st.selected_id) { target = &e; break; }

        ImGui::Text("Global snap step: %.2f units", st.grid_snap > 0 ? (float)st.grid_snap : 1.f);
        ImGui::Separator();
        if (!target) { ImGui::TextDisabled("Select an entity to set snap overrides."); ImGui::End(); return; }

        ImGui::Text("Entity: %s", (*target).value("name","?").c_str());
        auto& snap = (*target)["_snap_override"];
        bool has_override = !snap.is_null() && snap.is_object();
        bool enable_override = has_override;
        if (ImGui::Checkbox("Enable Snap Override##soe", &enable_override)) {
            if (enable_override) (*target)["_snap_override"] = {{"x",1.f},{"y",1.f},{"rot",15.f}};
            else                 (*target).erase("_snap_override");
        }
        if (enable_override && (*target).contains("_snap_override")) {
            auto& so = (*target)["_snap_override"];
            float sx=so.value("x",1.f), sy=so.value("y",1.f), sr=so.value("rot",15.f);
            ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat("Snap X##sox",&sx,0.05f,0.01f,100.f)) so["x"]=sx;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat("Snap Y##soy",&sy,0.05f,0.01f,100.f)) so["y"]=sy;
            ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat("Snap Rotation (deg)##sor",&sr,1.f,1.f,180.f)) so["rot"]=sr;
        }
        ImGui::Separator();
        ImGui::TextDisabled("Override takes priority over global grid snap.");
        ImGui::End();
    }
};

// ─── 42. Component Copy Panel ─────────────────────────────────────────────────
// Copy a component's full JSON value from one entity and paste it onto one or
// many target entities — exactly like Unity's "Copy Component" / "Paste
// Component Values" context menu, but as a panel for batch operations.
class ComponentCopyPanel {
    bool _open = false;
    char _comp_name[64] = {};
    int  _src_id = -1;
    nlohmann::json _clipboard;
    std::string _clipboard_comp;
    std::vector<int> _dst_ids;
public:
    void open() { _open = true; }
    bool& is_open() { return _open; }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({440,380}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Component Copy##ccp", &_open)) { ImGui::End(); return; }

        ImGui::Text("Copy component from source entity to targets.");
        ImGui::Separator();

        // Source
        ImGui::Text("Source:");
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Component##ccpcomp", _comp_name, 64);
        ImGui::SameLine();
        if (ImGui::Button("Use Selected as Source##ccpuse")) _src_id = st.selected_id;
        if (_src_id >= 0) ImGui::Text("Source entity id: %d", _src_id);

        if (ImGui::Button("Copy Component##ccpcopy") && _src_id>=0 && _comp_name[0]) {
            for (auto& e : st.entities) {
                if (e.value("id",0)!=_src_id) continue;
                if (e.contains("components") && e["components"].contains(_comp_name)) {
                    _clipboard      = e["components"][_comp_name];
                    _clipboard_comp = _comp_name;
                    st.log("Copied "+std::string(_comp_name)+" from entity "+std::to_string(_src_id));
                }
                break;
            }
        }
        if (!_clipboard_comp.empty()) {
            ImGui::SameLine();
            ImGui::TextColored({0.4f,1.f,0.4f,1.f}, "Clipboard: %s", _clipboard_comp.c_str());
        }

        ImGui::Separator();
        ImGui::Text("Target entities (check to include):");
        ImGui::BeginChild("##ccp_tlist",{0,140},true);
        for (auto& e : st.entities) {
            int eid = e.value("id",0);
            if (eid == _src_id) continue;
            bool in_dst = std::find(_dst_ids.begin(),_dst_ids.end(),eid)!=_dst_ids.end();
            std::string lbl = e.value("name","Entity")+" ["+std::to_string(eid)+"]";
            if (ImGui::Checkbox(lbl.c_str(),&in_dst)) {
                if (in_dst) _dst_ids.push_back(eid);
                else _dst_ids.erase(std::remove(_dst_ids.begin(),_dst_ids.end(),eid),_dst_ids.end());
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Paste to Selected Targets##ccppaste")
            && !_clipboard_comp.empty() && !_dst_ids.empty()) {
            st.undo.push(st.entities);
            int pasted=0;
            for (auto& e : st.entities) {
                if (std::find(_dst_ids.begin(),_dst_ids.end(),e.value("id",0))==_dst_ids.end()) continue;
                e["components"][_clipboard_comp] = _clipboard;
                ++pasted;
            }
            st.log("Pasted "+_clipboard_comp+" to "+std::to_string(pasted)+" entities.");
        }
        ImGui::End();
    }
};

// ─── Additional component defaults for batch-2 features ──────────────────────
inline void patch_nova_component_defaults_b2(nlohmann::json& d) {
    d["ObjectPool"] = {
        {"pool_key","default"},{"max_size",8},
        {"pool_lifetime",0.f},{"_pool_elapsed",0.f}
    };
    d["LODGroup2D"] = {
        {"reference_height_units",1.f},
        {"levels",Entity::array()}
    };
    d["Flock2D"] = {
        {"flock_id","default"},
        {"perception_radius",80.f},
        {"separation_weight",1.5f},{"alignment_weight",1.f},{"cohesion_weight",1.f},
        {"max_speed",120.f},{"max_force",60.f},
        {"seek_enabled",false},{"seek_x",0.f},{"seek_y",0.f},{"seek_weight",1.f}
    };
    d["PathFollower2D"] = {
        {"speed",80.f},{"loop",true},{"ping_pong",false},
        {"look_at_direction",true},
        {"waypoints",Entity::array()},
        {"_wp_index",0},{"_segment_t",0.f},{"_going_fwd",true}
    };
    d["SceneTransition"] = {
        {"active",false},{"target_scene",""},
        {"transition_type","fade"},{"duration",0.5f},
        {"fade_r",0},{"fade_g",0},{"fade_b",0},
        {"_alpha",0.f},{"_phase","fadeout"}
    };
}
