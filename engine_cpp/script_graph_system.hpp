#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <utility>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "script_system.hpp"
#include "unity2d_script_api.hpp"
#include "entity.hpp"
#include "runtime_value.hpp"

// Runtime visual scripting graph system.
// Loads event graphs saved by the editor and executes them at runtime.

struct ScriptGraphNode {
    int id;
    std::string type;
    std::string label;
    float x=0, y=0;
    std::string p1, p2, p3;  // text params
    float fp = 0.f;           // float param
    struct Port {
        int id = 0;
        std::string label;
        std::string type = "exec";
        std::string literal;
    };
    std::vector<Port> in_pins;
    std::vector<Port> out_pins;
};

struct ScriptGraphLink {
    int from_id = 0, to_id = 0;
    int from_pin = 0, to_pin = 0;
};

// Variable declarations are graph-owned authoring data, not a side effect of
// whichever Set Variable node happened to run first.  Runtime state is still
// per entity/graph-key; these values only seed that isolated state on load.
struct ScriptGraphVariable {
    std::string name;
    std::string type = "float";
    std::string default_value = "0";
};

class ScriptGraph {
public:
    std::vector<ScriptGraphNode> nodes;
    std::vector<ScriptGraphLink> links;
    std::vector<ScriptGraphVariable> variables;

    bool has_type(const std::string& type) const {
        for (auto& n : nodes) if (n.type == type) return true;
        return false;
    }
    bool has_any_type(const std::vector<std::string>& types) const {
        for (auto& n : nodes) for (auto& t : types) if (n.type == t) return true;
        return false;
    }

    bool load_json(const nlohmann::json& j) {
        nodes.clear(); links.clear(); variables.clear();
        const nlohmann::json* node_array = nullptr;
        const nlohmann::json* link_array = nullptr;
        if (j.is_array()) {
            node_array = &j;
        } else if (j.is_object() && j.contains("nodes") && j["nodes"].is_array()) {
            // Versioned graph assets are accepted alongside the original array format.
            node_array = &j["nodes"];
            if (j.contains("links") && j["links"].is_array()) link_array = &j["links"];
        } else {
            return false;
        }
        const auto load_variables = [&](const nlohmann::json& source) {
            if (!source.contains("variables") || !source["variables"].is_array()) return;
            for (const auto& entry : source["variables"]) {
                if (!entry.is_object()) continue;
                ScriptGraphVariable variable;
                variable.name = entry.value("name", std::string());
                variable.type = entry.value("type", std::string("float"));
                variable.default_value = entry.value("default", entry.value("value", std::string("0")));
                if (variable.name.empty()) continue;
                const bool duplicate = std::any_of(variables.begin(), variables.end(),
                    [&](const ScriptGraphVariable& existing) { return existing.name == variable.name; });
                if (!duplicate) variables.push_back(std::move(variable));
            }
        };
        if (j.is_object()) load_variables(j);
        for (auto& jn : *node_array) {
            const bool is_metadata = jn.is_object() && jn.contains("links") &&
                                     (jn.value("_meta", false) || !jn.contains("id"));
            if (!jn.is_object() || is_metadata) {
                if (jn.is_object() && jn.contains("links") && jn["links"].is_array()) link_array = &jn["links"];
                if (jn.is_object()) load_variables(jn);
                continue;
            }
            ScriptGraphNode n;
            n.id = jn.value("id", 0);
            n.type = jn.value("type", "log");
            n.label = jn.value("label", n.type);
            n.x = jn.value("x", 0.f);
            n.y = jn.value("y", 0.f);
            n.p1 = jn.value("p1", "");
            n.p2 = jn.value("p2", "");
            n.p3 = jn.value("p3", "");
            n.fp = jn.value("fp", 0.f);
            auto load_ports = [](const nlohmann::json& ports, std::vector<ScriptGraphNode::Port>& into) {
                if (!ports.is_array()) return;
                for (const auto& jp : ports) {
                    if (!jp.is_object()) continue;
                    ScriptGraphNode::Port p;
                    p.id = jp.value("id", 0);
                    p.label = jp.value("label", "");
                    p.type = jp.value("type", "exec");
                    p.literal = jp.value("literal", jp.value("lit", ""));
                    into.push_back(std::move(p));
                }
            };
            if (jn.contains("in_pins")) load_ports(jn["in_pins"], n.in_pins);
            if (jn.contains("out_pins")) load_ports(jn["out_pins"], n.out_pins);
            nodes.push_back(n);
        }
        if (link_array) {
            for (const auto& jl : *link_array) {
                if (!jl.is_object()) continue;
                ScriptGraphLink link;
                link.from_id = jl.value("fn", jl.value("from", 0));
                link.to_id = jl.value("tn", jl.value("to", 0));
                link.from_pin = jl.value("fp", 0);
                link.to_pin = jl.value("tp", 0);
                if (link.from_id != 0 && link.to_id != 0) links.push_back(link);
            }
        } else {
            // Legacy format: outs stored on individual nodes.
            for (const auto& jn : *node_array) {
                if (!jn.is_object() || jn.value("_meta", false)) continue;
                    int nid = jn.value("id", 0);
                    if (jn.contains("outs")) {
                        for (auto& v : jn["outs"]) {
                            links.push_back({nid, v.get<int>(), 0, 0});
                        }
                    }
            }
        }
        return true;
    }

    nlohmann::json save_json() const {
        // Write the same V3 document shape the editor owns. The loader still
        // accepts the original array form for old project assets.
        nlohmann::json j = {{"format", "gameengine.visual-script"}, {"version", 3},
                            {"nodes", nlohmann::json::array()}, {"links", nlohmann::json::array()},
                            {"variables", nlohmann::json::array()}};
        for (auto& n : nodes) {
            nlohmann::json jn;
            jn["id"] = n.id;
            jn["type"] = n.type;
            jn["label"] = n.label;
            jn["x"] = n.x;
            jn["y"] = n.y;
            jn["p1"] = n.p1;
            jn["p2"] = n.p2;
            jn["p3"] = n.p3;
            jn["fp"] = n.fp;
            auto save_ports = [](const std::vector<ScriptGraphNode::Port>& ports) {
                nlohmann::json result = nlohmann::json::array();
                for (const auto& p : ports) result.push_back({{"id", p.id}, {"label", p.label}, {"type", p.type}, {"literal", p.literal}});
                return result;
            };
            jn["in_pins"] = save_ports(n.in_pins);
            jn["out_pins"] = save_ports(n.out_pins);
            j["nodes"].push_back(jn);
        }
        for (auto& ln : links) {
            nlohmann::json jl;
            jl["fn"] = ln.from_id;
            jl["fp"] = ln.from_pin;
            jl["tn"] = ln.to_id;
            jl["tp"] = ln.to_pin;
            j["links"].push_back(jl);
        }
        for (const auto& variable : variables) {
            j["variables"].push_back({{"name", variable.name}, {"type", variable.type},
                                        {"default", variable.default_value}});
        }
        return j;
    }
};

// Event context must travel with queued graph work. A raw Entity* is unsafe
// across Spawn/Destroy because EntityList can reallocate between frames, so
// queued work stores stable IDs and resolves them only when a node runs.
struct GraphExecutionContext {
    int source_id = 0;
    int other_id = 0;
    // UI/custom events carry their action through delayed and downstream
    // graph work without relying on a dangling entity pointer.
    std::string event_value;
};

// Per-entity graph execution state
struct GraphExecutionState {
    struct PendingNode { int node_id = 0; GraphExecutionContext context; };
    struct DelayedNode { float remaining = 0.f; int node_id = 0; GraphExecutionContext context; };

