#pragma once

// Component metadata shared by the Inspector, creation menus, command palette,
// and visual-scripting pickers.  Scene JSON keeps its existing component keys;
// this registry only adds presentation, validation and creation policy.

#include "component_defs.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

enum class ComponentSupport {
    Production,
    Experimental,
    DataOnly
};

struct ComponentDescriptor {
    std::string id;
    std::string category;
    std::string description;
    std::string tool_hint;
    std::vector<std::string> required_components;
    ComponentSupport support = ComponentSupport::Production;
};

inline const char* component_support_label(ComponentSupport support) {
    switch (support) {
        case ComponentSupport::Production:   return "Ready";
        case ComponentSupport::Experimental: return "Experimental";
        case ComponentSupport::DataOnly:     return "Data only";
    }
    return "Unknown";
}

inline const std::vector<ComponentDescriptor>& component_catalog() {
    static const std::vector<ComponentDescriptor> catalog = {
        {"Transform", "Core", "Position, rotation, scale and hierarchy.", "Viewport", {}, ComponentSupport::Production},
        {"Script", "Core", "Native C++ behaviour attached to this entity.", "Scripts / Console", {}, ComponentSupport::Production},
        {"VisualScript", "Core", "A node graph asset that runs without compiling C++.", "Visual Scripting", {}, ComponentSupport::Production},
        {"EventEmitter", "Core", "Emits named gameplay events.", "Visual Scripting", {}, ComponentSupport::Production},
        {"ScriptableObjectRef", "Core", "Loads isolated project .sobj data for native and visual scripts.", "Scriptable Objects", {}, ComponentSupport::Production},

        {"SpriteRenderer", "Rendering", "Renders a 2D sprite.", "Sprite Editor", {}, ComponentSupport::Production},
        {"LineRenderer2D", "Rendering", "Renders an editable 2D line.", "Line Renderer", {}, ComponentSupport::Production},
        {"TrailRenderer2D", "Rendering", "Draws a motion trail behind the entity.", "Trail Renderer", {}, ComponentSupport::Production},
        {"TextMeshPro2D", "Rendering", "World-space 2D text.", "Text", {}, ComponentSupport::Production},
        {"Light2D", "Rendering", "2D lighting contribution.", "Lighting", {}, ComponentSupport::Production},
        {"Shadow2DCaster", "Rendering", "Projects a 2D occlusion silhouette from nearby Light2D sources.", "Shadow 2D Settings", {}, ComponentSupport::Production},
        {"SpriteMask", "Rendering", "Masks sprites inside or outside a region.", "Sprite Editor", {}, ComponentSupport::Production},
        {"SortingGroup", "Rendering", "Keeps layered sprites sorted together.", "Sorting Layers", {}, ComponentSupport::Production},
        {"ParallaxBackground", "Rendering", "Moves background art at a parallax rate.", "Viewport", {}, ComponentSupport::Production},
        {"CustomRenderTexture2D", "Rendering", "Generates a persistent runtime texture for its SpriteRenderer.", "Custom Render Texture", {"SpriteRenderer"}, ComponentSupport::Production},
        {"VideoPlayer2D", "Rendering", "Plays an animated GIF on its SpriteRenderer in Play and export.", "Video Player", {"SpriteRenderer"}, ComponentSupport::Production},

        {"Rigidbody2D", "Physics 2D", "Participates in 2D physics simulation.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"BoxCollider2D", "Physics 2D", "Axis-aligned rectangular collision shape.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"CircleCollider2D", "Physics 2D", "Circular collision shape.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"CapsuleCollider2D", "Physics 2D", "Capsule collision shape.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"PolygonCollider2D", "Physics 2D", "Custom polygon collision shape.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"EdgeCollider2D", "Physics 2D", "Open edge collision shape.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"CompositeCollider2D", "Physics 2D", "Combines collider geometry into one physics shape.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},
        {"DistanceJoint2D", "Physics 2D", "Keeps two rigidbodies at a distance.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},
        {"SpringJoint2D", "Physics 2D", "Connects rigidbodies with a spring.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},
        {"HingeJoint2D", "Physics 2D", "Connects rigidbodies around a pivot.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},
        {"SliderJoint2D", "Physics 2D", "Constrains a rigidbody to a line.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},
        {"WheelJoint2D", "Physics 2D", "Suspension and motor joint for a wheel.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},
        {"MouseJoint2D", "Physics 2D", "Drags a rigidbody toward a target.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},
        {"PlatformEffector2D", "Physics 2D", "Adds one-way platform behaviour.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"PointEffector2D", "Physics 2D", "Applies radial force to physics bodies.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"BuoyancyEffector2D", "Physics 2D", "Applies buoyancy to physics bodies.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"SurfaceEffector2D", "Physics 2D", "Applies surface movement force.", "Physics Debugger 2D", {}, ComponentSupport::Production},
        {"ConstantForce2D", "Physics 2D", "Applies continuous force to a rigidbody.", "Physics Debugger 2D", {"Rigidbody2D"}, ComponentSupport::Production},

        {"Camera2D", "Camera", "The scene camera.", "Viewport", {}, ComponentSupport::Production},
        {"Cinemachine2D", "Camera", "Virtual follow camera with composition controls.", "Virtual Camera", {}, ComponentSupport::Production},
        {"VirtualCamera", "Camera", "Priority virtual camera compatible with Cinemachine2D scenes.", "Virtual Camera", {}, ComponentSupport::Production},

        {"Animator", "Animation", "Sprite animation controller.", "Animator", {}, ComponentSupport::Production},
        {"AnimatorOverrideController", "Animation", "Overrides named Animator clips without mutating the base controller.", "Animator", {"Animator"}, ComponentSupport::Production},
        {"ParticleEmitter", "Animation", "GPU/CPU particle emitter.", "Particle Settings", {}, ComponentSupport::Production},
        {"LimbIK2D", "Animation", "Solves a weighted two-bone 2D limb toward a target.", "Animator", {}, ComponentSupport::Production},

        {"Tilemap", "2D World", "Grid-based tile layer.", "Tile Palette", {}, ComponentSupport::Production},
        {"Grid2D", "2D World", "Grid transform used by tilemaps.", "Tile Palette", {}, ComponentSupport::Production},
        {"NavMesh2D", "Navigation", "2D navigation graph source.", "NavMesh 2D", {}, ComponentSupport::Production},
        {"NavMeshAgent2D", "Navigation", "Moves an entity along a navigation path.", "NavMesh 2D", {}, ComponentSupport::Production},
        {"NavMeshObstacle2D", "Navigation", "Obstacle used by the navigation graph.", "NavMesh 2D", {}, ComponentSupport::Production},
        {"Waypoint2D", "Navigation", "Navigation graph marker used by NavMesh 2D baking and path gizmos.", "NavMesh 2D", {}, ComponentSupport::Production},
        {"PathFollower2D", "Navigation", "Moves an entity along an authored point path.", "Path Follower", {}, ComponentSupport::Production},

        {"AudioSource", "Audio", "Plays a sound or music clip.", "Audio Mixer", {}, ComponentSupport::Production},

        {"UICanvas", "UI", "Root for screen-space UI.", "UI Builder", {}, ComponentSupport::Production},
        {"UIPanel", "UI", "Rectangular UI container.", "UI Builder", {}, ComponentSupport::Production},
        {"UIText", "UI", "Screen-space UI text.", "UI Builder", {}, ComponentSupport::Production},
        {"UIButton", "UI", "Interactive UI button.", "UI Builder", {}, ComponentSupport::Production},
        {"UIImage", "UI", "Screen-space UI image.", "UI Builder", {}, ComponentSupport::Production},
        {"UIProgressBar", "UI", "Screen-space progress indicator.", "UI Builder", {}, ComponentSupport::Production},
        {"UILayoutGroup", "UI", "Automatic horizontal or vertical layout.", "UI Builder", {}, ComponentSupport::Production},

        {"Spawner2D", "Gameplay", "Spawns and reuses configured prefab instances at runtime.", "Visual Scripting", {}, ComponentSupport::Production},
        {"HealthComponent", "Gameplay", "Tracks health, i-frames, damage/death events and optional destruction.", "Visual Scripting", {}, ComponentSupport::Production},
        {"ObjectPool", "Gameplay", "Reusable object-pool settings.", "Visual Scripting", {}, ComponentSupport::Production},
        {"SceneTransition", "Gameplay", "Fades and safely loads a target scene.", "Timeline", {}, ComponentSupport::Production},
        {"Flock2D", "Gameplay", "Flocking-agent steering settings.", "Visual Scripting", {"Rigidbody2D"}, ComponentSupport::Production},
        {"LODGroup2D", "Gameplay", "2D level-of-detail sprite selection.", "Profiler", {"SpriteRenderer"}, ComponentSupport::Production},
    };
    return catalog;
}

inline const ComponentDescriptor* component_descriptor(const std::string& id) {
    const auto& catalog = component_catalog();
    auto it = std::find_if(catalog.begin(), catalog.end(), [&](const ComponentDescriptor& descriptor) {
        return descriptor.id == id;
    });
    return it == catalog.end() ? nullptr : &*it;
}

inline std::string component_category(const std::string& id) {
    if (const auto* descriptor = component_descriptor(id)) return descriptor->category;
    return "Other";
}

inline std::string component_description(const std::string& id) {
    if (const auto* descriptor = component_descriptor(id)) return descriptor->description;
    return "Custom or legacy component data preserved from the scene.";
}

// The normal Add Component surface is a promise that the component has a
// live runtime path.  Keep unfinished/authoring-only entries available behind
// an explicit opt-in for migration and internal testing, but never present
// them as ready-to-ship controls by default.
inline bool component_is_ready_for_creation(const std::string& id,
                                            bool include_experimental = false) {
    const ComponentDescriptor* descriptor = component_descriptor(id);
    if (!descriptor) return true; // Existing/legacy scene data must stay editable.
    return descriptor->support == ComponentSupport::Production ||
           (include_experimental && descriptor->support == ComponentSupport::Experimental);
}

inline std::vector<std::string> component_names_for_picker(bool include_experimental = false) {
    std::vector<std::string> names = all_component_names();
    names.push_back("VisualScript");
    for (const auto& descriptor : component_catalog()) {
        if (std::find(names.begin(), names.end(), descriptor.id) == names.end()) names.push_back(descriptor.id);
    }
    names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string& id) {
        return !component_is_ready_for_creation(id, include_experimental);
    }), names.end());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const std::string ac = component_category(a);
        const std::string bc = component_category(b);
        return ac == bc ? component_display_name(a) < component_display_name(b) : ac < bc;
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

template <typename ComponentCollection>
inline std::vector<std::string> add_component_with_requirements(ComponentCollection& components,
                                                                 const std::string& id) {
    std::vector<std::string> added;
    const auto add_one = [&](const std::string& component_id, auto&& add_one_ref) -> void {
        if (components.contains(component_id)) return;
        if (const auto* descriptor = component_descriptor(component_id)) {
            for (const std::string& required : descriptor->required_components) add_one_ref(required, add_one_ref);
        }
        if (component_defaults().contains(component_id)) components[component_id] = component_defaults()[component_id];
        else components[component_id] = nlohmann::json::object();
        added.push_back(component_id);
    };
    add_one(id, add_one);
    return added;
}
