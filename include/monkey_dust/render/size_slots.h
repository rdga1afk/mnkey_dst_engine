#pragma once
// DE-362: render targets kept per size. The dynamic resolution moves between a few fixed levels (dynres3.h), each with its own set of targets that is made once (warm-up)
// and then only swapped in; a change of scale must allocate nothing. `cur` is the active set, `alt[N]` the stored ones; a Set needs Empty().
#include <utility>

namespace md {
inline int& TargetCreations() { static int n = 0; return n; }   // sets created after start (the warm-up creates them; a scale change at run time must not)

template<class Set, int N, class Match> inline bool SlotSwapIn(Set& cur, Set (&alt)[N], Match m) {   // a stored set matches: it becomes the active one, the old active one takes its place
    for (int i = 0; i < N; ++i) if (!alt[i].Empty() && m(alt[i])) { std::swap(cur, alt[i]); return true; }
    return false;
}
template<class Set, int N, class Release> inline void SlotPushCur(Set& cur, Set (&alt)[N], int& rr, Release rel) {   // no match: the active set is stored (a free slot, else the oldest goes); `cur` comes back empty for the caller to create
    ++TargetCreations();
    if (cur.Empty()) return;   // nothing to store (the first set)
    int v = -1; for (int i = 0; i < N; ++i) if (alt[i].Empty()) { v = i; break; }
    if (v < 0) { v = rr; rr = (rr + 1) % N; rel(alt[v]); }
    std::swap(alt[v], cur);
}
}   // namespace md
