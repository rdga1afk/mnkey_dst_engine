#include <monkey_dust/ecs/hierarchy_utils.h>
#include <monkey_dust/components/hierarchy.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>

namespace Hierarchy {

static void RemoveFromChildrenList(MdRegistry& reg, MdEntity parent, MdEntity child) {
    if (!reg.Valid(parent) || !(reg.Handle(parent).has<ChildrenRef>())) return;
    auto& cr = reg.Handle(parent).get_mut<ChildrenRef>();
    for (int i = 0; i < cr.count; ++i) {
        if (cr.children[i] == child) {
            cr.children[i] = cr.children[cr.count - 1];
            --cr.count;
            return;
        }
    }
}

void ClearParent(MdEntity child) {
    auto& reg = MdRegistry::Get();
    if (!(reg.Handle(child).has<ParentRef>())) return;
    MdEntity parent = reg.Handle(child).get_mut<ParentRef>().parent;
    RemoveFromChildrenList(reg, parent, child);
    reg.Remove<ParentRef>(child);
}

bool SetParent(MdEntity child, MdEntity parent) {
    auto& reg = MdRegistry::Get();
    ClearParent(child);  // detach from any previous parent first

    if (!reg.Handle(parent).has<ChildrenRef>()) reg.Handle(parent).emplace<ChildrenRef>();
    auto& cr = reg.Handle(parent).get_mut<ChildrenRef>();
    if (cr.count >= HIERARCHY_MAX_CHILDREN) return false;
    cr.children[cr.count++] = child;
    reg.Handle(child).set<ParentRef>(ParentRef{parent});
    return true;
}

// Parent destroyed (or ChildrenRef removed) -> every child loses its
// ParentRef. Fires before the component data is actually erased, so
// reading cr here is valid (flecs OnRemove contract — verified empirically,
// same guarantee EnTT's on_destroy made; gaia's ObserverEvent::OnDel is the
// documented equivalent, see docs/GAIA_SEAM_AUDIT.md's Phase 0 probe P-I).
void RegisterDestroyHooks() {
    auto& w = MdRegistry::Get().Raw();
    w.observer().event(gaia::ecs::ObserverEvent::OnDel).all<ChildrenRef&>()
        .on_each([](gaia::ecs::Iter& it) {
            auto& reg = MdRegistry::Get();
            auto ents = it.view<gaia::ecs::Entity>();
            auto crs  = it.view_mut<ChildrenRef>();
            for (uint32_t r = 0; r < it.size(); ++r) {
                const ChildrenRef& cr = crs[r];
                for (int i = 0; i < cr.count; ++i) {
                    MdEntity child = cr.children[i];
                    if (reg.Valid(child) && (reg.Handle(child).has<ParentRef>())) reg.Remove<ParentRef>(child);
                }
            }
            (void)ents;
        });
    w.observer().event(gaia::ecs::ObserverEvent::OnDel).all<ParentRef&>()
        .on_each([](gaia::ecs::Iter& it) {
            auto ents = it.view<gaia::ecs::Entity>();
            auto prs  = it.view_mut<ParentRef>();
            for (uint32_t r = 0; r < it.size(); ++r)
                RemoveFromChildrenList(MdRegistry::Get(), prs[r].parent, MdEntity(ents[r]));
        });
}

} // namespace Hierarchy
