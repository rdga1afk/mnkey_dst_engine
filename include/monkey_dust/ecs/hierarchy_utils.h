#pragma once
// Hierarchy:: — keeps ParentRef/ChildrenRef (components/hierarchy.h)
// consistent on both sides, and guards against dangling references when
// either side of the relation is destroyed. See hierarchy.h for scope and
// confirmed use cases (carry unconscious NPC / lead captive).
#include <monkey_dust/ecs/md_entity.h>

namespace Hierarchy {



// Detaches child from its current parent, if any. No-op if child has no
// ParentRef.
void ClearParent(MdEntity child);

} // namespace Hierarchy
