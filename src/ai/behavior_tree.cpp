#include <monkey_dust/ai/behavior_tree.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/bt_components.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/components/npc_memory.h>
#include <monkey_dust/ai/squad_signal.h>
#include <monkey_dust/ai/fnv.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>

BehaviorTree::BehaviorTree() {
    memset(m_nodes,    0, sizeof(m_nodes));
    memset(m_children, 0, sizeof(m_children));
    memset(m_state,    0, sizeof(m_state));
}

uint16_t BehaviorTree::allocNodeIdx() {
    if (m_nodeCount >= MAX_NODES) {
        MD_LOG(MD_LOG_WARNING, "[BehaviorTree] MAX_NODES(%u) exceeded -- "
               "reusing last node slot instead of corrupting memory",
               (unsigned)MAX_NODES);
        return MAX_NODES - 1;
    }
    return m_nodeCount++;
}

void initNode(BTNode& n, BTNodeType t) {
    n.type       = t;
    n.flags      = 0;
    n.parent     = BehaviorTree::INVALID;
    n.childStart = 0;
    n.childCount = 0;
    n.data       = 0;
    n._padding   = nullptr;
}

// ── M21 factory implementations ───────────────────────────────────────────────

uint16_t BehaviorTree::addBranch(BTConditionFunc cond, BranchType btype, ShutdownSpeed speed) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::Branch);
    m_nodes[i].condition = cond;
    m_nodes[i].data      = static_cast<uint32_t>(btype);
    m_nodes[i].flags     = static_cast<uint8_t>(speed);
    return i;
}
uint16_t BehaviorTree::addTimerStart(uint8_t timer_id, uint32_t duration_ms) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::TimerStart);
    m_nodes[i].data = (static_cast<uint32_t>(timer_id) << 24) | (duration_ms & 0x00FFFFFFu);
    return i;
}
uint16_t BehaviorTree::addTimerCheck(uint8_t timer_id) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::TimerCheck);
    m_nodes[i].data = timer_id & 0x1Fu;  // 5 bits: 0-31 (covers all 26 slots)
    return i;
}
// Pattern 4: bit_idx = lcf::* constant (0-63)
uint16_t BehaviorTree::addFlagCheck(uint8_t bit_idx, bool check_set) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::FlagCheck);
    m_nodes[i].data  = bit_idx & 0x3Fu;  // 6 bits: 0-63
    m_nodes[i].flags = check_set ? 0u : 1u;
    return i;
}
uint16_t BehaviorTree::addFlagSet(uint8_t bit_idx, bool do_set) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::FlagSet);
    m_nodes[i].data  = bit_idx & 0x3Fu;
    m_nodes[i].flags = do_set ? 0u : 1u;
    return i;
}
uint16_t BehaviorTree::addSenseCheck(uint8_t sense_idx, float threshold) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SenseCheck);
    uint32_t t_fixed = static_cast<uint32_t>(threshold * 1000.f + 0.5f);
    m_nodes[i].data  = (static_cast<uint32_t>(sense_idx & 0xFu) << 24) | (t_fixed & 0x00FFFFFFu);
    return i;
}

// ── Pattern 1: MotivationCheck / SetMotivation ────────────────────────────────
uint16_t BehaviorTree::addMotivationCheck(MotivationType mot) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::MotivationCheck);
    m_nodes[i].data = static_cast<uint32_t>(mot);
    return i;
}
uint16_t BehaviorTree::addSetMotivation(MotivationType mot) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SetMotivation);
    m_nodes[i].data = static_cast<uint32_t>(mot);
    return i;
}

// ── Pattern 3: Reference ──────────────────────────────────────────────────────
uint16_t BehaviorTree::addReference(BehaviorTree* other, uint32_t name_hash) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::Reference);
    m_nodes[i]._padding = other;
    m_nodes[i].data     = name_hash;
    return i;
}

void BehaviorTree::PatchReference(uint32_t name_hash, BehaviorTree* target) noexcept {
    for (uint16_t i = 0; i < m_nodeCount; ++i) {
        if (m_nodes[i].type == BTNodeType::Reference && m_nodes[i].data == name_hash)
            m_nodes[i]._padding = target;
    }
}

