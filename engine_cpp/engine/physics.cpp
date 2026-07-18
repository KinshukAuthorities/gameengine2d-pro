/*
 * physics.cpp — Nova Engine Physics (Unity2D / Box2D parity upgrade)
 *
 * Major improvements over the previous version:
 *  ① Box2D-style face-clip contact generation  → 2 contacts per pair (was 1)
 *  ② Contact warm-starting                      → faster convergence, no jitter
 *  ③ True capsule narrow phase                  → segment-circle math (not polygon approx)
 *  ④ Speculative CCD                            → tunnelling prevention for fast bodies
 *  ⑤ add_force_at_point / add_impulse_at_point → correct off-centre torque
 *  ⑥ Proper mass/inertia from geometry          → box uses I = m(w²+h²)/12, circle = mr²/2
 *  ⑦ HingeJoint2D with limits + motor
 *  ⑧ SliderJoint2D (prismatic) with limits + motor
 *  ⑨ WheelJoint2D (suspension + motor)
 *  ⑩ CollisionMatrix (32-layer ignore grid)
 *  ⑪ Gravity zones (AreaEffector2D gravity override)
 *  ⑫ Angle-based one-way platform (not just velocity sign)
 *  ⑬ Per-body sleep (not only island sleep)
 *  ⑭ Contact exit dispatch (entity recovery from contact cache)
 *  ⑮ raycast_all()  · add_angular_impulse()
 *  ⑯ _acc_force_x/y / _acc_torque accumulators for Rigidbody2D.totalForce API
 *
 * See physics_ext.hpp / physics_ext.cpp for additional Unity2D gap-closing features:
 *  OnTriggerStay2D, OnCollisionStay2D, linecast, overlap_point, physics_simulate,
 *  *NonAlloc query variants, rebuild_composite_collider, effector priority/mask,
 *  velocity constraints (freeze axes), get_contact_impulses(), and more.
 */

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdint>
#include "physics.hpp"
#include <cassert>
#include <deque>
#include <numeric>
#include <stdexcept>
#include <iostream>
#include <limits>

namespace phys {

// Implemented in physics_ext.cpp for standalone exports. Editor and
// hot-reload script DLLs deliberately compile physics.cpp without that extra
// translation unit, so they retain the small fallback implementations below.
// This is a target-level choice -- including physics_ext.hpp in physics.cpp
// cannot express whether physics_ext.cpp is actually linked.
float compute_smooth_restitution(float raw_restitution, float rel_normal_vel);
bool get_queries_start_in_colliders();

#ifndef PHYSICS_EXT_LINKED
static bool  s_restitution_blend_enabled_fb = false;
static float s_restitution_blend_range_fb   = 20.f;
float compute_smooth_restitution(float raw_restitution, float rel_normal_vel) {
    if (!s_restitution_blend_enabled_fb) return raw_restitution;
    float v  = std::abs(rel_normal_vel);
    float lo = RESTITUTION_THRESH;
    float hi = lo + s_restitution_blend_range_fb;
    if (v <= lo) return 0.f;
    if (v >= hi) return raw_restitution;
    float t = (v - lo) / (hi - lo);
    float s = t * t * t * (t * (t * 6.f - 15.f) + 10.f);
    return raw_restitution * s;
}
static bool s_queries_start_in_colliders_fb = true;
bool get_queries_start_in_colliders() { return s_queries_start_in_colliders_fb; }
#endif // PHYSICS_EXT_LINKED

// ════════════════════════════════════════════════════════════════════════════
//  0.  GLOBALS
// ════════════════════════════════════════════════════════════════════════════
static ContactCache  s_cache;
static CollisionMatrix s_col_matrix;
static ContactListener s_contact_listener;
static EntityList* s_active_entities = nullptr;

// Interpolation state
static std::unordered_map<int, std::pair<Vec2, float>> s_prev_state; // prev_pos, prev_rot

static void prune_interpolation_state(const EntityList& entities) {
    std::unordered_set<int> live_transform_ids;
    live_transform_ids.reserve(entities.size());
    for (const auto& e : entities) {
        if (entity_active(e) && has_component(e, "Transform"))
            live_transform_ids.insert(e.value("id", 0));
    }
    for (auto it = s_prev_state.begin(); it != s_prev_state.end();) {
        if (!live_transform_ids.count(it->first)) it = s_prev_state.erase(it);
        else ++it;
    }
}

// Debug draw options
static DebugDrawOptions s_debug_draw_opts;

// Physics stats
static PhysicsStats s_physics_stats;

// Collision ignore pairs
static std::unordered_set<long long> s_collision_ignores;

static long long entity_pair_key(int a, int b) {
    const int lo = std::min(a, b);
    const int hi = std::max(a, b);
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(lo)) << 32) |
        static_cast<std::uint32_t>(hi);
    return static_cast<long long>(packed);
}
// Per-collider ignore pairs (Phase 3): keyed on (entity_id_a, sub_id_a,
// entity_id_b, sub_id_b) with the lower (entity_id,sub_id) pair ordered
// first so insertion/lookup are symmetric regardless of argument order.
// sub_id of -1 stored literally means "this exact entry only fires for
// wildcard queries" — see is_collider_ignored for how -1 is matched.
struct ColliderPairKey { int e1,s1,e2,s2; };
struct ColliderPairKeyHash {
    size_t operator()(const ColliderPairKey& k) const {
        size_t h=1469598103934665603ull;
        auto mix=[&](long long v){ h^=(size_t)v; h*=1099511628211ull; };
        mix(k.e1); mix(k.s1); mix(k.e2); mix(k.s2);
        return h;
    }
};
struct ColliderPairKeyEq {
    bool operator()(const ColliderPairKey& a, const ColliderPairKey& b) const {
        return a.e1==b.e1 && a.s1==b.s1 && a.e2==b.e2 && a.s2==b.s2;
    }
};
static std::unordered_set<ColliderPairKey,ColliderPairKeyHash,ColliderPairKeyEq> s_collider_ignores;

// IgnoreCollision data is keyed by entity IDs, not owning pointers. Prune
// entries whose participants no longer exist so temporary projectiles cannot
// make the global lookup tables grow for the lifetime of the process.
static void prune_collision_ignores(const EntityList& entities) {
    std::unordered_set<int> live_ids;
    live_ids.reserve(entities.size());
    for (const auto& e : entities) {
        if (!e.value("_destroyed", false)) live_ids.insert(e.value("id", 0));
    }
    for (auto it = s_collision_ignores.begin(); it != s_collision_ignores.end();) {
        const unsigned long long packed = static_cast<unsigned long long>(*it);
        const int lo = static_cast<int>(static_cast<int32_t>(packed >> 32));
        const int hi = static_cast<int>(static_cast<uint32_t>(packed));
        if (!live_ids.count(lo) || !live_ids.count(hi)) it = s_collision_ignores.erase(it);
        else ++it;
    }
    for (auto it = s_collider_ignores.begin(); it != s_collider_ignores.end();) {
        if (!live_ids.count(it->e1) || !live_ids.count(it->e2))
            it = s_collider_ignores.erase(it);
        else ++it;
    }
}

// Physics time scale
static float s_physics_time_scale = 1.f;

// ── Fixed timestep accumulator (Phase 1 fix) ────────────────────────────────
// TARGET_STEP is a compile-time default; s_fixed_dt is the runtime override.
// Call set_fixed_timestep(dt) to change the simulation rate at runtime.
// apply_physics_accumulated() drains the accumulator in fixed-size steps and
// returns the render interpolation alpha automatically.
static float s_fixed_dt       = TARGET_STEP;  // runtime-settable fixed step
static float s_accumulator    = 0.f;           // carry-over from last frame
static float s_render_alpha   = 0.f;           // last computed interpolation alpha

void set_fixed_timestep(float dt) {
    s_fixed_dt = std::max(dt, 1e-4f);  // floor at 0.1 ms to prevent infinite loops
}
float get_fixed_timestep()  { return s_fixed_dt; }
float get_render_alpha()    { return s_render_alpha; }

// Gravity direction override — set by physics_ext set_gravity_vector(); (0,0) = use default downward
// When non-zero, the integrator uses this normalised direction × magnitude instead of purely downward.
// We store the full vector (direction × magnitude) to avoid a second lookup.
static float s_gravity_override_x = 0.f;
static float s_gravity_override_y = 0.f;  // 0 = not set; integrator checks magnitude
static bool  s_gravity_override_active = false;

void phys_set_gravity_override(float gx, float gy, bool active) {
    s_gravity_override_x = gx;
    s_gravity_override_y = gy;
    s_gravity_override_active = active;
}

// Forward declarations for internal helpers called from apply_physics.
static void collect_ground_info(EntityList& entities, const std::vector<Manifold>& mfds, float dt = 1.f/60.f);
void apply_constant_forces(EntityList& entities);
void apply_buoyancy(EntityList& entities);
void apply_point_effectors(EntityList& entities);

CollisionMatrix& global_collision_matrix() { return s_col_matrix; }
void set_contact_listener(const ContactListener& listener) { s_contact_listener = listener; }
ContactListener& get_contact_listener() { return s_contact_listener; }
void set_debug_draw_options(const DebugDrawOptions& opts) { s_debug_draw_opts = opts; }
DebugDrawOptions& get_debug_draw_options() { return s_debug_draw_opts; }
PhysicsStats get_physics_stats() { return s_physics_stats; }
void reset_physics_stats() { s_physics_stats = PhysicsStats(); }

// Collision ignore pairs
void ignore_collision(int entity_id_a, int entity_id_b, bool ignore) {
    if (entity_id_a == entity_id_b) return;
    const long long key = entity_pair_key(entity_id_a, entity_id_b);
    if (ignore) s_collision_ignores.insert(key);
    else s_collision_ignores.erase(key);
}

bool is_collision_ignored(int entity_id_a, int entity_id_b) {
    if (entity_id_a == entity_id_b) return false;
    const long long key = entity_pair_key(entity_id_a, entity_id_b);
    return s_collision_ignores.count(key) > 0;
}

void clear_collision_ignores() { s_collision_ignores.clear(); }

// Per-collider ignore: normalizes ordering so (eA,sA,eB,sB) and (eB,sB,eA,sA)
// hash identically, by sorting on (entity_id, sub_id) pairs.
static ColliderPairKey normalize_collider_key(int e1,int s1,int e2,int s2){
    if (std::tie(e1,s1) > std::tie(e2,s2)) { std::swap(e1,e2); std::swap(s1,s2); }
    return {e1,s1,e2,s2};
}
void ignore_collider(int entity_id_a, int sub_id_a, int entity_id_b, int sub_id_b, bool ignore) {
    if (entity_id_a==entity_id_b && sub_id_a==sub_id_b) return;
    auto key = normalize_collider_key(entity_id_a, sub_id_a, entity_id_b, sub_id_b);
    if (ignore) s_collider_ignores.insert(key);
    else s_collider_ignores.erase(key);
}
bool is_collider_ignored(int entity_id_a, int sub_id_a, int entity_id_b, int sub_id_b) {
    if (entity_id_a==entity_id_b) return false;
    if (s_collider_ignores.empty()) return false;
    // Exact match
    if (s_collider_ignores.count(normalize_collider_key(entity_id_a, sub_id_a, entity_id_b, sub_id_b))) return true;
    // Wildcard on either side ("ignore ALL sub-colliders of this entity vs that one/sub-collider")
    if (s_collider_ignores.count(normalize_collider_key(entity_id_a, -1, entity_id_b, sub_id_b))) return true;
    if (s_collider_ignores.count(normalize_collider_key(entity_id_a, sub_id_a, entity_id_b, -1))) return true;
    if (s_collider_ignores.count(normalize_collider_key(entity_id_a, -1, entity_id_b, -1))) return true;
    return false;
}
void clear_collider_ignores() { s_collider_ignores.clear(); }

// Physics time scale
void set_physics_time_scale(float scale) { s_physics_time_scale = std::max(0.f, scale); }
float get_physics_time_scale() { return s_physics_time_scale; }

// Center of mass offset
void set_center_of_mass(Entity& entity, float offset_x, float offset_y) {
    if (!has_component(entity,"Rigidbody2D")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    rb["center_of_mass_x"] = offset_x;
    rb["center_of_mass_y"] = offset_y;
}

Vec2 get_center_of_mass(Entity& entity) {
    if (!has_component(entity,"Rigidbody2D")) return {0,0};
    auto& rb=entity["components"]["Rigidbody2D"];
    return {rb.value("center_of_mass_x",0.f), rb.value("center_of_mass_y",0.f)};
}

// Surface velocity
void set_surface_velocity(Entity& entity, float vx, float vy) {
    if (!has_component(entity,"Rigidbody2D")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    rb["surface_velocity_x"] = vx;
    rb["surface_velocity_y"] = vy;
}

Vec2 get_surface_velocity(Entity& entity) {
    if (!has_component(entity,"Rigidbody2D")) return {0,0};
    auto& rb=entity["components"]["Rigidbody2D"];
    return {rb.value("surface_velocity_x",0.f), rb.value("surface_velocity_y",0.f)};
}

// Auto mass calculation from density
static float compute_inertia(const Entity& e, float mass);
static std::vector<Shape> collect_shapes(Entity& e);

void auto_compute_mass(Entity& entity, float density) {
    if (!has_component(entity,"Rigidbody2D")||!has_component(entity,"Transform")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    auto shapes=collect_shapes(entity);
    if (shapes.empty()) return;

    float total_area = 0.f;
    for (auto& sh:shapes){
        if (sh.kind==ShapeKind::Circle){
            total_area += (float)M_PI * sh.radius * sh.radius;
        } else if (sh.kind==ShapeKind::Polygon){
            // Polygon area using shoelace formula
            float area = 0.f;
            int n = (int)sh.verts.size();
            for (int i=0;i<n;++i){
                auto& p1=sh.verts[i];
                auto& p2=sh.verts[(i+1)%n];
                area += p1.first*p2.second - p2.first*p1.second;
            }
            total_area += std::abs(area) * 0.5f;
        } else if (sh.kind==ShapeKind::Capsule){
            // Capsule: rectangle + 2 semicircles = rectangle + 1 circle
            float rect_area = 2.f * sh.radius * sh.cap_half_h;
            float circle_area = (float)M_PI * sh.radius * sh.radius;
            total_area += rect_area + circle_area;
        }
    }

    float mass = total_area * density;
    if (mass < 1e-6f) mass = 1.f;
    rb["mass"] = mass;

    // Auto-compute inertia
    float inertia = compute_inertia(entity, mass);
    if (inertia > 1e-9f) rb["inertia"] = inertia;
}
static inline int shape_cache_subkey(const Shape& s) {
    // Tilemap and any future multi-collider shapes should provide a unique sub_id.
    // For ordinary single-collider entities this stays at 0.
    return s.sub_id;
}

static inline long long contact_cache_key(int id1, int id2, int sub1 = 0, int sub2 = 0) {
    if (id1 > id2) {
        std::swap(id1, id2);
        std::swap(sub1, sub2);
    }
    unsigned long long a = (unsigned long long)(unsigned int)id1;
    unsigned long long b = (unsigned long long)(unsigned int)id2;
    unsigned long long s = (unsigned long long)(unsigned int)sub1;
    unsigned long long t = (unsigned long long)(unsigned int)sub2;
    return (long long)((a << 32) ^ b ^ (s * 0x9e3779b1u) ^ (t * 0x85ebca6bu));
}


// ════════════════════════════════════════════════════════════════════════════
//  1.  MATH UTILITIES
// ════════════════════════════════════════════════════════════════════════════
Verts convex_hull(Verts pts) {
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() <= 1) return pts;
    auto cross3 = [](Vec2 o, Vec2 a, Vec2 b) {
        return (a.first-o.first)*(b.second-o.second)-(a.second-o.second)*(b.first-o.first);
    };
    Verts lo, up;
    for (auto& p : pts) {
        while (lo.size()>=2 && cross3(lo[lo.size()-2],lo.back(),p)<=0) lo.pop_back();
        lo.push_back(p);
    }
    for (int i=(int)pts.size()-1;i>=0;--i){
        auto& p=pts[i];
        while (up.size()>=2 && cross3(up[up.size()-2],up.back(),p)<=0) up.pop_back();
        up.push_back(p);
    }
    lo.pop_back(); up.pop_back();
    lo.insert(lo.end(), up.begin(), up.end());
    return lo;
}

float poly_area(const Verts& v) {
    float a=0; int n=(int)v.size();
    for (int i=0;i<n;++i){
        auto [x1,y1]=v[i]; auto [x2,y2]=v[(i+1)%n];
        a += x1*y2-x2*y1;
    }
    return a*0.5f;
}

Verts ensure_ccw(Verts v) {
    if (v.size()>=3 && poly_area(v)<0) std::reverse(v.begin(),v.end());
    return v;
}

// ─── Polygon centroid + inertia via Green's theorem ───────────────────────────
// Returns {cx, cy, area, I_zz_about_centroid}  (all in world pixel units)
std::tuple<float,float,float,float> poly_mass_data(const Verts& v) {
    int n = (int)v.size();
    if (n < 3) return {0,0,1,1};
    float area = 0, cx = 0, cy = 0, Iz = 0;
    for (int i = 0; i < n; ++i) {
        auto [x0,y0] = v[i];
        auto [x1,y1] = v[(i+1)%n];
        float cross = x0*y1 - x1*y0;
        area += cross;
        cx   += (x0+x1) * cross;
        cy   += (y0+y1) * cross;
        // Second moment contribution (about origin)
        Iz   += (x0*x0 + x0*x1 + x1*x1 + y0*y0 + y0*y1 + y1*y1) * cross;
    }
    area *= 0.5f;
    if (std::abs(area) < 1e-12f) return {0,0,1e-6f,1e-6f};
    float inv6 = 1.f / (6.f * area);
    cx *= inv6; cy *= inv6;
    // Shift to centroid via parallel-axis theorem
    float I_centroid = std::abs(Iz) / 12.f - std::abs(area) * (cx*cx + cy*cy);
    return {cx, cy, std::abs(area), std::abs(I_centroid)};
}

// ════════════════════════════════════════════════════════════════════════════
//  2.  ENTITY HELPERS
// ════════════════════════════════════════════════════════════════════════════
static std::string body_type(const Entity& rb) {
    if (rb.is_null()) return "static";
    if (rb.value("is_kinematic",false)) return "kinematic";
    std::string bt = rb.value("body_type","dynamic");
    if (bt=="static"||bt=="kinematic"||bt=="dynamic") return bt;
    return "dynamic";
}
static bool is_static (const Entity* rb) {
    if (!rb||rb->is_null()) return true;
    if (body_type(*rb)=="static") return true;
    return rb->value("mass",1.f)<=0.f;
}
static bool is_dynamic(const Entity& rb) { return body_type(rb)=="dynamic"; }

// ─── Auto-compute inertia from shape geometry ─────────────────────────────
static float compute_inertia(const Entity& e, float mass) {
    if (!e.contains("components")) return mass*100.f*100.f/6.f;
    auto& c = e["components"];
    if (c.contains("BoxCollider2D")) {
        float w = finite_val(get_float(c["BoxCollider2D"],"width",1.f));
        float h = finite_val(get_float(c["BoxCollider2D"],"height",1.f));
        return mass*(w*w+h*h)/12.f;
    }
    if (c.contains("CircleCollider2D")) {
        float r = finite_val(get_float(c["CircleCollider2D"],"radius",8.f));
        return 0.5f*mass*r*r;
    }
    if (c.contains("CapsuleCollider2D")) {
        float r = finite_val(get_float(c["CapsuleCollider2D"],"radius",8.f));
        float h = finite_val(get_float(c["CapsuleCollider2D"],"height",32.f));
        float cyl_h = std::max(0.f, h - 2*r);
        float m_cyl = mass * cyl_h / std::max(cyl_h + (float)M_PI*r, 1e-9f);
        float m_cap = mass - m_cyl;
        return m_cyl*(3*r*r+cyl_h*cyl_h)/12.f + m_cap*(0.5f*r*r + cyl_h*cyl_h/4.f);
    }
    if (c.contains("PolygonCollider2D")) {
        // Exact polygon inertia via Green's theorem
        auto& poly = c["PolygonCollider2D"];
        auto pts = poly.value("points", Entity::array());
        if (pts.size() >= 3) {
            Verts verts;
            for (auto& p : pts)
                verts.push_back({finite_val((float)p[0]), finite_val((float)p[1])});
            auto [pcx, pcy, area, Iz] = poly_mass_data(verts);
            if (area > 1e-6f) return mass * Iz / area;
        }
    }
    // Fallback: treat as medium square
    return mass*100.f*100.f/6.f;
}

// ════════════════════════════════════════════════════════════════════════════
//  3.  SHAPE BUILDING
// ════════════════════════════════════════════════════════════════════════════
static PhysicsMaterial read_material(const Entity& col) {
    PhysicsMaterial m;
    m.friction   = finite_val(col.value("friction",  0.4f), 0.4f);
    m.bounciness = finite_val(col.value("bounciness",0.0f), 0.0f);
    auto str_mode = [&](const char* key, CombineMode def) -> CombineMode {
        std::string s = col.value(key, std::string{});
        if (s=="minimum")  return CombineMode::Minimum;
        if (s=="maximum")  return CombineMode::Maximum;
        if (s=="multiply") return CombineMode::Multiply;
        return def;
    };
    m.friction_combine = str_mode("friction_combine", CombineMode::Average);
    m.bounce_combine   = str_mode("bounce_combine",   CombineMode::Maximum);

    // Physics material override support
    if (col.contains("material_override")){
        auto& mat_override = col["material_override"];
        m.friction   = finite_val(mat_override.value("friction",  m.friction),  m.friction);
        m.bounciness = finite_val(mat_override.value("bounciness", m.bounciness), m.bounciness);
        m.friction_combine = str_mode("friction_combine", m.friction_combine);
        m.bounce_combine   = str_mode("bounce_combine",   m.bounce_combine);
    }
    return m;
}

// ════════════════════════════════════════════════════════════════════════════
//  2b. CONCAVE POLYGON DECOMPOSITION (Bayazit / Ear-Clipping)
//
//  Unity2D automatically decomposes concave PolygonCollider2D into convex pieces.
//  We implement a recursive diagonal-splitting approach (O(n²) but fine for game
//  polygons which are typically < 32 verts).
// ════════════════════════════════════════════════════════════════════════════

static bool segs_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    auto cr=[](Vec2 o,Vec2 p,Vec2 q){return (p.first-o.first)*(q.second-o.second)-(p.second-o.second)*(q.first-o.first);};
    float d1=cr(c,d,a),d2=cr(c,d,b),d3=cr(a,b,c),d4=cr(a,b,d);
    if(((d1>0&&d2<0)||(d1<0&&d2>0))&&((d3>0&&d4<0)||(d3<0&&d4>0))) return true;
    return false;
}

static bool diagonal_inside(const Verts& poly, int i, int j) {
    int n=(int)poly.size();
    Vec2 a=poly[i],b=poly[j];
    for(int k=0;k<n;++k){
        int k2=(k+1)%n;
        if(k==i||k==j||k2==i||k2==j) continue;
        if(segs_intersect(a,b,poly[k],poly[k2])) return false;
    }
    return true;
}

static bool is_convex_vertex(const Verts& poly, int i) {
    int n=(int)poly.size();
    Vec2 prev=poly[(i+n-1)%n],cur=poly[i],next=poly[(i+1)%n];
    float cross=(cur.first-prev.first)*(next.second-prev.second)-(cur.second-prev.second)*(next.first-prev.first);
    return cross>=0.f; // CCW polygon: positive = convex
}

static bool poly_is_convex(const Verts& v) {
    int n=(int)v.size();
    if(n<3) return true;
    for(int i=0;i<n;++i){
        Vec2 a=v[i],b=v[(i+1)%n],c=v[(i+2)%n];
        float cross=(b.first-a.first)*(c.second-a.second)-(b.second-a.second)*(c.first-a.first);
        if(cross<-1e-5f) return false; // reflex vertex found
    }
    return true;
}

// Find the best diagonal to split a concave polygon.
// Returns (i,j) indices or (-1,-1) if polygon is already convex.
static std::pair<int,int> find_best_diagonal(const Verts& poly) {
    int n=(int)poly.size();
    for(int i=0;i<n;++i){
        if(is_convex_vertex(poly,i)) continue; // only from reflex vertices
        for(int j=0;j<n;++j){
            if(std::abs(i-j)<=1||std::abs(i-j)==n-1) continue;
            if(diagonal_inside(poly,i,j)) return {i,j};
        }
    }
    return {-1,-1};
}

// Recursively decompose a CCW polygon into convex pieces.
static void decompose_convex(const Verts& poly, std::vector<Verts>& out, int depth=0) {
    if(depth>32||poly.size()<3){return;}
    if(poly_is_convex(poly)){out.push_back(poly);return;}
    auto [i,j]=find_best_diagonal(poly);
    if(i<0){out.push_back(poly);return;} // fallback: treat as convex
    int n=(int)poly.size();
    Verts left,right;
    for(int k=i;k!=j;k=(k+1)%n) left.push_back(poly[k]);
    left.push_back(poly[j]);
    for(int k=j;k!=i;k=(k+1)%n) right.push_back(poly[k]);
    right.push_back(poly[i]);
    decompose_convex(ensure_ccw(left), out, depth+1);
    decompose_convex(ensure_ccw(right), out, depth+1);
}

std::optional<Shape> build_shape(Entity& e) {
    if (!has_component(e,"Transform")) return std::nullopt;
    auto& comps = e["components"];
    auto wt  = transform::cached_world(e);
    float tx = finite_val(wt.x), ty = finite_val(wt.y);
    float rot = wt.rotation*(float)M_PI/180.f;
    float c=std::cos(rot), s=std::sin(rot);

    auto make_base = [&](Entity& col_comp) -> Shape {
        Shape sh; sh.entity=&e; sh.col=&col_comp; sh.sub_id=0;
        sh.material = read_material(col_comp);
        return sh;
    };

    if (comps.contains("BoxCollider2D")) {
        auto& box=comps["BoxCollider2D"];
        float ox=finite_val(get_float(box,"offset_x")),oy=finite_val(get_float(box,"offset_y"));
        float hw=std::max(0.f,finite_val(get_float(box,"width",1.f)))*0.5f;
        float hh=std::max(0.f,finite_val(get_float(box,"height",1.f)))*0.5f;
        Verts verts;
        for (auto [dx,dy]:std::initializer_list<Vec2>{{-hw,-hh},{hw,-hh},{hw,hh},{-hw,hh}})
            verts.push_back(world_from_local(tx,ty,c,s,ox+dx,oy+dy));
        Shape sh=make_base(box); sh.kind=ShapeKind::Polygon;
        sh.verts=ensure_ccw(verts); sh.cx=tx; sh.cy=ty;
        return sh;
    }
    if (comps.contains("CircleCollider2D")) {
        auto& circ=comps["CircleCollider2D"];
        float ox=finite_val(get_float(circ,"offset_x")),oy=finite_val(get_float(circ,"offset_y"));
        auto [cx2,cy2]=world_from_local(tx,ty,c,s,ox,oy);
        Shape sh=make_base(circ); sh.kind=ShapeKind::Circle;
        sh.cx=cx2; sh.cy=cy2; sh.radius=std::max(0.f,finite_val(get_float(circ,"radius",8.f)));
        return sh;
    }
    if (comps.contains("CapsuleCollider2D")) {
        auto& cap=comps["CapsuleCollider2D"];
        float ox=finite_val(get_float(cap,"offset_x")),oy=finite_val(get_float(cap,"offset_y"));
        float r  =std::max(1e-3f,finite_val(get_float(cap,"radius",8.f)));
        float h  =std::max(0.f,   finite_val(get_float(cap,"height",32.f)));
        bool horiz=(cap.value("direction","vertical")=="horizontal");
        Shape sh=make_base(cap); sh.kind=ShapeKind::Capsule;
        sh.radius=r; sh.cap_half_h=std::max(0.f,h*0.5f-r); sh.cap_horiz=horiz;
        // Store local center in world space
        auto [wcx,wcy]=world_from_local(tx,ty,c,s,ox,oy);
        sh.cx=wcx; sh.cy=wcy;
        // Store world-space axis endpoints in world_pts[0..1] for solver
        if (horiz) {
            sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox-sh.cap_half_h,oy));
            sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox+sh.cap_half_h,oy));
        } else {
            sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox,oy-sh.cap_half_h));
            sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox,oy+sh.cap_half_h));
        }
        return sh;
    }
    if (comps.contains("PolygonCollider2D")) {
        auto& poly=comps["PolygonCollider2D"];
        auto pts=poly.value("points",Entity::array());
        if (pts.size()<3) return std::nullopt;
        float ox=finite_val(get_float(poly,"offset_x")),oy=finite_val(get_float(poly,"offset_y"));
        Verts verts;
        for (auto& p:pts)
            verts.push_back(world_from_local(tx,ty,c,s,finite_val((float)p[0])+ox,finite_val((float)p[1])+oy));
        Shape sh=make_base(poly); sh.kind=ShapeKind::Polygon;
        sh.verts=ensure_ccw(verts); sh.cx=tx; sh.cy=ty;
        return sh;
    }
    if (comps.contains("EdgeCollider2D")) {
        auto& edge=comps["EdgeCollider2D"];
        auto pts=edge.value("points",Entity::array());
        if (pts.size()<2) return std::nullopt;
        float ox=finite_val(get_float(edge,"offset_x")),oy=finite_val(get_float(edge,"offset_y"));
        float th=std::max(1.f,finite_val(get_float(edge,"thickness",2.f)));
        std::vector<Vec2> world;
        for (auto& p:pts)
            world.push_back(world_from_local(tx,ty,c,s,finite_val((float)p[0])+ox,finite_val((float)p[1])+oy));
        Shape sh=make_base(edge); sh.kind=ShapeKind::Edge;
        sh.world_pts=world; sh.thickness=th; sh.cx=tx; sh.cy=ty;
        return sh;
    }
    return std::nullopt;
}

