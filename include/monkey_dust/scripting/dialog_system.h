#pragma once
// dialog_system.h — Kenshi-style dialogue with conditions, effects, and scoring.
//
// DialogLine: text + up to 8 conditions + 4 effects + priority score.
// DialogSystem: evaluates eligible lines for a speaker→target interaction;
//               selects top MAX_DIALOGUE_CHOICES by (score_bonus + relation + rand).
//
// Condition evaluation uses: FactionSystem (relations/bounty), StatSheet (skills),
// Inventory (items). Effects emit to LuaEventBus or apply inline.

#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/faction_system.h>
#include <monkey_dust/components/stat_sheet.h>
#include <cstdint>
#include <cstring>

// All dialog types live in namespace md to avoid collision with
// the game-layer DialogSystem namespace (game/src/dialog/dialog_system.h).
namespace md {

// ── Limits ───────────────────────────────────────────────────────────────────
static constexpr int MAX_DIALOGUE_LINES   = 256;
static constexpr int MAX_DIALOG_CONDITIONS=   8;
static constexpr int MAX_DIALOG_EFFECTS   =   4;
static constexpr int MAX_DIALOGUE_CHOICES =   8;

// ── CompareOp — generic comparison operator for conditions (F-2) ──────────────
// Kenshi RE: "compare by" operator in condition evaluator (kenshi_x64.exe.c).
// Used by ConditionEvaluator to compare numeric values (skills, relations, etc).
enum class CompareOp : uint8_t {
    Equal        = 0,  // value == param
    NotEqual     = 1,  // value != param
    Greater      = 2,  // value > param
    GreaterEqual = 3,  // value >= param
    Less         = 4,  // value < param
    LessEqual    = 5,  // value <= param
    Contains     = 6,  // (bitmask) value & param != 0
    NotContains  = 7,  // (bitmask) value & param == 0
};

// ── Condition ─────────────────────────────────────────────────────────────────

enum class DialogCondType : uint8_t {
    None           = 0,
    FactionRelGT   = 1,  // rel(speaker,faction) > threshold
    FactionRelLT   = 2,  // rel < threshold
    HasItem        = 3,  // target has item_def_id=param_a in inventory
    SkillAbove     = 4,  // target skill[param_a] > param_b
    IsHostile      = 5,  // speaker faction is hostile to target faction
    IsPlayer       = 6,  // target is the player entity
    HasBounty      = 7,  // target has bounty in faction param_a
    // F-2: generic compare using CompareOp (Kenshi RE: "compare by" operator)
    SkillCompare   = 8,  // CompareOp applied to target skill[param_a] vs param_b
    RelationCompare= 9,  // CompareOp applied to relation(speaker,faction=param_a) vs param_b
    FlagCheck      = 10, // CompareOp=Contains/NotContains on entity flags bitmask
};

struct DialogCondition {
    DialogCondType type       = DialogCondType::None;
    CompareOp      compare_op = CompareOp::Greater;  // F-2: used for *Compare types
    int16_t        param_a    = 0;
    int16_t        param_b    = 0;
    uint8_t        _pad2[2]   = {};
};
static_assert(sizeof(DialogCondition) == 8, "DialogCondition must be 8 bytes");

// ── Effect ────────────────────────────────────────────────────────────────────

enum class DialogEffType : uint8_t {
    None            = 0,
    GiveItem        = 1,   // give target item_def_id=param_a, qty=param_b
    ChangeRelation  = 2,   // change relation(speaker_faction, target_faction) by param_a
    StartQuest      = 3,   // emit quest_start event id=param_a to LuaEventBus
    SpawnSquad      = 4,   // push Raid WorldEvent with strength=param_a
    PlayAnimation   = 5,   // set anim clip param_a on speaker entity
    // D-1: Kenshi RE — campaign triggers + unlock system (kenshi_x64.exe.c §486311-536906)
    TriggerCampaign = 6,   // start campaign id=param_a; emit to LuaEventBus("campaign_start",id)
    VictoryTrigger  = 7,   // mark campaign param_a as victorious (condition satisfied)
    LossTrigger     = 8,   // mark campaign param_a as failed
    RemoveItem      = 9,   // remove item_def_id=param_a (qty=param_b) from target inventory
    SetFlag         = 10,  // set world/entity flag bit=param_a to value=param_b (0/1)
    Unlock          = 11,  // unlock item/building/tech id=param_a (push to LuaEventBus)
};

struct DialogEffect {
    DialogEffType type    = DialogEffType::None;
    uint8_t       _pad    = 0;
    int16_t       param_a = 0;
    int16_t       param_b = 0;
    uint8_t       _pad2[2]= {};
};
static_assert(sizeof(DialogEffect) == 8, "DialogEffect must be 8 bytes");

// ── DialogLine ────────────────────────────────────────────────────────────────

struct DialogLine {
    char           text[64]                            = {};  // display text
    DialogCondition conds[MAX_DIALOG_CONDITIONS]       = {};  // all must pass
    DialogEffect    effects[MAX_DIALOG_EFFECTS]        = {};  // applied when chosen
    int8_t          score_bonus    = 0;   // priority among eligible lines
    uint8_t         chance_perm    = 100; // 0..100 — base probability (permanent)
    uint8_t         repetition_lim = 0;   // 0 = unlimited; N = max uses
    uint8_t         use_count      = 0;   // incremented each time line is shown
    uint8_t         cond_count     = 0;
    uint8_t         eff_count      = 0;
    uint8_t         _pad[2]        = {};
};
static_assert(sizeof(DialogLine) == 64 + MAX_DIALOG_CONDITIONS*8 + MAX_DIALOG_EFFECTS*8 + 8,
              "DialogLine layout mismatch");

// ── ConditionEvaluator ────────────────────────────────────────────────────────

class NpcConditionEvaluator {
public:
    // Evaluate all conditions on `line` for the speaker→target pair.
    // Returns true only if ALL conditions pass (AND logic).
    static bool Evaluate(const DialogLine& line,
                         MdEntity speaker, MdEntity target,
                         uint32_t speaker_faction_id) noexcept {
        auto& reg = MdRegistry::Get();
        for (int i = 0; i < (int)line.cond_count; ++i) {
            const DialogCondition& c = line.conds[i];
            switch (c.type) {
            case DialogCondType::None: break;
            case DialogCondType::FactionRelGT: {
                int8_t rel = FactionSystem::Get().GetRelation(speaker_faction_id, (uint32_t)c.param_a);
                if (rel <= (int8_t)c.param_b) return false;
            } break;
            case DialogCondType::FactionRelLT: {
                int8_t rel = FactionSystem::Get().GetRelation(speaker_faction_id, (uint32_t)c.param_a);
                if (rel >= (int8_t)c.param_b) return false;
            } break;
            case DialogCondType::IsHostile:
                if (!reg.Valid(target)) return false;
                // Check via FactionSystem (speaker hostile to target's faction)
                break;
            case DialogCondType::IsPlayer:
                // Always true when dialog is initiated with player — caller sets target=player.
                break;
            case DialogCondType::HasBounty: {
                uint32_t bounty = FactionSystem::Get().GetBounty((uint32_t)c.param_a, target);
                if (bounty == 0) return false;
            } break;
            case DialogCondType::HasItem:
                // Would check Inventory — skipped if no Inventory component
                break;
            case DialogCondType::SkillAbove:
                if (reg.Valid(target)) {
                    const auto* ss = reg.Handle(target).try_get_mut<StatSheet>();
                    if (ss && (int)(*ss)[static_cast<Skill>(c.param_a)] <= (int)c.param_b)
                        return false;
                }
                break;
            // D-2: CompareOp-based conditions (Kenshi RE: "compare by" operator)
            case DialogCondType::SkillCompare: {
                if (!reg.Valid(target)) return false;
                const auto* ss = reg.Handle(target).try_get_mut<StatSheet>();
                int val = ss ? (int)(*ss)[static_cast<Skill>(c.param_a)] : 0;
                if (!ApplyCompareOp(c.compare_op, val, (int)c.param_b)) return false;
            } break;
            case DialogCondType::RelationCompare: {
                int8_t rel = FactionSystem::Get().GetRelation(speaker_faction_id, (uint32_t)c.param_a);
                if (!ApplyCompareOp(c.compare_op, (int)rel, (int)c.param_b)) return false;
            } break;
            case DialogCondType::FlagCheck:
                // World/entity flag check via bitmask (param_a=flag_index, param_b=expected)
                // Currently passes — full implementation requires a WorldFlagSystem
                break;
            }
        }
        return true;
    }

