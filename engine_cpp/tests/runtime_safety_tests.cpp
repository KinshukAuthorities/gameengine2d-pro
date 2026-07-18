#include "script_system.hpp"
#include "file_watcher.hpp"
#include "script_graph_system.hpp"
#include "systems.hpp"

// SDL maps main to SDL_main on Windows. This is a console/unit-test target,
// not an SDL application, so retain the C++ runtime entry point.
#ifdef main
#undef main
#endif

#include <iostream>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace {

int failures = 0;
int multi_script_a_events = 0;
int multi_script_b_events = 0;
int burst_spawn_count = 0;

void expect(bool value, const char* description) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << description << "\n";
    }
}

class MultiScriptA final : public ScriptBase {
public:
    void on_trigger_enter(EntityRef) override { ++multi_script_a_events; }
};

class MultiScriptB final : public ScriptBase {
public:
    void on_trigger_enter(EntityRef) override { ++multi_script_b_events; }
};

// Mirrors a player/enemy action that creates many bullets or effects from one
// Update. It deliberately forces EntityList capacity growth while the script
// callback is active, the historical use-after-free crash shape.
class BurstSpawner final : public ScriptBase {
public:
    void update(float) override {
        if (_ran) return;
        _ran = true;
        Entity prototype = Entity::object();
        prototype["active"] = true;
        prototype["components"] = Entity::object();
        prototype["components"]["Transform"] = {
            {"x", 0.f}, {"y", 0.f}, {"rotation", 0.f},
            {"scale_x", 1.f}, {"scale_y", 1.f}
        };
        for (int i = 0; i < 600; ++i)
            if (instantiate(prototype, (float)i, 0.f)) ++burst_spawn_count;
    }
private:
    bool _ran = false;
};

ScriptBase* make_multi_script_a() { return new MultiScriptA(); }
ScriptBase* make_multi_script_b() { return new MultiScriptB(); }
ScriptBase* make_burst_spawner() { return new BurstSpawner(); }
void destroy_test_script(ScriptBase* script) { delete script; }

} // namespace

