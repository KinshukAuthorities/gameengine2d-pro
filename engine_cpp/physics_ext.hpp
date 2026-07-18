#pragma once
// This define lets physics.cpp detect that physics_ext is in the build
// and skip its fallback definitions, avoiding duplicate symbol errors.
#define PHYS_EXT_INCLUDED
/*
 * physics_ext.hpp — Nova Engine Physics Extension (Unity2D gap-closing)
 *
 * New features added:
 *  ① Global gravity direction vector (not just downward)
 *  ② BuoyancyEffector2D — fluid density, linear/angular drag, surface offset
 *  ③ ConstantForce2D — persistent per-body force/torque component
 *  ④ PointEffector2D — radial attract/repel with fall-off modes
 *  ⑤ AreaEffector2D — full implementation: force angle, magnitude, uniform direction
 *  ⑥ PulleyJoint2D — two-body pulley with ratio
 *  ⑦ Rigidbody2D.MovePosition / MoveRotation — kinematic motion with implicit velocity
 *  ⑧ CircleCast / BoxCast / CapsuleCast — shape cast queries
 *  ⑨ ContactFilter2D — richer filtering for queries and callbacks
 *  ⑩ Breakable joints with proper impulse accumulation and OnJointBreak dispatch
 *  ⑪ Physics material runtime modification API
 *  ⑫ Overlap queries returning RayHit-style contact info (overlap_capsule, etc.)
 *  ⑬ Physics2D.GetContacts() equivalent — query existing contacts for an entity
 *  ⑭ Rigidbody2D.GetRelativePoint / GetRelativeVector helpers
 *  ⑮ Custom gravity per-body (gravity override vector, not just scale)
 *  ⑯ OnTriggerStay2D / OnCollisionStay2D callbacks (persistent overlap tracking)
 *  ⑰ Linecast — Physics2D.Linecast / LinecastAll (segment-clipped raycast)
 *  ⑱ OverlapPoint — Physics2D.OverlapPoint / OverlapPointAll
 *  ⑲ get_total_force / get_total_torque — Rigidbody2D.totalForce / totalTorque
 *  ⑳ physics_simulate() — Physics2D.Simulate (manual deterministic step)
 *  ㉑ *NonAlloc query variants — OverlapCircleNonAlloc, RaycastNonAlloc, etc.
 *  ㉒ rebuild_composite_collider() — runtime CompositeCollider2D merge
 *  ㉓ Effector priority & layer mask — Effector2D.colliderMask parity
 *  ㉔ Velocity constraints — Rigidbody2D.constraints per-axis freeze
 *  ㉕ get_contact_impulses() — ContactPoint2D.normalImpulse / tangentImpulse
 *  ㉖ set_velocity / set_angular_velocity / clamp_velocity — direct velocity setters
 *  ㉗ set_body_type / get_body_type — runtime BodyType2D switching
 *  ㉘ closest_point_on_collider — Collider2D.ClosestPoint
 *  ㉙ shape_distance — Physics2D.Distance (separation/overlap between two colliders)
 *  ㉚ sweep_test / sweep_test_all — Rigidbody2D.SweepTest
 *  ㉛ sleep thresholds — Physics2D.velocityThreshold / sleepingThreshold
 *  ㉜ ignore_layer_collision — Physics2D.IgnoreLayerCollision per-layer matrix
 *  ㉝ FixedJoint2D solver — rigid / soft weld joint
 *  ㉞ per-entity physics callbacks — OnCollisionEnter/Exit/Stay, OnTrigger* per entity
 *  ㉟ interpolation mode — None / Interpolate / Extrapolate + get_render_position
 *  ㊱ predict_trajectory — projectile/arc trajectory simulation
 *  ㊲ overlap_collider — Collider2D.OverlapCollider
 *  ㊳ attach_collider / detach_collider — runtime collider add/remove
 *  ㊴ physics material combine modes — frictionCombine / bounceCombine runtime API
 *  ㊵ set_inertia / reset_inertia — Rigidbody2D.inertia override
 *  ㊶ ForceMode2D variants — Force / Impulse / VelocityChange / Acceleration
 *  ㊷ CollisionDetectionMode2D — Discrete/Continuous/ContinuousDynamic/ContinuousCollide
 *  ㊸ Physics2D global settings — queriesHitTriggers, queriesStartInColliders,
 *       autoSyncTransforms, maxLinearSpeed, maxAngularSpeed
 *  ㊹ clamp_body_speeds() — enforces configurable per-step linear/angular speed caps
 *  ㊺ Rigidbody2D.Slide — SlideMovement/SlideResults, MovePosition/Linecast/ReuseCollision
 *
 * New in this revision (smooth/realistic physics upgrade):
 *  ㊻ apply_angular_spring() — torsion spring between bodies (like HingeJoint soft limit)
 *  ㊼ apply_drag_field() — localized quadratic drag zone (wind tunnel, water volume)
 *  ㊽ apply_magnus_effect() — spinning-body lift force (realism for thrown balls)
 *  ㊾ predict_trajectory_with_drag() — arc prediction accounting for drag + gravity dir
 *  ㊿ get_coyote_grounded() — coyote-time grounded query using _coyote_t from core
 */

#include "physics.hpp"

