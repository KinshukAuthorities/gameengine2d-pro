// C++ script — save this file; it's registered automatically the
// next time you hit Play (or click "Rebuild Scripts" in the toolbar).
#include "../../../engine_cpp/script_system.hpp"

class NewScript : public ScriptBase {
public:
    void awake() override {}
    void start() override {}
    void update(float dt) override {}

    void on_collision_enter(EntityRef other) override {}
    void on_trigger_enter(EntityRef other) override {}
};