// ── Pattern 6: GaugeCheck / GaugeSet ─────────────────────────────────────────
uint16_t BehaviorTree::addGaugeCheck(GaugeType gauge, float threshold) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::GaugeCheck);
    uint32_t t_fixed = static_cast<uint32_t>(threshold * 1000.f + 0.5f);
    m_nodes[i].data  = (static_cast<uint32_t>(gauge) << 24) | (t_fixed & 0x00FFFFFFu);
    return i;
}
uint16_t BehaviorTree::addGaugeSet(GaugeType gauge, float value) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::GaugeSet);
    uint32_t v_fixed = static_cast<uint32_t>(value * 1000.f + 0.5f);
    m_nodes[i].data  = (static_cast<uint32_t>(gauge) << 24) | (v_fixed & 0x00FFFFFFu);
    return i;
}

// ── Extended AI node types ───────────────────────────────────────────────────

// C11
uint16_t BehaviorTree::addSequenceStateless() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SequenceStateless);
    return i;
}
uint16_t BehaviorTree::addSequenceIgnoreChildFail() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SequenceIgnoreChildFail);
    return i;
}

// C12: same as addTimerStart but sets flag bit 0 (only_increase)
uint16_t BehaviorTree::addTimerStartOnlyIncrease(uint8_t timer_id, uint32_t duration_ms) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::TimerStart);
    m_nodes[i].data  = (static_cast<uint32_t>(timer_id) << 24) | (duration_ms & 0x00FFFFFFu);
    m_nodes[i].flags = 0x01u; // only_increase
    return i;
}

// C13
uint16_t BehaviorTree::addFrameFlagCheck(uint8_t bit_idx, bool check_set) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::FrameFlagCheck);
    m_nodes[i].data  = bit_idx & 0x3Fu;
    m_nodes[i].flags = check_set ? 0u : 1u;
    return i;
}
uint16_t BehaviorTree::addFrameFlagSet(uint8_t bit_idx, bool do_set) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::FrameFlagSet);
    m_nodes[i].data  = bit_idx & 0x3Fu;
    m_nodes[i].flags = do_set ? 0u : 1u;
    return i;
}

// C14: weights[0..3] packed as uint8 into data
uint16_t BehaviorTree::addWeightedSelector(const uint8_t weights[4]) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::WeightedSelector);
    m_nodes[i].data = static_cast<uint32_t>(weights[0])
                    | (static_cast<uint32_t>(weights[1]) <<  8)
                    | (static_cast<uint32_t>(weights[2]) << 16)
                    | (static_cast<uint32_t>(weights[3]) << 24);
    return i;
}

// C15
uint16_t BehaviorTree::addAwarenessCheck(AwarenessState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::AwarenessCheck);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}

// C16
uint16_t BehaviorTree::addAlertnessCheck(AlertnessState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::AlertnessCheck);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}

// C17
uint16_t BehaviorTree::addMoodCheck(NpcMood mood) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::MoodCheck);
    m_nodes[i].data = static_cast<uint32_t>(mood);
    return i;
}

// C18
uint16_t BehaviorTree::addRoleCheck(NpcRole role, bool check_could_perform) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::RoleCheck);
    m_nodes[i].data = (static_cast<uint32_t>(role) << 8) | (check_could_perform ? 1u : 0u);
    return i;
}
uint16_t BehaviorTree::addRoleClaim(NpcRole role, uint32_t query_id) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::RoleClaim);
    m_nodes[i].data = (query_id << 8) | (static_cast<uint32_t>(role) & 0xFFu);
    return i;
}
uint16_t BehaviorTree::addRoleRelease(NpcRole role) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::RoleRelease);
    m_nodes[i].data = static_cast<uint32_t>(role);
    return i;
}

// C19
uint16_t BehaviorTree::addWithdrawCheck(WithdrawState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::WithdrawCheck);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}
uint16_t BehaviorTree::addSetWithdraw(WithdrawState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SetWithdraw);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}

uint16_t BehaviorTree::addMemoryCheck(uint8_t mode) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::MemoryCheck);
    m_nodes[i].data = mode;
    return i;
}

uint16_t BehaviorTree::addMemoryForget() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::MemoryForget);
    return i;
}

uint16_t BehaviorTree::addAreaSweepCheck(AreaSweepType type) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::AreaSweepCheck);
    m_nodes[i].data = static_cast<uint32_t>(type);
    return i;
}

uint16_t BehaviorTree::addDecoratorPercentage(uint8_t pct) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::DecoratorPercentage);
    m_nodes[i].data = pct;
    return i;
}

uint16_t BehaviorTree::addSelectorPercentage() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SelectorPercentage);
    return i;
}