// Deserialize a cached shape previously serialized in collect_shapes
static Shape deserialize_shape(Entity& se, Entity& e, Entity& tm) {
    Shape sh; sh.entity=&e; sh.col=&tm; sh.kind=(ShapeKind)(int)se["k"];
    sh.sub_id=(int)se["s"]; sh.cx=(float)se["cx"]; sh.cy=(float)se["cy"];
    if (se.contains("pts")) {
        for (auto& p:se["pts"]) {
            if (sh.kind==ShapeKind::Polygon)
                sh.verts.push_back({(float)p[0],(float)p[1]});
            else
                sh.world_pts.push_back({(float)p[0],(float)p[1]});
        }
    }
    sh.thickness=(float)se.value("t",8.f);
    sh.face_nx=(float)se.value("fnx",0.f); sh.face_ny=(float)se.value("fny",0.f);
    return sh;
}

static std::vector<Shape> collect_shapes(Entity& e) {
    std::vector<Shape> out;

    // ── PolygonCollider2D: decompose concave polygons into convex pieces ──────
    // Unity auto-decomposes; we now match that behaviour.
    if (has_component(e,"Transform") && has_component(e,"components") && e["components"].contains("PolygonCollider2D")) {
        auto& poly = e["components"]["PolygonCollider2D"];
        auto pts = poly.value("points", Entity::array());
        if (pts.size() >= 3) {
            auto wt = transform::cached_world(e);
            float tx=finite_val(wt.x), ty=finite_val(wt.y);
            float rot=wt.rotation*(float)M_PI/180.f;
            float c=std::cos(rot), s=std::sin(rot);
            float ox=finite_val(get_float(poly,"offset_x")), oy=finite_val(get_float(poly,"offset_y"));
            Verts local_verts;
            for (auto& p : pts)
                local_verts.push_back({finite_val((float)p[0])+ox, finite_val((float)p[1])+oy});
            local_verts = ensure_ccw(local_verts);

            if (poly_is_convex(local_verts)) {
                // Fast path: already convex, transform directly
                Verts world_verts;
                for (auto [lx,ly] : local_verts)
                    world_verts.push_back(world_from_local(tx,ty,c,s,lx,ly));
                PhysicsMaterial mat = read_material(poly);
                Shape sh; sh.entity=&e; sh.col=&poly; sh.sub_id=0;
                sh.material=mat; sh.kind=ShapeKind::Polygon;
                sh.verts=ensure_ccw(world_verts); sh.cx=tx; sh.cy=ty;
                out.push_back(std::move(sh));
            } else {
                // Concave: decompose into convex pieces
                std::vector<Verts> pieces;
                decompose_convex(local_verts, pieces);
                PhysicsMaterial mat = read_material(poly);
                for (int pi=0; pi<(int)pieces.size(); ++pi) {
                    Verts world_verts;
                    for (auto [lx,ly] : pieces[pi])
                        world_verts.push_back(world_from_local(tx,ty,c,s,lx,ly));
                    Shape sh; sh.entity=&e; sh.col=&poly; sh.sub_id=pi;
                    sh.material=mat; sh.kind=ShapeKind::Polygon;
                    sh.verts=ensure_ccw(world_verts); sh.cx=tx; sh.cy=ty;
                    out.push_back(std::move(sh));
                }
            }
            // Skip build_shape's PolygonCollider2D branch if we generated shapes
            if (!out.empty()) goto skip_polygon_build_shape;
        }
    }

    // Primary collider (non-polygon handled above)
    if (auto sh=build_shape(e)) out.push_back(*sh);

    skip_polygon_build_shape:;

    // Additional colliders (composite support)
    if (has_component(e,"CompositeCollider2D")){
        auto& comp=e["components"]["CompositeCollider2D"];
        auto colliders=comp.value("colliders",Entity::array());
        for (auto& col_ref:colliders){
            if (col_ref.contains("type")&&col_ref.contains("offset_x")&&col_ref.contains("offset_y")){
                std::string type = col_ref.value("type", std::string{});
                float ox = finite_val(col_ref.value("offset_x", 0.f));
                float oy = finite_val(col_ref.value("offset_y", 0.f));
                auto wt=transform::cached_world(e);
                float tx=finite_val(wt.x), ty=finite_val(wt.y);
                float rot=wt.rotation*(float)M_PI/180.f;
                float c=std::cos(rot), s=std::sin(rot);

                Shape sh; sh.entity=&e; sh.col=&comp; sh.sub_id=(int)out.size();
                sh.material=read_material(comp);

                if (type=="box"){
                    float w = finite_val(col_ref.value("width", 1.f)) * 0.5f;
                    float h = finite_val(col_ref.value("height", 1.f)) * 0.5f;
                    Verts verts;
                    for (auto [dx,dy]:std::initializer_list<Vec2>{{-w,-h},{w,-h},{w,h},{-w,h}})
                        verts.push_back(world_from_local(tx,ty,c,s,ox+dx,oy+dy));
                    sh.kind=ShapeKind::Polygon; sh.verts=ensure_ccw(verts);
                    sh.cx=tx; sh.cy=ty;
                    out.push_back(std::move(sh));
                } else if (type=="circle"){
                    float r = std::max(0.f, finite_val(col_ref.value("radius", 8.f)));
                    auto [cx2,cy2]=world_from_local(tx,ty,c,s,ox,oy);
                    sh.kind=ShapeKind::Circle; sh.cx=cx2; sh.cy=cy2; sh.radius=r;
                    out.push_back(std::move(sh));
                } else if (type=="capsule"){
                    float r = std::max(1e-3f, finite_val(col_ref.value("radius", 8.f)));
                    float h = std::max(0.f, finite_val(col_ref.value("height", 32.f)));
                    bool horiz=col_ref.value("direction","vertical")=="horizontal";
                    sh.kind=ShapeKind::Capsule; sh.radius=r; sh.cap_half_h=std::max(0.f,h*0.5f-r); sh.cap_horiz=horiz;
                    auto [wcx,wcy]=world_from_local(tx,ty,c,s,ox,oy);
                    sh.cx=wcx; sh.cy=wcy;
                    if (horiz){
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox-sh.cap_half_h,oy));
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox+sh.cap_half_h,oy));
                    } else {
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox,oy-sh.cap_half_h));
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox,oy+sh.cap_half_h));
                    }
                    out.push_back(std::move(sh));
                }
            }
        }
    }

    if (!has_component(e,"Tilemap")) return out;
    auto& tm=e["components"]["Tilemap"];
    if (!tm.value("generate_colliders",false)) return out;

    const float cell_width=std::max(1.f,tm.value("_grid_cell_width",(float)tm.value("tile_size",32)));
    const float cell_height=std::max(1.f,tm.value("_grid_cell_height",(float)tm.value("tile_size",32)));
    const float gap_x=tm.value("_grid_cell_gap_x",0.f), gap_y=tm.value("_grid_cell_gap_y",0.f);
    const float stride_x=std::max(1.f,cell_width+gap_x), stride_y=std::max(1.f,cell_height+gap_y);
    const bool merge_cells=std::abs(gap_x)<0.0001f && std::abs(gap_y)<0.0001f;
    int origin_x=tm.value("origin_x",0), origin_y=tm.value("origin_y",0);
    auto& grid=tm["grid"];
    std::unordered_set<int> col_set;
    if (tm.contains("collider_tile_ids"))
        for (auto& id:tm["collider_tile_ids"]) if(id.is_number()) col_set.insert(id.get<int>());
    auto wt=transform::cached_world(e);
    float tx=finite_val(wt.x), ty=finite_val(wt.y);

    int rows=(int)grid.size();
    if (rows==0) return out;
    int cols=(int)grid[0].size();

    auto is_solid=[&](int r, int c)->bool{
        if(r<0||r>=rows||c<0||c>=cols) return false;
        if(grid[r][c].is_null()||!grid[r][c].is_number()) return false;
        int tid=grid[r][c].get<int>();
        if(tid<0) return false;
        if(!col_set.empty()&&!col_set.count(tid)) return false;
        if (tm.contains("tile_collision") && tm["tile_collision"].is_object()) {
            const std::string key = std::to_string(tid);
            if (tm["tile_collision"].contains(key)) {
                const Entity& collision = tm["tile_collision"][key];
                if (collision.is_string() && collision.get<std::string>() == "none") return false;
            }
        }
        return true;
    };

    std::vector<std::vector<bool>> visited(rows, std::vector<bool>(cols, false));

    auto add_box=[&](int r, int c, int w, int h) {
        float x1 = tx + (c + origin_x) * stride_x;
        float y1 = ty + (r + origin_y) * stride_y;
        float x2 = tx + (c + w - 1 + origin_x) * stride_x + cell_width;
        float y2 = ty + (r + h - 1 + origin_y) * stride_y + cell_height;
        Verts verts = {{x1,y1},{x2,y1},{x2,y2},{x1,y2}};
        Shape sh; sh.entity=&e; sh.col=&tm; sh.kind=ShapeKind::Polygon;
        sh.sub_id=(int)out.size(); sh.verts=ensure_ccw(verts);
        sh.cx=(x1+x2)*0.5f; sh.cy=(y1+y2)*0.5f;
        sh.material=PhysicsMaterial();
        out.push_back(sh);
    };

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (visited[r][c] || !is_solid(r, c)) continue;
            int c2 = c;
            while (merge_cells && c2+1<cols && !visited[r][c2+1] && is_solid(r, c2+1)) ++c2;
            int r2 = r;
            for (int rw = r+1; merge_cells && rw < rows; ++rw) {
                bool full = true;
                for (int cx = c; cx <= c2; ++cx) { if (visited[rw][cx]||!is_solid(rw,cx)){full=false;break;} }
                if (full) r2 = rw; else break;
            }
            for (int vr=r;vr<=r2;++vr) for (int vc=c;vc<=c2;++vc) visited[vr][vc]=true;
            add_box(r, c, c2-c+1, r2-r+1);
        }
    }
    return out;
}

std::tuple<float,float,float,float> shape_aabb(const Shape& s) {
    if (s.kind==ShapeKind::Circle)
        return {s.cx-s.radius,s.cy-s.radius,s.cx+s.radius,s.cy+s.radius};
    if (s.kind==ShapeKind::Capsule) {
        float ex=s.radius+std::max(std::abs(s.world_pts[0].first-s.world_pts[1].first)*0.5f,s.cap_half_h);
        float ey=s.radius+std::max(std::abs(s.world_pts[0].second-s.world_pts[1].second)*0.5f,s.cap_half_h);
        return {s.cx-ex,s.cy-ey,s.cx+ex,s.cy+ey};
    }
    if (s.kind==ShapeKind::Polygon) {
        float x1=1e30f,y1=1e30f,x2=-1e30f,y2=-1e30f;
        for (auto [x,y]:s.verts){x1=std::min(x1,x);y1=std::min(y1,y);x2=std::max(x2,x);y2=std::max(y2,y);}
        return {x1,y1,x2,y2};
    }
    // Edge
    float x1=1e30f,y1=1e30f,x2=-1e30f,y2=-1e30f;
    for (auto [x,y]:s.world_pts){x1=std::min(x1,x);y1=std::min(y1,y);x2=std::max(x2,x);y2=std::max(y2,y);}
    return {x1-s.thickness,y1-s.thickness,x2+s.thickness,y2+s.thickness};
}

// ════════════════════════════════════════════════════════════════════════════
//  4.  FORCE & IMPULSE API
// ════════════════════════════════════════════════════════════════════════════
void add_force(Entity& rb, float fx, float fy) {
    if (body_type(rb)!="dynamic") return;
    rb["_force_x"]=rb.value("_force_x",0.f)+finite_val(fx);
    rb["_force_y"]=rb.value("_force_y",0.f)+finite_val(fy);
    // Mirror into inspector-visible accumulator (used by get_total_force / totalForce API)
    rb["_acc_force_x"]=rb.value("_acc_force_x",0.f)+finite_val(fx);
    rb["_acc_force_y"]=rb.value("_acc_force_y",0.f)+finite_val(fy);
    rb["_sleeping"]=false; rb["_sleep_t"]=0.f;
}
void add_force_at_point(Entity& rb, float fx, float fy, float wx, float wy) {
    if (body_type(rb)!="dynamic") return;
    add_force(rb,fx,fy);
    // torque = r × F
    auto& t=rb; // rb IS the Rigidbody2D component; Transform is sibling
    // caller must pass the entity's Transform pos via wx/wy relative to center
    // we store the torque accumulator
    float torque = finite_val(cross(wx,wy,fx,fy));
    rb["_torque"]=rb.value("_torque",0.f)+torque;
    rb["_acc_torque"]=rb.value("_acc_torque",0.f)+torque;
    rb["_sleeping"]=false;
}
void add_impulse(Entity& rb, float jx, float jy) {
    if (body_type(rb)!="dynamic") return;
    float m=std::max(finite_val(rb.value("mass",1.f),1.f),1e-9f);
    rb["velocity_x"]=rb.value("velocity_x",0.f)+finite_val(jx)/m;
    rb["velocity_y"]=rb.value("velocity_y",0.f)+finite_val(jy)/m;
    rb["_sleeping"]=false; rb["_sleep_t"]=0.f;
}
void add_impulse_at_point(Entity& rb, float jx, float jy, float rx, float ry) {
    if (body_type(rb)!="dynamic") return;
    add_impulse(rb,jx,jy);
    float mass=std::max(finite_val(rb.value("mass",1.f),1.f),1e-9f);
    float I=std::max(finite_val(rb.value("inertia",mass*100.f*100.f/6.f)),1e-9f);
    rb["angular_velocity"]=(float)rb["angular_velocity"]+cross(rx,ry,jx,jy)/I;
    rb["_sleeping"]=false;
}
void add_torque(Entity& rb, float torque) {
    if (body_type(rb)!="dynamic") return;
    rb["_torque"]=rb.value("_torque",0.f)+finite_val(torque);
    rb["_acc_torque"]=rb.value("_acc_torque",0.f)+finite_val(torque);
    rb["_sleeping"]=false; rb["_sleep_t"]=0.f;
}
void add_angular_impulse(Entity& rb, float j) {
    if (body_type(rb)!="dynamic") return;
    float mass=std::max(finite_val(rb.value("mass",1.f),1.f),1e-9f);
    float I=std::max(finite_val(rb.value("inertia",mass*100.f*100.f/6.f)),1e-9f);
    rb["angular_velocity"]=(float)rb.value("angular_velocity",0.f)+finite_val(j)/I;
    rb["_sleeping"]=false;
}

