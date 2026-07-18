/*
 * core.cpp — Fixed-timestep game loop replacing core.py
 *
 * Architecture:
 *   SDL2 window/renderer → InputSystem → Physics (accumulator) →
 *   ScriptSystem (per-frame) → AnimatorSystem → ParticleSystem →
 *   AudioSystem → RenderSystem (with alpha interp) → UISystem → Present
 *
 * Physics: fixed 1/120 s sub-steps via accumulator (Gaffer on Games pattern).
 * Render:  uncapped (vsync) with render-state interpolation using alpha.
 */

#include "core.hpp"
#include "entity.hpp"
#include "time.hpp"
#include "camera.hpp"
#include "input_system.hpp"
#include "physics.hpp"
#include "render_system_vk.hpp"
#include "vk_render/vk_particle_compute.hpp"
#include "systems.hpp"
#include "script_system.hpp"
#include "feature_systems.hpp"
#include "transform_system.hpp"
#include "script_graph_system.hpp"
#include "net/network.hpp"
#include "net/matchmaking.hpp"
#include "net/player_spawn.hpp"
#include "../crash_reporter.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <filesystem>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

// Returns the absolute directory the running executable lives in. Used so
// runtime asset lookups (shaders/, assets/fonts/) work regardless of the
// process's current working directory — same fix already applied to
// editor_main.cpp's launch path; the standalone exported game had the same
// bare-relative-path issue (shader_dir defaulted to "shaders", resolved
// against cwd) and was never updated to match.
static std::string get_executable_dir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return ".";
    std::filesystem::path exe_path(buf);
    return exe_path.parent_path().string();
#else
    std::error_code ec;
    auto p = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) return p.parent_path().string();
    return ".";
#endif
}

// ─── Tuning constants ─────────────────────────────────────────────────────────
static constexpr double PHYSICS_STEP   = 1.0 / 120.0;   // fixed physics tick
static constexpr double MAX_FRAME_TIME = 0.05;           // spiral-of-death guard
static constexpr int    TARGET_FPS     = 0;              // 0 = uncapped (vsync disabled)

// ─── Rolling FPS counter ─────────────────────────────────────────────────────
struct FrameStats {
    double fps=0, ms=0;
private:
    double accum=0; int cnt=0;
public:
    void update(double dt){
        accum+=dt; ++cnt;
        if(accum>=1.0){ fps=cnt/accum; ms=accum/cnt*1000; accum=0; cnt=0; }
    }
};


static Entity convert_json_value(const nlohmann::json& j) {
    if (j.is_null()) return Entity{};
    if (j.is_boolean()) return Entity(j.get<bool>());
    if (j.is_number_integer()) return Entity(j.get<long long>());
    if (j.is_number_float()) return Entity(j.get<double>());
    if (j.is_string()) return Entity(j.get<std::string>());
    if (j.is_array()) {
        Entity arr = Entity::array();
        for (const auto& v : j) arr.push_back(convert_json_value(v));
        return arr;
    }
    if (j.is_object()) {
        Entity obj = Entity::object();
        for (auto it = j.begin(); it != j.end(); ++it) obj[it.key()] = convert_json_value(it.value());
        return obj;
    }
    return Entity{};
}