    std::vector<PendingNode> exec_stack; // pending node IDs to execute
    std::vector<DelayedNode> delays;     // delayed work retains event context
    GraphExecutionContext active_context;
    bool running = false;

    void push(int node_id) { push(node_id, active_context); }
    void push(int node_id, GraphExecutionContext context) {
        exec_stack.push_back({node_id, context});
    }
    void delay(float seconds, int node_id) {
        delays.push_back({seconds, node_id, active_context});
    }
    bool has_pending() const { return !exec_stack.empty() || !delays.empty(); }
};

class ScriptGraphSystem {
public:
    static ScriptGraphSystem& instance() {
        static ScriptGraphSystem s;
        return s;
    }

    // Load a graph for a scene (idempotent - only loads once).  Assets can
    // live directly in asset_dir or in its assets/ child so standalone and
    // editor Play mode resolve the same graph file.
    void load_scene_graph(const std::string& scene_name, const std::string& asset_dir) {
        if (_graphs.find(scene_name) != _graphs.end()) return; // already loaded
        _asset_dirs[scene_name] = asset_dir;
        std::string path = resolve_scene_graph_path(scene_name, asset_dir);
        auto j = load_json_file(path);
        if (j.is_array() || j.is_object()) {
            auto& g = _graphs[scene_name];
            if (!g.load_json(j)) {
                _graphs[scene_name] = ScriptGraph();
            }
            seed_graph_variables(scene_name, _graphs[scene_name]);
            _graph_states[scene_name] = GraphExecutionState();
            if (!_graphs[scene_name].nodes.empty())
                Debug::log("[ScriptGraph] Loaded graph: " + path);
        } else {
            // Mark as loaded (empty) so we don't retry every frame
            _graphs[scene_name] = ScriptGraph();
            _graph_vars.erase(scene_name);
            _graph_states[scene_name] = GraphExecutionState();
        }
    }

    // Get graph (const access)
    const ScriptGraph& get_graph(const std::string& scene_name) const {
        static ScriptGraph empty;
        auto it = _graphs.find(scene_name);
        if (it != _graphs.end()) return it->second;
        return empty;
    }

    // Force reload (for editor Play mode)
    void reload_scene_graph(const std::string& scene_name, const std::string& asset_dir) {
        _graphs.erase(scene_name);
        _graph_states.erase(scene_name);
        load_scene_graph(scene_name, asset_dir.empty() ? remembered_asset_dir(scene_name) : asset_dir);
    }

    // Entity VisualScript components use their own assets and execution
    // contexts. Keeping those keyed separately avoids variables, delays, and
    // Do Once state leaking between two entities that reuse the same graph.
    void load_graph_asset(const std::string& graph_key, const std::string& path) {
        if (_graphs.find(graph_key) != _graphs.end()) return;
        _graph_paths[graph_key] = path;
        auto j = load_json_file(path);
        ScriptGraph graph;
        if ((j.is_array() || j.is_object()) && graph.load_json(j)) {
            _graphs[graph_key] = std::move(graph);
            seed_graph_variables(graph_key, _graphs[graph_key]);
            if (!_graphs[graph_key].nodes.empty())
                Debug::log("[ScriptGraph] Loaded component graph: " + path);
        } else {
            _graphs[graph_key] = ScriptGraph();
            _graph_vars.erase(graph_key);
        }
        _graph_states[graph_key] = GraphExecutionState();
    }

    void reset_scene_context(const std::string& scene_name) {
        const std::string entity_prefix = scene_name + "#entity:";
        const auto belongs_to_scene = [&](const std::string& key) {
            return key == scene_name || key.rfind(entity_prefix, 0) == 0;
        };
        for (auto it = _graphs.begin(); it != _graphs.end();) {
            if (belongs_to_scene(it->first)) it = _graphs.erase(it); else ++it;
        }
        for (auto it = _graph_states.begin(); it != _graph_states.end();) {
            if (belongs_to_scene(it->first)) it = _graph_states.erase(it); else ++it;
        }
        for (auto it = _asset_dirs.begin(); it != _asset_dirs.end();) {
            if (belongs_to_scene(it->first)) it = _asset_dirs.erase(it); else ++it;
        }
        for (auto it = _graph_paths.begin(); it != _graph_paths.end();) {
            if (belongs_to_scene(it->first)) it = _graph_paths.erase(it); else ++it;
        }
        for (auto it = _graph_vars.begin(); it != _graph_vars.end();) {
            if (belongs_to_scene(it->first)) it = _graph_vars.erase(it); else ++it;
        }
        _active_tweens.erase(std::remove_if(_active_tweens.begin(), _active_tweens.end(),
            [&](const ActiveTween& tween) { return belongs_to_scene(tween.graph_key); }), _active_tweens.end());
        for (auto it = _flipflop_state.begin(); it != _flipflop_state.end();) {
            if (it->first.rfind(scene_name + ":", 0) == 0 || it->first.rfind(entity_prefix, 0) == 0)
                it = _flipflop_state.erase(it);
            else ++it;
        }
        for (auto it = _once_flags.begin(); it != _once_flags.end();) {
            if (it->rfind(scene_name + ":", 0) == 0 || it->rfind(entity_prefix, 0) == 0)
                it = _once_flags.erase(it);
            else ++it;
        }
    }

    void set_entity_list(EntityList* entities) {
        _entities = entities;
        // VisualScript can be the only behaviour in a scene, so it cannot
        // rely on ScriptSystem having advanced the shared allocator first.
        // Reserve against the live scene before its Spawn nodes allocate.
        if (_entities) reserve_entity_ids(*_entities);
    }

    // Trigger an event - finds matching event nodes and pushes them to execution
    void trigger(const std::string& event_name, const std::string& scene_name, Entity* source = nullptr,
                 Entity* other = nullptr, std::string event_value = {}) {
        auto it = _graphs.find(scene_name);
        if (it == _graphs.end()) return;
        auto& graph = it->second;
        auto& state = _graph_states[scene_name];
        const std::string graph_event = normalize_event_name(event_name);

        const GraphExecutionContext context{
            source ? source->value("id", 0) : 0,
            other ? other->value("id", 0) : 0,
            std::move(event_value)
        };
        for (auto& node : graph.nodes) {
            if (node.type == graph_event && event_matches_filter(node, graph_event, source, other, context.event_value)) {
                state.push(node.id, context);
            }
        }
        if (state.has_pending()) state.running = true;
    }

