#pragma once

// Include this after unity_gap_features.hpp.  That header provides the shared
// project picker, atomic JSON writer, image decoder and TilePaletteAsset data
// model used here.

class TilePalettePanel {
public:
    // Tile images are real project textures, not generated placeholders. This
    // cache is shared by the palette window and the compact Inspector picker.
    void init(vkr::RendererBackend& backend) { _thumbs.init(backend); }
    void shutdown() { _thumbs.clear(); }

    void open(EditorState& st, const std::string& palette_ref = {}) {
        _open = true;
        if (!palette_ref.empty()) _load(st, palette_ref);
        else if (!_loaded_ref.empty()) _load(st, _loaded_ref);
        _scan(st);
    }

    bool is_open() const { return _open; }

    // Compact Tilemap Inspector version of the palette. It is intentionally a
    // real image grid rather than a numeric Tile ID field: click one tile to
    // paint it, Ctrl-click to make a multi-tile brush, or Shift-click to select
    // a contiguous range. The full window remains available for asset and
    // brush management.
    void draw_inline_picker(EditorState& st, Entity& tilemap_entity) {
        if (!tilemap_entity.contains("components") || !tilemap_entity["components"].contains("Tilemap")) return;
        auto& tilemap = tilemap_entity["components"]["Tilemap"];
        const std::string palette_ref = tilemap.value("tile_palette", std::string());
        if (palette_ref.empty()) return;

        TilePaletteAsset palette;
        if (!TilePaletteAsset::from_json(load_json(fs::path(st.asset_dir) / palette_ref), palette)) {
            ImGui::TextDisabled("Tile Palette could not be read.");
            return;
        }
        const int owner_id = tilemap_entity.value("id", -1);
        if (_inline_owner_id != owner_id || _inline_palette_ref != palette_ref) {
            _inline_owner_id = owner_id;
            _inline_palette_ref = palette_ref;
            _inline_selected_tiles.clear();
            _inline_selection_anchor = -1;
        }

        ImGui::SeparatorText("Palette Tiles");
        ImGui::TextDisabled("Click to paint. Ctrl-click adds/removes; Shift-click selects a range.");
        ImGui::BeginChild("##inline_tile_palette", {0, 158}, true, ImGuiWindowFlags_HorizontalScrollbar);
        const float card_width = 76.f;
        const int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / card_width));
        int index = 0;
        for (const auto& tile : palette.tiles) {
            const bool selected = std::find(_inline_selected_tiles.begin(), _inline_selected_tiles.end(), tile.id) != _inline_selected_tiles.end();
            ImGui::PushID(tile.id);
            if (_draw_tile_card(st, tile, {68.f, 68.f}, selected, "##inline_tile"))
                _select_inline_tile(st, palette, palette_ref, index);
            ImGui::PopID();
            if (++index % columns != 0) ImGui::SameLine();
        }
        if (palette.tiles.empty()) ImGui::TextDisabled("This palette has no tiles. Open Tile Palette to import images.");
        ImGui::EndChild();
        ImGui::TextDisabled("%d selected", (int)_inline_selected_tiles.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Tile Selection##inline")) {
            _inline_selected_tiles.clear(); _inline_selection_anchor = -1;
            st.paint_brush_cells.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Tile Palette##inline")) open(st, palette_ref);
    }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({980, 660}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Tile Palette##tilepalette", &_open)) { ImGui::End(); return; }

        if (ImGui::Button("New Palette")) ImGui::OpenPopup("##new_tile_palette");
        ImGui::SameLine();
        if (ImGui::Button("Rescan")) _scan(st);
        ImGui::SameLine();
        ImGui::BeginDisabled(_loaded_ref.empty() || _palette.tiles.empty());
        if (ImGui::Button("Build Atlas")) _build(st);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Import images once, then save and stamp multi-tile brushes.");
        _draw_create_popup(st);
        ImGui::Separator();

        ImGui::BeginChild("##palette_list", {220, 0}, true);
        ImGui::TextUnformatted("Tile Palettes");
        ImGui::Separator();
        for (const auto& reference : _palette_refs) {
            const bool selected = reference == _loaded_ref;
            const std::string label = fs::path(reference).parent_path().filename().string();
            if (ImGui::Selectable((label.empty() ? reference : label).c_str(), selected)) _load(st, reference);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", reference.c_str());
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("##palette_workspace", {0, 0}, false);
        if (_loaded_ref.empty()) {
            ImGui::TextDisabled("Create a Tile Palette or select one from the list.");
            ImGui::TextWrapped("A palette stores source images, generated atlas positions, collision settings, and reusable multi-tile brushes.");
            ImGui::EndChild(); ImGui::End(); return;
        }

        ImGui::Text("%s", _palette.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d cells | %d tiles", _palette.cell_width, _palette.cell_height, (int)_palette.tiles.size());
        ImGui::SameLine();
        if (ImGui::Button("Use Palette on Selected Tilemap")) _assign_to_selected_tilemap(st);
        ImGui::Separator();

        ImGui::BeginChild("##tiles", {0, 305}, true);
        if (ImGui::Button("Add Image...")) {
            const std::string source = browse_project_asset_reference(
                st, "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.webp\0\0", "Add Tile Image");
            if (!source.empty()) _add_image(st, source);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(_selected_tiles.empty());
        if (ImGui::Button("Save Selected as Brush")) ImGui::OpenPopup("##save_tile_brush");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear Selection")) _selected_tiles.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("%d selected  |  Ctrl: add/remove  Shift: range", (int)_selected_tiles.size());
        ImGui::Separator();

        const float cell = 126.f;
        const int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cell));
        int index = 0;
        for (const auto& tile : _palette.tiles) {
            const bool selected = std::find(_selected_tiles.begin(), _selected_tiles.end(), tile.id) != _selected_tiles.end();
            ImGui::PushID(tile.id);
            if (_draw_tile_card(st, tile, {118.f, 96.f}, selected, "##palette_tile"))
                _select_palette_tile(st, index);
            ImGui::PopID();
            if (++index % columns != 0) ImGui::SameLine();
        }
        ImGui::EndChild();

        _draw_brush_popup(st);
        ImGui::Separator();
        ImGui::Columns(2, "##tilepalette_bottom", false);
        ImGui::TextUnformatted("Saved Brushes");
        ImGui::BeginChild("##brushes", {0, 0}, true);
        _scan_brushes(st);
        for (const auto& brush_ref : _brush_refs) {
            TileBrushAsset brush;
            const auto json = load_json(fs::path(st.asset_dir) / brush_ref);
            if (!TileBrushAsset::from_json(json, brush) || brush.palette != _loaded_ref) continue;
            ImGui::PushID(brush_ref.c_str());
            if (ImGui::Selectable(brush.name.c_str(), brush_ref == st.active_tile_brush)) _activate_brush(st, brush, brush_ref);
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) { std::error_code ec; fs::remove(fs::path(st.asset_dir) / brush_ref, ec); _scan_brushes(st); }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::NextColumn();
        ImGui::TextUnformatted("Selected Tile");
        if (TilePaletteTile* tile = _find_tile(_selected_tile)) {
            ImGui::Text("%s", fs::path(tile->source).filename().string().c_str());
            ImGui::TextDisabled("Stable ID %d  |  Source crop %d,%d  %d x %d", tile->id,
                                tile->source_x, tile->source_y, tile->source_w, tile->source_h);
            int collision_index = tile->collision == "none" ? 1 : (tile->collision == "trigger" ? 2 : 0);
            if (ImGui::Combo("Collision", &collision_index, "Solid\0None\0Trigger\0")) {
                tile->collision = collision_index == 1 ? "none" : (collision_index == 2 ? "trigger" : "solid");
                _save_manifest(st);
            }
            ImGui::TextDisabled("Atlas: %d, %d (%d x %d)", tile->atlas_x, tile->atlas_y, tile->atlas_w, tile->atlas_h);
            ImGui::TextWrapped("Tile IDs never change when the atlas is rebuilt. Add a new tile instead of replacing an existing source when you need old painted cells to keep their art.");
        } else ImGui::TextDisabled("Select a tile to edit collision behavior.");
        ImGui::Columns(1);
        ImGui::EndChild();
        ImGui::End();
    }

private:
    bool _open = false;
    std::string _loaded_ref;
    TilePaletteAsset _palette;
    thumbnail_cache::Cache _thumbs;
    int _selected_tile = -1;
    std::vector<int> _selected_tiles;
    int _selection_anchor_index = -1;
    int _inline_owner_id = -1;
    std::string _inline_palette_ref;
    std::vector<int> _inline_selected_tiles;
    int _inline_selection_anchor = -1;
    std::vector<std::string> _palette_refs;
    std::vector<std::string> _brush_refs;
    char _new_palette_name[64] = "New Tile Palette";
    int _new_cell_size = 32;
    char _new_brush_name[64] = "Brush";
    int _brush_columns = 0;

    bool _draw_tile_card(EditorState& st, const TilePaletteTile& tile, ImVec2 size,
                         bool selected, const char* id) {
        ImGui::InvisibleButton(id, size);
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 border = selected ? IM_COL32(83, 179, 255, 255)
                           : (hovered ? IM_COL32(180, 190, 205, 230) : IM_COL32(83, 88, 100, 210));
        draw->AddRectFilled(min, max, selected ? IM_COL32(38, 82, 126, 255) : IM_COL32(31, 33, 40, 255), 4.f);
        draw->AddRect(min, max, border, 4.f, 0, selected ? 2.2f : 1.f);

        const fs::path source_path = fs::path(st.asset_dir) / tile.source;
        const thumbnail_cache::Entry* image = _thumbs.get(source_path.string());
        const float pad = 5.f, label_h = 17.f;
        const ImVec2 image_min{min.x + pad, min.y + pad};
        const ImVec2 image_max{max.x - pad, max.y - label_h - 2.f};
        if (image && image->imgui_ds && image->w > 0 && image->h > 0) {
            const float x = std::clamp((float)tile.source_x, 0.f, (float)image->w - 1.f);
            const float y = std::clamp((float)tile.source_y, 0.f, (float)image->h - 1.f);
            const float w = std::clamp((float)(tile.source_w > 0 ? tile.source_w : image->w), 1.f, (float)image->w - x);
            const float h = std::clamp((float)(tile.source_h > 0 ? tile.source_h : image->h), 1.f, (float)image->h - y);
            const float scale = std::min((image_max.x - image_min.x) / w, (image_max.y - image_min.y) / h);
            const float dw = w * scale, dh = h * scale;
            const ImVec2 draw_min{image_min.x + ((image_max.x - image_min.x) - dw) * .5f,
                                  image_min.y + ((image_max.y - image_min.y) - dh) * .5f};
            const ImVec2 draw_max{draw_min.x + dw, draw_min.y + dh};
            draw->AddImage((ImTextureID)(intptr_t)image->imgui_ds, draw_min, draw_max,
                           {x / image->w, y / image->h}, {(x + w) / image->w, (y + h) / image->h});
        } else {
            draw->AddText({min.x + 7.f, min.y + 7.f}, IM_COL32(175, 180, 190, 255), "Image unavailable");
        }
        const std::string label = "#" + std::to_string(tile.id);
        const ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
        draw->AddText({min.x + (size.x - text_size.x) * .5f, max.y - label_h},
                      IM_COL32(230, 232, 238, 255), label.c_str());
        if (hovered) ImGui::SetTooltip("%s\nStable tile ID %d", tile.source.c_str(), tile.id);
        return clicked;
    }

    static void _toggle_id(std::vector<int>& ids, int id) {
        const auto it = std::find(ids.begin(), ids.end(), id);
        if (it == ids.end()) ids.push_back(id); else ids.erase(it);
    }

    void _select_palette_tile(EditorState& st, int index) {
        if (index < 0 || index >= (int)_palette.tiles.size()) return;
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        const bool shift = ImGui::GetIO().KeyShift;
        if (shift && _selection_anchor_index >= 0) {
            _selected_tiles.clear();
            const int first = std::min(_selection_anchor_index, index), last = std::max(_selection_anchor_index, index);
            for (int i = first; i <= last; ++i) _selected_tiles.push_back(_palette.tiles[i].id);
        } else if (ctrl) {
            _toggle_id(_selected_tiles, _palette.tiles[index].id);
            _selection_anchor_index = index;
        } else {
            _selected_tiles = {_palette.tiles[index].id};
            _selection_anchor_index = index;
        }
        _selected_tile = _palette.tiles[index].id;
        _activate_tiles(st, _selected_tiles);
    }

    void _select_inline_tile(EditorState& st, const TilePaletteAsset& palette,
                             const std::string& palette_ref, int index) {
        if (index < 0 || index >= (int)palette.tiles.size()) return;
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        const bool shift = ImGui::GetIO().KeyShift;
        if (shift && _inline_selection_anchor >= 0) {
            _inline_selected_tiles.clear();
            const int first = std::min(_inline_selection_anchor, index), last = std::max(_inline_selection_anchor, index);
            for (int i = first; i <= last; ++i) _inline_selected_tiles.push_back(palette.tiles[i].id);
        } else if (ctrl) {
            _toggle_id(_inline_selected_tiles, palette.tiles[index].id);
            _inline_selection_anchor = index;
        } else {
            _inline_selected_tiles = {palette.tiles[index].id};
            _inline_selection_anchor = index;
        }
        st.active_tile_palette = palette_ref;
        st.active_tile_brush.clear();
        st.paint_erase = false;
        st.paint_brush_cells.clear();
        for (size_t i = 0; i < _inline_selected_tiles.size(); ++i)
            st.paint_brush_cells.push_back({(int)i, 0, _inline_selected_tiles[i]});
        if (!_inline_selected_tiles.empty()) st.paint_tile = _inline_selected_tiles.front();
    }

    static bool _has_suffix(const std::string& value, const std::string& suffix) {
        return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    void _scan(const EditorState& st) {
        _palette_refs.clear();
        const fs::path root = fs::path(st.asset_dir) / "tile_palettes";
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !_has_suffix(it->path().filename().string(), ".tilepalette.json")) continue;
            const fs::path relative = fs::relative(it->path(), fs::path(st.asset_dir), ec);
            if (!ec) _palette_refs.push_back(relative.generic_string());
            ec.clear();
        }
        std::sort(_palette_refs.begin(), _palette_refs.end());
    }
    void _scan_brushes(const EditorState& st) {
        _brush_refs.clear();
        const fs::path root = fs::path(st.asset_dir) / "tile_palettes";
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !_has_suffix(it->path().filename().string(), ".tilebrush.json")) continue;
            const fs::path relative = fs::relative(it->path(), fs::path(st.asset_dir), ec);
            if (!ec) _brush_refs.push_back(relative.generic_string());
            ec.clear();
        }
        std::sort(_brush_refs.begin(), _brush_refs.end());
    }
    void _load(EditorState& st, const std::string& reference) {
        TilePaletteAsset palette;
        if (!TilePaletteAsset::from_json(load_json(fs::path(st.asset_dir) / reference), palette)) {
            st.log_error("Could not load Tile Palette: " + reference); return;
        }
        _loaded_ref = reference; _palette = std::move(palette); _selected_tile = -1; _selected_tiles.clear();
        _selection_anchor_index = -1;
        st.active_tile_palette = reference;
    }
    void _draw_create_popup(EditorState& st) {
        if (!ImGui::BeginPopup("##new_tile_palette")) return;
        ImGui::InputText("Name", _new_palette_name, sizeof(_new_palette_name));
        ImGui::InputInt("Cell Size", &_new_cell_size, 1, 8); _new_cell_size = std::clamp(_new_cell_size, 1, 512);
        if (ImGui::Button("Create")) {
            const std::string safe = tile_palette_safe_name(_new_palette_name);
            const fs::path relative = fs::path("tile_palettes") / safe / (safe + ".tilepalette.json");
            const fs::path absolute = fs::path(st.asset_dir) / relative;
            std::error_code ec; fs::create_directories(absolute.parent_path(), ec);
            if (ec || fs::exists(absolute)) st.log_warn("Tile Palette name already exists or folder could not be created.");
            else {
                _palette = TilePaletteAsset{}; _palette.guid = tile_palette_guid(safe); _palette.name = _new_palette_name;
                _palette.cell_width = _new_cell_size; _palette.cell_height = _new_cell_size; _loaded_ref = relative.generic_string();
                if (save_json(absolute, _palette.to_json())) { st.log_success("Created Tile Palette: " + _loaded_ref); _scan(st); st.active_tile_palette = _loaded_ref; }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    void _add_image(EditorState& st, const std::string& source) {
        if (_loaded_ref.empty() || !atlas_is_image_file(fs::path(source))) { st.log_warn("Choose a compatible image for Tile Palette."); return; }
        int next_id = 0; for (const auto& tile : _palette.tiles) next_id = std::max(next_id, tile.id + 1);
        std::vector<int> added;
        // Sprite Editor writes a `.sprites` sidecar.  Import every slice as a
        // separate stable palette tile instead of forcing the user to type
        // crop rectangles or re-slice the texture here.
        fs::path slices_path = fs::path(st.asset_dir) / source;
        slices_path.replace_extension(".sprites");
        const auto slices = load_json(slices_path);
        if (slices.is_array()) {
            for (const auto& slice : slices) {
                if (!slice.is_object() || slice.value("w", 0) <= 0 || slice.value("h", 0) <= 0) continue;
                TilePaletteTile tile; tile.id = next_id++; tile.guid = tile_palette_guid(source + std::to_string(tile.id)); tile.source = source;
                tile.source_x = slice.value("x", 0); tile.source_y = slice.value("y", 0);
                tile.source_w = slice.value("w", 0); tile.source_h = slice.value("h", 0);
                _palette.tiles.push_back(std::move(tile)); added.push_back(next_id - 1);
            }
        }
        if (added.empty()) {
            TilePaletteTile tile; tile.id = next_id; tile.guid = tile_palette_guid(source); tile.source = source;
            _palette.tiles.push_back(std::move(tile)); added.push_back(next_id);
        }
        _build(st); _selected_tile = added.front(); _selected_tiles = added; _activate_tiles(st, _selected_tiles);
    }
    void _build(EditorState& st) {
        std::string diagnostic;
        if (build_tile_palette_atlas(st, _palette, fs::path(st.asset_dir) / _loaded_ref, diagnostic)) {
            st.log_success("Tile Palette atlas built: " + _palette.atlas); _scan(st);
        } else st.log_error("Tile Palette: " + diagnostic);
    }
    void _save_manifest(EditorState& st) {
        if (!save_json(fs::path(st.asset_dir) / _loaded_ref, _palette.to_json())) st.log_error("Could not save Tile Palette.");
    }
    TilePaletteTile* _find_tile(int id) {
        for (auto& tile : _palette.tiles) if (tile.id == id) return &tile;
        return nullptr;
    }
    void _assign_to_selected_tilemap(EditorState& st) {
        Entity* selected = st.selected_entity();
        if (!selected || !has_component(*selected, "Tilemap")) { st.log_warn("Select a Tilemap entity before assigning a palette."); return; }
        auto& map = (*selected)["components"]["Tilemap"];
        const std::string current_palette = map.value("tile_palette", std::string());
        bool has_painted_cells = false;
        if (map.contains("grid") && map["grid"].is_array()) {
            for (const auto& row : map["grid"]) {
                if (!row.is_array()) continue;
                for (const auto& cell : row) {
                    if (cell.is_number_integer() && cell.get<int>() >= 0) { has_painted_cells = true; break; }
                }
                if (has_painted_cells) break;
            }
        }
        if (has_painted_cells && current_palette != _loaded_ref) {
            st.log_warn("Palette assignment blocked: this Tilemap already has painted cells. Clear or duplicate it before assigning a palette, so existing tile IDs cannot be remapped to different art.");
            return;
        }
        map["tile_palette"] = _loaded_ref; map["tile_size"] = _palette.cell_width;
        map["_grid_cell_width"] = _palette.cell_width; map["_grid_cell_height"] = _palette.cell_height;
        Entity collision = Entity::object();
        for (const auto& tile : _palette.tiles) collision[std::to_string(tile.id)] = tile.collision;
        map["tile_collision"] = std::move(collision);
        st.active_tile_palette = _loaded_ref;
        st.log_success("Assigned Tile Palette to " + selected->value("name", "Tilemap") + ".");
    }
    void _activate_tiles(EditorState& st, const std::vector<int>& ids) {
        st.active_tile_palette = _loaded_ref; st.active_tile_brush.clear(); st.paint_erase = false; st.paint_brush_cells.clear();
        const int columns = _brush_columns > 0 ? std::min<int>(_brush_columns, std::max<int>(1, ids.size())) : (int)ids.size();
        for (size_t index = 0; index < ids.size(); ++index)
            st.paint_brush_cells.push_back({(int)(index % columns), (int)(index / columns), ids[index]});
        if (!ids.empty()) st.paint_tile = ids.front();
    }
    void _activate_brush(EditorState& st, const TileBrushAsset& brush, const std::string& reference) {
        st.active_tile_palette = brush.palette; st.active_tile_brush = reference; st.paint_erase = false; st.paint_brush_cells.clear();
        for (const auto& cell : brush.cells) st.paint_brush_cells.push_back({cell.x, cell.y, cell.tile_id});
        if (!brush.cells.empty()) st.paint_tile = brush.cells.front().tile_id;
    }
    void _draw_brush_popup(EditorState& st) {
        if (!ImGui::BeginPopup("##save_tile_brush")) return;
        ImGui::TextDisabled("The selected tiles are stored as a reusable brush pattern.");
        ImGui::InputText("Brush Name", _new_brush_name, sizeof(_new_brush_name));
        ImGui::SetNextItemWidth(130.f);
        ImGui::InputInt("Tiles per row", &_brush_columns);
        _brush_columns = std::clamp(_brush_columns, 0, std::max(1, (int)_selected_tiles.size()));
        ImGui::TextDisabled("0 keeps one row; choose e.g. 2 to save a 2 x N brush.");
        if (ImGui::Button("Save Brush")) {
            const fs::path folder = (fs::path(st.asset_dir) / _loaded_ref).parent_path() / "brushes";
            std::error_code ec; fs::create_directories(folder, ec);
            TileBrushAsset brush; brush.guid = tile_palette_guid(_new_brush_name); brush.name = _new_brush_name; brush.palette = _loaded_ref;
            const int columns = _brush_columns > 0 ? _brush_columns : (int)_selected_tiles.size();
            for (size_t index = 0; index < _selected_tiles.size(); ++index)
                brush.cells.push_back({(int)(index % columns), (int)(index / columns), _selected_tiles[index]});
            const fs::path path = folder / (tile_palette_safe_name(brush.name) + ".tilebrush.json");
            if (!ec && save_json(path, brush.to_json())) {
                const fs::path reference = fs::relative(path, fs::path(st.asset_dir), ec);
                if (!ec) { _activate_brush(st, brush, reference.generic_string()); _scan_brushes(st); st.log_success("Saved Tile Brush: " + brush.name); }
            } else st.log_error("Could not save Tile Brush.");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
};
