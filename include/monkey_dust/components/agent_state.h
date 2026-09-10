#pragma once
#include <monkey_dust/ai/fnv.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdint>
#if defined(MD_ECS_GAIA)
#include <gaia.h>
#endif

// ── Pattern 1: MotivationType ─────────────────────────────────────────────────
// determines which BT branch is active.
// DirectorSystem/FlowGraph write this; BT MotivationCheck nodes read it.
enum class MotivationType : uint8_t {
    None           =  0,
    Idle           =  1,  // default wander/patrol
    Stalk          =  2,  // STALK_MOTIVATION
    Attack         =  3,  // ATTACK_MOTIVATION
    ThreatAware    =  4,  // THREAT_AWARE_MOTIVATION
    Search         =  5,  // SEARCH_SYSTEMATIC_MOTIVATION
    Flee           =  6,  // retreat from threat
    Script         =  7,  // CINEMATIC_MOTIVATION
    Despawn        =  8,  // DESPAWN_MOTIVATION
    Suspicious     =  9,  // SUSPECT_TARGET_RESPONSE_MOTIVATION
    BackstageStalk = 10,  // BACKSTAGE_STALK_MOTIVATION
    Ambush         = 11,  // AMBUSH_MOTIVATION
    Breakout       = 12,  // BREAKOUT_MOTIVATION
    PlayerHiding   = 13,  // PLAYER_HIDE_MOTIVATION
    Work           = 14,  // Kenshi: NPC working at building/task
    Rest           = 15,  // Kenshi: sleeping/resting
    Trade          = 16,  // Kenshi: trading at shop
    Medic          = 17,  // Kenshi: healing self or ally
    Dormant        = 18,  // CATHODE RE: NPC_Sleeping_Android; skip full AI, minimal update
    COUNT
};

// ── Pattern 5: AgentTimerSlot ─────────────────────────────────────────────────
// 26 named per-agent timer slots.
// BT TimerStart/TimerCheck nodes index by slot enum cast to uint8_t.
enum class AgentTimerSlot : uint8_t {
    SuspectTargetResponse =  0,
    HeightenedSenses      =  1,  // 25 s — raised alertness after combat
    ThreatAwareTimeout    =  2,
    ThreatAwareDuration   =  3,
    SearchTimeout         =  4,
    BackstageStalkTimeout =  5,
    AmbushTimeout         =  6,
    AttackBan             =  7,  // cooldown between attacks
    MeleeAttackBan        =  8,
    VentBan               =  9,
    NpcStayInCoverShoot   = 10,
    NpcJustLeftCombat     = 11,
    AttackKeepChasing     = 12,
    DelayReturnToSpawn    = 13,
    TargetInCrawlspace    = 14,
    DurationSinceSearch   = 15,
    BackstagePickKilltrap = 16,
    FlankedVentAttack     = 17,
    ThreatAwareVisual     = 18,
    ResponseToBackstage   = 19,
    VentAttract           = 20,
    SeenPlayerAimWeapon   = 21,
    SearchBan             = 22,
    ObserveTarget         = 23,
    RepeatedPathfindFail  = 24,
    Generic               = 25,
    COUNT
};
static constexpr int MAX_AGENT_TIMERS = static_cast<int>(AgentTimerSlot::COUNT);  // 26