    // Update: process pending nodes and delay timers
    void update(float dt, const std::string& scene_name) {
        auto it = _graph_states.find(scene_name);
        if (it == _graph_states.end()) return;
        auto& state = it->second;
        auto& graph = _graphs[scene_name];
        _current_delta_time = std::max(0.f, dt);
        if (!state.running && !has_active_tweens(scene_name)) return;

        // Process delay timers
        for (int i = (int)state.delays.size() - 1; i >= 0; --i) {
            auto& delayed = state.delays[i];
            delayed.remaining -= dt;
            if (delayed.remaining <= 0.f) {
                state.push(delayed.node_id, delayed.context);
                state.delays.erase(state.delays.begin() + i);
            }
        }

        // Process active tweens (interpolated movement)
        for (int i = (int)_active_tweens.size() - 1; i >= 0; --i) {
            auto& tw = _active_tweens[i];
            if (tw.graph_key != scene_name) continue;
            tw.elapsed += dt;
            if (tw.elapsed >= tw.duration) {
                tw.elapsed = tw.duration;
                // Final position
                if (Entity* entity = find_entity_by_id(tw.entity_id)) {
                    (*entity)["components"]["Transform"]["x"] = tw.tx;
                    (*entity)["components"]["Transform"]["y"] = tw.ty;
                }
                // Continue from the tween's output pins once it has really
                // completed. Re-queuing the tween node itself caused an
                // accidental restart loop and eventual frame stalls.
                for (int output : tw.completion_outputs) state.push(output, tw.context);
                _active_tweens.erase(_active_tweens.begin() + i);
            } else {
                float t = tw.elapsed / tw.duration;
                t = t * t * (3.f - 2.f * t); // smoothstep
                if (Entity* entity = find_entity_by_id(tw.entity_id)) {
                    (*entity)["components"]["Transform"]["x"] = tw.sx + (tw.tx - tw.sx) * t;
                    (*entity)["components"]["Transform"]["y"] = tw.sy + (tw.ty - tw.sy) * t;
                }
            }
        }

        // Process execution stack (limit per frame to avoid infinite loops)
        int safety = 500;
        while (!state.exec_stack.empty() && safety-- > 0) {
            const auto pending = state.exec_stack.back();
            state.exec_stack.pop_back();
            state.active_context = pending.context;
            _active_context = pending.context;
            process_node(pending.node_id, graph, state, scene_name);
        }

        if (safety <= 0 && !state.exec_stack.empty()) {
            // Keep the editor/game responsive if a graph accidentally loops in
            // one frame. The remaining work continues next frame.
            Debug::log_warning("[ScriptGraph] Execution budget reached; graph work deferred to next frame.");
        }
        state.running = state.has_pending() || has_active_tweens(scene_name);
    }

    // Must be called outside any EntityList iteration. It is deliberately a
    // separate commit point so spawning from an entity component graph cannot
    // invalidate the loop currently visiting that entity.
    void flush_pending_spawns() {
        if (_entities && !_pending_spawns.empty()) {
            const size_t required = _entities->size() + _pending_spawns.size();
            if (_entities->capacity() < required)
                _entities->reserve(std::max(required + 64u, _entities->size() * 2));
            _entities->insert(_entities->end(), _pending_spawns.begin(), _pending_spawns.end());
            _pending_spawns.clear();
        }
    }

    // Check pressed / held / released key events.  These are separate graph
    // entry points so a Blueprint can model movement, one-shot actions and
    // release-to-charge behaviour without a native C++ script.
    void check_key_events(const std::string& scene_name, Entity* source = nullptr) {
        auto git = _graphs.find(scene_name);
        if (git == _graphs.end()) return;
        if (!git->second.has_type("on_key") && !git->second.has_type("on_key_held") &&
            !git->second.has_type("on_key_released")) return;
        
        auto& graph = git->second;
        auto& state = _graph_states[scene_name];
        bool triggered = false;
        const GraphExecutionContext context{source ? source->value("id", 0) : 0, 0};

        for (auto& node : graph.nodes) {
            if ((node.type != "on_key" && node.type != "on_key_held" && node.type != "on_key_released") ||
                node.p1.empty()) continue;
            SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
            try { sc = (SDL_Scancode)std::stoi(node.p1); }
            catch (...) { sc = SDL_GetScancodeFromName(node.p1.c_str()); }
            if (sc == SDL_SCANCODE_UNKNOWN) continue;
            const bool matches = node.type == "on_key" ? Input::GetKeyDown(sc) :
                (node.type == "on_key_held" ? Input::GetKey(sc) : Input::GetKeyUp(sc));
            if (matches) {
                state.push(node.id, context);
                triggered = true;
            }
        }
        if (triggered) state.running = true;
    }

private:
    std::unordered_map<std::string, ScriptGraph> _graphs;
    std::unordered_map<std::string, GraphExecutionState> _graph_states;
    std::unordered_map<std::string, std::string> _asset_dirs;
    std::unordered_map<std::string, std::string> _graph_paths;
    EntityList* _entities = nullptr;
    GraphExecutionContext _active_context;
    std::vector<Entity> _pending_spawns;
    float _current_delta_time = 0.f;

    void seed_graph_variables(const std::string& graph_key, const ScriptGraph& graph) {
        auto& values = _graph_vars[graph_key];
        values.clear();
        for (const auto& variable : graph.variables) {
            if (!variable.name.empty()) values[variable.name] = variable.default_value;
        }
    }

    std::string remembered_asset_dir(const std::string& scene_name) const {
        auto it = _asset_dirs.find(scene_name);
        return it == _asset_dirs.end() ? std::string() : it->second;
    }

    std::string resolve_scene_graph_path(const std::string& scene_name, const std::string& asset_dir) const {
        namespace fs = std::filesystem;
        const std::string filename = "event_graph_" + scene_name + ".json";
        fs::path root(asset_dir);
        const fs::path direct = root / filename;
        const fs::path nested = root / "assets" / filename;
        std::error_code ec;
        if (!asset_dir.empty() && fs::exists(direct, ec)) return direct.string();
        ec.clear();
        if (!asset_dir.empty() && fs::exists(nested, ec)) return nested.string();
        // Use the direct form for a useful diagnostic path when the asset does
        // not yet exist.
        return direct.string();
    }