uint16_t BehaviorTree::addSenseTimeCheck(uint8_t sense_idx, uint32_t max_elapsed_ms) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SenseTimeCheck);
    m_nodes[i].data = (static_cast<uint32_t>(sense_idx) << 24) | (max_elapsed_ms & 0x00FFFFFFu);
    return i;
}

uint16_t BehaviorTree::addActionSetDead() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionSetDead);
    return i;
}

uint16_t BehaviorTree::addActionDespawn() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionDespawn);
    return i;
}

// MD_deepseek: AggroLevelCheck / SetAggroLevel / NpcCombatStateCheck / SetNpcCombatState
uint16_t BehaviorTree::addAggroLevelCheck(NpcAggroLevel level) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::AggroLevelCheck);
    m_nodes[i].data = static_cast<uint32_t>(level);
    return i;
}
uint16_t BehaviorTree::addSetAggroLevel(NpcAggroLevel level) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SetAggroLevel);
    m_nodes[i].data = static_cast<uint32_t>(level);
    return i;
}
uint16_t BehaviorTree::addNpcCombatStateCheck(NpcCombatState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::NpcCombatStateCheck);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}
uint16_t BehaviorTree::addSetNpcCombatState(NpcCombatState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SetNpcCombatState);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}

// ── MD_z factory implementations ────────────────────────────────────────

uint16_t BehaviorTree::addDecoratorMood(NpcMood mood) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::DecoratorMood);
    m_nodes[i].data = static_cast<uint32_t>(mood);
    return i;
}

uint16_t BehaviorTree::addDecoratorAwareness(AwarenessState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::DecoratorAwareness);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}

uint16_t BehaviorTree::addDecoratorTimerAuto(uint8_t timer_id, uint32_t duration_ms,
                                              bool only_increase) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::DecoratorTimerAuto);
    m_nodes[i].data  = (static_cast<uint32_t>(timer_id) << 24) | (duration_ms & 0x00FFFFFFu);
    m_nodes[i].flags = only_increase ? 0x01u : 0x00u;
    return i;
}

uint16_t BehaviorTree::addActionTimerRandom(uint8_t timer_id, uint32_t min_ms, uint32_t max_ms) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionTimerRandom);
    // Round to nearest 100ms unit (12 bits each, max 4095*100=409,500ms)
    uint32_t min_u = (min_ms / 100u) & 0xFFFu;
    uint32_t max_u = (max_ms / 100u) & 0xFFFu;
    if (max_u < min_u) max_u = min_u;
    m_nodes[i].data = (static_cast<uint32_t>(timer_id) << 24) | (max_u << 12) | min_u;
    return i;
}

uint16_t BehaviorTree::addActionSquadNotify(SquadSignal signal) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionSquadNotify);
    m_nodes[i].data = static_cast<uint32_t>(signal);
    return i;
}

uint16_t BehaviorTree::addConditionSquadSignal(SquadSignal signal) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionSquadSignal);
    m_nodes[i].data = static_cast<uint32_t>(signal);
    return i;
}

uint16_t BehaviorTree::addConditionAnySenseWithinTime(uint32_t time_ms, bool specific,
                                                       uint8_t sense_idx) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionAnySenseWithinTime);
    // data bits 0-27 = time_window_ms; bits 28-31 = sense_idx (0-8)
    m_nodes[i].data  = (time_ms & 0x0FFFFFFFu) | (static_cast<uint32_t>(sense_idx & 0xFu) << 28);
    m_nodes[i].flags = specific ? 1u : 0u;
    return i;
}

uint16_t BehaviorTree::addActionExpireTimer(uint8_t timer_id) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionExpireTimer);
    m_nodes[i].data = timer_id & 0x1Fu;
    return i;
}

uint16_t BehaviorTree::addTargetFlagCheck(uint8_t bit_idx, bool check_set) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::TargetFlagCheck);
    m_nodes[i].data  = bit_idx & 0x3Fu;
    m_nodes[i].flags = check_set ? 0u : 1u;
    return i;
}

uint16_t BehaviorTree::addSetLocomotionState(LocomotionState state) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::SetLocomotionState);
    m_nodes[i].data = static_cast<uint32_t>(state);
    return i;
}

// MD_arch Pattern 4: DecoratorNamedBranch
uint16_t BehaviorTree::addDecoratorNamedBranch(uint32_t name_hash, bool inverted) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::DecoratorNamedBranch);
    m_nodes[i].data  = name_hash;
    m_nodes[i].flags = inverted ? 1u : 0u;
    return i;
}
uint16_t BehaviorTree::addDecoratorNamedBranch(const char* name, bool inverted) {
    return addDecoratorNamedBranch(md::fnv1a_rt(name), inverted);
}

