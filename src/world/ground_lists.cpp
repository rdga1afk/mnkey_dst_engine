#include <monkey_dust/world/ground_lists.h>
#include <algorithm>

namespace md {

bool GroundList::operator==(const GroundList& o) const {
    if (count != o.count) return false;
    for (int i = 0; i < count; ++i) if (ids[i] != o.ids[i] || channels[i] != o.channels[i]) return false;
    return true;
}

void GroundLists::Build(const Kbi1& k) {
    lists_.clear(); index_.clear(); side_ = 0;
    if (!k.Loaded()) return;
    const int dim = k.Dim(), res = k.Res(); side_ = dim * res;
    index_.assign((size_t)side_ * (size_t)side_, 0);
    lists_.push_back(GroundList{});   // 0 = empty
    for (int cz = 0; cz < dim; ++cz) for (int cx = 0; cx < dim; ++cx) {
        const auto& slots = k.Slots(cz * dim + cx);
        for (int sz = 0; sz < res; ++sz) for (int sx = 0; sx < res; ++sx) {
            const uint8_t mask = k.LeafMask(cz * dim + cx, sx, sz);
            GroundList l;
            for (int slot = 4; slot >= 0 && l.count < 4; --slot) {
                if (!(mask & (1u << slot)) || slots[(size_t)slot] == 0) continue;
                l.ids[l.count] = slots[(size_t)slot]; l.channels[l.count] = slot; ++l.count;
            }
            int found = -1;
            for (size_t i = 0; i < lists_.size(); ++i) if (lists_[i] == l) { found = (int)i; break; }
            if (found < 0) { found = (int)lists_.size(); lists_.push_back(l); }
            index_[(size_t)(cz * res + sz) * (size_t)side_ + (size_t)(cx * res + sx)] = (uint16_t)found;
        }
    }
}

std::vector<int> GroundLists::ListsIn(int tx0, int tz0, int tx1, int tz1) const {
    std::vector<int> out;
    tx0 = std::max(tx0, 0); tz0 = std::max(tz0, 0); tx1 = std::min(tx1, side_); tz1 = std::min(tz1, side_);
    std::vector<char> seen(lists_.size(), 0);
    for (int z = tz0; z < tz1; ++z) for (int x = tx0; x < tx1; ++x) { const uint16_t i = index_[(size_t)z * (size_t)side_ + (size_t)x]; if (i && !seen[i]) { seen[i] = 1; out.push_back(i); } }
    return out;
}

}   // namespace md
