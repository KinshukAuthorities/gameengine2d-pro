#pragma once
#include "entity.hpp"
#include "transform_system.hpp"
#include <cmath>
#include <algorithm>

struct Camera {
    float x = 0.f, y = 0.f;
    float zoom = 1.f;
    // ── Rotation (task 11) ────────────────────────────────────────────────────
    // Angle in radians, clockwise positive (screen +Y is down, same convention
    // as the rest of the engine). A positive angle rotates the world CW as
    // seen by the viewer — i.e. the camera itself is tilted CCW.
    // 0 = no rotation (default, preserves all existing behaviour).
    float angle = 0.f;

    float smooth_speed = 5.f;
    float offset_x = 0.f, offset_y = 0.f;
    float min_zoom = 0.05f, max_zoom = 20.f;
    int   width, height;

    // Internal: id of the entity currently being followed
    int _active_camera_id = -1;

    Camera(int w, int h) : width(w), height(h) {}

    // ── World → screen ────────────────────────────────────────────────────────
    // Applies camera position, zoom and rotation.
    // Convention: rotate the world-space offset around the camera centre
    // (clockwise for positive angle), then scale and re-centre on screen.
    //
    // Without rotation (angle == 0) this is identical to the original formula:
    //   sx = (wx - x) * zoom + width  * 0.5
    //   sy = (wy - y) * zoom + height * 0.5
    std::pair<float,float> world_to_screen(float wx, float wy) const {
        // Offset from camera position in world space
        float dx = (wx - x) * zoom;
        float dy = (wy - y) * zoom;
        // Rotate by -angle (we rotate the world relative to the camera,
        // so a CW camera tilt shows the world CCW — negate the angle)
        float ca = std::cos(-angle), sa = std::sin(-angle);
        float rx = dx * ca - dy * sa;
        float ry = dx * sa + dy * ca;
        return { rx + width  * 0.5f,
                 ry + height * 0.5f };
    }

    // ── Screen → world ────────────────────────────────────────────────────────
    // Exact inverse of world_to_screen.
    std::pair<float,float> screen_to_world(float sx, float sy) const {
        // Un-centre
        float rx = sx - width  * 0.5f;
        float ry = sy - height * 0.5f;
        // Inverse rotation: +angle (undo the -angle applied in world_to_screen)
        float ca = std::cos(angle), sa = std::sin(angle);
        float dx = rx * ca - ry * sa;
        float dy = rx * sa + ry * ca;
        // Un-scale and un-translate
        return { dx / zoom + x,
                 dy / zoom + y };
    }

    void set_zoom(float z) {
        zoom = std::max(min_zoom, std::min(max_zoom, z));
    }

    // ── Visible world bounds ──────────────────────────────────────────────────
    // Returns the AABB in world space that encloses the camera's visible
    // rectangle.  When angle != 0 the visible region is a rotated rectangle;
    // the returned AABB is the axis-aligned box that contains it, which is
    // slightly larger than strictly necessary but always conservative (no
    // false-negative culls).
    //
    // Used by RenderSystem::draw() for frustum culling.
    struct WorldBounds { float min_x, min_y, max_x, max_y; };
    WorldBounds visible_world_bounds(float margin = 0.f) const {
        // Four screen corners → world space
        auto [wx0, wy0] = screen_to_world(0.f,           0.f);
        auto [wx1, wy1] = screen_to_world((float)width,  0.f);
        auto [wx2, wy2] = screen_to_world((float)width,  (float)height);
        auto [wx3, wy3] = screen_to_world(0.f,           (float)height);
        return {
            std::min({wx0, wx1, wx2, wx3}) - margin,
            std::min({wy0, wy1, wy2, wy3}) - margin,
            std::max({wx0, wx1, wx2, wx3}) + margin,
            std::max({wy0, wy1, wy2, wy3}) + margin
        };
    }

    // ── Zoom to point ─────────────────────────────────────────────────────────
    // Zoom in/out while keeping the world point under (screen_x, screen_y)
    // stationary. Works correctly with non-zero angle.
    void zoom_to_point(float screen_x, float screen_y, float delta) {
        auto [wx, wy] = screen_to_world(screen_x, screen_y);
        float factor = std::exp(delta);
        set_zoom(zoom * factor);
        // Re-anchor: find the camera position that maps (wx,wy) back to
        // (screen_x, screen_y) after the zoom change.
        // world_to_screen gives:  rx + w/2 = screen_x, ry + h/2 = screen_y
        // where rx,ry are the rotated-and-scaled offset.
        float dx = screen_x - width  * 0.5f;
        float dy = screen_y - height * 0.5f;
        // Rotate back by +angle to get un-rotated scaled offset
        float ca = std::cos(angle), sa = std::sin(angle);
        float rdx = dx * ca - dy * sa;
        float rdy = dx * sa + dy * ca;
        x = wx - rdx / zoom;
        y = wy - rdy / zoom;
    }

