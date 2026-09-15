#pragma once
// SquadController / SquadSystem (VBfA-AI5) — group AI LOD.
//
// Source: Viking Battle for Asgard squad system (tasks.bin AT_SQUAD_ACTIVITY_* states,
// 2026-05-19 RE). VBfA ran 1 pathfind per squad (not per NPC), reducing AI decisions
// from O(n) to O(n/squad_size) — the key to 1000-unit battles.
//
// Architecture:
//   SquadController is a component on a dedicated "squad entity" (not on member NPCs).
//   SquadSystem::Update() iterates squad entities (cheap: ~n/16 iterations) and
//   computes 1 target position per squad per THINK_INTERVAL_MS.
//   Members receive the target via CrowdSystem::SetTarget (ORCA steers them there)
//   and via blackboard (squad_tx/squad_tz) for BT nodes.
//
// Activity ladder (dist = squad_home → player, not per-NPC):
//   > 60m : PatrolRagged — random wander, no pathfinding, cheapest
//   40-60m : Patrol      — move toward home position
//   20-40m : AttackMove  — move toward player (1 ORCA target set per squad)
//   < 20m  : Fight       — BT takes over individual combat; squad only updates BB
//
// Integration in logic_tick.cpp:
//   SquadSystem::Update(dt_ms, player_x, player_z)  — before ai.Update()
//
// Creating squads from world_init.cpp:
//   SquadSystem::CreateSquad(members, count, home_x, home_z, faction_id)

#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/nav/crowd_system.h>
#include <monkey_dust/ai/fnv.h>
#include <monkey_dust/ai/ai_queries.h>
#include <cmath>

// ── Squad activity states (VBfA AT_SQUAD_ACTIVITY_*) ─────────────────────────
// Expanded from 13 → 29 states (VBfA-R VB-4 gap fill).
// Startup states last one think-tick (THINK_INTERVAL_MS = 2s) then auto-advance.
// BT members can read squad_activity from blackboard key "squad_activity" (uint8).
enum class SquadActivity : uint8_t {
    // ── Idle / pre-battle ────────────────────────────────────────────────────
    Idle                    = 0,  // AT_SQUAD_ACT_IDLE
    IdlePriorToBattle       = 1,  // AT_SQUAD_ACT_IDLE_PRIOR_TO_BATTLE
    IdlePriorToBattleReady  = 2,  // AT_SQUAD_ACT_IDLE_PRIOR_TO_BATTLE_MOTIVATED
    DrawWeapon              = 3,  // AT_SQUAD_ACT_DRAW_WEAPON — 1-tick transition
    // ── Patrol ───────────────────────────────────────────────────────────────
    PatrolRagged            = 4,  // AT_SQUAD_ACTIVITY_PATROL_RAGGED_START
    PatrolStartup           = 5,  // AT_SQUAD_ACTIVITY_PATROL_STARTUP
    Patrol                  = 6,  // AT_SQUAD_ACTIVITY_GUARD_PATROLLING
    PatrolFailed            = 7,  // AT_SQUAD_ACTIVITY_PATROL_PF_FAILED
    // ── Attack ───────────────────────────────────────────────────────────────
    AttackStartup           = 8,  // AT_SQUAD_ACTIVITY_ATTACK_STARTUP
    AttackMove              = 9,  // AT_SQUAD_ACTIVITY_ATTACK_PATROL
    AttackFightStartup      = 10, // AT_SQUAD_ACTIVITY_ATTACK_FIGHT_STARTUP
    Fight                   = 11, // AT_SQUAD_ACTIVITY_ATTACK_FIGHT
    StandGround             = 12, // AT_SQUAD_ACTIVITY_ATTACK_STAND_GROUND_COMBAT
    // ── Retreat / regroup ────────────────────────────────────────────────────
    Retreating              = 13, // VBfA-R: squad falling back
    Regrouping              = 14, // VBfA-R: squad gathering around home point
    Fleeing                 = 15, // VBfA-R: full rout — high-speed flee
    // ── Cover / suppression ──────────────────────────────────────────────────
    TakingCover             = 16, // VBfA-R: moving to cover node
    InCover                 = 17, // VBfA-R: behind cover, suppressed
    Flanking                = 18, // VBfA-R: flanking maneuver
    Suppressed              = 19, // VBfA-R: pinned, cannot advance
    // ── Non-combat activity ───────────────────────────────────────────────────
    Guarding                = 20, // VBfA-R: guarding position or object
    Escorting               = 21, // VBfA-R: escorting VIP or cargo
    Scouting                = 22, // VBfA-R: scouting ahead
    Looting                 = 23, // VBfA-R: looting bodies / containers
    Investigating           = 24, // VBfA-R: investigating suspicious area
    // ── Social / labour ──────────────────────────────────────────────────────
    Resting                 = 25, // VBfA-R: resting / sleeping
    Working                 = 26, // VBfA-R: labour / crafting task
    Alerted                 = 27, // VBfA-R: suspicious, weapon half-drawn
    Dead                    = 28, // VBfA-R: squad wiped out
    // ── Extended activity set (2026-05-28) ───────────────────────────────────
    // AT_SQUAD_* naming matches the reference game's activity taxonomy.
    IdleMain                = 29, // AT_SQUAD_ACT_IDLE_MAIN
    IdlePatrolToPos         = 30, // AT_SQUAD_ACT_IDLE_PATROL_TO_POS
    GuardStartup            = 31, // AT_SQUAD_ACTIVITY_GUARD_STARTUP
    GuardGuarding           = 32, // AT_SQUAD_ACTIVITY_GUARD_GUARDING
    SheathWeapon            = 33, // AT_SQUAD_ACT_SHEATH_WEAPON
    ShootTarget             = 34, // AT_SQUAD_SHOOT_TARGET
    Animate                 = 35, // AT_SQUAD_ACT_ANIMATE
    Summoned                = 36, // AT_SQUAD_ACT_SUMMONED
    Murder                  = 37, // AT_SQUAD_MURDER
    FormationActive         = 38, // ACTIVE_AT_SQUAD_FORMATION
    RecoverMembers          = 39, // AT_SQUAD_ACT_RECOVER_MEMBERS_FROM_SUB_VILLAGE
    NoActivity              = 40, // SQUAD_HAS_NO_ACTIVITY
    StartingUpPatrolToPos   = 41, // SQUAD_STARTING_UP_PATROL_TO_POS
    Deactivated             = 42, // SQUAD_DEACTIVATED
    COUNT                   = 43
};
static constexpr uint8_t SQUAD_ACTIVITY_COUNT = 43;