void explosion_force(EntityList& entities, float x, float y,
                     float radius, float strength, float upward, const std::string& mode) {
    float r2=radius*radius;
    for (auto& e:entities) {
        if (!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb=e["components"]["Rigidbody2D"];
        if (is_static(&rb)) continue;
        auto& t=e["components"]["Transform"];
        float dx=finite_val(get_float(t,"x"))-x, dy=finite_val(get_float(t,"y"))-y;
        float d2=dx*dx+dy*dy;
        if (d2>r2||d2<1e-12f) continue;
        float d=std::sqrt(d2), falloff=1.f-d/radius;
        float nx=dx/d, ny=dy/d;
        float fx=nx*strength*falloff, fy=ny*strength*falloff+upward*strength*falloff;
        if (mode=="force") add_force(rb,fx,fy);
        else               add_impulse(rb,fx,fy);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  5.  GEOMETRY HELPERS
// ════════════════════════════════════════════════════════════════════════════
static bool point_in_poly(float px, float py, const Verts& v) {
    bool inside=false; int n=(int)v.size(), j=n-1;
    for (int i=0;i<n;j=i++) {
        auto [xi,yi]=v[i]; auto [xj,yj]=v[j];
        if (((yi>py)!=(yj>py))&&
            (px<(xj-xi)*(py-yi)/((yj-yi)?(yj-yi):1e-12f)+xi))
            inside=!inside;
    }
    return inside;
}

static std::pair<Vec2,float> closest_on_seg(float ax,float ay,float bx,float by,float px,float py){
    float dx=bx-ax, dy=by-ay, l2=dx*dx+dy*dy;
    if (l2<1e-12f) return {{ax,ay},(px-ax)*(px-ax)+(py-ay)*(py-ay)};
    float t=clamp(((px-ax)*dx+(py-ay)*dy)/l2,0.f,1.f);
    Vec2 p={ax+t*dx,ay+t*dy};
    return {p,(px-p.first)*(px-p.first)+(py-p.second)*(py-p.second)};
}

static std::pair<Vec2,float> closest_on_poly(float px,float py,const Verts& v){
    Vec2 best=v[0]; float bd=1e30f;
    for (int i=0;i<(int)v.size();++i){
        auto [p,d2]=closest_on_seg(v[i].first,v[i].second,v[(i+1)%v.size()].first,v[(i+1)%v.size()].second,px,py);
        if (d2<bd){bd=d2;best=p;}
    }
    return {best,bd};
}

// Closest point on capsule segment AB to point P
static Vec2 closest_on_capsule_seg(Vec2 A, Vec2 B, float px, float py){
    auto [p,_]=closest_on_seg(A.first,A.second,B.first,B.second,px,py);
    return p;
}

// ════════════════════════════════════════════════════════════════════════════
//  6.  COLLISION DETECTION  (Box2D-style)
// ════════════════════════════════════════════════════════════════════════════

// ── 6a  SAT poly-poly – returns (penetration, axis) ──────────────────────
struct SATResult { float pen; Vec2 axis; int ref_edge_idx; bool from_a; };

static std::optional<SATResult> sat_one_way(const Verts& va, const Verts& vb, bool from_a) {
    float best=1e30f; int best_i=-1; Vec2 baxis{0,1};
    int n=(int)va.size();
    for (int i=0;i<n;++i){
        auto [ax,ay]=va[i]; auto [bx,by]=va[(i+1)%n];
        auto [nx,ny]=normalize(by-ay,ax-bx);
        float mn=-1e30f;
        for (auto [px,py]:va) mn=std::max(mn,dot(px,py,nx,ny));
        float mn2=1e30f;
        for (auto [px,py]:vb) mn2=std::min(mn2,dot(px,py,nx,ny));
        float ov=mn-mn2;
        if (ov<0) return std::nullopt;
        if (ov<best){best=ov;baxis={nx,ny};best_i=i;}
    }
    return SATResult{best,baxis,best_i,from_a};
}

// ── 6b  Face clipping (Sutherland-Hodgman style, ≤2 contacts) ────────────
static int clip_segment_to_plane(Vec2 v0, Vec2 v1, float nx, float ny, float d,
                                  Vec2 out[2]) {
    float d0 = dot(v0.first,v0.second,nx,ny) - d;
    float d1 = dot(v1.first,v1.second,nx,ny) - d;
    int cnt=0;
    if (d0>=0) out[cnt++]=v0;
    if (d1>=0) out[cnt++]=v1;
    if ((d0>0&&d1<0)||(d0<0&&d1>0)){
        float t=d0/(d0-d1);
        out[cnt++]={v0.first+(v1.first-v0.first)*t, v0.second+(v1.second-v0.second)*t};
    }
    return std::min(cnt,2);
}

static std::optional<Manifold> poly_vs_poly(const Shape& A, const Shape& B) {
    auto ra = sat_one_way(A.verts, B.verts, true);
    if (!ra) return std::nullopt;
    auto rb = sat_one_way(B.verts, A.verts, false);
    if (!rb) return std::nullopt;

    // Choose reference face = least penetration
    bool use_a = (ra->pen <= rb->pen + 0.05f);
    const SATResult& ref = use_a ? *ra : *rb;
    const Verts& ref_v   = use_a ? A.verts : B.verts;
    const Verts& inc_v   = use_a ? B.verts : A.verts;
    float nx = ref.axis.first, ny = ref.axis.second;

    // Ensure normal points from ref → inc
    float inc_cx=0,inc_cy=0;
    for (auto [x,y]:inc_v){inc_cx+=x;inc_cy+=y;}
    inc_cx/=(float)inc_v.size(); inc_cy/=(float)inc_v.size();
    float ref_cx=0,ref_cy=0;
    for (auto [x,y]:ref_v){ref_cx+=x;ref_cy+=y;}
    ref_cx/=(float)ref_v.size(); ref_cy/=(float)ref_v.size();
    if (dot(inc_cx-ref_cx,inc_cy-ref_cy,nx,ny)<0){nx=-nx;ny=-ny;}

    // Reference edge endpoints
    int ei = ref.ref_edge_idx; if(ei<0)ei=0;
    Vec2 rA = ref_v[ei], rB = ref_v[(ei+1)%ref_v.size()];

    // Find incident face on inc: face most anti-parallel to ref normal
    int best_i=0; float best_dot=1e30f;
    int m=(int)inc_v.size();
    for (int i=0;i<m;++i){
        auto [ax,ay]=inc_v[i]; auto [bx,by]=inc_v[(i+1)%m];
        auto [fnx,fny]=normalize(by-ay,ax-bx);
        float d=dot(fnx,fny,nx,ny);
        if (d<best_dot){best_dot=d;best_i=i;}
    }
    Vec2 iA=inc_v[best_i], iB=inc_v[(best_i+1)%m];

    // Clip incident edge to side planes of reference face
    auto [tex,tey]=normalize(rB.first-rA.first, rB.second-rA.second);
    float d0=dot(rA.first,rA.second,tex,tey), d1=dot(rB.first,rB.second,tex,tey);

    Vec2 tmp[2], clip[2];
    tmp[0]=iA; tmp[1]=iB;
    int cnt=clip_segment_to_plane(tmp[0],tmp[1],tex,tey,d0,clip);
    if (cnt<1) return std::nullopt;
    Vec2 tmp2[2];
    std::copy(clip,clip+cnt,tmp2);
    cnt=clip_segment_to_plane(tmp2[0],cnt>1?tmp2[1]:tmp2[0],-tex,-tey,-d1,clip);

    // Keep points behind reference face
    float ref_d = dot(rA.first,rA.second,nx,ny);
    Manifold man; man.nx=nx; man.ny=ny;
    for (int i=0;i<cnt;++i){
        float sep = dot(clip[i].first,clip[i].second,nx,ny) - ref_d;
        if (sep<=0.01f) {
            CP cp; cp.x=clip[i].first; cp.y=clip[i].second;
            cp.depth=std::max(0.f,-sep+ref.pen*0.5f);
            // ── Vertex-pair feature key (gap fix) ────────────────────────────
            // Old formula (best_i*1000+i) collides on polygons with ≥32 verts
            // because best_i can be up to n-1 and i only 0..1, giving overlap.
            // Use a hash that encodes BOTH the incident vertex index (best_i+i)
            // and the reference edge index (ei) so each contact point has a
            // unique stable key across frames regardless of polygon complexity.
            // The Cantor pairing function n*(n+1)/2+k is collision-free for
            // non-negative integers, giving keys up to ~2M for 2000-vert polys.
            {
                int inc_vtx = (best_i + i) % m; // actual incident vertex index
                int ref_vtx = ei;
                int key_a = std::min(inc_vtx, ref_vtx);
                int key_b = std::max(inc_vtx, ref_vtx);
                cp.fkey = key_b * (key_b + 1) / 2 + key_a;  // Cantor pairing
            }
            if (man.contact_count < MAX_CONTACTS)
                man.contacts[man.contact_count++]=cp;
        }
    }
    if (man.contact_count==0) {
        // Fallback: centroid contact
        float cpx=(rA.first+rB.first)*0.5f, cpy=(rA.second+rB.second)*0.5f;
        man.contacts[0]={cpx,cpy,ref.pen,0,0,0};
        man.contact_count=1;
    }
    if (!use_a) { man.nx=-man.nx; man.ny=-man.ny; } // flip back to A→B
    return man;
}

// ── 6c  Poly vs Circle ────────────────────────────────────────────────────
static std::optional<Manifold> poly_vs_circle(const Shape& poly, const Shape& circ) {
    float cx=circ.cx, cy=circ.cy, r=circ.radius;
    const auto& verts=poly.verts;
    if (verts.size()<3) return std::nullopt;

    // SAT axes from polygon edges
    float best=1e30f; Vec2 baxis={0,1};
    int n=(int)verts.size();
    for (int i=0;i<n;++i){
        auto [ax,ay]=verts[i]; auto [bx,by]=verts[(i+1)%n];
        auto [nx,ny]=normalize(by-ay,ax-bx);
        float mn=1e30f,mx=-1e30f;
        for (auto [px,py]:verts){float d=dot(px,py,nx,ny);mn=std::min(mn,d);mx=std::max(mx,d);}
        float cproj=dot(cx,cy,nx,ny);
        float ov=std::min(mx,cproj+r)-std::max(mn,cproj-r);
        if (ov<=0) return std::nullopt;
        if (ov<best){best=ov;baxis={nx,ny};}
    }
    // Axis from poly boundary to circle center
    auto [closest,d2]=closest_on_poly(cx,cy,verts);
    if (d2>1e-12f){
        auto [nx2,ny2]=normalize(cx-closest.first,cy-closest.second);
        float mn=1e30f,mx=-1e30f;
        for(auto [px,py]:verts){float d=dot(px,py,nx2,ny2);mn=std::min(mn,d);mx=std::max(mx,d);}
        float cproj=dot(cx,cy,nx2,ny2);
        float ov=std::min(mx,cproj+r)-std::max(mn,cproj-r);
        if(ov>0&&ov<best){best=ov;baxis={nx2,ny2};}
    }
    // Orient normal from poly center toward circle
    float pcx=0,pcy=0;
    for(auto [x,y]:verts){pcx+=x;pcy+=y;}
    pcx/=n; pcy/=n;
    auto [nx,ny]=baxis;
    if(dot(cx-pcx,cy-pcy,nx,ny)<0){nx=-nx;ny=-ny;}

    // Contact point: closest point on poly boundary (or circle surface)
    Manifold m; m.nx=nx; m.ny=ny;
    m.contacts[0]={closest.first,closest.second,best,0,0,0};
    m.contact_count=1;
    return m;
}

// ── 6d  Capsule narrow phase (segment-circle / segment-segment) ───────────
static std::optional<Manifold> capsule_vs_circle(const Shape& cap, const Shape& circ) {
    Vec2 A=cap.world_pts[0], B=cap.world_pts[1];
    float r1=cap.radius, r2=circ.radius;
    auto [closest,d2]=closest_on_seg(A.first,A.second,B.first,B.second,circ.cx,circ.cy);
    float rr=r1+r2;
    if (d2>rr*rr) return std::nullopt;
    float d=std::sqrt(d2);
    float nx2,ny2;
    if (d<1e-12f){ nx2=0; ny2=-1; }
    else {
        auto [n1,n2]=normalize(circ.cx-closest.first,circ.cy-closest.second);
        nx2=n1; ny2=n2;
    }
    Manifold m; m.nx=nx2; m.ny=ny2;
    m.contacts[0]={closest.first+nx2*r1,closest.second+ny2*r1,rr-d,0,0,0};
    m.contact_count=1;
    return m;
}

static std::optional<Manifold> capsule_vs_capsule(const Shape& A, const Shape& B) {
    // Segment-segment closest points
    Vec2 p1=A.world_pts[0],p2=A.world_pts[1],p3=B.world_pts[0],p4=B.world_pts[1];
    float d1x=p2.first-p1.first,d1y=p2.second-p1.second;
    float d2x=p4.first-p3.first,d2y=p4.second-p3.second;
    float r1x=p1.first-p3.first,r1y=p1.second-p3.second;
    float a=len2(d1x,d1y), e=len2(d2x,d2y);
    float f=dot(d2x,d2y,r1x,r1y);
    float s,t;
    if (a<1e-12f&&e<1e-12f){ s=t=0; }
    else if (a<1e-12f){ s=0; t=clamp(f/e,0.f,1.f); }
    else {
        float c=dot(d1x,d1y,r1x,r1y);
        if (e<1e-12f){ t=0; s=clamp(-c/a,0.f,1.f); }
        else {
            float b=dot(d1x,d1y,d2x,d2y), denom=a*e-b*b;
            if (std::abs(denom)>1e-12f) s=clamp((b*f-c*e)/denom,0.f,1.f); else s=0;
            t=clamp((b*s+f)/e,0.f,1.f);
            s=clamp((b*t-c)/a,0.f,1.f);
            t=clamp((b*s+f)/e,0.f,1.f);
        }
    }
    Vec2 cA={p1.first+d1x*s,p1.second+d1y*s};
    Vec2 cB={p3.first+d2x*t,p3.second+d2y*t};
    float rr=A.radius+B.radius;
    float dx=cB.first-cA.first, dy=cB.second-cA.second;
    float d2=dx*dx+dy*dy;
    if (d2>rr*rr) return std::nullopt;
    float d=std::sqrt(d2);
    float nx,ny;
    if (d<1e-12f){nx=0;ny=-1;}
    else{nx=dx/d;ny=dy/d;}
    Manifold m; m.nx=nx; m.ny=ny;
    float depth=rr-d;
    m.contacts[0]={cA.first+nx*A.radius,cA.second+ny*A.radius,depth,0,0,0};
    m.contact_count=1;
    return m;
}

static std::optional<Manifold> capsule_vs_poly(const Shape& cap, const Shape& poly) {
    // Test the capsule as a "swept circle": find closest point on polygon boundary
    // to the capsule's medial segment, then check against radius.
    Vec2 A = cap.world_pts[0], B = cap.world_pts[1];
    float r = cap.radius;
    const auto& verts = poly.verts;
    int n = (int)verts.size();
    if (n < 3) return std::nullopt;

    float best_depth = -1e30f;
    Vec2  best_norm  = {0, -1};
    Vec2  best_cp    = {cap.cx, cap.cy};

    // ① Test each polygon edge against the capsule medial segment
    for (int i = 0; i < n; ++i) {
        Vec2 ea = verts[i], eb = verts[(i+1)%n];
        // Edge normal (outward for CCW poly)
        float enx = eb.second - ea.second, eny = -(eb.first - ea.first);
        float elen = std::hypot(enx, eny);
        if (elen < 1e-12f) continue;
        enx /= elen; eny /= elen;

        // Closest point on edge to each capsule endpoint
        for (int k = 0; k < 2; ++k) {
            Vec2 P = (k == 0) ? A : B;
            auto [cp, d2] = closest_on_seg(ea.first, ea.second, eb.first, eb.second, P.first, P.second);
            float d = std::sqrt(d2);
            float depth = r - d;
            if (depth > best_depth) {
                best_depth = depth;
                // Normal from poly edge toward capsule endpoint
                if (d < 1e-12f) { best_norm = {enx, eny}; }
                else { best_norm = {(P.first - cp.first)/d, (P.second - cp.second)/d}; }
                best_cp = {cp.first + best_norm.first * (r - depth),
                           cp.second + best_norm.second * (r - depth)};
            }
        }

        // Also: closest point on medial segment to the edge midpoint
        Vec2 em = {(ea.first+eb.first)*0.5f, (ea.second+eb.second)*0.5f};
        auto [seg_cp, seg_d2] = closest_on_seg(A.first, A.second, B.first, B.second, em.first, em.second);
        float seg_d = std::sqrt(seg_d2);
        float depth2 = r - seg_d;
        if (depth2 > best_depth) {
            best_depth = depth2;
            if (seg_d < 1e-12f) { best_norm = {enx, eny}; }
            else { best_norm = {(em.first - seg_cp.first)/seg_d, (em.second - seg_cp.second)/seg_d}; }
            best_cp = {seg_cp.first + best_norm.first * r, seg_cp.second + best_norm.second * r};
        }
    }

    // ② Check if center of caps is inside polygon (full containment)
    if (point_in_poly(cap.cx, cap.cy, verts)) {
        // Use SAT axes to find shallowest penetration axis for extraction
        Shape cap_circ; cap_circ.kind=ShapeKind::Circle; cap_circ.entity=cap.entity;
        cap_circ.col=cap.col; cap_circ.cx=cap.cx; cap_circ.cy=cap.cy; cap_circ.radius=cap.radius;
        auto m = poly_vs_circle(poly, cap_circ);
        if (m) return m;
    }

    if (best_depth <= 0) return std::nullopt;

    Manifold man;
    man.nx = best_norm.first; man.ny = best_norm.second;
    man.contacts[0] = {best_cp.first, best_cp.second, best_depth, 0, 0, 0, 0};
    man.contact_count = 1;

    // ③ Try to generate a second contact point when the capsule's flat side
    //    is nearly parallel to a polygon edge (common for standing on a flat surface).
    //    We test both capsule endpoints against the best-normal direction and add the
    //    second one if it's also penetrating. This gives 2-point manifolds like Box2D.
    if (man.contact_count < MAX_CONTACTS) {
        // The "other" endpoint relative to best_cp
        Vec2 other_pt = (cap.world_pts[0].first != best_cp.first ||
                         cap.world_pts[0].second != best_cp.second - cap.radius * best_norm.second)
                        ? A : B;
        // Project other endpoint onto the contact normal and check penetration
        float ref_d = dot(best_cp.first - best_norm.first * cap.radius,
                          best_cp.second - best_norm.second * cap.radius,
                          best_norm.first, best_norm.second);
        // Closest poly point to the other cap endpoint
        auto [cp2, d2_2] = closest_on_poly(other_pt.first, other_pt.second, verts);
        float depth2 = cap.radius - std::sqrt(d2_2);
        if (depth2 > 0.f && depth2 > best_depth * 0.3f) {
            // Check that normal is similar (same face)
            Vec2 n2 = {0.f, 0.f};
            if (d2_2 > 1e-12f) {
                float d2s = std::sqrt(d2_2);
                n2 = {(other_pt.first - cp2.first)/d2s, (other_pt.second - cp2.second)/d2s};
            }
            float ndot = dot(n2.first, n2.second, best_norm.first, best_norm.second);
            if (ndot > 0.5f) {  // normals agree — same contact face
                CP cp2_cp;
                cp2_cp.x = cp2.first + best_norm.first * (cap.radius - depth2);
                cp2_cp.y = cp2.second + best_norm.second * (cap.radius - depth2);
                cp2_cp.depth = depth2;
                cp2_cp.fkey  = 1;
                man.contacts[man.contact_count++] = cp2_cp;
            }
        }
    }

    return man;
}

// ── 6e  Dispatch ─────────────────────────────────────────────────────────
static std::optional<Manifold> dispatch_shapes(Shape& s1, Shape& s2) {
    if (s1.kind==ShapeKind::Edge){
        std::optional<Manifold> best;
        bool one_sided = (s1.face_nx != 0.f || s1.face_ny != 0.f);
        for (int i=0;i+1<(int)s1.world_pts.size();++i){
            auto [ax,ay]=s1.world_pts[i]; auto [bx,by]=s1.world_pts[i+1];
            float t=s1.thickness;
            Shape seg; seg.kind=ShapeKind::Polygon; seg.entity=s1.entity; seg.col=s1.col;
            if (one_sided) {
                // One-sided slab: outer face lies exactly on the edge surface,
                // inner face extends t*2 into the tile interior (in face_n direction).
                // This prevents SAT from ever choosing the "push into tile" axis.
                float fnx=s1.face_nx, fny=s1.face_ny; // outward normal
                float inx=-fnx, iny=-fny;               // inward (into tile)
                // Outer edge points (on the surface)
                float ox1=ax, oy1=ay, ox2=bx, oy2=by;
                // Inner edge points (shifted inward by 2*t)
                float ix1=ax+inx*t*2.f, iy1=ay+iny*t*2.f;
                float ix2=bx+inx*t*2.f, iy2=by+iny*t*2.f;
                seg.verts=ensure_ccw({{ox1,oy1},{ox2,oy2},{ix2,iy2},{ix1,iy1}});
            } else {
                // Two-sided fallback (non-tilemap edges like EdgeCollider2D components)
                auto [nx,ny]=normalize(by-ay,ax-bx);
                seg.verts=ensure_ccw({{ax+nx*t,ay+ny*t},{bx+nx*t,by+ny*t},{bx-nx*t,by-ny*t},{ax-nx*t,ay-ny*t}});
            }
            seg.cx=(ax+bx)*0.5f; seg.cy=(ay+by)*0.5f;
            auto m=dispatch_shapes(seg,s2);
            if (m&&m->contact_count>0){
                // For one-sided edges: discard contacts where the manifold normal
                // points inward (into the tile). This is the final guard against
                // SAT resolving in the wrong direction at corner/edge transitions.
                if (one_sided) {
                    float dot = m->nx*s1.face_nx + m->ny*s1.face_ny;
                    if (dot < 0.f) { m->nx=-m->nx; m->ny=-m->ny; }
                }
                if (!best||m->contacts[0].depth>best->contacts[0].depth) best=m;
            }
        }
        return best;
    }
    if (s2.kind==ShapeKind::Edge){
        auto m=dispatch_shapes(s2,s1);
        if (m){m->nx=-m->nx;m->ny=-m->ny;}
        return m;
    }

    // Circle-Circle
    if (s1.kind==ShapeKind::Circle&&s2.kind==ShapeKind::Circle){
        float dx=s2.cx-s1.cx,dy=s2.cy-s1.cy,rr=s1.radius+s2.radius;
        float d2=dx*dx+dy*dy;
        if (d2>=rr*rr) return std::nullopt;
        float d=std::sqrt(d2); if(d<1e-12f){d=1e-9f;dy=1;}
        float nx=dx/d,ny=dy/d;
        Manifold m; m.nx=nx;m.ny=ny;
        m.contacts[0]={s1.cx+nx*s1.radius,s1.cy+ny*s1.radius,rr-d,0,0,0};
        m.contact_count=1;
        return m;
    }
    // Circle-Poly
    if (s1.kind==ShapeKind::Circle&&s2.kind==ShapeKind::Polygon){
        auto m=poly_vs_circle(s2,s1); if(m){m->nx=-m->nx;m->ny=-m->ny;} return m;
    }
    if (s1.kind==ShapeKind::Polygon&&s2.kind==ShapeKind::Circle) return poly_vs_circle(s1,s2);
    // Poly-Poly
    if (s1.kind==ShapeKind::Polygon&&s2.kind==ShapeKind::Polygon) return poly_vs_poly(s1,s2);
    // Capsule combos
    if (s1.kind==ShapeKind::Capsule&&s2.kind==ShapeKind::Capsule) return capsule_vs_capsule(s1,s2);
    if (s1.kind==ShapeKind::Capsule&&s2.kind==ShapeKind::Circle)  return capsule_vs_circle(s1,s2);
    if (s1.kind==ShapeKind::Circle&&s2.kind==ShapeKind::Capsule){ auto m=capsule_vs_circle(s2,s1); if(m){m->nx=-m->nx;m->ny=-m->ny;} return m; }
    if (s1.kind==ShapeKind::Capsule&&s2.kind==ShapeKind::Polygon) return capsule_vs_poly(s1,s2);
    if (s1.kind==ShapeKind::Polygon&&s2.kind==ShapeKind::Capsule){ auto m=capsule_vs_poly(s2,s1); if(m){m->nx=-m->nx;m->ny=-m->ny;} return m; }
    return std::nullopt;
}

// ════════════════════════════════════════════════════════════════════════════
//  7.  BROAD PHASE
// ════════════════════════════════════════════════════════════════════════════
using ShapePair=std::tuple<Entity*,Shape,Entity*,Shape>;

// Shared pair-acceptance filter used by both the uniform-grid and the
// dynamic-AABB-tree broadphase, so the two backends can never silently
// diverge in which pairs they consider collidable. Returns false to reject
// (skip) a broadphase-overlapping candidate pair.
static bool accept_broadphase_pair(Entity* e1, const Shape& s1, Entity* e2, const Shape& s2) {
    int id1=e1->value("id",0), id2=e2->value("id",0);
    if (is_collision_ignored(id1, id2)) return false;
    if (is_collider_ignored(id1, s1.sub_id, id2, s2.sub_id)) return false;
    auto* rb1=has_component(*e1,"Rigidbody2D")?&(*e1)["components"]["Rigidbody2D"]:nullptr;
    auto* rb2=has_component(*e2,"Rigidbody2D")?&(*e2)["components"]["Rigidbody2D"]:nullptr;
    if (rb1&&!rb1->value("simulated",true)) return false;
    if (rb2&&!rb2->value("simulated",true)) return false;
    int la=rb1?rb1->value("layer",0):0;
    int lb=rb2?rb2->value("layer",0):0;
    if (!global_collision_matrix().can_collide(la,lb)) return false;
    return true;
}

// Per-shape speculative-margin AABB expansion (velocity * step + margin for
// CCD-opted-in dynamic bodies). Shared by both broadphase backends.
static std::tuple<float,float,float,float> speculative_aabb(Entity& e, const Shape& sh) {
    auto [x1,y1,x2,y2]=shape_aabb(sh);
    float spec = 0.f;
    if (has_component(e,"Rigidbody2D")) {
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) == "dynamic" && (rb.value("continuous_collision",false) || rb.value("ccd",false))) {
            float vx = finite_val(rb.value("velocity_x",0.f));
            float vy = finite_val(rb.value("velocity_y",0.f));
            spec = std::hypot(vx,vy) * TARGET_STEP + SPECULATIVE_MARGIN;
        }
    }
    return {x1-spec, y1-spec, x2+spec, y2+spec};
}

static std::vector<ShapePair> broad_phase_grid(EntityList& entities) {
    using Key2=std::pair<int,int>;
    struct PH{size_t operator()(Key2 k)const{return std::hash<long long>()((long long)k.first<<32|k.second);}};\
    std::unordered_map<Key2,std::vector<std::pair<Entity*,Shape>>,PH> grid;

    for (auto& e:entities){
        if (!entity_active(e)) continue;
        // ── Sleeping bodies excluded from broadphase (gap fix) ──────────────
        // Sleeping dynamic bodies skip integration and cannot generate new
        // contacts this step; inserting them into the grid wastes cell-bucket
        // lookups and pair deduplication. Static/kinematic bodies still need
        // to be present so sleeping bodies can be WOKEN by a new intruder.
        if (has_component(e,"Rigidbody2D")) {
            auto& rb = e["components"]["Rigidbody2D"];
            if (body_type(rb)=="dynamic" && rb.value("_sleeping",false)) continue;
        }
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            auto [x1,y1,x2,y2]=speculative_aabb(e,sh);
            int cx1=(int)std::floor(x1)/CELL_SIZE,cy1=(int)std::floor(y1)/CELL_SIZE;
            int cx2=(int)std::floor(x2)/CELL_SIZE,cy2=(int)std::floor(y2)/CELL_SIZE;
            for (int cx=cx1;cx<=cx2;++cx)
                for (int cy=cy1;cy<=cy2;++cy)
                    grid[{cx,cy}].push_back({&e,sh});
        }
    }

    std::unordered_set<long long> seen;
    std::vector<ShapePair> pairs;
    for (auto& [cell,bucket]:grid){
        for (int i=0;i<(int)bucket.size();++i)
          for (int j=i+1;j<(int)bucket.size();++j){
            Entity* e1=bucket[i].first; Entity* e2=bucket[j].first;
            const Shape& s1=bucket[i].second; const Shape& s2=bucket[j].second;
            if (!accept_broadphase_pair(e1,s1,e2,s2)) continue;
            int id1=e1->value("id",0),id2=e2->value("id",0);
            int sub1=s1.sub_id,sub2=s2.sub_id;
            int lo=std::min(id1,id2),hi=std::max(id1,id2);
            long long key=((long long)lo<<32|(unsigned)hi)^((long long)sub1*31+sub2);
            if (seen.count(key)) continue;
            seen.insert(key);
            pairs.push_back({e1,s1,e2,s2});
          }
    }
    return pairs;
}

// ─── Dynamic AABB Tree (Phase 3 — Box2D b2DynamicTree equivalent) ────────────
// A simple top-down-rebuilt dynamic AABB tree. Unlike Box2D's incrementally-
// refit tree (which needs persistent per-body node handles surviving across
// frames — a larger structural change for this engine's per-step shape
// collection model), this rebuilds the tree fresh each broadphase call from
// the current shape AABBs using a median-split build, which is still
// O(n log n) overall (build + query) versus the uniform grid's effective
// O(n²) worst case when bodies cluster into few cells. For scenes with
// >~200 bodies (the threshold the gap doc calls out) or uneven spatial
// distribution, this scales much better than the grid; for small/evenly
// -spread scenes the grid's lower constant factor still wins, which is why
// both backends are kept and selectable (see set_broadphase_mode /
// set_broadphase_auto_threshold).
namespace bvh_detail {
    struct Node {
        float x1,y1,x2,y2;
        int left=-1, right=-1;     // child node indices, -1 = leaf
        int shape_idx=-1;          // index into the flat shape list, leaves only
    };

    static bool aabb_overlap(float ax1,float ay1,float ax2,float ay2,
                              float bx1,float by1,float bx2,float by2) {
        return ax1<=bx2 && ax2>=bx1 && ay1<=by2 && ay2>=by1;
    }

    // Builds a tree over shapes[lo,hi) by recursively splitting on the
    // longer axis at the median centroid. Returns the index of the new node.
    static int build(std::vector<Node>& nodes, std::vector<int>& idx,
                      const std::vector<std::tuple<float,float,float,float>>& aabbs,
                      int lo, int hi) {
        Node n;
        n.x1=1e30f; n.y1=1e30f; n.x2=-1e30f; n.y2=-1e30f;
        for (int i=lo;i<hi;++i){
            auto [x1,y1,x2,y2]=aabbs[idx[i]];
            n.x1=std::min(n.x1,x1); n.y1=std::min(n.y1,y1);
            n.x2=std::max(n.x2,x2); n.y2=std::max(n.y2,y2);
        }
        if (hi-lo<=1) {
            n.shape_idx=idx[lo];
            nodes.push_back(n);
            return (int)nodes.size()-1;
        }
        float w=n.x2-n.x1, h=n.y2-n.y1;
        int mid=lo+(hi-lo)/2;
        if (w>=h) {
            std::nth_element(idx.begin()+lo, idx.begin()+mid, idx.begin()+hi,
                [&](int a,int b){ return (std::get<0>(aabbs[a])+std::get<2>(aabbs[a])) <
                                          (std::get<0>(aabbs[b])+std::get<2>(aabbs[b])); });
        } else {
            std::nth_element(idx.begin()+lo, idx.begin()+mid, idx.begin()+hi,
                [&](int a,int b){ return (std::get<1>(aabbs[a])+std::get<3>(aabbs[a])) <
                                          (std::get<1>(aabbs[b])+std::get<3>(aabbs[b])); });
        }
        int self=(int)nodes.size();
        nodes.push_back(n); // placeholder, children appended after
        int left=build(nodes,idx,aabbs,lo,mid);
        int right=build(nodes,idx,aabbs,mid,hi);
        nodes[self].left=left; nodes[self].right=right;
        return self;
    }

    // Collects all leaf shape indices whose AABB overlaps the query box,
    // descending only into subtrees whose bound also overlaps.
    static void query(const std::vector<Node>& nodes, int node_idx,
                       float qx1,float qy1,float qx2,float qy2,
                       std::vector<int>& out) {
        if (node_idx<0) return;
        const Node& n=nodes[node_idx];
        if (!aabb_overlap(n.x1,n.y1,n.x2,n.y2,qx1,qy1,qx2,qy2)) return;
        if (n.shape_idx>=0) { out.push_back(n.shape_idx); return; }
        query(nodes,n.left,qx1,qy1,qx2,qy2,out);
        query(nodes,n.right,qx1,qy1,qx2,qy2,out);
    }
}

static std::vector<ShapePair> broad_phase_bvh(EntityList& entities) {
    // Flatten all shapes from all active entities into one indexable list.
    std::vector<std::pair<Entity*,Shape>> flat;
    std::vector<std::tuple<float,float,float,float>> aabbs;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        // Sleeping dynamic bodies excluded from broadphase (see broad_phase_grid comment)
        if (has_component(e,"Rigidbody2D")) {
            auto& rb = e["components"]["Rigidbody2D"];
            if (body_type(rb)=="dynamic" && rb.value("_sleeping",false)) continue;
        }
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            aabbs.push_back(speculative_aabb(e,sh));
            flat.push_back({&e,sh});
        }
    }
    if (flat.empty()) return {};

    std::vector<bvh_detail::Node> nodes;
    nodes.reserve(flat.size()*2);
    std::vector<int> idx(flat.size());
    for (int i=0;i<(int)idx.size();++i) idx[i]=i;
    int root = bvh_detail::build(nodes, idx, aabbs, 0, (int)idx.size());

    std::unordered_set<long long> seen;
    std::vector<ShapePair> pairs;
    // For each shape, query the tree for overlapping shapes and only keep
    // candidates with a higher flat-index (i<j) to dedupe each pair once —
    // mirrors the i<j convention the grid backend uses per-cell.
    for (int i=0;i<(int)flat.size();++i){
        auto [x1,y1,x2,y2]=aabbs[i];
        std::vector<int> hits;
        bvh_detail::query(nodes, root, x1,y1,x2,y2, hits);
        for (int j:hits){
            if (j<=i) continue;
            Entity* e1=flat[i].first; Entity* e2=flat[j].first;
            if (e1==e2) continue; // shapes on the same entity never collide with each other
            const Shape& s1=flat[i].second; const Shape& s2=flat[j].second;
            if (!accept_broadphase_pair(e1,s1,e2,s2)) continue;
            int id1=e1->value("id",0),id2=e2->value("id",0);
            int sub1=s1.sub_id,sub2=s2.sub_id;
            int lo=std::min(id1,id2),hi=std::max(id1,id2);
            long long key=((long long)lo<<32|(unsigned)hi)^((long long)sub1*31+sub2);
            if (seen.count(key)) continue;
            seen.insert(key);
            pairs.push_back({e1,s1,e2,s2});
        }
    }
    return pairs;
}

static BroadphaseMode s_broadphase_mode = BroadphaseMode::UniformGrid;
static int s_broadphase_auto_threshold = 200; // matches gap doc's "~200 bodies" callout

void set_broadphase_mode(BroadphaseMode mode) { s_broadphase_mode = mode; }
BroadphaseMode get_broadphase_mode() { return s_broadphase_mode; }
void set_broadphase_auto_threshold(int body_count) { s_broadphase_auto_threshold = body_count; }

static std::vector<ShapePair> broad_phase(EntityList& entities) {
    BroadphaseMode mode = s_broadphase_mode;
    if (s_broadphase_auto_threshold >= 0) {
        int n=0;
        for (auto& e:entities) { if (entity_active(e)) ++n; if (n>s_broadphase_auto_threshold) break; }
        mode = (n>s_broadphase_auto_threshold) ? BroadphaseMode::DynamicTree : s_broadphase_mode;
    }
    return (mode==BroadphaseMode::DynamicTree) ? broad_phase_bvh(entities) : broad_phase_grid(entities);
}

// ════════════════════════════════════════════════════════════════════════════
//  8.  MATERIAL COMBINING
// ════════════════════════════════════════════════════════════════════════════
static float combine(float a, float b, CombineMode m) {
    switch(m){
        case CombineMode::Minimum:  return std::min(a,b);
        case CombineMode::Maximum:  return std::max(a,b);
        case CombineMode::Multiply: return a*b;
        default:                    return (a+b)*0.5f;
    }
}
static CombineMode dominant(CombineMode a, CombineMode b) {
    // Unity priority: Average < Minimum < Maximum < Multiply
    return (int)a>(int)b?a:b;
}

// ════════════════════════════════════════════════════════════════════════════
//  9.  NARROW PHASE
// ════════════════════════════════════════════════════════════════════════════
static std::vector<Manifold> narrow_phase(std::vector<ShapePair>& pairs, ContactCache& cache) {
    std::vector<Manifold> mfds;
    for (auto& [pe1,s1,pe2,s2]:pairs){
        auto& e1=*pe1; auto& e2=*pe2;
        Entity dummy=Entity::object();
        auto& rb1c=has_component(e1,"Rigidbody2D")?e1["components"]["Rigidbody2D"]:dummy;
        auto& rb2c=has_component(e2,"Rigidbody2D")?e2["components"]["Rigidbody2D"]:dummy;

        int l1=rb1c.value("layer",0), l2=rb2c.value("layer",0);
        // layer_mask filtering: only apply when the entity actually HAS a
        // Rigidbody2D with an explicit layer_mask. Static geometry (no RB, or
        // RB with default mask 0xFFFF / -1) must always collide regardless of
        // what layer the dynamic body is on — otherwise changing the player's
        // layer_mask causes it to fall through floors that have no Rigidbody2D.
        bool has_rb1 = has_component(e1,"Rigidbody2D");
        bool has_rb2 = has_component(e2,"Rigidbody2D");
        int m1 = has_rb1 ? rb1c.value("layer_mask", -1) : -1;
        int m2 = has_rb2 ? rb2c.value("layer_mask", -1) : -1;
        // -1 / 0xFFFFFFFF means "not set" → treat as "collide with all"
        if (m1 != -1 && !(m1 & (1<<l2))) continue;
        if (m2 != -1 && !(m2 & (1<<l1))) continue;
        if (s_col_matrix.ignores(l1,l2)) continue;

        Shape ms1=s1,ms2=s2;
        auto res=dispatch_shapes(ms1,ms2);
        if (!res||res->contact_count==0) continue;

        auto& m=*res;
        m.e1=pe1; m.e2=pe2; m.col1=s1.col; m.col2=s2.col;
        m.sub1=shape_cache_subkey(s1); m.sub2=shape_cache_subkey(s2);
        m.rb1=has_component(e1,"Rigidbody2D")?&e1["components"]["Rigidbody2D"]:nullptr;
        m.rb2=has_component(e2,"Rigidbody2D")?&e2["components"]["Rigidbody2D"]:nullptr;
        m.is_trigger=(s1.col&&s1.col->value("is_trigger",false))||
                     (s2.col&&s2.col->value("is_trigger",false));

        // SurfaceEffector2D: conveyor-belt surface speed
        // Store in manifold so velocity solver can offset relative tangential velocity.
        float surface_speed = 0.f;
        auto check_surface_effector = [&](Entity* ent) {
            if (!ent || !has_component(*ent,"SurfaceEffector2D")) return;
            auto& se = (*ent)["components"]["SurfaceEffector2D"];
            surface_speed = finite_val(se.value("speed", 0.f));
        };
        check_surface_effector(&e1);
        check_surface_effector(&e2);
        // Surface speed is stored as a temporary field on the manifold
        // (we reuse friction_d as storage carrier; see velocity solver)
        // Actually store on sub2 scratch field – use restitution field sign trick:
        // We tag manifolds with surface speed via a dedicated member (added to Manifold in hpp)
        m.surface_speed = surface_speed;

        // Material combine
        auto fc=dominant(s1.material.friction_combine,s2.material.friction_combine);
        auto bc=dominant(s1.material.bounce_combine,  s2.material.bounce_combine);
        float fd=combine(s1.material.friction,   s2.material.friction,   fc);
        m.friction_d=fd; m.friction_s=fd*STATIC_MU_MULT;
        m.restitution=combine(s1.material.bounciness,s2.material.bounciness,bc);

        // Angle-based one-way platform (Unity uses ~30°)
        auto check_ow = [&](Entity* pf_entity, Entity* body_rb, float normal_side) {
            if (!pf_entity) return;
            auto* pf=has_component(*pf_entity,"PlatformEffector2D")?&(*pf_entity)["components"]["PlatformEffector2D"]:nullptr;
            if (!pf||!pf->value("use_one_way",true)) return;
            float arc = pf->value("surface_arc",180.f) * (float)M_PI/360.f; // half arc
            // Normal must point upward-ish (within arc) and body must be falling onto it
            float norm_dot = dot(m.nx,m.ny,0.f,normal_side);
            if (norm_dot < std::cos(arc)) m.one_way_skip=true;
            else if (body_rb&&body_rb->value("velocity_y",0.f)*normal_side<0.f)
                m.one_way_skip=true;

            // PlatformEffector2D bounce enhancement
            if (!m.one_way_skip && pf->value("use_bouncy",false)){
                float bounce = finite_val(pf->value("bounciness", 0.5f));
                m.restitution = std::max(m.restitution, bounce);
            }
        };
        if (!m.one_way_skip){
            check_ow(&e2,m.rb1, 1.f);  // body1 coming from above
            check_ow(&e1,m.rb2,-1.f);
        }

        // Warm-start: copy cached lambdas by feature key (scale by WARM_START_SCALE per Box2D)
        int id1=e1.value("id",0),id2=e2.value("id",0);
        long long ckey = contact_cache_key(id1, id2, shape_cache_subkey(s1), shape_cache_subkey(s2));
        auto it=cache.find(ckey);
        if (it!=cache.end()&&!m.is_trigger){
            auto& cm=it->second;
            // Contact normal smoothing: blend current normal toward cached smooth normal
            // to suppress jitter at curved/tiled surfaces
            if (cm.age > 0) {
                float blend = std::min(NORMAL_SMOOTH_ALPHA * (float)cm.age, 0.7f);
                float sn_dot = m.nx * cm.smooth_nx + m.ny * cm.smooth_ny;
                if (sn_dot > 0.7f) { // same face: smooth (was 0.3 — too loose, blended across face boundaries)
                    m.nx = m.nx * (1.f - blend) + cm.smooth_nx * blend;
                    m.ny = m.ny * (1.f - blend) + cm.smooth_ny * blend;
                    float nl = std::hypot(m.nx, m.ny);
                    if (nl > 1e-9f) { m.nx /= nl; m.ny /= nl; }
                }
            }
            // Match by fkey and warm-start (scaled to avoid over-shoot)
            for (int ci=0;ci<m.contact_count;++ci){
                for (int pi=0;pi<cm.count;++pi){
                    if (cm.contacts[pi].fkey==m.contacts[ci].fkey){
                        float prev_ln = cm.contacts[pi].lambda_n;
                        float new_ln  = cm.contacts[pi].lambda_n * WARM_START_SCALE;
                        m.contacts[ci].lambda_n = new_ln;
                        // ── Tangent warm-start ratio scaling (gap fix) ────────────
                        // When the normal impulse changes significantly between frames
                        // (e.g. object lands harder), the friction cone changes too.
                        // Scale lambda_t by the same ratio to keep it inside the cone,
                        // preventing the friction-drift artifact where lambda_t is
                        // warm-started outside the new cone and wastes an iteration
                        // correcting backward before converging.
                        float ratio = (prev_ln > 1e-9f) ? (new_ln / prev_ln) : WARM_START_SCALE;
                        ratio = std::clamp(ratio, 0.f, 1.0f); // cap to avoid amplification (was 1.5 — caused friction overshoot)
                        m.contacts[ci].lambda_t = cm.contacts[pi].lambda_t * WARM_START_SCALE * ratio;
                        break;
                    }
                }
            }
        }

        mfds.push_back(std::move(m));
    }
    return mfds;
}

