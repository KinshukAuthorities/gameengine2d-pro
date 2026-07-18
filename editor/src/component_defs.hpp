#pragma once
/*
 * component_defs.hpp — Default values for every component type.
 * Mirrors engine/components.py :: COMPONENT_DEFAULTS
 */

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

inline nlohmann::json make_component_defaults() {
    using J = nlohmann::json;
    J d;

    d["Transform"]        = {{"x",0},{"y",0},{"rotation",0},{"scale_x",1},{"scale_y",1},{"parent",-1}};
    // ── SpriteRenderer ──────────────────────────────────────────────────────
    // draw_mode mirrors Unity2D's Sprite Renderer "Draw Mode":
    //   "simple" — texture is stretched to fill the Transform-scaled box (old/default behavior).
    //   "tiled"  — texture keeps its own pixel size (x pixels_per_unit) and repeats to fill
    //              a target world-space box (tile_width x tile_height). This is the "grass
    //              block keeps its size and copies itself to fill the platform" behavior.
    //   "sliced" — 9-slice: the four border_* insets (in source pixels) stay unscaled while
    //              the middle stretches/tiles to fill tile_width x tile_height. Used for
    //              panels, platforms, dialog boxes — anything resizable without warping.
    d["SpriteRenderer"]   = {{"texture",""},{"layer",0},{"sorting_layer",""},{"order_in_layer",0},
                              {"flip_x",false},{"flip_y",false},
                              {"opacity",1.0},{"color",[]{J c=J::array();c.push_back(255);c.push_back(255);c.push_back(255);c.push_back(255);return c;}()},
                              {"src_x",0},{"src_y",0},{"src_w",0},{"src_h",0},
                              {"draw_mode","simple"},
                              {"tile_width",128},{"tile_height",128},
                              {"pixels_per_unit",100.0},
                              {"border_left",0},{"border_right",0},{"border_top",0},{"border_bottom",0},
                              {"sliced_fill_center",true},{"sliced_tile_edges",false},
                              {"pivot_x",0.5},{"pivot_y",0.5},
                              {"filter_mode","point"},
                              {"mask_interaction","none"},
                              {"material",""},
                              {"gpu_instancing",false}};
    d["Rigidbody2D"]      = {{"mass",1.0},{"gravity_scale",1.0},{"drag",0.05},{"angular_drag",0.05},
                              {"is_kinematic",false},{"body_type","dynamic"},{"freeze_rotation",false},
                              {"velocity_x",0},{"velocity_y",0},{"angular_velocity",0},{"layer",0},{"layer_mask",65535}};
    d["BoxCollider2D"]    = {{"width",32},{"height",32},{"offset_x",0},{"offset_y",0},
                              {"is_trigger",false},{"bounciness",0.0},{"friction",0.3}};
    d["CircleCollider2D"] = {{"radius",16},{"offset_x",0},{"offset_y",0},
                              {"is_trigger",false},{"bounciness",0.0},{"friction",0.3}};
    d["CapsuleCollider2D"]= {{"radius",12},{"height",32},{"offset_x",0},{"offset_y",0},
                              {"direction","vertical"},{"is_trigger",false}};
    d["PolygonCollider2D"]= {{"points",J::array()},{"offset_x",0},{"offset_y",0},{"is_trigger",false}};
    d["EdgeCollider2D"]   = {{"points",J::array()},{"offset_x",0},{"offset_y",0},{"thickness",2},{"is_trigger",false}};
    d["Camera2D"]         = {{"follow_target",""},{"offset_x",0},{"offset_y",0},
                              {"orthographic_size",5.0},{"orthographic_reference_size",5.0},
                              {"projection_size_mode","zoom"},{"zoom",1.0},{"smooth",0.0},{"angle",0.0}};
    d["Animator"]         = {{"current_animation",""},{"playing",true},{"loop",true},
                              {"speed",1.0},{"animations",J::object()},{"default_fps",12}};
    // size_start/size_end/color_start/color_end remain the simple 2-point
    // curve (kept for back-compat with old scenes and as a quick-edit
    // shorthand). When size_curve/color_curve below contain 2+ keyframes,
    // they take precedence — see ParticleSystem::_eval_curve in systems.hpp.
    // Curves are stored as flat arrays of keyframes: [t0,v0, t1,v1, ...] for
    // size_curve (t,size pairs) and [t0,r0,g0,b0,a0, t1,r1,g1,b1,a1, ...] for
    // color_curve. Empty array = "use the 2-point start/end fields instead".
    d["ParticleEmitter"]  = {{"rate",10.0},{"lifetime",2.0},{"speed",80.0},{"spread",360.0},
                              {"gravity_scale",0.3},{"size_start",4.0},{"size_end",0.0},
                              {"burst",false},{"burst_count",20},{"looping",true},
                              {"color_start",J::array({255,180,60,255})},
                              {"color_end",J::array({255,60,60,0})},
                              {"size_curve",J::array()},
                              {"color_curve",J::array()},
                              // Sub-emitter: fired per-particle on spawn and/or death. Spawns
                              // a small burst of simple particles at the trigger particle's
                              // position — not a full prefab/entity, just a cheap visual kick
                              // (e.g. sparks on death, trail puffs on spawn).
                              {"sub_emitter_on_death",false},
                              {"sub_emitter_on_spawn",false},
                              {"sub_emitter_count",6.0},
                              {"sub_emitter_speed",60.0},
                              {"sub_emitter_lifetime",0.4},
                              {"sub_emitter_size",3.0},
                              {"sub_emitter_color",J::array({255,255,255,255})},
                              // Atlas: optional spritesheet for particles instead of a flat
                              // circle. atlas_cols/rows define a uniform grid; each particle
                              // is randomly (or sequentially) assigned a frame on spawn.
                              {"atlas_texture",""},
                              {"atlas_cols",1.0},
                              {"atlas_rows",1.0},
                              {"atlas_random_frame",true}};
    d["AudioSource"]      = {{"clip",""},{"volume",1.0},{"pitch",1.0},{"loop",false},
                              {"play_on_awake",false},{"spatial",false}};
    d["Tilemap"]          = {{"tile_size",32},{"tileset",""},{"tile_palette",""},{"tile_collision",J::object()},{"grid",J::array()},
                              {"origin_x",0},{"origin_y",0},
                              {"generate_colliders",false},{"filter_mode","point"},
                              {"sorting_layer",""},{"order_in_layer",0}};
    d["Light2D"]          = {{"radius",200},{"intensity",1.0},{"color",J::array({255,220,150,200})},
                              {"enabled",true},{"cast_shadows",false}};
    d["DistanceJoint2D"]  = {{"connected_entity",-1},{"max_length",100},{"min_length",0}};
    d["SpringJoint2D"]    = {{"connected_entity",-1},{"rest_length",80},{"stiffness",10.0},{"damping",1.0}};
    d["MouseJoint2D"]     = {{"target_x",0},{"target_y",0},{"frequency",8.0},{"damping_ratio",0.7},{"max_force",1000}};
    d["PlatformEffector2D"]={{"use_one_way",true},{"surface_arc",180}};
    d["ParallaxBackground"]={{"depth",-1000.0},{"opacity",1.0},
                              {"speed_x",0.15},{"speed_y",0.0},
                              {"texture",""},{"color",J::array({255,255,255,255})},
                              {"tiling_x",true},{"tiling_y",true},{"scale",1.0},
                              {"filter_mode","point"}};
    d["UICanvas"]         = {{"render_mode","screen_space"}};
    d["UIPanel"]          = {{"anchor_x",0.5},{"anchor_y",0.5},{"pivot_x",0.5},{"pivot_y",0.5},
                              {"pos_x",0},{"pos_y",0},{"width",200},{"height",40},
                              {"color",J::array({30,30,40,200})},
                              {"texture",""},{"draw_mode","simple"},
                              {"border_left",0},{"border_right",0},{"border_top",0},{"border_bottom",0}};
    d["UIText"]           = {{"text","Text"},{"font_size",18},{"anchor_x",0.5},{"anchor_y",0.5},
                              {"pivot_x",0.5},{"pivot_y",0.5},{"pos_x",0},{"pos_y",0},
                              {"color",J::array({255,255,255,255})}};
    d["UIButton"]         = {{"label","Button"},{"anchor_x",0.5},{"anchor_y",0.5},
                              {"pivot_x",0.5},{"pivot_y",0.5},{"pos_x",0},{"pos_y",0},
                              {"width",120},{"height",40},{"on_click",""}};
    d["UIImage"]          = {{"texture",""},{"anchor_x",0.5},{"anchor_y",0.5},
                              {"pivot_x",0.5},{"pivot_y",0.5},{"pos_x",0},{"pos_y",0},
                              {"width",64},{"height",64},
                              {"draw_mode","simple"},
                              {"border_left",0},{"border_right",0},{"border_top",0},{"border_bottom",0},
                              {"filter_mode","point"}};
    d["UIProgressBar"]    = {{"anchor_x",0.5},{"anchor_y",1.0},{"pivot_x",0.5},{"pivot_y",0.5},
                              {"pos_x",0},{"pos_y",-10},{"width",200},{"height",16},
                              {"value",0.5},{"min",0},{"max",1},
                              {"fill_color",J::array({80,200,80,255})}};
    // ── UILayoutGroup ──────────────────────────────────────────────────────────
    // Mirrors Unity's Horizontal/Vertical/Grid Layout Group: arranges every
    // entity parented under this one (Transform.parent) into a row, column,
    // or grid each frame (see UILayoutSystem in feature_systems.hpp) instead
    // of each child needing hand-placed anchor/pos values. type is
    // "horizontal"|"vertical"|"grid"; columns/cell_width/cell_height only
    // apply to "grid". child_alignment is "start"|"center"|"end" along the
    // cross axis (e.g. vertical alignment within a horizontal group).
    d["UILayoutGroup"]    = {{"type","vertical"},
                              {"anchor_x",0.5},{"anchor_y",0.5},{"pivot_x",0.5},{"pivot_y",0.5},
                              {"pos_x",0},{"pos_y",0},{"width",300},{"height",300},
                              {"spacing",4},
                              {"padding_left",8},{"padding_right",8},{"padding_top",8},{"padding_bottom",8},
                              {"child_alignment","start"},
                              {"columns",4},{"cell_width",64},{"cell_height",64}};
    d["Script"]           = {{"class_name",""},{"scripts",J::array()},{"field_overrides",J::object()}};
    // A graph asset can be attached to an entity without compiling C++.
    // Legacy scene-wide event graphs remain supported by the runtime too.
    d["VisualScript"]     = {{"asset",""},{"enabled",true},{"run_on_start",true}};
    d["EventEmitter"]     = {{"event_name",""},{"enabled",true},{"emit_on_start",true},
                              {"emit_every",0.0},{"once",false},{"payload",J::object()}};
    // ── SpriteMask ───────────────────────────────────────────────────────────
    // Unity2D-style sprite mask: sprites with mask_interaction != "none" are
    // clipped against the alpha of the nearest enabled SpriteMask below them
    // in sort order. front_sorting_layer/back_sorting_layer + order bounds
    // mirror Unity's mask range fields (which sorting layers the mask affects).
    d["SpriteMask"]       = {{"texture",""},{"src_x",0},{"src_y",0},{"src_w",0},{"src_h",0},
                              {"alpha_cutoff",0.5},{"front_sorting_layer",""},{"back_sorting_layer",""}};
    // ── SortingGroup ─────────────────────────────────────────────────────────
    // Mirrors Unity2D's Sorting Group: all child renderers sort together as one
    // unit using this entity's sorting_layer/order_in_layer instead of sorting
    // independently — handy for multi-sprite characters made of separate limbs.
    d["SortingGroup"]     = {{"sorting_layer",""},{"order_in_layer",0}};

    // ── LineRenderer2D ────────────────────────────────────────────────────────
    // Draws a polyline through world-space points — ropes, trajectories, lasers.
    // Points stored as flat [x0,y0, x1,y1, ...] array. width_start/end taper.
    d["LineRenderer2D"]   = {{"points",J::array()},
                              {"width_start",4.0},{"width_end",4.0},
                              {"color_start",J::array({255,255,255,255})},
                              {"color_end",J::array({255,255,255,0})},
                              {"texture",""},{"loop",false},
                              {"sorting_layer",""},{"order_in_layer",0},
                              {"use_world_space",true}};

    // ── ConstantForce2D ───────────────────────────────────────────────────────
    // Applies a constant force/torque to the sibling Rigidbody2D each frame.
    // Good for wind zones, conveyor belt forces, persistent gravity overrides.
    d["ConstantForce2D"]  = {{"force_x",0.0},{"force_y",0.0},
                              {"relative_force_x",0.0},{"relative_force_y",0.0},
                              {"torque",0.0}};

    // ── PointEffector2D ───────────────────────────────────────────────────────
    // Attract/repel bodies that overlap the collider (magnets, explosions).
    // forceMagnitude < 0 = attract. Mirrors Unity's PointEffector2D.
    d["PointEffector2D"]  = {{"force_magnitude",10.0},{"force_variation",0.0},
                              {"distance_scale",1.0},{"drag",0.0},
                              {"angular_drag",0.0},{"force_source","collider"},
                              {"force_target","rigidbody"},{"force_mode","constant"}};

    // ── BuoyancyEffector2D ────────────────────────────────────────────────────
    // Simulates fluid: bodies above surface_level experience upward force.
    // Attach to a trigger collider to define the fluid zone.
    d["BuoyancyEffector2D"] = {{"surface_level",0.0},{"density",1.0},
                                {"linear_drag",1.0},{"angular_drag",1.0},
                                {"flow_angle",0.0},{"flow_magnitude",0.0},
                                {"flow_variation",0.0}};

    // ── SurfaceEffector2D ─────────────────────────────────────────────────────
    // Pushes bodies along the collider surface — conveyor belts, moving platforms.
    d["SurfaceEffector2D"] = {{"speed",5.0},{"speed_variation",0.0},
                               {"force_scale",0.1},{"use_contact_force",true},
                               {"use_friction",true},{"use_bounce",false}};

    // ── NavMeshAgent2D ────────────────────────────────────────────────────────
    // Pathfinding component: navigates to destination_x/y via a baked NavMesh.
    // avoidance_radius enables local RVO-style obstacle avoidance between agents.
    d["NavMeshAgent2D"]   = {{"speed",150.0},{"angular_speed",360.0},
                              {"acceleration",500.0},{"stopping_distance",8.0},
                              {"auto_braking",true},{"radius",16.0},
                              {"avoidance_radius",20.0},{"avoidance_priority",50},
                              {"destination_x",0.0},{"destination_y",0.0},
                              {"auto_repath",true},{"path_layer_mask",65535}};

    // ── NavMeshObstacle2D ─────────────────────────────────────────────────────
    // Dynamic obstacle that carves holes in the NavMesh so agents route around
    // moving objects (crates, doors). carve_only_stationary limits mesh cutting
    // to when the body is nearly still — cheaper than per-frame carving.
    d["NavMeshObstacle2D"] = {{"width",32.0},{"height",32.0},
                               {"offset_x",0.0},{"offset_y",0.0},
                               {"carve",true},{"carve_only_stationary",true},
                               {"carve_threshold",0.1}};

    // ── Waypoint2D ────────────────────────────────────────────────────────────
    // Ordered path marker used by patrol scripts and NavMesh baking.
    // `path` holds ordered entity IDs; the NavPathSystem stitches them at runtime.
    d["Waypoint2D"]       = {{"path",J::array()},{"loop",true},{"reverse",false},
                              {"gizmo_color",J::array({0,255,100,200})}};

    // ── Cinemachine2D (Virtual Camera) ────────────────────────────────────────
    // Procedural camera: follows a target with dead zone / soft zone damping,
    // lookahead, screen composition, and optional world-bounds confinement.
    // The highest-priority enabled Cinemachine2D drives the active Camera2D.
    d["Cinemachine2D"]    = {{"follow_target",-1},{"look_at_target",-1},
                              {"priority",10},
                              {"dead_zone_w",0.2},{"dead_zone_h",0.15},
                              {"soft_zone_w",0.6},{"soft_zone_h",0.6},
                              {"screen_x",0.5},{"screen_y",0.5},
                              {"damping_x",0.3},{"damping_y",0.3},
                              {"lookahead_time",0.0},{"lookahead_smoothing",10.0},
                              {"confine",false},
                              {"confine_min_x",-1000.0},{"confine_max_x",1000.0},
                              {"confine_min_y",-1000.0},{"confine_max_y",1000.0},
                              {"orthographic_size",5.0},{"zoom",1.0}};

    // ── TextMeshPro2D ─────────────────────────────────────────────────────────
    // SDF world-space text: rich text markup, outline, per-char animation hooks.
    d["TextMeshPro2D"]    = {{"text","Text"},{"font",""},{"font_size",24.0},
                              {"color",J::array({255,255,255,255})},
                              {"outline_color",J::array({0,0,0,255})},{"outline_width",0.0},
                              {"face_dilate",0.0},
                              {"alignment","center"},{"wrapping",true},
                              {"overflow","overflow"},{"bounds_w",200.0},{"bounds_h",0.0},
                              {"sorting_layer",""},{"order_in_layer",1},
                              {"rich_text",true},{"auto_size",false},
                              {"min_size",8.0},{"max_size",72.0}};

    // ── CompositeCollider2D ───────────────────────────────────────────────────
    // Merges sibling polygon/tilemap colliders into a single optimized shape,
    // eliminating internal-seam friction artifacts on tilemaps.
    d["CompositeCollider2D"] = {{"geometry_type","outlines"},
                                 {"generation_type","synchronous"},
                                 {"vertex_distance",0.5},{"offset_distance",0.001},
                                 {"is_trigger",false},{"bounciness",0.0},{"friction",0.3}};

    // ── HingeJoint2D ─────────────────────────────────────────────────────────
    // Pins two bodies at an anchor with optional motor and angle limits.
    d["HingeJoint2D"]     = {{"connected_entity",-1},
                              {"anchor_x",0.0},{"anchor_y",0.0},
                              {"connected_anchor_x",0.0},{"connected_anchor_y",0.0},
                              {"use_motor",false},{"motor_speed",0.0},{"max_torque",10000.0},
                              {"use_limits",false},{"lower_angle",-90.0},{"upper_angle",90.0},
                              {"enable_collision",false}};

    // ── SliderJoint2D ─────────────────────────────────────────────────────────
    // Constrains motion to one axis (rail, piston). Motor drives the slide.
    d["SliderJoint2D"]    = {{"connected_entity",-1},{"angle",0.0},
                              {"use_motor",false},{"motor_speed",0.0},{"max_force",10000.0},
                              {"use_limits",false},
                              {"lower_translation",-100.0},{"upper_translation",100.0},
                              {"enable_collision",false}};

    // ── WheelJoint2D ──────────────────────────────────────────────────────────
    // Spring-damper suspension + optional motor — 2D vehicle wheels.
    d["WheelJoint2D"]     = {{"connected_entity",-1},
                              {"anchor_x",0.0},{"anchor_y",-20.0},
                              {"suspension_angle",90.0},{"suspension_frequency",2.0},
                              {"suspension_damping_ratio",0.7},
                              {"use_motor",false},{"motor_speed",0.0},{"max_torque",10000.0}};

    // ── VideoPlayer2D ─────────────────────────────────────────────────────────
    // Cross-platform animated-GIF playback onto the owning SpriteRenderer.
    // Keeping this decoder in the runtime avoids a platform codec dependency
    // in exported projects while still providing a real timeline/playback API.
    d["VideoPlayer2D"]    = {{"enabled",true},{"clip",""},{"play_on_awake",false},{"loop",false},
                              {"playing",false},{"restart",false},{"playback_time",0.0},
                              {"playback_speed",1.0}};

    // ── ScriptableObjectRef ───────────────────────────────────────────────────
    // Reference to a .sobj JSON asset; scripts can read/write fields at runtime.
    d["ScriptableObjectRef"] = {{"asset_path",""},{"type_name",""}};

    // ── CustomRenderTexture2D ──────────────────────────────────────────────────
    // Off-screen render target for water ripples, shadow maps, trail effects.
    d["CustomRenderTexture2D"] = {{"enabled",true},{"width",256},{"height",256},
                                   {"format","RGBA8"},{"depth_bits",0},
                                   {"filter_mode","bilinear"},{"wrap_mode","repeat"},
                                   {"update_mode","on_demand"},{"update_interval",0.0666667},
                                   {"generator","solid"},{"clear_color",J::array({255,255,255,255})},
                                   {"checker_size",16},{"seed",1},{"animation_speed",1.0},
                                   {"request_update",false},{"double_buffered",false}};

    // ── Grid2D ────────────────────────────────────────────────────────────────
    // Visual guide grid component (Unity-style Grid parent for Tilemap children).
    // Defines cell size and gap for grid-snapping tools and the tilemap system.
    d["Grid2D"]           = {{"cell_width",32},{"cell_height",32},
                              {"cell_gap_x",0},{"cell_gap_y",0},
                              {"cell_layout","rectangle"},{"cell_swizzle","XYZ"}};

    // ── SortingGroup ─────────────────────────────────────────────────────────
    // Mirrors Unity's SortingGroup: all SpriteRenderer children render as a unit
    // at this group's sorting position. Children's order_in_layer only sort within
    // the group. Used for layered character sprites (body + hat + weapon = 1 unit).
    d["SortingGroup"]     = {{"sorting_layer","Default"},{"order_in_layer",0},{"enabled",true}};

    // ── SpriteMask ────────────────────────────────────────────────────────────
    // Defines a rectangular region that masks SpriteRenderers inside/outside it.
    // mask_interaction on SpriteRenderer: "none"|"visible_inside"|"visible_outside"
    d["SpriteMask"]       = {{"enabled",true},{"width",64},{"height",64},
                              {"alpha_cutoff",0.5},{"sorting_layer_range_start",-32768},
                              {"sorting_layer_range_end",32767}};

    // ── AreaEffector2D ────────────────────────────────────────────────────────
    // Applies a directional force + lift to all Rigidbody2D within the collider.
    d["AreaEffector2D"]   = {{"enabled",true},{"force_angle",-90},{"force_magnitude",0},
                              {"lift_magnitude",0},{"drag",0},{"angular_drag",0},
                              {"force_variation",0},{"use_collider_mask",true}};

    // ── ConstantForce2D ───────────────────────────────────────────────────────
    // Continuously applies a fixed world-space or local-space force to Rigidbody2D.
    d["ConstantForce2D"]  = {{"enabled",true},{"force_x",0},{"force_y",0},
                              {"relative_force_x",0},{"relative_force_y",0},{"torque",0}};

    // ── PointEffector2D ───────────────────────────────────────────────────────
    // Attract/repel from a world point with distance falloff.
    d["PointEffector2D"]  = {{"enabled",true},{"force_magnitude",0},{"force_variation",0},
                              {"distance_scale",1},{"max_radius",200},
                              {"drag",0},{"angular_drag",0}};

    // ── PlatformEffector2D ────────────────────────────────────────────────────
    // One-way collision: body can pass through from below, blocked from above.
    d["PlatformEffector2D"] = {{"enabled",true},{"use_one_way",true},{"surface_arc",180},
                                {"use_one_way_grouping",true},{"use_side_friction",false},
                                {"use_side_bounce",false},{"side_arc",50}};

    // ── BuoyancyEffector2D ────────────────────────────────────────────────────
    // Applies upward buoyancy + drag for water physics.
    d["BuoyancyEffector2D"] = {{"enabled",true},{"surface_level",0},{"density",2},
                                {"linear_drag",3},{"angular_drag",1},
                                {"flow_angle",0},{"flow_magnitude",0},{"flow_variation",0}};

    // ── NavMesh2D ─────────────────────────────────────────────────────────────
    // Placed on the same entity as a Tilemap. Marks it as a nav graph source.
    d["NavMesh2D"]        = {{"obstacle_ids",nlohmann::json::array()},{"allow_diagonals",false},
                              {"agent_radius",0.5}};

    // ── NavAgent2D ────────────────────────────────────────────────────────────
    // Attached to any entity to give it A* pathfinding movement.
    d["NavAgent2D"]       = {{"speed",100},{"stopping_dist",4},{"auto_move",true},
                              {"path_target_x",0},{"path_target_y",0},
                              {"acceleration",100},{"angular_speed",120}};

    // ── VirtualCamera ─────────────────────────────────────────────────────────
    // Cinemachine-style virtual camera. Brain entity (Camera2D with is_brain=true)
    // blends the highest-priority active VirtualCamera's output.
    d["VirtualCamera"]    = {{"priority",10},{"enabled",true},
                              {"follow_target",-1},{"look_at_target",-1},
                              {"dead_zone_w",0},{"dead_zone_h",0},
                              {"soft_zone_w",0.8},{"soft_zone_h",0.8},
                              {"look_ahead_time",0},{"look_ahead_smoothing",0},
                              {"x_damp",2},{"y_damp",2},
                              {"ortho_size",5},{"ortho_damp",0},
                              {"confine",false},
                              {"confiner_x",0},{"confiner_y",0},
                              {"confiner_w",1000},{"confiner_h",1000},
                              {"aspect_ratio",1.777},
                              {"shake_magnitude_position",0.3},
                              {"shake_magnitude_rotation",2},{"shake_decay",1.5}};

    return d;
}