// ── SquadMemType — member role within a squad (A-2, Kenshi RE: "squad mem type") ──
// Controls formation position, AI priority, and response behaviour.
// Stored per-member in SquadController::member_roles[] indexed by member slot.
enum class SquadMemType : uint8_t {
    Generic  = 0,  // default — no specialisation
    Melee    = 1,  // front-line fighter, closes distance first
    Ranged   = 2,  // stays back, shoots before closing
    Medic    = 3,  // hangs back, prioritises healing downed allies
    Leader   = 4,  // squad leader — SquadController entity itself
    Scout    = 5,  // forward recon, higher detection range
    Heavy    = 6,  // slow, tanky; holds position under fire
    COUNT    = 7
};

// ── SquadController component (lives on squad entity, not on member NPCs) ─────
struct SquadController {
    static constexpr int   MAX_MEMBERS       = 30;  // F3: 16 → 30 (Kenshi patrol squads up to 20+)
    static constexpr float THINK_INTERVAL_MS = 2000.f;
    static constexpr float WANDER_INTERVAL_MS = 4000.f; // PatrolRagged retarget

    MdEntity  members[MAX_MEMBERS]      = {};
    SquadMemType  member_roles[MAX_MEMBERS] = {};  // A-2: per-member role enum
    int           member_count              = 0;

    SquadActivity activity             = SquadActivity::PatrolRagged;
    SquadActivity prev_activity        = SquadActivity::Idle; // for transition detection
    float         home_x              = 0.f;
    float         home_z              = 0.f;
    float         target_x            = 0.f;
    float         target_z            = 0.f;
    float         patrol_radius       = 40.f;
    uint8_t       faction_id          = 0;

    float         think_accum_ms      = 0.f;
    float         wander_accum_ms     = WANDER_INTERVAL_MS; // fire immediately
    uint32_t      rng_state           = 0xDEADBEEFu;

    // F3: squad-level morale + reinforcement (Kenshi RE patterns)
    uint8_t       morale              = 128;  // 0=routing, 255=fearless
    uint8_t       flee_threshold      = 30;   // morale below this → Fleeing
    uint8_t       reinforcement_id    = 0;    // faction_id of backup squad (0=none)
    uint8_t       _pad                = 0;
};

// ── SquadSystem ───────────────────────────────────────────────────────────────
class SquadSystem {
public:
    // Blackboard keys (compile-time FNV-1a) for member BT nodes.
    static constexpr uint32_t kSquadTx       = md::fnv1a("squad_tx");
    static constexpr uint32_t kSquadTz       = md::fnv1a("squad_tz");
    static constexpr uint32_t kSquadActivity = md::fnv1a("squad_activity"); // uint8 SquadActivity