    nlohmann::json load_json_file(const std::string& path) {
        // Try to use file I/O
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return nullptr;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); return nullptr; }
        std::string buf((size_t)sz, '\0');
        const size_t read = fread(buf.data(), 1, buf.size(), f);
        fclose(f);
        if (read != buf.size()) return nullptr;
        try { return nlohmann::json::parse(buf); }
        catch (...) { return nullptr; }
    }

    static bool entity_has_tag(const Entity* entity, const std::string& wanted) {
        if (!entity || wanted.empty()) return false;
        if (entity->value("tag", std::string()) == wanted) return true;
        if (!entity->contains("tags") || !(*entity)["tags"].is_array()) return false;
        for (const auto& tag : (*entity)["tags"])
            if (tag.is_string() && tag.get<std::string>() == wanted) return true;
        return false;
    }

    // Physics/script callbacks use Unity-style names such as
    // on_trigger_enter, while the graph palette presents their entry nodes as
    // "On Trigger Enter" with the stable graph type on_trigger. Normalize at
    // the runtime boundary so authored palette nodes receive real contacts.
    static std::string normalize_event_name(const std::string& event_name) {
        if (event_name == "on_trigger_enter") return "on_trigger";
        if (event_name == "on_collision_enter") return "on_collision";
        return event_name;
    }

    static bool event_matches_filter(const ScriptGraphNode& node, const std::string& event_name,
                                     const Entity* source, const Entity* other,
                                     const std::string& event_value = {}) {
        if (event_name == "on_ui_click")
            return node.p1.empty() || node.p1 == event_value;
        const bool is_contact_event = event_name == "on_trigger" ||
            event_name == "on_trigger_stay" || event_name == "on_trigger_exit" ||
            event_name == "on_collision" || event_name == "on_collision_stay" ||
            event_name == "on_collision_exit";
        if (!is_contact_event || node.p1.empty()) return true;
        // In a collision graph, the useful filter is normally the *other*
        // collider's tag. Checking source too makes scene-wide graphs useful
        // when the event was injected by a system without an "other" entity.
        return entity_has_tag(other, node.p1) || entity_has_tag(source, node.p1);
    }

    // Data wires are resolved at the point a node consumes an input.  This is
    // deliberately separate from exec flow: a value-producing node can feed
    // several actions without needing a dummy execution wire, while an
    // unconnected input still uses the value authored directly on its card.
    const ScriptGraphNode* graph_node(const ScriptGraph& graph, int id) const {
        for (const auto& node : graph.nodes) if (node.id == id) return &node;
        return nullptr;
    }

    const ScriptGraphNode::Port* named_input(const ScriptGraphNode& node,
                                              const std::string& label) const {
        for (const auto& port : node.in_pins)
            if (port.type != "exec" && port.label == label) return &port;
        return nullptr;
    }

    static SDL_Scancode graph_scancode(const std::string& name_or_number) {
        if (name_or_number.empty()) return SDL_SCANCODE_UNKNOWN;
        try { return static_cast<SDL_Scancode>(std::stoi(name_or_number)); }
        catch (...) { return SDL_GetScancodeFromName(name_or_number.c_str()); }
    }

    std::string graph_input(const ScriptGraphNode& node, ScriptGraph& graph,
                            const std::string& graph_key, const std::string& label,
                            const std::string& fallback,
                            std::unordered_set<int>* recursion = nullptr) {
        const ScriptGraphNode::Port* port = named_input(node, label);
        if (!port) return fallback; // legacy graph with no typed port
        for (const auto& link : graph.links) {
            if (link.to_id != node.id || link.to_pin != port->id) continue;
            const ScriptGraphNode* source = graph_node(graph, link.from_id);
            if (!source) continue;
            std::unordered_set<int> local_recursion;
            std::unordered_set<int>& visited = recursion ? *recursion : local_recursion;
            const std::string value = graph_output(*source, graph, graph_key, link.from_pin, visited);
            if (!value.empty()) return value;
        }
        return port->literal.empty() ? fallback : port->literal;
    }

    std::string graph_output(const ScriptGraphNode& node, ScriptGraph& graph,
                             const std::string& graph_key, int output_pin,
                             std::unordered_set<int>& visited) {
        if (!visited.insert(node.id).second) return {}; // data-cycle guard
        const auto finish = [&](std::string value) {
            visited.erase(node.id);
            return value;
        };
        const ScriptGraphNode::Port* port = nullptr;
        for (const auto& candidate : node.out_pins)
            if (candidate.id == output_pin) { port = &candidate; break; }
        if (!port || port->type == "exec") return finish({});
        if (!port->literal.empty()) return finish(port->literal);

        if (node.type == "on_ui_click" && port->label == "Action")
            return finish(_active_context.event_value);

        if (node.type == "input_key" && port->label == "Held") {
            const SDL_Scancode sc = graph_scancode(graph_input(node, graph, graph_key, "Key", node.p1, &visited));
            return finish(sc != SDL_SCANCODE_UNKNOWN && Input::GetKey(sc) ? "true" : "false");
        }
        if (node.type == "input_axis" && port->label == "Value") {
            const SDL_Scancode negative = graph_scancode(graph_input(node, graph, graph_key, "Negative", node.p1, &visited));
            const SDL_Scancode positive = graph_scancode(graph_input(node, graph, graph_key, "Positive", node.p2, &visited));
            const float value = (positive != SDL_SCANCODE_UNKNOWN && Input::GetKey(positive) ? 1.f : 0.f) -
                                (negative != SDL_SCANCODE_UNKNOWN && Input::GetKey(negative) ? 1.f : 0.f);
            return finish(std::to_string(value));
        }

        if (node.type == "get_variable") {
            const std::string name = graph_input(node, graph, graph_key, "Name", node.p1, &visited);
            const auto graph_it = _graph_vars.find(graph_key);
            if (graph_it == _graph_vars.end()) return finish({});
            const auto value_it = graph_it->second.find(name);
            return finish(value_it == graph_it->second.end() ? std::string() : value_it->second);
        }
        if (node.type == "get_field") {
            Entity* entity = find_entity(graph_input(node, graph, graph_key, "Entity", node.p1, &visited));
            const std::string field = graph_input(node, graph, graph_key, "Field", node.p2, &visited);
            return finish(entity && !field.empty() ? get_field_string(*entity, field) : std::string());
        }
        if (node.type == "math_add" || node.type == "math_sub" || node.type == "math_mul" ||
            node.type == "math_div" || node.type == "random_range" || node.type == "clamp") {
            return finish(std::to_string(evaluate_math_node(node, graph, graph_key, &visited)));
        }
        if (node.type == "reroute") {
            return finish(graph_input(node, graph, graph_key, "In", std::string(), &visited));
        }
        return finish({});
    }

    float evaluate_math_node(const ScriptGraphNode& node, ScriptGraph& graph,
                             const std::string& graph_key,
                             std::unordered_set<int>* recursion = nullptr) {
        const auto input = [&](const std::string& label, const std::string& fallback) {
            return number_or(graph_input(node, graph, graph_key, label, fallback, recursion), 0.f);
        };
        if (node.type == "math_add") return input("A", std::to_string(node.fp)) + input("B", node.p2);
        if (node.type == "math_sub") return input("A", std::to_string(node.fp)) - input("B", node.p2);
        if (node.type == "math_mul") return input("A", std::to_string(node.fp)) * input("B", node.p2);
        if (node.type == "math_div") {
            const float denominator = input("B", node.p2);
            return denominator == 0.f ? 0.f : input("A", std::to_string(node.fp)) / denominator;
        }
        if (node.type == "random_range") {
            float minimum = input("Min", std::to_string(node.fp));
            float maximum = input("Max", node.p2);
            if (maximum < minimum) std::swap(minimum, maximum);
            return minimum + ((float)rand() / (float)RAND_MAX) * (maximum - minimum);
        }
        if (node.type == "clamp") {
            float minimum = input("Min", node.p2);
            float maximum = input("Max", node.p3);
            if (minimum > maximum) std::swap(minimum, maximum);
            return std::max(minimum, std::min(maximum, input("Value", std::to_string(node.fp))));
        }
        return 0.f;
    }

    void process_node(int node_id, ScriptGraph& graph, GraphExecutionState& state, const std::string& scene_name) {
        ScriptGraphNode* node = nullptr;
        for (auto& n : graph.nodes) { if (n.id == node_id) { node = &n; break; } }
        if (!node) return;
        const auto input = [&](const std::string& label, const std::string& fallback) {
            return graph_input(*node, graph, scene_name, label, fallback);
        };

        // Dispatch by type
        if (node->type == "on_start" || node->type == "on_trigger" || node->type == "on_trigger_exit" || node->type == "on_trigger_stay" || node->type == "on_collision" || node->type == "on_collision_exit" || node->type == "on_collision_stay" || node->type == "on_key" || node->type == "on_key_held" || node->type == "on_key_released" || node->type == "on_update" || node->type == "on_fixed_update" || node->type == "on_ui_click") {
            // Event nodes are entry points, but still need to advance the
            // execution wire. Leaving this empty made a saved graph look
            // valid in the editor while every event chain silently stopped
            // at its first node.
            pass_through(node->id, graph, state);
        }
        else if (node->type == "log" || node->type == "debug_log") {
            Debug::log("[Graph] " + input("Message", node->p1));
            pass_through(node->id, graph, state);
        }
        else if (node->type == "delay") {
            auto outputs = get_outputs(node->id, graph);
            for (int output : outputs) {
                state.delay(std::max(0.01f, number_or(input("Seconds", std::to_string(node->fp)), node->fp)), output);
            }
        }
        else if (node->type == "sequence") {
            // pass_through preserves the visible output-pin order while the
            // execution stack remains LIFO internally.
            pass_through(node->id, graph, state);
        }
        else if (node->type == "condition") {
            // Simple condition: check if p1 expression is truthy
            bool result = evaluate_condition(*node, graph, scene_name);
            auto outputs = get_outputs_for_label(node->id, graph, result ? "True" : "False", result ? 0 : 1);
            for (int output : outputs) state.push(output);
        }
        else if (node->type == "set_active") {
            Entity* e = find_entity(input("Entity", node->p1));
            const std::string active = input("Active", node->p2);
            if (e) { (*e)["active"] = (active == "true" || active == "1" || active.empty()); }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "destroy") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e) { (*e)["_destroyed"] = true; }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "spawn") {
            // Modern graphs expose the source as an entity pin. Legacy graphs
            // that stored a template name remain supported through the second
            // lookup, so existing authoring is not invalidated.
            const std::string template_name = input("Template Entity", input("Template", node->p1));
            if (!template_name.empty() && _entities) {
                Entity* source = find_entity(template_name);
                if (!source) {
                    for (auto& candidate : *_entities) {
                        if (entity_active(candidate) && candidate.value("name", "") == template_name) {
                            source = &candidate;
                            break;
                        }
                    }
                }
                if (source && entity_active(*source)) {
                        Entity clone = source->deep_clone();
                        clone["id"] = next_entity_id();
                        const float x = number_or(input("X", std::to_string(node->fp)), node->fp);
                        const float y = number_or(input("Y", node->p2), number_or(node->p2, 0.f));
                        if (x != 0.f || y != 0.f) {
                            clone["components"]["Transform"]["x"] = x;
                            clone["components"]["Transform"]["y"] = y;
                        }
                        // Appending directly can invalidate Entity* values held by
                        // physics, scripts, and active graph tweens. Commit new
                        // entities after this graph update has finished.
                        _pending_spawns.push_back(std::move(clone));
                }
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "play_animation") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e) {
                (*e)["components"]["Animator"]["_play"] = input("Animation", node->p2);
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_field") {
            Entity* e = find_entity(input("Entity", node->p1));
            const std::string field = input("Field", node->p2);
            if (e && !field.empty()) {
                set_field_path(*e, field, input("Value", node->p3));
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "load_scene") {
            const std::string scene = input("Scene", node->p1);
            if (!scene.empty()) {
                SceneManager::LoadScene(scene);
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "audio_play") {
            Entity* e = find_entity(input("Source", node->p1));
            if (e) {
                const std::string clip = input("Clip", node->p2);
                if (!clip.empty()) (*e)["components"]["AudioSource"]["clip"] = clip;
                (*e)["components"]["AudioSource"]["_play_now"] = true;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_text") {
            Entity* e = find_entity(input("Entity", node->p1));
            const std::string text = input("Text", node->p2);
            if (e) {
                // Use the component already on the selected entity.  This
                // allows one Blueprint action to drive screen-space UIText or
                // world-space TextMeshPro2D without silently creating an
                // unrelated second text component.
                if (has_component(*e, "TextMeshPro2D")) (*e)["components"]["TextMeshPro2D"]["text"] = text;
                else if (has_component(*e, "UIText")) (*e)["components"]["UIText"]["text"] = text;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_sprite") {
            Entity* e = find_entity(input("Entity", node->p1));
            const std::string sprite = input("Sprite", node->p2);
            if (e && has_component(*e, "SpriteRenderer") && !sprite.empty())
                (*e)["components"]["SpriteRenderer"]["texture"] = sprite;
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_ui_progress") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e && has_component(*e, "UIProgressBar")) {
                const float value = number_or(input("Value", std::to_string(node->fp)), node->fp);
                (*e)["components"]["UIProgressBar"]["value"] = value;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_audio_volume") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e && has_component(*e, "AudioSource")) {
                const float volume = std::max(0.f, std::min(1.f,
                    number_or(input("Volume", std::to_string(node->fp)), node->fp)));
                (*e)["components"]["AudioSource"]["volume"] = volume;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_velocity") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e && has_component(*e, "Rigidbody2D")) {
                float vx = number_or(input("X", std::to_string(node->fp)), node->fp);
                float vy = number_or(input("Y", node->p2), number_or(node->p2, 0.f));
                (*e)["components"]["Rigidbody2D"]["velocity_x"] = vx;
                (*e)["components"]["Rigidbody2D"]["velocity_y"] = vy;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "add_force") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e && has_component(*e, "Rigidbody2D")) {
                float fx = number_or(input("X", std::to_string(node->fp)), node->fp);
                float fy = number_or(input("Y", node->p2), number_or(node->p2, 0.f));
                float vx = (*e)["components"]["Rigidbody2D"].value("velocity_x", 0.f);
                float vy = (*e)["components"]["Rigidbody2D"].value("velocity_y", 0.f);
                (*e)["components"]["Rigidbody2D"]["velocity_x"] = vx + fx;
                (*e)["components"]["Rigidbody2D"]["velocity_y"] = vy + fy;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "move_by") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e && has_component(*e, "Transform")) {
                const float vx = number_or(input("X / sec", std::to_string(node->fp)), node->fp);
                const float vy = number_or(input("Y / sec", node->p2), number_or(node->p2, 0.f));
                (*e)["components"]["Transform"]["x"] = (*e)["components"]["Transform"].value("x", 0.f) + vx * _current_delta_time;
                (*e)["components"]["Transform"]["y"] = (*e)["components"]["Transform"].value("y", 0.f) + vy * _current_delta_time;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_rotation") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e && has_component(*e, "Transform"))
                (*e)["components"]["Transform"]["rotation"] = number_or(input("Degrees", std::to_string(node->fp)), node->fp);
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_gravity") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e && has_component(*e, "Rigidbody2D"))
                (*e)["components"]["Rigidbody2D"]["gravity_scale"] = number_or(input("Scale", std::to_string(node->fp)), node->fp);
            pass_through(node->id, graph, state);
        }
        else if (node->type == "wait") {
            auto outputs = get_outputs(node->id, graph);
            for (int output : outputs) {
                state.delay(std::max(0.01f, number_or(input("Seconds", std::to_string(node->fp)), node->fp)), output);
            }
        }
        else if (node->type == "print_value" || node->type == "log_value") {
            std::string val_str;
            const std::string entity = input("Entity", node->p1);
            const std::string field = input("Field", node->p2);
            if (!entity.empty()) {
                Entity* e = find_entity(entity);
                if (e && !field.empty()) val_str = get_field_string(*e, field);
            }
            Debug::log("[Graph] " + entity + "." + field + " = " + val_str);
            pass_through(node->id, graph, state);
        }
        else if (node->type == "math_add" || node->type == "math_sub" || node->type == "math_mul" || node->type == "math_div" || node->type == "random_range" || node->type == "clamp") {
            // The same evaluator powers the Result data pin and the legacy
            // "Store in" field. Connected A/B/Min/Max pins take precedence.
            if (!node->p1.empty()) {
                _graph_vars[scene_name][node->p1] = std::to_string(evaluate_math_node(*node, graph, scene_name));
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "for_loop") {
            int count = std::max(0, (int)number_or(input("Count", std::to_string(node->fp)), node->fp));
            auto outputs = get_outputs_for_label(node->id, graph, "Body", 0);
            auto completed = get_outputs_for_label(node->id, graph, "Completed", 1);
            // Stack is LIFO: queue completion first, then iterations in
            // reverse so the graph executes Body #1..#N, then Completed.
            for (auto it = completed.rbegin(); it != completed.rend(); ++it) state.push(*it);
            for (int i = count - 1; i >= 0; --i) {
                (void)i;
                for (auto it = outputs.rbegin(); it != outputs.rend(); ++it) state.push(*it);
            }
        }
        else if (node->type == "while_loop") {
            // Evaluate condition (p1) and loop body while true
            bool cond = evaluate_condition(*node, graph, scene_name);
            if (cond) {
                // Run Body before the next predicate check. Re-queue through
                // a tiny delay so a true condition cannot consume the entire
                // per-frame graph budget before its Body ever runs.
                state.delay(0.0001f, node->id);
                auto body = get_outputs_for_label(node->id, graph, "Body", 0);
                for (auto it = body.rbegin(); it != body.rend(); ++it) state.push(*it);
            } else {
                // Push Completed output (index 1)
                auto completed = get_outputs_for_label(node->id, graph, "Completed", 1);
                for (auto it = completed.rbegin(); it != completed.rend(); ++it) state.push(*it);
            }
        }
        else if (node->type == "branch") {
            bool cond = evaluate_condition(*node, graph, scene_name);
            for (int output : get_outputs_for_label(node->id, graph, cond ? "True" : "False", cond ? 0 : 1)) state.push(output);
        }
        else if (node->type == "flipflop") {
            // Toggle state per session
            auto& ffs = _flipflop_state[scene_name + ":" + std::to_string(node->id)];
            ffs = !ffs;
            for (int output : get_outputs_for_label(node->id, graph, ffs ? "A" : "B", ffs ? 0 : 1)) state.push(output);
        }
        else if (node->type == "gate") {
            bool is_open = evaluate_condition(*node, graph, scene_name);
            if (is_open) pass_through(node->id, graph, state);
        }
        else if (node->type == "do_once") {
            const std::string once_key = scene_name + ":" + std::to_string(node->id);
            if (_once_flags.find(once_key) == _once_flags.end()) {
                _once_flags.insert(once_key);
                pass_through(node->id, graph, state);
            }
        }
        else if (node->type == "teleport_to") {
            Entity* e = find_entity(input("Entity", node->p1));
            if (e) {
                float tx = number_or(input("X", std::to_string(node->fp)), node->fp);
                float ty = number_or(input("Y", node->p2), number_or(node->p2, 0.f));
                (*e)["components"]["Transform"]["x"] = tx;
                (*e)["components"]["Transform"]["y"] = ty;
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "breakpoint") {
            Debug::log_warning("[Graph] BREAKPOINT at node " + std::to_string(node->id) + " (" + node->label + ")");
            Debug::log("[Graph]   p1=" + node->p1);
            pass_through(node->id, graph, state);
        }
        else if (node->type == "set_variable") {
            const std::string name = input("Name", node->p1);
            if (!name.empty()) {
                _graph_vars[scene_name][name] = input("Value", node->p2.empty() ? std::to_string(node->fp) : node->p2);
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "get_variable") {
            // Just pass through; the value is conceptually read
            pass_through(node->id, graph, state);
        }
        else if (node->type == "increment_variable" || node->type == "decrement_variable") {
            if (!node->p1.empty()) {
                std::string& val = _graph_vars[scene_name][node->p1];
                try {
                    int iv = std::stoi(val);
                    iv += (node->type == "decrement_variable") ? -1 : 1;
                    val = std::to_string(iv);
                } catch (...) {
                    try {
                        float fv = std::stof(val);
                        fv += (node->type == "decrement_variable") ? -1.f : 1.f;
                        val = std::to_string(fv);
                    } catch (...) {
                        val = "1";
                    }
                }
            }
            pass_through(node->id, graph, state);
        }
        else if (node->type == "tween_to") {
            bool tween_started = false;
            Entity* e = find_entity(input("Entity", node->p1));
            if (e) {
                // New graphs expose typed Entity/X/Y/Duration pins. Retain
                // the old comma-separated Target field for existing assets.
                float tx = number_or(input("X", std::to_string(node->fp)), node->fp);
                float ty = number_or(input("Y", node->p2), number_or(node->p2, 0.f));
                float dur = std::max(0.01f, number_or(input("Duration", node->p3), number_or(node->p3, 0.3f)));
                const std::string legacy_target = node->p2;
                size_t comma = legacy_target.find(',');
                if (comma != std::string::npos) {
                    try { tx = std::stof(legacy_target.substr(0, comma)); } catch (...) {}
                    try { ty = std::stof(legacy_target.substr(comma + 1)); } catch (...) {}
                    dur = std::max(0.01f, number_or(node->p3, 0.3f));
                }
                // Get start position
                float sx = e->value("components", Entity::object())["Transform"].value("x", 0.f);
                float sy = e->value("components", Entity::object())["Transform"].value("y", 0.f);
                // Store the graph-local completion pins. This action owns
                // control flow until interpolation has reached its target.
                _active_tweens.push_back({scene_name, e->value("id", 0),
                    get_outputs(node->id, graph), state.active_context, sx, sy, tx, ty, dur, 0.f});
                tween_started = true;
            }
            // A bad/missing target should not dead-end an otherwise valid
            // graph. Only hold control flow when a tween is actually active.
            if (!tween_started) pass_through(node->id, graph, state);
        }
        else if (node->type == "comment") {
            // A comment normally has no exec pins, but preserve a manually
            // wired legacy graph instead of unexpectedly swallowing flow.
            pass_through(node->id, graph, state);
        }
        else {
            // Unknown node type - pass through
            pass_through(node->id, graph, state);
        }
    }

    void pass_through(int node_id, ScriptGraph& graph, GraphExecutionState& state) {
        auto outputs = get_outputs(node_id, graph);
        // `exec_stack` is LIFO. Push right-to-left so a graph's first
        // visible output executes first (Sequence is the clearest case).
        for (auto it = outputs.rbegin(); it != outputs.rend(); ++it) state.push(*it);
    }

    std::vector<int> get_outputs(int node_id, ScriptGraph& graph, int output_pin = -1) {
        std::vector<int> result;
        std::vector<const ScriptGraphLink*> candidates;
        ScriptGraphNode* node = nullptr;
        for (auto& n : graph.nodes) if (n.id == node_id) { node = &n; break; }
        for (const auto& ln : graph.links) {
            if (ln.from_id != node_id || (output_pin >= 0 && ln.from_pin != output_pin)) continue;
            candidates.push_back(&ln);
        }
        // Pin IDs, not link insertion order, define executable branch and
        // sequence ordering. This preserves the editor's wire semantics.
        if (node && !node->out_pins.empty() && output_pin < 0) {
            bool has_pinned_links = false;
            for (const auto* link : candidates) if (link->from_pin != 0) { has_pinned_links = true; break; }
            for (const auto& port : node->out_pins) {
                if (port.type != "exec") continue;
                for (const auto* link : candidates) {
                    if (link->from_pin == port.id) result.push_back(link->to_id);
                }
            }
            if (has_pinned_links || !result.empty()) return result;
            // Pre-pin graph assets only stored node-to-node links. Preserve
            // their original behavior instead of treating them as disconnected.
        }
        for (const auto* link : candidates) result.push_back(link->to_id);
        return result;
    }

    std::vector<int> get_outputs_for_label(int node_id, ScriptGraph& graph, const std::string& label, int fallback_index) {
        for (const auto& node : graph.nodes) {
            if (node.id != node_id) continue;
            for (const auto& port : node.out_pins) {
                if (port.label == label) return get_outputs(node_id, graph, port.id);
            }
            break;
        }
        auto outputs = get_outputs(node_id, graph);
        if (fallback_index >= 0 && fallback_index < (int)outputs.size()) return {outputs[fallback_index]};
        return {};
    }

    // Simple input pins for a node (reverse links)
    std::vector<int> get_inputs(int node_id, ScriptGraph& graph) {
        std::vector<int> result;
        for (auto& ln : graph.links) {
            if (ln.to_id == node_id) result.push_back(ln.from_id);
        }
        return result;
    }

    Entity* find_entity(const std::string& name_or_id) {
        if (!_entities) return nullptr;
        if (name_or_id == "$self") return find_entity_by_id(_active_context.source_id);
        if (name_or_id == "$other") return find_entity_by_id(_active_context.other_id);
        // Try as ID first
        try {
            int id = std::stoi(name_or_id);
            for (auto& e : *_entities) {
                if (e.value("id", 0) == id) return &e;
            }
        } catch (...) {}
        // Try as name
        for (auto& e : *_entities) {
            if (e.value("name", "") == name_or_id) return &e;
        }
        return nullptr;
    }

    Entity* find_entity_by_id(int id) {
        if (!_entities || id == 0) return nullptr;
        for (auto& e : *_entities) if (e.value("id", 0) == id) return &e;
        return nullptr;
    }

    static float number_or(const std::string& text, float fallback) {
        if (text.empty()) return fallback;
        try { return std::stof(text); } catch (...) { return fallback; }
    }

    bool has_active_tweens(const std::string& graph_key) const {
        return std::any_of(_active_tweens.begin(), _active_tweens.end(), [&](const ActiveTween& tween) {
            return tween.graph_key == graph_key;
        });
    }

    bool evaluate_condition(const ScriptGraphNode& node, ScriptGraph& graph,
                            const std::string& graph_key) {
        const std::string resolved = graph_input(node, graph, graph_key, "Condition", node.p1);
        if (resolved.empty()) return true;
        // Simple comparison: try "value1 op value2" format
        std::string s = resolved;
        // Check for common truthy values
        if (s == "true" || s == "1" || s == "yes") return true;
        if (s == "false" || s == "0" || s == "no") return false;
        // Check if entity exists
        if (s.find("exists(") == 0) {
            std::string ename = s.substr(7, s.size() - 8);
            return find_entity(ename) != nullptr;
        }
        // Try numeric comparison
        try {
            float val = std::stof(s);
            float cmp = node.p2.empty() ? 0.f : std::stof(node.p2);
            if (node.p3 == ">") return val > cmp;
            if (node.p3 == "<") return val < cmp;
            if (node.p3 == ">=") return val >= cmp;
            if (node.p3 == "<=") return val <= cmp;
            if (node.p3 == "!=") return val != cmp;
            return std::abs(val - cmp) < 0.001f; // == by default
        } catch (...) {}
        return !s.empty();
    }

    void set_field_path(Entity& e, const std::string& path, const std::string& value) {
        // Parse a picker-produced "Components/Transform/x" path.  Do not
        // materialize missing keys here: a typo or an incompatible component
        // must not silently create JSON-only data that has no runtime effect.
        size_t pos = 0, end;
        runtime::Value* current = &e;
        while ((end = path.find('/', pos)) != std::string::npos) {
            std::string seg = path.substr(pos, end - pos);
            if (seg == "Components") seg = "components";
            if (current->is_object() && current->contains(seg)) {
                current = &(*current)[seg];
            } else { return; }
            pos = end + 1;
        }
        std::string field = path.substr(pos);
        if (field.empty() || !current->is_object() || !current->contains(field)) return;

        // Preserve the existing property's type.  Visual graph data pins are
        // represented as strings internally, so the destination component is
        // the authority for whether a value is bool/int/float/string.
        runtime::Value& destination = (*current)[field];
        try {
            if (destination.is_boolean()) {
                if (value == "true" || value == "1") destination = true;
                else if (value == "false" || value == "0") destination = false;
                return;
            }
            if (destination.is_number_integer()) {
                destination = std::stoll(value);
                return;
            }
            if (destination.is_number_float()) {
                destination = std::stod(value);
                return;
            }
            if (destination.is_string()) destination = value;
        } catch (...) {
            // Invalid typed text leaves the existing component value intact.
        }
    }

    std::string get_field_string(Entity& e, const std::string& path) {
        size_t pos = 0, end;
        runtime::Value* current = &e;
        while ((end = path.find('/', pos)) != std::string::npos) {
            std::string seg = path.substr(pos, end - pos);
            if (seg == "Components") seg = "components";
            if (current->is_object()) current = &(*current)[seg];
            else return "";
            pos = end + 1;
        }
        std::string field = path.substr(pos);
        if (current->is_object() && current->contains(field)) {
            auto& v = (*current)[field];
            if (v.is_number()) return std::to_string(v.get<double>());
            if (v.is_string()) return v.get<std::string>();
            if (v.is_bool()) return v.get<bool>() ? "true" : "false";
        }
        return "";
    }

    struct ActiveTween {
        std::string graph_key;
        int entity_id;
        std::vector<int> completion_outputs;
        GraphExecutionContext context;
        float sx, sy, tx, ty, duration, elapsed;
    };
    std::vector<ActiveTween> _active_tweens;

    std::unordered_map<std::string, bool> _flipflop_state;
    std::unordered_set<std::string> _once_flags;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> _graph_vars;
};

// ── Graph system integration into ScriptSystem ──
// Forward declarations
inline std::unordered_set<std::string>& script_graph_started_scenes();

inline std::string resolve_visual_script_asset(const std::string& asset_dir, const std::string& raw_asset) {
    namespace fs = std::filesystem;
    if (raw_asset.empty()) return {};
    fs::path raw(raw_asset);
    std::error_code ec;
    if (raw.is_absolute()) return fs::exists(raw, ec) ? raw.string() : std::string();
    const fs::path assets_root(asset_dir);
    const fs::path project_root = assets_root.parent_path();
    const fs::path direct = assets_root / raw;
    ec.clear();
    if (fs::exists(direct, ec)) return direct.string();
    const fs::path project_relative = project_root / raw;
    ec.clear();
    if (fs::exists(project_relative, ec)) return project_relative.string();
    return {};
}

// Call from ScriptSystem::update() to process graph events each frame
inline void script_graph_integration(EntityList& entities, const std::string& scene_name, const std::string& asset_dir, float dt) {
    auto& gs = ScriptGraphSystem::instance();
    gs.set_entity_list(&entities);

    // Load graph if not loaded
    gs.load_scene_graph(scene_name, asset_dir);

    // Check if graph actually has nodes before triggering events
    auto& graph = ScriptGraphSystem::instance().get_graph(scene_name);
    
    // Fire OnStart once
    auto& started_scenes = script_graph_started_scenes();
    if (started_scenes.find(scene_name) == started_scenes.end()) {
        started_scenes.insert(scene_name);
        gs.trigger("on_start", scene_name, nullptr);
    }

    // Only fire per-frame events if graph has matching nodes
    if (graph.has_type("on_update")) {
        gs.trigger("on_update", scene_name, nullptr);
    }

    // Check OnKey events - look for "on_key" nodes and check their p1 param for scancode
    // Use Input::GetKey to check key state
    gs.check_key_events(scene_name);

    // Snapshot contact events once for both scene and component graphs. Native
    // scripts may already have consumed `_pending_events`; in that case
    // ScriptSystem leaves `_pending_graph_events` as a one-frame mirror.
    // Pure visual-script entities still provide the original queue. Reading a
    // clone prevents a graph action from invalidating the event data midway
    // through dispatch.
    std::unordered_map<int, Entity> graph_events;
    for (const auto& e : entities) {
        const Entity* pending = nullptr;
        if (e.contains("_pending_graph_events") && e["_pending_graph_events"].is_array() &&
            !e["_pending_graph_events"].empty()) {
            pending = &e["_pending_graph_events"];
        } else if (e.contains("_pending_events") && e["_pending_events"].is_array() &&
                   !e["_pending_events"].empty()) {
            pending = &e["_pending_events"];
        }
        if (pending) graph_events.emplace(e.value("id", 0), pending->deep_clone());
    }

    // Fire scene-wide graph trigger/collision events. Preserve both
    // participants so tag filters and $self/$other work.
    for (auto& e : entities) {
        const auto pending_it = graph_events.find(e.value("id", 0));
        if (pending_it == graph_events.end()) continue;
        for (const auto& ev : pending_it->second) {
            std::string method = ev.value("method", "");
            if (method == "on_ui_click") {
                gs.trigger(method, scene_name, &e, nullptr, ev.value("action", std::string()));
            } else if (method.find("on_trigger_") == 0 || method.find("on_collision_") == 0) {
                Entity* other = nullptr;
                const int other_id = ev.value("other_id", 0);
                for (auto& candidate : entities) {
                    if (candidate.value("id", 0) == other_id) { other = &candidate; break; }
                }
                gs.trigger(method, scene_name, &e, other);
            }
        }
    }

    // Process pending nodes
    gs.update(dt, scene_name);

    // Component graphs are scoped to their owner and intentionally execute
    // after the scene graph. This lets scene graphs set up shared state first,
    // while each entity still has a private variable/delay context.
    for (auto& e : entities) {
        if (!e.contains("components") || !e["components"].contains("VisualScript")) continue;
        auto& component = e["components"]["VisualScript"];
        if (!component.is_object() || !component.value("enabled", true)) continue;
        const std::string asset = component.value("asset", std::string());
        const std::string path = resolve_visual_script_asset(asset_dir, asset);
        if (path.empty()) continue;
        const int entity_id = e.value("id", 0);
        const std::string graph_key = scene_name + "#entity:" + std::to_string(entity_id) + ":" + asset;
        gs.load_graph_asset(graph_key, path);
        const auto& component_graph = gs.get_graph(graph_key);
        auto& started = script_graph_started_scenes();
        if (component.value("run_on_start", true) && started.insert(graph_key).second)
            gs.trigger("on_start", graph_key, &e);
        if (component_graph.has_type("on_update")) gs.trigger("on_update", graph_key, &e);
        gs.check_key_events(graph_key, &e);
        const auto pending_it = graph_events.find(entity_id);
        if (pending_it != graph_events.end()) {
            for (const auto& ev : pending_it->second) {
                const std::string method = ev.value("method", std::string());
                if (method == "on_ui_click") {
                    gs.trigger(method, graph_key, &e, nullptr, ev.value("action", std::string()));
                    continue;
                }
                if (method.rfind("on_trigger_", 0) != 0 && method.rfind("on_collision_", 0) != 0) continue;
                Entity* other = nullptr;
                const int other_id = ev.value("other_id", 0);
                for (auto& candidate : entities) {
                    if (candidate.value("id", 0) == other_id) { other = &candidate; break; }
                }
                gs.trigger(method, graph_key, &e, other);
            }
        }
        gs.update(dt, graph_key);
    }
    // An event queue is a one-frame delivery mechanism. Without this clear,
    // entities using only VisualScript re-fired an Enter event every frame;
    // with native scripts the mirror would otherwise accumulate stale data.
    for (auto& e : entities) {
        if (!graph_events.count(e.value("id", 0))) continue;
        e["_pending_events"] = Entity::array();
        e.erase("_pending_graph_events");
    }
    gs.flush_pending_spawns();
}

// FixedUpdate counterpart to script_graph_integration().  Keeping this
// separate prevents graph fixed events from incorrectly running once per
// rendered frame. It intentionally processes only immediate graph work with
// dt=0: delays and tweens advance in the normal variable-rate integration,
// while the fixed event itself runs exactly once for every physics step.
inline void script_graph_fixed_integration(EntityList& entities, const std::string& scene_name,
                                           const std::string& asset_dir, float fixed_dt) {
    (void)fixed_dt; // event cadence is fixed; variable-time state advances in normal update.
    if (scene_name.empty()) return;
    auto& gs = ScriptGraphSystem::instance();
    gs.set_entity_list(&entities);
    gs.load_scene_graph(scene_name, asset_dir);

    const auto& graph = gs.get_graph(scene_name);
    if (graph.has_type("on_fixed_update")) {
        gs.trigger("on_fixed_update", scene_name, nullptr);
        gs.update(0.f, scene_name);
    }

    // Component graphs retain their owner as `$self`, exactly as they do in
    // variable Update. Load is idempotent, so this is only a map lookup after
    // the first regular graph tick.
    for (auto& e : entities) {
        if (!e.contains("components") || !e["components"].contains("VisualScript")) continue;
        auto& component = e["components"]["VisualScript"];
        if (!component.is_object() || !component.value("enabled", true)) continue;
        const std::string asset = component.value("asset", std::string());
        const std::string path = resolve_visual_script_asset(asset_dir, asset);
        if (path.empty()) continue;
        const int entity_id = e.value("id", 0);
        const std::string graph_key = scene_name + "#entity:" + std::to_string(entity_id) + ":" + asset;
        gs.load_graph_asset(graph_key, path);
        if (gs.get_graph(graph_key).has_type("on_fixed_update")) {
            gs.trigger("on_fixed_update", graph_key, &e);
            gs.update(0.f, graph_key);
        }
    }
    gs.flush_pending_spawns();
}

// Call when scene restarts
inline std::unordered_set<std::string>& script_graph_started_scenes() {
    static std::unordered_set<std::string> s;
    return s;
}

inline void script_graph_reset_scene(const std::string& scene_name) {
    auto& gs = ScriptGraphSystem::instance();
    gs.reload_scene_graph(scene_name, "");
    script_graph_started_scenes().erase(scene_name);
} 

// Explicit lifecycle entry point for Editor Play. Reloading here prevents
// variables, Do Once state, and delayed nodes from one Play session leaking
// into the next one while retaining the correct project asset directory.
inline void script_graph_prepare_scene(const std::string& scene_name, const std::string& asset_dir) {
    auto& gs = ScriptGraphSystem::instance();
    gs.reset_scene_context(scene_name);
    gs.load_scene_graph(scene_name, asset_dir);
    auto& started = script_graph_started_scenes();
    const std::string entity_prefix = scene_name + "#entity:";
    for (auto it = started.begin(); it != started.end();) {
        if (*it == scene_name || it->rfind(entity_prefix, 0) == 0) it = started.erase(it);
        else ++it;
    }
}

// Fire a trigger/collision event into the graph
inline void script_graph_entity_event(EntityList& entities, const std::string& scene_name, 
    const std::string& event_name, Entity* source, Entity* other) {
    auto& gs = ScriptGraphSystem::instance();
    gs.set_entity_list(&entities);
    gs.load_scene_graph(scene_name, "");
    gs.trigger(event_name, scene_name, source, other);
    gs.update(0.f, scene_name);
    gs.flush_pending_spawns();
}

// Call from collision/trigger callbacks
inline void script_graph_trigger_event(EntityList& entities, const std::string& scene_name, const std::string& event_name, Entity* source = nullptr, Entity* other = nullptr) {
    auto& gs = ScriptGraphSystem::instance();
    gs.set_entity_list(&entities);
    gs.load_scene_graph(scene_name, "");
    gs.trigger(event_name, scene_name, source, other);
}
