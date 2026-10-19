#include <monkey_dust/ecs/hierarchy_utils.h>
#include <monkey_dust/components/hierarchy.h>
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




} // namespace Hierarchy