// ── MD_grok factories ────────────────────────────────────────────────────

uint16_t BehaviorTree::addActionIdleTime(uint32_t duration_ms) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionIdleTime);
    m_nodes[i].data = duration_ms;
    return i;
}
uint16_t BehaviorTree::addConditionHaveTarget() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionHaveTarget);
    return i;
}
uint16_t BehaviorTree::addConditionHaveNextTarget() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionHaveNextTarget);
    return i;
}
uint16_t BehaviorTree::addActionTriggerSound(NpcSoundEvent ev) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionTriggerSound);
    m_nodes[i].data = static_cast<uint32_t>(ev);
    return i;
}
uint16_t BehaviorTree::addConditionIsSenseActivationAbove(uint8_t sense_idx, SenseThresholdQualifier q) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionIsSenseActivationAbove);
    m_nodes[i].data = (static_cast<uint32_t>(sense_idx) << 8) | static_cast<uint32_t>(q);
    return i;
}
uint16_t BehaviorTree::addConditionHasAnySenseBeenAbove(SenseThresholdQualifier q) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionHasAnySenseBeenAbove);
    m_nodes[i].data = static_cast<uint32_t>(q);
    return i;
}
uint16_t BehaviorTree::addActionSuspiciousItemDoneStage(SuspiciousItemStage stage) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ActionSuspiciousItemDoneStage);
    m_nodes[i].data = static_cast<uint32_t>(stage);
    return i;
}
uint16_t BehaviorTree::addConditionSuspiciousItemBTPriority(uint8_t min_intensity) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionSuspiciousItemBTPriority);
    m_nodes[i].data = min_intensity;
    return i;
}
uint16_t BehaviorTree::addConditionLastTimeSquadNotified(SquadSignal signal, uint8_t time_threshold_s) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::ConditionLastTimeSquadNotified);
    m_nodes[i].data = (static_cast<uint32_t>(signal) << 8) | static_cast<uint32_t>(time_threshold_s);
    return i;
}
uint16_t BehaviorTree::addDecoratorAggressionEscalation() {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::DecoratorAggressionEscalation);
    return i;
}

// ── Existing factories ────────────────────────────────────────────────────────

uint16_t BehaviorTree::addSelector()  { uint16_t i = allocNodeIdx(); initNode(m_nodes[i], BTNodeType::Selector);  return i; }
uint16_t BehaviorTree::addSequence()  { uint16_t i = allocNodeIdx(); initNode(m_nodes[i], BTNodeType::Sequence);  return i; }
uint16_t BehaviorTree::addInverter()  { uint16_t i = allocNodeIdx(); initNode(m_nodes[i], BTNodeType::Inverter);  return i; }

uint16_t BehaviorTree::addRepeat(uint32_t count) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::Repeat);
    m_nodes[i].data = count;
    return i;
}
uint16_t BehaviorTree::addWait(uint32_t ms) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::Wait);
    m_nodes[i].data = ms;
    return i;
}
uint16_t BehaviorTree::addCondition(BTConditionFunc func) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::Condition);
    m_nodes[i].condition = func;
    return i;
}
uint16_t BehaviorTree::addAction(BTActionFunc func) {
    uint16_t i = allocNodeIdx();
    initNode(m_nodes[i], BTNodeType::Action);
    m_nodes[i].action = func;
    return i;
}

void BehaviorTree::addChild(uint16_t parent, uint16_t child) {
    if (m_childCount >= MAX_CHILDREN) {
        MD_LOG(MD_LOG_WARNING, "[BehaviorTree] MAX_CHILDREN(%u) exceeded -- "
               "child link dropped instead of corrupting memory",
               (unsigned)MAX_CHILDREN);
        return;
    }
    BTNode& pn = m_nodes[parent];
    if (pn.childCount == 0)
        pn.childStart = m_childCount;
    m_children[m_childCount++] = child;
    pn.childCount++;
    m_nodes[child].parent = parent;
}

void BehaviorTree::setRoot(uint16_t node) { m_root = node; }

void BehaviorTree::reset() {
    memset(m_state, 0, sizeof(BTState) * m_nodeCount);
}

// VM tick is in behavior_tree_vm.cpp