    // ── Smooth follow (mirrors CameraSystem.update) ───────────────────────────
    void update(EntityList& entities, float dt) {
        Entity* active_cam = nullptr;
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (has_component(e, "Camera2D")) {
                active_cam = &e;
                if (e.value("tags", Entity::array()).contains("MainCamera"))
                    break;
            }
        }
        if (!active_cam) return;

        auto& comp  = (*active_cam)["components"]["Camera2D"];

        // Sync component settings
        if (comp.contains("zoom"))         set_zoom(comp["zoom"].get<float>());
        // Older scenes use the direct zoom field.  Orthographic size is an
        // opt-in authoring mode so those scenes keep their exact framing, while
        // a designer changing "Ortho Size" in the Inspector gets a real camera
        // result rather than an inert serialized number.  Five is the engine's
        // historic default orthographic size at 1x; projects may calibrate the
        // reference explicitly if they use a different world scale.
        if (comp.value("projection_size_mode", std::string("zoom")) == "orthographic") {
            const float ortho = std::max(0.001f, comp.value("orthographic_size", 5.f));
            const float reference = std::max(0.001f, comp.value("orthographic_reference_size", 5.f));
            set_zoom(reference / ortho);
        }
        if (comp.contains("smooth_speed")) smooth_speed = comp["smooth_speed"].get<float>();
        if (comp.contains("offset_x"))     offset_x = comp["offset_x"].get<float>();
        if (comp.contains("offset_y"))     offset_y = comp["offset_y"].get<float>();
        // ── Rotation (task 11) ────────────────────────────────────────────────
        // angle stored in the component as degrees (friendlier for designers);
        // converted to radians here.  Defaults to 0 so existing scenes that
        // don't set angle in their Camera2D JSON are unaffected.
        if (comp.contains("angle"))
            angle = comp["angle"].get<float>() * (3.14159265f / 180.f);

        // A Cinemachine2D/VirtualCamera brain supplies an explicit camera
        // pose.  Do this before the regular follow-target branch so a virtual
        // camera cannot be overwritten by the Camera2D's own legacy follow
        // settings on the next frame.
        if (comp.value("_virtual_camera_active", false)) {
            x = comp.value("_virtual_camera_x", x);
            y = comp.value("_virtual_camera_y", y);
            _active_camera_id = (*active_cam).value("id", -1);
            return;
        }

        int cam_id = (*active_cam).value("id", -1);
        if (_active_camera_id != cam_id) {
            _active_camera_id = cam_id;
            x = transform::world_x(*active_cam);
            y = transform::world_y(*active_cam);
        }

        // Follow target
        std::string target_name = comp.value("follow_target", "");
        Entity* target = nullptr;
        if (!target_name.empty()) {
            for (auto& e : entities) {
                if (e.value("name", "") == target_name) { target = &e; break; }
            }
        }

        if (target && has_component(*target, "Transform")) {
            float tx = transform::world_x(*target) + offset_x;
            float ty = transform::world_y(*target) + offset_y;
            float t  = std::min(1.f, smooth_speed * dt);
            x += (tx - x) * t;
            y += (ty - y) * t;
        } else {
            x = transform::world_x(*active_cam);
            y = transform::world_y(*active_cam);
        }
    }
};

// ── Multi-viewport support (task 11) ─────────────────────────────────────────
// Describes one viewport sub-region of the render target together with its
// own camera.  Pass a list of these to RenderSystem::draw_viewports() to
// render split-screen, minimap, or picture-in-picture effects without
// needing separate RenderSystem instances.
//
// Uses plain int fields so camera.hpp does not need to include Vulkan headers.
// RenderSystem converts these to VkRect2D / VkViewport internally.
struct CameraViewportRect {
    int x = 0, y = 0;
    int width = 0, height = 0;
};

struct ViewportDesc {
    CameraViewportRect rect;     // sub-region of the render target (pixels, top-left origin)
    Camera*            camera = nullptr; // camera to use for this region
};