inline const nlohmann::json& component_defaults() {
    static nlohmann::json d = make_component_defaults();
    return d;
}

// Components that can't be removed
inline bool is_required_component(const std::string& name) {
    return name == "Transform";
}

// Keys to hide in inspector
inline bool should_hide_key(const std::string& k) {
    if (k.rfind("_",0)==0) return true;  // starts with _
    if (k == "parent") return true;       // Transform.parent: shown via dedicated picker, not raw int
    if (k == "field_overrides") return true; // Script: shown via dedicated per-script field UI
    if (k == "gpu_instancing") return true;  // SpriteRenderer: rendered manually at bottom with tooltip
    // ParticleEmitter: size_curve/color_curve are variable-length flat float
    // arrays the generic loop can't render usefully (it only special-cases
    // fixed 4-element color arrays). sub_emitter_*/atlas_* are grouped into
    // collapsible sections instead of a flat dump. All shown via the custom
    // ParticleEmitter block in panels.hpp.
    if (k == "size_curve" || k == "color_curve") return true;
    if (k.rfind("sub_emitter_",0)==0) return true;
    if (k.rfind("atlas_",0)==0) return true;
    // Camera2D: drawn via custom controls (entity picker, sliders)
    if (k == "follow_target" || k == "zoom" || k == "smooth" || k == "offset_x" || k == "offset_y") return true;
    // Rigidbody2D: freeze axes drawn as checkboxes; layer_mask as bit-grid; velocity shown live
    if (k == "constraints") return true;
    if (k == "layer_mask") return true;
    if (k == "velocity_x" || k == "velocity_y" || k == "angular_velocity") return true;
    // Colliders: is_trigger, bounciness, friction drawn as custom controls
    if (k == "is_trigger" || k == "bounciness" || k == "friction") return true;
    // SpriteRenderer: drawn via custom block above
    if (k == "texture" || k == "color" || k == "flip_x" || k == "flip_y") return true;
    if (k == "opacity" || k == "pivot_x" || k == "pivot_y") return true;
    if (k == "order_in_layer" || k == "gpu_instancing") return true;
    // Cinemachine2D: entity pickers + zone sliders drawn custom
    if (k == "follow_target" || k == "look_at_target" || k == "priority") return true;
    if (k == "dead_zone_w" || k == "dead_zone_h" || k == "soft_zone_w" || k == "soft_zone_h") return true;
    if (k == "confine" || k == "confine_min_x" || k == "confine_max_x" || k == "confine_min_y" || k == "confine_max_y") return true;
    // Joints: connected_entity drawn as entity picker
    if (k == "connected_entity") return true;
    // LineRenderer2D: points drawn as editable list
    if (k == "points") return true;
    // Waypoint2D: path drawn as ordered list
    if (k == "path") return true;
    // EventEmitter: payload drawn as key-value editor; other fields shown custom
    if (k == "event_name" || k == "emit_on_start" || k == "once" || k == "emit_every" || k == "payload") return true;
    // TextMeshPro2D: text, alignment, font_size, color, outline drawn custom
    if (k == "text" || k == "alignment" || k == "font_size") return true;
    if (k == "color" && false) return false; // color on TextMeshPro2D shown custom; but other components still need it
    // ConstantForce2D: force_x/y drawn via direction wheel; angle/mag derived
    if (k == "force_x" || k == "force_y") return true;
    // UI: anchor_x/y drawn via 3x3 grid picker
    if (k == "anchor_x" || k == "anchor_y") return true;
    // AudioSource: all fields drawn via custom block — suppress generic loop duplication
    if (k == "clip" || k == "volume" || k == "pitch" || k == "loop" ||
        k == "play_on_awake" || k == "spatial" || k == "min_distance" || k == "max_distance") return true;
    // Rigidbody2D: freeze_rotation drawn in custom Freeze checkboxes block; mass/drag/etc
    // drawn via custom sliders below, is_kinematic shown via body_type combo
    if (k == "freeze_rotation" || k == "is_kinematic" || k == "layer") return true;
    if (k == "mass" || k == "gravity_scale" || k == "drag" || k == "angular_drag") return true;
    // ParticleEmitter: all detail fields drawn via the custom ParticleEmitter blocks
    if (k == "rate" || k == "lifetime" || k == "speed" || k == "spread" || k == "gravity_scale") return true;
    if (k == "size_start" || k == "size_end" || k == "burst" || k == "burst_count" || k == "looping") return true;
    if (k == "color_start" || k == "color_end") return true;
    if (k == "direction_angle" || k == "speed_variation" || k == "lifetime_variation") return true;
    if (k == "rotation_start" || k == "rotation_variation") return true;
    if (k == "angular_velocity" || k == "angular_velocity_variation") return true;
    if (k == "max_particles" || k == "emitting") return true;
    // Animator: current_animation drawn as dropdown; speed/loop/playing drawn custom
    if (k == "current_animation" || k == "playing" || k == "loop" || k == "speed") return true;
    if (k == "speed_multiplier" || k == "ping_pong" || k == "default_fps") return true;
    // Camera2D: orthographic_size and angle drawn custom
    if (k == "orthographic_size" || k == "angle") return true;
    // Cinemachine2D: damping, lookahead, zoom, screen composition drawn custom
    if (k == "damping_x" || k == "damping_y") return true;
    if (k == "lookahead_time" || k == "lookahead_smoothing") return true;
    if (k == "zoom" || k == "screen_x" || k == "screen_y") return true;
    // Joints: stiffness/damping/lengths/motor/limits drawn per-joint-type
    if (k == "rest_length" || k == "stiffness" || k == "damping" || k == "damping_ratio") return true;
    if (k == "max_length"  || k == "min_length") return true;
    if (k == "use_motor"   || k == "motor_speed" || k == "max_torque" || k == "max_force") return true;
    if (k == "use_limits"  || k == "lower_angle" || k == "upper_angle") return true;
    // LineRenderer2D: width, color, loop, world_space drawn custom
    if (k == "width_start" || k == "width_end") return true;
    if (k == "color_start" || k == "color_end") return true;
    if (k == "use_world_space") return true;
    // NavMeshAgent2D: movement params drawn custom
    if (k == "speed" || k == "acceleration" || k == "stopping_distance") return true;
    if (k == "avoidance_radius" || k == "avoidance_priority" || k == "auto_repath") return true;
    if (k == "path_layer_mask" || k == "destination_x" || k == "destination_y") return true;
    // Waypoint2D: loop/reverse/gizmo_color drawn custom
    if (k == "reverse" || k == "gizmo_color") return true;
    // ConstantForce2D: relative_force drawn custom (force_x/y already hidden above)
    if (k == "relative_force_x" || k == "relative_force_y") return true;
    // TextMeshPro2D: wrapping, rich_text, auto_size, bounds, face_dilate drawn custom
    if (k == "wrapping" || k == "rich_text" || k == "auto_size") return true;
    if (k == "min_size" || k == "max_size" || k == "bounds_w" || k == "bounds_h") return true;
    if (k == "face_dilate" || k == "overflow") return true;
    // EventEmitter: enabled shown via header toggle
    if (k == "enabled") return true;
    return false;
}

