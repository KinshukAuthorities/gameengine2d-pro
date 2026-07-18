#pragma once
#ifndef _USE_MATH_DEFINES
#  define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <cstddef>
#include "entity.hpp"
#include "transform_system.hpp"
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>
#include <memory>
#include <tuple>
#include <optional>
#include <deque>
#include <array>

// ─── Tunable constants (Box2D-calibrated) ـ───────────────────────────────────
static constexpr float GRAVITY            = 980.f;    // pixels/s²  (≈Unity default scaled)
static constexpr float TARGET_STEP        = 1.f / 60.f;
static constexpr int   MAX_SUBSTEPS       = 8;
static constexpr int   VELOCITY_ITERS     = 10;        // Box2D default
static constexpr int   POSITION_ITERS     = 6;
static constexpr float BAUMGARTE          = 0.20f;    // Box2D: 0.2
static constexpr float SLOP               = 1.0f;     // 1px dead-zone prevents micro-oscillation; check_overlap uses its own shrink
static constexpr float MAX_LINEAR_CORR    = 8.0f;     // cap position correction per step
static constexpr float SLEEP_VEL_SQ      = 4.f*4.f;  // raised: 2px/s was too low — player pressing wall could sleep mid-press
static constexpr float SLEEP_ANG          = 0.5f;     // rad/s
static constexpr float SLEEP_TIME         = 1.0f;     // longer timer: reduces false-sleep on slow-moving contacts
// Split-impulse position solver (separates bias velocity from real velocity)
static constexpr float SPLIT_IMPULSE_BIAS     = 0.40f;   // fraction of penetration corrected per step (was 0.20 — too slow, player sank through floor over multiple frames)
static constexpr float SPLIT_IMPULSE_MAX      = 200.0f;  // max bias velocity (pixels/s)
// Warm-start scale (Box2D: 0.8 avoids over-shoot on first frame)
static constexpr float WARM_START_SCALE       = 0.80f;
// Energy-based sleep (per-body kinetic energy threshold, pixel²/s²)
static constexpr float SLEEP_ENERGY_THRESH    = 16.0f;   // ½mv² per unit mass — raised to match SLEEP_VEL_SQ = 4²
static constexpr float SLEEP_ENERGY_AVG_ALPHA = 0.1f;    // EMA decay for energy averaging
// Speculative contact distance (generate contacts before overlap)
static constexpr float SPECULATIVE_MARGIN     = 6.0f;    // pixels look-ahead
// Sub-step normal smoothing (damp contact normal drift across substeps)
static constexpr float NORMAL_SMOOTH_ALPHA    = 0.15f;   // blend factor toward cached normal (was 0.3 — too strong, drifted normals)
// Coyote-time for grounded detection (seconds)
static constexpr float COYOTE_TIME            = 0.10f;
static constexpr float MAX_LINEAR_VEL     = 10000.f;
static constexpr float MAX_ANGULAR_VEL    = 50.f;     // rad/s
static constexpr float RESTITUTION_THRESH = 55.f;     // below this speed → no bounce
static constexpr int   CELL_SIZE          = 128;
static constexpr float STATIC_MU_MULT     = 1.41f;    // static = sqrt(2)*dynamic approx
static constexpr float WALL_CLING_MU_MULT = 2.75f;   // extra static friction for near-vertical wall contacts
static constexpr float WALL_CLING_VELOCITY_EPS = 2.5f; // suppress tiny wall-slide drift
static constexpr float WALL_NORMAL_THRESHOLD = 0.65f;  // near-vertical wall classification
static constexpr float WALL_PRESS_VEL_THRESH  = 0.5f;   // minimum press speed to qualify as wall-cling
static constexpr int   MAX_CONTACTS       = 2;        // Box2D max per manifold
// CCD
static constexpr float CCD_THRESHOLD      = 0.5f;     // fraction of AABB size → trigger sweep
static constexpr float CCD_TOI_EPSILON    = 1e-4f;
// Breakable joints
static constexpr float BREAK_FORCE_DEFAULT = 1e6f;
static constexpr float BREAK_TORQUE_DEFAULT = 1e5f;

