#pragma once

// Central registry for dockable/editor tools.  It prevents the Window menu,
// command palette and tool availability from drifting apart as features grow.

#include <imgui.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

enum class ToolMaturity { Production, Experimental };

struct EditorToolDescriptor {
    std::string id;
    std::string group;
    std::string title;
    std::string shortcut;
    std::string description;
    ToolMaturity maturity = ToolMaturity::Production;
    std::function<void()> open;
    std::function<bool()> is_open;
    std::function<bool()> is_available;
    std::function<std::string()> unavailable_reason;
};

class EditorToolRegistry {
public:
    void add(EditorToolDescriptor descriptor) { _tools.push_back(std::move(descriptor)); }

    const std::vector<EditorToolDescriptor>& entries() const { return _tools; }

    void draw_window_menu() const {
        std::vector<std::string> groups;
        for (const auto& tool : _tools) {
            if (tool.maturity == ToolMaturity::Experimental && !_show_experimental) continue;
            if (std::find(groups.begin(), groups.end(), tool.group) == groups.end()) groups.push_back(tool.group);
        }
        for (const std::string& group : groups) {
            if (!ImGui::BeginMenu(group.c_str())) continue;
            for (const auto& tool : _tools) {
                if (tool.group != group) continue;
                if (tool.maturity == ToolMaturity::Experimental && !_show_experimental) continue;
                const bool available = !tool.is_available || tool.is_available();
                const bool selected = tool.is_open && tool.is_open();
                std::string label = tool.title;
                if (tool.maturity == ToolMaturity::Experimental) label += "  (Experimental)";
                if (ImGui::MenuItem(label.c_str(), tool.shortcut.empty() ? nullptr : tool.shortcut.c_str(), selected, available)) {
                    tool.open();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(tool.description.c_str());
                    if (tool.maturity == ToolMaturity::Experimental)
                        ImGui::TextDisabled("Experimental: verify against your project before shipping.");
                    if (!available && tool.unavailable_reason)
                        ImGui::TextDisabled("%s", tool.unavailable_reason().c_str());
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Show experimental tools", nullptr, &_show_experimental);
    }

private:
    std::vector<EditorToolDescriptor> _tools;
    mutable bool _show_experimental = false;
};