// ─── Load scene JSON ──────────────────────────────────────────────────────────
static EntityList load_scene(const std::string& path, std::vector<std::string>* out_sorting_layers = nullptr) {
    std::cerr << "[DEBUG] load_scene: opening '" << path << "'\n";
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot open scene: " + path);

    // Read raw file content first so we can show exactly what's on disk
    // if JSON parsing produces something unexpected.
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::cerr << "[DEBUG] load_scene: read " << raw.size() << " bytes from disk\n";
    if (raw.size() < 200) {
        std::cerr << "[DEBUG] load_scene: raw content = \"" << raw << "\"\n";
    } else {
        std::cerr << "[DEBUG] load_scene: raw content (first 200 chars) = \""
                   << raw.substr(0, 200) << "...\"\n";
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw);
    } catch (const nlohmann::json::parse_error& pe) {
        std::cerr << "[DEBUG] load_scene: JSON PARSE FAILED at byte "
                   << pe.byte << ": " << pe.what() << "\n";
        throw;
    }

    std::cerr << "[DEBUG] load_scene: parsed JSON type = " << j.type_name() << "\n";

    if (!j.is_object()) {
        std::cerr << "[DEBUG] load_scene: FATAL - top-level JSON is '"
                   << j.type_name() << "', not an object. Path was: " << path << "\n";
        throw std::runtime_error("Scene file is empty or corrupted: " + path);
    }

    EntityList entities;
    if (!j.contains("entities") || !j["entities"].is_array()) {
        std::cerr << "[DEBUG] load_scene: WARNING - 'entities' missing or not an array; using empty scene\n";
    } else {
        int idx = 0;
        for (const auto& e : j["entities"]) {
            if (!e.is_object()) {
                std::cerr << "[DEBUG] load_scene: entity[" << idx
                          << "] is type '" << e.type_name()
                          << "' (not an object) - skipped\n";
                ++idx;
                continue;
            }
            entities.push_back(convert_json_value(e));
            ++idx;
        }
    }
    std::cerr << "[DEBUG] load_scene: loaded " << entities.size() << " entities OK\n";

    // Sorting layers (Unity2D-style named draw order) — see editor's
    // scene_io.hpp for the matching save/load logic. Absent in older scene
    // files, in which case the caller keeps whatever default order it has.
    if (out_sorting_layers && j.contains("sorting_layers") && j["sorting_layers"].is_array()) {
        std::vector<std::string> layers;
        for (auto& v : j["sorting_layers"])
            if (v.is_string()) layers.push_back(v.get<std::string>());
        if (!layers.empty()) *out_sorting_layers = std::move(layers);
    }

    return entities;
}

// ─── Save prev physics positions for render interpolation ────────────────────
static void snapshot_positions(EntityList& entities) {
    for (auto& e : entities) {
        if (!has_component(e,"Transform")) continue;
        e["_prev_x"] = transform::world_x(e);
        e["_prev_y"] = transform::world_y(e);
    }
}

// ─── Debug helper: wrap a subsystem call so a JSON exception tells us exactly
// which stage of the frame it happened in, and on which frame number. ────────
#define STAGE(label, frame_num, code)                                         \
    do {                                                                      \
        crashreport::set_stage(label);                                        \
        try {                                                                 \
            code;                                                             \
        } catch (const std::exception& ex) {                                  \
            std::cerr << "[DEBUG] CRASH in stage '" << label                  \
                      << "' on frame " << frame_num                           \
                      << " - exception: " << ex.what() << "\n";           \
            throw;                                                            \
        }                                                                     \
    } while (0)

// ─── RAII SDL window ─────────────────────────────────────────────────────────
struct SDLContext {
    SDL_Window* window=nullptr;
    SDLContext(const char* title, int w, int h){
        if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER)!=0)
            throw std::runtime_error(std::string("SDL_Init: ")+SDL_GetError());
        window=SDL_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
            w,h,SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI|SDL_WINDOW_VULKAN);
        if(!window) throw std::runtime_error(std::string("SDL_CreateWindow: ")+SDL_GetError());
    }
    ~SDLContext(){ if(window) SDL_DestroyWindow(window); SDL_Quit(); }
    SDLContext(const SDLContext&)=delete;
    SDLContext& operator=(const SDLContext&)=delete;
};


static std::string resolve_scene_reference(const std::string& raw_scene, const std::string& current_scene) {
    namespace fs = std::filesystem;
    if (raw_scene.empty()) return raw_scene;

    fs::path scene(raw_scene);
    if (scene.is_absolute()) return scene.lexically_normal().generic_string();

    fs::path current(current_scene);
    fs::path base = current.has_parent_path() ? current.parent_path() : fs::path{};
    if (!base.empty()) {
        fs::path candidate = (base / scene).lexically_normal();
        if (fs::exists(candidate)) return candidate.generic_string();
    }

    if (fs::exists(scene)) return fs::absolute(scene).lexically_normal().generic_string();
    return (base / scene).lexically_normal().generic_string();
}

