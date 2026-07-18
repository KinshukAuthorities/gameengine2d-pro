// C++ script — save this file; it's registered automatically the
// one-file hot reload finishes. The editor stays locked while it compiles.
#include "../../../engine_cpp/script_system.hpp"

class NewScript9 : public ScriptBase {
public:
    void awake() override {}
    void start() override {}
    void update(float dt) override {}

    void on_collision_enter(EntityRef other) override {}
    void on_trigger_enter(EntityRef other) override {}
};