// ── Pattern 4: LogicCharacterFlags ───────────────────────────────────────────
// 40-bit flat bitmask per agent.
// Index by lcf::* constants (uint8_t bit position 0-63).
// Replaces the old uint32_t flags field. O(1) test/set/clear, trivially serializable.
namespace lcf {
    static constexpr uint8_t DONE_BREAKOUT          =  0;
    static constexpr uint8_t SHOULD_RESET           =  1;
    static constexpr uint8_t DO_ASSAULT_CHECKS      =  2;
    static constexpr uint8_t IS_IN_VENT             =  3;
    static constexpr uint8_t BANNED_FROM_VENT       =  4;
    static constexpr uint8_t IS_SITTING             =  5;
    static constexpr uint8_t DONE_ESCALATION        =  6;
    static constexpr uint8_t HAS_DONE_GRAPPLE_BREAK =  7;
    static constexpr uint8_t HAS_RECEIVED_DOT       =  8;
    static constexpr uint8_t SHOULD_BREAKOUT        =  9;
    static constexpr uint8_t SHOULD_ATTACK          = 10;
    static constexpr uint8_t SHOULD_HIT_AND_RUN     = 11;
    static constexpr uint8_t DONE_HIT_AND_RUN       = 12;
    static constexpr uint8_t PLAYER_HIDING          = 13;
    static constexpr uint8_t ATTACK_HIDING_PLAYER   = 14;
    static constexpr uint8_t NPC_KNOWS_VENT         = 15;
    static constexpr uint8_t IS_CORPSE_TRAP         = 16;
    static constexpr uint8_t SHOULD_DESPAWN         = 17;
    static constexpr uint8_t ATTACK_IN_THRESHOLD    = 18;
    static constexpr uint8_t LOCK_BACKSTAGE_STALK   = 19;
    static constexpr uint8_t TOTALLY_BLIND          = 20;
    static constexpr uint8_t PLAYER_WON_QTE         = 21;
    static constexpr uint8_t NPC_IS_INERT           = 22;
    static constexpr uint8_t NPC_IS_DUMMY           = 23;
    static constexpr uint8_t SHOULD_AMBUSH          = 24;
    static constexpr uint8_t NEVER_AGGRESSIVE       = 25;
    static constexpr uint8_t MUTE_DIALOGUE          = 26;
    static constexpr uint8_t DOING_THREAT_ANIM      = 27;
    static constexpr uint8_t DONE_THREAT_AWARE      = 28;
    static constexpr uint8_t BLOCK_AMBUSH           = 29;
    static constexpr uint8_t PREVENT_GRAPPLES       = 30;
    static constexpr uint8_t PREVENT_ALL_ATTACKS    = 31;
    static constexpr uint8_t ALLOW_FLANK_VENT       = 32;
    static constexpr uint8_t IGNORE_VENT_BEHAV      = 33;
    static constexpr uint8_t AIMED_STANCE           = 34;
    static constexpr uint8_t AIMED_LOW_STANCE       = 35;
    static constexpr uint8_t CLOSE_TO_BACKSTAGE     = 36;
    static constexpr uint8_t IS_IN_EXPLOITABLE_AREA = 37;
    static constexpr uint8_t IS_ON_LADDER           = 38;
    static constexpr uint8_t HAS_PATH_FAIL          = 39;
    static constexpr uint8_t IS_DEAD                = 40;
    static constexpr uint8_t IS_SUSPENDED           = 41;  // Batch 11 P8: BT tick skipped when set
    static constexpr uint8_t IS_PLAYER              = 42;  // Batch 15: marks the player-controlled entity
    static constexpr uint8_t IS_IN_COVER            = 43;  // Batch 17: entity is currently in cover
    static constexpr uint8_t HAS_SEARCHED_RECENT_SENSED_POS     = 44;  // Batch 22
    static constexpr uint8_t IS_DONE_SUSPECT_RESPONSE_MOVETO    = 45;  // Batch 26
    static constexpr uint8_t HAS_KILLTRAP                        = 46;  // Batch 26: searched most recent sensed position
    static constexpr uint8_t IS_TARGETED                         = 47;  // Batch 33: another entity has this as Combat::target (game combat sets)
    static constexpr uint8_t IS_USING_MELEE                      = 48;  // Batch 33: entity performed melee this tick (game combat sets)
    // Kenshi-style state flags (Batch K)
    static constexpr uint8_t IS_KNOCKED_OUT                      = 49;  // NPC unconscious — HP≈0, not dead; recovers via medic
    static constexpr uint8_t IS_SURRENDERED                      = 50;  // NPC gave up; won't attack; can be taken prisoner
    static constexpr uint8_t IS_PRISONER                         = 51;  // NPC being escorted as prisoner (slaver/bounty)
}

struct LogicCharacterFlags {
    uint64_t bits = 0;
    bool test  (uint8_t idx) const noexcept { return (bits >> idx) & 1ull; }
    void set   (uint8_t idx)       noexcept { bits |=  (1ull << idx); }
    void clear (uint8_t idx)       noexcept { bits &= ~(1ull << idx); }
    void assign(uint8_t idx, bool v) noexcept { v ? set(idx) : clear(idx); }
};

// ── Pattern 6: AgentGauges ────────────────────────────────────────────────────
// float [0..1] per agent.
// GaugeCheck BT node fires when val >= threshold; GaugeSet resets to 0.
enum class GaugeType : uint8_t { Retreat = 0, StunDamage = 1, COUNT };
static constexpr int MAX_AGENT_GAUGES = static_cast<int>(GaugeType::COUNT);

struct AgentGauges {
    float val[MAX_AGENT_GAUGES] = {};
    float get(GaugeType t) const noexcept { return val[static_cast<int>(t)]; }
    void  set(GaugeType t, float v) noexcept { val[static_cast<int>(t)] = v; }
    void  add(GaugeType t, float d) noexcept { val[static_cast<int>(t)] += d; }
};

// ── Pattern 8: EntityStateFlag ────────────────────────────────────────────────
// 23 lifecycle flags per entity.
// entity_state field in AgentState stores the current OR of active flags.
enum class EntityStateFlag : uint32_t {
    None         = 0x000000,
    Activate     = 0x000001,
    Spawn        = 0x000002,
    Start        = 0x000004,
    Pause        = 0x000008,
    Attach       = 0x000010,
    Enable       = 0x000080,
    Simulate     = 0x000100,
    Lock         = 0x000200,
    Show         = 0x000400,
    Suspend      = 0x000800,
    ProxyEnable  = 0x001000,
    Floating     = 0x002000,
    LightOn      = 0x004000,
    InstallProxy = 0x008000,
    Suspended    = 0x080000,
    Ghosted      = 0x100000,
    Invisible    = 0x200000,
    Frozen       = 0x400000,
};

