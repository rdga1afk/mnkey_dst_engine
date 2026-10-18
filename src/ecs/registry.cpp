#include <monkey_dust/ecs/registry.h>

gaia::ecs::World& Registry::Get() {
    static gaia::ecs::World w;
    // Function-local statics initialize in declaration order, exactly
    // once, before Get() can return `w` to any caller -- avoids
    // constructing World inside a lambda-and-return-by-value (World
    // isn't necessarily move-constructible; not worth relying on).
    static bool s_schedInstalled = (MdGaiaSchedAdapter::Install(w), true);
    (void)s_schedInstalled;
    return w;
}