// Human-readable names
inline std::string component_display_name(const std::string& name) {
    static std::unordered_map<std::string,std::string> names = {
        {"Transform","Transform"},
        {"SpriteRenderer","Sprite Renderer"},
        {"Rigidbody2D","Rigidbody 2D"},
        {"BoxCollider2D","Box Collider 2D"},
        {"CircleCollider2D","Circle Collider 2D"},
        {"CapsuleCollider2D","Capsule Collider 2D"},
        {"PolygonCollider2D","Polygon Collider 2D"},
        {"EdgeCollider2D","Edge Collider 2D"},
        {"Camera2D","Camera 2D"},
        {"Animator","Animator"},
        {"ParticleEmitter","Particle Emitter"},
        {"AudioSource","Audio Source"},
        {"Tilemap","Tilemap"},
        {"Light2D","Light 2D"},
        {"DistanceJoint2D","Distance Joint 2D"},
        {"SpringJoint2D","Spring Joint 2D"},
        {"MouseJoint2D","Mouse Joint 2D"},
        {"PlatformEffector2D","Platform Effector 2D"},
        {"ParallaxBackground","Parallax Background"},
        {"UICanvas","UI Canvas"},
        {"UIPanel","UI Panel"},
        {"UIText","UI Text"},
        {"UIButton","UI Button"},
        {"UIImage","UI Image"},
        {"UIProgressBar","UI Progress Bar"},
        {"UILayoutGroup","UI Layout Group"},
        {"Script","Script"},
        {"VisualScript","Visual Script"},
        {"EventEmitter","Event Emitter"},
        {"SpriteMask","Sprite Mask"},
        {"SortingGroup","Sorting Group"},
        {"LineRenderer2D","Line Renderer 2D"},
        {"ConstantForce2D","Constant Force 2D"},
        {"PointEffector2D","Point Effector 2D"},
        {"BuoyancyEffector2D","Buoyancy Effector 2D"},
        {"SurfaceEffector2D","Surface Effector 2D"},
        {"NavMeshAgent2D","Nav Mesh Agent 2D"},
        {"NavMeshObstacle2D","Nav Mesh Obstacle 2D"},
        {"Waypoint2D","Waypoint 2D"},
        {"Cinemachine2D","Virtual Camera (Cinemachine)"},
        {"TextMeshPro2D","Text Mesh Pro 2D"},
        {"CompositeCollider2D","Composite Collider 2D"},
        {"HingeJoint2D","Hinge Joint 2D"},
        {"SliderJoint2D","Slider Joint 2D"},
        {"WheelJoint2D","Wheel Joint 2D"},
        {"VideoPlayer2D","Video Player 2D"},
        {"ScriptableObjectRef","Scriptable Object Ref"},
        {"CustomRenderTexture2D","Custom Render Texture 2D"},
        {"Grid2D","Grid 2D"},
    };
    auto it = names.find(name);
    return it != names.end() ? it->second : name;
}