// ─── Derive a window title from the scene path's project folder ──────────────
// Scenes live at games/<project>/scene.json — pull out <project> so each
// standalone export gets its own identifiable window title instead of every
// exported game showing the same generic engine name. Falls back to the
// scene file's stem, then to the generic title, if no "games/" segment is
// found (e.g. a scene loaded from an arbitrary path during development).
static std::string derive_window_title(const std::string& scene_path) {
    namespace fs = std::filesystem;
    fs::path p(scene_path);
    std::string gen = p.generic_string();
    const std::string marker = "games/";
    auto pos = gen.find(marker);
    if (pos != std::string::npos) {
        std::string rest = gen.substr(pos + marker.size());
        auto slash = rest.find('/');
        if (slash != std::string::npos) return rest.substr(0, slash);
    }
    std::string stem = p.stem().string();
    return stem.empty() ? "2D Game Engine Pro" : stem;
}

// Export metadata is authored by Project Settings.  It deliberately lives
// beside the executable so the internal project folder/id never leaks into a
// player-facing standalone title after the project is renamed for release.
static std::string exported_product_title() {
    namespace fs = std::filesystem;
    std::ifstream input(fs::path(get_executable_dir()) / "game_metadata.json");
    if (!input) return {};
    try {
        nlohmann::json metadata;
        input >> metadata;
        if (!metadata.is_object()) return {};
        const auto it = metadata.find("product_name");
        return it != metadata.end() && it->is_string() ? it->get<std::string>() : std::string{};
    } catch (...) {
        return {};
    }
}

// ─── run_game ─────────────────────────────────────────────────────────────────

