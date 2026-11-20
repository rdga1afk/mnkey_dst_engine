#pragma once
// Simple melee hit resolution (DE-364). Pure: no allocation, no globals, no engine state; every number comes from the
// install's GLOBAL CONSTANTS record (HitConstants) or from the caller. Facts and addresses: docs/research/COMBAT_HIT_EXE_READ_2026-11-20.md.
// Read level per part (exe `kenshi_x64.exe`): attack roll 0x140665370, block chance 0x140885d80 / 0x1408862d0, toughness helpers
// 0x140644770 / 0x140657620, hit application 0x1406508d0 (part pick, armour, resistance), part damage 0x14064f300.
// DEVIATION: the random stream is splitmix64 reduced to 15 bits (the exe uses C rand() * 2^-15); same distribution, own sequence.
// UNKNOWN (kept out of the maths, see the doc): which side of the attack roll lands, the polarity of the exe's first block flag,
// the global damage factor DAT_142133580, the armour cap 0x14168a25c, the weapon-side filling of the damage record.
#include <cstdint>
#include <monkey_dust/io/fcs_gamedata.h>

namespace md::world::combat {

constexpr int kMaxParts = 16;

struct HitConstants {
    float attack_chance_factor = 0.05f;   // `attack chance factor`
    float base_block_chance = 70.f;       // `base block chance`, percent
    float block_reduction_per_level = 1.5f;  // `block chance reduction per 10levels` x 0.1
    float block_increase_per_level = 1.2f;   // `block chance increase per 10levels` x 0.1
    float resistance_min = -0.5f, resistance_max = 0.5f;  // `damage resistance min/max`
    float ko_point_min = 10.f, ko_point_max = 99.f;       // `min/max toughness ko point`
    float ko_mult_1 = 4.f, ko_mult_99 = 1.f;              // `knockout mult 1/99`
    float ko_time_base = 30.f;                            // `knockout time base`
    float blunt_permanent_organ_damage = 0.f;             // `blunt permanent organ damage`
    float immediate_blood_loss = 0.2f;                    // `immediate blood loss`
    bool loaded = false;                  // true when filled from an install record
};

// Reads the GLOBAL CONSTANTS record (ItemType 27) of merged data. False (out unchanged) when the record or any key is missing.
inline bool LoadHitConstants(const md::fcs::GameDataContainer& data, HitConstants& out) {
    const auto rec = data.OfType(27);
    if (rec.empty()) return false;
    const md::fcs::GameData& r = *rec.front();
    HitConstants c;
    auto get = [&r](const char* key, float& v) {
        const auto it = r.floats.find(key);
        if (it == r.floats.end()) return false;
        v = it->second;
        return true;
    };
    float up = 0.f, down = 0.f;
    const bool ok = get("attack chance factor", c.attack_chance_factor) && get("base block chance", c.base_block_chance)
        && get("block chance reduction per 10levels", down) && get("block chance increase per 10levels", up)
        && get("damage resistance min", c.resistance_min) && get("damage resistance max", c.resistance_max)
        && get("min toughness ko point", c.ko_point_min) && get("max toughness ko point", c.ko_point_max)
        && get("knockout mult 1", c.ko_mult_1) && get("knockout mult 99", c.ko_mult_99)
        && get("knockout time base", c.ko_time_base) && get("blunt permanent organ damage", c.blunt_permanent_organ_damage)
        && get("immediate blood loss", c.immediate_blood_loss);
    if (!ok) return false;
    c.block_reduction_per_level = down * 0.1f;
    c.block_increase_per_level = up * 0.1f;
    c.loaded = true;
    out = c;
    return true;
}

// Random stream: splitmix64; Next15() is uniform over 0..32767, Next01() = Next15() * 2^-15 (the exe's rand01 range).
struct Rng {
    uint64_t s = 0;
    explicit Rng(uint64_t seed = 0) : s(seed) {}
    uint32_t Next15() {
        s += 0x9e3779b97f4a7c15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        return static_cast<uint32_t>(z >> 49);  // top 15 bits
    }
    float Next01() { return static_cast<float>(Next15()) * (1.f / 32768.f); }
};

// Attack roll 0x140665370(a, b) as the exe's boolean: the lower side gets weight 1 + k*difference on the other; true when
// f3*r1 < f4*r2 with f3 = (b-a)k+1, f4 = 1 for a <= b, else f3 = 1, f4 = (a-b)k+1. Equal levels: 0.5.
inline bool AttackRoll(const HitConstants& c, float a, float b, float r1, float r2) {
    float f3 = 1.f, f4 = 1.f;
    if (a <= b) f3 = (b - a) * c.attack_chance_factor + 1.f;
    else f4 = (a - b) * c.attack_chance_factor + 1.f;
    return f3 * r1 < f4 * r2;
}

// Block chance in percent (0x140885d80): d = defender block level - attacker level; base + 1.5 d (d <= 0) or + 1.2 d (d > 0);
// the part above 90 counts 0.2; upper clamp 95; lower clamp `lower` (0 in 0x140885d80, 5 in 0x1408862d0).
inline float BlockChancePercent(const HitConstants& c, float defender_level, float attacker_level, float lower = 0.f) {
    const float d = defender_level - attacker_level;
    float p = c.base_block_chance + d * (d <= 0.f ? c.block_reduction_per_level : c.block_increase_per_level);
    if (p > 90.f) p = 90.f + (p - 90.f) * 0.2f;
    if (p > 95.f) p = 95.f;
    if (p < lower) p = lower;
    return p;
}

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline float Blend(float t, float lo, float hi) { return lo + (hi - lo) * t; }  // 0x140015b63

// Toughness factor t = clamp(stat * scale * 0.01, 0, 1) (stat +0x90 times +0x34 in the exe).
inline float ToughnessFactor(float stat, float scale = 1.f) { return Clamp01(stat * scale * 0.01f); }
inline float DamageResistance(const HitConstants& c, float t) { return Blend(Clamp01(t), c.resistance_min, c.resistance_max); }  // 0x140644770
// Knock-out threshold (0x140657620): (100 - blend(t, min ko, max ko) * arg) * 0.01.
inline float KnockoutThreshold(const HitConstants& c, float t, float arg) {
    return (100.f - Blend(t, c.ko_point_min, c.ko_point_max) * arg) * 0.01f;
}
// Knock-out time: knockout mult blended by toughness (4 -> 1) times the base (0x1406447d0, partial read).
inline float KnockoutTime(const HitConstants& c, float t) { return Blend(Clamp01(t), c.ko_mult_1, c.ko_mult_99) * c.ko_time_base; }

// Damage record as the exe passes it to 0x14064f300: c0 cut, c1 blunt, c2 pierce, c3 carried part, c4 scale (naming inferred).
struct DamageRecord { float cut = 0, blunt = 0, pierce = 0, carried = 0, scale = 1; };

// Armour on the chosen part, already merged over the worn pieces: fractions 0..1 for cut and pierce, a flat absorb taken off
// the third component, and the blunt coefficient is carried by `blunt_absorb` (fraction). Merge model read-level partial.
struct ArmourOnPart { float cut_reduction = 0, pierce_reduction = 0, flat_absorb = 0, blunt_absorb = 0; };

inline DamageRecord ApplyArmour(const DamageRecord& d, const ArmourOnPart& a) {
    DamageRecord r = d;
    r.cut = d.cut * (1.f - Clamp01(a.cut_reduction));
    r.pierce = d.pierce * (1.f - Clamp01(a.pierce_reduction));
    r.pierce = r.pierce > a.flat_absorb ? r.pierce - a.flat_absorb : 0.f;
    r.blunt = d.blunt * (1.f - Clamp01(a.blunt_absorb));
    return r;
}
// Every component x (1 - k), negatives cleared (0x1406508d0 step 3).
inline DamageRecord ApplyResistance(const DamageRecord& d, float k) {
    DamageRecord r = d;
    const float m = 1.f - k;
    auto f = [m](float v) { float x = v * m; return x < 0.f ? 0.f : x; };
    r.cut = f(d.cut); r.blunt = f(d.blunt); r.pierce = f(d.pierce); r.carried = f(d.carried);
    return r;
}

struct PartState {
    float size = 1.f;          // pick weight (+0x38)
    float weight_factor = 1.f; // +0x3c: 0 after a hit on a dead part
    float health = 100.f;      // +0x40
    float recoverable = 0.f;   // +0x44
    float bonus = 0.f;         // +0x4c
    float max_health = 100.f;  // max health x factors - offset, supplied by the caller
    bool vital = false;        // +0x32
    ArmourOnPart armour;
    bool down = false;
};

struct BodyState {
    PartState parts[kMaxParts];
    int part_count = 0;
    float blood = 100.f;       // +0x70
    float toughness = 0.f;     // stat 0..100 (already scaled)
    bool knocked_out = false;
};

struct HitInput {
    DamageRecord damage;       // the weapon's record (filling it from a weapon: UNKNOWN, caller supplies)
    float attacker_level = 0;
    float defender_block_level = 0;
};

struct HitResult {
    bool blocked = false;
    int part = -1;
    DamageRecord applied;      // after armour and resistance
    float health_delta = 0;    // part health change (negative)
    float blood_delta = 0;     // body blood change (<= 0)
    bool part_down = false;
    bool knockout = false;
    float knockout_time = 0;
};

// Part pick (0x1406508d0 step 1): weights size * weight_factor over parts that are still alive; one draw r01 picks by upper bound on
// the cumulative sum; one candidate is taken directly, none gives the last part.
inline int PickPart(const BodyState& b, float r01) {
    float sum = 0.f; int last = -1, count = 0;
    for (int i = 0; i < b.part_count; ++i) {
        const PartState& p = b.parts[i];
        if (p.down) continue;
        const float w = p.size * p.weight_factor;
        if (w <= 0.f) continue;
        sum += w; last = i; ++count;
    }
    if (count == 0) return b.part_count - 1;
    if (count == 1) return last;
    const float x = r01 * sum;
    float acc = 0.f;
    for (int i = 0; i < b.part_count; ++i) {
        const PartState& p = b.parts[i];
        if (p.down) continue;
        const float w = p.size * p.weight_factor;
        if (w <= 0.f) continue;
        acc += w;
        if (x < acc) return i;
    }
    return last;
}

// Resolve one melee hit against `body` (mutated). Draw order: block, part pick. The block polarity is read as
// "blocked when draw*100 < chance" (the exe's second flag in 0x140887970; the first flag's polarity is UNKNOWN).
inline HitResult ResolveHit(const HitConstants& c, const HitInput& in, BodyState& body, Rng& rng) {
    HitResult res;
    if (body.part_count <= 0) return res;
    const float chance = BlockChancePercent(c, in.defender_block_level, in.attacker_level);
    res.blocked = rng.Next01() * 100.f < chance;
    const float r_part = rng.Next01();
    if (res.blocked) return res;

    res.part = PickPart(body, r_part);
    PartState& p = body.parts[res.part];
    const float t = ToughnessFactor(body.toughness);
    DamageRecord d = ApplyResistance(ApplyArmour(in.damage, p.armour), DamageResistance(c, t));
    res.applied = d;

    // 0x14064f300: blood, part health, recoverable damage.
    res.blood_delta = -((d.cut + d.pierce) * c.immediate_blood_loss * d.scale);
    res.health_delta = -(d.cut + d.pierce + c.blunt_permanent_organ_damage * d.blunt);
    body.blood += res.blood_delta;
    p.health += res.health_delta;
    p.recoverable += (1.f - c.blunt_permanent_organ_damage) * d.blunt + d.carried;

    const float effective = p.health - p.recoverable + p.bonus;
    if (effective < 0.f) { p.down = true; res.part_down = true; p.weight_factor = 0.f; }
    if ((p.down && p.vital) || body.blood <= 0.f) {
        if (!body.knocked_out) {
            body.knocked_out = true;
            res.knockout = true;
            res.knockout_time = KnockoutTime(c, t);
        }
    }
    return res;
}

} // namespace md::world::combat