inline EntityStateFlag operator|(EntityStateFlag a, EntityStateFlag b) noexcept {
    return static_cast<EntityStateFlag>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool esf_test(uint32_t state, EntityStateFlag f) noexcept {
    return (state & static_cast<uint32_t>(f)) != 0u;
}

// ── C15: AwarenessState ───────────────────────────────────────────────────────
// cognitive state (knows where threat is?).
// BT AwarenessCheck node gates branches by exact state equality.
// DirectorSystem writes; BT reads.
enum class AwarenessState : uint8_t {
    Dead          = 0,  // DEAD:0
    Unaware       = 1,  // default patrol
    Suspicious    = 2,  // heard/saw something indirect
    Investigating = 3,  // moving to stimulus
    SearchingArea = 4,  // SEARCHING_AREA:4 — area sweep without lock-on
    Alert         = 5,  // ALERT — on-edge, weapon raised
    Aware         = 6,  // AWARE:6 — full combat lock-on
};

// ── C16: AlertnessState ───────────────────────────────────────────────────────
// behavioural alertness level.
// Orthogonal to AwarenessState: Android can be ALERT but not Aware.
// DirectorSystem/damage handler writes; BT reads.
enum class AlertnessState : uint8_t {
    Relaxed = 0,  // normal patrol / idle
    Alert   = 1,  // ALERT:1 — just received stimulus, reacting
    Combat  = 2,  // actively engaging
    Fleeing = 3,  // retreating / panicked
};

// ── C17: NpcMood ──────────────────────────────────────────────────────────────
// affective state driving animation blending.
// DirectorSystem / FlowGraph writes; BT MoodCheck gates branches; AnimSystem reads.
enum class NpcMood : uint8_t {
    Neutral    = 0,
    Curious    = 1,
    Cautious   = 2,
    Suspicious = 3,  // SUSPICIOUS:3 — search-mode animations
    Panicked   = 4,  // PANICKED:4 — panic run / flinch
    Calm       = 5,  // post-deescalation
};

// ── C19: WithdrawState ────────────────────────────────────────────────────────
// 3-stage retreat FSM.
// Prevents attack continuation after cinematic/script hand-off.
// Invariant: actionMeleeAttack must call SetWithdraw(NotWithdrawing) before attacking.
enum class WithdrawState : uint8_t {
    NotWithdrawing     = 0,  // NOT_WITHDRAWING:0
    NeedsToWithdraw    = 1,  // NEEDS_TO_WITHDRAW:1 — intent set, not yet moving
    Withdrawing        = 2,  // WITHDRAWING:2 — actively retreating
    WithdrawnLocally   = 3,  // RE: WithdrawnLocally — left combat zone, still in level
    WithdrawnFromLevel = 4,  // RE: WithdrawnFromLevel — fully despawned / exited
};

// ── C: LocomotionState ────────────────────────────────────────────────────────
// MD LOCOMOTION_STATE enum — animation & physics system reads this.
// BT writes via actEnterVent (IS_IN_VENT maps to InVent).
enum class LocomotionState : uint8_t {
    Walking    = 0,
    Running    = 1,
    Crouching  = 2,
    InVent     = 3,  // mirrors lcf::IS_IN_VENT; kept in sync by actEnterVent/actExitVent
    Aiming     = 4,
    Traversing = 6,
    Idling     = 7,
};

// MD LOCOMOTION_TARGET_SPEED — NavSystem uses this to select speed profile.
enum class LocomotionTargetSpeed : uint8_t {
    Slowest = 0,
    Slow    = 1,
    Fast    = 2,
    Fastest = 3,
};

// ── C: AreaSweepType ──────────────────────────────────────────────────────────
// MD AREA_SWEEP_TYPE_CODE — controls the search pattern when motivation==Search.
// DirectorSystem writes; AreaSweepCheck BT node gates branches by type.
enum class AreaSweepType : uint8_t {
    InAndOutBetweenTargetAndPosition = 0,  // alternates between target and known position
    FixedRadiusAroundPosition        = 1,  // fixed-radius sweep around a fixed point
    AroundTarget                     = 2,  // continuous orbit around last-seen target
};

// ── MD_deepseek: NpcAggroLevel ───────────────────────────────────────────
// MD NPC_AGGRO_LEVEL — NPC escalation toward lethal engagement.
// Orthogonal to AlertnessState: androids escalate through verbal warnings before shooting.
enum class NpcAggroLevel : uint8_t {
    None          = 0,  // passive / off-duty
    StandDown     = 1,  // de-escalation order
    Interrogative = 2,  // verbal challenge — who are you?
    Warning       = 3,  // explicit warning shot / back off
    LastChance    = 4,  // final warning before lethal
    NoLimit       = 5,  // shoot on sight (same value as Aggressive in MD)
    Unknown       = 0xFF,
};

// ── MD_deepseek: NpcCombatState ──────────────────────────────────────────
// MD NPC_COMBAT_STATE — fine-grained combat phase within AlertnessState::Combat.
// Written by CombatSystem; BT reads via NpcCombatStateCheck/SetNpcCombatState.
enum class NpcCombatState : uint8_t {
    None                    =  0,
    Warning                 =  1,
    Attacking               =  2,
    ReachedObjective        =  3,
    EnteredCover            =  4,
    LeaveCover              =  5,
    StartRetreating         =  6,
    ReachedRetreat          =  7,
    LostSense               =  8,
    SuspiciousWarning       =  9,
    SuspiciousWarningFailed = 10,
    StartAdvance            = 11,
    DoneAdvance             = 12,
    Blocking                = 13,
    HeardBackstageAlien     = 14,
    AlienSighted            = 15,
    Unknown                 = 0xFF,
};

// ── MD_deepseek: MoodIntensity ───────────────────────────────────────────
// Intensity level of the current NpcMood — modulates animation blend weights.
enum class MoodIntensity : uint8_t { Low = 0, Medium = 1, High = 2, Unknown = 0xFF };

// ── MD_deepseek: AggressionGain ─────────────────────────────────────────
// Rate of NpcAggroLevel escalation on player detection events.
enum class AggressionGain : uint8_t { Low = 0, Med = 1, High = 2 };

// ── Batch 7: SuspiciousItemReaction ──────────────────────────────────────────
// MD SUSPICIOUS_ITEM_REACTION — NPC decision on how to react to a suspicious item.
// Orthogonal to SuspiciousItemStage (which tracks progress of investigation).
// Written by game/director logic; BT reads via SIReactionCheck.
enum class SuspiciousItemReaction : uint8_t {
    Investigate = 0,   // SUSPICIOUS_ITEM_REACTION_INVESTIGATE
    Alarm       = 1,   // SUSPICIOUS_ITEM_REACTION_ALARM — call for backup
    Ignore      = 2,   // SUSPICIOUS_ITEM_REACTION_IGNORE — not worth acting on
    Flee        = 3,   // SUSPICIOUS_ITEM_REACTION_FLEE — threat too high
    Unknown     = 0xFF,
};

// ── Batch 7: AmbushType ───────────────────────────────────────────────────────
// MD AMBUSH_TYPE — subtype of ambush approach when motivation==Ambush.
// Allows the BT to select different action sequences per ambush variant.
enum class AmbushType : uint8_t {
    None      = 0,
    Vent      = 1,   // drop from vent above target
    Ceiling   = 2,   // ceiling traverse to drop point
    Direct    = 3,   // direct approach from hiding
    Backstage = 4,   // wall-breach backstage ambush
    Unknown   = 0xFF,
};

// ── Batch 7: NoiseType ────────────────────────────────────────────────────────
// MD NOISE_TYPE — sub-classification of Audio-sense stimuli.
// SenseType::Audio covers all noise; NoiseType gives finer reaction detail.
// Written when an audio event fires; BT reads via NoiseTypeCheck.
enum class NoiseType : uint8_t {
    None      = 0,
    Footstep  = 1,   // NOISE_TYPE_FOOTSTEP
    Weapon    = 2,   // NOISE_TYPE_WEAPON — gunshot / melee
    Vent      = 3,   // NOISE_TYPE_VENT — vent panel, crawling
    Explosion = 4,   // NOISE_TYPE_EXPLOSION
    Voice     = 5,   // NOISE_TYPE_VOICE — NPC speech / radio
    Unknown   = 0xFF,
};

// ── Batch 4: EventType ────────────────────────────────────────────────────────
// Timestamped NPC event categories used by ConditionEventAOccuredAfterB.
// AgentState::event_ts[i] is set to nowMs when event i fires; 0 = never occurred.
enum class EventType : uint8_t {
    SensedTarget           = 0,
    SensedSuspiciousItem   = 1,
    TargetHiding           = 2,
    SuspectTargetResponse  = 3,
    COUNT                  = 4,
};
static constexpr uint8_t MAX_EVENT_TYPES = static_cast<uint8_t>(EventType::COUNT);

// ── ff:: frame-flag bit indices ───────────────────────────────────────────────
// MD FRAME_FLAGS enum — single-tick signals cleared at logic tick start.
// Use with AgentState::frame_flags (same mechanism as C13 pattern).
// Parallel to lcf::* but for per-frame transient signals only.
namespace ff {
    static constexpr uint8_t SUSPICIOUS_ITEM_LOW_PRIORITY          = 0;
    static constexpr uint8_t SUSPICIOUS_ITEM_MEDIUM_PRIORITY       = 1;
    static constexpr uint8_t SUSPICIOUS_ITEM_HIGH_PRIORITY         = 2;
    static constexpr uint8_t COULD_SEARCH                          = 3;
    static constexpr uint8_t COULD_RESPOND_TO_HIDING_PLAYER        = 4;
    static constexpr uint8_t COULD_DO_SUSPICIOUS_ITEM_HIGH         = 5;
    static constexpr uint8_t COULD_DO_SUSPECT_TARGET_RESPONSE_MOVETO = 6;
    static constexpr uint8_t SHOULD_MOVE_THROUGH_TARGET              = 7;  // Batch 10 P5
    static constexpr uint8_t SHOULD_MOVE_TO_COVER                   = 8;  // Batch 17: nav moves to cover point
    static constexpr uint8_t SHOULD_RANGED_SHOOT                    = 9;  // Batch 19: trigger ranged attack
    static constexpr uint8_t SHOULD_MOVE_TO_TARGET                  = 10; // Batch 21: nav moves toward target entity
    static constexpr uint8_t SHOULD_PLAY_DEATH                      = 11; // Batch 21: trigger death animation
    static constexpr uint8_t SHOULD_MELEE_ATTACK                    = 12; // Batch 21: trigger melee attack
    static constexpr uint8_t SHOULD_FACE_TARGET                      = 13; // Batch 23: rotate toward target while idling
    static constexpr uint8_t SHOULD_FACE_LAST_KNOWN_POS             = 14; // Batch 23: face last sensed position
    static constexpr uint8_t SHOULD_FACE_SI_POS                     = 15; // Batch 23: face suspicious item position
    static constexpr uint8_t SHOULD_RANGED_AIM                      = 16; // Batch 23: keep weapon raised and aimed
    static constexpr uint8_t SI_REACTION_SET                        = 17; // Batch 23: si_reaction was updated this frame
    static constexpr uint8_t SHOULD_DO_SUSPECT_TARGET_RESPONSE      = 18; // Batch 23: initiate suspect target response
    static constexpr uint8_t REQUESTING_COVER                       = 19; // Batch 24: request a cover position
    static constexpr uint8_t SHOULD_BREAKOUT                        = 20; // Batch 27: escape from threat
    static constexpr uint8_t SHOULD_MOVE_TO_LAST_KNOWN             = 21; // Batch 27: move to last sensed position
    static constexpr uint8_t SHOULD_MOVE_NEAR_TARGET               = 22; // Batch 27: move to nearest standing point to target
    static constexpr uint8_t SHOULD_ATTACK_MOVE                    = 23; // Batch 27: move while attacking
    static constexpr uint8_t SHOULD_CHANGE_COVER                   = 24; // Batch 27: move to a different cover position
    static constexpr uint8_t SHOULD_UPDATE_LAST_KNOWN              = 25; // Batch 28: refresh last known position from current sense
    static constexpr uint8_t SHOULD_INVESTIGATE                    = 26; // Batch 28: request investigation of last known position
    static constexpr uint8_t SHOULD_MARK_TARGET_LOST               = 27; // Batch 28: mark target as lost/gone
    static constexpr uint8_t SHOULD_FORCE_RETREAT                  = 28; // Batch 28: force retreat from current threat
    static constexpr uint8_t SHOULD_HOLD_POSITION                  = 29; // Batch 28: stop and hold current position
    static constexpr uint8_t SHOULD_PERFORM_AMBUSH                 = 30; // Batch 29: trigger ambush execution
    static constexpr uint8_t SHOULD_START_SEARCH                   = 31; // Batch 29: begin area search pattern
    static constexpr uint8_t SHOULD_CALL_FOR_HELP                  = 32; // Batch 29: broadcast distress to allies
    static constexpr uint8_t SHOULD_TAUNT_TARGET                   = 33; // Batch 29: play taunt animation/audio
    static constexpr uint8_t SHOULD_SURRENDER                      = 34; // Batch 29: initiate surrender sequence
    static constexpr uint8_t SHOULD_PURSUE_TARGET                  = 35; // Batch 30: aggressive pursuit of target
    static constexpr uint8_t SHOULD_CIRCLE_TARGET                  = 36; // Batch 30: circle/strafe around target
    static constexpr uint8_t SHOULD_BACK_OFF                       = 37; // Batch 30: retreat to safe distance
    static constexpr uint8_t SHOULD_CROUCH_MOVE                    = 38; // Batch 30: move while crouching
    static constexpr uint8_t SHOULD_VAULT                          = 39; // Batch 30: vault over obstacle
    static constexpr uint8_t SHOULD_ABORT_MELEE                   = 40; // Batch 33: cancel in-progress melee attack
    static constexpr uint8_t SHOULD_GET_OUT_OF_WAY                = 41; // Batch 33: clear path for ally/projectile
    static constexpr uint8_t SHOULD_TAKE_STEP                     = 42; // Batch 33: advance one step toward target
    static constexpr uint8_t SHOULD_THREAT_AWARE                  = 43; // Batch 33: play threat-aware animation
    static constexpr uint8_t SHOULD_THREAT_ESCALATE               = 44; // Batch 33: escalate aggro_level by one step
    static constexpr uint8_t SHOULD_MOVE_IN_DIRECTION             = 45; // Batch 33: strafe/advance; direction in as->move_direction
    static constexpr uint8_t SHOULD_FORCE_SEARCH                  = 46; // Batch 34: force search regardless of conditions
    static constexpr uint8_t SHOULD_APPLY_DAMAGE_CONTROL          = 47; // Batch 35: apply first aid / damage control response
    static constexpr uint8_t SHOULD_MOVE_TO_OBJECTIVE             = 48; // Batch 35: move to squad objective (bb squad_tx/tz)
    // Kenshi-style action flags (Batch K)
    static constexpr uint8_t SHOULD_STEALTH_KO                   = 49; // attempt silent knock-out on adjacent unsuspecting target
    static constexpr uint8_t SHOULD_ESCORT_PRISONER              = 50; // guard/escort entity in bb["escort_target"] toward bb["escort_dest"]
    static constexpr uint8_t SHOULD_CAPTURE_TARGET               = 51; // non-lethal capture attempt (stops at KO, not death)
}

// ── AgentBlackboard entry ─────────────────────────────────────────────────────
// typed parameter slot keyed by FNV-1a hash.
// type: 0=bool  1=int  2=float  3=vec3  4=enum
struct BlackboardEntry {
    uint32_t key;   // md::fnv1a("field_name") — compile-time or runtime
    uint8_t  type;
    uint8_t  _pad[3];
    union {
        bool     b;
        int32_t  i;
        float    f;
        float    v[3];
        uint32_t e;
    } val;
};
static_assert(sizeof(BlackboardEntry) == 20, "BlackboardEntry must be 20 bytes");

// ── AgentBlackboard ───────────────────────────────────────────────────────────
// Cold component — separate ECS component from AgentState.
// Only loaded when BT nodes or DirectorSystem need keyed dynamic values.
// Keeping it separate reduces AgentState from 728B → 244B, allowing 4× more
// AgentState entries to fit in Intel HD 520 L2 (1 MB).
static constexpr int MAX_BB_ENTRIES = 24;

// GAIA_STORAGE(Sparse) -- Phase 2 (prompt_/PROMPT_GAIA_MIGRATION.md §0/§4):
// one of the 5 components with a proven B3.4 risk. See world_transform.h's
// doc comment for the shared rationale.
struct AgentBlackboard {
#if defined(MD_ECS_GAIA)
    GAIA_STORAGE(Sparse);
#endif
    int             bb_count = 0;
    BlackboardEntry bb[MAX_BB_ENTRIES];
    // CATHODE RE §7.8: NPC_Squad_GetAwarenessWatermark — peak awareness level seen
    // within a rolling time window; avoids re-querying if awareness hasn't changed.
    // Written by SenseSystem/Director when awareness rises; read by BT AwarenessCheck.
    AwarenessState  awareness_watermark = AwarenessState::Unaware;
    uint32_t        watermark_ms        = 0;  // game-ms when watermark was last raised
};

// ── Batch 18: BehaviourMoodSet ───────────────────────────────────────────────
// MD BEHAVIOUR_MOOD_SET — threat-escalation composite state; orthogonal to NpcMood.
// Captures the escalation *trajectory* (neutral → escalating → escalated/aggressive/panicked).
// DirectorSystem writes; BehaviourMoodSetCheck BT node reads.
enum class BehaviourMoodSet : uint8_t {
    Neutral                      = 0,  // NEUTRAL
    ThreatEscalationAggressive   = 1,  // THREAT_ESCALATION_AGGRESSIVE — ramping toward aggression
    ThreatEscalationPanicked     = 2,  // THREAT_ESCALATION_PANICKED — ramping toward panic
    Aggressive                   = 3,  // AGGRESSIVE — sustained combat
    Panicked                     = 4,  // PANICKED — sustained flee
    ThreatEscalationEscalated    = 5,  // THREAT_ESCALATION_ESCALATED — ramping toward escalation
    Escalated                    = 6,  // ESCALATED — maximum threat state
    Unknown                      = 0xFF,
};

// ── Batch 18: ViewconeType ────────────────────────────────────────────────────
// MD VIEWCONE_TYPE — shape of the NPC's visual detection frustum.
// NavSystem / SenseSystem reads to compute activation overlap.
// BT reads via ViewconeTypeCheck; SenseSystem writes (or game config sets at spawn).
enum class ViewconeType : uint8_t {
    Rectangle = 0,  // RECTANGLE — wide flat rectangle (androids / security)
    Ellipse   = 1,  // ELLIPSE   — oval cone (alien / humans)
    Unknown   = 0xFF,
};

// ── Batch 18: SensoryType ─────────────────────────────────────────────────────
// MD SENSORY_TYPE — high-level channel that last triggered sense activation.
// Orthogonal to SenseType (which enumerates all sense slots): SensoryType
// classifies the *stimulus* (sight vs weapon-sound vs movement-sound).
// Written by SenseSystem when a channel fires above threshold; BT reads.
enum class SensoryType : uint8_t {
    Visual        = 0,  // VISUAL — line-of-sight detection
    WeaponSound   = 1,  // WEAPON_SOUND — gunshot, explosion
    MovementSound = 2,  // MOVEMENT_SOUND — footsteps, vent crawling
    Unknown       = 0xFF,
};

// ── Batch 17: CharacterClass ──────────────────────────────────────────────────
// MD CHARACTER_CLASS — high-level entity archetype for ConditionIsCharacterClass BT node.
// Orthogonal to AllianceGroup: class determines *what* the entity is; group determines *who* it fights.
enum class CharacterClass : uint8_t {
    Player       = 0,
    Alien        = 1,
    Android      = 2,
    Civilian     = 3,
    Security     = 4,
    Innocent     = 6,
    AndroidHeavy = 7,
    Unknown      = 0xFF,
};

// ── AgentState ────────────────────────────────────────────────────────────────
// Hot component (~244 bytes). Pairs with BehaviorTreeComponent on every AI entity.
// Accessed every BT tick; must stay cache-line-friendly.
//
// timers:        absolute deadline in game-ms; 0 = inactive. Indexed by AgentTimerSlot.
// lcflags:       Pattern 4 — 40-bit LogicCharacterFlags bitmask.
// frame_flags:   C13 — single-tick signal bits; cleared each logic tick.
// motivation:    Pattern 1 — current motivation; BT MotivationCheck reads this.
// entity_state:  Pattern 8 — EntityStateFlag lifecycle bitmask.
// gauges:        Pattern 6 — retreat/stun float gauges [0..1].
// awareness:     C15 — cognitive state (Unaware→Aware).
// alertness:     C16 — behavioural alertness (Relaxed→Fleeing).
// mood:          C17 — affective state for anim blending.
// withdraw_state:C19 — 3-stage retreat FSM.
// locomotion_state: MD LocomotionState — animation & physics system reads this.
// target_speed:     MD LocomotionTargetSpeed — nav speed profile.
// area_sweep_type:  MD AreaSweepType — current search pattern for Search motivation.
// Blackboard (bb): now in separate AgentBlackboard component (cold path).
struct AgentState {
    uint64_t            timers[MAX_AGENT_TIMERS];  // ms deadlines; 0 = inactive
    LogicCharacterFlags lcflags;                    // Pattern 4: 40-bit flag bitmask
    uint64_t            frame_flags;               // C13: cleared each logic tick
    uint32_t            entity_state;               // Pattern 8: EntityStateFlag OR
    AgentGauges         gauges;                     // Pattern 6: retreat/stun gauges
    MotivationType      motivation;                 // Pattern 1: active motivation
    AwarenessState      awareness;                  // C15: cognitive threat awareness
    AlertnessState      alertness;                  // C16: behavioural alertness level
    NpcMood             mood;                       // C17: affective state / anim layer
    WithdrawState       withdraw_state;             // C19: 3-stage retreat FSM
    LocomotionState     locomotion_state;           // MD: anim/physics locomotion
    LocomotionTargetSpeed target_speed;             // MD: nav speed profile
    AreaSweepType       area_sweep_type;            // MD: search pattern for Search
    NpcAggroLevel       aggro_level;                // deepseek: NPC_AGGRO_LEVEL escalation
    NpcCombatState      combat_state;               // deepseek: combat phase sub-state
    MoodIntensity       mood_intensity;             // deepseek: intensity of current mood
    uint8_t             motivation_ticks;           // Batch 13 P13: consecutive ticks on current motivation
    uint32_t            event_ts[MAX_EVENT_TYPES];   // Batch 4: ms when EventType[i] last fired; 0=never
    SuspiciousItemReaction si_reaction;               // Batch 7: NPC reaction decision for SI
    AmbushType             ambush_type;               // Batch 7: subtype of ambush approach
    NoiseType              last_noise_type;           // Batch 7: most recent noise sub-type
    uint8_t                alliance_group;            // Batch 15: AllianceGroup cast to uint8 (0=Player)
    CharacterClass         character_class;           // Batch 17: entity archetype for ConditionIsCharacterClass
    BehaviourMoodSet       behaviour_mood_set;        // Batch 18: MD threat-escalation composite state
    ViewconeType           viewcone_type;             // Batch 18: shape of visual detection frustum
    SensoryType            last_sensory_type;         // Batch 18: channel that last triggered sense activation
    uint32_t               last_searched_ms;          // Batch 26: ms when entity last searched a position; 0=never
    // ── Batch 33 fields ────────────────────────────────────────────────────────
    uint32_t               last_shot_at_ms   = 0;    // ms when this entity last took ranged damage; 0=never
    uint8_t                hp_pct            = 100;  // 0-100 HP ratio; game CombatSystem writes each tick
    uint8_t                move_direction    = 0;    // 0=toward 1=away 2=strafe_left 3=strafe_right
    uint8_t                sched_priority    = 0;    // priority of current schedule-driven motivation
    uint8_t                _pad_b33[1]       = {};
    // Kenshi RE: prison_time @+0x5b — jail sentence countdown (seconds); 0 = free
    float                  prison_timer      = 0.f;
    // ── VBfA crowd-perf fields (RE 2026-07-04) ─────────────────────────────────
    // npc_dormant: external suspend flag — set by area/zone manager to fully
    //   skip BT tick without destroying entity. Cleared by the same system.
    //   Different from IS_SUSPENDED (internal BT state) and MotivationType::Dormant
    //   (ultra-low-rate 19s tick). npc_dormant = true → zero BT cost, zero tick.
    uint8_t  npc_dormant   = 0;
    // npc_tick_cost: per-tick work accumulator; reset in BTSystem phases 1+2.
    //   Incremented each time BT actually runs (after all LOD/dormant gates).
    //   Provides per-entity over-work detection: if ≥ NPC_TICK_COST_MAX, BT skipped.
    //   At normal rates (1 tick/entity/frame) this never fires — acts as a safety
    //   guard against pathological re-entry. VBfA threshold: 2800.
    uint16_t npc_tick_cost = 0;
};

// ── AgentStateSnapshot ────────────────────────────────────────────────────────
// POD snapshot of policy fields in AgentState for M48 persistence and
// interrupt-safe hand-off (MD EntityState snapshot/restore pattern).
// Timers and gauges are intentionally excluded — they reset on restore.
struct AgentStateSnapshot {
    uint64_t       lc_flags;        // lcflags.bits
    uint32_t       entity_state;    // EntityStateFlag bitmask
    MotivationType motivation;
    AwarenessState awareness;
    AlertnessState alertness;
    WithdrawState  withdraw_state;

    static AgentStateSnapshot Capture(const AgentState& s) noexcept {
        AgentStateSnapshot snap{};
        snap.lc_flags      = s.lcflags.bits;
        snap.entity_state  = s.entity_state;
        snap.motivation    = s.motivation;
        snap.awareness     = s.awareness;
        snap.alertness     = s.alertness;
        snap.withdraw_state= s.withdraw_state;
        return snap;
    }

    static void Restore(AgentState& s, const AgentStateSnapshot& snap) noexcept {
        s.lcflags.bits   = snap.lc_flags;
        s.entity_state   = snap.entity_state;
        s.motivation     = snap.motivation;
        s.awareness      = snap.awareness;
        s.alertness      = snap.alertness;
        s.withdraw_state = snap.withdraw_state;
    }
};
static_assert(sizeof(AgentStateSnapshot) == 16, "AgentStateSnapshot must be 16 bytes");

// ── Blackboard helpers ────────────────────────────────────────────────────────
// All helpers operate on AgentBlackboard (cold component), not AgentState.

inline BlackboardEntry* bb_find(AgentBlackboard& s, uint32_t key) noexcept {
    for (int i = 0; i < s.bb_count; ++i)
        if (s.bb[i].key == key) return &s.bb[i];
    return nullptr;
}

inline const BlackboardEntry* bb_find(const AgentBlackboard& s, uint32_t key) noexcept {
    for (int i = 0; i < s.bb_count; ++i)
        if (s.bb[i].key == key) return &s.bb[i];
    return nullptr;
}

inline BlackboardEntry* bb_insert(AgentBlackboard& s, uint32_t key, uint8_t type) noexcept {
    BlackboardEntry* e = bb_find(s, key);
    if (e) return e;
    if (s.bb_count >= MAX_BB_ENTRIES) {
        MD_LOG(MD_LOG_WARNING, "[AgentBlackboard] full (MAX_BB_ENTRIES=%d) — key=%u write dropped",
               MAX_BB_ENTRIES, key);
        return nullptr;
    }
    e = &s.bb[s.bb_count++];
    e->key  = key;
    e->type = type;
    return e;
}

inline void bb_set_float(AgentBlackboard& s, uint32_t key, float val) noexcept {
    if (BlackboardEntry* e = bb_insert(s, key, 2)) { e->val.f = val; }
}

inline void bb_set_bool(AgentBlackboard& s, uint32_t key, bool val) noexcept {
    if (BlackboardEntry* e = bb_insert(s, key, 0)) { e->val.b = val; }
}

inline void bb_set_int(AgentBlackboard& s, uint32_t key, int32_t val) noexcept {
    if (BlackboardEntry* e = bb_insert(s, key, 1)) { e->val.i = val; }
}

inline float bb_get_float(const AgentBlackboard& s, uint32_t key, float def = 0.f) noexcept {
    for (int i = 0; i < s.bb_count; ++i)
        if (s.bb[i].key == key) return s.bb[i].val.f;
    return def;
}

inline bool bb_get_bool(const AgentBlackboard& s, uint32_t key, bool def = false) noexcept {
    for (int i = 0; i < s.bb_count; ++i)
        if (s.bb[i].key == key) return s.bb[i].val.b;
    return def;
}