void run_game(const std::string& scene_path, int width, int height, bool /*vsync*/) {

    // 1. SDL init
    std::string active_scene = scene_path;
    std::string window_title = exported_product_title();
    if (window_title.empty()) window_title = derive_window_title(active_scene);
    SDLContext sdl(window_title.c_str(), width, height);

    while (true) {
        // 2. Load scene
        ScriptRegistry::instance().set_active_project_from_scene_path(active_scene);
        {
            namespace fs = std::filesystem;
            std::string project_name;
            std::string gen = fs::absolute(fs::path(active_scene)).generic_string();
            const std::string marker = "games/";
            auto pos = gen.find(marker);
            if (pos != std::string::npos) {
                std::string rest = gen.substr(pos + marker.size());
                auto slash = rest.find('/');
                if (slash != std::string::npos) project_name = rest.substr(0, slash);
            }
            if (project_name.empty()) {
                fs::path stem = fs::path(active_scene).stem();
                project_name = stem.empty() ? "export" : stem.string();
            }
            Matchmaking::SetProjectName(project_name);
        }
        std::vector<std::string> sorting_layers = {"Background","Default","Foreground","UI"};
        EntityList entities = load_scene(active_scene, &sorting_layers);
        // Scene-local diagnostics should not bleed into the next scene.
        Debug::clear_lines();
        // Contacts, warm-start impulses, and interpolation records belong to
        // the old scene's entity IDs. Reset them before the new scene starts.
        phys::reset_contact_state();
        Network::SeedSessionFromString(active_scene);
        Network::BindScene(&entities);
        // Reserve real headroom up front. instantiate() (script_system.hpp) will
        // grow this vector on demand, but doing so mid-frame reallocates the
        // buffer and dangles every Entity* any script (or ScriptSystem itself)
        // is still holding for the rest of this frame — e.g. the entity pointer
        // ScriptSystem::update() captured right before calling a script's
        // update(), which may itself call Instantiate() (firing a bullet,
        // spawning an enemy, etc.). Starting with generous capacity means normal
        // gameplay essentially never reallocates mid-frame in the first place.
        entities.reserve(std::max<size_t>(entities.size() * 4, entities.size() + 1024));
        transform::invalidate_all();
        const std::string graph_scene_name = std::filesystem::path(active_scene).stem().string();
        const std::string graph_asset_dir = std::filesystem::path(active_scene).parent_path().string();
        script_graph_prepare_scene(graph_scene_name, graph_asset_dir);

        // Same reasoning as the editor's _apply_scene_switch hook: if this
        // (re)load is the one driven by a match starting, spawn every lobby
        // member's player entity now that the scene is actually live.
        if (Matchmaking::InMatch()) {
            NetSpawn::SpawnAllPlayers(entities, std::filesystem::path(active_scene).parent_path().string());
        }

        // 3. Systems
        Camera              camera(width, height);
        InputSystem         input;
        const std::string exe_dir = get_executable_dir();
        RenderSystem        render(sdl.window, camera, exe_dir + "/shaders");
        render.load_default_font(exe_dir + "/assets/fonts/default.ttf");
        render.set_asset_dir(std::filesystem::path(active_scene).parent_path().string());
        render.set_sorting_layers(sorting_layers);
        // Build index map for SortingGroupSystem
        std::unordered_map<std::string,int> sorting_layers_index;
        for (int i=0;i<(int)sorting_layers.size();++i) sorting_layers_index[sorting_layers[i]]=i;
        AnimatorSystem      anim_sys;
        // Per-emitter GPU particle compute — created lazily after first frame
        // so render (and its Vulkan context) is fully initialized.
        std::unordered_map<int, std::unique_ptr<vkr::GpuParticleCompute>> gpu_emitters;
        // Task 1 fix: give RenderSystem a view of the GPU emitter map so
        // _draw_particles() can read particle data from the host-visible SSBO
        // instead of the now-empty CPU e["_particles"] array.
        render.set_gpu_emitters(&gpu_emitters);
        ParticleSystem      particle_sys;
        AudioSystem         audio_sys;
        ScriptSystem        script_sys; script_sys.set_input(&input);
        Grid2DSystem          grid2d_sys;
        TilemapColliderSystem tilemap_sys;
        EventSystem         event_sys;
        UILayoutSystem      ui_layout_sys;
        // New systems: physics effectors, A* pathfinding, Cinemachine virtual cameras,
        // and sorting groups.  SpriteMask is handled directly by RenderSystem:
        // it owns the Vulkan scissor state and must update it in submission order.
        PhysicsEffectorSystem physeffector_sys;
        NavMesh2DSystem       navmesh_sys;
        FollowerSystem        follower_sys;
        VirtualCameraSystem   vcam_sys;
        SortingGroupSystem    sortingGroup_sys;
        // These systems already have complete component implementations; wire
        // them into the standalone loop so their inspector controls affect the
        // exported game rather than only scene JSON.
        ObjectPool2DSystem     object_pool_sys;
        FlockingSystem         flocking_sys;
        LODGroupSystem         lod_group_sys;
        TilemapAnimationSystem tilemap_animation_sys;
        SceneTransitionSystem  transition_sys;
        TrailRenderer2DSystem  trail_renderer_sys;
        CustomRenderTexture2DSystem custom_render_texture_sys;
        VideoPlayer2DSystem    video_player_sys;
        HealthSystem           health_sys;
        ScriptableObjectSystem scriptable_object_sys;
        Spawner2DSystem        spawner_sys;
        LimbIK2DSystem         limb_ik_sys;
        transform::TransformSystem transform_sys;
        object_pool_sys.init(entities);
        spawner_sys.set_asset_dir(graph_asset_dir);
        scriptable_object_sys.set_asset_dir(graph_asset_dir);

        // scene switching callback for scripts
        std::string requested_scene;
        bool scene_requested = false;
        SceneManager::SetLoadSceneHandler([&](const std::string& next_scene) {
            requested_scene = resolve_scene_reference(next_scene, active_scene);
            scene_requested = true;
        });
        // Component-driven transitions request their target at the midpoint.
        // The owner token guarantees this capture is removed before the outer
        // scene loop constructs a fresh set of runtime systems.
        EventBus::instance().subscribe("OnTransitionMidpoint", &transition_sys,
            [&](EntityRef payload, EntityRef) {
                if (!payload) return;
                const std::string target = payload.value("target_scene", std::string());
                if (target.empty()) return;
                requested_scene = resolve_scene_reference(target, active_scene);
                scene_requested = true;
            });

        // 4. Time
        Time::fixed_delta_time = (float)PHYSICS_STEP;

        using Clock    = std::chrono::steady_clock;
        using Duration = std::chrono::duration<double>;

        auto  prev_time   = Clock::now();
        double accumulator = 0.0;
        double elapsed     = 0.0;
        int    frame_num   = 0;
        FrameStats stats;
        bool running = true;

        // 5. Main loop ─────────────────────────────────────────────────────────────
        while (running) {

            // ── 5a. Delta time ────────────────────────────────────────────────────
            auto now = Clock::now();
            double dt = Duration(now - prev_time).count();
            prev_time = now;
            dt = std::min(dt, MAX_FRAME_TIME);
            elapsed    += dt;
            ++frame_num;
            stats.update(dt);
            Time::update((float)dt);
            const float simulation_scale = std::max(0.0f, (float)Time::time_scale);
            const float simulation_dt = (float)dt * simulation_scale;
            // A paused game must not accumulate a backlog of fixed steps and
            // "catch up" violently when resumed. The scaled accumulator is
            // the sole time-scale application for fixed physics; leave its
            // internal multiplier at one to avoid applying slow motion twice.
            accumulator += dt * simulation_scale;
            phys::set_physics_time_scale(1.0f);

            // ── 5b. Input ─────────────────────────────────────────────────────────
            running = input.poll_events((float)dt);
            int mx = input.mouse_x, my = input.mouse_y;
            auto [wx,wy] = camera.screen_to_world((float)mx,(float)my);
            input.set_mouse_world_pos(wx,wy);

            // Publish the actual current render-target size so script-side UI
            // hit-testing (ButtonClicked() etc.) resolves button rects against
            // the same canvas the renderer will draw them into this frame —
            // see Screen's doc comment in unity2d_script_api.hpp for why this
            // can't just be a hardcoded constant.
            {
                VkExtent2D ext = render.current_extent();
                Screen::Set((int)ext.width, (int)ext.height);
            }

            Network::Update(0.f);
            for (auto& ev : Network::ConsumeEvents()) {
                Matchmaking::HandleNetworkEvent(ev);
                EventBus::instance().emit(ev.name, ev.data, nullptr);
            }
            Matchmaking::Update((float)dt);

            // Age the shared Debug queue before scripts can append this frame's
            // primitives. The old standalone path omitted this completely, so
            // effects that drew a short-lived line every frame eventually
            // exhausted memory and froze/crashed the process.
            Debug::update((float)dt);

            // ── 5c. Script update (variable rate) ────────────────────────────────
            // Scripts run once per render frame (same as Unity Update())
            // The pybind11 bridge calls on_update on each Python script here.
            // In pure C++ builds, this is a no-op placeholder.
            advance_destroy_timers(entities, simulation_dt);
            STAGE("script_sys.update", frame_num, script_sys.update(entities, (float)dt));
            // late_update runs after all update()s — camera follow, IK, trailing VFX
            STAGE("script_sys.late_update", frame_num, script_sys.late_update(entities, (float)dt));

            if (Matchmaking::InMatch()) {
                NetSpawn::ReplicateLocalPlayer(entities);
            }

            if (scene_requested) {
                running = false;
                break;
            }

            // ── 5c-2. Transform hierarchy resolve ─────────────────────────────────
            // Parents must be resolved before children for every system below
            // that reads world-space position (physics queries, camera follow,
            // rendering). Local Transform.x/y values written by scripts above
            // are composed with ancestor transforms into a cached "_world" TRS
            // on each entity (see transform_system.hpp).
            STAGE("transform_sys.update (pre-physics)", frame_num, transform_sys.update(entities));

            // ── 5d. Feature systems ───────────────────────────────────────────────
            STAGE("grid2d_sys.update", frame_num, grid2d_sys.update(entities));
            STAGE("tilemap_animation_sys.update", frame_num,
                  tilemap_animation_sys.update(entities, simulation_dt));
            STAGE("custom_render_texture_sys.update", frame_num,
                  custom_render_texture_sys.update(entities, simulation_dt));
            STAGE("video_player_sys.update", frame_num,
                  video_player_sys.update(entities, simulation_dt));
            STAGE("tilemap_sys.update", frame_num, tilemap_sys.update(entities));
            STAGE("event_sys.update", frame_num, event_sys.update(entities, simulation_dt));
            STAGE("object_pool_sys.update", frame_num, object_pool_sys.update(entities, simulation_dt));

            // ── 5d-2. ScriptGraph (visual scripting) ──────────────────────────────
            STAGE("script_graph.update", frame_num,
                script_graph_integration(entities, graph_scene_name, graph_asset_dir, simulation_dt));

            STAGE("scene_transition_sys.update", frame_num,
                  transition_sys.update(entities, simulation_dt));
            // Do not continue simulation/rendering against a scene after its
            // transition has chosen a new target; the outer loop reloads it.
            if (scene_requested) {
                running = false;
                break;
            }

            // ── 5e. Fixed-step physics (Gaffer on Games accumulator) ──────────────
            while (accumulator >= PHYSICS_STEP) {
                STAGE("snapshot_positions", frame_num, snapshot_positions(entities));
                // scripts' fixed_update runs before the integrator, matching Unity order
                STAGE("script_sys.fixed_update", frame_num, script_sys.fixed_update(entities, (float)PHYSICS_STEP));
                // Visual-script Fixed Update follows the exact same physics
                // cadence as native ScriptBase::fixed_update(), rather than
                // being tied to the renderer's frame rate.
                STAGE("script_graph.fixed_update", frame_num,
                    script_graph_fixed_integration(entities, graph_scene_name, graph_asset_dir, (float)PHYSICS_STEP));
                // Physics effectors (AreaEffector2D, ConstantForce2D, etc.) apply
                // forces before the integrator, same as Unity's FixedUpdate order.
                STAGE("physeffector_sys.update", frame_num, physeffector_sys.update(entities, (float)PHYSICS_STEP));
                STAGE("flocking_sys.update", frame_num, flocking_sys.update(entities, (float)PHYSICS_STEP));
                STAGE("phys::apply_physics", frame_num, phys::apply_physics(entities, (float)PHYSICS_STEP));
                accumulator -= PHYSICS_STEP;
            }

            // Physics may have moved parented bodies, so refresh cached world
            // transforms once after the fixed-step loop without forcing a full
            // hierarchy rebuild.
            STAGE("transform_sys.update (post-physics)", frame_num, transform_sys.update(entities));

            const float alpha = (float)(accumulator / PHYSICS_STEP);

            // ── 5f. Camera ────────────────────────────────────────────────────────
            STAGE("camera.update", frame_num, camera.update(entities, simulation_dt));
            // VirtualCameraSystem (Cinemachine): drives Camera2D from highest-priority VirtualCamera
            STAGE("vcam_sys.update", frame_num, vcam_sys.update(entities, simulation_dt));
            // NavMesh2D A* pathfinding
            STAGE("navmesh_sys.update", frame_num, navmesh_sys.update(entities, simulation_dt));
            // PathFollower2D uses the same world-space transform contract as
            // the editor's path panel. Refresh cached transforms immediately
            // afterwards so camera, animation and rendering see this frame's
            // position rather than a one-frame-old value.
            STAGE("follower_sys.update", frame_num, follower_sys.update(entities, simulation_dt));
            STAGE("transform_sys.update (post-navigation)", frame_num, transform_sys.update(entities));
            STAGE("lod_group_sys.update", frame_num, lod_group_sys.update(entities, simulation_dt));
            STAGE("trail_renderer_sys.update", frame_num,
                  trail_renderer_sys.update(entities, simulation_dt));
            STAGE("health_sys.update", frame_num, health_sys.update(entities, simulation_dt));
            STAGE("scriptable_object_sys.update", frame_num, scriptable_object_sys.update(entities));
            STAGE("spawner_sys.update", frame_num, spawner_sys.update(entities, simulation_dt));
            STAGE("limb_ik_sys.update", frame_num, limb_ik_sys.update(entities));
            STAGE("transform_sys.update (post-ik)", frame_num, transform_sys.update(entities));

            // ── 5g. Animation / particles / audio ────────────────────────────────
            // SortingGroup: stamp _sorting_key before draw
            STAGE("sortingGroup_sys.update", frame_num, sortingGroup_sys.update(entities));
            STAGE("anim_sys.update", frame_num, anim_sys.update(entities, simulation_dt));
            STAGE("particle_sys.update", frame_num, particle_sys.update(entities, simulation_dt));
            STAGE("audio_sys.update", frame_num, audio_sys.update(entities, simulation_dt));
            Network::CaptureSnapshot((std::uint32_t)frame_num);

            // ── 5h. Render ───────────────────────────────────────────────────────
            // ── GPU particle compute (dispatched into the frame command buffer) ──
            // Explicitly opted-in large emitters can use the GPU compute prototype.
            // CPU simulation remains the default and handles all emitters safely.
            //
            // Task 2 fix: we no longer use begin_one_shot()/end_one_shot() which
            // called vkQueueWaitIdle() — a full GPU stall every frame per emitter.
            // Instead we start the frame command buffer first (render.clear()),
            // record all compute dispatches into that same command buffer, then
            // insert a compute→vertex/fragment pipeline barrier so the render pass
            // that follows can safely read the SSBO results.  resolve_count() is
            // deferred behind the existing per-frame-in-flight fence inside
            // end_frame() — the live-count readback is one frame latent but that
            // is imperceptible and does not affect the render path (task 1).

            render.clear({30,30,30,255}); // starts the frame command buffer

            {
                VkCommandBuffer frame_cmd = render.current_cmd();
                bool any_gpu = false;

                for (auto& e : entities) {
                    if (!entity_active(e)) continue;
                    auto* em = has_component(e,"ParticleEmitter") ? &e["components"]["ParticleEmitter"] : nullptr;
                    if (!em) continue;
                    // The compute prototype owns a single pair of mapped
                    // buffers. Automatically promoting a CPU emitter while
                    // multiple frames are in flight races those buffers and
                    // can make a stale counter drive an enormous render loop.
                    // Keep the proven CPU implementation as the default.
                    // GPU simulation remains an explicit experimental opt-in
                    // until it uses per-frame staging/timeline synchronization.
                    if (!em->value("gpu_simulation", false)) continue;
                    int eid = e.value("id", -1);
                    uint32_t pcnt = e.contains("_particles") ? (uint32_t)e["_particles"].size() : 0;
                    if (pcnt < vkr::GpuParticleCompute::kGpuThreshold) continue;

                    // Lazily create the GPU emitter
                    if (!gpu_emitters.count(eid)) {
                        gpu_emitters[eid] = std::make_unique<vkr::GpuParticleCompute>(
                            render.vk_ctx(), exe_dir + "/shaders");
                    }

                    // Seed new particles spawned by CPU this frame, then dispatch
                    auto& gpu = *gpu_emitters[eid];
                    std::vector<vkr::GpuParticle> spawned;
                    for (auto& p : e["_particles"]) {
                        spawned.push_back({p.value("x",0.f), p.value("y",0.f),
                                           p.value("vx",0.f), p.value("vy",0.f),
                                           p.value("age",0.f), p.value("lifetime",1.f),
                                           p.value("frame",0), 0.f});
                    }
                    gpu.spawn(spawned);
                    gpu.tick(frame_cmd, (float)dt); // records into frame_cmd, no stall
                    e["_particles"] = Entity::array(); // GPU owns particles now
                    any_gpu = true;
                }

                // After all compute dispatches, insert a single pipeline barrier
                // so the vertex/fragment stages in the render pass that follows
                // can safely read any SSBO written by the compute shaders.
                if (any_gpu) {
                    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
                    vkCmdPipelineBarrier(frame_cmd,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 1, &barrier, 0, nullptr, 0, nullptr);
                }

                // resolve_count() reads the atomic counter written by the compute
                // shader.  With the old vkQueueWaitIdle approach this was safe to
                // do immediately.  Now the GPU may still be executing the dispatch
                // when we reach this line, so we resolve on the *previous* frame's
                // results — one frame of latency on the live-count, which is
                // invisible in practice.  The render path (task 1) uses the same
                // mapped buffer so it also reflects the previous frame's survivors,
                // which is consistent and correct.
                for (auto& [eid, gpu] : gpu_emitters) gpu->resolve_count();
            }
            STAGE("render.draw_parallax", frame_num, render.draw_parallax(entities));
            STAGE("render.draw", frame_num, render.draw(entities, alpha));
            // Resume any coroutine parked on CO_WAIT_END_OF_FRAME — must run
            // exactly here: after the world pass has rasterized this frame's
            // pixels, before UI/present. See CO_WAIT_END_OF_FRAME's comment
            // in script_system.hpp.
            STAGE("script_sys.resume_end_of_frame", frame_num, script_sys.resume_end_of_frame(entities));

            // Resolve UILayoutGroup arrangement (Horizontal/Vertical/Grid —
            // see feature_systems.hpp) before draw_ui reads each child's
            // anchor/pivot/pos/width/height fields below.
            STAGE("ui_layout_sys.update", frame_num, ui_layout_sys.update(entities));
            // SDL reports pointer coordinates in logical window pixels while
            // Vulkan renders in drawable pixels. On a high-DPI display those
            // differ; passing logical coordinates to the UI made every button
            // miss its hit rectangle in exported games. Convert only for the
            // renderer's UI canvas (world input keeps the Camera's logical
            // coordinate system).
            int ui_mouse_x = input.mouse_x, ui_mouse_y = input.mouse_y;
            int logical_w = 0, logical_h = 0;
            SDL_GetWindowSize(sdl.window, &logical_w, &logical_h);
            const VkExtent2D ui_extent = render.current_extent();
            if (logical_w > 0 && logical_h > 0 && ui_extent.width > 0 && ui_extent.height > 0) {
                ui_mouse_x = (int)std::lround((double)input.mouse_x * ui_extent.width / logical_w);
                ui_mouse_y = (int)std::lround((double)input.mouse_y * ui_extent.height / logical_h);
            }
            STAGE("render.draw_ui", frame_num, render.draw_ui(entities, ui_mouse_x, ui_mouse_y,
                  input.get_mouse_button(1), input.get_mouse_button_down(1)));

            render.present();

            // ── 5i. Window title FPS counter (every 60 frames) ───────────────────
            if (frame_num % 60 == 0) {
                char title[160];
                std::snprintf(title, sizeof(title),
                    "%s  |  %.0f fps  |  %.2f ms/frame  |  C++ native",
                    window_title.c_str(), stats.fps, stats.ms);
                SDL_SetWindowTitle(sdl.window, title);
            }
        }

        EventBus::instance().unsubscribe_all(&transition_sys);
        SceneManager::ClearLoadSceneHandler();

        if (!scene_requested || requested_scene.empty()) {
            break;
        }

        active_scene = requested_scene;
        window_title = exported_product_title();
        if (window_title.empty()) window_title = derive_window_title(active_scene);
        SDL_SetWindowTitle(sdl.window, window_title.c_str());
    }
}
