#pragma once

// Runtime representation of a Unity-style Tile Palette.  The editor owns the
// authoring UI and generated atlas; the game only needs this small immutable
// lookup table so a Tilemap cell can stay an integer while resolving to a
// stable palette tile and atlas rectangle.

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <system_error>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace tilepalette {

struct Tile {
    int id = -1;
    int atlas_x = 0;
    int atlas_y = 0;
    int atlas_w = 0;
    int atlas_h = 0;
    std::string collision = "solid";
};

struct Palette {
    std::string guid;
    std::string atlas;
    int cell_width = 32;
    int cell_height = 32;
    std::unordered_map<int, Tile> tiles;
};

inline bool load(const std::filesystem::path& manifest_path, Palette& out,
                 std::string* diagnostic = nullptr) {
    out = Palette{};
    if (diagnostic) diagnostic->clear();
    std::ifstream input(manifest_path);
    nlohmann::json json;
    try {
        if (!input || !(input >> json) || !json.is_object()) {
            if (diagnostic) *diagnostic = "Tile Palette file is missing or invalid.";
            return false;
        }
        if (json.value("format", std::string()) != "gameengine.tile-palette") {
            if (diagnostic) *diagnostic = "The selected asset is not a GameEngine Tile Palette.";
            return false;
        }
        out.guid = json.value("guid", std::string());
        out.atlas = json.value("atlas", std::string());
        out.cell_width = std::max(1, json.value("cell_width", 32));
        out.cell_height = std::max(1, json.value("cell_height", out.cell_width));
        const auto entries = json.value("tiles", nlohmann::json::array());
        if (!entries.is_array()) throw std::runtime_error("tiles is not an array");
        for (const auto& entry : entries) {
            if (!entry.is_object()) continue;
            Tile tile;
            tile.id = entry.value("id", -1);
            tile.atlas_x = entry.value("atlas_x", 0);
            tile.atlas_y = entry.value("atlas_y", 0);
            tile.atlas_w = entry.value("atlas_w", out.cell_width);
            tile.atlas_h = entry.value("atlas_h", out.cell_height);
            tile.collision = entry.value("collision", std::string("solid"));
            if (tile.id >= 0 && tile.atlas_w > 0 && tile.atlas_h > 0)
                out.tiles[tile.id] = std::move(tile);
        }
        if (out.atlas.empty() || out.tiles.empty()) {
            if (diagnostic) *diagnostic = "Tile Palette has no generated atlas or tiles.";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        if (diagnostic) *diagnostic = std::string("Could not read Tile Palette: ") + ex.what();
        return false;
    }
}

class Cache {
public:
    const Palette* get(const std::string& asset_dir, const std::string& reference) {
        if (reference.empty()) return nullptr;
        namespace fs = std::filesystem;
        fs::path path(reference);
        if (path.is_relative()) path = fs::path(asset_dir) / path;
        std::error_code ec;
        path = fs::weakly_canonical(path, ec);
        if (ec || path.empty()) return nullptr;
        const std::string key = path.generic_string();
        auto& entry = _entries[key];
        const auto stamp = fs::last_write_time(path, ec);
        if (!entry.loaded || (!ec && stamp != entry.stamp)) {
            entry.loaded = load(path, entry.palette, &entry.diagnostic);
            entry.stamp = ec ? fs::file_time_type{} : stamp;
        }
        return entry.loaded ? &entry.palette : nullptr;
    }

    void clear() { _entries.clear(); }

private:
    struct Entry {
        Palette palette;
        std::filesystem::file_time_type stamp{};
        std::string diagnostic;
        bool loaded = false;
    };
    std::unordered_map<std::string, Entry> _entries;
};

} // namespace tilepalette