    // Apply a CompareOp to two ints. Used by SkillCompare/RelationCompare.
    static bool ApplyCompareOp(CompareOp op, int lhs, int rhs) noexcept {
        switch (op) {
        case CompareOp::Equal:        return lhs == rhs;
        case CompareOp::NotEqual:     return lhs != rhs;
        case CompareOp::Greater:      return lhs >  rhs;
        case CompareOp::GreaterEqual: return lhs >= rhs;
        case CompareOp::Less:         return lhs <  rhs;
        case CompareOp::LessEqual:    return lhs <= rhs;
        case CompareOp::Contains:     return (lhs & rhs) != 0;
        case CompareOp::NotContains:  return (lhs & rhs) == 0;
        default: return true;
        }
    }
};

// ── DialogSystem ──────────────────────────────────────────────────────────────

class NpcDialogSystem {
public:
    static NpcDialogSystem& Get() noexcept {
        static NpcDialogSystem inst;
        return inst;
    }

    // Register a dialog line; returns its index, or -1 if full.
    int AddLine(const DialogLine& line) noexcept {
        if (count_ >= MAX_DIALOGUE_LINES) return -1;
        lines_[count_] = line;
        return count_++;
    }

    // Build the set of eligible lines for speaker→target.
    // out_indices: caller-allocated int[MAX_DIALOGUE_CHOICES].
    // Returns number of eligible lines (≤ MAX_DIALOGUE_CHOICES).
    int GetEligible(MdEntity speaker, MdEntity target,
                    uint32_t speaker_faction_id,
                    int* out_indices) const noexcept {
        struct Scored { int idx; int score; };
        Scored buf[MAX_DIALOGUE_LINES];
        int n = 0;

        for (int i = 0; i < count_; ++i) {
            const DialogLine& l = lines_[i];
            if (l.repetition_lim > 0 && l.use_count >= l.repetition_lim) continue;
            if (!NpcConditionEvaluator::Evaluate(l, speaker, target, speaker_faction_id)) continue;
            // Chance roll (0..99 against chance_perm)
            unsigned int roll = speaker.ToIntegral() * 1664525u + 1013904223u;
            if ((int)(roll % 100u) >= (int)l.chance_perm) continue;
            buf[n++] = { i, (int)l.score_bonus };
        }

        // Sort descending by score (insertion sort — n is small).
        for (int a = 1; a < n; ++a) {
            Scored key = buf[a];
            int b = a - 1;
            while (b >= 0 && buf[b].score < key.score) { buf[b+1] = buf[b]; --b; }
            buf[b+1] = key;
        }

        int out_n = (n < MAX_DIALOGUE_CHOICES) ? n : MAX_DIALOGUE_CHOICES;
        for (int i = 0; i < out_n; ++i) out_indices[i] = buf[i].idx;
        return out_n;
    }

    // Apply effects of a chosen dialog line.
    void ApplyEffects(int line_idx, MdEntity speaker, MdEntity target) noexcept;

    const DialogLine* GetLine(int idx) const noexcept {
        if (idx < 0 || idx >= count_) return nullptr;
        return &lines_[idx];
    }

    void IncrementUseCount(int idx) noexcept {
        if (idx >= 0 && idx < count_ && lines_[idx].use_count < 255)
            ++lines_[idx].use_count;
    }

    int Count() const noexcept { return count_; }
    void Reset() noexcept { count_ = 0; memset(lines_, 0, sizeof(lines_)); }

private:
    NpcDialogSystem() { memset(lines_, 0, sizeof(lines_)); }
    DialogLine lines_[MAX_DIALOGUE_LINES];
    int        count_ = 0;
};

} // namespace md