    // Create a squad entity and return its handle.
    // members[count]: pre-spawned NPC entities to include.
    // home_x/z:       spawn-centre used for activity distance checks.
    static MdEntity CreateSquad(const MdEntity* members, int count,
                                    float home_x, float home_z,
                                    uint8_t faction_id,
                                    float patrol_radius = 40.f) {
        auto& reg        = MdRegistry::Get();
        MdEntity sq  = reg.Create();
        reg.Handle(sq).emplace<SquadController>();
        auto& sc         = reg.Handle(sq).get_mut<SquadController>();
        sc.home_x        = home_x;
        sc.home_z        = home_z;
        sc.target_x      = home_x;
        sc.target_z      = home_z;
        sc.faction_id    = faction_id;
        sc.patrol_radius = patrol_radius;
        // LCG seed from position so different squads wander differently
        sc.rng_state     = static_cast<uint32_t>(home_x * 73856093.f)
                         ^ static_cast<uint32_t>(home_z * 19349663.f);
        int n = count < SquadController::MAX_MEMBERS ? count : SquadController::MAX_MEMBERS;
        for (int i = 0; i < n; ++i) sc.members[i] = members[i];
        sc.member_count = n;
        return sq;
    }

    // Call once per logic tick (100ms) BEFORE ai.Update().
    // dt_ms:    LOGIC_TICK_S * 1000
    // player_x/z: current player world position
    static void Update(float dt_ms, float player_x, float player_z) {
        auto& reg = MdRegistry::Get();
        MdEach(ai_queries::SquadControllers(), [&](MdEntity, SquadController& sc) {
            if (sc.member_count <= 0) return;

            // ── Squad thinks every THINK_INTERVAL_MS ─────────────────────────
            sc.think_accum_ms  += dt_ms;
            sc.wander_accum_ms += dt_ms;
            if (sc.think_accum_ms < SquadController::THINK_INTERVAL_MS) return;
            sc.think_accum_ms = 0.f;

            // ── Activity ladder with VBfA startup transitions ────────────────
            float dx   = sc.home_x - player_x;
            float dz   = sc.home_z - player_z;
            float dist = sqrtf(dx * dx + dz * dz);

            sc.prev_activity = sc.activity;

            // F3: morale check — low morale overrides activity to Fleeing
            if (sc.morale < sc.flee_threshold &&
                sc.activity != SquadActivity::Fleeing &&
                sc.activity != SquadActivity::Dead) {
                sc.activity = SquadActivity::Fleeing;
            }
            // Recover morale slowly when not in combat
            if (sc.activity == SquadActivity::Fleeing && sc.morale < 255) ++sc.morale;

            // Startup states auto-advance to their running state each think tick.
            // Distance ladder then decides if we need a new transition.
            switch (sc.activity) {
                case SquadActivity::DrawWeapon:
                    sc.activity = SquadActivity::AttackStartup; break;
                case SquadActivity::PatrolStartup:
                    sc.activity = SquadActivity::Patrol; break;
                case SquadActivity::AttackStartup:
                    sc.activity = SquadActivity::AttackMove; break;
                case SquadActivity::AttackFightStartup:
                    sc.activity = SquadActivity::Fight; break;
                default: break;
            }

            // Distance-driven transitions (only on stable running states)
            if (dist > 60.f) {
                if (sc.activity == SquadActivity::Fight ||
                    sc.activity == SquadActivity::AttackMove ||
                    sc.activity == SquadActivity::StandGround)
                    sc.activity = SquadActivity::PatrolStartup;    // disengage → patrol
                else if (sc.activity != SquadActivity::Patrol &&
                         sc.activity != SquadActivity::PatrolStartup)
                    sc.activity = SquadActivity::PatrolRagged;
            } else if (dist > 40.f) {
                if (sc.activity == SquadActivity::PatrolRagged ||
                    sc.activity == SquadActivity::Idle)
                    sc.activity = SquadActivity::PatrolStartup;
            } else if (dist > 20.f) {
                if (sc.activity == SquadActivity::Patrol ||
                    sc.activity == SquadActivity::PatrolRagged ||
                    sc.activity == SquadActivity::IdlePriorToBattle)
                    sc.activity = SquadActivity::DrawWeapon;       // commit to attack
                else if (sc.activity == SquadActivity::Fight ||
                         sc.activity == SquadActivity::StandGround)
                    sc.activity = SquadActivity::AttackMove;       // player backed off
            } else {
                if (sc.activity == SquadActivity::AttackMove)
                    sc.activity = SquadActivity::AttackFightStartup;
                else if (sc.activity == SquadActivity::Fight && dist < 5.f)
                    sc.activity = SquadActivity::StandGround;      // AT_SQUAD_STAND_GROUND
                else if (sc.activity == SquadActivity::Patrol ||
                         sc.activity == SquadActivity::PatrolRagged)
                    sc.activity = SquadActivity::IdlePriorToBattle; // threat spotted
            }

            // Determine if members are in active fight (for BroadcastTarget)
            const bool is_fight = (sc.activity == SquadActivity::Fight ||
                                   sc.activity == SquadActivity::StandGround);

            // ── Compute squad target ─────────────────────────────────────────
            switch (sc.activity) {
                case SquadActivity::Idle:
                case SquadActivity::IdlePriorToBattle:
                case SquadActivity::IdlePriorToBattleReady:
                    break;

                case SquadActivity::PatrolRagged:
                    if (sc.wander_accum_ms >= SquadController::WANDER_INTERVAL_MS) {
                        sc.wander_accum_ms = 0.f;
                        sc.rng_state = sc.rng_state * 1664525u + 1013904223u;
                        float ang   = (sc.rng_state & 0xFFFFu) * (6.2832f / 65536.f);
                        float r     = sc.patrol_radius * 0.45f;
                        sc.target_x = sc.home_x + cosf(ang) * r;
                        sc.target_z = sc.home_z + sinf(ang) * r;
                    }
                    break;

                case SquadActivity::PatrolStartup:
                case SquadActivity::Patrol:
                case SquadActivity::PatrolFailed:
                    sc.target_x = sc.home_x;
                    sc.target_z = sc.home_z;
                    break;

                case SquadActivity::DrawWeapon:
                case SquadActivity::AttackStartup:
                case SquadActivity::AttackMove:
                case SquadActivity::AttackFightStartup:
                case SquadActivity::Fight:
                case SquadActivity::StandGround:
                    sc.target_x = player_x;
                    sc.target_z = player_z;
                    break;

                case SquadActivity::Fleeing:
                case SquadActivity::Retreating:
                    // F3: flee away from player (mirror position through home)
                    sc.target_x = sc.home_x + (sc.home_x - player_x) * 0.5f;
                    sc.target_z = sc.home_z + (sc.home_z - player_z) * 0.5f;
                    break;

                default: break;
            }

            // ── Broadcast target to all members ──────────────────────────────
            BroadcastTarget(sc, reg);
        });
    }

private:
    static void BroadcastTarget(SquadController& sc, MdRegistry& reg) {
        const float tx = sc.target_x, tz = sc.target_z;
        const bool is_fight = (sc.activity == SquadActivity::Fight ||
                               sc.activity == SquadActivity::StandGround);
        const bool is_moving = (sc.activity != SquadActivity::Idle &&
                                sc.activity != SquadActivity::IdlePriorToBattle &&
                                sc.activity != SquadActivity::StandGround);
        const uint8_t act_u8 = static_cast<uint8_t>(sc.activity);

        // Formation radius: spread members around target to avoid heap convergence.
        // Combat: tight (1.5m), patrol: loose (3m).
        const float form_r = is_fight ? 1.5f : 3.0f;
        const float angle_step = (sc.member_count > 1)
            ? 6.2832f / (float)sc.member_count : 0.f;

        for (int i = 0; i < sc.member_count; ++i) {
            MdEntity m = sc.members[i];
            if (!reg.Valid(m)) continue;

            auto* bb = reg.Handle(m).try_get_mut<AgentBlackboard>();
            if (bb) {
                bb_set_float(*bb, kSquadTx, tx);
                bb_set_float(*bb, kSquadTz, tz);
                bb_set_int  (*bb, kSquadActivity, static_cast<int32_t>(act_u8));
            }

            // In Fight/StandGround: BT drives individual movement
            if (is_fight) continue;

            auto* nav = reg.Handle(m).try_get_mut<NavAgent>();
            if (!nav) continue;

            // Per-member angular offset so they don't converge to a single point.
            float ang   = angle_step * i;
            float mtx   = tx + cosf(ang) * form_r;
            float mtz   = tz + sinf(ang) * form_r;
            nav->target_x  = mtx;
            nav->target_z  = mtz;
            nav->is_moving = is_moving;

            if (nav->crowd_idx >= 0 && CrowdSystem::Get().IsReady())
                CrowdSystem::Get().SetTarget(nav->crowd_idx, mtx, mtz);
        }
    }
};
