#pragma once
// Simple two-character fight loop (DE-364) on top of combat_hit.h. Off by default: with `enabled == false` RunFight does nothing.
// One round: each living, not knocked-out side attacks in turn. Order per attack: attack roll (AttackRoll, levels of both sides),
// then ResolveHit (block roll, part pick, armour, resistance, damage, knock-out). The fight ends when one side is knocked out.
// DEVIATION: the exe's attack timing, range and animation gates are not modelled; one attack per side per round.
#include <monkey_dust/world/combat_hit.h>

namespace md::world::combat {

struct FightConfig {
    bool enabled = false;      // the switch; the game leaves it off until the owner decides
    int max_rounds = 200;
};

struct Fighter {
    BodyState body;
    DamageRecord weapon;       // filled by the caller (weapon-side filling is the damage-component read, see the research doc)
    float attack_level = 0;    // the attack skill used against the other side's block level
    float block_level = 0;
};

struct FightResult {
    bool ran = false;
    int rounds = 0;
    int winner = -1;           // 0 = a, 1 = b, -1 = none (disabled or round limit)
    int hits[2] = {0, 0};      // landed hits by side
    int blocks[2] = {0, 0};    // attacks of that side that were blocked
    int misses[2] = {0, 0};    // attacks of that side that lost the attack roll
};

inline FightResult RunFight(const HitConstants& c, const FightConfig& cfg, Fighter& a, Fighter& b, Rng& rng) {
    FightResult out;
    if (!cfg.enabled) return out;
    out.ran = true;
    Fighter* side[2] = {&a, &b};
    for (int round = 0; round < cfg.max_rounds; ++round) {
        out.rounds = round + 1;
        for (int i = 0; i < 2; ++i) {
            Fighter& at = *side[i];
            Fighter& df = *side[1 - i];
            if (at.body.knocked_out || df.body.knocked_out) continue;
            const float r1 = rng.Next01(), r2 = rng.Next01();
            if (!AttackRoll(c, at.attack_level, df.block_level, r1, r2)) { ++out.misses[i]; continue; }
            HitInput in;
            in.damage = at.weapon;
            in.attacker_level = at.attack_level;
            in.defender_block_level = df.block_level;
            const HitResult h = ResolveHit(c, in, df.body, rng);
            if (h.blocked) ++out.blocks[i]; else ++out.hits[i];
            if (df.body.knocked_out) { out.winner = i; return out; }
        }
    }
    return out;
}

} // namespace md::world::combat