// ════════════════════════════════════════════════════════════════════════════
//  10.  INTEGRATION (Semi-implicit Euler + gravity zones)
// ════════════════════════════════════════════════════════════════════════════
static void integrate(EntityList& entities, float dt, float global_grav) {
    // Store previous state for interpolation. Stale records are pruned once
    // per outer physics step, rather than once for every adaptive substep.
    for (auto& e:entities){
        if (!entity_active(e)||!has_component(e,"Transform")) continue;
        int eid=e.value("id",0);
        if (eid <= 0) continue;
        auto& trans=e["components"]["Transform"];
        s_prev_state[eid]={{get_float(trans,"x"),get_float(trans,"y")}, get_float(trans,"rotation")};
    }

    for (auto& e:entities){
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb=e["components"]["Rigidbody2D"];
        auto& trans=e["components"]["Transform"];
        std::string bt=body_type(rb);
        if (bt=="static") continue;
        if (rb.value("_sleeping",false)&&bt=="dynamic") continue;

        float vx=finite_val(rb.contains("vx") ? (float)rb["vx"] : rb.value("velocity_x",0.f));
        float vy=finite_val(rb.contains("vy") ? (float)rb["vy"] : rb.value("velocity_y",0.f));
        float av=finite_val(rb.value("angular_velocity",0.f));
        auto constr=rb.value("constraints",Entity::object());

        if (bt=="dynamic"){
            float mass=std::max(finite_val(rb.value("mass",1.f),1.f),1e-9f);
            float inv_m=1.f/mass;
            float fx=finite_val(rb.value("_force_x",0.f)); rb.erase("_force_x");
            float fy=finite_val(rb.value("_force_y",0.f)); rb.erase("_force_y");
            float torque=finite_val(rb.value("_torque",0.f)); rb.erase("_torque");
            // Clear frame accumulators (totalForce / totalTorque reset each step)
            rb["_acc_force_x"] = 0.f;
            rb["_acc_force_y"] = 0.f;
            rb["_acc_torque"]  = 0.f;

            // Gravity: check for AreaEffector2D override, then global direction override
            float grav = global_grav * finite_val(rb.value("gravity_scale",1.f),1.f);
            float gx = finite_val(rb.value("_grav_x", 0.f));
            float gy = finite_val(rb.value("_grav_y", grav));
            bool had_area_grav = rb.contains("_grav_x");
            if (rb.contains("_grav_x")) rb.erase("_grav_x");
            if (rb.contains("_grav_y")) rb.erase("_grav_y");

            // If no per-body/area gravity was set this frame, apply the global
            // gravity direction (set by physics_ext set_gravity_vector).
            if (!had_area_grav && s_gravity_override_active) {
                float gs = finite_val(rb.value("gravity_scale", 1.f), 1.f);
                gx = s_gravity_override_x * gs;
                gy = s_gravity_override_y * gs;
            }

            bool freeze_x = constr.value("freeze_pos_x",false);
            bool freeze_y = constr.value("freeze_pos_y",false);
            bool freeze_rot = rb.value("freeze_rotation",false);

            if (!freeze_x) fx += gx*mass;
            if (!freeze_y) fy += gy*mass;

            // Exponential drag (more accurate than linear multiply at large dt)
            // Unity uses rb.drag; formula: v *= exp(-drag * dt) which is stable
            float drag=std::max(0.f,finite_val(rb.value("drag",0.05f),0.05f));
            float damp = std::exp(-drag * dt);
            vx = vx * damp + fx * inv_m * dt;
            vy = vy * damp + fy * inv_m * dt;

            // Speed cap
            float spd2=vx*vx+vy*vy;
            if (spd2>MAX_LINEAR_VEL*MAX_LINEAR_VEL){float sc=MAX_LINEAR_VEL/std::sqrt(spd2);vx*=sc;vy*=sc;}
            if (freeze_x) vx=0;
            if (freeze_y) vy=0;
            rb["velocity_x"]=vx; rb["velocity_y"]=vy; rb["vx"]=vx; rb["vy"]=vy;

            if (!freeze_rot){
                float inertia_def=compute_inertia(e,mass);
                float inertia=std::max(finite_val(rb.value("inertia",inertia_def),inertia_def),1e-9f);
                // Exponential angular drag
                float ang_drag=std::max(0.f,finite_val(rb.value("angular_drag",0.05f),0.05f));
                float adamp = std::exp(-ang_drag * dt);
                av = av * adamp + torque / inertia * dt;
                av = clamp(av,-MAX_ANGULAR_VEL,MAX_ANGULAR_VEL);
                rb["angular_velocity"]=av;
                trans["rotation"]=finite_val(get_float(trans,"rotation"))+av*(180.f/(float)M_PI)*dt;
            } else {
                rb["angular_velocity"]=0.f;
            }

            // Energy-based sleep EMA (smoothed kinetic energy per unit mass)
            float v2 = vx*vx + vy*vy;
            float av2 = finite_val(rb.value("angular_velocity",0.f));
            float ke = 0.5f * (v2 + av2 * av2 * 100.f); // approximate rotational contribution
            float ke_avg = finite_val(rb.value("_sleep_energy", ke));
            ke_avg = ke_avg * (1.f - SLEEP_ENERGY_AVG_ALPHA) + ke * SLEEP_ENERGY_AVG_ALPHA;
            rb["_sleep_energy"] = ke_avg;
            if (ke_avg < SLEEP_ENERGY_THRESH)
                rb["_sleep_t"] = rb.value("_sleep_t",0.f) + dt;
            else
                rb["_sleep_t"] = 0.f;

            // COM offset: correct position to rotate about COM not origin
            float com_x = finite_val(rb.value("center_of_mass_x", 0.f));
            float com_y = finite_val(rb.value("center_of_mass_y", 0.f));
            if ((com_x != 0.f || com_y != 0.f) && !freeze_rot) {
                // The velocity of the origin vs COM differs by av × com_offset
                float dav = finite_val(rb.value("angular_velocity",0.f)) * (float)M_PI / 180.f;
                // Rotation of COM offset adds a correction to linear velocity
                float rot_rad = get_float(trans,"rotation") * (float)M_PI / 180.f;
                float c = std::cos(rot_rad), s2 = std::sin(rot_rad);
                float wcx = com_x * c - com_y * s2;
                float wcy = com_x * s2 + com_y * c;
                // v_origin = v_com + omega × r_com; already handled by solver; just record
                rb["_com_wx"] = wcx; rb["_com_wy"] = wcy;
            }

        } else {
            // Kinematic: freeze constraints
            if (constr.value("freeze_pos_x",false)) vx=0;
            if (constr.value("freeze_pos_y",false)) vy=0;
            rb["velocity_x"]=vx; rb["velocity_y"]=vy; rb["vx"]=vx; rb["vy"]=vy;
            if (!rb.value("freeze_rotation",false))
                trans["rotation"]=finite_val(get_float(trans,"rotation"))+av*(180.f/(float)M_PI)*dt;
        }
        if (!constr.value("freeze_pos_x",false)) trans["x"]=finite_val(get_float(trans,"x"))+vx*dt;
        if (!constr.value("freeze_pos_y",false)) trans["y"]=finite_val(get_float(trans,"y"))+vy*dt;
        int eid = e.value("id", 0);
        transform::mark_local_dirty(eid);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  11.  SOLVER HELPERS
// ════════════════════════════════════════════════════════════════════════════
static float body_inv_mass(const Entity* rb){
    if (!rb||body_type(*rb)!="dynamic") return 0.f;
    return 1.f/std::max(finite_val(rb->value("mass",1.f),1.f),1e-9f);
}
static float body_inv_inertia(const Entity* rb, const Entity* e=nullptr){
    if (!rb||body_type(*rb)!="dynamic"||rb->value("freeze_rotation",false)) return 0.f;
    float m=std::max(finite_val(rb->value("mass",1.f),1.f),1e-9f);
    float I_def = e ? compute_inertia(*e,m) : m*100.f*100.f/6.f;
    return 1.f/std::max(finite_val(rb->value("inertia",I_def),I_def),1e-9f);
}
static std::pair<float,float> point_vel(const Entity* rb, float rx, float ry){
    if (!rb) return {0,0};
    float av=finite_val(rb->value("angular_velocity",0.f));
    return {rb->value("velocity_x",0.f)-av*ry, rb->value("velocity_y",0.f)+av*rx};
}

// ─── Generic manifold-range helper ────────────────────────────────────────────
// solve_velocities/correct_positions are templated on the manifold container
// so they work both on a flat std::vector<Manifold>& (the non-island path)
// and on a std::vector<Manifold*>& (one island's slice, Phase 3). deref()
// normalizes "an element of the range" to a Manifold& either way.
static inline Manifold& deref(Manifold& m){ return m; }
static inline Manifold& deref(Manifold* m){ return *m; }

// ════════════════════════════════════════════════════════════════════════════
//  12.  VELOCITY SOLVER  (Sequential Impulses + warm-starting)
// ════════════════════════════════════════════════════════════════════════════
template<typename MfdRange>
static void solve_velocities(MfdRange& mfds, float dt) {
    for (auto& m_:mfds){
        auto& m = deref(m_);
        if (m.is_trigger||m.one_way_skip) continue;

        // Pre-solve callback (allow modifying manifold before solving)
        if (s_contact_listener.pre_solve){
            s_contact_listener.pre_solve(m.e1, m.e2, m, 0.f); // dt not used in pre-solve
        }

        float inv_m1=body_inv_mass(m.rb1), inv_m2=body_inv_mass(m.rb2);
        if (inv_m1+inv_m2<=1e-12f) continue;
        float inv_I1=body_inv_inertia(m.rb1,m.e1), inv_I2=body_inv_inertia(m.rb2,m.e2);
        bool fr1=!m.rb1||is_static(m.rb1)||m.rb1->value("freeze_rotation",false);
        bool fr2=!m.rb2||is_static(m.rb2)||m.rb2->value("freeze_rotation",false);
        float nx=m.nx, ny=m.ny, tx=-ny, ty=nx;

        Entity _dt1=Entity::object(),_dt2=Entity::object();
        auto& t1c=m.e1&&has_component(*m.e1,"Transform")?(*m.e1)["components"]["Transform"]:_dt1;
        auto& t2c=m.e2&&has_component(*m.e2,"Transform")?(*m.e2)["components"]["Transform"]:_dt2;
        bool ht1=m.e1&&has_component(*m.e1,"Transform");
        bool ht2=m.e2&&has_component(*m.e2,"Transform");

        for (int ci=0;ci<m.contact_count;++ci){
            auto& cp=m.contacts[ci];
            float t1x=ht1?get_float(t1c,"x"):0.f, t1y=ht1?get_float(t1c,"y"):0.f;
            float t2x=ht2?get_float(t2c,"x"):0.f, t2y=ht2?get_float(t2c,"y"):0.f;
            float r1x=cp.x-t1x,r1y=cp.y-t1y;
            float r2x=cp.x-t2x,r2y=cp.y-t2y;

            float r1n=cross(r1x,r1y,nx,ny),r2n=cross(r2x,r2y,nx,ny);
            float r1t=cross(r1x,r1y,tx,ty),r2t=cross(r2x,r2y,tx,ty);

            // ── Per-contact effective mass caching (Phase gap fix) ────────────
            // K is constant within a substep (body mass/inertia and lever arms
            // don't change between velocity iterations). Cache it once on the
            // first call and reuse every subsequent iteration — eliminates 8×
            // redundant mul/add chains per contact per substep.
            if (!cp.k_cached) {
                float Kn = inv_m1+inv_m2
                    + (fr1?0.f:r1n*r1n*inv_I1)
                    + (fr2?0.f:r2n*r2n*inv_I2);
                float Kt = inv_m1+inv_m2
                    + (fr1?0.f:r1t*r1t*inv_I1)
                    + (fr2?0.f:r2t*r2t*inv_I2);
                cp.k_normal  = (Kn > 1e-12f) ? (1.f/Kn) : 0.f;
                cp.k_tangent = (Kt > 1e-12f) ? (1.f/Kt) : 0.f;
                cp.k_cached  = true;
                // Populate ContactPoint2D.separationDistance (signed; negative = penetrating)
                cp.separation_distance = -cp.depth;
                // Populate ContactPoint2D.bounciness from manifold combined restitution
                cp.bounciness = m.restitution;
            }
            if (cp.k_normal < 1e-12f) continue;

            auto [v1x,v1y]=point_vel(inv_m1>0?m.rb1:nullptr,r1x,r1y);
            auto [v2x,v2y]=point_vel(inv_m2>0?m.rb2:nullptr,r2x,r2y);
            float rvx=v2x-v1x,rvy=v2y-v1y;
            float vn=dot(rvx,rvy,nx,ny);

            // Only apply restitution if the relative normal velocity is significant.
            // Using |vn| (contact-relative, not global speed) is the correct Box2D approach —
            // it prevents ghost bounces on resting contacts and jitter on slow collisions.
            // Phase 1 fix: use compute_smooth_restitution() for a quintic falloff near the
            // threshold, eliminating micro-bounce on slow-moving resting contacts.
            float rest = compute_smooth_restitution(m.restitution, vn);
            float jn=-(1.f+rest)*vn*cp.k_normal;   // use cached 1/K
            float old_n=cp.lambda_n;
            cp.lambda_n=std::max(0.f,old_n+jn);
            jn=cp.lambda_n-old_n;

            auto apply=[&](Entity* rb, float sign, float rx, float ry, float im, float iI, bool fr){\
                if (!rb||im<=0) return;
                (*rb)["velocity_x"]=(float)(*rb)["velocity_x"]+sign*jn*im*nx;
                (*rb)["velocity_y"]=(float)(*rb)["velocity_y"]+sign*jn*im*ny;
                if (!fr) (*rb)["angular_velocity"]=(float)(*rb)["angular_velocity"]+sign*jn*iI*cross(rx,ry,nx,ny);
            };
            apply(m.rb1,-1,r1x,r1y,inv_m1,inv_I1,fr1);
            apply(m.rb2,+1,r2x,r2y,inv_m2,inv_I2,fr2);

            // Friction tangential velocity (must come before wall-cling)
            auto [v1x2,v1y2]=point_vel(inv_m1>0?m.rb1:nullptr,r1x,r1y);
            auto [v2x2,v2y2]=point_vel(inv_m2>0?m.rb2:nullptr,r2x,r2y);
            float vt=dot(v2x2-v1x2,v2y2-v1y2,tx,ty);
            vt -= m.surface_speed;
            if (cp.k_tangent < 1e-12f) continue;
            float jt = -vt * cp.k_tangent;

            // ── Contact-normal-aware friction ────────────────────────────────────
            // wallness: how vertical the contact normal is (1 = perfectly vertical wall).
            // For floor contacts (ny≈1, wallness≈0): use full friction — stops sliding.
            // For wall contacts (nx≈1, wallness≈1): the contact tangent IS the Y-axis,
            // so full friction would kill vertical velocity (including jump impulses).
            // We scale friction to near-zero on pure wall contacts, and let gravity +
            // normal impulse do the work. wall_slide_resist > 1 opts into more grip.
            float wallness = std::clamp(std::abs(nx), 0.f, 1.f);
            auto wall_grip_for = [&](Entity* rb) -> float {
                if (!rb || body_type(*rb) != "dynamic") return 1.f;
                return std::max(1.f, finite_val(rb->value("wall_slide_resist", 1.f), 1.f));
            };
            float grip_mult = std::max(wall_grip_for(m.rb1), wall_grip_for(m.rb2));
            // floor_factor: 1 on flat floor, 0 on vertical wall
            float floor_factor = 1.f - wallness;
            // Allow opt-in bodies to retain some wall grip, but cap at 30% for default bodies
            float effective_scale = floor_factor + wallness * std::min((grip_mult - 1.f) * 0.3f, 0.3f);
            effective_scale = std::max(0.f, std::min(effective_scale, grip_mult));

            float max_s = m.friction_s * cp.lambda_n * effective_scale;
            float max_d = m.friction_d * cp.lambda_n * effective_scale;
            float new_t = clamp(cp.lambda_t + jt, -max_s, max_s);
            // Stick/slip: large tangential velocity → use dynamic cone
            const float slip_threshold = 30.f;
            if (std::abs(vt) > slip_threshold) {
                new_t = clamp(new_t, -max_d, max_d);
            }
            jt = new_t - cp.lambda_t; cp.lambda_t = new_t;

            auto applyT=[&](Entity* rb, float sign, float rx, float ry, float im, float iI, bool fr){
                if (!rb||im<=0) return;
                (*rb)["velocity_x"]=(float)(*rb)["velocity_x"]+sign*jt*im*tx;
                (*rb)["velocity_y"]=(float)(*rb)["velocity_y"]+sign*jt*im*ty;
                if (!fr) (*rb)["angular_velocity"]=(float)(*rb)["angular_velocity"]+sign*jt*iI*cross(rx,ry,tx,ty);
            };
            applyT(m.rb1,-1,r1x,r1y,inv_m1,inv_I1,fr1);
            applyT(m.rb2,+1,r2x,r2y,inv_m2,inv_I2,fr2);

            // ── Rolling resistance (angular friction at contact) ─────────────
            // Applies a torque opposing relative angular spin, proportional to
            // the normal impulse. This makes balls/circles naturally decelerate
            // their spin when in contact (matches Unity's physic material behavior).
            // rolling_friction coefficient: per-body "rolling_friction" property (default 0.01)
            auto apply_rolling = [&](Entity* rb_ent, Entity* rb_comp, float sign) {
                if (!rb_comp || !rb_ent) return;
                if (body_type(*rb_comp) != "dynamic") return;
                if (rb_comp->value("freeze_rotation", false)) return;
                float rf = finite_val(rb_comp->value("rolling_friction", 0.01f), 0.f);
                if (rf <= 0.f) return;
                float av = finite_val(rb_comp->value("angular_velocity", 0.f));
                float iI = body_inv_inertia(rb_comp, rb_ent);
                if (iI <= 0.f) return;
                float max_roll = rf * std::abs(cp.lambda_n) * iI;
                float dav = clamp(-av, -max_roll, max_roll);
                (*rb_comp)["angular_velocity"] = av + dav;
            };
            apply_rolling(m.e1, m.rb1, -1.f);
            apply_rolling(m.e2, m.rb2, +1.f);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  13.  POSITION CORRECTION — Split Impulse (no ghost velocity)
//
//  Instead of adding a bias velocity into the real velocity (Baumgarte),
//  we maintain separate pseudo-velocities that only move positions.
//  This matches Box2D 2.4+'s "split impulse" approach and eliminates the
//  energy injection that causes jitter and tunnelling at high penetrations.
// ════════════════════════════════════════════════════════════════════════════
template<typename MfdRange>
static void correct_positions_impl(MfdRange& mfds, float dt) {
    // Per-entity pseudo-velocity store (bias only, not written to rb velocity)
    std::unordered_map<int,float> psvx, psvy; // entity_id → linear bias velocity
    std::unordered_map<int,float> psva;        // entity_id → angular bias velocity (rad/s)

    auto get_psv = [&](int id, float& bvx, float& bvy){
        bvx = psvx.count(id) ? psvx[id] : 0.f;
        bvy = psvy.count(id) ? psvy[id] : 0.f;
    };
    auto add_psv = [&](int id, float dvx, float dvy){
        psvx[id] = (psvx.count(id) ? psvx[id] : 0.f) + dvx;
        psvy[id] = (psvy.count(id) ? psvy[id] : 0.f) + dvy;
    };
    auto add_psva = [&](int id, float dav){
        psva[id] = (psva.count(id) ? psva[id] : 0.f) + dav;
    };

    // Bias velocity iterations
    for (int iter = 0; iter < POSITION_ITERS; ++iter) {
        for (auto& m_ : mfds) {
            auto& m = deref(m_);
            if (m.is_trigger || m.one_way_skip) continue;
            float inv_m1 = body_inv_mass(m.rb1), inv_m2 = body_inv_mass(m.rb2);
            if (inv_m1 + inv_m2 <= 1e-12f) continue;
            float inv_I1 = body_inv_inertia(m.rb1, m.e1), inv_I2 = body_inv_inertia(m.rb2, m.e2);
            bool ht1 = m.e1 && has_component(*m.e1, "Transform");
            bool ht2 = m.e2 && has_component(*m.e2, "Transform");
            bool fr1 = m.rb1 && m.rb1->value("freeze_rotation", false);
            bool fr2 = m.rb2 && m.rb2->value("freeze_rotation", false);
            int id1 = m.e1 ? m.e1->value("id", 0) : -1;
            int id2 = m.e2 ? m.e2->value("id", 0) : -1;

            for (int ci = 0; ci < m.contact_count; ++ci) {
                auto& cp = m.contacts[ci];
                float pen = clamp(cp.depth - SLOP, 0.f, MAX_LINEAR_CORR); // clamp both ends to prevent overshoot
                if (pen <= 1e-6f) continue;

                float t1x = ht1 ? get_float((*m.e1)["components"]["Transform"],"x") : 0.f;
                float t1y = ht1 ? get_float((*m.e1)["components"]["Transform"],"y") : 0.f;
                float t2x = ht2 ? get_float((*m.e2)["components"]["Transform"],"x") : 0.f;
                float t2y = ht2 ? get_float((*m.e2)["components"]["Transform"],"y") : 0.f;
                float r1x = cp.x - t1x, r1y = cp.y - t1y;
                float r2x = cp.x - t2x, r2y = cp.y - t2y;

                float r1n = cross(r1x, r1y, m.nx, m.ny);
                float r2n = cross(r2x, r2y, m.nx, m.ny);
                float K = inv_m1 + inv_m2
                        + (fr1 ? 0.f : r1n*r1n*inv_I1)
                        + (fr2 ? 0.f : r2n*r2n*inv_I2);
                if (K < 1e-12f) continue;

                // Compute bias relative pseudo-velocity (linear + angular contribution)
                float bvx1=0, bvy1=0, bvx2=0, bvy2=0;
                if (id1 >= 0) get_psv(id1, bvx1, bvy1);
                if (id2 >= 0) get_psv(id2, bvx2, bvy2);
                float bav1 = (!fr1 && id1 >= 0 && psva.count(id1)) ? psva[id1] : 0.f;
                float bav2 = (!fr2 && id2 >= 0 && psva.count(id2)) ? psva[id2] : 0.f;
                // Point velocity at contact from pseudo-velocities (v + ω × r)
                float bpx1 = bvx1 - bav1 * r1y,  bpy1 = bvy1 + bav1 * r1x;
                float bpx2 = bvx2 - bav2 * r2y,  bpy2 = bvy2 + bav2 * r2x;
                float bvn = dot(bpx2 - bpx1, bpy2 - bpy1, m.nx, m.ny);

                // Bias velocity target: push out at SPLIT_IMPULSE_BIAS fraction/dt
                float bias = -SPLIT_IMPULSE_BIAS * pen / std::max(dt, 1e-6f);
                bias = clamp(bias, -SPLIT_IMPULSE_MAX, 0.f);

                float jb = (bias - bvn) / K;
                // Clamp accumulated pseudo impulse (must be non-negative)
                float old_pn = cp.pseudo_n;
                cp.pseudo_n = std::max(0.f, old_pn + jb);
                jb = cp.pseudo_n - old_pn;

                // Apply bias velocities (purely positional, don't touch rb velocities)
                if (inv_m1 > 0 && id1 >= 0) {
                    add_psv(id1, -jb * inv_m1 * m.nx, -jb * inv_m1 * m.ny);
                    if (!fr1) add_psva(id1, -jb * inv_I1 * r1n);  // angular position correction
                }
                if (inv_m2 > 0 && id2 >= 0) {
                    add_psv(id2, +jb * inv_m2 * m.nx, +jb * inv_m2 * m.ny);
                    if (!fr2) add_psva(id2, +jb * inv_I2 * r2n);  // angular position correction
                }
            }
        }
    }

    // Apply accumulated bias displacements to transforms.
    // Collect all dynamic entity IDs that need correction first, then apply
    // each exactly once — avoids the bug where an entity appearing as e2 in
    // manifold[0] after being erased as e1 would lose its correction.
    std::unordered_set<int> applied_ids;
    for (auto& m_ : mfds) {
        auto& m = deref(m_);
        if (m.is_trigger || m.one_way_skip) continue;
        auto apply_bias = [&](Entity* ent, Entity* rb, float inv_m) {
            if (!ent || !rb || inv_m <= 0) return;
            if (!has_component(*ent, "Transform")) return;
            int eid = ent->value("id", 0);
            if (applied_ids.count(eid)) return; // already applied this frame
            float bvx = psvx.count(eid) ? psvx[eid] : 0.f;
            float bvy = psvy.count(eid) ? psvy[eid] : 0.f;
            float bav = psva.count(eid) ? psva[eid] : 0.f;
            bool has_motion = (std::abs(bvx) > 1e-8f || std::abs(bvy) > 1e-8f || std::abs(bav) > 1e-8f);
            if (!has_motion) { applied_ids.insert(eid); return; }
            auto& tr = (*ent)["components"]["Transform"];
            tr["x"] = finite_val(get_float(tr, "x")) + bvx * dt;
            tr["y"] = finite_val(get_float(tr, "y")) + bvy * dt;
            bool fr = rb->value("freeze_rotation", false);
            if (!fr && std::abs(bav) > 1e-8f) {
                float da_deg = bav * dt * (180.f / (float)M_PI);
                tr["rotation"] = finite_val(get_float(tr, "rotation")) + da_deg;
            }
            transform::mark_local_dirty(eid);
            applied_ids.insert(eid);
        };
        if (m.e1 && m.rb1) apply_bias(m.e1, m.rb1, body_inv_mass(m.rb1));
        if (m.e2 && m.rb2) apply_bias(m.e2, m.rb2, body_inv_mass(m.rb2));
    }

    // Hard depenetration safety pass for dynamic-vs-static contacts.
    // This catches the common "slowly sink into the floor/wall" case when the
    // split-impulse solver does not fully clear a tiny residual penetration.
    // We only touch contacts where exactly one side is dynamic, so dynamic-
    // dynamic stacks keep using the regular iterative solver above.
    static constexpr float HARD_DEPEN_SLOP = 1.0f; // must match SLOP constant
    for (int pass = 0; pass < 2; ++pass) {
        std::unordered_map<int, Vec2> linear_push;
        std::unordered_set<int> touched;
        bool any = false;

        for (auto& m_ : mfds) {
            auto& m = deref(m_);
            if (m.is_trigger || m.one_way_skip) continue;

            float inv_m1 = body_inv_mass(m.rb1), inv_m2 = body_inv_mass(m.rb2);
            if (inv_m1 + inv_m2 <= 1e-12f) continue;

            // Only snap contacts where one side is static/kinematic/frozen.
            if (!((inv_m1 > 0.f) ^ (inv_m2 > 0.f))) continue;

            int id1 = m.e1 ? m.e1->value("id", 0) : -1;
            int id2 = m.e2 ? m.e2->value("id", 0) : -1;

            bool ht1 = m.e1 && has_component(*m.e1, "Transform");
            bool ht2 = m.e2 && has_component(*m.e2, "Transform");
            float t1x = ht1 ? get_float((*m.e1)["components"]["Transform"], "x") : 0.f;
            float t1y = ht1 ? get_float((*m.e1)["components"]["Transform"], "y") : 0.f;
            float t2x = ht2 ? get_float((*m.e2)["components"]["Transform"], "x") : 0.f;
            float t2y = ht2 ? get_float((*m.e2)["components"]["Transform"], "y") : 0.f;

            float inv_I1 = body_inv_inertia(m.rb1, m.e1), inv_I2 = body_inv_inertia(m.rb2, m.e2);
            bool fr1 = !m.rb1 || is_static(m.rb1) || m.rb1->value("freeze_rotation", false);
            bool fr2 = !m.rb2 || is_static(m.rb2) || m.rb2->value("freeze_rotation", false);

            for (int ci = 0; ci < m.contact_count; ++ci) {
                auto& cp = m.contacts[ci];
                float pen = std::max(0.f, cp.depth - HARD_DEPEN_SLOP);
                if (pen <= 1e-5f) continue;

                float r1x = cp.x - t1x, r1y = cp.y - t1y;
                float r2x = cp.x - t2x, r2y = cp.y - t2y;
                float r1n = cross(r1x, r1y, m.nx, m.ny);
                float r2n = cross(r2x, r2y, m.nx, m.ny);

                float K = inv_m1 + inv_m2
                        + (fr1 ? 0.f : r1n * r1n * inv_I1)
                        + (fr2 ? 0.f : r2n * r2n * inv_I2);
                if (K <= 1e-12f) continue;

                float push = pen / K;
                // Cap the per-contact push to avoid injecting large displacements on deep penetrations.
                // 16px matches the one-sided slab thickness so a fully-through player can be ejected in one pass.
                push = std::min(push, 16.0f);
                if (inv_m1 > 0.f && id1 >= 0) {
                    linear_push[id1].first  += -m.nx * push * inv_m1;
                    linear_push[id1].second += -m.ny * push * inv_m1;
                    touched.insert(id1);
                }
                if (inv_m2 > 0.f && id2 >= 0) {
                    linear_push[id2].first  += +m.nx * push * inv_m2;
                    linear_push[id2].second += +m.ny * push * inv_m2;
                    touched.insert(id2);
                }
                any = true;
            }
        }

        if (!any) break;

        for (int eid : touched) {
            auto it = linear_push.find(eid);
            if (it == linear_push.end()) continue;
            for (auto& e : *s_active_entities) {
                if (e.value("id", 0) != eid) continue;
                if (!has_component(e, "Transform")) break;
                auto& tr = e["components"]["Transform"];
                tr["x"] = finite_val(get_float(tr, "x")) + it->second.first;
                tr["y"] = finite_val(get_float(tr, "y")) + it->second.second;
                transform::mark_local_dirty(eid);
                break;
            }
        }
    }
}
// Phase 1 ✅: angular position correction is now implemented inside correct_positions_impl
// (pseudo angular velocity psva applied alongside linear psvx/psvy per contact).
static void correct_positions(std::vector<Manifold>& mfds, float dt) {
    correct_positions_impl(mfds, dt);
}

// ════════════════════════════════════════════════════════════════════════════
//  14.  JOINTS
// ════════════════════════════════════════════════════════════════════════════
static float body_inv_mass_e(Entity* rb){ return rb?body_inv_mass(rb):0.f; }

static void solve_joints(EntityList& entities, float dt, std::unordered_map<int,Entity*>& emap) {
    for (auto& e:entities){
        if (!entity_active(e)||!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb1=e["components"]["Rigidbody2D"];
        auto& t1 =e["components"]["Transform"];
        if (is_static(&rb1)) continue;
        float im1=body_inv_mass(&rb1);
        float iI1=body_inv_inertia(&rb1,&e);

        // ── DistanceJoint2D ─────────────────────────────────────────────
        if (has_component(e,"DistanceJoint2D")){
            auto& dj=e["components"]["DistanceJoint2D"];
            int oid=dj.value("connected_entity",-1);
            if (emap.count(oid)){
                auto* other=emap[oid];
                auto& t2=(*other)["components"]["Transform"];
                float dx=get_float(t2,"x")-get_float(t1,"x");
                float dy=get_float(t2,"y")-get_float(t1,"y");
                float dist=std::hypot(dx,dy);
                if (dist>1e-6f){
                    float max_len=dj.value("max_length",dj.value("length",dist));
                    float min_len=dj.value("min_length",0.f);
                    float target=-1;
                    if (dist<min_len) target=min_len;
                    else if (dist>max_len) target=max_len;
                    if (target>=0){
                        float nx=dx/dist,ny=dy/dist,corr=dist-target;
                        auto* prb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                        float im2=body_inv_mass_e(prb2);
                        float tot=im1+im2; if(tot<1e-12f)tot=1;
                        float vx1=rb1.value("velocity_x",0.f),vy1=rb1.value("velocity_y",0.f);
                        float vx2=prb2?prb2->value("velocity_x",0.f):0.f,vy2=prb2?prb2->value("velocity_y",0.f):0.f;
                        float rv=dot(vx2-vx1,vy2-vy1,nx,ny);
                        // Warm-start: apply cached impulse from previous frame
                        float lambda_cached=dj.value("_lambda_ws",0.f);
                        if(std::abs(lambda_cached)>1e-9f){
                            rb1["velocity_x"]=(float)rb1["velocity_x"]-nx*lambda_cached*im1;
                            rb1["velocity_y"]=(float)rb1["velocity_y"]-ny*lambda_cached*im1;
                            if(prb2&&!is_static(prb2)){(*prb2)["velocity_x"]=(float)(*prb2)["velocity_x"]+nx*lambda_cached*im2;(*prb2)["velocity_y"]=(float)(*prb2)["velocity_y"]+ny*lambda_cached*im2;}
                        }
                        float jn=(-rv + corr/dt*BAUMGARTE)/tot;
                        dj["_lambda_ws"]=jn; // store for next frame warm-start
                        dj["_accumulated_impulse"]=std::abs(jn);
                        rb1["velocity_x"]=(float)rb1["velocity_x"]-nx*jn*im1;
                        rb1["velocity_y"]=(float)rb1["velocity_y"]-ny*jn*im1;
                        if (prb2&&!is_static(prb2)){
                            (*prb2)["velocity_x"]=(float)(*prb2)["velocity_x"]+nx*jn*im2;
                            (*prb2)["velocity_y"]=(float)(*prb2)["velocity_y"]+ny*jn*im2;
                        }
                    }
                }
            }
        }

        // ── SpringJoint2D ───────────────────────────────────────────────
        if (has_component(e,"SpringJoint2D")){
            auto& sj=e["components"]["SpringJoint2D"];
            int oid=sj.value("connected_entity",-1);
            if (emap.count(oid)){
                auto* other=emap[oid];
                auto& t2=(*other)["components"]["Transform"];
                float dx=get_float(t2,"x")-get_float(t1,"x");
                float dy=get_float(t2,"y")-get_float(t1,"y");
                float dist=std::hypot(dx,dy);
                if (dist>1e-6f){
                    float nx=dx/dist,ny=dy/dist;
                    float rest=sj.value("rest_length",sj.value("length",dist));
                    float k=sj.value("stiffness",10.f),damp=sj.value("damping",1.f);
                    auto* rb2ptr=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                    float rvx=rb1.value("velocity_x",0.f)-(rb2ptr?rb2ptr->value("velocity_x",0.f):0.f);
                    float rvy=rb1.value("velocity_y",0.f)-(rb2ptr?rb2ptr->value("velocity_y",0.f):0.f);
                    float rv=dot(rvx,rvy,nx,ny);
                    float force=-(dist-rest)*k-rv*damp;
                    // Phase 1: accumulate impulse magnitude for breakable joint support
                    float impulse_mag = std::abs(force) * dt;
                    sj["_accumulated_impulse"] = impulse_mag;
                    add_force(rb1,nx*force,ny*force);
                    if (rb2ptr&&!is_static(rb2ptr)) add_force(*rb2ptr,-nx*force,-ny*force);
                }
            }
        }

        // ── HingeJoint2D (pin + optional motor + limits) ────────────────
        if (has_component(e,"HingeJoint2D")){
            auto& hj=e["components"]["HingeJoint2D"];
            int oid=hj.value("connected_entity",-1);
            float ax=finite_val(hj.value("anchor_x",0.f));
            float ay=finite_val(hj.value("anchor_y",0.f));
            float rot1=finite_val(get_float(t1,"rotation"))*(float)M_PI/180.f;
            float c1=std::cos(rot1),s1=std::sin(rot1);
            Vec2 world_anc=world_from_local(get_float(t1,"x"),get_float(t1,"y"),c1,s1,ax,ay);

            // Always initialise _ref_angle so motor-only hinges can add limits later
            float cur_ang=finite_val(get_float(t1,"rotation"))*(float)M_PI/180.f;
            if (!hj.contains("_ref_angle")) hj["_ref_angle"]=cur_ang;

            float total_impulse_mag=0.f;

            // Pin impulse using proper velocity constraint (split impulse avoidance):
            // We compute the constraint Jacobian and solve for impulse directly,
            // avoiding direct velocity bias injection (ghost velocity).
            if (emap.count(oid)){
                auto* other=emap[oid];
                if (has_component(*other,"Transform")){
                    auto& t2=(*other)["components"]["Transform"];
                    auto* rb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                    float im2=body_inv_mass_e(rb2);
                    float iI2=body_inv_inertia(rb2,other);
                    float tot_m=im1+im2; if(tot_m<1e-12f) tot_m=1.f;

                    // World-space anchor on body B
                    float rot2=finite_val(get_float(t2,"rotation"))*(float)M_PI/180.f;
                    float c2=std::cos(rot2),s2=std::sin(rot2);
                    float cax=finite_val(hj.value("connected_anchor_x",world_anc.first));
                    float cay=finite_val(hj.value("connected_anchor_y",world_anc.second));

                    // r1, r2 arm vectors from body CoMs to the anchor point
                    float r1x=world_anc.first  - get_float(t1,"x");
                    float r1y=world_anc.second - get_float(t1,"y");
                    float r2x=cax - get_float(t2,"x");
                    float r2y=cay - get_float(t2,"y");

                    // Relative velocity at contact point
                    float av1=finite_val(rb1.value("angular_velocity",0.f));
                    float av2=rb2?finite_val(rb2->value("angular_velocity",0.f)):0.f;
                    float vx1=rb1.value("velocity_x",0.f)-av1*r1y;
                    float vy1=rb1.value("velocity_y",0.f)+av1*r1x;
                    float vx2=rb2?rb2->value("velocity_x",0.f)-av2*r2y:0.f;
                    float vy2=rb2?rb2->value("velocity_y",0.f)+av2*r2x:0.f;
                    float rvx=vx1-vx2, rvy=vy1-vy2;

                    // Position error (Baumgarte bias on position only — no ghost velocity)
                    float err_x=world_anc.first-cax, err_y=world_anc.second-cay;
                    float bias_x=BAUMGARTE*err_x/dt;
                    float bias_y=BAUMGARTE*err_y/dt;

                    // Effective mass for each axis
                    // K_xx = im1 + im2 + (r1y²*iI1 + r2y²*iI2)
                    // K_yy = im1 + im2 + (r1x²*iI1 + r2x²*iI2)
                    float Kxx=im1+im2+(r1y*r1y*iI1+r2y*r2y*iI2);
                    float Kyy=im1+im2+(r1x*r1x*iI1+r2x*r2x*iI2);
                    float Kxy=-(r1x*r1y*iI1+r2x*r2y*iI2);
                    float det=Kxx*Kyy-Kxy*Kxy; if(std::abs(det)<1e-12f) det=1e-12f;
                    float inv_det=1.f/det;

                    float jx=-(( Kyy*(rvx+bias_x)-Kxy*(rvy+bias_y))*inv_det);
                    float jy=-((-Kxy*(rvx+bias_x)+Kxx*(rvy+bias_y))*inv_det);

                    // Accumulate impulse for breakable joint check
                    total_impulse_mag+=std::hypot(jx,jy);

                    rb1["velocity_x"]=(float)rb1["velocity_x"]+jx*im1;
                    rb1["velocity_y"]=(float)rb1["velocity_y"]+jy*im1;
                    if(!rb1.value("freeze_rotation",false))
                        rb1["angular_velocity"]=(float)rb1["angular_velocity"]+(r1x*jy-r1y*jx)*iI1;
                    if (rb2&&!is_static(rb2)){
                        float im2_=body_inv_mass(rb2);
                        (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"]-jx*im2_;
                        (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"]-jy*im2_;
                        if(!rb2->value("freeze_rotation",false))
                            (*rb2)["angular_velocity"]=(float)(*rb2)["angular_velocity"]-(r2x*jy-r2y*jx)*iI2;
                    }
                }
            }
            // Motor
            if (hj.value("use_motor",false)){
                float target_spd=hj.value("motor_speed",0.f)*(float)M_PI/180.f;
                float max_torque=hj.value("max_motor_torque",1e4f);
                float av1=finite_val(rb1.value("angular_velocity",0.f));
                float iI=body_inv_inertia(&rb1,&e);
                float err_v=target_spd-av1;
                float j=clamp(err_v/std::max(iI,1e-9f),-max_torque*dt,max_torque*dt);
                rb1["angular_velocity"]=(float)rb1["angular_velocity"]+j*iI;
                total_impulse_mag+=std::abs(j);
            }
            // Limits — use angular impulse (no ghost velocity)
            if (hj.value("use_limits",false)){
                float lo=hj.value("lower_angle",0.f)*(float)M_PI/180.f;
                float hi=hj.value("upper_angle",0.f)*(float)M_PI/180.f;
                float cur=finite_val(get_float(t1,"rotation"))*(float)M_PI/180.f;
                float ref_ang=hj.value("_ref_angle",cur);
                float delta=cur-ref_ang;
                float iI=body_inv_inertia(&rb1,&e);
                float av1=finite_val(rb1.value("angular_velocity",0.f));
                float j=0.f;
                if (delta<lo){
                    float C=lo-delta; float Cd=av1;
                    float K=iI; if(K<1e-12f)K=1e-12f;
                    j=(-Cd+C*BAUMGARTE/dt)/K;
                    j=std::max(0.f,j);
                } else if (delta>hi){
                    float C=hi-delta; float Cd=av1;
                    float K=iI; if(K<1e-12f)K=1e-12f;
                    j=(-Cd+C*BAUMGARTE/dt)/K;
                    j=std::min(0.f,j);
                }
                if (std::abs(j)>1e-9f){
                    rb1["angular_velocity"]=(float)rb1["angular_velocity"]+j*iI;
                    total_impulse_mag+=std::abs(j);
                }
            }
            // Write accumulated impulse for breakable joints
            hj["_accumulated_impulse"]=total_impulse_mag;
        }

        // ── SliderJoint2D (prismatic) ────────────────────────────────────
        if (has_component(e,"SliderJoint2D")){
            auto& sj=e["components"]["SliderJoint2D"];
            int oid=sj.value("connected_entity",-1);
            // Axis in world space
            float ang=sj.value("angle",0.f)*(float)M_PI/180.f;
            float axnx=std::cos(ang),axny=std::sin(ang);
            // Perp axis (locked direction)
            float pnx=-axny,pny=axnx;
            if (emap.count(oid)){
                auto* other=emap[oid];
                auto& t2=(*other)["components"]["Transform"];
                float dx=get_float(t2,"x")-get_float(t1,"x");
                float dy=get_float(t2,"y")-get_float(t1,"y");
                // Constrain motion perpendicular to axis
                float perp_err=dot(dx,dy,pnx,pny);
                auto* rb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                float im2=body_inv_mass_e(rb2);
                float tot=im1+im2; if(tot<1e-12f)tot=1;
                float rv_perp=dot(rb1.value("velocity_x",0.f)-( rb2?rb2->value("velocity_x",0.f):0.f),
                                  rb1.value("velocity_y",0.f)-( rb2?rb2->value("velocity_y",0.f):0.f),pnx,pny);
                float j=(-rv_perp+perp_err*BAUMGARTE/dt)/tot;
                rb1["velocity_x"]=(float)rb1["velocity_x"]-pnx*j*im1;
                rb1["velocity_y"]=(float)rb1["velocity_y"]-pny*j*im1;
                if (rb2&&!is_static(rb2)){
                    (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"]+pnx*j*im2;
                    (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"]+pny*j*im2;
                }
                // Limits along axis
                if (sj.value("use_limits",false)){
                    float proj=dot(dx,dy,axnx,axny);
                    float lo=sj.value("limits_min",0.f),hi=sj.value("limits_max",0.f);
                    float clamp_err=0;
                    if (proj<lo) clamp_err=lo-proj;
                    else if (proj>hi) clamp_err=hi-proj;
                    if (std::abs(clamp_err)>1e-4f){
                        float rv_ax=dot(rb1.value("velocity_x",0.f)-(rb2?rb2->value("velocity_x",0.f):0.f),
                                        rb1.value("velocity_y",0.f)-(rb2?rb2->value("velocity_y",0.f):0.f),axnx,axny);
                        float jl=(-rv_ax+clamp_err*BAUMGARTE/dt)/tot;
                        jl=clamp(jl,-1e6f,1e6f);
                        rb1["velocity_x"]=(float)rb1["velocity_x"]-axnx*jl*im1;
                        rb1["velocity_y"]=(float)rb1["velocity_y"]-axny*jl*im1;
                        if (rb2&&!is_static(rb2)){
                            (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"]+axnx*jl*im2;
                            (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"]+axny*jl*im2;
                        }
                    }
                }
                // Motor
                if (sj.value("use_motor",false)){
                    float tspd=sj.value("motor_speed",0.f);
                    float mf=sj.value("max_motor_force",1e4f);
                    float rv_ax=dot(rb1.value("velocity_x",0.f)-(rb2?rb2->value("velocity_x",0.f):0.f),
                                    rb1.value("velocity_y",0.f)-(rb2?rb2->value("velocity_y",0.f):0.f),axnx,axny);
                    float jm=clamp((tspd-rv_ax)/tot,-mf*dt,mf*dt);
                    rb1["velocity_x"]=(float)rb1["velocity_x"]-axnx*jm*im1;
                    rb1["velocity_y"]=(float)rb1["velocity_y"]-axny*jm*im1;
                    if (rb2&&!is_static(rb2)){
                        (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"]+axnx*jm*im2;
                        (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"]+axny*jm*im2;
                    }
                }
                // ── Rotation lock: constrain relative angular velocity ────────
                // Unity's SliderJoint2D locks relative rotation so bodies can't
                // spin freely around the slide axis.
                if (sj.value("lock_rotation", true)) {
                    float av1=finite_val(rb1.value("angular_velocity",0.f));
                    float av2=rb2?finite_val(rb2->value("angular_velocity",0.f)):0.f;
                    float rav=av1-av2;
                    float iI2=body_inv_inertia(rb2,emap.count(sj.value("connected_entity",-1))?emap[sj.value("connected_entity",-1)]:nullptr);
                    float denom=iI1+iI2; if(denom<1e-12f) denom=1e-12f;
                    // Angle error for Baumgarte
                    float rot1=finite_val(get_float(t1,"rotation")*(float)M_PI/180.f);
                    float rot2_=rb2?finite_val(get_float((*emap[sj.value("connected_entity",-1)])["components"]["Transform"],"rotation")*(float)M_PI/180.f):0.f;
                    if(!sj.contains("_ref_rot_diff")) sj["_ref_rot_diff"]=rot1-rot2_;
                    float ref_diff=sj.value("_ref_rot_diff",0.f);
                    float cur_diff=rot1-rot2_;
                    float angle_err=cur_diff-ref_diff;
                    float ja=(-rav + angle_err*BAUMGARTE/dt)/denom;
                    rb1["angular_velocity"]=(float)rb1["angular_velocity"]+ja*iI1;
                    if(rb2&&!is_static(rb2)) (*rb2)["angular_velocity"]=(float)(*rb2)["angular_velocity"]-ja*iI2;
                }
                // Write accumulated impulse for breakable joint support
                sj["_accumulated_impulse"]=std::abs(im1)>1e-9f?
                    std::hypot((float)rb1["velocity_x"],(float)rb1["velocity_y"])*
                    std::max(finite_val(sj.value("mass",1.f),1.f),1e-9f)*0.01f:0.f;
            }
        }

        // ── WheelJoint2D (spring suspension + motor) ────────────────────
        if (has_component(e,"WheelJoint2D")){
            auto& wj=e["components"]["WheelJoint2D"];
            int oid=wj.value("connected_entity",-1);
            if (emap.count(oid)){
                auto* other=emap[oid];
                auto& t2=(*other)["components"]["Transform"];
                auto* rb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                float im2=body_inv_mass_e(rb2);
                float tot=im1+im2; if(tot<1e-12f)tot=1;
                float dx=get_float(t2,"x")-get_float(t1,"x");
                float dy=get_float(t2,"y")-get_float(t1,"y");
                float dist=std::hypot(dx,dy); if(dist<1e-6f)dist=1e-6f;
                float nx2=dx/dist,ny2=dy/dist;
                // Spring suspension along link
                float rest=wj.value("suspension_distance",dist);
                float k=wj.value("suspension_stiffness",10.f),damp=wj.value("damping_ratio",0.7f);
                float freq=std::max(wj.value("suspension_frequency",2.f),0.01f);
                float omega=2*(float)M_PI*freq;
                float rv=dot(rb1.value("velocity_x",0.f)-(rb2?rb2->value("velocity_x",0.f):0.f),
                             rb1.value("velocity_y",0.f)-(rb2?rb2->value("velocity_y",0.f):0.f),nx2,ny2);
                float err=dist-rest;
                float force=-(omega*omega*err + 2.f*damp*omega*rv);
                float fm=std::min(std::abs(force),1e6f)*((force<0)?-1.f:1.f);
                // Phase 1: track impulse magnitude for breakable joint support
                wj["_accumulated_impulse"] = std::abs(fm) * dt;
                rb1["velocity_x"]=(float)rb1["velocity_x"]+nx2*fm*im1*dt;
                rb1["velocity_y"]=(float)rb1["velocity_y"]+ny2*fm*im1*dt;
                if (rb2&&!is_static(rb2)){
                    (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"]-nx2*fm*im2*dt;
                    (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"]-ny2*fm*im2*dt;
                }
                // Motor (spin the wheel body)
                if (wj.value("use_motor",false)){
                    float tspd=wj.value("motor_speed",0.f)*(float)M_PI/180.f;
                    float mt=wj.value("max_motor_torque",1e4f);
                    float iI=body_inv_inertia(&rb1,&e);
                    float av1=finite_val(rb1.value("angular_velocity",0.f));
                    float j=clamp((tspd-av1)/std::max(iI,1e-9f),-mt*dt,mt*dt);
                    rb1["angular_velocity"]=(float)rb1["angular_velocity"]+j*iI;
                }
            }
        }

        // ── GearJoint2D (angular ratio constraint) ───────────────────────
        if (has_component(e,"GearJoint2D")){
            auto& gj=e["components"]["GearJoint2D"];
            int oid=gj.value("connected_entity",-1);
            if (emap.count(oid)){
                auto* other=emap[oid];
                if (has_component(*other,"Transform")){
                    auto& t2=(*other)["components"]["Transform"];
                    auto* rb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                    if (rb2&&!is_static(rb2)){
                        float ratio=finite_val(gj.value("ratio",1.f),1.f);
                        if (std::abs(ratio) < 1e-6f) ratio = 1.f;
                        float ref1=gj.contains("_ref_rot1") ? gj.value("_ref_rot1", get_float(t1,"rotation")) : get_float(t1,"rotation");
                        float ref2=gj.contains("_ref_rot2") ? gj.value("_ref_rot2", get_float(t2,"rotation")) : get_float(t2,"rotation");
                        if (!gj.contains("_ref_rot1")) { gj["_ref_rot1"]=ref1; gj["_ref_rot2"]=ref2; }
                        float a1=finite_val(get_float(t1,"rotation"))-ref1;
                        float a2=finite_val(get_float(t2,"rotation"))-ref2;
                        float av1=finite_val(rb1.value("angular_velocity",0.f));
                        float av2=finite_val(rb2->value("angular_velocity",0.f));
                        float iI1=body_inv_inertia(&rb1,&e);
                        float iI2=body_inv_inertia(rb2,other);
                        float C  = a1 + ratio*a2;
                        float Cd = av1 + ratio*av2;
                        float denom = iI1 + ratio*ratio*iI2;
                        if (denom>1e-12f){
                            float j = (-(Cd) + (C*BAUMGARTE/dt))/denom;
                            // Phase 1: track impulse magnitude for breakable joint support
                            gj["_accumulated_impulse"] = std::abs(j);
                            rb1["angular_velocity"]=(float)rb1["angular_velocity"] + j*iI1;
                            (*rb2)["angular_velocity"]=(float)(*rb2)["angular_velocity"] + j*ratio*iI2;
                        }
                    }
                }
            }
        }

        // ── RelativeJoint2D (maintain relative transform) ─────────────────
        if (has_component(e,"RelativeJoint2D")){
            auto& rj=e["components"]["RelativeJoint2D"];
            int oid=rj.value("connected_entity",-1);
            if (emap.count(oid)){
                auto* other=emap[oid];
                if (has_component(*other,"Transform")){
                    auto& t2=(*other)["components"]["Transform"];
                    auto* rb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                    float im2=body_inv_mass_e(rb2);
                    float iI2=body_inv_inertia(rb2,other);
                    float im1=body_inv_mass(&rb1);
                    float iI1=body_inv_inertia(&rb1,&e);

                    if (rj.value("cache_reference",true) && !rj.contains("_ref_dx")){
                        rj["_ref_dx"] = finite_val(get_float(t2,"x")) - finite_val(get_float(t1,"x"));
                        rj["_ref_dy"] = finite_val(get_float(t2,"y")) - finite_val(get_float(t1,"y"));
                        rj["_ref_da"] = finite_val(get_float(t2,"rotation")) - finite_val(get_float(t1,"rotation"));
                    }
                    float ref_dx = finite_val(rj.value("_ref_dx",0.f));
                    float ref_dy = finite_val(rj.value("_ref_dy",0.f));
                    float ref_da = finite_val(rj.value("_ref_da",0.f));
                    float k = std::max(0.f, finite_val(rj.value("stiffness", 18.f), 18.f));
                    float d = std::max(0.f, finite_val(rj.value("damping", 3.f), 3.f));

                    float dx = finite_val(get_float(t2,"x")) - finite_val(get_float(t1,"x")) - ref_dx;
                    float dy = finite_val(get_float(t2,"y")) - finite_val(get_float(t1,"y")) - ref_dy;
                    float rvx = rb1.value("velocity_x",0.f) - (rb2 ? rb2->value("velocity_x",0.f) : 0.f);
                    float rvy = rb1.value("velocity_y",0.f) - (rb2 ? rb2->value("velocity_y",0.f) : 0.f);

                    if ((im1 + im2) > 1e-12f){
                        float fx = -(dx*k + rvx*d);
                        float fy = -(dy*k + rvy*d);
                        // Phase 1: track impulse magnitude for breakable joint support
                        rj["_accumulated_impulse"] = std::hypot(fx, fy) * dt;
                        rb1["velocity_x"]=(float)rb1["velocity_x"] + fx*im1*dt;
                        rb1["velocity_y"]=(float)rb1["velocity_y"] + fy*im1*dt;
                        if (rb2 && !is_static(rb2)){
                            (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"] - fx*im2*dt;
                            (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"] - fy*im2*dt;
                        }
                    }

                    if (rj.value("lock_rotation", true)){
                        float da = (finite_val(get_float(t2,"rotation")) - finite_val(get_float(t1,"rotation")) - ref_da);
                        float av1 = finite_val(rb1.value("angular_velocity",0.f));
                        float av2 = rb2 ? finite_val(rb2->value("angular_velocity",0.f)) : 0.f;
                        float Cd = av1 - av2;
                        float denom = iI1 + iI2;
                        if (denom > 1e-12f){
                            float j = (-(Cd) + (da*BAUMGARTE/dt)) / denom;
                            rb1["angular_velocity"]=(float)rb1["angular_velocity"] + j*iI1;
                            if (rb2 && !is_static(rb2)) (*rb2)["angular_velocity"]=(float)(*rb2)["angular_velocity"] - j*iI2;
                        }
                    }
                }
            }
        }

        // ── MouseJoint2D ────────────────────────────────────────────────
        if (has_component(e,"MouseJoint2D")){
            auto& mj=e["components"]["MouseJoint2D"];
            float ttx=mj.value("target_x",get_float(t1,"x"));
            float tty=mj.value("target_y",get_float(t1,"y"));
            float dx=ttx-get_float(t1,"x"),dy=tty-get_float(t1,"y");
            float freq=mj.value("frequency",8.f),damp=mj.value("damping_ratio",0.7f);
            float fmax=mj.value("max_force",1000.f);
            float vx=rb1.value("velocity_x",0.f),vy=rb1.value("velocity_y",0.f);
            float om=2.f*(float)M_PI*freq;
            float fx=clamp(dx*om*om-vx*2*damp*om,-fmax,fmax);
            float fy=clamp(dy*om*om-vy*2*damp*om,-fmax,fmax);
            float m=std::max(finite_val(rb1.value("mass",1.f),1.f),1e-9f);
            // Phase 1: track impulse magnitude for breakable joint support
            mj["_accumulated_impulse"] = std::hypot(fx, fy) * dt / std::max(m, 1e-9f);
            rb1["velocity_x"]=(float)rb1["velocity_x"]+fx/m*dt;
            rb1["velocity_y"]=(float)rb1["velocity_y"]+fy/m*dt;
            rb1["_sleeping"]=false;
        }

        // ── FrictionJoint2D (Unity2D parity) ───────────────────────────────
        if (has_component(e,"FrictionJoint2D")){
            auto& fj=e["components"]["FrictionJoint2D"];
            int oid=fj.value("connected_entity",-1);
            if (emap.count(oid)){
                auto* other=emap[oid];
                auto* rb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                float im2=body_inv_mass_e(rb2);
                float iI2=body_inv_inertia(rb2,other);
                float tot=im1+im2; if(tot<1e-12f)tot=1;
                float iI1=body_inv_inertia(&rb1,&e);

                // Max force and max torque (friction limits)
                float max_force=fj.value("max_force",BREAK_FORCE_DEFAULT);
                float max_torque=fj.value("max_torque",BREAK_TORQUE_DEFAULT);

                // Apply friction to stop relative motion
                float rvx=rb1.value("velocity_x",0.f)-(rb2?rb2->value("velocity_x",0.f):0.f);
                float rvy=rb1.value("velocity_y",0.f)-(rb2?rb2->value("velocity_y",0.f):0.f);
                float rav=finite_val(rb1.value("angular_velocity",0.f))-(rb2?finite_val(rb2->value("angular_velocity",0.f)):0.f);

                // Linear friction impulse
                float jx=-rvx/tot, jy=-rvy/tot;
                float jmag=std::hypot(jx,jy);
                if (jmag>max_force*dt){
                    float scale=max_force*dt/jmag;
                    jx*=scale; jy*=scale;
                }
                // Phase 1: track impulse magnitude for breakable joint support
                fj["_accumulated_impulse"] = std::hypot(jx, jy);
                rb1["velocity_x"]=(float)rb1["velocity_x"]+jx*im1;
                rb1["velocity_y"]=(float)rb1["velocity_y"]+jy*im1;
                if (rb2&&!is_static(rb2)){
                    (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"]-jx*im2;
                    (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"]-jy*im2;
                }

                // Angular friction impulse
                float denom=iI1+iI2;
                if (denom>1e-12f){
                    float ja=-rav/denom;
                    ja=clamp(ja,-max_torque*dt,max_torque*dt);
                    rb1["angular_velocity"]=(float)rb1["angular_velocity"]+ja*iI1;
                    if (rb2&&!is_static(rb2)){
                        (*rb2)["angular_velocity"]=(float)(*rb2)["angular_velocity"]-ja*iI2;
                    }
                }
            }
        }

        // ── TargetJoint2D (Unity2D parity - like MouseJoint but with offset) ──
        if (has_component(e,"TargetJoint2D")){
            auto& tj=e["components"]["TargetJoint2D"];
            float ttx=tj.value("target_x",get_float(t1,"x"));
            float tty=tj.value("target_y",get_float(t1,"y"));
            float ox=tj.value("offset_x",0.f), oy=tj.value("offset_y",0.f);
            float dx=ttx-(get_float(t1,"x")+ox), dy=tty-(get_float(t1,"y")+oy);
            float freq=tj.value("frequency",5.f), damp=tj.value("damping_ratio",0.7f);
            float fmax=tj.value("max_force",10000.f);
            float vx=rb1.value("velocity_x",0.f), vy=rb1.value("velocity_y",0.f);
            float om=2.f*(float)M_PI*freq;
            float fx=clamp(dx*om*om-vx*2*damp*om,-fmax,fmax);
            float fy=clamp(dy*om*om-vy*2*damp*om,-fmax,fmax);
            float m=std::max(finite_val(rb1.value("mass",1.f),1.f),1e-9f);
            // Phase 1: track impulse magnitude for breakable joint support
            tj["_accumulated_impulse"] = std::hypot(fx, fy) * dt / std::max(m, 1e-9f);
            rb1["velocity_x"]=(float)rb1["velocity_x"]+fx/m*dt;
            rb1["velocity_y"]=(float)rb1["velocity_y"]+fy/m*dt;
            rb1["_sleeping"]=false;
        }

        // ── RopeJoint2D (Unity2D parity - distance constraint with max length only) ──
        if (has_component(e,"RopeJoint2D")){
            auto& rj=e["components"]["RopeJoint2D"];
            int oid=rj.value("connected_entity",-1);
            if (emap.count(oid)){
                auto* other=emap[oid];
                auto& t2=(*other)["components"]["Transform"];
                float dx=get_float(t2,"x")-get_float(t1,"x");
                float dy=get_float(t2,"y")-get_float(t1,"y");
                float dist=std::hypot(dx,dy);
                float max_len=rj.value("max_length",dist);
                if (dist>max_len){
                    float nx=dx/dist, ny=dy/dist;
                    auto* rb2=has_component(*other,"Rigidbody2D")?&(*other)["components"]["Rigidbody2D"]:nullptr;
                    float im2=body_inv_mass_e(rb2);
                    float tot=im1+im2; if(tot<1e-12f)tot=1;
                    float corr=dist-max_len;
                    float j=corr/dt*BAUMGARTE/tot;
                    // Phase 1: track impulse magnitude for breakable joint support
                    rj["_accumulated_impulse"] = std::abs(j);
                    rb1["velocity_x"]=(float)rb1["velocity_x"]+nx*j*im1;
                    rb1["velocity_y"]=(float)rb1["velocity_y"]+ny*j*im1;
                    if (rb2&&!is_static(rb2)){
                        (*rb2)["velocity_x"]=(float)(*rb2)["velocity_x"]-nx*j*im2;
                        (*rb2)["velocity_y"]=(float)(*rb2)["velocity_y"]-ny*j*im2;
                    }
                }
            }
        }

        // ── Breakable joint check (applies to all joint types) ─────────────
        auto check_break = [&](Entity* joint_ent, Entity* rb_a, Entity* rb_b) {
            if (!joint_ent) return;
            float break_force = joint_ent->value("break_force", BREAK_FORCE_DEFAULT);
            float break_torque = joint_ent->value("break_torque", BREAK_TORQUE_DEFAULT);
            if (break_force <= 0 && break_torque <= 0) return;

            // Estimate reaction force/torque from constraint solver
            // Simplified: use accumulated impulse magnitude
            float impulse = joint_ent->value("_accumulated_impulse", 0.f);
            float force_est = impulse / dt;
            if (force_est > break_force || impulse > break_torque) {
                // Mark for destruction (script will handle actual removal)
                (*joint_ent)["_broken"] = true;
            }
        };

        // Check breakable status for all joints
        if (has_component(e,"DistanceJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"SpringJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"HingeJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"SliderJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"WheelJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"GearJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"RelativeJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"FrictionJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"MouseJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"TargetJoint2D")) check_break(&e,&rb1,nullptr);
        if (has_component(e,"RopeJoint2D")) check_break(&e,&rb1,nullptr);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  15.  SLEEP
// ════════════════════════════════════════════════════════════════════════════
static void update_sleep_islands(EntityList& entities, const std::vector<Manifold>& mfds) {
    // Per-body sleep based on velocity threshold + timer
    // Island propagation: if any island member is awake, wake the rest
    //
    // Static/kinematic bodies are intentionally excluded from adjacency:
    // two dynamic stacks resting on the same static floor are independent
    // islands, not one giant island bridged through the floor. Previously
    // every manifold added an edge regardless of body type, which silently
    // worked (the per-node loop below still skips non-dynamic bodies when
    // checking all_sleep) but needlessly bloated island BFS size on any
    // scene with a shared static floor/wall — exactly the gap doc's
    // "Static body islands... slightly bloating island sizes" note.
    std::unordered_map<int,std::vector<int>> adj;
    std::unordered_map<int,Entity*> emap;
    for (auto& e:entities) emap[e.value("id",0)]=&e;

    auto is_dynamic_entity=[&](int eid)->bool{
        auto it=emap.find(eid);
        if (it==emap.end()||!has_component(*it->second,"Rigidbody2D")) return false;
        return body_type((*it->second)["components"]["Rigidbody2D"])=="dynamic";
    };

    for (auto& m:mfds){
        if (m.is_trigger||m.one_way_skip||m.contact_count==0) continue;
        int id1=m.e1->value("id",0),id2=m.e2->value("id",0);
        if (!is_dynamic_entity(id1) || !is_dynamic_entity(id2)) continue; // skip static/kinematic edges
        adj[id1].push_back(id2); adj[id2].push_back(id1);
    }

    std::unordered_set<int> visited;
    for (auto& e:entities){
        int eid=e.value("id",0);
        if (visited.count(eid)) continue;
        if (!has_component(e,"Rigidbody2D")||body_type(e["components"]["Rigidbody2D"])!="dynamic"){
            visited.insert(eid); continue;
        }
        std::vector<int> island; std::deque<int> q; q.push_back(eid);
        while (!q.empty()){
            int cur=q.front();q.pop_front();
            if (visited.count(cur)) continue;
            visited.insert(cur); island.push_back(cur);
            for (int nb:adj[cur]) if(!visited.count(nb)) q.push_back(nb);
        }
        bool all_sleep=true;
        for (int id:island){
            auto* ep=emap.count(id)?emap[id]:nullptr;
            if (!ep||!has_component(*ep,"Rigidbody2D")) continue;
            auto& rb=(*ep)["components"]["Rigidbody2D"];
            if (body_type(rb)!="dynamic") continue;
            if (!rb.value("allow_sleep",true)){ all_sleep=false; break; }
            float vx=finite_val(rb.value("velocity_x",0.f));
            float vy=finite_val(rb.value("velocity_y",0.f));
            float av=finite_val(rb.value("angular_velocity",0.f));
            float st=finite_val(rb.value("_sleep_t",0.f));
            if (vx*vx+vy*vy>=SLEEP_VEL_SQ||std::abs(av)>=SLEEP_ANG||st<SLEEP_TIME){all_sleep=false;break;}
        }
        if (all_sleep){
            for (int id:island){
                auto* ep=emap.count(id)?emap[id]:nullptr;
                if (!ep||!has_component(*ep,"Rigidbody2D")) continue;
                auto& rb=(*ep)["components"]["Rigidbody2D"];
                if (!is_static(&rb)){
                    rb["_sleeping"]=true;
                    rb["velocity_x"]=0.f; rb["velocity_y"]=0.f; rb["vx"]=0.f; rb["vy"]=0.f; rb["angular_velocity"]=0.f;
                }
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  16.  CONTACT EVENT DISPATCH
// ════════════════════════════════════════════════════════════════════════════
// A contact is identified by both entity IDs *and* its trigger classification.
// Do not try to squeeze the trigger bit into the 64-bit entity-pair key: that
// aliases a real high entity-id bit and cannot be decoded reliably on exit.
struct ContactEventKey {
    int lo = 0;
    int hi = 0;
    bool is_trigger = false;

    bool operator==(const ContactEventKey& other) const noexcept {
        return lo == other.lo && hi == other.hi && is_trigger == other.is_trigger;
    }
};

struct ContactEventKeyHash {
    std::size_t operator()(const ContactEventKey& key) const noexcept {
        std::size_t h1 = std::hash<int>{}(key.lo);
        std::size_t h2 = std::hash<int>{}(key.hi);
        std::size_t h3 = std::hash<bool>{}(key.is_trigger);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

static std::unordered_map<ContactEventKey, bool, ContactEventKeyHash> _contact_state;
// Store entity IDs for exit dispatch.
static std::unordered_map<ContactEventKey, std::pair<int,int>, ContactEventKeyHash> _contact_ids;

void reset_contact_state() {
    _contact_state.clear();
    _contact_ids.clear();
    s_cache.clear();
    s_prev_state.clear();
}

PhysicsRuntimeCacheStats get_runtime_cache_stats() {
    return {
        s_prev_state.size(),
        _contact_state.size(),
        _contact_ids.size(),
        s_collision_ignores.size(),
        s_collider_ignores.size()
    };
}

static void dispatch_contact_events(EntityList& entities, const std::vector<Manifold>& mfds,
                                     std::unordered_map<int,Entity*>& emap) {
    constexpr std::size_t kMaxPendingEventsPerEntity = 256;
    auto call_scripts=[&](Entity* entity, Entity* other, const std::string& method){
        if (!entity) return;
        if (!entity->contains("_pending_events") || !(*entity)["_pending_events"].is_array())
            (*entity)["_pending_events"]=Entity::array();
        auto& queue = (*entity)["_pending_events"];
        // A dense tilemap contact or a temporarily stalled script must not
        // create an unbounded JSON queue. Keep the current frame responsive
        // and expose dropped diagnostics for the editor/runtime profiler.
        if (queue.size() >= kMaxPendingEventsPerEntity) {
            (*entity)["_dropped_physics_events"] =
                entity->value("_dropped_physics_events", 0) + 1;
            return;
        }
        queue.push_back({{"method",method},{"other_id",other?other->value("id",0):0}});
    };

    std::unordered_map<ContactEventKey, bool, ContactEventKeyHash> current;
    for (auto& m:mfds){
        if (m.contact_count==0) continue;
        int id1=m.e1->value("id",0),id2=m.e2->value("id",0);
        int lo=std::min(id1,id2),hi=std::max(id1,id2);
        ContactEventKey key{lo, hi, m.is_trigger};
        current[key]=true;
        _contact_ids[key]={lo,hi};
        bool was=_contact_state.count(key)>0;
        std::string phase=was?"stay":"enter";
        std::string prefix=m.is_trigger?"on_trigger_":"on_collision_";

        // Compute relative velocity at contact for Collision2D.relativeVelocity
        float rv_x=0.f, rv_y=0.f;
        if (m.rb1&&m.rb2){
            rv_x=(float)(*m.rb1)["velocity_x"]-(float)(*m.rb2)["velocity_x"];
            rv_y=(float)(*m.rb1)["velocity_y"]-(float)(*m.rb2)["velocity_y"];
        } else if (m.rb1){
            rv_x=(float)(*m.rb1)["velocity_x"]; rv_y=(float)(*m.rb1)["velocity_y"];
        }
        // Store relative velocity on entity for script access (Collision2D.relativeVelocity)
        if (m.e1) { (*m.e1)["_contact_rel_vel_x"]=rv_x; (*m.e1)["_contact_rel_vel_y"]=rv_y; }
        if (m.e2) { (*m.e2)["_contact_rel_vel_x"]=-rv_x; (*m.e2)["_contact_rel_vel_y"]=-rv_y; }

        call_scripts(m.e1,m.e2,prefix+phase);
        call_scripts(m.e2,m.e1,prefix+phase);

        // Contact listener callbacks
        if (!was && s_contact_listener.on_collision_enter && !m.is_trigger){
            s_contact_listener.on_collision_enter(m.e1, m.e2, m);
        }
        if (!was && s_contact_listener.on_trigger_enter && m.is_trigger){
            s_contact_listener.on_trigger_enter(m.e1, m.e2, m);
        }
        if (was && s_contact_listener.post_solve){
            s_contact_listener.post_solve(m.e1, m.e2, m);
        }
    }
    // Exits
    for (auto& [key,_]:_contact_state){
        if (!current.count(key)){
            const bool trig = key.is_trigger;
            std::string prefix=trig?"on_trigger_":"on_collision_";
            auto it=_contact_ids.find(key);
            if (it!=_contact_ids.end()){
                auto [lo2,hi2]=it->second;
                Entity* ea=emap.count(lo2)?emap[lo2]:nullptr;
                Entity* eb=emap.count(hi2)?emap[hi2]:nullptr;
                call_scripts(ea,eb,prefix+"exit");
                call_scripts(eb,ea,prefix+"exit");

                // Contact listener exit callbacks
                if (trig && s_contact_listener.on_trigger_exit){
                    // Create temporary manifold for callback
                    Manifold temp_m; temp_m.e1=ea; temp_m.e2=eb; temp_m.is_trigger=true;
                    s_contact_listener.on_trigger_exit(ea, eb, temp_m);
                }
                if (!trig && s_contact_listener.on_collision_exit){
                    Manifold temp_m; temp_m.e1=ea; temp_m.e2=eb; temp_m.is_trigger=false;
                    s_contact_listener.on_collision_exit(ea, eb, temp_m);
                }
            }
            // The key is no longer active. Keeping it here used memory
            // proportional to every historical bullet/trigger contact in a
            // session, even after both entities had been destroyed.
            _contact_ids.erase(key);
        }
    }
    _contact_state=current;
}

// ════════════════════════════════════════════════════════════════════════════
//  17.  SPECULATIVE CCD (Improved with swept AABB)
// ════════════════════════════════════════════════════════════════════════════
// For each fast-moving body: extend its AABB by velocity*dt and add extra broad-phase pairs
// We implement it as a velocity clamp: if body would pass through a static within sub_dt,
// back off velocity so the leading edge only reaches the surface.
static void apply_ccd(EntityList& entities, float dt) {
    for (auto& e:entities){
        if (!entity_active(e)||!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb=e["components"]["Rigidbody2D"];
        if (body_type(rb)!="dynamic") continue;
        if (!rb.value("continuous_collision",false)&&!rb.value("ccd",false)) continue;

        float vx=rb.value("velocity_x",0.f),vy=rb.value("velocity_y",0.f);
        float spd=std::hypot(vx,vy);
        if (spd<1e-3f) continue;

        auto shapes=collect_shapes(e);
        if (shapes.empty()) continue;
        auto [sx1,sy1,sx2,sy2]=shape_aabb(shapes[0]);
        float aabb_size=std::max(sx2-sx1,sy2-sy1);
        if (spd*dt < aabb_size*CCD_THRESHOLD) continue; // not fast enough to need CCD

        // Improved CCD: swept AABB test with sub-stepping
        float nx=vx/spd,ny=vy/spd;
        float toi=dt;
        int ccd_substeps=std::min(4,(int)std::ceil(spd*dt/aabb_size));
        float sub_dt_ccd=dt/ccd_substeps;

        for (int sub=0;sub<ccd_substeps;++sub){
            float cur_t=sub*sub_dt_ccd;
            float next_t=(sub+1)*sub_dt_ccd;
            if (next_t>dt) next_t=dt;

            // Predicted position at next_t
            float pred_x=get_float(e["components"]["Transform"],"x")+vx*next_t;
            float pred_y=get_float(e["components"]["Transform"],"y")+vy*next_t;

            // Swept AABB from current to predicted
            float swx1=std::min(sx1,pred_x-(sx2-sx1)*0.5f);
            float swy1=std::min(sy1,pred_y-(sy2-sy1)*0.5f);
            float swx2=std::max(sx2,pred_x+(sx2-sx1)*0.5f);
            float swy2=std::max(sy2,pred_y+(sy2-sy1)*0.5f);

            for (auto& e2:entities){
                if (&e2==&e||!entity_active(e2)) continue;
                if (!has_component(e2,"Rigidbody2D")&&!has_component(e2,"Transform")) continue;
                const auto& rb2c=has_component(e2,"Rigidbody2D")?e2["components"]["Rigidbody2D"]:Entity::object();
                if (!is_static(&rb2c)) continue; // only CCD against statics for now

                auto shapes2=collect_shapes(e2);
                for (auto& s2:shapes2){
                    auto [bx1,by1,bx2,by2]=shape_aabb(s2);
                    // Swept AABB intersection test
                    if (swx2<bx1||swx1>bx2||swy2<by1||swy1>by2) continue;

                    // Slab test along velocity direction
                    float tmin=0,tmax=next_t-cur_t;
                    if (std::abs(nx)>1e-9f){
                        float ta=(bx1-sx2)/vx,tb=(bx2-sx1)/vx;
                        if (ta>tb)std::swap(ta,tb);
                        tmin=std::max(tmin,ta); tmax=std::min(tmax,tb);
                    }
                    if (std::abs(ny)>1e-9f){
                        float ta=(by1-sy2)/vy,tb=(by2-sy1)/vy;
                        if (ta>tb)std::swap(ta,tb);
                        tmin=std::max(tmin,ta); tmax=std::min(tmax,tb);
                    }
                    if (tmin<tmax&&tmin>=0&&tmin<toi-cur_t){
                        toi=cur_t+tmin*0.98f; // small epsilon
                    }
                }
            }
            if (toi<next_t) break;
        }

        if (toi<dt){
            // Rewind position to the time-of-impact so the body arrives at the
            // contact surface rather than already penetrating it. Previously only
            // velocity was scaled, leaving the body embedded — the split-impulse
            // solver (BIAS=0.20) could only remove 20% of penetration per step,
            // causing the player to visibly sink through floors before recovery.
            auto& tr=e["components"]["Transform"];
            float cur_x=get_float(tr,"x"), cur_y=get_float(tr,"y");
            float overshoot_x = vx * (dt - toi);
            float overshoot_y = vy * (dt - toi);
            tr["x"] = finite_val(cur_x - overshoot_x);
            tr["y"] = finite_val(cur_y - overshoot_y);
            transform::mark_local_dirty(e.value("id",0));
            rb["velocity_x"]=(float)vx*toi/dt; rb["vx"]=(float)vx*toi/dt;
            rb["velocity_y"]=(float)vy*toi/dt; rb["vy"]=(float)vy*toi/dt;
        }
    }
}

// Unity's ContinuousDynamic mode checks swept shape vs ALL dynamic bodies,
// not just statics. We implement this as a second CCD pass.
static void apply_ccd_dynamic(EntityList& entities, float dt) {
    for (auto& e:entities){
        if (!entity_active(e)||!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb=e["components"]["Rigidbody2D"];
        if (body_type(rb)!="dynamic") continue;
        // Only run for bodies with continuous_dynamic = true
        if (!rb.value("continuous_dynamic",false)) continue;

        float vx=rb.value("velocity_x",0.f),vy=rb.value("velocity_y",0.f);
        float spd=std::hypot(vx,vy);
        if (spd<1e-3f) continue;

        auto shapes=collect_shapes(e);
        if (shapes.empty()) continue;
        auto [sx1,sy1,sx2,sy2]=shape_aabb(shapes[0]);
        float aabb_size=std::max(sx2-sx1,sy2-sy1);
        if (spd*dt < aabb_size*CCD_THRESHOLD) continue;

        float toi=dt;

        for (auto& e2:entities){
            if (&e2==&e||!entity_active(e2)) continue;
            if (!has_component(e2,"Rigidbody2D")||!has_component(e2,"Transform")) continue;
            auto& rb2c=e2["components"]["Rigidbody2D"];
            if (is_static(&rb2c)) continue; // statics handled by apply_ccd
            if (body_type(rb2c)!="dynamic") continue;

            // Relative velocity for swept test
            float vx2=rb2c.value("velocity_x",0.f), vy2=rb2c.value("velocity_y",0.f);
            float rvx=vx-vx2, rvy=vy-vy2;
            float rv_spd=std::hypot(rvx,rvy);
            if (rv_spd<1e-3f) continue;

            auto shapes2=collect_shapes(e2);
            for (auto& s2:shapes2){
                auto [bx1,by1,bx2,by2]=shape_aabb(s2);
                // Swept AABB using relative velocity
                float swx1=std::min(sx1,sx1+rvx*dt), swy1=std::min(sy1,sy1+rvy*dt);
                float swx2=std::max(sx2,sx2+rvx*dt), swy2=std::max(sy2,sy2+rvy*dt);
                if (swx2<bx1||swx1>bx2||swy2<by1||swy1>by2) continue;

                float tmin=0,tmax=dt;
                float rnx=rvx/rv_spd, rny=rvy/rv_spd;
                if (std::abs(rnx)>1e-9f){
                    float ta=(bx1-sx2)/rvx,tb=(bx2-sx1)/rvx;
                    if(ta>tb)std::swap(ta,tb);
                    tmin=std::max(tmin,ta); tmax=std::min(tmax,tb);
                }
                if (std::abs(rny)>1e-9f){
                    float ta=(by1-sy2)/rvy,tb=(by2-sy1)/rvy;
                    if(ta>tb)std::swap(ta,tb);
                    tmin=std::max(tmin,ta); tmax=std::min(tmax,tb);
                }
                if (tmin<tmax&&tmin>=0&&tmin<toi){
                    toi=tmin*0.98f;
                }
            }
        }

        if (toi<dt){
            float scale=toi/dt;
            rb["velocity_x"]=(float)vx*scale; rb["vx"]=(float)vx*scale;
            rb["velocity_y"]=(float)vy*scale; rb["vy"]=(float)vy*scale;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  18.  QUERIES
// ════════════════════════════════════════════════════════════════════════════
std::vector<Entity*> point_cast(EntityList& entities, float x, float y, int mask) {
    std::vector<Entity*> hits;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        if (has_component(e,"Rigidbody2D")){
            int layer=e["components"]["Rigidbody2D"].value("layer",0);
            if (!(mask&(1<<layer))) continue;
        }
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            bool hit=false;
            if (sh.kind==ShapeKind::Circle){float dx=x-sh.cx,dy=y-sh.cy;hit=dx*dx+dy*dy<=sh.radius*sh.radius;}
            else if (sh.kind==ShapeKind::Polygon) hit=point_in_poly(x,y,sh.verts);
            else if (sh.kind==ShapeKind::Capsule){
                auto [closest,d2]=closest_on_seg(sh.world_pts[0].first,sh.world_pts[0].second,
                                                  sh.world_pts[1].first,sh.world_pts[1].second,x,y);
                hit=d2<=sh.radius*sh.radius;
            }
            if (hit){hits.push_back(&e);break;}
        }
    }
    return hits;
}

std::vector<Entity*> overlap_circle(EntityList& entities, float x, float y, float r, int mask) {
    std::vector<Entity*> hits;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            bool hit=false;
            if (sh.kind==ShapeKind::Circle){float dx=sh.cx-x,dy=sh.cy-y;hit=dx*dx+dy*dy<=(sh.radius+r)*(sh.radius+r);}
            else if (sh.kind==ShapeKind::Polygon){
                hit=point_in_poly(x,y,sh.verts);
                if (!hit){float r2=r*r;for(int i=0;i<(int)sh.verts.size();++i){auto [a,b]=closest_on_seg(sh.verts[i].first,sh.verts[i].second,sh.verts[(i+1)%sh.verts.size()].first,sh.verts[(i+1)%sh.verts.size()].second,x,y);if(a.first*0+b<=r2){hit=true;break;}}}
            }
            else if (sh.kind==ShapeKind::Capsule){
                auto [p,d2]=closest_on_seg(sh.world_pts[0].first,sh.world_pts[0].second,sh.world_pts[1].first,sh.world_pts[1].second,x,y);
                hit=d2<=(sh.radius+r)*(sh.radius+r);
            }
            if (hit){hits.push_back(&e);break;}
        }
    }
    return hits;
}

std::vector<Entity*> overlap_box(EntityList& entities, float x, float y, float w, float h, float rot_deg, int mask) {
    float rot=rot_deg*(float)M_PI/180.f,c=std::cos(rot),s=std::sin(rot);
    float hw=w*0.5f,hh=h*0.5f;
    Verts qv;
    for(auto [dx,dy]:std::initializer_list<Vec2>{{-hw,-hh},{hw,-hh},{hw,hh},{-hw,hh}})
        qv.push_back(world_from_local(x,y,c,s,dx,dy));
    Shape q; q.kind=ShapeKind::Polygon; q.verts=ensure_ccw(qv); q.cx=x;q.cy=y;
    std::vector<Entity*> hits;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            Shape qc=q;
            auto m=dispatch_shapes(qc,sh);
            if (m){hits.push_back(&e);break;}
        }
    }
    return hits;
}

// Enhanced queries with filters
std::vector<Entity*> point_cast_filtered(EntityList& entities, float x, float y, const QueryFilter& filter) {
    std::vector<Entity*> hits;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        if (filter.predicate && !filter.predicate(&e)) continue;
        if (has_component(e,"Rigidbody2D")){
            int layer=e["components"]["Rigidbody2D"].value("layer",0);
            if (!(filter.layer_mask&(1<<layer))) continue;
        }
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            bool hit=false;
            if (sh.kind==ShapeKind::Circle){float dx=x-sh.cx,dy=y-sh.cy;hit=dx*dx+dy*dy<=sh.radius*sh.radius;}
            else if (sh.kind==ShapeKind::Polygon) hit=point_in_poly(x,y,sh.verts);
            else if (sh.kind==ShapeKind::Capsule){
                auto [closest,d2]=closest_on_seg(sh.world_pts[0].first,sh.world_pts[0].second,
                                                  sh.world_pts[1].first,sh.world_pts[1].second,x,y);
                hit=d2<=sh.radius*sh.radius;
            }
            if (hit){
                if (!filter.triggers && sh.col && sh.col->value("is_trigger",false)) continue;
                hits.push_back(&e);break;
            }
        }
    }
    return hits;
}

std::vector<Entity*> overlap_circle_filtered(EntityList& entities, float x, float y, float r, const QueryFilter& filter) {
    std::vector<Entity*> hits;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        if (filter.predicate && !filter.predicate(&e)) continue;
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            bool hit=false;
            if (sh.kind==ShapeKind::Circle){float dx=sh.cx-x,dy=sh.cy-y;hit=dx*dx+dy*dy<=(sh.radius+r)*(sh.radius+r);}
            else if (sh.kind==ShapeKind::Polygon){
                hit=point_in_poly(x,y,sh.verts);
                if (!hit){float r2=r*r;for(int i=0;i<(int)sh.verts.size();++i){auto [a,b]=closest_on_seg(sh.verts[i].first,sh.verts[i].second,sh.verts[(i+1)%sh.verts.size()].first,sh.verts[(i+1)%sh.verts.size()].second,x,y);if(a.first*0+b<=r2){hit=true;break;}}}
            }
            else if (sh.kind==ShapeKind::Capsule){
                auto [p,d2]=closest_on_seg(sh.world_pts[0].first,sh.world_pts[0].second,sh.world_pts[1].first,sh.world_pts[1].second,x,y);
                hit=d2<=(sh.radius+r)*(sh.radius+r);
            }
            if (hit){
                if (!filter.triggers && sh.col && sh.col->value("is_trigger",false)) continue;
                hits.push_back(&e);break;
            }
        }
    }
    return hits;
}

std::vector<Entity*> overlap_box_filtered(EntityList& entities, float x, float y, float w, float h, float rot_deg, const QueryFilter& filter) {
    float rot=rot_deg*(float)M_PI/180.f,c=std::cos(rot),s=std::sin(rot);
    float hw=w*0.5f,hh=h*0.5f;
    Verts qv;
    for(auto [dx,dy]:std::initializer_list<Vec2>{{-hw,-hh},{hw,-hh},{hw,hh},{-hw,hh}})
        qv.push_back(world_from_local(x,y,c,s,dx,dy));
    Shape q; q.kind=ShapeKind::Polygon; q.verts=ensure_ccw(qv); q.cx=x;q.cy=y;
    std::vector<Entity*> hits;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        if (filter.predicate && !filter.predicate(&e)) continue;
        if (has_component(e,"Rigidbody2D")){
            int layer=e["components"]["Rigidbody2D"].value("layer",0);
            if (!(filter.layer_mask&(1<<layer))) continue;
        }
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            Shape qc=q;
            auto m=dispatch_shapes(qc,sh);
            if (m){
                if (!filter.triggers && sh.col && sh.col->value("is_trigger",false)) continue;
                hits.push_back(&e);break;
            }
        }
    }
    return hits;
}

static std::optional<RayHit> ray_vs_shape(const Shape& sh, float ox, float oy, float dx, float dy, float dist_max, Entity* ent) {
    std::optional<RayHit> hit;
    if (sh.kind==ShapeKind::Circle){
        float fx=ox-sh.cx,fy=oy-sh.cy,r=sh.radius;
        float b=2*(fx*dx+fy*dy),cc=fx*fx+fy*fy-r*r;
        float disc=b*b-4*cc;
        if (disc>=0){
            float ss=std::sqrt(disc),t=(-b-ss)*0.5f;
            if (t<0)t=(-b+ss)*0.5f;
            if (t>=0&&t<=dist_max){
                float px=ox+dx*t,py=oy+dy*t;
                auto [nx2,ny2]=normalize(px-sh.cx,py-sh.cy);
                hit=RayHit{t,{px,py},{nx2,ny2},ent,sh.col};
            }
        }
    } else if (sh.kind==ShapeKind::Polygon){
        const auto& vs=sh.verts;
        for (int i=0;i<(int)vs.size();++i){
            auto [ax,ay]=vs[i]; auto [bx,by]=vs[(i+1)%vs.size()];
            float den=cross(dx,dy,bx-ax,by-ay);
            if (std::abs(den)<1e-12f) continue;
            float t=cross(ax-ox,ay-oy,bx-ax,by-ay)/den;
            float u=cross(ax-ox,ay-oy,dx,dy)/den;
            if (t>=0&&t<=dist_max&&u>=0&&u<=1){
                float px=ox+dx*t,py=oy+dy*t;
                auto [nx2,ny2]=normalize(by-ay,ax-bx);
                if (!hit||t<hit->distance) hit=RayHit{t,{px,py},{nx2,ny2},ent,sh.col};
            }
        }
    } else if (sh.kind==ShapeKind::Capsule){
        // Segment-by-segment with endcap circles
        Vec2 A=sh.world_pts[0],B=sh.world_pts[1];
        // Check segment as cylinder: closest approach
        for (int cap=0;cap<3;++cap){
            float cx2,cy2; float R=sh.radius;
            if (cap==0){cx2=A.first;cy2=A.second;}
            else if (cap==1){cx2=B.first;cy2=B.second;}
            else {cx2=sh.cx;cy2=sh.cy;R=sh.radius*1.001f;}
            float fx2=ox-cx2,fy2=oy-cy2;
            float bb=2*(fx2*dx+fy2*dy),cc2=fx2*fx2+fy2*fy2-R*R;
            float disc2=bb*bb-4*cc2;
            if (disc2<0) continue;
            float ss2=std::sqrt(disc2),t=(-bb-ss2)*0.5f;
            if (t<0)t=(-bb+ss2)*0.5f;
            if (t>=0&&t<=dist_max){
                float px=ox+dx*t,py=oy+dy*t;
                auto [nx2,ny2]=normalize(px-cx2,py-cy2);
                if (!hit||t<hit->distance) hit=RayHit{t,{px,py},{nx2,ny2},ent,sh.col};
            }
        }
    }
    return hit;
}

std::optional<RayHit> raycast(EntityList& entities, float ox, float oy, float dx, float dy,
                               float dist_max, int mask, bool triggers) {
    float dlen=std::hypot(dx,dy); if(dlen<1e-12f) return std::nullopt;
    dx/=dlen; dy/=dlen;
    bool allow_start_in = get_queries_start_in_colliders();
    std::optional<RayHit> best;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            if (sh.col&&sh.col->value("is_trigger",false)&&!triggers) continue;
            // queriesStartInColliders: skip shapes that contain the origin
            if (!allow_start_in) {
                bool inside = false;
                switch (sh.kind) {
                case ShapeKind::Circle: { float fx=ox-sh.cx,fy=oy-sh.cy; inside=(fx*fx+fy*fy<=sh.radius*sh.radius); break; }
                case ShapeKind::Polygon: inside=point_in_poly(ox,oy,sh.verts); break;
                case ShapeKind::Capsule: { auto [cl,d2]=closest_on_seg(sh.world_pts[0].first,sh.world_pts[0].second,sh.world_pts[1].first,sh.world_pts[1].second,ox,oy); inside=(d2<=sh.radius*sh.radius); break; }
                default: break;
                }
                if (inside) continue;
            }
            auto h=ray_vs_shape(sh,ox,oy,dx,dy,dist_max,&e);
            if (h&&(!best||h->distance<best->distance)) best=h;
        }
    }
    return best;
}

std::vector<RayHit> raycast_all(EntityList& entities, float ox, float oy, float dx, float dy,
                                  float dist_max, int mask, bool triggers) {
    float dlen=std::hypot(dx,dy); if(dlen<1e-12f) return {};
    dx/=dlen; dy/=dlen;
    bool allow_start_in = get_queries_start_in_colliders();
    std::vector<RayHit> hits;
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        auto shapes=collect_shapes(e);
        for (auto& sh:shapes){
            if (sh.col&&sh.col->value("is_trigger",false)&&!triggers) continue;
            if (!allow_start_in) {
                bool inside = false;
                switch (sh.kind) {
                case ShapeKind::Circle: { float fx=ox-sh.cx,fy=oy-sh.cy; inside=(fx*fx+fy*fy<=sh.radius*sh.radius); break; }
                case ShapeKind::Polygon: inside=point_in_poly(ox,oy,sh.verts); break;
                case ShapeKind::Capsule: { auto [cl,d2]=closest_on_seg(sh.world_pts[0].first,sh.world_pts[0].second,sh.world_pts[1].first,sh.world_pts[1].second,ox,oy); inside=(d2<=sh.radius*sh.radius); break; }
                default: break;
                }
                if (inside) continue;
            }
            auto h=ray_vs_shape(sh,ox,oy,dx,dy,dist_max,&e);
            if (h) hits.push_back(*h);
        }
    }
    std::sort(hits.begin(),hits.end(),[](auto& a,auto& b){return a.distance<b.distance;});
    return hits;
}

// ════════════════════════════════════════════════════════════════════════════
//  18b.  ISLAND-BASED SOLVING (Phase 3)
//
//  Box2D/Unity solve each connected component ("island") of touching dynamic
//  bodies independently. Nova previously ran VELOCITY_ITERS/POSITION_ITERS
//  globally across every manifold in the scene each substep, which means
//  iterations are "wasted" converging contacts in one corner of the level
//  while a completely unrelated stack on the other side of the map gets the
//  same fixed iteration budget it would've gotten solving alone.
//
//  Splitting into islands doesn't change the physics result (each island is,
//  by construction, non-interacting with the others within this step), but
//  it means the existing solve_velocities/correct_positions calls each only
//  ever see their own island's manifolds — which is both faster (no need to
//  touch unrelated Manifold entries every iteration) and is what actually
//  lets future per-island work (parallelism, adaptive iteration counts)
//  slot in later without changing this structure again.
//
//  Static and kinematic bodies are NOT used to merge islands together (this
//  also resolves the gap-doc's "Static body islands" note: two dynamic
//  stacks resting on the same static floor are independent islands, not one
//  giant island bridged through the floor).
// ════════════════════════════════════════════════════════════════════════════
static bool s_island_solving_enabled = true;

void set_island_solving_enabled(bool enabled) { s_island_solving_enabled = enabled; }
bool get_island_solving_enabled() { return s_island_solving_enabled; }

// Partition `manifolds` into groups whose dynamic-body sets don't touch each
// other. A manifold is assigned to a group by its two dynamic-body endpoints;
// edges through a static/kinematic body do not merge groups (mirrors
// update_sleep_islands's adjacency rule, but for *solving*, not sleep).
static std::vector<std::vector<Manifold*>> partition_islands(std::vector<Manifold>& manifolds) {
    // Union-Find over dynamic-body entity ids that appear in non-trigger
    // manifolds together. Bodies that never appear with another dynamic body
    // in a real (non-trigger) contact end up as their own singleton island.
    std::unordered_map<int,int> parent; // entity id -> parent id (union-find)
    auto find=[&](int x)->int{
        while (parent.count(x) && parent[x]!=x) {
            if (parent.count(parent[x])) x=parent[x]=parent[parent[x]];
            else x=parent[x];
        }
        return x;
    };
    auto make_set=[&](int x){ if(!parent.count(x)) parent[x]=x; };
    auto unite=[&](int a,int b){
        make_set(a); make_set(b);
        int ra=find(a), rb=find(b);
        if (ra!=rb) parent[ra]=rb;
    };

    auto dyn_id=[&](Entity* rb, Entity* e)->int{
        // Returns the entity id if rb is a dynamic body, else -1 (statics/
        // kinematics don't propagate island membership).
        if (!rb||!e) return -1;
        if (body_type(*rb)!="dynamic") return -1;
        return e->value("id",0);
    };

    std::vector<Manifold*> relevant;
    relevant.reserve(manifolds.size());
    for (auto& m:manifolds) {
        if (m.is_trigger||m.one_way_skip||m.contact_count==0) continue;
        relevant.push_back(&m);
        int id1=dyn_id(m.rb1,m.e1), id2=dyn_id(m.rb2,m.e2);
        if (id1>=0) make_set(id1);
        if (id2>=0) make_set(id2);
        if (id1>=0 && id2>=0) unite(id1,id2);
    }

    // Group manifolds by the root of either dynamic endpoint. A manifold
    // touching a static body only (id of the other side == -1) is keyed by
    // whichever side is dynamic; manifolds between two statics can't occur
    // (no solver work needed) but are handled defensively as their own group.
    std::unordered_map<int,int> root_to_group;
    std::vector<std::vector<Manifold*>> islands;
    for (auto* mp:relevant) {
        int id1=dyn_id(mp->rb1,mp->e1), id2=dyn_id(mp->rb2,mp->e2);
        int rep = id1>=0 ? find(id1) : (id2>=0 ? find(id2) : -1);
        if (rep<0) {
            // Two statics touching (shouldn't normally produce a solvable
            // manifold since both have inv_mass 0, but keep it safe).
            islands.push_back({mp});
            continue;
        }
        auto it = root_to_group.find(rep);
        if (it==root_to_group.end()) {
            root_to_group[rep] = (int)islands.size();
            islands.push_back({mp});
        } else {
            islands[it->second].push_back(mp);
        }
    }
    return islands;
}

// ════════════════════════════════════════════════════════════════════════════
//  19.  MAIN STEP
// ════════════════════════════════════════════════════════════════════════════
void apply_physics(EntityList& entities, float dt, float gravity) {
    // A combat script can spawn projectiles/FX immediately before physics.
    // That may reallocate EntityList and invalidate the transform registry's
    // raw entity pointers.  Refresh at the simulation boundary, before any
    // collider asks transform::cached_world() for a world transform.
    transform::ensure_registry_current(entities);
    s_active_entities = &entities;
    if (dt<=0) {
        s_active_entities = nullptr;
        return;
    }

    // Apply physics time scale
    float scaled_dt = dt * s_physics_time_scale;
    if (scaled_dt <= 0) {
        s_active_entities = nullptr;
        return;
    }

    prune_collision_ignores(entities);
    prune_interpolation_state(entities);

    // Reset stats for this frame
    s_physics_stats = PhysicsStats();

    // Count bodies
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        if (has_component(e,"Rigidbody2D")){
            s_physics_stats.total_bodies++;
            auto& rb=e["components"]["Rigidbody2D"];
            std::string bt=body_type(rb);
            if (bt=="dynamic") s_physics_stats.dynamic_bodies++;
            else if (bt=="static") s_physics_stats.static_bodies++;
            else if (bt=="kinematic") s_physics_stats.kinematic_bodies++;
            if (rb.value("_sleeping",false)) s_physics_stats.sleep_count++;
        }
    }

    int substeps=std::max(1,std::min(MAX_SUBSTEPS,(int)std::ceil(scaled_dt/TARGET_STEP)));
    float sub_dt=scaled_dt/substeps;

    std::unordered_map<int,Entity*> emap;
    for (auto& e:entities) emap[e.value("id",0)]=&e;

    std::vector<Manifold> manifolds;

    for (int sub=0;sub<substeps;++sub){
        apply_constant_forces(entities);    // ① constant per-frame forces
        apply_buoyancy(entities);           // ② buoyancy volumes
        apply_point_effectors(entities);    // ③ radial fields
        apply_area_effectors(entities);     // ④ area effectors
        integrate(entities, sub_dt, gravity); // ⑤ integrate forces → new velocities & positions
        apply_ccd(entities, sub_dt);        // ⑥ CCD velocity clamp AFTER integrate so speeds are known
        apply_ccd_dynamic(entities, sub_dt); // ⑥b ContinuousDynamic CCD
        solve_joints(entities, sub_dt, emap);  // ⑦ joints

        auto pairs=broad_phase(entities);
        s_physics_stats.broad_phase_pairs = (int)pairs.size();
        manifolds=narrow_phase(pairs, s_cache); // ④ narrow (warm-start from cache)
        s_physics_stats.total_contacts = 0;
        for (auto& m:manifolds) {
            s_physics_stats.total_contacts += m.contact_count;
            // Invalidate per-contact effective mass cache — must recompute each
            // substep because lever arms (r1,r2) may have changed with body rotation.
            for (int ci=0;ci<m.contact_count;++ci) m.contacts[ci].k_cached = false;
        }

        // Warm-start: apply cached lambdas before iterations
        for (auto& m:manifolds){
            if (m.is_trigger||m.one_way_skip) continue;
            float inv_m1=body_inv_mass(m.rb1),inv_m2=body_inv_mass(m.rb2);
            float inv_I1=body_inv_inertia(m.rb1,m.e1),inv_I2=body_inv_inertia(m.rb2,m.e2);
            bool fr1=!m.rb1||is_static(m.rb1)||m.rb1->value("freeze_rotation",false);
            bool fr2=!m.rb2||is_static(m.rb2)||m.rb2->value("freeze_rotation",false);
            for (int ci=0;ci<m.contact_count;++ci){
                auto& cp=m.contacts[ci];
                if (std::abs(cp.lambda_n)<1e-9f&&std::abs(cp.lambda_t)<1e-9f) continue;
                Entity _dt1=Entity::object(),_dt2=Entity::object();
                auto& t1c=m.e1&&has_component(*m.e1,"Transform")?(*m.e1)["components"]["Transform"]:_dt1;
                auto& t2c=m.e2&&has_component(*m.e2,"Transform")?(*m.e2)["components"]["Transform"]:_dt2;
                bool ht1=m.e1&&has_component(*m.e1,"Transform");
                bool ht2=m.e2&&has_component(*m.e2,"Transform");
                float t1x=ht1?get_float(t1c,"x"):0.f,t1y=ht1?get_float(t1c,"y"):0.f;
                float t2x=ht2?get_float(t2c,"x"):0.f,t2y=ht2?get_float(t2c,"y"):0.f;
                float r1x=cp.x-t1x,r1y=cp.y-t1y,r2x=cp.x-t2x,r2y=cp.y-t2y;
                // Apply stored normal impulse
                if (m.rb1&&inv_m1>0){(*m.rb1)["velocity_x"]=(float)(*m.rb1)["velocity_x"]-cp.lambda_n*inv_m1*m.nx;(*m.rb1)["velocity_y"]=(float)(*m.rb1)["velocity_y"]-cp.lambda_n*inv_m1*m.ny;if(!fr1)(*m.rb1)["angular_velocity"]=(float)(*m.rb1)["angular_velocity"]-cp.lambda_n*inv_I1*cross(r1x,r1y,m.nx,m.ny);}
                if (m.rb2&&inv_m2>0){(*m.rb2)["velocity_x"]=(float)(*m.rb2)["velocity_x"]+cp.lambda_n*inv_m2*m.nx;(*m.rb2)["velocity_y"]=(float)(*m.rb2)["velocity_y"]+cp.lambda_n*inv_m2*m.ny;if(!fr2)(*m.rb2)["angular_velocity"]=(float)(*m.rb2)["angular_velocity"]+cp.lambda_n*inv_I2*cross(r2x,r2y,m.nx,m.ny);}
                // Apply stored tangent impulse
                float tx=-m.ny,ty=m.nx;
                if (m.rb1&&inv_m1>0){(*m.rb1)["velocity_x"]=(float)(*m.rb1)["velocity_x"]-cp.lambda_t*inv_m1*tx;(*m.rb1)["velocity_y"]=(float)(*m.rb1)["velocity_y"]-cp.lambda_t*inv_m1*ty;if(!fr1)(*m.rb1)["angular_velocity"]=(float)(*m.rb1)["angular_velocity"]-cp.lambda_t*inv_I1*cross(r1x,r1y,tx,ty);}
                if (m.rb2&&inv_m2>0){(*m.rb2)["velocity_x"]=(float)(*m.rb2)["velocity_x"]+cp.lambda_t*inv_m2*tx;(*m.rb2)["velocity_y"]=(float)(*m.rb2)["velocity_y"]+cp.lambda_t*inv_m2*ty;if(!fr2)(*m.rb2)["angular_velocity"]=(float)(*m.rb2)["angular_velocity"]+cp.lambda_t*inv_I2*cross(r2x,r2y,tx,ty);}
            }
        }

        if (s_island_solving_enabled) {
            // Solve each connected island of dynamic bodies independently.
            // Same total work per contact as the global path (each manifold
            // still gets VELOCITY_ITERS velocity passes and POSITION_ITERS
            // position passes) — the difference is that an island's
            // iterations only ever touch that island's own manifolds, so a
            // big isolated stack elsewhere in the scene can't dilute/starve
            // a small island that would've converged in fewer passes, and
            // vice versa. This also keeps the door open for solving islands
            // in parallel later without further restructuring.
            auto islands = partition_islands(manifolds);
            for (auto& island : islands) {
                for (int vi=0; vi<VELOCITY_ITERS; ++vi) solve_velocities(island, sub_dt); // ⑤ velocity solver
                correct_positions_impl(island, sub_dt);                                    // ⑥ position solver (split impulse)
            }
        } else {
            for (int vi=0;vi<VELOCITY_ITERS;++vi) solve_velocities(manifolds, sub_dt); // ⑤ velocity solver
            correct_positions(manifolds, sub_dt);                                 // ⑥ position solver (split impulse)
        }
    }

        // Update contact cache with final lambdas and smoothed normals
    ContactCache prev_cache = s_cache;  // snapshot before clear to inherit smooth normals
    s_cache.clear();
    for (auto& m:manifolds){
        if (m.is_trigger||m.contact_count==0) continue;
        int id1=m.e1->value("id",0), id2=m.e2->value("id",0);
        long long ckey = contact_cache_key(id1, id2, m.sub1, m.sub2);
        CachedManifold cm;
        cm.count=m.contact_count; cm.sub1=m.sub1; cm.sub2=m.sub2;
        cm.nx=m.nx; cm.ny=m.ny;
        // Inherit and update smooth normal from previous frame
        auto prev_it = prev_cache.find(ckey);
        if (prev_it != prev_cache.end()) {
            float psnx = prev_it->second.smooth_nx, psny = prev_it->second.smooth_ny;
            float blend = std::min(NORMAL_SMOOTH_ALPHA * (float)prev_it->second.age, 0.6f);
            float sdot = m.nx * psnx + m.ny * psny;
            if (sdot > 0.1f) {
                float snx = m.nx * (1.f - blend) + psnx * blend;
                float sny = m.ny * (1.f - blend) + psny * blend;
                float sl = std::hypot(snx, sny);
                if (sl > 1e-9f) { snx /= sl; sny /= sl; }
                cm.smooth_nx = snx; cm.smooth_ny = sny;
            } else {
                cm.smooth_nx = m.nx; cm.smooth_ny = m.ny;
            }
            cm.age = std::min(prev_it->second.age + 1, 10);
        } else {
            cm.smooth_nx = m.nx; cm.smooth_ny = m.ny;
            cm.age = 1;
        }
        for (int ci=0;ci<m.contact_count;++ci) cm.contacts[ci]=m.contacts[ci];
        s_cache[ckey]=cm;
    }

    collect_ground_info(entities, manifolds, sub_dt);
    update_sleep_islands(entities,manifolds);
    dispatch_contact_events(entities,manifolds,emap);

    // Sync vx/vy from velocity_x/velocity_y for scripting revolution compatibility
    for (auto& e:entities){
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D")) continue;
        auto& rb=e["components"]["Rigidbody2D"];
        if (body_type(rb)=="dynamic"||body_type(rb)=="kinematic"){
            rb["vx"]=(float)rb["velocity_x"];
            rb["vy"]=(float)rb["velocity_y"];
        }
    }

    s_active_entities = nullptr;
}

// ════════════════════════════════════════════════════════════════════════════
//  Phase 1: FIXED TIMESTEP ACCUMULATOR
//  Wraps apply_physics() with a carry-over accumulator so physics always
//  advances in exact s_fixed_dt steps regardless of the raw frame delta.
//  Returns the render interpolation alpha [0,1] for the caller to pass to
//  get_render_position() / get_interpolated_rotation().
//
//  Typical game-loop usage:
//      float alpha = apply_physics_accumulated(entities, frame_dt);
//      // then render all bodies using get_render_position(e, alpha)
// ════════════════════════════════════════════════════════════════════════════
float apply_physics_accumulated(EntityList& entities, float frame_dt, float gravity) {
    if (frame_dt <= 0.f) return s_render_alpha;

    // Hard cap: never simulate more than MAX_SUBSTEPS fixed steps per frame
    // to avoid the spiral-of-death on very slow frames.
    float max_consume = s_fixed_dt * (float)MAX_SUBSTEPS;
    s_accumulator += frame_dt * s_physics_time_scale;
    if (s_accumulator > max_consume) s_accumulator = max_consume;

    while (s_accumulator >= s_fixed_dt) {
        apply_physics(entities, s_fixed_dt, gravity);
        s_accumulator -= s_fixed_dt;
    }

    s_render_alpha = s_accumulator / s_fixed_dt;
    return s_render_alpha;
}

// ════════════════════════════════════════════════════════════════════════════
//  20b.  CONSTANT FORCE 2D
//  Mirrors Unity's ConstantForce2D: applies a per-body continuous force and
//  optional torque each frame.  Call once per frame BEFORE apply_physics.
// ════════════════════════════════════════════════════════════════════════════
void apply_constant_forces(EntityList& entities) {
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"ConstantForce2D")) continue;
        auto& rb  = e["components"]["Rigidbody2D"];
        auto& cf  = e["components"]["ConstantForce2D"];
        if (is_static(&rb)) continue;

        float fx = finite_val(cf.value("force_x", 0.f));
        float fy = finite_val(cf.value("force_y", 0.f));
        float tq = finite_val(cf.value("torque",  0.f));

        if (cf.value("relative", false) && has_component(e,"Transform")) {
            float rot_deg = finite_val(get_float(e["components"]["Transform"],"rotation"));
            float rot = rot_deg * (float)M_PI / 180.f;
            float c = std::cos(rot), s = std::sin(rot);
            float wfx = fx*c - fy*s;
            float wfy = fx*s + fy*c;
            fx = wfx; fy = wfy;
        }

        add_force(rb, fx, fy);
        if (std::abs(tq) > 1e-9f) add_torque(rb, tq);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  20c.  BUOYANCY EFFECTOR 2D
//  Overlapping dynamic bodies get upward buoyancy + linear/angular drag.
//  Component "BuoyancyEffector2D": density, surface_level, linear_drag, angular_drag.
//  Call once per frame BEFORE apply_physics.
// ════════════════════════════════════════════════════════════════════════════
void apply_buoyancy(EntityList& entities) {
    // Collect fluid volumes
    struct FluidVolume {
        float x1,y1,x2,y2;    // AABB of the trigger region
        float surface_y;       // world Y of water surface
        float density;
        float lin_drag;
        float ang_drag;
    };
    std::vector<FluidVolume> fluids;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"BuoyancyEffector2D")) continue;
        auto& be = e["components"]["BuoyancyEffector2D"];
        // Build AABB from the entity's collider shape
        auto shapes = collect_shapes(e);
        for (auto& sh : shapes) {
            auto [ax,ay,bx,by] = shape_aabb(sh);
            FluidVolume fv;
            fv.x1 = ax; fv.y1 = ay; fv.x2 = bx; fv.y2 = by;
            fv.surface_y  = finite_val(be.value("surface_level", ay), ay);
            fv.density    = std::max(0.f, finite_val(be.value("density", 1.f)));
            fv.lin_drag   = std::max(0.f, finite_val(be.value("linear_drag", 1.f)));
            fv.ang_drag   = std::max(0.f, finite_val(be.value("angular_drag", 0.5f)));
            fluids.push_back(fv);
        }
    }
    if (fluids.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (is_static(&rb)) continue;

        float mass = std::max(finite_val(rb.value("mass",1.f), 1.f), 1e-9f);
        auto shapes = collect_shapes(e);
        if (shapes.empty()) continue;
        auto [bx1,by1,bx2,by2] = shape_aabb(shapes[0]);
        float body_h = std::max(1.f, by2 - by1);
        float body_area = std::max(1.f, (bx2-bx1) * body_h);

        for (auto& fv : fluids) {
            // Quick AABB overlap
            if (bx2 < fv.x1 || bx1 > fv.x2 || by2 < fv.y1 || by1 > fv.y2) continue;

            // Fraction of body submerged (below surface_y)
            float submerged_top    = std::min(by2, fv.surface_y);
            float submerged_bottom = std::max(by1, fv.y1);
            float submerged_h = std::max(0.f, submerged_top - submerged_bottom);
            float fraction = submerged_h / body_h;
            if (fraction < 1e-4f) continue;

            // Buoyancy force = displaced_volume * fluid_density * gravity
            // In 2D we use area as proxy for volume.
            float buoy = fraction * body_area * fv.density * GRAVITY;
            add_force(rb, 0.f, -buoy);   // upward (y-up convention: -y is up if gravity is +y)

            // Fluid drag (velocity-proportional)
            float vx = finite_val(rb.value("velocity_x", 0.f));
            float vy = finite_val(rb.value("velocity_y", 0.f));
            float av = finite_val(rb.value("angular_velocity", 0.f));
            float drag_scale = fraction * fv.lin_drag;
            add_force(rb, -vx * drag_scale * mass, -vy * drag_scale * mass);
            add_torque(rb, -av * fraction * fv.ang_drag * mass);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  20d.  POINT EFFECTOR 2D
//  Attract / repel radial force field.
//  Component "PointEffector2D": force_magnitude, distance_scale, mode ("attract"|"repel"|"both")
//  forceMode: "force" (default) or "impulse"
//  Call once per frame BEFORE apply_physics.
// ════════════════════════════════════════════════════════════════════════════
void apply_point_effectors(EntityList& entities) {
    struct PointField {
        float x, y;
        float force_magnitude;
        float distance_scale;   // effective radius
        std::string mode;       // "attract","repel"
        int   layer_mask;
    };
    std::vector<PointField> fields;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"PointEffector2D") || !has_component(e,"Transform")) continue;
        auto& pe  = e["components"]["PointEffector2D"];
        auto& tr  = e["components"]["Transform"];
        auto wt   = transform::cached_world(e);
        PointField pf;
        pf.x = finite_val(wt.x);
        pf.y = finite_val(wt.y);
        pf.force_magnitude = finite_val(pe.value("force_magnitude", 100.f));
        pf.distance_scale  = std::max(1.f, finite_val(pe.value("distance_scale", 100.f)));
        pf.mode            = pe.value("mode", std::string("repel"));
        pf.layer_mask      = pe.value("collider_mask", 0xFFFF);
        fields.push_back(pf);
    }
    if (fields.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (is_static(&rb)) continue;
        auto wt = transform::cached_world(e);
        float ex = finite_val(wt.x), ey = finite_val(wt.y);

        for (auto& pf : fields) {
            float dx = ex - pf.x, dy = ey - pf.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < 1e-6f) continue;
            float d = std::sqrt(d2);
            float falloff = std::max(0.f, 1.f - d / pf.distance_scale);
            float mag = pf.force_magnitude * falloff * falloff;  // quadratic falloff
            float nx2 = dx/d, ny2 = dy/d;
            if (pf.mode == "attract") { nx2 = -nx2; ny2 = -ny2; }
            add_force(rb, nx2*mag, ny2*mag);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  20e.  AREA EFFECTOR 2D (Unity2D parity)
//  Applies radial or directional forces to bodies within its collider area.
//  Component "AreaEffector2D": force_magnitude, force_angle, drag, use_global_angle,
//  mode ("radial"|"directional"), layer_mask
//  Call once per frame BEFORE apply_physics.
// ════════════════════════════════════════════════════════════════════════════
void apply_area_effectors(EntityList& entities) {
    struct AreaField {
        Entity* entity;
        float force_magnitude;
        float force_angle;      // degrees
        float drag;
        bool  use_global_angle;
        std::string mode;       // "radial"|"directional"
        int   layer_mask;
        std::vector<Shape> shapes;
    };
    std::vector<AreaField> fields;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"AreaEffector2D") || !has_component(e,"Transform")) continue;
        auto& ae = e["components"]["AreaEffector2D"];
        AreaField af;
        af.entity = &e;
        af.force_magnitude = finite_val(ae.value("force_magnitude", 50.f));
        af.force_angle = finite_val(ae.value("force_angle", 0.f));
        af.drag = finite_val(ae.value("drag", 0.f));
        af.use_global_angle = ae.value("use_global_angle", false);
        af.mode = ae.value("mode", std::string("directional"));
        af.layer_mask = ae.value("collider_mask", 0xFFFF);
        af.shapes = collect_shapes(e);
        if (!af.shapes.empty()) fields.push_back(af);
    }
    if (fields.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (is_static(&rb)) continue;
        auto wt = transform::cached_world(e);
        float ex = finite_val(wt.x), ey = finite_val(wt.y);

        for (auto& af : fields) {
            // Check if body is inside area effector's collider
            bool inside = false;
            for (auto& sh : af.shapes) {
                if (sh.kind == ShapeKind::Circle) {
                    float dx = ex - sh.cx, dy = ey - sh.cy;
                    if (dx*dx + dy*dy <= sh.radius*sh.radius) { inside = true; break; }
                } else if (sh.kind == ShapeKind::Polygon) {
                    if (point_in_poly(ex, ey, sh.verts)) { inside = true; break; }
                }
            }
            if (!inside) continue;

            // Apply force based on mode
            float fx = 0, fy = 0;
            if (af.mode == "radial") {
                // Radial force from area center
                for (auto& sh : af.shapes) {
                    if (sh.kind == ShapeKind::Circle) {
                        float dx = ex - sh.cx, dy = ey - sh.cy;
                        float d2 = dx*dx + dy*dy;
                        if (d2 < 1e-6f) continue;
                        float d = std::sqrt(d2);
                        float nx = dx/d, ny = dy/d;
                        fx += nx * af.force_magnitude;
                        fy += ny * af.force_magnitude;
                    }
                }
            } else {
                // Directional force
                float angle = af.force_angle;
                if (af.use_global_angle) {
                    // Use global rotation
                    angle += finite_val(get_float(e["components"]["Transform"], "rotation"));
                }
                float rad = angle * (float)M_PI / 180.f;
                fx = std::cos(rad) * af.force_magnitude;
                fy = std::sin(rad) * af.force_magnitude;
            }

            add_force(rb, fx, fy);

            // Apply drag
            if (af.drag > 0) {
                float vx = rb.value("velocity_x", 0.f);
                float vy = rb.value("velocity_y", 0.f);
                float mass = std::max(finite_val(rb.value("mass", 1.f), 1.f), 1e-9f);
                add_force(rb, -vx * af.drag * mass, -vy * af.drag * mass);
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  20e.  GROUND / WALL INFO QUERY
//  Inspect the last frame's manifolds to classify ground/wall contacts.
//  Returns grounded / wall_left / wall_right / slope_angle for a given entity.
// ════════════════════════════════════════════════════════════════════════════
GroundInfo query_ground_info(Entity& e, const std::vector<void*>& manifold_opaque) {
    GroundInfo info;
    if (!has_component(e,"Rigidbody2D")) return info;
    auto& rb = e["components"]["Rigidbody2D"];
    info.grounded        = rb.value("_grounded", false);
    info.on_wall         = rb.value("_on_wall",  false);
    info.wall_left       = rb.value("_wall_left", false);
    info.wall_right      = rb.value("_wall_right",false);
    info.ground_normal_x = finite_val(rb.value("_ground_nx", 0.f));
    info.ground_normal_y = finite_val(rb.value("_ground_ny", 1.f));
    info.wall_normal_x   = finite_val(rb.value("_wall_nx",   0.f));
    info.wall_normal_y   = finite_val(rb.value("_wall_ny",   0.f));
    info.coyote_timer    = finite_val(rb.value("_coyote_t",  0.f));
    info.was_grounded    = rb.value("_was_grounded", false);
    float gnx = info.ground_normal_x, gny = info.ground_normal_y;
    info.slope_angle = std::acos(std::clamp(std::abs(gny), 0.f, 1.f)) * 180.f / (float)M_PI;
    return info;
}

// Internal: write ground/wall state onto each Rigidbody2D after solving
static void collect_ground_info(EntityList& entities, const std::vector<Manifold>& mfds, float dt) {
    // Classify each contact, then handle coyote time + slope sliding
    static constexpr float GROUND_DOT = 0.65f;
    static constexpr float WALL_DOT   = 0.78f;

    // Snapshot previous grounded state for coyote time
    std::unordered_map<int, bool> prev_grounded;
    for (auto& e : entities) {
        if (!has_component(e,"Rigidbody2D")) continue;
        int eid = e.value("id", 0);
        prev_grounded[eid] = e["components"]["Rigidbody2D"].value("_grounded", false);
    }

    // Reset per-frame ground info.
    // Sleeping bodies are excluded from broadphase and generate no contacts this
    // frame, so preserve their grounded/wall flags — a body sleeping on the floor
    // is still on the floor. Without this guard, scripts see _grounded=false the
    // frame the player first inputs a jump (body was sleeping, flags were wiped),
    // causing the jump to fire as an air-jump and consuming the double-jump charge.
    for (auto& e : entities) {
        if (!has_component(e,"Rigidbody2D")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) == "dynamic" && rb.value("_sleeping", false)) continue;
        rb["_grounded"]  = false;
        rb["_on_wall"]   = false;
        rb["_wall_left"] = false;
        rb["_wall_right"]= false;
        rb["_ground_nx"] = 0.f;
        rb["_ground_ny"] = 1.f;
        rb["_wall_nx"]   = 0.f;
        rb["_wall_ny"]   = 0.f;
    }

    // Classify contacts
    for (auto& m : mfds) {
        if (m.is_trigger || m.one_way_skip || m.contact_count == 0) continue;
        float absnx = std::abs(m.nx);
        float absny = std::abs(m.ny);
        // Prefer the dominant axis so a diagonal corner contact does not
        // simultaneously count as both floor and wall. That corner case is
        // what causes false wall-press jitter and makes jumps feel unreliable.
        bool is_ground = absny >= GROUND_DOT && absny >= absnx;
        bool is_wall   = absnx >= WALL_DOT   && absnx >  absny;

        auto tag = [&](Entity* ent, Entity* rb_ent, float sign_nx) {
            if (!ent || !rb_ent) return;
            if (!has_component(*ent,"Rigidbody2D")) return;
            auto& rb = (*ent)["components"]["Rigidbody2D"];
            if (is_ground && m.ny * sign_nx < 0.f) {
                rb["_grounded"] = true;
                rb["_ground_nx"] = m.nx * sign_nx;
                rb["_ground_ny"] = m.ny * sign_nx;
            }
            if (is_wall) {
                rb["_on_wall"] = true;
                float wall_nx_local = m.nx * sign_nx;
                rb["_wall_nx"] = wall_nx_local;
                rb["_wall_ny"] = m.ny * sign_nx;
                // wall_nx_local is this body's push-out (away-from-wall) direction.
                // Negative push-out (push left) means the wall is on the body's
                // RIGHT side; positive push-out (push right) means the wall is
                // on the LEFT. (Previously swapped — see fix comment above.)
                if (wall_nx_local < 0) rb["_wall_right"] = true;
                else                   rb["_wall_left"]  = true;
            }
        };
        tag(m.e1, m.rb1, -1.f);
        tag(m.e2, m.rb2, +1.f);
    }

    // Coyote time + slope sliding
    for (auto& e : entities) {
        if (!has_component(e,"Rigidbody2D")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;

        int eid = e.value("id", 0);
        bool grounded_now = rb.value("_grounded", false);
        bool on_wall_now  = rb.value("_on_wall",  false);
        bool was_ground   = prev_grounded.count(eid) ? prev_grounded[eid] : false;

        // Wake any sleeping body that has an active contact (ground or wall).
        // Without this, a body resting on a floor or pressing a wall can go to sleep
        // with _sleeping=true, skip broadphase, and then never receive new impulses
        // (e.g. jump) until something else wakes it. We wake it here so update_sleep_islands
        // later in the same frame gets a fair re-evaluation.
        if ((grounded_now || on_wall_now) && rb.value("_sleeping", false)) {
            rb["_sleeping"] = false;
        }

        // Coyote timer: count up while airborne, reset when grounded
        float coyote_t = finite_val(rb.value("_coyote_t", 0.f));
        if (grounded_now) {
            coyote_t = 0.f;
            rb["_was_grounded"] = true;
        } else {
            coyote_t += dt;
            if (coyote_t > COYOTE_TIME) rb["_was_grounded"] = false;
        }
        rb["_coyote_t"] = coyote_t;

        // Slope sliding: if grounded on a steep slope (beyond slope_limit), push body downhill
        if (grounded_now) {
            float gnx = finite_val(rb.value("_ground_nx", 0.f));
            float gny = finite_val(rb.value("_ground_ny", 1.f));
            float slope_deg = std::acos(std::clamp(std::abs(gny), 0.f, 1.f)) * 180.f / (float)M_PI;
            float slope_limit = finite_val(rb.value("slope_limit", 60.f));
            if (slope_deg > slope_limit && std::abs(gnx) > 0.1f) {
                // Compute downhill direction (tangent pointing downward)
                float dn_x = gnx;   // tangent of slope = rotate normal 90°
                float dn_y = -std::abs(gny);
                float slide_force = finite_val(rb.value("slope_slide_force", 200.f));
                float mass = std::max(finite_val(rb.value("mass", 1.f), 1.f), 1e-9f);
                rb["_force_x"] = finite_val(rb.value("_force_x", 0.f)) + dn_x * slide_force * mass;
                rb["_force_y"] = finite_val(rb.value("_force_y", 0.f)) + dn_y * slide_force * mass;
            }
        }
    }
}


static const char* FEATURES[]={
    "soft_body","cloth","rope","chain","wheel","vehicle","character","destructible",
    "liquid","foam","jelly","granular","magnet","wind","surface","friction","gravity",
    "orbital","sensor","breakable","hinge","prismatic","gear","pulley","portal"
};
static constexpr int N_FEATURES=25;

PhysicsPackRegistry& PhysicsPackRegistry::instance(){static PhysicsPackRegistry inst;return inst;}

PhysicsPackRegistry::PhysicsPackRegistry(){
    _registry.reserve(600);
    for (int i=0;i<600;++i){
        const char* feature=FEATURES[i%N_FEATURES]; int variant=i%24;
        std::string name="PhysicsPack_"+std::to_string(1000+i).substr(1);
        _registry.push_back({name,feature,variant,[feature,variant,name](){
            auto p=std::make_shared<PhysicsPackBase>();
            p->name=name;p->feature=feature;p->variant=variant;return p;
        }});
        _by_feature[feature].push_back(i);
    }
}
std::shared_ptr<PhysicsPackBase> PhysicsPackRegistry::make(const std::string& feature,int index){
    auto it=_by_feature.find(feature);
    if(it==_by_feature.end()||it->second.empty()) return nullptr;
    return _registry[it->second[std::max(0,std::min(index,(int)it->second.size()-1))]].factory();
}
std::shared_ptr<PhysicsPackBase> PhysicsPackRegistry::make_by_number(int n){
    if(n<0||n>=(int)_registry.size()) return nullptr;
    return _registry[n].factory();
}
std::vector<std::string> PhysicsPackRegistry::features()const{
    std::vector<std::string> out;
    for(auto& [k,_]:_by_feature) out.push_back(k);
    std::sort(out.begin(),out.end());
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
//  21.  CHARACTER CONTROLLER (Unity2D CharacterController2D parity)
// ════════════════════════════════════════════════════════════════════════════

// CollisionFlags bitmask matching Unity:
//   None=0, Sides=1, Above=2, Below=4
static const int CC_FLAG_NONE  = 0;
static const int CC_FLAG_SIDES = 1;
static const int CC_FLAG_ABOVE = 2;
static const int CC_FLAG_BELOW = 4;

int move_character(Entity& entity, float dx, float dy, const CharacterController& cc) {
    if (!has_component(entity,"Transform")||!has_component(entity,"Rigidbody2D")) return CC_FLAG_NONE;
    auto& trans=entity["components"]["Transform"];
    auto& rb=entity["components"]["Rigidbody2D"];

    float min_dist=cc.min_move_distance;
    if (std::hypot(dx,dy)<min_dist&&std::hypot(dx,dy)<1e-7f) return CC_FLAG_NONE;

    float skin=cc.skin_width;
    float slope_limit_rad=cc.slope_limit*(float)M_PI/180.f;
    float step=cc.step_offset;
    int mask=cc.layer_mask;
    int flags=CC_FLAG_NONE;
    bool is_on_steep_slope=false;

    float orig_x=get_float(trans,"x");
    float orig_y=get_float(trans,"y");

    static EntityList fallback_world;
    EntityList* world = s_active_entities ? s_active_entities : &fallback_world;

    auto check_overlap = [&](float cx, float cy)->bool{
        auto shapes=collect_shapes(entity);
        for (auto& sh:shapes){
            auto [sx1,sy1,sx2,sy2]=shape_aabb(sh);
            float bx=cx+(sx1-orig_x), by=cy+(sy1-orig_y);
            float bw=std::max(sx2-sx1, 1e-3f);
            float bh=std::max(sy2-sy1, 1e-3f);
            // Shrink the probe box by SLOP on each side so that resting contacts
            // (where the physics solver intentionally leaves ≤SLOP px of penetration)
            // are not reported as overlapping. Without this, a player resting on a
            // floor or wall is "inside" it by up to SLOP px, check_overlap returns
            // true at the *current* position, and move_character immediately flags
            // CC_SIDES/CC_BELOW before any movement — causing permanent stuck state.
            float probe = std::max(1.5f, skin);  // shrink by at least 1.5px (< SLOP=2px)
            float qx = bx + probe;
            float qy = by + probe;
            float qw = std::max(bw - probe * 2.f, 1e-3f);
            float qh = std::max(bh - probe * 2.f, 1e-3f);
            auto hits = overlap_box(*world, qx, qy, qw, qh, 0, mask);
            for (auto* hit : hits) if (hit != &entity) return true;
        }
        return false;
    };

    auto binary_escape = [&](float from_x, float from_y, float to_x, float to_y) -> bool {
        if (check_overlap(from_x, from_y)) return false;
        if (!check_overlap(to_x, to_y)) {
            trans["x"] = to_x;
            trans["y"] = to_y;
            return true;
        }

        float lo = 0.f;
        float hi = 1.f;
        for (int i = 0; i < 20; ++i) {
            float mid = (lo + hi) * 0.5f;
            float mx = from_x + (to_x - from_x) * mid;
            float my = from_y + (to_y - from_y) * mid;
            if (check_overlap(mx, my)) hi = mid;
            else lo = mid;
        }

        float safe_t = std::max(0.f, lo - 1e-4f);
        trans["x"] = from_x + (to_x - from_x) * safe_t;
        trans["y"] = from_y + (to_y - from_y) * safe_t;
        return !check_overlap(get_float(trans, "x"), get_float(trans, "y"));
    };

    if (cc.detect_collisions){

        // ── Horizontal movement ──────────────────────────────────────────
        float remaining_x=dx;
        int max_iter=8;
        for (int i=0;i<max_iter&&std::abs(remaining_x)>1e-6f;++i){
            float step_x=std::copysign(std::min(std::abs(remaining_x),skin*2.f),remaining_x);
            float nx=get_float(trans,"x")+step_x;
            float ny=get_float(trans,"y");
            if (check_overlap(nx,ny)){
                bool stepped=false;
                if (step>1e-6f){
                    for(float s_try=step;s_try>skin;s_try-=skin*2){
                        if(!check_overlap(nx,ny-s_try)){
                            trans["y"]=ny-s_try; trans["x"]=nx;
                            remaining_x-=step_x; stepped=true; break;
                        }
                    }
                }
                if(!stepped){
                    rb["velocity_x"]=0.f;
                    remaining_x=0;
                    flags|=CC_FLAG_SIDES;
                }
            } else {
                trans["x"]=nx;
                remaining_x-=step_x;
            }
        }

        // ── Vertical movement ────────────────────────────────────────────
        float remaining_y=dy;
        for (int i=0;i<max_iter&&std::abs(remaining_y)>1e-6f;++i){
            float step_y=std::copysign(std::min(std::abs(remaining_y),skin*2.f),remaining_y);
            float nx=get_float(trans,"x");
            float ny=get_float(trans,"y")+step_y;
            if (check_overlap(nx,ny)){
                bool moving_down=step_y>0;
                if(moving_down) flags|=CC_FLAG_BELOW;
                else            flags|=CC_FLAG_ABOVE;
                rb["velocity_y"]=0.f;
                remaining_y=0;
            } else {
                trans["y"]=ny;
                remaining_y-=step_y;
            }
        }

        // ── Slope sliding ────────────────────────────────────────────────
        if (flags&CC_FLAG_BELOW){
            float cx=get_float(trans,"x"), cy=get_float(trans,"y");
            auto ground_hits=raycast(*world, cx,cy, 0.f,1.f, step+skin*4, mask);
            if(ground_hits.has_value()){
                auto& gh=ground_hits.value();
                float gnx=gh.normal.first, gny=gh.normal.second;
                float slope_ang=std::acos(clamp(std::abs(gny),0.f,1.f));
                if(slope_ang>slope_limit_rad){
                    is_on_steep_slope=true;
                    float sx=-gny, sy=gnx;
                    float slide=dot(dx,dy,sx,sy);
                    float sdx=sx*slide, sdy=sy*slide;
                    rb["velocity_x"]=(float)rb["velocity_x"]+sdx;
                    rb["velocity_y"]=(float)rb["velocity_y"]+sdy;
                }
            }
        }
    } else {
        trans["x"]=orig_x+dx;
        trans["y"]=orig_y+dy;
    }

    // Smooth depenetration: binary-search back along the travelled path first,
    // then probe a small set of escape directions only if necessary.
    if (cc.detect_collisions) {
        float cur_x = get_float(trans, "x");
        float cur_y = get_float(trans, "y");

        if (check_overlap(cur_x, cur_y)) {
            float path_x = cur_x - orig_x;
            float path_y = cur_y - orig_y;
            float path_len = std::hypot(path_x, path_y);

            bool resolved = false;

            if (path_len > 1e-6f && !check_overlap(orig_x, orig_y)) {
                resolved = binary_escape(orig_x, orig_y, cur_x, cur_y);

                if (!resolved) {
                    float ux = path_x / path_len;
                    float uy = path_y / path_len;
                    float retreat = std::max(0.02f, skin * 0.15f);
                    for (int i = 0; i < 12 && check_overlap(get_float(trans, "x"), get_float(trans, "y")); ++i) {
                        trans["x"] = get_float(trans, "x") - ux * retreat;
                        trans["y"] = get_float(trans, "y") - uy * retreat;
                        retreat *= 1.1f;
                    }
                    resolved = !check_overlap(get_float(trans, "x"), get_float(trans, "y"));
                }
            }

            if (!resolved) {
                // Spawn-inside or near-vertex fallback: try a smooth outward search.
                const std::pair<float,float> dirs[] = {
                    { 1.f, 0.f }, { -1.f, 0.f }, { 0.f, 1.f }, { 0.f, -1.f },
                    { 0.70710678f, 0.70710678f }, { -0.70710678f, 0.70710678f },
                    { 0.70710678f, -0.70710678f }, { -0.70710678f, -0.70710678f }
                };

                float best_x = cur_x;
                float best_y = cur_y;
                float best_dist = 1e30f;
                bool found = false;

                for (auto [dxo, dyo] : dirs) {
                    float step_len = std::max(0.05f, skin * 0.5f);
                    float far_x = cur_x + dxo * step_len;
                    float far_y = cur_y + dyo * step_len;

                    int expand = 0;
                    while (check_overlap(far_x, far_y) && step_len < skin * 24.f && expand < 8) {
                        step_len *= 2.f;
                        far_x = cur_x + dxo * step_len;
                        far_y = cur_y + dyo * step_len;
                        ++expand;
                    }

                    if (check_overlap(far_x, far_y)) continue;

                    float lo = 0.f, hi = step_len;
                    for (int i = 0; i < 18; ++i) {
                        float mid = (lo + hi) * 0.5f;
                        float mx = cur_x + dxo * mid;
                        float my = cur_y + dyo * mid;
                        if (check_overlap(mx, my)) lo = mid;
                        else hi = mid;
                    }

                    float candidate_x = cur_x + dxo * hi;
                    float candidate_y = cur_y + dyo * hi;
                    float dist = std::hypot(candidate_x - cur_x, candidate_y - cur_y);
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_x = candidate_x;
                        best_y = candidate_y;
                        found = true;
                    }
                }

                if (found) {
                    trans["x"] = best_x;
                    trans["y"] = best_y;
                }
            }
        }
    }

    // Update velocity without feeding the tiny correction back into the next frame.
    // Blocked axes are cleared; free axes keep the requested motion.
    if (!is_on_steep_slope) {
        // Only update velocity components that weren't blocked by geometry.
        // Do NOT zero a blocked axis if the player is simply resting against a surface:
        // the physics solver already handles that. Zeroing here would kill jump velocity
        // on the same frame a wall/floor contact is detected.
        if (!(flags & CC_FLAG_SIDES)) rb["velocity_x"] = dx;
        // Note: velocity_x is NOT zeroed on CC_FLAG_SIDES — the physics normal impulse
        // already stops penetration. Zeroing here caused the "stuck on wall" bug where
        // any frame with a wall contact would permanently kill horizontal velocity.

        if (!(flags & CC_FLAG_ABOVE) && !(flags & CC_FLAG_BELOW)) rb["velocity_y"] = dy;
        // Same for vertical: don't zero on contact — the solver handles it.
    }

    transform::mark_local_dirty(entity.value("id",0));
    return flags;
}
bool is_grounded(Entity& entity) {
    if (!has_component(entity,"Rigidbody2D")) return false;
    auto& rb=entity["components"]["Rigidbody2D"];
    return rb.value("_grounded",false);
}

Vec2 get_velocity(Entity& entity) {
    if (!has_component(entity,"Rigidbody2D")) return {0,0};
    auto& rb=entity["components"]["Rigidbody2D"];
    return {rb.value("velocity_x",0.f), rb.value("velocity_y",0.f)};
}

// ════════════════════════════════════════════════════════════════════════════
//  22.  BODY INTERPOLATION
// ════════════════════════════════════════════════════════════════════════════
void enable_interpolation(Entity& entity, bool enable) {
    if (!has_component(entity,"Rigidbody2D")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    rb["_interpolation_enabled"]=enable;
}

Vec2 get_interpolated_position(Entity& entity, float alpha) {
    if (!has_component(entity,"Transform")) return {0,0};
    int eid=entity.value("id",0);
    auto it=s_prev_state.find(eid);
    if (it==s_prev_state.end()) return {get_float(entity["components"]["Transform"],"x"), get_float(entity["components"]["Transform"],"y")};

    auto& [prev_pos, prev_rot]=it->second;
    float curr_x=get_float(entity["components"]["Transform"],"x");
    float curr_y=get_float(entity["components"]["Transform"],"y");

    return {prev_pos.first + (curr_x - prev_pos.first) * alpha,
            prev_pos.second + (curr_y - prev_pos.second) * alpha};
}

float get_interpolated_rotation(Entity& entity, float alpha) {
    if (!has_component(entity,"Transform")) return 0.f;
    int eid=entity.value("id",0);
    auto it=s_prev_state.find(eid);
    if (it==s_prev_state.end()) return get_float(entity["components"]["Transform"],"rotation");

    auto& [prev_pos, prev_rot]=it->second;
    float curr_rot=get_float(entity["components"]["Transform"],"rotation");

    // Handle angle wrapping for smooth interpolation
    float diff=curr_rot - prev_rot;
    while (diff > 180.f) diff -= 360.f;
    while (diff < -180.f) diff += 360.f;

    return prev_rot + diff * alpha;
}

// ════════════════════════════════════════════════════════════════════════════
//  22.  DIRECT BODY POSITION/ROTATION SETTERS (Unity rb.position / rb.rotation)
// ════════════════════════════════════════════════════════════════════════════

void set_body_position(Entity& entity, float x, float y) {
    if (!has_component(entity,"Transform")) return;
    auto& trans=entity["components"]["Transform"];
    trans["x"]=x; trans["y"]=y;
    // Clear any velocity inference that would have applied from previous position
    // (matches Unity rb.position = v which does NOT infer velocity)
    if (has_component(entity,"Rigidbody2D")){
        // Update interpolation prev state so there's no visual jump artifact
        int eid=entity.value("id",0);
        s_prev_state[eid].first={x,y};
    }
    transform::mark_local_dirty(entity.value("id",0));
}

void set_body_rotation(Entity& entity, float angle_deg) {
    if (!has_component(entity,"Transform")) return;
    auto& trans=entity["components"]["Transform"];
    trans["rotation"]=angle_deg;
    if (has_component(entity,"Rigidbody2D")){
        int eid=entity.value("id",0);
        s_prev_state[eid].second=angle_deg;
    }
    transform::mark_local_dirty(entity.value("id",0));
}

// ════════════════════════════════════════════════════════════════════════════
//  23.  IS_TOUCHING / IS_TOUCHING_LAYERS (Unity Rigidbody2D.IsTouching parity)
// ════════════════════════════════════════════════════════════════════════════

bool is_touching(Entity& entity, Entity& other) {
    int id1=entity.value("id",0), id2=other.value("id",0);
    int lo=std::min(id1,id2), hi=std::max(id1,id2);
    // Check both trigger and non-trigger contact states
    return _contact_state.count(ContactEventKey{lo, hi, false}) > 0 ||
           _contact_state.count(ContactEventKey{lo, hi, true}) > 0;
}

bool is_touching_layers(Entity& entity, int layer_mask) {
    int eid=entity.value("id",0);
    for (auto& [key,active]:_contact_state){
        if (!active) continue;
        auto it=_contact_ids.find(key);
        if (it==_contact_ids.end()) continue;
        auto [lo,hi]=it->second;
        int other_id=(lo==eid)?hi:(hi==eid?lo:-1);
        if (other_id<0) continue;
        // Find entity in active set
        if (s_active_entities){
            for (auto& e2:*s_active_entities){
                if (e2.value("id",0)==other_id){
                    if (has_component(e2,"Rigidbody2D")){
                        int layer=e2["components"]["Rigidbody2D"].value("layer",0);
                        if (layer_mask&(1<<layer)) return true;
                    }
                    break;
                }
            }
        }
    }
    return false;
}

} // namespace phys
