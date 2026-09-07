#pragma once
#if defined(MD_ECS_GAIA)
#include <gaia.h>
#else
#include <flecs.h>
#endif

// Глобальний singleton flecs::world (task #8 B3.4 — was entt::registry
// through B1-B3.3). КРИТИЧНО: не передавати по значенню, не мутувати під
// час query ітерації. При потребі змінити entities під час ітерації —
// збирати в temp vector, застосовувати після завершення query.each().

class Registry {
public:
#if defined(MD_ECS_GAIA)
    static gaia::ecs::World& Get() {
        static gaia::ecs::World w;
        return w;
    }
#else
    static flecs::world& Get() {
        static flecs::world w;
        return w;
    }
#endif
    Registry() = delete;
};