namespace phys {

// ════════════════════════════════════════════════════════════════════════════
//  ①  GLOBAL GRAVITY DIRECTION
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Physics2D.gravity = new Vector2(0, -9.81f)
// In Nova coords: positive Y is typically down (pixel space), so default is (0,+1).
// Calling set_gravity_direction(0,-1) flips to Unity-standard "up is negative Y".
void  set_gravity_direction(float gx, float gy);
Vec2  get_gravity_direction();
// Full gravity vector shortcut (sets both direction and magnitude)
void  set_gravity_vector(float gx, float gy);
// Revert to default downward gravity (direction = (0, +gravity_param))
void  reset_gravity_vector();
Vec2  get_gravity_vector();

// ════════════════════════════════════════════════════════════════════════════
//  ②  BUOYANCY EFFECTOR 2D (Unity2D: BuoyancyEffector2D)
// ════════════════════════════════════════════════════════════════════════════
// Component JSON fields on the entity with BuoyancyEffector2D:
//   surface_level   (float) – world-Y of the fluid surface
//   density         (float) – fluid density; higher = more upward force (default 1)
//   linear_drag     (float) – velocity damping inside fluid (default 1)
//   angular_drag    (float) – angular velocity damping inside fluid (default 1)
//   flow_angle      (float) – degrees; direction of current (default 0 = rightward)
//   flow_magnitude  (float) – speed of current (default 0)
void apply_buoyancy_effectors(EntityList& entities, float global_gravity);

// ════════════════════════════════════════════════════════════════════════════
//  ③  CONSTANT FORCE 2D (Unity2D: ConstantForce2D)
// ════════════════════════════════════════════════════════════════════════════
// Component JSON fields on the entity with ConstantForce2D:
//   force_x, force_y   – world-space force applied every physics step
//   relative_force_x, relative_force_y – local-space force (rotated by body angle)
//   torque             – constant torque (Nm)
void apply_constant_force2d(EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  ④  POINT EFFECTOR 2D (Unity2D: PointEffector2D)
// ════════════════════════════════════════════════════════════════════════════
// Component JSON fields on the entity with PointEffector2D:
//   force_magnitude (float) – positive = attract, negative = repel (or vice versa by convention)
//   force_mode      (string) – "constant" | "inverse_linear" | "inverse_square"
//   distance_scale  (float) – scales the distance for fall-off (default 1)
//   force_source    (string) – "collider_center" | "rigidbody_center"
//   angular_drag    (float) – angular drag applied to affected bodies
//   linear_drag     (float)
//   use_collider_mask (bool) – respect layer mask
void apply_point_effectors2(EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  ⑤  AREA EFFECTOR 2D — full implementation
// ════════════════════════════════════════════════════════════════════════════
// Component JSON fields on the entity with AreaEffector2D:
//   force_angle     (float) – direction of applied force in degrees (0=right, 90=up)
//   force_magnitude (float) – magnitude of the force
//   force_variation (float) – random variation added each step (default 0)
//   drag            (float) – linear drag override while inside area
//   angular_drag    (float) – angular drag override while inside area
//   use_global_angle (bool) – if false, force_angle is relative to area's rotation
void apply_area_effectors2(EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  ⑥  PULLEY JOINT 2D
// ════════════════════════════════════════════════════════════════════════════
// PulleyJoint2D component on entity A:
//   connected_entity  (int)   – entity B id
//   ground_anchor_ax/ay (float) – world-space ground anchor for A
//   ground_anchor_bx/by (float) – world-space ground anchor for B
//   ratio             (float) – pulley ratio (default 1)
//   total_length      (float) – total rope length (computed from rest if 0)
// Constraint: distA + ratio * distB = total_length
// (Solved as a 1D velocity constraint)
void solve_pulley_joints(EntityList& entities, float dt,
                         std::unordered_map<int,Entity*>& emap);

// ════════════════════════════════════════════════════════════════════════════
//  ⑦  KINEMATIC MOVE POSITION / MOVE ROTATION
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Rigidbody2D.MovePosition / MoveRotation
// Sets target position/rotation; engine computes implicit velocity so constraints
// and joints are respected during the next physics step.
void move_position(Entity& entity, float wx, float wy, float dt);
void move_rotation(Entity& entity, float target_angle_deg, float dt);

// ════════════════════════════════════════════════════════════════════════════
//  ⑧  SHAPE CASTS
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Physics2D.CircleCast / BoxCast / CapsuleCast
// All return the first hit (or all hits for *_all variants).

struct ShapeCastHit {
    float     distance;
    Vec2      point;       // surface contact point
    Vec2      normal;      // surface normal
    Vec2      centroid;    // shape center at point of contact
    Entity*   entity    = nullptr;
    Entity*   collider  = nullptr;
};

// Circle cast: sweeps a circle of radius r along (dx,dy) from (ox,oy)
std::optional<ShapeCastHit> circle_cast(
    EntityList& entities,
    float ox, float oy, float radius,
    float dx, float dy,
    float distance = 1e9f,
    int layer_mask = 0xFFFF,
    bool query_triggers = false);

std::vector<ShapeCastHit> circle_cast_all(
    EntityList& entities,
    float ox, float oy, float radius,
    float dx, float dy,
    float distance = 1e9f,
    int layer_mask = 0xFFFF,
    bool query_triggers = false);

// Box cast: sweeps an axis-aligned box (w×h) rotated by rot_deg
std::optional<ShapeCastHit> box_cast(
    EntityList& entities,
    float ox, float oy, float w, float h, float rot_deg,
    float dx, float dy,
    float distance = 1e9f,
    int layer_mask = 0xFFFF,
    bool query_triggers = false);

std::vector<ShapeCastHit> box_cast_all(
    EntityList& entities,
    float ox, float oy, float w, float h, float rot_deg,
    float dx, float dy,
    float distance = 1e9f,
    int layer_mask = 0xFFFF,
    bool query_triggers = false);

// Capsule cast: sweeps a capsule (radius, half_h, vertical by default)
std::optional<ShapeCastHit> capsule_cast(
    EntityList& entities,
    float ox, float oy, float radius, float half_h, bool horizontal,
    float dx, float dy,
    float distance = 1e9f,
    int layer_mask = 0xFFFF,
    bool query_triggers = false);

std::vector<ShapeCastHit> capsule_cast_all(
    EntityList& entities,
    float ox, float oy, float radius, float half_h, bool horizontal,
    float dx, float dy,
    float distance = 1e9f,
    int layer_mask = 0xFFFF,
    bool query_triggers = false);

// ════════════════════════════════════════════════════════════════════════════
//  ⑨  CONTACT FILTER 2D
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: ContactFilter2D — controls what contacts are returned by queries.
struct ContactFilter2D {
    bool  use_depth          = false;
    float min_depth          = -1e9f;
    float max_depth          = +1e9f;
    bool  use_normal_angle   = false;
    float min_normal_angle   = 0.f;    // degrees
    float max_normal_angle   = 360.f;
    bool  use_layer_mask     = true;
    int   layer_mask         = 0xFFFF;
    bool  use_triggers       = false;  // if false, skip triggers
    std::function<bool(Entity*)> custom_predicate;

    // Checks whether a given manifold passes this filter
    bool accepts(const Manifold& m) const;
};

// ════════════════════════════════════════════════════════════════════════════
//  ⑩  BREAKABLE JOINTS — proper impulse accumulation
// ════════════════════════════════════════════════════════════════════════════
// The existing check_break stub uses a placeholder "_accumulated_impulse".
// These helpers are called by solve_joints to properly track and trigger breaks.
//
// Usage: call accumulate_joint_impulse() inside joint solvers when an impulse jn
// is applied; check_and_break_joint() then fires OnJointBreak callback if exceeded.

void accumulate_joint_impulse(Entity& joint_entity, float jx, float jy, float j_angular = 0.f);
// Returns true if the joint is now broken (sets _broken=true, fires callback).
bool check_and_break_joint(Entity& joint_entity, float dt,
                           std::function<void(Entity&)> on_break = nullptr);

// Global joint break callback (like Unity's SendMessage "OnJointBreak")
void set_joint_break_listener(std::function<void(Entity* joint_entity, float break_force)> cb);

// ════════════════════════════════════════════════════════════════════════════
//  ⑪  PHYSICS MATERIAL RUNTIME API
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: collider.sharedMaterial / collider.material
void set_collider_material(Entity& entity, const PhysicsMaterial& mat,
                           const std::string& collider_type = "BoxCollider2D");
PhysicsMaterial get_collider_material(const Entity& entity,
                                      const std::string& collider_type = "BoxCollider2D");
// Convenience: change friction/bounciness directly
void set_friction   (Entity& entity, float friction,   const std::string& col = "BoxCollider2D");
void set_bounciness (Entity& entity, float bounciness, const std::string& col = "BoxCollider2D");

// ════════════════════════════════════════════════════════════════════════════
//  ⑫  OVERLAP QUERIES WITH CONTACT INFO
// ════════════════════════════════════════════════════════════════════════════
// Like overlap_circle but also returns contact normals and depths.
struct OverlapHit {
    Entity* entity   = nullptr;
    Entity* collider = nullptr;
    Vec2    normal;          // from query shape into this entity
    float   depth    = 0.f;
};

std::vector<OverlapHit> overlap_circle_ex(EntityList& entities,
                                          float x, float y, float r,
                                          const ContactFilter2D& filter = {});

std::vector<OverlapHit> overlap_box_ex(EntityList& entities,
                                       float x, float y, float w, float h, float rot_deg = 0.f,
                                       const ContactFilter2D& filter = {});

std::vector<OverlapHit> overlap_capsule_ex(EntityList& entities,
                                           float x, float y, float radius, float half_h,
                                           bool horizontal = false,
                                           const ContactFilter2D& filter = {});

// ════════════════════════════════════════════════════════════════════════════
//  ⑬  GET CONTACTS FOR ENTITY  (Physics2D.GetContacts equivalent)
// ════════════════════════════════════════════════════════════════════════════
// Returns all Manifolds from the last physics step that involve this entity.
// Requires that apply_physics_ext() stores manifolds in s_last_manifolds.
const std::vector<Manifold>& get_last_manifolds();
std::vector<const Manifold*> get_contacts(const Entity& entity);
// Number of contacts touching this entity
int get_contact_count(const Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ⑭  RIGIDBODY HELPERS
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Rigidbody2D.GetRelativePoint / GetRelativeVector
// Transforms a world-space point into the body's local space.
Vec2 get_relative_point  (const Entity& entity, float wx, float wy);
// Transforms a local point into world space.
Vec2 get_point           (const Entity& entity, float lx, float ly);
// Transforms a world-space vector into local space (no translation).
Vec2 get_relative_vector (const Entity& entity, float wx, float wy);
// Returns velocity of a world-space point on the body (includes angular contribution).
Vec2 get_point_velocity  (const Entity& entity, float wx, float wy);
// True if the body is currently sleeping.
bool is_sleeping         (const Entity& entity);
// Wake the body (clears sleep flag, like Rigidbody2D.WakeUp()).
void wake_up             (Entity& entity);
// Put body to sleep immediately.
void sleep               (Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ⑮  CUSTOM GRAVITY PER-BODY
// ════════════════════════════════════════════════════════════════════════════
// Override gravity direction+magnitude for a single body (independent of global).
// Set gravity_scale = 0 and use this for full control.
// Stores in rb["_custom_grav_x"] / rb["_custom_grav_y"].
void set_body_gravity(Entity& entity, float gx, float gy);
void clear_body_gravity(Entity& entity);  // reverts to global gravity * gravity_scale

// ════════════════════════════════════════════════════════════════════════════
//  ⑯  TRIGGER STAY / COLLISION STAY CALLBACKS
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: OnTriggerStay2D / OnCollisionStay2D
// The core ContactListener only fires enter/exit.  This extension adds a
// persistent overlap tracker so you can receive per-frame stay callbacks.
//
// Usage:
//   phys::set_trigger_stay_listener([](Entity* a, Entity* b, const Manifold& m){ ... });
//   phys::set_collision_stay_listener([](Entity* a, Entity* b, const Manifold& m){ ... });
//   // Then call apply_physics_ext() — stay callbacks fire automatically.
void set_trigger_stay_listener  (std::function<void(Entity*, Entity*, const Manifold&)> cb);
void set_collision_stay_listener(std::function<void(Entity*, Entity*, const Manifold&)> cb);

// ════════════════════════════════════════════════════════════════════════════
//  ⑰  LINECAST  (Unity2D: Physics2D.Linecast)
// ════════════════════════════════════════════════════════════════════════════
// Casts a line segment from (x1,y1) to (x2,y2) and returns the first hit.
// Equivalent to a raycast clamped to the segment length.
std::optional<RayHit> linecast(
    EntityList& entities,
    float x1, float y1, float x2, float y2,
    int   layer_mask    = 0xFFFF,
    bool  query_triggers = false);

std::vector<RayHit> linecast_all(
    EntityList& entities,
    float x1, float y1, float x2, float y2,
    int   layer_mask    = 0xFFFF,
    bool  query_triggers = false);

// ════════════════════════════════════════════════════════════════════════════
//  ⑱  OVERLAP POINT  (Unity2D: Physics2D.OverlapPoint)
// ════════════════════════════════════════════════════════════════════════════
// Returns all entities whose collider contains the world-space point (px, py).
std::vector<Entity*> overlap_point(
    EntityList& entities,
    float px, float py,
    int   layer_mask    = 0xFFFF,
    bool  query_triggers = true);

// Extended variant returning contact info (depth is 0, normal is zero vector)
std::vector<OverlapHit> overlap_point_ex(
    EntityList& entities,
    float px, float py,
    const ContactFilter2D& filter = {});

// ════════════════════════════════════════════════════════════════════════════
//  ⑲  TOTAL FORCE / TORQUE INSPECTION
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Rigidbody2D.totalForce / Rigidbody2D.totalTorque
// Returns the accumulated force/torque that will be applied on the next step.
// Forces are accumulated into _acc_force_x/_acc_force_y / _acc_torque by
// add_force / add_torque / apply_constant_force2d etc.
Vec2  get_total_force (const Entity& entity);
float get_total_torque(const Entity& entity);

// Reset accumulated force/torque to zero (Unity: Rigidbody2D.totalForce = Vector2.zero)
void reset_forces(Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ⑳  PHYSICS_SIMULATE  (Unity2D: Physics2D.Simulate)
// ════════════════════════════════════════════════════════════════════════════
// Manually advance the physics simulation by dt seconds, independent of the
// game loop.  Useful for deterministic replays, offline computation, or
// per-scene bootstrapping.  Calls apply_physics_ext internally.
void physics_simulate(EntityList& entities, float dt, float gravity = GRAVITY);

// ════════════════════════════════════════════════════════════════════════════
//  ㉑  NON-ALLOC QUERY VARIANTS  (Unity2D: *NonAlloc)
// ════════════════════════════════════════════════════════════════════════════
// Fill caller-provided buffers instead of heap-allocating result vectors.
// Returns the number of results written (capped at buf_size).
//
//  phys::overlap_circle_nonalloc(entities, x,y,r, results, 16);
int overlap_circle_nonalloc(
    EntityList& entities,
    float ox, float oy, float radius,
    Entity** results, int buf_size,
    int layer_mask   = 0xFFFF,
    bool query_triggers = true);

int overlap_box_nonalloc(
    EntityList& entities,
    float ox, float oy, float w, float h, float rot_deg,
    Entity** results, int buf_size,
    int layer_mask   = 0xFFFF,
    bool query_triggers = true);

int raycast_nonalloc(
    EntityList& entities,
    float ox, float oy, float dx, float dy, float distance,
    RayHit* results, int buf_size,
    int layer_mask   = 0xFFFF,
    bool query_triggers = false);

// ════════════════════════════════════════════════════════════════════════════
//  ㉒  COMPOSITE COLLIDER REBUILD API
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: CompositeCollider2D — merges child colliders into one optimised shape.
// After adding/removing child colliders at runtime call rebuild_composite_collider()
// to recompute the merged hull stored in the CompositeCollider2D component.
//
// Component JSON fields on the entity with CompositeCollider2D:
//   geometry_type  (string) – "outlines" (keep each polygon) | "polygons" (convex hull merge)
//   offset_distance (float) – expand/contract the merged outline (default 0)
//   child_collider_types (array of strings) – e.g. ["BoxCollider2D","PolygonCollider2D"]
void rebuild_composite_collider(Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ㉓  EFFECTOR PRIORITY & LAYER MASK
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Effector2D.colliderMask — controls which collider layers are affected.
// We extend each effector component with optional "_effector_priority" (int) and
// "_effector_layer_mask" (int) JSON fields.  When multiple effectors overlap,
// only the one with the highest priority is applied (ties: all are applied).
//
// Call set_effector_priority / set_effector_layer_mask at setup time, or set the
// JSON fields directly in your entity data.
void set_effector_priority  (Entity& entity, const std::string& effector_type, int priority);
void set_effector_layer_mask(Entity& entity, const std::string& effector_type, int mask);

// ════════════════════════════════════════════════════════════════════════════
//  ㉔  VELOCITY CONSTRAINTS  (Unity2D: constraints bitmask)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Rigidbody2D.constraints — freeze individual axes at the velocity level.
// Extends the existing freeze_rotation flag to per-axis freeze.
//
// Flags (combinable with |):
constexpr int FREEZE_NONE         = 0;
constexpr int FREEZE_POS_X        = 1 << 0;
constexpr int FREEZE_POS_Y        = 1 << 1;
constexpr int FREEZE_ROTATION_EXT = 1 << 2;  // alias for rb.freeze_rotation
constexpr int FREEZE_ALL          = FREEZE_POS_X | FREEZE_POS_Y | FREEZE_ROTATION_EXT;

// Apply constraints to a body — call once per frame before integration, or
// let apply_physics_ext() call it automatically.
void apply_constraints(Entity& entity);
// Set constraint flags (stored in rb["_constraints"])
void set_constraints(Entity& entity, int flags);
int  get_constraints(const Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ㉕  CONTACT NORMAL IMPULSE QUERY  (Unity2D: ContactPoint2D.normalImpulse)
// ════════════════════════════════════════════════════════════════════════════
// Returns the normal and tangent impulse magnitudes from the last step for
// each contact point involving this entity.  Data is sourced from the lambda
// values stored in the contact cache by the velocity solver.
struct ContactImpulse {
    Vec2  point;           // world-space contact point
    Vec2  normal;          // collision normal (pointing away from other body)
    float normal_impulse;  // magnitude of normal impulse applied (Ns)
    float tangent_impulse; // magnitude of friction impulse applied (Ns)
    Entity* other;         // the other entity in the contact
};

std::vector<ContactImpulse> get_contact_impulses(const Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ㉖  VELOCITY SETTERS / GETTERS  (Rigidbody2D.velocity, angularVelocity)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: rb.velocity = new Vector2(x,y)  /  rb.angularVelocity = deg/s
void set_velocity        (Entity& entity, float vx, float vy);
void set_angular_velocity(Entity& entity, float deg_per_sec);
float get_angular_velocity(const Entity& entity);
// Clamp velocity to a maximum speed (Unity2D: no direct equiv, but commonly needed)
void clamp_velocity      (Entity& entity, float max_speed);

// ════════════════════════════════════════════════════════════════════════════
//  ㉗  RUNTIME BODY TYPE SWITCHING  (Rigidbody2D.bodyType)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: rb.bodyType = RigidbodyType2D.Kinematic / Dynamic / Static
// Switching to Static zeros velocity and freezes the body.
// Switching from Static/Kinematic to Dynamic restores simulated motion.
enum class BodyType2D { Dynamic, Kinematic, Static };
void      set_body_type(Entity& entity, BodyType2D type);
BodyType2D get_body_type(const Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ㉘  CLOSEST POINT ON COLLIDER  (Collider2D.ClosestPoint)
// ════════════════════════════════════════════════════════════════════════════
// Returns the closest point on the entity's collider surface to the given
// world-space point.  If the point is inside the collider the surface point
// is still returned (not the interior point itself).
Vec2 closest_point_on_collider(const Entity& entity, float wx, float wy);

// ════════════════════════════════════════════════════════════════════════════
//  ㉙  SHAPE DISTANCE  (Physics2D.Distance)
// ════════════════════════════════════════════════════════════════════════════
// Computes the minimum separation/overlap distance between two entities'
// colliders.  Mirrors Unity's Physics2D.Distance(colliderA, colliderB).
struct ShapeDistance {
    float distance;     // negative = overlapping (penetration depth)
    Vec2  point_on_a;   // closest point on entity A's collider
    Vec2  point_on_b;   // closest point on entity B's collider
    Vec2  normal;       // unit vector from A toward B at closest points
    bool  overlapping;  // true when distance < 0
};
ShapeDistance shape_distance(const Entity& entity_a, const Entity& entity_b);

// ════════════════════════════════════════════════════════════════════════════
//  ㉚  SWEEP TEST  (Rigidbody2D.SweepTest / SweepTestAll)
// ════════════════════════════════════════════════════════════════════════════
// Sweeps the entity's own collider shape along (dx,dy) and returns hits.
// Unity2D: rb.SweepTest(direction, out RaycastHit2D hit, distance)
std::optional<ShapeCastHit> sweep_test(
    Entity& entity,
    EntityList& world,
    float dx, float dy,
    float distance = 1e9f,
    int   layer_mask    = 0xFFFF,
    bool  query_triggers = false);

std::vector<ShapeCastHit> sweep_test_all(
    Entity& entity,
    EntityList& world,
    float dx, float dy,
    float distance = 1e9f,
    int   layer_mask    = 0xFFFF,
    bool  query_triggers = false);

// ════════════════════════════════════════════════════════════════════════════
//  ㉛  SLEEP THRESHOLD  (Physics2D.velocityThreshold / sleepingThreshold)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Physics2D.velocityThreshold  (global)
void  set_sleep_velocity_threshold(float pixels_per_sec);
float get_sleep_velocity_threshold();
void  set_sleep_angular_threshold (float deg_per_sec);
float get_sleep_angular_threshold ();
void  set_sleep_time_threshold    (float seconds);
float get_sleep_time_threshold    ();

// ════════════════════════════════════════════════════════════════════════════
//  ㉜  PER-LAYER COLLISION IGNORE  (Physics2D.IgnoreLayerCollision)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Physics2D.IgnoreLayerCollision(layerA, layerB, ignore)
//          Physics2D.GetIgnoreLayerCollision(layerA, layerB)
// Layers are 0-31 (integer index).  The collision matrix in physics.cpp
// handles entity-pair ignores; this API adds per-layer ignores on top.
void ignore_layer_collision(int layer_a, int layer_b, bool ignore = true);
bool get_ignore_layer_collision(int layer_a, int layer_b);

// ════════════════════════════════════════════════════════════════════════════
//  ㉝  FIXED JOINT 2D  (Unity2D: FixedJoint2D)
// ════════════════════════════════════════════════════════════════════════════
// Welds two bodies at their current relative transform (position + angle).
// Component JSON fields on the entity with FixedJoint2D:
//   connected_entity (int) – id of the other body
//   damping_ratio   (float) – 0..1, higher = more damping (default 0)
//   frequency       (float) – Hz of the soft constraint (0 = rigid, default 0)
//   break_force     (float) – break threshold (default infinity)
// A frequency > 0 makes it a soft weld (spring-damper); frequency = 0 is rigid.
void solve_fixed_joints(EntityList& entities, float dt,
                        std::unordered_map<int,Entity*>& emap);

// ════════════════════════════════════════════════════════════════════════════
//  ㉞  PER-ENTITY PHYSICS CALLBACKS  (Unity2D: MonoBehaviour callbacks)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D raises OnCollisionEnter2D / OnCollisionExit2D / OnCollisionStay2D /
// OnTriggerEnter2D / OnTriggerExit2D / OnTriggerStay2D on each involved
// MonoBehaviour.  The global ContactListener fires once per pair; this
// system lets each entity register its own handlers.
//
// Usage:
//   phys::register_collision_enter(my_entity, [](Entity* self, Entity* other, const Manifold& m){ ... });
using PhysicsCallback = std::function<void(Entity* self, Entity* other, const Manifold&)>;
void register_collision_enter(Entity& entity, PhysicsCallback cb);
void register_collision_exit (Entity& entity, PhysicsCallback cb);
void register_collision_stay (Entity& entity, PhysicsCallback cb);
void register_trigger_enter  (Entity& entity, PhysicsCallback cb);
void register_trigger_exit   (Entity& entity, PhysicsCallback cb);
void register_trigger_stay   (Entity& entity, PhysicsCallback cb);
// Remove all callbacks for an entity (e.g. on entity destroy)
void unregister_physics_callbacks(Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ㉟  INTERPOLATION MODE  (Rigidbody2D.interpolation)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: RigidbodyInterpolation2D { None, Interpolate, Extrapolate }
// "Interpolate" smooths between last and current physics positions.
// "Extrapolate" predicts ahead using current velocity.
enum class InterpolationMode { None, Interpolate, Extrapolate };
void             set_interpolation_mode(Entity& entity, InterpolationMode mode);
InterpolationMode get_interpolation_mode(const Entity& entity);
// Returns the render-frame position given 0..1 alpha (fraction through physics step).
// For Extrapolate mode, alpha may be > 1 (time since last step / step_dt).
Vec2  get_render_position(const Entity& entity, float alpha, float step_dt = 0.f);
float get_render_rotation(const Entity& entity, float alpha, float step_dt = 0.f);

// ════════════════════════════════════════════════════════════════════════════
//  ㊱  TRAJECTORY PREDICTION  (no direct Unity2D equiv; commonly built on top)
// ════════════════════════════════════════════════════════════════════════════
// Simulates the entity's trajectory for `steps` physics steps of size `step_dt`
// without modifying any entity state.  Returns a list of (position, velocity)
// snapshots — useful for drawing aim/throw arcs.
struct TrajectoryPoint {
    Vec2  position;
    Vec2  velocity;
    float time;       // seconds from now
};
std::vector<TrajectoryPoint> predict_trajectory(
    const Entity& entity,
    int   steps,
    float step_dt,
    float gravity = GRAVITY);

// ════════════════════════════════════════════════════════════════════════════
//  ㊲  OVERLAP COLLIDER  (Collider2D.OverlapCollider)
// ════════════════════════════════════════════════════════════════════════════
// Returns all entities whose colliders overlap with this entity's collider.
// Unity2D: collider.OverlapCollider(filter, results)
std::vector<OverlapHit> overlap_collider(
    Entity& entity,
    EntityList& world,
    const ContactFilter2D& filter = {});

int overlap_collider_nonalloc(
    Entity& entity,
    EntityList& world,
    Entity** results, int buf_size,
    const ContactFilter2D& filter = {});

// ════════════════════════════════════════════════════════════════════════════
//  ㊳  RUNTIME COLLIDER ATTACH / DETACH
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Rigidbody2D.attachedColliderCount / Rigidbody2D (collider parenting)
// Adds or removes a collider component by type string at runtime.
// attach_collider adds the named component key with the given JSON data.
// detach_collider removes it and wakes the body.
void attach_collider(Entity& entity, const std::string& collider_type,
                     const Entity& collider_data);
void detach_collider(Entity& entity, const std::string& collider_type);
int  get_attached_collider_count(const Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ㊴  PHYSICS MATERIAL COMBINE MODES  (PhysicsMaterial2D.combine)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: PhysicsMaterial2D with frictionCombine / bounceCombine
// (Average, Minimum, Multiply, Maximum)
// The core physics.cpp already has CombineMode internally; this exposes it.
enum class CombineMode2D { Average, Minimum, Multiply, Maximum };
void set_friction_combine  (Entity& entity, CombineMode2D mode,
                             const std::string& col = "BoxCollider2D");
void set_bounciness_combine(Entity& entity, CombineMode2D mode,
                             const std::string& col = "BoxCollider2D");

// ════════════════════════════════════════════════════════════════════════════
//  ㊵  RIGIDBODY INERTIA TENSOR OVERRIDE  (Rigidbody2D.inertia)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: rb.inertia = value  (override computed inertia)
//          rb.ResetInertiaTensor() (recompute from geometry)
void  set_inertia      (Entity& entity, float inertia);
float get_inertia      (const Entity& entity);
void  reset_inertia    (Entity& entity);  // recomputes from geometry via auto_compute_mass

// ════════════════════════════════════════════════════════════════════════════
//  ㊶  FORCE MODE VARIANTS  (ForceMode2D.Force / Impulse / VelocityChange / Acceleration)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: rb.AddForce(v, ForceMode2D.Impulse)
enum class ForceMode2D { Force, Impulse, VelocityChange, Acceleration };
void add_force_mode    (Entity& entity, float fx, float fy,   ForceMode2D mode, float dt = 1.f/60.f);
void add_force_at_mode (Entity& entity, float fx, float fy,
                        float wx, float wy, ForceMode2D mode, float dt = 1.f/60.f);

// ════════════════════════════════════════════════════════════════════════════
//  ㊷  COLLISION DETECTION MODE  (Unity2D: Rigidbody2D.collisionDetectionMode)
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: CollisionDetectionMode2D { Discrete, Continuous, ContinuousDynamic, ContinuousCollide }
// Any Continuous mode enables speculative CCD for this body (the core apply_ccd()
// already keys off the "continuous_collision"/"ccd" flags we set here), preventing
// tunnelling through thin colliders at high speed.  Discrete turns it back off.
//
//   ContinuousCollide    — CCD against all bodies (static + dynamic); most accurate
//   ContinuousDynamic    — CCD against dynamic bodies too (Unity default for fast movers)
//   Continuous           — CCD against statics/kinematics only
//   Discrete             — no CCD (cheapest; default)
enum class CollisionDetectionMode2D { Discrete, Continuous, ContinuousDynamic, ContinuousCollide };
void                     set_collision_detection_mode(Entity& entity, CollisionDetectionMode2D mode);
CollisionDetectionMode2D get_collision_detection_mode(const Entity& entity);

// ════════════════════════════════════════════════════════════════════════════
//  ㊸  PHYSICS2D GLOBAL SETTINGS
// ════════════════════════════════════════════════════════════════════════════
// Unity2D: Physics2D.queriesHitTriggers, Physics2D.queriesStartInColliders,
//          Physics2D.autoSyncTransforms, Physics2D.maxLinearSpeed, maxAngularSpeed
//
// queriesHitTriggers      — default value for the "query_triggers" param of ext-layer
//                           casts/overlaps when the caller doesn't override it.
// queriesStartInColliders — when false, casts/overlaps that originate inside a
//                           collider ignore that starting collider (Unity parity).
// autoSyncTransforms      — informational flag (syncs Transform changes before the
//                           physics step); stored+queryable, honoured by Slide.
// maxLinearSpeed/maxAngularSpeed — enforced each step by clamp_body_speeds(), which
//                           apply_physics_ext() runs automatically.  Set to <= 0 to
//                           disable the cap (engine uses its internal default).
void  set_queries_hit_triggers      (bool v);
bool  get_queries_hit_triggers      ();
void  set_queries_start_in_colliders(bool v);
bool  get_queries_start_in_colliders();
void  set_auto_sync_transforms      (bool v);
bool  get_auto_sync_transforms      ();
void  set_max_linear_speed          (float pixels_per_sec);  // <= 0 = off
float get_max_linear_speed          ();
void  set_max_angular_speed         (float deg_per_sec);      // <= 0 = off
float get_max_angular_speed         ();

// ════════════════════════════════════════════════════════════════════════════
//  ㊹  CLAMP BODY SPEEDS  (Unity2D: Physics2D.maxLinearSpeed / maxAngularSpeed)
// ════════════════════════════════════════════════════════════════════════════
// Clamps every dynamic body's linear speed to set_max_linear_speed() and angular
// speed to set_max_angular_speed().  Called automatically each step inside
// apply_physics_ext(); you may also call it manually (e.g. after setting velocities).
void clamp_body_speeds(EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  ㊺  RIGIDBODY2D.SLIDE  (Unity2D 2022.1: Rigidbody2D.Slide)
// ════════════════════════════════════════════════════════════════════════════
// Moves a body by `slide_move` while sliding along surfaces it meets.  This is an
// EXPLICIT API (call it from game code like move_position); it is not run inside
// apply_physics_ext().  Honours the global queries_hit_triggers / queries_start_in_colliders
// settings (㊸).
//
// Movement types (Unity SlideMovement.movementType):
//   MovePosition  — discretely advance the body and resolve overlaps by pushing
//                   out along the contact normal, sliding the remainder tangentially.
//   Linecast      — sweep (binary-search TOI) from current to target; stop at the
//                   first contact and slide the leftover distance along the surface.
//   ReuseCollision— reuse the body's existing contacts (get_contacts) as the slide
//                   surface; no new casts — the fastest path.
//
// surface_angle / surface_up_angle define the maximum "walkable" slope: surfaces
// steeper than surface_angle (measured from the up direction) are treated as walls
// and stop the slide along their normal rather than letting the body climb them.
enum class SlideMovementType { MovePosition, Linecast, ReuseCollision };

struct SlideMovement {
    Vec2  gravity           = {0.f, 0.f};   // extra gravity folded into slide_move
    float gravity_scale     = 1.f;
    float surface_up_angle  = 90.f;         // degrees; reference "up" direction
    float surface_angle     = 90.f;         // max slope (deg from up) the body will walk
    SlideMovementType movement_type = SlideMovementType::MovePosition;
};

struct SlideResults {
    Vec2    distance_moved         = {0.f, 0.f};   // actual displacement achieved
    Vec2    surface_hit_point      = {0.f, 0.f};   // world point on the first blocking surface
    Entity* surface_hit_collider   = nullptr;      // collider component (or nullptr)
    Entity* surface_hit_entity     = nullptr;      // entity owning that collider
};

SlideResults slide(Entity& entity, EntityList& world, Vec2 slide_move,
                   const SlideMovement& movement = {},
                   const std::vector<Entity*>& ignore_colliders = {});

// Non-alloc variant: writes a single SlideResults into the caller's buffer.
int slide_nonalloc(Entity& entity, EntityList& world, Vec2 slide_move,
                   SlideResults* results,
                   const SlideMovement& movement = {},
                   const std::vector<Entity*>& ignore_colliders = {});

// ════════════════════════════════════════════════════════════════════════════
//  ㊻  ANGULAR SPRING  (torsion spring / soft hinge limit)
// ════════════════════════════════════════════════════════════════════════════
// Applies a restoring torque to pull body toward a target angle. Useful for:
//   - Soft joint angle limits (avoids hard constraint popping)
//   - Pendulum / balance mechanics
//   - Ragdoll limb orientation
// target_deg: world-space target rotation in degrees
// stiffness:  torque per radian of deviation (N·m/rad equivalent)
// damping:    angular velocity damping coefficient
void apply_angular_spring(Entity& entity, float target_deg,
                          float stiffness = 100.f, float damping = 5.f,
                          float max_torque = 1e5f);

// ════════════════════════════════════════════════════════════════════════════
//  ㊼  DRAG FIELD  (localized quadratic drag zone)
// ════════════════════════════════════════════════════════════════════════════
// Applies velocity-squared drag inside a circular region. Simulates wind
// tunnels, underwater zones, thick atmosphere, etc.
// cx/cy: world center of field
// radius: effective radius
// drag_coeff: F = -drag_coeff * |v|² * v̂   (like Unity's drag but spatial)
// angular_drag: additional rotational damping inside zone
void apply_drag_field(EntityList& entities, float cx, float cy, float radius,
                      float drag_coeff = 0.002f, float angular_drag = 0.5f);

// ════════════════════════════════════════════════════════════════════════════
//  ㊽  MAGNUS EFFECT  (spinning-body lift)
// ════════════════════════════════════════════════════════════════════════════
// For a spinning body (angular_velocity ≠ 0) moving through the air,
// applies a lift force perpendicular to velocity (the Magnus force).
// This makes thrown spinning balls curve realistically.
// magnus_coeff: F = magnus_coeff * omega × v  (scalar coefficient)
void apply_magnus_effect(EntityList& entities, float magnus_coeff = 0.005f);

// ════════════════════════════════════════════════════════════════════════════
//  ㊾  TRAJECTORY PREDICTION WITH DRAG
// ════════════════════════════════════════════════════════════════════════════
// Extends predict_trajectory to account for linear drag and global gravity
// direction. Returns a list of world-space points along the projected arc.
// drag: linear drag coefficient (same as Rigidbody2D.drag)
// steps: number of sample points
// sub_dt: simulation time step per sample
std::vector<TrajectoryPoint> predict_trajectory_with_drag(
    Vec2 start_pos, Vec2 start_vel,
    float gravity, float drag = 0.05f,
    int steps = 60, float sub_dt = 1.f/60.f);

// ════════════════════════════════════════════════════════════════════════════
//  ㊿  COYOTE-TIME GROUNDED QUERY
// ════════════════════════════════════════════════════════════════════════════
// Returns true if entity is either grounded now OR within its coyote window
// (i.e. left ground very recently — useful for forgiving jump detection).
// coyote_window: override coyote duration; <= 0 uses COYOTE_TIME constant
bool get_coyote_grounded(const Entity& entity, float coyote_window = -1.f);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ①  VELOCITY DAMPING FIELD  (Stokes + turbulent drag zone)
// ════════════════════════════════════════════════════════════════════════════
// Component JSON: VelocityDampingField2D on an entity with a collider.
//   linear_coeff / quadratic_coeff — drag force coefficients
//   flow_x, flow_y — ambient flow velocity (wind/current)
//   density        — fluid density scale
// Call once per frame, or add to apply_physics_ext via set_custom_pre_step.
void apply_velocity_damping_fields(EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ②  SOFT BODY 2D  (mass-spring network)
// ════════════════════════════════════════════════════════════════════════════
// SoftBodyLink2D component: links two entities (particles) with a spring.
//   entity_a / entity_b — particle entity IDs
//   rest_length         — equilibrium distance (0 = auto on first frame)
//   stiffness           — spring constant (default 300)
//   damping             — velocity damping along spring axis (default 10)
//   break_length        — auto-break distance (0 = never)
void apply_soft_body_springs(EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ③  EXTRA DEPENETRATION PASS  (post-step overlap correction)
// ════════════════════════════════════════════════════════════════════════════
// A gentle velocity push-out for overlapping dynamic bodies that were missed
// by the main solver. Call after apply_physics_ext() for extra stability.
// max_correction: max speed injected per frame (pixels/s, default 200)
void apply_depenetration_pass(EntityList& entities, float dt, float max_correction = 200.f);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ④  RESTITUTION SMOOTHING  (no more micro-bounce at threshold)
// ════════════════════════════════════════════════════════════════════════════
void set_restitution_blend(bool enabled, float blend_range = 20.f);
bool get_restitution_blend();
float compute_smooth_restitution(float raw_restitution, float rel_normal_vel);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ⑤  TERRAIN NORMAL SMOOTHING  (anti-jitter on tiled surfaces)
// ════════════════════════════════════════════════════════════════════════════
// Blend contact normals of co-planar contacts from tilemap/edge chains to
// remove the "staircase" artifact at tile corners.
// Called automatically by apply_physics_ext; exposed for manual use.
void smooth_contact_normals(std::vector<Manifold>& manifolds);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ⑥  SHAPE-AWARE DRAG  (cross-section estimated drag)
// ════════════════════════════════════════════════════════════════════════════
// Enable per-body with rb["shape_drag"] = true.
// Optional: rb["drag_coeff"] = 0.47 (Cd), default is sphere value.
// air_density: 0.001225 for air, higher for water/fluid.
void apply_shape_aware_drag(EntityList& entities, float air_density = 0.001225f);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ⑦  VERLET INTEGRATION MODE  (energy-conserving spring bodies)
// ════════════════════════════════════════════════════════════════════════════
// Enable per-body with rb["use_verlet"] = true.
// Best for soft bodies, pendulums, cloth. Stores _prev_x/_prev_y automatically.
void apply_verlet_bodies(EntityList& entities, float dt, float gravity = GRAVITY);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ⑧  GRAVITY WELL  (inverse-square attraction / repulsion)
// ════════════════════════════════════════════════════════════════════════════
// Component JSON: GravityWell2D
//   mass, G, min_radius, max_radius, repulsive
void apply_gravity_wells(EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ⑨  IMPACT SOUND EVENTS  (velocity-threshold collision audio hook)
// ════════════════════════════════════════════════════════════════════════════
// Set a callback to fire when a collision exceeds a force threshold.
// threshold: minimum accumulated normal impulse to trigger (default 50)
void set_impact_event_listener(std::function<void(Entity*, Entity*, float)> cb);
void dispatch_impact_events(const std::vector<Manifold>& manifolds, float threshold = 50.f);

// ════════════════════════════════════════════════════════════════════════════
//  NEW ⑩  EXPLOSION RING  (expanding shockwave force)
// ════════════════════════════════════════════════════════════════════════════
// Registers a ring shockwave and applies it to bodies as it expands.
//   expand_speed  — ring expansion rate (pixels/s)
//   peak_force    — force applied at ring centre (impulse)
//   lifetime      — seconds the ring lives
//   ring_width    — radial thickness of the force band (0 = auto)
//   layer_mask    — which layers to affect (default 0xFFFF)
void add_explosion_ring(float cx, float cy, float expand_speed, float peak_force,
                        float lifetime, float ring_width = 0.f, int layer_mask = 0xFFFF);
void update_explosion_rings(EntityList& entities, float dt);
void clear_explosion_rings();
int  get_explosion_ring_count();

// ════════════════════════════════════════════════════════════════════════════
//  RUNTIME PHYSICS SETTINGS  (Physics2D global tunable equivalents)
// ════════════════════════════════════════════════════════════════════════════
void  set_baumgarte_scale(float v);
float get_baumgarte_scale();
void  set_baumgarte_toi_scale(float v);
float get_baumgarte_toi_scale();
void  set_default_contact_offset(float v);
float get_default_contact_offset();

// ════════════════════════════════════════════════════════════════════════════
//  NAMED LAYER REGISTRY  (LayerMask.NameToLayer / LayerToName)
// ════════════════════════════════════════════════════════════════════════════
void        set_layer_name(int layer_index, const std::string& name);
int         layer_name_to_index(const std::string& name);
std::string layer_index_to_name(int layer_index);
int         layer_mask_from_names(const std::vector<std::string>& names);

// ════════════════════════════════════════════════════════════════════════════
//  SHARED PHYSICSMATERIAL2D
// ════════════════════════════════════════════════════════════════════════════
struct PhysicsMaterial2D {
    float friction        = 0.4f;
    float bounciness      = 0.f;
    float static_friction  = 0.4f;
    float kinetic_friction = 0.4f;
    std::string friction_combine = "average";
    std::string bounce_combine   = "average";
};
void register_physics_material(const std::string& name, const PhysicsMaterial2D& mat);
bool get_physics_material(const std::string& name, PhysicsMaterial2D& out);
void update_physics_material(const std::string& name, const PhysicsMaterial2D& mat);
void sync_shared_material(EntityList& entities, const std::string& name);

// ════════════════════════════════════════════════════════════════════════════
//  QUERY VARIANTS
// ════════════════════════════════════════════════════════════════════════════
// overlap_capsule — plain variant (no contact info), matches Unity Physics2D.OverlapCapsule
std::vector<Entity*> overlap_capsule(EntityList& entities,
                                     float cx, float cy,
                                     float radius, float half_height,
                                     bool horizontal,
                                     int layer_mask = 0xFFFF,
                                     bool query_triggers = false);
// OverlapArea — Physics2D.OverlapArea(pointA, pointB): AABB by two corners
std::vector<Entity*> overlap_area(EntityList& entities,
                                  float ax, float ay, float bx, float by,
                                  int layer_mask = 0xFFFF,
                                  bool query_triggers = false);
std::vector<Entity*> overlap_area_all(EntityList& entities,
                                      float ax, float ay, float bx, float by,
                                      int layer_mask = 0xFFFF,
                                      bool query_triggers = false);
// queriesStartInColliders enforcement helper
bool should_skip_start_in_collider(float ox, float oy, const Shape& sh);

// ════════════════════════════════════════════════════════════════════════════
//  COLLIDER / JOINT UTILITIES
// ════════════════════════════════════════════════════════════════════════════
std::vector<Entity*> get_attached_colliders(Entity& entity);
int                  get_attached_collider_count(Entity& entity);
void                 custom_collider_set_shapes(
                         Entity& entity,
                         const std::vector<std::vector<std::pair<float,float>>>& polygons);
void                 custom_collider_clear_shapes(Entity& entity);
void                 auto_configure_connected_anchor(Entity& entity,
                                                     const std::string& joint_type);
void                 apply_wheel_lateral_friction(
                         EntityList& entities, float dt,
                         std::unordered_map<int,Entity*>& emap);
void                 apply_platform_side_friction(
                         std::vector<Manifold>& manifolds, EntityList& entities);

// ════════════════════════════════════════════════════════════════════════════
//  PER-ENTITY CONTACT INDEX  (O(1) lookup)
// ════════════════════════════════════════════════════════════════════════════
std::vector<Manifold*> get_contacts_fast(int entity_id);
int                    get_contact_count_fast(int entity_id);

// ════════════════════════════════════════════════════════════════════════════
//  DEBUG DRAW
// ════════════════════════════════════════════════════════════════════════════
struct DebugDrawCallbacks {
    std::function<void(float x1,float y1,float x2,float y2,
                       float r,float g,float b,float a)> draw_line;
    std::function<void(float cx,float cy,float radius,
                       float r,float g,float b,float a)> draw_circle;
    std::function<void(float x,float y,const std::string& text,
                       float r,float g,float b,float a)> draw_text;
};
void set_debug_draw_callbacks(const DebugDrawCallbacks& cbs);
void draw_debug_physics(EntityList& entities,
                        const std::vector<Manifold>& manifolds,
                        const DebugDrawOptions& opts);

// Drop-in replacement for apply_physics() that additionally runs:
//   apply_constant_force2d, apply_buoyancy_effectors, apply_point_effectors2,
//   apply_area_effectors2, solve_pulley_joints, solve_fixed_joints,
//   check joint breaks, trigger/collision stay callbacks, per-entity callbacks,
//   constraint enforcement, layer-collision ignore filtering,
//   apply_magnus_effect (if enabled via set_magnus_enabled).
// Call this instead of apply_physics() to get all new features.
void set_magnus_enabled(bool enabled);
void apply_physics_ext(EntityList& entities, float dt, float gravity = GRAVITY);

} // namespace phys