inline std::vector<std::string> all_component_names() {
    return {
        // ── Rendering ─────────────────────────────────────────────────────────
        "SpriteRenderer","LineRenderer2D","TextMeshPro2D","Light2D",
        "ParallaxBackground","SpriteMask","SortingGroup","VideoPlayer2D",
        "CustomRenderTexture2D",
        // ── Physics ───────────────────────────────────────────────────────────
        "Rigidbody2D","BoxCollider2D","CircleCollider2D","CapsuleCollider2D",
        "PolygonCollider2D","EdgeCollider2D","CompositeCollider2D",
        "DistanceJoint2D","SpringJoint2D","HingeJoint2D","SliderJoint2D",
        "WheelJoint2D","MouseJoint2D",
        "PlatformEffector2D","PointEffector2D","BuoyancyEffector2D","SurfaceEffector2D",
        "ConstantForce2D",
        // ── Camera ────────────────────────────────────────────────────────────
        "Camera2D","Cinemachine2D",
        // ── Animation ─────────────────────────────────────────────────────────
        "Animator","ParticleEmitter",
        // ── Tilemap ───────────────────────────────────────────────────────────
        "Tilemap","Grid2D",
        // ── Audio ─────────────────────────────────────────────────────────────
        "AudioSource",
        // ── Navigation ────────────────────────────────────────────────────────
        "NavMeshAgent2D","NavMeshObstacle2D","Waypoint2D",
        // ── Scripting / Data ──────────────────────────────────────────────────
        "Script","VisualScript","EventEmitter","ScriptableObjectRef",
        // ── UI ────────────────────────────────────────────────────────────────
        "UICanvas","UIPanel","UIText","UIButton",
        "UIImage","UIProgressBar","UILayoutGroup",
        // ── Nova Advanced Components ───────────────────────────────────────
        "TrailRenderer2D","Shadow2DCaster","AnimatorOverrideController",
        "LimbIK2D","Spawner2D","HealthComponent",
    };
}
// ─── Nova Advanced Components (patched in at startup) ─────────────────────────
// Call patch_nova_component_defaults(d) after make_component_defaults() to add:
// TrailRenderer2D, Shadow2DCaster, AnimatorOverrideController, LimbIK2D,
// Spawner2D, HealthComponent.
// This function is defined in unity_gap_features.hpp.
// To activate: in make_component_defaults(), after return d; add a patch call,
// OR call it from editor_main.cpp after component_defaults() is first accessed.

// Additional display names for nova components.
inline void patch_nova_display_names(std::unordered_map<std::string,std::string>& names) {
    names["TrailRenderer2D"]          = "Trail Renderer 2D";
    names["Shadow2DCaster"]            = "Shadow Caster 2D";
    names["AnimatorOverrideController"]= "Animator Override Controller";
    names["LimbIK2D"]                  = "Limb IK 2D";
    names["Spawner2D"]                 = "Spawner 2D";
    names["HealthComponent"]           = "Health Component";
}

inline std::vector<std::string> nova_component_names() {
    return {
        "TrailRenderer2D","Shadow2DCaster","AnimatorOverrideController",
        "LimbIK2D","Spawner2D","HealthComponent"
    };
}
