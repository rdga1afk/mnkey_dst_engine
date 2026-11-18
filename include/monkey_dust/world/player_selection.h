#pragma once
// The player's selection set, as the exe's select / deselect core keeps it (our read of 0x1407f7f20, private notes; ksdp/functions/exe_movement.yml):
// a set of object ids; selecting adds, deselecting removes ONLY while two or more are selected (the last selected character cannot be deselected
// this way); either change marks the controller dirty; selecting stores the object's position as the last selected position, and an empty set resets it.
// Not wired into the live game yet (the slice has no player selection); read level only, not emulated.
#include <cstdint>

namespace md::world {

class PlayerSelection {
public:
    static constexpr int kMax = 256;
    struct Pos { float x = 0.f, y = 0.f, z = 0.f; };

    bool Has(uint64_t id) const { return Find(id) >= 0; }
    int Count() const { return n_; }
    bool Dirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }
    const Pos& LastPos() const { return last_; }

    // Returns true when the set changed. id 0 (a missing object) does nothing.
    bool Select(uint64_t id, const Pos& p) {
        if (id == 0 || Has(id) || n_ >= kMax) return false;
        ids_[n_++] = id; last_ = p; dirty_ = true;
        return true;
    }
    bool Deselect(uint64_t id) {
        const int i = Find(id);
        if (id == 0 || i < 0 || n_ < 2) return false;     // the last selected stays
        ids_[i] = ids_[--n_]; dirty_ = true;
        return true;
    }

private:
    int Find(uint64_t id) const { for (int i = 0; i < n_; ++i) if (ids_[i] == id) return i; return -1; }
    uint64_t ids_[kMax] = {};
    int n_ = 0;
    bool dirty_ = false;
    Pos last_;
};

} // namespace md::world