// ─── Math helpers ─────────────────────────────────────────────────────────────
namespace phys {

inline float finite_val(float v, float def = 0.f) {
    return std::isfinite(v) ? v : def;
}
inline float clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
inline float dot(float ax, float ay, float bx, float by) { return ax*bx + ay*by; }
inline float cross(float ax, float ay, float bx, float by) { return ax*by - ay*bx; }
inline float len2(float x, float y) { return x*x + y*y; }
inline float len(float x, float y)  { return std::hypot(x,y); }

inline std::pair<float,float> normalize(float x, float y) {
    float d = std::hypot(x, y);
    if (!std::isfinite(d) || d < 1e-12f) return {0.f, 1.f};
    return {x/d, y/d};
}
inline std::pair<float,float> rot(float x, float y, float c, float s) {
    return {x*c - y*s, x*s + y*c};
}

using Vec2  = std::pair<float,float>;
using Verts = std::vector<Vec2>;

inline Vec2 world_from_local(float tx, float ty, float c, float s, float lx, float ly) {
    return {tx + lx*c - ly*s, ty + lx*s + ly*c};
}

// ─── Convex hull, winding ─────────────────────────────────────────────────────
Verts convex_hull(Verts pts);
float poly_area(const Verts& v);
Verts ensure_ccw(Verts v);

// ─── Physics Material ─────────────────────────────────────────────────────────
enum class CombineMode { Average, Minimum, Maximum, Multiply };
struct PhysicsMaterial {
    float friction    = 0.4f;
    float bounciness  = 0.0f;
    CombineMode friction_combine   = CombineMode::Average;
    CombineMode bounce_combine     = CombineMode::Maximum;
};

// ─── Shape ────────────────────────────────────────────────────────────────────
enum class ShapeKind { Circle, Polygon, Edge, Capsule };

struct Shape {
    ShapeKind  kind   = ShapeKind::Polygon;
    Entity*    entity = nullptr;
    Entity*    col    = nullptr;
    int        sub_id = 0;
    float cx = 0, cy = 0;
    float radius = 0;
    // Capsule-specific
    float cap_half_h = 0;      // distance from center to end-cap center
    bool  cap_horiz  = false;
    Verts verts;               // polygon verts (world space)
    std::vector<Vec2> world_pts; // edge chain
    float thickness = 2.f;
    // One-sided edge: outward face normal. (0,0) = two-sided (default).
    // When set, dispatch_shapes builds an asymmetric slab that only pushes
    // in the face_n direction, preventing SAT from resolving into the tile.
    float face_nx = 0.f, face_ny = 0.f;
    PhysicsMaterial material;
};

std::optional<Shape> build_shape(Entity& e);
std::tuple<float,float,float,float> shape_aabb(const Shape& s);

// ─── Contact point ────────────────────────────────────────────────────────────
struct CP {
    float x, y;       // world position
    float depth;
    float lambda_n = 0.f;   // accumulated normal impulse (warm-start)
    float lambda_t = 0.f;   // accumulated tangent impulse
    // Split-impulse: pseudo-velocities correcting position only (not real velocities)
    float pseudo_n = 0.f;   // bias normal pseudo-impulse
    float pseudo_t = 0.f;   // bias tangent pseudo-impulse
    int   fkey     = 0;     // feature key for contact persistence
    // Per-contact effective mass cache (computed once per substep, reused each iteration)
    float k_normal  = 0.f;  // 1/K for normal direction
    float k_tangent = 0.f;  // 1/K for tangent direction
    bool  k_cached  = false; // true after first pre-step computation this substep
    // ContactPoint2D.separationDistance: signed penetration depth (negative = overlapping)
    float separation_distance = 0.f;
    // ContactPoint2D.bounciness: resolved combined bounciness at this contact (for audio/VFX)
    float bounciness = 0.f;
};

// ─── Manifold ─────────────────────────────────────────────────────────────────
struct Manifold {
    Entity* e1   = nullptr; Entity* e2   = nullptr;
    Entity* col1 = nullptr; Entity* col2 = nullptr;
    Entity* rb1  = nullptr; Entity* rb2  = nullptr;
    float nx = 0.f, ny = 1.f;
    std::array<CP, MAX_CONTACTS> contacts;
    int   contact_count   = 0;
    bool  is_trigger      = false;
    bool  one_way_skip    = false;
    int   sub1           = 0;
    int   sub2           = 0;
    float restitution     = 0.f;
    float friction_d      = 0.4f;
    float friction_s      = 0.56f;
    float surface_speed   = 0.f;   // Conveyor / SurfaceEffector2D speed
};

struct GroundInfo {
    bool grounded = false;
    bool on_wall = false;
    bool wall_left = false;
    bool wall_right = false;
    float ground_normal_x = 0.f;
    float ground_normal_y = 1.f;
    float wall_normal_x = 0.f;
    float wall_normal_y = 0.f;
    float slope_angle = 0.f; // degrees
    float coyote_timer = 0.f; // seconds since last grounded — useful for platformers
    bool  was_grounded = false; // true while within coyote window
};

// ─── Polygon mass/inertia (centroid + inertia via Green's theorem) ────────────
// Returns {centroid_x, centroid_y, area, Izz} for a CCW polygon (world verts)
std::tuple<float,float,float,float> poly_mass_data(const Verts& v);

// ─── Persistent contact cache (warm-starting) ─────────────────────────────────
struct CachedManifold {
    std::array<CP, MAX_CONTACTS> contacts;
    int count = 0;
    int sub1 = 0;
    int sub2 = 0;
    float nx = 0, ny = 1;
    // Smoothed normal (LERP toward this to prevent jitter at curved/edge surfaces)
    float smooth_nx = 0, smooth_ny = 1;
    int   age = 0;   // frames alive — used to fade smooth weight
};
// key: (min_id<<32|max_id) ^ (sub1 ^ sub2)
using ContactCache = std::unordered_map<long long, CachedManifold>;

// ─── Force API ────────────────────────────────────────────────────────────────
void add_force        (Entity& rb, float fx, float fy);
void add_force_at_point(Entity& rb, float fx, float fy, float wx, float wy);
void add_impulse      (Entity& rb, float jx, float jy);
void add_impulse_at_point(Entity& rb, float jx, float jy, float wx, float wy);
void add_torque       (Entity& rb, float torque);
void add_angular_impulse(Entity& rb, float j);
void explosion_force  (EntityList& entities, float x, float y,
                       float radius, float strength,
                       float upward_modifier = 0.f,
                       const std::string& mode = "impulse");
void apply_constant_forces(EntityList& entities);
void apply_buoyancy(EntityList& entities);
void apply_point_effectors(EntityList& entities);
void apply_area_effectors(EntityList& entities);
GroundInfo query_ground_info(Entity& e, const std::vector<void*>& manifold_opaque);
inline GroundInfo query_ground_info(Entity& e) {
    static const std::vector<void*> empty;
    return query_ground_info(e, empty);
}
template <class T, decltype(std::declval<T>().operator->(), 0) = 0>
inline GroundInfo query_ground_info(T e) {
    if (!e) return {};
    return query_ground_info(*e);
}

// ─── Physics query filter (for advanced overlap/raycast filtering) ─────────────
struct QueryFilter {
    int layer_mask = 0xFFFF;
    std::function<bool(Entity*)> predicate;
    bool triggers = false;
};

// ─── Queries ──────────────────────────────────────────────────────────────────
std::vector<Entity*> point_cast   (EntityList& e, float x, float y,             int mask=0xFFFF);
std::vector<Entity*> overlap_circle(EntityList& e, float x, float y, float r,   int mask=0xFFFF);
std::vector<Entity*> overlap_box  (EntityList& e, float x, float y, float w, float h, float rot_deg=0, int mask=0xFFFF);

// Enhanced queries with filters
std::vector<Entity*> point_cast_filtered   (EntityList& e, float x, float y,             const QueryFilter& filter);
std::vector<Entity*> overlap_circle_filtered(EntityList& e, float x, float y, float r,   const QueryFilter& filter);
std::vector<Entity*> overlap_box_filtered  (EntityList& e, float x, float y, float w, float h, float rot_deg = 0, const QueryFilter& filter = QueryFilter{});

struct RayHit {
    float distance;
    Vec2  point;
    Vec2  normal;
    Entity* entity;
    Entity* collider;   // the specific collider component
};
std::optional<RayHit> raycast(EntityList& e, float ox, float oy,
                               float dx, float dy, float dist=1e9f,
                               int mask=0xFFFF, bool triggers=false);
std::vector<RayHit>  raycast_all(EntityList& e, float ox, float oy,
                                  float dx, float dy, float dist=1e9f,
                                  int mask=0xFFFF, bool triggers=false);

// ─── Broadphase selection (Phase 3 — Dynamic AABB Tree) ───────────────────────
// Unity / Box2D use a dynamic AABB tree (b2DynamicTree) for broadphase, which
// gives O(n log n) pair generation instead of the uniform grid's degradation
// past ~200 bodies in dense or unevenly-distributed scenes. Nova now supports
// both; the grid remains the default for small scenes (lower constant factor),
// and the BVH can be enabled for large scenes.
enum class BroadphaseMode { UniformGrid, DynamicTree };
void set_broadphase_mode(BroadphaseMode mode);
BroadphaseMode get_broadphase_mode();
// Convenience: automatically picks DynamicTree once body count crosses this
// threshold, UniformGrid below it. Set threshold <0 to disable auto-switching
// and always respect set_broadphase_mode().
void set_broadphase_auto_threshold(int body_count);

// ─── Per-collider collision ignore (Physics2D.IgnoreCollision parity) ────────
// The existing ignore_collision(entity_id_a, entity_id_b) ignores at the whole
// -entity level. Composite bodies (CompositeCollider2D, decomposed concave
// polygons, Tilemap edge chains) expose multiple sub-colliders per entity via
// Shape::sub_id; this lets you ignore collisions between one specific
// sub-collider and another, leaving sibling sub-colliders unaffected.
// sub_id_a / sub_id_b of -1 means "match any sub-collider on that entity"
// (so ignore_collider(eA, -1, eB, -1) behaves like the legacy whole-entity API).
void ignore_collider(int entity_id_a, int sub_id_a, int entity_id_b, int sub_id_b, bool ignore = true);
bool is_collider_ignored(int entity_id_a, int sub_id_a, int entity_id_b, int sub_id_b);
void clear_collider_ignores();

// ─── Island-based solving (Phase 3) ───────────────────────────────────────────
// Unity/Box2D solve each connected island of touching dynamic bodies
// independently. This lets the solver converge per-island instead of running
// all VELOCITY_ITERS/POSITION_ITERS globally across unrelated contacts.
// Static bodies are excluded from island adjacency (they don't need to be in
// an island — that part of the gap doc's "Static body islands" note).
void set_island_solving_enabled(bool enabled);
bool get_island_solving_enabled();

// ─── Collision matrix (layer-vs-layer ignoring) ───────────────────────────────
// 32 layers × 32 layers bitmask (identical to Unity's Physics2D.GetIgnoreLayerCollision)
struct CollisionMatrix {
    uint32_t bits[32] = {};   // bits[i] & (1<<j) → ignore layer i vs j
    bool ignores(int l1, int l2) const {
        if (l1<0||l1>31||l2<0||l2>31) return false;
        return (bits[l1] >> l2) & 1;
    }
    bool can_collide(int l1, int l2) const { return !ignores(l1, l2); }
    void set_ignore(int l1, int l2, bool ig) {
        if (l1<0||l1>31||l2<0||l2>31) return;
        if (ig) { bits[l1]|=(1u<<l2); bits[l2]|=(1u<<l1); }
        else    { bits[l1]&=~(1u<<l2); bits[l2]&=~(1u<<l1); }
    }
};
CollisionMatrix& global_collision_matrix();

// ─── Main physics step ────────────────────────────────────────────────────────
void apply_physics(EntityList& entities, float dt, float gravity = GRAVITY);

// ── Phase 1: Fixed Timestep Accumulator ────────────────────────────────────
// set_fixed_timestep() changes the simulation rate at runtime (default: 1/60s).
// apply_physics_accumulated() drains the accumulator in fixed steps and returns
// the render interpolation alpha [0,1] for use with get_render_position().
void  set_fixed_timestep(float dt);
float get_fixed_timestep();
float get_render_alpha();
float apply_physics_accumulated(EntityList& entities, float frame_dt, float gravity = GRAVITY);
// Clears scene-local contact/warm-start/interpolation state. Call before
// replacing the entity list (scene switch or entering a fresh editor Play run).
void reset_contact_state();

// Internal: called by physics_ext set_gravity_vector() to route a custom
// gravity direction into the core integrator.
void phys_set_gravity_override(float gx, float gy, bool active);

// ─── Character Controller (Unity2D CharacterController2D parity) ─────────────
struct CharacterController {
    float min_move_distance = 0.001f;
    float skin_width = 0.01f;
    float slope_limit = 45.f;      // degrees
    float step_offset = 0.3f;
    bool  detect_collisions = true;
    int   layer_mask = 0xFFFF;
};
// CollisionFlags bitmask — return value from move_character()
// Matches Unity: None=0, Sides=1, Above=2, Below=4
static const int CC_NONE  = 0;
static const int CC_SIDES = 1;
static const int CC_ABOVE = 2;
static const int CC_BELOW = 4;
// Returns CollisionFlags bitmask indicating which surfaces were hit
int  move_character(Entity& entity, float dx, float dy, const CharacterController& cc = {});
bool is_grounded(Entity& entity);
Vec2 get_velocity(Entity& entity);

// ─── Rigidbody2D direct position/rotation setters (no ghost velocity) ────────
// Unity's rb.position = v  — teleports without velocity inference.
void set_body_position(Entity& entity, float x, float y);
void set_body_rotation(Entity& entity, float angle_deg);

// ─── Rigidbody2D.IsTouching / IsTouchingLayers ───────────────────────────────
bool is_touching(Entity& entity, Entity& other);
bool is_touching_layers(Entity& entity, int layer_mask);

// ─── Contact Listener (Unity2D OnCollision2D callbacks) ─────────────────────
struct ContactListener {
    std::function<void(Entity*, Entity*, const Manifold&)> on_collision_enter;
    std::function<void(Entity*, Entity*, const Manifold&)> on_collision_exit;
    std::function<void(Entity*, Entity*, const Manifold&)> on_trigger_enter;
    std::function<void(Entity*, Entity*, const Manifold&)> on_trigger_exit;
    std::function<void(Entity*, Entity*, const Manifold&, float)> pre_solve;  // modify manifold before solve
    std::function<void(Entity*, Entity*, const Manifold&)> post_solve;       // after impulse applied
};
void set_contact_listener(const ContactListener& listener);
ContactListener& get_contact_listener();

// ─── Rigidbody2D direct position/rotation setters (no ghost velocity) ────────
// Unity's rb.position = v  — teleports without velocity inference.
void set_body_position(Entity& entity, float x, float y);
void set_body_rotation(Entity& entity, float angle_deg);

// ─── Rigidbody2D.IsTouching / IsTouchingLayers ───────────────────────────────
bool is_touching(Entity& entity, Entity& other);
bool is_touching_layers(Entity& entity, int layer_mask);

// ─── Contact relative velocity (Collision2D.relativeVelocity) ────────────────
// Returns the relative velocity stored by dispatch_contact_events for the entity.
inline Vec2 get_contact_relative_velocity(Entity& entity) {
    return {entity.value("_contact_rel_vel_x",0.f), entity.value("_contact_rel_vel_y",0.f)};
}
Vec2 get_interpolated_position(Entity& entity, float alpha);
float get_interpolated_rotation(Entity& entity, float alpha);

// ─── Physics Debug Drawing ────────────────────────────────────────────────────
struct DebugDrawOptions {
    bool draw_colliders = true;
    bool draw_contacts = true;
    bool draw_aabbs = false;
    bool draw_velocities = false;
    bool draw_joints = true;
    bool draw_ground_info = false;
    float line_thickness = 1.f;
    uint32_t color_static = 0xFF00FF00;   // green
    uint32_t color_dynamic = 0xFF0000FF;  // blue
    uint32_t color_kinematic = 0xFFFF0000; // red
    uint32_t color_trigger = 0xFFFFFF00;   // yellow
    uint32_t color_contact = 0xFFFFFFFF;  // white
    uint32_t color_joint = 0xFFFF00FF;     // magenta
};
void set_debug_draw_options(const DebugDrawOptions& opts);
DebugDrawOptions& get_debug_draw_options();

// ─── Physics Profiler ─────────────────────────────────────────────────────────
struct PhysicsStats {
    int total_bodies = 0;
    int dynamic_bodies = 0;
    int static_bodies = 0;
    int kinematic_bodies = 0;
    int total_contacts = 0;
    int broad_phase_pairs = 0;
    float solve_time_ms = 0.f;
    float broad_phase_time_ms = 0.f;
    float narrow_phase_time_ms = 0.f;
    int sleep_count = 0;
};
PhysicsStats get_physics_stats();
void reset_physics_stats();

// Lightweight cache counters for the editor profiler and regression tests.
// These are counts only: no runtime internals or pointers are exposed.
struct PhysicsRuntimeCacheStats {
    std::size_t interpolation_records = 0;
    std::size_t active_contact_pairs = 0;
    std::size_t contact_id_records = 0;
    std::size_t ignored_entity_pairs = 0;
    std::size_t ignored_collider_pairs = 0;
};
PhysicsRuntimeCacheStats get_runtime_cache_stats();

// ─── Collision Ignore Pairs (Physics2D.IgnoreCollision) ───────────────────────
void ignore_collision(int entity_id_a, int entity_id_b, bool ignore = true);
bool is_collision_ignored(int entity_id_a, int entity_id_b);
void clear_collision_ignores();

// ─── Physics Time Scale (Time.timeScale for physics) ─────────────────────────
void set_physics_time_scale(float scale);
float get_physics_time_scale();

// ─── Center of Mass Offset (Rigidbody2D.centerOfMass) ────────────────────────
void set_center_of_mass(Entity& entity, float offset_x, float offset_y);
Vec2 get_center_of_mass(Entity& entity);

// ─── Surface Velocity (PhysicsMaterial2D.surfaceVelocity) ─────────────────────
void set_surface_velocity(Entity& entity, float vx, float vy);
Vec2 get_surface_velocity(Entity& entity);

// ─── Auto Mass Calculation (Rigidbody2D.autoMass) ─────────────────────────────
void auto_compute_mass(Entity& entity, float density = 1.f);

// ─── PhysicsPack registry ─────────────────────────────────────────────────────
struct PhysicsPackBase {
    std::string name, feature;
    int variant = 0;
    virtual ~PhysicsPackBase() = default;
    virtual void pre_solve (void*, float) {}
    virtual void solve_contact(void*, void*, void*) {}
    virtual void post_solve(void*, float) {}
};

class PhysicsPackRegistry {
public:
    static PhysicsPackRegistry& instance();
    std::shared_ptr<PhysicsPackBase> make(const std::string& feature, int index=0);
    std::shared_ptr<PhysicsPackBase> make_by_number(int pack_num);
    std::vector<std::string> features() const;
    int total() const { return (int)_registry.size(); }
private:
    struct Entry { std::string name, feature; int variant; std::function<std::shared_ptr<PhysicsPackBase>()> factory; };
    std::vector<Entry> _registry;
    std::unordered_map<std::string, std::vector<int>> _by_feature;
    PhysicsPackRegistry();
};

} // namespace phys