int main() {
    EventBus::State host_state;
    EventBus::bind_state(&host_state);
    EventBus& bus = EventBus::instance();
    Entity payload = Entity::object();

    int owner_a = 0;
    int owner_b = 0;
    int owner_a_calls = 0;
    int owner_b_calls = 0;

    // This mirrors the ScriptSystem callback scope used by real scripts:
    // legacy raw subscribe(...) calls acquire the active script owner.
    EventBus::set_subscription_owner(&owner_a);
    bus.subscribe("owned", [&](EntityRef, EntityRef) { ++owner_a_calls; });
    EventBus::set_subscription_owner(&owner_b);
    bus.subscribe("owned", [&](EntityRef, EntityRef) { ++owner_b_calls; });
    EventBus::set_subscription_owner(nullptr);

    bus.emit("owned", payload);
    expect(owner_a_calls == 1 && owner_b_calls == 1,
           "owner-tagged callbacks receive the shared event");
    bus.unsubscribe_all(&owner_a);
    bus.emit("owned", payload);
    expect(owner_a_calls == 1 && owner_b_calls == 2,
           "destroyed-owner callbacks are removed without clearing live owners");

    // Nested event delivery must preserve the currently executing owner's
    // registration context so a handler can safely subscribe another event.
    int nested_owner = 0;
    int nested_calls = 0;
    EventBus::set_subscription_owner(&nested_owner);
    bus.subscribe("outer", [&](EntityRef, EntityRef) {
        bus.subscribe("inner", [&](EntityRef, EntityRef) { ++nested_calls; });
    });
    EventBus::set_subscription_owner(nullptr);
    bus.emit("outer", payload);
    bus.emit("inner", payload);
    expect(nested_calls == 1, "nested subscription inherits the callback owner");
    bus.unsubscribe_all(&nested_owner);
    bus.emit("inner", payload);
    expect(nested_calls == 1, "nested callback is removed with its owner");

    // Mutating a registration list during dispatch must not invalidate the
    // iterator in use or run a just-added callback in the same emission.
    int mutation_owner = 0;
    int first_calls = 0;
    int late_calls = 0;
    bool added_late_handler = false;
    EventBus::set_subscription_owner(&mutation_owner);
    bus.subscribe("mutation", [&](EntityRef, EntityRef) {
        ++first_calls;
        if (!added_late_handler) {
            added_late_handler = true;
            bus.subscribe("mutation", [&](EntityRef, EntityRef) { ++late_calls; });
        }
    });
    EventBus::set_subscription_owner(nullptr);
    bus.emit("mutation", payload);
    expect(first_calls == 1 && late_calls == 0,
           "registration mutation is safe during event dispatch");
    bus.emit("mutation", payload);
    expect(first_calls == 2 && late_calls == 1,
           "new handler participates on the next emission");

    // Debug state uses the same host-bound model as EventBus. Hot-reload
    // script DLLs must contribute to the editor's visible diagnostics rather
    // than accumulating private per-module buffers.
    Debug::State host_debug_state;
    Debug::bind_state(&host_debug_state);
    int debug_logs = 0;
    Debug::set_log_callback([&](const std::string&, int) { ++debug_logs; });
    Debug::draw_line(0.f, 0.f, 1.f, 1.f, 255, 255, 255, 255, 1.f);
    Debug::log("host-bound debug test");
    expect(Debug::lines().size() == 1 && debug_logs == 1,
           "debug lines and logs use the supplied host state");
    Debug::clear_lines();
    Debug::draw_line(0.f, 0.f, 1.f, 1.f); // duration zero: one render frame
    Debug::update(1.f / 60.f);
    expect(Debug::lines().empty(),
           "short-lived debug primitives are retired instead of accumulating");

    // Runtime-created entities share one allocator with scene-authored IDs.
    // A newly loaded project may use low IDs, so generated entities must
    // always reserve past the highest loaded value before any spawn occurs.
    Entity authored_entity = Entity::object();
    authored_entity["id"] = 9600;
    EntityList authored_scene;
    authored_scene.push_back(std::move(authored_entity));
    reserve_entity_ids(authored_scene);
    expect(next_entity_id() > 9600,
           "runtime entity allocation never collides with a loaded scene ID");

    // Losing a window focus can swallow the matching key-up event. Input
    // state must be cleared so a background Play session cannot keep moving
    // or firing until the next keyboard event happens to arrive.
    InputSystem focus_input;
    focus_input.keys_down.insert(SDL_SCANCODE_D);
    focus_input.mouse_buttons[1] = true;
    focus_input.axes["Horizontal"]._value = 1.f;
    focus_input.axis_values["Horizontal"] = 1.f;
    SDL_Event focus_lost{};
    focus_lost.type = SDL_WINDOWEVENT;
    focus_lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    focus_input.process_event(focus_lost);
    expect(focus_input.keys_down.empty() && !focus_input.mouse_buttons[1] &&
           focus_input.get_axis("Horizontal") == 0.f,
           "focus loss clears held input instead of leaving gameplay controls latched");

    // Every Script component on one entity must receive the same collision
    // event, independent of unordered-map iteration order. The graph mirror
    // is produced from that same snapshot rather than being consumed by the
    // first native script.
    ScriptRegistry::instance().reg("test_multi_a", {make_multi_script_a, destroy_test_script});
    ScriptRegistry::instance().reg("test_multi_b", {make_multi_script_b, destroy_test_script});
    Entity multi_script_entity = Entity::object();
    multi_script_entity["id"] = 9101;
    multi_script_entity["active"] = true;
    multi_script_entity["components"] = Entity::object();
    multi_script_entity["components"]["ScriptComponent"]["scripts"] = Entity::array();
    multi_script_entity["components"]["ScriptComponent"]["scripts"].push_back("test_multi_a");
    multi_script_entity["components"]["ScriptComponent"]["scripts"].push_back("test_multi_b");
    multi_script_entity["_pending_events"] = Entity::array();
    multi_script_entity["_pending_events"].push_back(
        {{"method", "on_trigger_enter"}, {"other_id", 0}});
    EntityList multi_script_entities;
    multi_script_entities.push_back(std::move(multi_script_entity));
    ScriptSystem multi_script_system;
    multi_script_system.update(multi_script_entities, 1.f / 60.f);
    expect(multi_script_a_events == 1 && multi_script_b_events == 1 &&
           multi_script_entities[0]["_pending_events"].empty() &&
           multi_script_entities[0]["_pending_graph_events"].size() == 1,
           "all native scripts and VisualScript receive one shared trigger event");
    multi_script_system.reset_all_instances();
    ScriptRegistry::instance().unreg("test_multi_a");
    ScriptRegistry::instance().unreg("test_multi_b");

    // A single action can instantiate a burst of projectiles/effects. The
    // script runtime must rebind pointers after EntityList growth and assign
    // unique IDs to every clone instead of crashing or corrupting its map.
    ScriptRegistry::instance().reg("test_burst_spawner", {make_burst_spawner, destroy_test_script});
    Entity burst_source = Entity::object();
    burst_source["id"] = 9700;
    burst_source["active"] = true;
    burst_source["components"] = Entity::object();
    burst_source["components"]["ScriptComponent"]["scripts"] = Entity::array();
    burst_source["components"]["ScriptComponent"]["scripts"].push_back("test_burst_spawner");
    EntityList burst_entities;
    burst_entities.push_back(std::move(burst_source));
    ScriptSystem burst_system;
    burst_system.update(burst_entities, 1.f / 60.f);
    std::unordered_set<int> burst_ids;
    for (const auto& entity : burst_entities) burst_ids.insert(entity.value("id", 0));
    expect(burst_spawn_count == 600 && burst_entities.size() == 601 &&
           burst_ids.size() == burst_entities.size(),
           "high-volume script spawning preserves valid pointers and unique entity IDs");
    burst_system.reset_all_instances();
    ScriptRegistry::instance().unreg("test_burst_spawner");

    // Physics owns a few process-wide caches for interpolation/contact state.
    // Temporary gameplay objects must not leave those caches growing after the
    // objects are removed (the failure mode behind long-session freezes).
    phys::reset_contact_state();
    EntityList transient_entities;
    for (int id = 9201; id <= 9202; ++id) {
        Entity e = Entity::object();
        e["id"] = id;
        e["active"] = true;
        e["components"] = Entity::object();
        e["components"]["Transform"] = {
            {"x", (float)(id - 9201) * 32.f}, {"y", 0.f},
            {"rotation", 0.f}, {"scale_x", 1.f}, {"scale_y", 1.f}
        };
        transient_entities.push_back(std::move(e));
    }
    transform::TransformSystem cache_transform_system;
    cache_transform_system.update(transient_entities);
    phys::ignore_collision(9201, 9202, true);
    phys::apply_physics(transient_entities, 1.f / 60.f);
    auto live_cache_stats = phys::get_runtime_cache_stats();
    expect(live_cache_stats.interpolation_records == 2 &&
           live_cache_stats.ignored_entity_pairs == 1,
           "physics runtime caches record live temporary entities only");
    transient_entities.clear();
    phys::apply_physics(transient_entities, 1.f / 60.f);
    auto cleared_cache_stats = phys::get_runtime_cache_stats();
    expect(cleared_cache_stats.interpolation_records == 0 &&
           cleared_cache_stats.active_contact_pairs == 0 &&
           cleared_cache_stats.contact_id_records == 0 &&
           cleared_cache_stats.ignored_entity_pairs == 0 &&
           cleared_cache_stats.ignored_collider_pairs == 0,
           "physics cache state is pruned after temporary entities disappear");

    // A stalled consumer or a dense collider overlap must not make the
    // physics callback queue unbounded. Existing queued events are preserved
    // and excess new contacts are counted for diagnostics.
    phys::reset_contact_state();
    auto make_contact_body = [](int id, const char* type) {
        Entity e = Entity::object();
        e["id"] = id;
        e["active"] = true;
        e["components"] = Entity::object();
        e["components"]["Transform"] = {
            {"x", 0.f}, {"y", 0.f}, {"rotation", 0.f},
            {"scale_x", 1.f}, {"scale_y", 1.f}
        };
        e["components"]["Rigidbody2D"] = {
            {"body_type", type}, {"mass", 1.f}, {"gravity_scale", 0.f}
        };
        e["components"]["BoxCollider2D"] = {
            {"width", 20.f}, {"height", 20.f}
        };
        return e;
    };
    EntityList queue_entities;
    queue_entities.push_back(make_contact_body(9301, "dynamic"));
    queue_entities.push_back(make_contact_body(9302, "static"));
    queue_entities[0]["_pending_events"] = Entity::array();
    for (int i = 0; i < 256; ++i)
        queue_entities[0]["_pending_events"].push_back({{"method", "test"}, {"other_id", 0}});
    cache_transform_system.update(queue_entities);
    phys::apply_physics(queue_entities, 1.f / 60.f);
    expect(queue_entities[0]["_pending_events"].size() == 256 &&
           queue_entities[0].value("_dropped_physics_events", 0) > 0,
           "physics contact queue is bounded and records dropped excess events");
    phys::reset_contact_state();

    // Particle data is runtime-editable and effect helpers can request
    // sub-emitter bursts.  Neither a malformed persisted array nor one
    // particle's sub-burst may bypass the emitter's CPU particle budget.
    Entity particle_entity = Entity::object();
    particle_entity["id"] = 9401;
    particle_entity["active"] = true;
    particle_entity["components"] = Entity::object();
    particle_entity["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}};
    particle_entity["components"]["ParticleEmitter"] = {
        {"emitting", false}, {"max_particles", 4},
        {"sub_emitter_on_death", true}, {"sub_emitter_count", 64}
    };
    particle_entity["_particles"] = Entity::array();
    particle_entity["_particles"].push_back(
        {{"age", 1.f}, {"lifetime", 0.1f}, {"x", 0.f}, {"y", 0.f}});
    for (int i = 0; i < 64; ++i) {
        particle_entity["_particles"].push_back(
            {{"age", 0.f}, {"lifetime", 30.f}, {"x", (float)i}, {"y", 0.f}});
    }
    EntityList particle_entities;
    particle_entities.push_back(std::move(particle_entity));
    ParticleSystem particle_system;
    particle_system.update(particle_entities, 1.f / 60.f);
    expect(particle_entities[0]["_particles"].size() <= 4,
           "particle and sub-emitter data never exceeds the configured CPU budget");

    // Grid2D values must affect Tilemap runtime geometry, not merely the
    // Inspector.  A parent grid resolves its cell dimensions/gaps onto the
    // child map and the generated collider uses those resolved dimensions.
    Entity grid_root = Entity::object();
    grid_root["id"] = 9450;
    grid_root["active"] = true;
    grid_root["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}, {"parent", -1}};
    grid_root["components"]["Grid2D"] = {{"cell_width", 48.f}, {"cell_height", 24.f},
                                          {"cell_gap_x", 2.f}, {"cell_gap_y", 3.f}};
    Entity grid_tilemap = Entity::object();
    grid_tilemap["id"] = 9451;
    grid_tilemap["active"] = true;
    grid_tilemap["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}, {"parent", 9450}};
    grid_tilemap["components"]["Tilemap"] = {
        {"tile_size", 32}, {"generate_colliders", true},
        {"grid", nlohmann::json::array({nlohmann::json::array({0})})}
    };
    EntityList grid_entities;
    grid_entities.push_back(grid_root);
    grid_entities.push_back(grid_tilemap);
    Grid2DSystem grid_system;
    TilemapColliderSystem grid_collider_system;
    grid_system.update(grid_entities);
    grid_collider_system.update(grid_entities);
    const auto& resolved_map = grid_entities[1]["components"]["Tilemap"];
    expect(std::abs(resolved_map.value("_grid_cell_width", 0.f) - 48.f) < 0.001f &&
           std::abs(resolved_map.value("_grid_cell_height", 0.f) - 24.f) < 0.001f &&
           grid_entities[1]["_tilemap_colliders"].size() == 1 &&
           std::abs(grid_entities[1]["_tilemap_colliders"][0].value("width", 0.f) - 48.f) < 0.001f &&
           std::abs(grid_entities[1]["_tilemap_colliders"][0].value("height", 0.f) - 24.f) < 0.001f,
           "Grid2D cell size is honoured by Tilemap collision geometry");

    // The public navigation component name must be the one the runtime uses.
    // A carved NavMeshObstacle2D must also be applied to the live grid before
    // pathing, rather than existing only as an Inspector data object.
    Entity nav_map = Entity::object();
    nav_map["id"] = 9460;
    nav_map["active"] = true;
    nav_map["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}};
    nav_map["components"]["Tilemap"] = {
        {"tile_size", 32}, {"origin_x", 0}, {"origin_y", 0},
        {"grid", nlohmann::json::array({nlohmann::json::array({0, 0, 0})})}
    };
    nav_map["components"]["NavMesh2D"] = {{"obstacle_ids", nlohmann::json::array({1})}};
    nav_map["_nav_dirty"] = true;
    Entity nav_agent = Entity::object();
    nav_agent["id"] = 9461;
    nav_agent["active"] = true;
    nav_agent["components"]["Transform"] = {{"x", 16.f}, {"y", 16.f}};
    nav_agent["components"]["NavMeshAgent2D"] = {
        {"speed", 100.f}, {"stopping_distance", 1.f},
        {"destination_x", 80.f}, {"destination_y", 16.f}, {"auto_repath", true}
    };
    EntityList nav_entities;
    nav_entities.push_back(nav_map);
    nav_entities.push_back(nav_agent);
    transform::TransformSystem nav_transform_system;
    NavMesh2DSystem nav_system;
    nav_transform_system.update(nav_entities);
    nav_system.update(nav_entities, 0.01f);
    expect(nav_entities[1]["components"]["NavMeshAgent2D"]["_path_waypoints"].is_array() &&
           !nav_entities[1]["components"]["NavMeshAgent2D"]["_path_waypoints"].empty(),
           "NavMeshAgent2D uses the public component name and finds a path");
    Entity nav_obstacle = Entity::object();
    nav_obstacle["id"] = 9462;
    nav_obstacle["active"] = true;
    nav_obstacle["components"]["Transform"] = {{"x", 48.f}, {"y", 16.f}};
    nav_obstacle["components"]["NavMeshObstacle2D"] = {
        {"width", 32.f}, {"height", 32.f}, {"carve", true}
    };
    nav_entities.push_back(nav_obstacle);
    nav_entities[1]["components"]["Transform"]["x"] = 16.f;
    nav_entities[1]["components"]["Transform"]["y"] = 16.f;
    nav_entities[1]["components"]["NavMeshAgent2D"]["_path_requested"] = true;
    nav_transform_system.update(nav_entities);
    nav_system.update(nav_entities, 0.01f);
    expect(nav_entities[1]["components"]["NavMeshAgent2D"]["_path_waypoints"].empty(),
           "NavMeshObstacle2D carves an impassable cell in the live navigation grid");

    // PathFollower2D is an addable production component: its stored points
    // must advance the normal Transform.x/y fields used by rendering, not a
    // dead pair of position_x/position_y properties.
    Entity follower = Entity::object();
    follower["id"] = 9463;
    follower["active"] = true;
    follower["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}, {"rotation", 0.f}};
    follower["components"]["PathFollower2D"] = {
        {"speed", 100.f}, {"loop", false}, {"ping_pong", false},
        {"look_at_direction", false},
        {"waypoints", nlohmann::json::array({{{"x", 0.f}, {"y", 0.f}}, {{"x", 100.f}, {"y", 0.f}}})}
    };
    EntityList follower_entities;
    follower_entities.push_back(follower);
    FollowerSystem follower_system;
    follower_system.update(follower_entities, 0.5f);
    expect(std::abs(follower_entities[0]["components"]["Transform"].value("x", 0.f) - 50.f) < 0.001f &&
           std::abs(follower_entities[0]["components"]["Transform"].value("y", 99.f)) < 0.001f,
           "PathFollower2D advances the rendered Transform along its authored path");

    // These authoring components have dedicated feature systems. Exercise
    // them here so their inspector fields cannot silently regress into JSON
    // that looks editable but has no standalone runtime effect.
    Entity animated_tiles = Entity::object();
    animated_tiles["id"] = 9464;
    animated_tiles["active"] = true;
    animated_tiles["components"]["Tilemap"] = {
        {"grid", nlohmann::json::array({nlohmann::json::array({1})})},
        {"animated_tiles", {{"0,0", {{"frames", nlohmann::json::array({1, 9})}, {"fps", 10.f}, {"_t", 0.f}}}}}
    };
    EntityList animated_tile_entities{animated_tiles};
    TilemapAnimationSystem tile_animation_system;
    tile_animation_system.update(animated_tile_entities, 0.11f);
    expect(animated_tile_entities[0]["components"]["Tilemap"]["grid"][0][0].get<int>() == 9,
           "Tilemap animated-tile settings update the exported tile grid");

    Entity trail = Entity::object();
    trail["id"] = 94645;
    trail["active"] = true;
    trail["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}};
    trail["components"]["TrailRenderer2D"] = {
        {"time", 0.2f}, {"min_vertex_distance", 2.f},
        {"width_start", 8.f}, {"width_end", 0.f}, {"emitting", true}
    };
    EntityList trail_entities{trail};
    transform::TransformSystem trail_transform_system;
    TrailRenderer2DSystem trail_system;
    trail_transform_system.update(trail_entities);
    trail_system.update(trail_entities, 0.01f);
    trail_entities[0]["components"]["Transform"]["x"] = 10.f;
    trail_transform_system.update(trail_entities);
    trail_system.update(trail_entities, 0.01f);
    const auto& live_trail = trail_entities[0]["components"]["TrailRenderer2D"]["_trail_points"];
    expect(live_trail.size() == 2 && live_trail[0].value("width", 0.f) > live_trail[1].value("width", 99.f),
           "TrailRenderer2D records movement and tapers live points in the standalone runtime");
    trail_system.update(trail_entities, 0.25f);
    expect(trail_entities[0]["components"]["TrailRenderer2D"]["_trail_points"].empty(),
           "TrailRenderer2D expires old points without retaining unbounded trail data");

    Entity render_texture = Entity::object();
    render_texture["id"] = 946451;
    render_texture["active"] = true;
    render_texture["components"]["CustomRenderTexture2D"] = {
        {"enabled", true}, {"update_mode", "on_demand"}, {"request_update", false}
    };
    EntityList render_texture_entities{render_texture};
    CustomRenderTexture2DSystem render_texture_system;
    render_texture_system.update(render_texture_entities, 0.01f);
    const int initial_texture_revision = render_texture_entities[0]["components"]["CustomRenderTexture2D"].value("_runtime_revision", 0);
    render_texture_entities[0]["components"]["CustomRenderTexture2D"]["request_update"] = true;
    render_texture_system.update(render_texture_entities, 0.01f);
    const int requested_texture_revision = render_texture_entities[0]["components"]["CustomRenderTexture2D"].value("_runtime_revision", 0);
    render_texture_entities[0]["components"]["CustomRenderTexture2D"]["update_mode"] = "realtime";
    render_texture_entities[0]["components"]["CustomRenderTexture2D"]["update_interval"] = 0.02f;
    render_texture_system.update(render_texture_entities, 0.03f);
    expect(initial_texture_revision >= 1 && requested_texture_revision == initial_texture_revision + 1 &&
           render_texture_entities[0]["components"]["CustomRenderTexture2D"].value("_runtime_revision", 0) == requested_texture_revision + 1,
           "CustomRenderTexture2D honors on-demand and realtime update revisions in the standalone runtime");

    Entity video = Entity::object();
    video["active"] = true;
    video["components"]["VideoPlayer2D"] = {
        {"clip", "intro.gif"}, {"play_on_awake", true}, {"playing", false}, {"playback_speed", 2.f}
    };
    EntityList video_entities{video};
    VideoPlayer2DSystem video_system;
    video_system.update(video_entities, 0.25f);
    const float video_time = video_entities[0]["components"]["VideoPlayer2D"].value("playback_time", 0.f);
    video_entities[0]["components"]["VideoPlayer2D"]["restart"] = true;
    video_system.update(video_entities, 0.f);
    expect(std::abs(video_time - 0.5f) < 0.001f &&
           video_entities[0]["components"]["VideoPlayer2D"].value("playing", false) &&
           std::abs(video_entities[0]["components"]["VideoPlayer2D"].value("playback_time", 1.f)) < 0.001f,
           "VideoPlayer2D maintains play, speed and restart state for GIF frame playback");

    int health_damage_events = 0;
    int health_death_events = 0;
    int health_event_owner = 0;
    EventBus::instance().subscribe("unit_health_damage", &health_event_owner,
        [&](EntityRef, EntityRef) { ++health_damage_events; });
    EventBus::instance().subscribe("unit_health_death", &health_event_owner,
        [&](EntityRef, EntityRef) { ++health_death_events; });
    Entity health_entity = Entity::object();
    health_entity["id"] = 94646;
    health_entity["active"] = true;
    health_entity["components"]["HealthComponent"] = {
        {"max_health", 100.f}, {"current_health", 100.f}, {"invincibility_time", 0.f},
        {"auto_destroy_on_death", true}, {"on_damage_event", "unit_health_damage"},
        {"on_death_event", "unit_health_death"}
    };
    EntityList health_entities{health_entity};
    HealthSystem health_system;
    health_system.update(health_entities, 0.01f);
    health_entities[0]["components"]["HealthComponent"]["current_health"] = 65.f;
    health_system.update(health_entities, 0.01f);
    health_entities[0]["components"]["HealthComponent"]["current_health"] = -20.f;
    health_system.update(health_entities, 0.01f);
    EventBus::instance().unsubscribe_all(&health_event_owner);
    expect(health_damage_events == 2 && health_death_events == 1 && !health_entities[0].value("active", true),
           "HealthComponent emits authored damage/death events and destroys exactly once when configured");

    const std::filesystem::path sobj_dir = std::filesystem::temp_directory_path() /
        ("gameengine_sobj_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code sobj_ec;
    std::filesystem::create_directories(sobj_dir, sobj_ec);
    { std::ofstream sobj_file(sobj_dir / "enemy.sobj"); sobj_file << R"({"type":"EnemyStats","damage":15,"name":"Crawler"})"; }
    Entity sobj_entity = Entity::object();
    sobj_entity["active"] = true;
    sobj_entity["components"]["ScriptableObjectRef"] = {{"asset_path", "enemy.sobj"}};
    EntityList sobj_entities{sobj_entity};
    ScriptableObjectSystem sobj_system;
    sobj_system.set_asset_dir(sobj_dir.string());
    sobj_system.update(sobj_entities);
    expect(sobj_entities[0]["components"]["ScriptableObjectRef"]["data"].value("damage", 0) == 15 &&
           sobj_entities[0]["components"]["ScriptableObjectRef"].value("type_name", std::string()) == "EnemyStats",
           "ScriptableObjectRef loads project-relative authored data into a runtime component copy");
    std::filesystem::remove_all(sobj_dir, sobj_ec);

    const std::filesystem::path spawner_dir = std::filesystem::temp_directory_path() /
        ("gameengine_spawner_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code spawner_ec;
    std::filesystem::create_directories(spawner_dir, spawner_ec);
    { std::ofstream prefab_file(spawner_dir / "bolt.prefab");
      prefab_file << R"({"root":{"name":"Bolt","components":{"Transform":{"x":2,"y":3},"SpriteRenderer":{"texture":"bolt.png"}}}})"; }
    Entity spawner_entity = Entity::object();
    spawner_entity["id"] = 94647;
    spawner_entity["active"] = true;
    spawner_entity["components"]["Transform"] = {{"x", 10.f}, {"y", 20.f}};
    spawner_entity["components"]["Spawner2D"] = {
        {"prefab", "bolt.prefab"}, {"spawn_on_start", true}, {"interval", 60.f},
        {"max_count", 1}, {"offset_x", 5.f}, {"offset_y", -2.f}
    };
    EntityList spawner_entities{spawner_entity};
    transform::TransformSystem spawner_transform_system;
    spawner_transform_system.update(spawner_entities);
    Spawner2DSystem spawner_system;
    spawner_system.set_asset_dir(spawner_dir.string());
    spawner_system.update(spawner_entities, 0.01f);
    expect(spawner_entities.size() == 2 && spawner_entities[1].value("_spawner_owner", -1) == 94647 &&
           std::abs(spawner_entities[1]["components"]["Transform"].value("x", 0.f) - 15.f) < 0.001f,
           "Spawner2D creates a project-relative prefab instance at its authored offset");
    std::filesystem::remove_all(spawner_dir, spawner_ec);

    Entity override_animator = Entity::object();
    override_animator["active"] = true;
    override_animator["components"]["SpriteRenderer"] = {{"texture", "base.png"}};
    override_animator["components"]["Animator"] = {
        {"playing", true}, {"current_animation", "walk"}, {"default_fps", 1.f},
        {"animations", {{"walk", nlohmann::json::array({"walk.png"})},
                        {"armored_walk", nlohmann::json::array({"armored.png"})}}}
    };
    override_animator["components"]["AnimatorOverrideController"] = {
        {"overrides", {{"walk", "armored_walk"}}}
    };
    EntityList override_entities{override_animator};
    AnimatorSystem override_system;
    override_system.update(override_entities, 0.01f);
    expect(override_entities[0]["components"]["SpriteRenderer"].value("texture", std::string()) == "armored.png",
           "AnimatorOverrideController resolves the authored replacement clip at runtime");

    Entity ik_root = Entity::object();
    ik_root["id"] = 94650; ik_root["active"] = true;
    ik_root["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}, {"rotation", 0.f}};
    ik_root["components"]["LimbIK2D"] = {{"root_entity", 94650}, {"mid_entity", 94651}, {"end_entity", 94652},
                                            {"target_entity", 94653}, {"weight", 1.f}, {"bend_direction", 1}};
    Entity ik_middle = Entity::object();
    ik_middle["id"] = 94651; ik_middle["active"] = true;
    ik_middle["components"]["Transform"] = {{"x", 5.f}, {"y", 0.f}, {"parent", 94650}};
    Entity ik_end = Entity::object();
    ik_end["id"] = 94652; ik_end["active"] = true;
    ik_end["components"]["Transform"] = {{"x", 5.f}, {"y", 0.f}, {"parent", 94651}};
    Entity ik_target = Entity::object();
    ik_target["id"] = 94653; ik_target["active"] = true;
    ik_target["components"]["Transform"] = {{"x", 5.f}, {"y", 5.f}};
    EntityList ik_entities{ik_root, ik_middle, ik_end, ik_target};
    transform::TransformSystem ik_transform_system;
    ik_transform_system.update(ik_entities);
    LimbIK2DSystem ik_system;
    ik_system.update(ik_entities);
    ik_transform_system.update(ik_entities);
    const auto ik_end_world = transform::cached_world(ik_entities[2]);
    expect(std::hypot(ik_end_world.x - 5.f, ik_end_world.y - 5.f) < 0.05f,
           "LimbIK2D solves a parented two-bone chain to its selected target");

    Entity pooled = Entity::object();
    pooled["id"] = 9465;
    pooled["active"] = false;
    pooled["components"]["ObjectPool"] = {{"pool_key", "bullets"}, {"pool_lifetime", 0.1f}};
    pooled["components"]["Rigidbody2D"] = {{"velocity_x", 44.f}, {"velocity_y", -7.f}};
    EntityList pool_entities{pooled};
    ObjectPool2DSystem pool_system;
    pool_system.init(pool_entities);
    const int pooled_id = pool_system.get("bullets", pool_entities);
    pool_system.update(pool_entities, 0.11f);
    expect(pooled_id == 9465 && !pool_entities[0].value("active", true) &&
           std::abs(pool_entities[0]["components"]["Rigidbody2D"].value("velocity_x", 1.f)) < 0.001f,
           "ObjectPool activates and automatically returns pooled rigidbodies");

    Entity flock_agent = Entity::object();
    flock_agent["id"] = 9466;
    flock_agent["active"] = true;
    flock_agent["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}};
    flock_agent["components"]["Rigidbody2D"] = {{"velocity_x", 0.f}, {"velocity_y", 0.f}};
    flock_agent["components"]["Flock2D"] = {{"flock_id", "test"}, {"seek_enabled", true},
        {"seek_x", 100.f}, {"seek_y", 0.f}, {"seek_weight", 1.f},
        {"max_speed", 120.f}, {"max_force", 60.f}};
    EntityList flock_entities{flock_agent};
    transform::TransformSystem flock_transform_system;
    flock_transform_system.update(flock_entities);
    FlockingSystem flock_system;
    flock_system.update(flock_entities, 0.25f);
    expect(flock_entities[0]["components"]["Rigidbody2D"].value("velocity_x", 0.f) > 0.f,
           "Flock2D steering fields drive Rigidbody2D velocity");

    Entity lod_camera = Entity::object();
    lod_camera["active"] = true;
    lod_camera["components"]["Camera2D"] = {{"is_main", true}, {"orthographic_size", 5.f}, {"screen_height", 600.f}};
    Entity lod_sprite = Entity::object();
    lod_sprite["active"] = true;
    lod_sprite["components"]["SpriteRenderer"] = {{"texture", "far.png"}};
    lod_sprite["components"]["LODGroup2D"] = {{"reference_height_units", 10.f},
        {"levels", nlohmann::json::array({{{"screen_threshold", 1.1f}, {"texture", "near.png"}}})}};
    EntityList lod_entities{lod_camera, lod_sprite};
    LODGroupSystem lod_system;
    lod_system.update(lod_entities, 0.016f);
    expect(lod_entities[1]["components"]["SpriteRenderer"].value("texture", std::string()) == "near.png",
           "LODGroup2D selects the authored runtime sprite level");

    Entity virtual_brain = Entity::object();
    virtual_brain["id"] = 94670; virtual_brain["active"] = true;
    virtual_brain["components"]["Camera2D"] = {{"is_brain", true}, {"orthographic_size", 5.f}, {"offset_x", 0.f}, {"offset_y", 0.f}};
    Entity virtual_target = Entity::object();
    virtual_target["id"] = 94671; virtual_target["active"] = true;
    virtual_target["components"]["Transform"] = {{"x", 20.f}, {"y", -8.f}};
    Entity virtual_camera = Entity::object();
    virtual_camera["id"] = 94672; virtual_camera["active"] = true;
    virtual_camera["components"]["VirtualCamera"] = {{"enabled", true}, {"priority", 10}, {"follow_target", 94671},
                                                         {"dead_zone_w", 0.f}, {"dead_zone_h", 0.f}, {"soft_zone_w", 0.f}, {"soft_zone_h", 0.f},
                                                         {"x_damp", 0.f}, {"y_damp", 0.f}, {"ortho_size", 3.f}};
    EntityList virtual_camera_entities{virtual_brain, virtual_target, virtual_camera};
    transform::TransformSystem virtual_camera_transforms;
    virtual_camera_transforms.update(virtual_camera_entities);
    VirtualCameraSystem virtual_camera_system;
    virtual_camera_system.update(virtual_camera_entities, 0.016f);
    expect(std::abs(virtual_camera_entities[0]["components"]["Camera2D"].value("offset_x", 0.f) - 20.f) < 0.001f &&
           std::abs(virtual_camera_entities[0]["components"]["Camera2D"].value("orthographic_size", 0.f) - 3.f) < 0.001f,
           "VirtualCamera drives the Camera2D brain using its follow and lens settings");

    Entity transition_entity = Entity::object();
    transition_entity["active"] = true;
    transition_entity["components"]["SceneTransition"] = {
        {"active", true}, {"target_scene", "next_scene.json"}, {"duration", 0.1f}, {"_alpha", 0.f}, {"_phase", "fadeout"}
    };
    int transition_midpoints = 0;
    int transition_owner = 0;
    EventBus::instance().subscribe("OnTransitionMidpoint", &transition_owner,
        [&](EntityRef event, EntityRef) {
            if (event && event.value("target_scene", std::string()) == "next_scene.json") ++transition_midpoints;
        });
    EntityList transition_entities{transition_entity};
    SceneTransitionSystem transition_system;
    transition_system.update(transition_entities, 0.1f);
    EventBus::instance().unsubscribe_all(&transition_owner);
    expect(transition_midpoints == 1 &&
           std::abs(transition_entities[0]["components"]["SceneTransition"].value("_alpha", 0.f) - 1.f) < 0.001f,
           "SceneTransition reaches its midpoint and emits the target-scene event");

    // The file watcher must invoke callbacks outside its scan mutex. This
    // exercises the real hot-reload shape: a callback may stop the watcher
    // as part of shutdown without deadlocking itself or the UI thread.
    namespace fs = std::filesystem;
    const fs::path watch_dir = fs::temp_directory_path() /
        ("gameengine_filewatch_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code watch_ec;
    fs::create_directories(watch_dir, watch_ec);
    std::atomic<int> watch_callbacks{0};
    FileWatcher watcher(std::chrono::milliseconds(10));
    watcher.watch(watch_dir, false, ".cpp");
    watcher.set_callback([&](const std::vector<FileWatchEvent>& events) {
        if (!events.empty()) ++watch_callbacks;
        watcher.stop();
    });
    watcher.start();
    { std::ofstream source(watch_dir / "new_script.cpp"); source << "// watcher test\n"; }
    for (int i = 0; i < 100 && watch_callbacks.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(watch_callbacks.load() == 1 && !watcher.is_running(),
           "file watcher callbacks can safely stop their watcher");
    watcher.stop();
    fs::remove_all(watch_dir, watch_ec);

    // Graph Fixed Update must follow the physics cadence, not the render
    // cadence. A regular graph tick should not execute this node; one fixed
    // integration tick should execute it exactly once.
    const fs::path graph_dir = fs::temp_directory_path() /
        ("gameengine_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(graph_dir, watch_ec);
    const std::string graph_scene = "fixed_graph_test";
    nlohmann::json fixed_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_fixed_update"}, {"label", "On Fixed Update"}},
        {{"id", 2}, {"type", "log"}, {"label", "Log"}, {"p1", "fixed graph test"}},
        {{"_meta", true}, {"links", nlohmann::json::array({{{"fn", 1}, {"fp", 0}, {"tn", 2}, {"tp", 0}}})}}
    });
    { std::ofstream graph_file(graph_dir / ("event_graph_" + graph_scene + ".json")); graph_file << fixed_graph.dump(); }
    int fixed_graph_logs = 0;
    Debug::set_log_callback([&](const std::string& message, int) {
        if (message.find("fixed graph test") != std::string::npos) ++fixed_graph_logs;
    });
    EntityList graph_entities;
    script_graph_prepare_scene(graph_scene, graph_dir.string());
    script_graph_integration(graph_entities, graph_scene, graph_dir.string(), 1.f / 30.f);
    expect(fixed_graph_logs == 0, "visual graph Fixed Update does not run on a render tick");
    script_graph_fixed_integration(graph_entities, graph_scene, graph_dir.string(), 1.f / 60.f);
    expect(fixed_graph_logs == 1, "visual graph Fixed Update runs on a physics tick");
    ScriptGraphSystem::instance().reset_scene_context(graph_scene);
    script_graph_started_scenes().erase(graph_scene);
    fs::remove_all(graph_dir, watch_ec);

    // Physics queues Unity-style `*_enter` method names. The graph palette
    // uses on_trigger/on_collision, so verify the runtime normalization and
    // one-frame event consumption used by pure VisualScript entities.
    const fs::path trigger_dir = fs::temp_directory_path() /
        ("gameengine_trigger_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(trigger_dir, watch_ec);
    const std::string trigger_scene = "trigger_graph_test";
    nlohmann::json trigger_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_trigger"}, {"label", "On Trigger Enter"}},
        {{"id", 2}, {"type", "log"}, {"label", "Log"}, {"p1", "trigger graph test"}},
        {{"_meta", true}, {"links", nlohmann::json::array({{{"fn", 1}, {"fp", 0}, {"tn", 2}, {"tp", 0}}})}}
    });
    { std::ofstream graph_file(trigger_dir / ("event_graph_" + trigger_scene + ".json")); graph_file << trigger_graph.dump(); }
    int trigger_graph_logs = 0;
    Debug::set_log_callback([&](const std::string& message, int) {
        if (message.find("trigger graph test") != std::string::npos) ++trigger_graph_logs;
    });
    Entity trigger_source = Entity::object();
    trigger_source["id"] = 7001;
    trigger_source["active"] = true;
    trigger_source["_pending_events"] = Entity::array();
    trigger_source["_pending_events"].push_back({{"method", "on_trigger_enter"}, {"other_id", 0}});
    EntityList trigger_entities;
    trigger_entities.push_back(trigger_source);
    script_graph_prepare_scene(trigger_scene, trigger_dir.string());
    script_graph_integration(trigger_entities, trigger_scene, trigger_dir.string(), 1.f / 60.f);
    expect(trigger_graph_logs == 1 && trigger_entities[0]["_pending_events"].empty(),
           "visual graph receives trigger-enter once and consumes its queue");
    script_graph_integration(trigger_entities, trigger_scene, trigger_dir.string(), 1.f / 60.f);
    expect(trigger_graph_logs == 1, "visual graph does not replay an already-consumed trigger");
    ScriptGraphSystem::instance().reset_scene_context(trigger_scene);
    script_graph_started_scenes().erase(trigger_scene);
    fs::remove_all(trigger_dir, watch_ec);

    // Typed graph wires must be the value source at runtime.  This component
    // graph drives Set Field from Add.Result; the literal on Set Field is a
    // deliberately different value so this catches UI-only data wires.
    const fs::path typed_dir = fs::temp_directory_path() /
        ("gameengine_typed_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(typed_dir, watch_ec);
    const std::string typed_scene = "typed_graph_test";
    const std::string typed_asset = "typed_values.graph.json";
    nlohmann::json typed_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_start"}, {"label", "On Start"},
            {"in_pins", nlohmann::json::array({{{"id", 10}, {"type", "exec"}}})},
            {"out_pins", nlohmann::json::array({{{"id", 11}, {"type", "exec"}}})}},
        {{"id", 2}, {"type", "math_add"}, {"label", "Add"}, {"p1", "total"}, {"fp", 3.f}, {"p2", "4"},
            {"in_pins", nlohmann::json::array({{{"id", 21}, {"type", "exec"}}, {{"id", 22}, {"label", "A"}, {"type", "float"}}, {{"id", 23}, {"label", "B"}, {"type", "float"}}})},
            {"out_pins", nlohmann::json::array({{{"id", 24}, {"type", "exec"}}, {{"id", 25}, {"label", "Result"}, {"type", "float"}}})}},
        {{"id", 3}, {"type", "set_field"}, {"label", "Set Field"}, {"p1", "$self"}, {"p2", "Components/Transform/x"}, {"p3", "999"},
            {"in_pins", nlohmann::json::array({{{"id", 31}, {"type", "exec"}}, {{"id", 32}, {"label", "Entity"}, {"type", "entity"}}, {{"id", 33}, {"label", "Field"}, {"type", "string"}}, {{"id", 34}, {"label", "Value"}, {"type", "wildcard"}}})},
            {"out_pins", nlohmann::json::array({{{"id", 35}, {"type", "exec"}}})}},
        {{"id", 4}, {"type", "set_field"}, {"label", "Set Field"}, {"p1", "$self"}, {"p2", "Components/MissingComponent/value"}, {"p3", "99"},
            {"in_pins", nlohmann::json::array({{{"id", 41}, {"type", "exec"}}})}},
        {{"_meta", true}, {"links", nlohmann::json::array({
            {{"fn", 1}, {"fp", 11}, {"tn", 2}, {"tp", 21}},
            {{"fn", 2}, {"fp", 24}, {"tn", 3}, {"tp", 31}},
            {{"fn", 2}, {"fp", 25}, {"tn", 3}, {"tp", 34}},
            {{"fn", 1}, {"fp", 11}, {"tn", 4}, {"tp", 41}}
        })}}
    });
    { std::ofstream graph_file(typed_dir / typed_asset); graph_file << typed_graph.dump(); }
    Entity typed_entity = Entity::object();
    typed_entity["id"] = 8011;
    typed_entity["name"] = "Typed Graph Owner";
    typed_entity["active"] = true;
    typed_entity["components"]["Transform"]["x"] = 0.f;
    typed_entity["components"]["Transform"]["y"] = 0.f;
    typed_entity["components"]["VisualScript"] = {{"asset", typed_asset}, {"enabled", true}, {"run_on_start", true}};
    EntityList typed_entities;
    typed_entities.push_back(typed_entity);
    script_graph_prepare_scene(typed_scene, typed_dir.string());
    script_graph_integration(typed_entities, typed_scene, typed_dir.string(), 1.f / 60.f);
    expect(std::abs(typed_entities[0]["components"]["Transform"].value("x", 0.f) - 7.f) < 0.001f &&
           typed_entities[0]["components"]["Transform"]["x"].is_number_float() &&
           !typed_entities[0]["components"].contains("MissingComponent"),
           "typed visual-script data wires preserve component types and reject invalid properties");
    ScriptGraphSystem::instance().reset_scene_context(typed_scene);
    script_graph_started_scenes().erase(typed_scene);
    fs::remove_all(typed_dir, watch_ec);

    // Graph declarations seed state per graph owner. This verifies that a
    // Get Variable data pin receives its declared default rather than relying
    // on an earlier Set Variable node or leaking a value from another entity.
    const fs::path variable_dir = fs::temp_directory_path() /
        ("gameengine_variable_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(variable_dir, watch_ec);
    const std::string variable_scene = "variable_graph_test";
    const std::string variable_asset = "declared_default.graph.json";
    nlohmann::json variable_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_start"}, {"label", "On Start"},
            {"out_pins", nlohmann::json::array({{{"id", 11}, {"type", "exec"}}})}},
        {{"id", 2}, {"type", "get_variable"}, {"label", "Get Variable"}, {"p1", "spawn_x"},
            {"out_pins", nlohmann::json::array({{{"id", 21}, {"label", "Value"}, {"type", "float"}}})}},
        {{"id", 3}, {"type", "set_field"}, {"label", "Set Field"},
            {"p1", "$self"}, {"p2", "Components/Transform/x"}, {"p3", "999"},
            {"in_pins", nlohmann::json::array({{{"id", 31}, {"type", "exec"}}, {{"id", 34}, {"label", "Value"}, {"type", "float"}}})}},
        {{"_meta", true}, {"variables", nlohmann::json::array({{{"name", "spawn_x"}, {"type", "float"}, {"default", "42.5"}}})},
            {"links", nlohmann::json::array({
                {{"fn", 1}, {"fp", 11}, {"tn", 3}, {"tp", 31}},
                {{"fn", 2}, {"fp", 21}, {"tn", 3}, {"tp", 34}}
            })}}
    });
    { std::ofstream graph_file(variable_dir / variable_asset); graph_file << variable_graph.dump(); }
    Entity variable_entity = Entity::object();
    variable_entity["id"] = 8021;
    variable_entity["active"] = true;
    variable_entity["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}};
    variable_entity["components"]["VisualScript"] = {{"asset", variable_asset}, {"enabled", true}, {"run_on_start", true}};
    EntityList variable_entities;
    variable_entities.push_back(variable_entity);
    script_graph_prepare_scene(variable_scene, variable_dir.string());
    script_graph_integration(variable_entities, variable_scene, variable_dir.string(), 1.f / 60.f);
    expect(std::abs(variable_entities[0]["components"]["Transform"].value("x", 0.f) - 42.5f) < 0.001f,
           "graph-owned variable defaults feed typed Get Variable wires per entity");
    ScriptGraphSystem::instance().reset_scene_context(variable_scene);
    script_graph_started_scenes().erase(variable_scene);
    fs::remove_all(variable_dir, watch_ec);

    // UIButton click events are delivered to the owning VisualScript graph
    // and the event's action is available through the node's typed data pin.
    const fs::path click_dir = fs::temp_directory_path() /
        ("gameengine_ui_click_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(click_dir, watch_ec);
    const std::string click_scene = "ui_click_graph_test";
    const std::string click_asset = "button_click.graph.json";
    nlohmann::json click_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_ui_click"}, {"label", "On UI Click"}, {"p1", "resume"},
            {"out_pins", nlohmann::json::array({{{"id", 11}, {"type", "exec"}}, {{"id", 12}, {"label", "Action"}, {"type", "string"}}})}},
        {{"id", 2}, {"type", "log"}, {"label", "Log"}, {"p1", "wrong literal"},
            {"in_pins", nlohmann::json::array({{{"id", 21}, {"type", "exec"}}, {{"id", 22}, {"label", "Message"}, {"type", "string"}}})}},
        {{"_meta", true}, {"links", nlohmann::json::array({
            {{"fn", 1}, {"fp", 11}, {"tn", 2}, {"tp", 21}},
            {{"fn", 1}, {"fp", 12}, {"tn", 2}, {"tp", 22}}
        })}}
    });
    { std::ofstream graph_file(click_dir / click_asset); graph_file << click_graph.dump(); }
    int ui_click_logs = 0;
    Debug::set_log_callback([&](const std::string& message, int) {
        if (message.find("[Graph] resume") != std::string::npos) ++ui_click_logs;
    });
    Entity click_entity = Entity::object();
    click_entity["id"] = 8031;
    click_entity["active"] = true;
    click_entity["components"]["VisualScript"] = {{"asset", click_asset}, {"enabled", true}, {"run_on_start", false}};
    click_entity["_pending_events"] = nlohmann::json::array({{{"method", "on_ui_click"}, {"action", "resume"}}});
    EntityList click_entities;
    click_entities.push_back(click_entity);
    script_graph_prepare_scene(click_scene, click_dir.string());
    script_graph_integration(click_entities, click_scene, click_dir.string(), 1.f / 60.f);
    expect(ui_click_logs == 1 && click_entities[0]["_pending_events"].empty(),
           "UIButton action reaches its owner VisualScript graph exactly once");
    ScriptGraphSystem::instance().reset_scene_context(click_scene);
    script_graph_started_scenes().erase(click_scene);
    fs::remove_all(click_dir, watch_ec);

    // Authoring convenience only matters if it reaches the game runtime.
    // Exercise the modern typed Spawn and Tween pins rather than their old
    // text-only representations: Spawn must clone the selected source entity
    // and Tween must honour Entity/X/Y/Duration in a component-owned graph.
    const fs::path action_dir = fs::temp_directory_path() /
        ("gameengine_action_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(action_dir, watch_ec);
    const std::string action_scene = "typed_action_graph_test";
    const std::string action_asset = "typed_actions.graph.json";
    nlohmann::json action_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_start"}, {"label", "On Start"},
            {"out_pins", nlohmann::json::array({{{"id", 11}, {"type", "exec"}}})}},
        {{"id", 2}, {"type", "spawn"}, {"label", "Spawn"},
            {"in_pins", nlohmann::json::array({
                {{"id", 20}, {"type", "exec"}},
                {{"id", 21}, {"label", "Template Entity"}, {"type", "entity"}, {"literal", "8101"}},
                {{"id", 22}, {"label", "X"}, {"type", "float"}, {"literal", "12"}},
                {{"id", 23}, {"label", "Y"}, {"type", "float"}, {"literal", "34"}}
            })}},
        {{"id", 3}, {"type", "tween_to"}, {"label", "Tween To"},
            {"in_pins", nlohmann::json::array({
                {{"id", 30}, {"type", "exec"}},
                {{"id", 31}, {"label", "Entity"}, {"type", "entity"}, {"literal", "$self"}},
                {{"id", 32}, {"label", "X"}, {"type", "float"}, {"literal", "80"}},
                {{"id", 33}, {"label", "Y"}, {"type", "float"}, {"literal", "25"}},
                {{"id", 34}, {"label", "Duration"}, {"type", "float"}, {"literal", "0.01"}}
            })}},
        {{"_meta", true}, {"links", nlohmann::json::array({
            {{"fn", 1}, {"fp", 11}, {"tn", 2}, {"tp", 20}},
            {{"fn", 1}, {"fp", 11}, {"tn", 3}, {"tp", 30}}
        })}}
    });
    { std::ofstream graph_file(action_dir / action_asset); graph_file << action_graph.dump(); }
    Entity action_template = Entity::object();
    action_template["id"] = 8101;
    action_template["name"] = "Projectile Template";
    action_template["active"] = true;
    action_template["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}};
    Entity action_owner = Entity::object();
    action_owner["id"] = 8102;
    action_owner["active"] = true;
    action_owner["components"]["Transform"] = {{"x", 0.f}, {"y", 0.f}};
    action_owner["components"]["VisualScript"] = {{"asset", action_asset}, {"enabled", true}, {"run_on_start", true}};
    EntityList action_entities{action_template, action_owner};
    script_graph_prepare_scene(action_scene, action_dir.string());
    script_graph_integration(action_entities, action_scene, action_dir.string(), 0.01f);
    script_graph_integration(action_entities, action_scene, action_dir.string(), 0.02f);
    bool spawn_position_ok = action_entities.size() == 3;
    if (spawn_position_ok) {
        const Entity& clone = action_entities.back();
        spawn_position_ok = std::abs(clone["components"]["Transform"].value("x", 0.f) - 12.f) < 0.001f &&
                            std::abs(clone["components"]["Transform"].value("y", 0.f) - 34.f) < 0.001f;
    }
    expect(spawn_position_ok &&
           std::abs(action_entities[1]["components"]["Transform"].value("x", 0.f) - 80.f) < 0.001f &&
           std::abs(action_entities[1]["components"]["Transform"].value("y", 0.f) - 25.f) < 0.001f,
           "typed Spawn and Tween nodes execute their selected entity and numeric pin values at runtime");
    ScriptGraphSystem::instance().reset_scene_context(action_scene);
    script_graph_started_scenes().erase(action_scene);
    fs::remove_all(action_dir, watch_ec);

    // Component-facing graph actions must mutate the real components rather
    // than only their editor-side JSON.  Keep this focused on the components
    // that a no-code game author most commonly drives from UI/gameplay.
    const fs::path component_action_dir = fs::temp_directory_path() /
        ("gameengine_component_action_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(component_action_dir, watch_ec);
    const std::string component_action_scene = "component_action_graph_test";
    const std::string component_action_asset = "component_actions.graph.json";
    nlohmann::json component_action_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_start"}, {"label", "On Start"},
            {"out_pins", nlohmann::json::array({{{"id", 11}, {"type", "exec"}}})}},
        {{"id", 2}, {"type", "set_text"}, {"label", "Set Text"},
            {"in_pins", nlohmann::json::array({
                {{"id", 20}, {"type", "exec"}},
                {{"id", 21}, {"label", "Entity"}, {"type", "entity"}, {"literal", "8201"}},
                {{"id", 22}, {"label", "Text"}, {"type", "string"}, {"literal", "Ready"}}
            })}},
        {{"id", 3}, {"type", "set_sprite"}, {"label", "Set Sprite"},
            {"in_pins", nlohmann::json::array({
                {{"id", 30}, {"type", "exec"}},
                {{"id", 31}, {"label", "Entity"}, {"type", "entity"}, {"literal", "8202"}},
                {{"id", 32}, {"label", "Sprite"}, {"type", "asset"}, {"literal", "assets/ui/ready.png"}}
            })}},
        {{"id", 4}, {"type", "set_ui_progress"}, {"label", "Set UI Progress"},
            {"in_pins", nlohmann::json::array({
                {{"id", 40}, {"type", "exec"}},
                {{"id", 41}, {"label", "Entity"}, {"type", "entity"}, {"literal", "8203"}},
                {{"id", 42}, {"label", "Value"}, {"type", "float"}, {"literal", "0.75"}}
            })}},
        {{"id", 5}, {"type", "set_audio_volume"}, {"label", "Set Audio Volume"},
            {"in_pins", nlohmann::json::array({
                {{"id", 50}, {"type", "exec"}},
                {{"id", 51}, {"label", "Entity"}, {"type", "entity"}, {"literal", "8204"}},
                {{"id", 52}, {"label", "Volume"}, {"type", "float"}, {"literal", "1.25"}}
            })}},
        {{"_meta", true}, {"links", nlohmann::json::array({
            {{"fn", 1}, {"fp", 11}, {"tn", 2}, {"tp", 20}},
            {{"fn", 1}, {"fp", 11}, {"tn", 3}, {"tp", 30}},
            {{"fn", 1}, {"fp", 11}, {"tn", 4}, {"tp", 40}},
            {{"fn", 1}, {"fp", 11}, {"tn", 5}, {"tp", 50}}
        })}}
    });
    { std::ofstream graph_file(component_action_dir / component_action_asset); graph_file << component_action_graph.dump(); }
    Entity component_owner = Entity::object();
    component_owner["id"] = 8200;
    component_owner["active"] = true;
    component_owner["components"]["VisualScript"] = {{"asset", component_action_asset}, {"enabled", true}, {"run_on_start", true}};
    Entity text_target = Entity::object();
    text_target["id"] = 8201;
    text_target["active"] = true;
    text_target["components"]["TextMeshPro2D"] = {{"text", "Waiting"}};
    Entity sprite_target = Entity::object();
    sprite_target["id"] = 8202;
    sprite_target["active"] = true;
    sprite_target["components"]["SpriteRenderer"] = {{"texture", "assets/ui/old.png"}};
    Entity progress_target = Entity::object();
    progress_target["id"] = 8203;
    progress_target["active"] = true;
    progress_target["components"]["UIProgressBar"] = {{"value", 0.f}};
    Entity audio_target = Entity::object();
    audio_target["id"] = 8204;
    audio_target["active"] = true;
    audio_target["components"]["AudioSource"] = {{"volume", 0.f}};
    EntityList component_action_entities{component_owner, text_target, sprite_target, progress_target, audio_target};
    script_graph_prepare_scene(component_action_scene, component_action_dir.string());
    script_graph_integration(component_action_entities, component_action_scene, component_action_dir.string(), 1.f / 60.f);
    expect(component_action_entities[1]["components"]["TextMeshPro2D"].value("text", "") == "Ready" &&
           component_action_entities[2]["components"]["SpriteRenderer"].value("texture", "") == "assets/ui/ready.png" &&
           std::abs(component_action_entities[3]["components"]["UIProgressBar"].value("value", 0.f) - 0.75f) < 0.001f &&
           std::abs(component_action_entities[4]["components"]["AudioSource"].value("volume", 0.f) - 1.f) < 0.001f,
           "component Visual Script actions mutate text, sprites, UI progress, and clamped audio volume at runtime");
    ScriptGraphSystem::instance().reset_scene_context(component_action_scene);
    script_graph_started_scenes().erase(component_action_scene);
    fs::remove_all(component_action_dir, watch_ec);

    // Input events are first-class visual-script entry points.  Verify that
    // held and released events have their own semantics (rather than both
    // being aliases for key-down) and reach a component-owned graph.
    const fs::path key_graph_dir = fs::temp_directory_path() /
        ("gameengine_key_graph_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(key_graph_dir, watch_ec);
    const std::string key_graph_scene = "key_graph_test";
    const std::string key_graph_asset = "key_events.graph.json";
    nlohmann::json key_graph = nlohmann::json::array({
        {{"id", 1}, {"type", "on_key_held"}, {"label", "On Key Held"}, {"p1", "D"},
            {"out_pins", nlohmann::json::array({{{"id", 11}, {"type", "exec"}}})}},
        {{"id", 2}, {"type", "on_key_released"}, {"label", "On Key Released"}, {"p1", "D"},
            {"out_pins", nlohmann::json::array({{{"id", 21}, {"type", "exec"}}})}},
        {{"id", 3}, {"type", "debug_log"}, {"label", "Log Value"}, {"p1", "held"},
            {"in_pins", nlohmann::json::array({{{"id", 30}, {"type", "exec"}}})}},
        {{"id", 4}, {"type", "debug_log"}, {"label", "Log Value"}, {"p1", "released"},
            {"in_pins", nlohmann::json::array({{{"id", 40}, {"type", "exec"}}})}},
        {{"_meta", true}, {"links", nlohmann::json::array({
            {{"fn", 1}, {"fp", 11}, {"tn", 3}, {"tp", 30}},
            {{"fn", 2}, {"fp", 21}, {"tn", 4}, {"tp", 40}}
        })}}
    });
    { std::ofstream graph_file(key_graph_dir / key_graph_asset); graph_file << key_graph.dump(); }
    Entity key_graph_owner = Entity::object();
    key_graph_owner["id"] = 8301;
    key_graph_owner["active"] = true;
    key_graph_owner["components"]["VisualScript"] = {{"asset", key_graph_asset}, {"enabled", true}, {"run_on_start", false}};
    EntityList key_graph_entities{key_graph_owner};
    int held_logs = 0, released_logs = 0;
    Debug::set_log_callback([&](const std::string& message, int) {
        if (message.find("[Graph] held") != std::string::npos) ++held_logs;
        if (message.find("[Graph] released") != std::string::npos) ++released_logs;
    });
    InputSystem key_input;
    Input::Bind(&key_input);
    SDL_Event key_down{};
    key_down.type = SDL_KEYDOWN;
    key_down.key.keysym.scancode = SDL_SCANCODE_D;
    key_input.begin_frame();
    key_input.process_event(key_down);
    script_graph_prepare_scene(key_graph_scene, key_graph_dir.string());
    script_graph_integration(key_graph_entities, key_graph_scene, key_graph_dir.string(), 1.f / 60.f);
    SDL_Event key_up{};
    key_up.type = SDL_KEYUP;
    key_up.key.keysym.scancode = SDL_SCANCODE_D;
    key_input.begin_frame();
    key_input.process_event(key_up);
    script_graph_integration(key_graph_entities, key_graph_scene, key_graph_dir.string(), 1.f / 60.f);
    expect(held_logs == 1 && released_logs == 1,
           "visual-script key-held and key-released nodes execute with distinct input semantics");
    Input::Bind(nullptr);
    ScriptGraphSystem::instance().reset_scene_context(key_graph_scene);
    script_graph_started_scenes().erase(key_graph_scene);
    fs::remove_all(key_graph_dir, watch_ec);

    // New graphs are explicit V3 documents, not a scene-level array with a
    // hidden metadata node. Verify that the runtime loads the exact editor
    // shape and preserves typed pins/declared values on a save round-trip.
    const nlohmann::json v3_graph_json = {
        {"format", "gameengine.visual-script"}, {"version", 3},
        {"owner", {{"entity_id", 9001}, {"entity_name", "V3 Owner"}}},
        {"next_id", 3}, {"next_pin_id", 24},
        {"variables", nlohmann::json::array({{{"name", "speed"}, {"type", "float"}, {"default", "240"}}})},
        {"nodes", nlohmann::json::array({
            {{"id", 1}, {"type", "on_start"}, {"out_pins", nlohmann::json::array({{{"id", 11}, {"type", "exec"}}})}},
            {{"id", 2}, {"type", "move_by"}, {"in_pins", nlohmann::json::array({
                {{"id", 20}, {"type", "exec"}}, {{"id", 21}, {"label", "Entity"}, {"type", "entity"}, {"literal", "$self"}},
                {{"id", 22}, {"label", "X / sec"}, {"type", "float"}, {"literal", "100"}},
                {{"id", 23}, {"label", "Y / sec"}, {"type", "float"}, {"literal", "0"}}
            })}}
        })},
        {"links", nlohmann::json::array({{{"fn", 1}, {"fp", 11}, {"tn", 2}, {"tp", 20}}})}
    };
    ScriptGraph v3_graph;
    const bool v3_loaded = v3_graph.load_json(v3_graph_json);
    const nlohmann::json v3_saved = v3_graph.save_json();
    expect(v3_loaded && v3_graph.nodes.size() == 2 && v3_graph.links.size() == 1 &&
           v3_graph.variables.size() == 1 && v3_saved.is_object() &&
           v3_saved.value("version", 0) == 3 && v3_saved["nodes"].is_array(),
           "V3 entity-owned visual-script documents load and round-trip without a metadata pseudo-node");

    bus.clear_all();
    return failures == 0 ? 0 : 1;
}
