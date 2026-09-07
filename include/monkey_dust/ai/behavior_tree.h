#pragma once
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/ai/bt_types.h>

struct AgentState;

// Defined in behavior_tree.cpp; declared here (not just relied on via Unity
// Build TU-merging, which bt_factories.cpp's "addXxx()" factory methods
// used to depend on implicitly — broke when adding engine/src/ai/
// ai_queries.cpp shifted the Unity batch split and put the two files in
// different translation units).
void initNode(BTNode& n, BTNodeType t);

class BehaviorTree {
public:
    static constexpr uint16_t INVALID      = 0xFFFF;
    static constexpr uint16_t MAX_NODES    = 128;
    static constexpr uint16_t MAX_CHILDREN = 256;

    BehaviorTree();

    uint16_t addSelector ();
    uint16_t addSequence ();
    uint16_t addInverter ();
    uint16_t addRepeat   (uint32_t count);  // 0 = infinite
    uint16_t addWait     (uint32_t ms);
    uint16_t addCondition(BTConditionFunc func);
    uint16_t addAction   (BTActionFunc    func);
    // M21 extensions
    uint16_t addBranch    (BTConditionFunc cond,
                           BranchType btype    = BranchType::Standard,
                           ShutdownSpeed speed = ShutdownSpeed::Graceful);
    uint16_t addTimerStart(uint8_t timer_id, uint32_t duration_ms);
    uint16_t addTimerCheck(uint8_t timer_id);
    // Pattern 4: bit_idx = lcf::* constant (0-63)
    uint16_t addFlagCheck (uint8_t bit_idx, bool check_set = true);
    uint16_t addFlagSet   (uint8_t bit_idx, bool do_set    = true);
    uint16_t addSenseCheck(uint8_t sense_idx, float threshold);
    // Pattern 1: MotivationCheck / SetMotivation
    uint16_t addMotivationCheck(MotivationType mot);
    uint16_t addSetMotivation  (MotivationType mot);
    // Pattern 3: Reference
    uint16_t addReference(BehaviorTree* other, uint32_t name_hash = 0);
    void PatchReference(uint32_t name_hash, BehaviorTree* target) noexcept;
    // Pattern 6: GaugeCheck / GaugeSet
    uint16_t addGaugeCheck(GaugeType gauge, float threshold);
    uint16_t addGaugeSet  (GaugeType gauge, float value);

    // C11
    uint16_t addSequenceStateless();
    // C12
    uint16_t addTimerStartOnlyIncrease(uint8_t timer_id, uint32_t duration_ms);
    // C13
    uint16_t addFrameFlagCheck(uint8_t bit_idx, bool check_set = true);
    uint16_t addFrameFlagSet  (uint8_t bit_idx, bool do_set    = true);
    // C14
    uint16_t addWeightedSelector(const uint8_t weights[4]);
    // C15
    uint16_t addAwarenessCheck(AwarenessState state);
    // C16
    uint16_t addAlertnessCheck(AlertnessState state);
    // C17
    uint16_t addMoodCheck(NpcMood mood);
    // C18
    uint16_t addRoleCheck  (NpcRole role, bool check_could_perform = false);
    uint16_t addRoleClaim  (NpcRole role, uint32_t query_id);
    uint16_t addRoleRelease(NpcRole role);
    // C19
    uint16_t addWithdrawCheck(WithdrawState state);
    uint16_t addSetWithdraw  (WithdrawState state);
    // Echo NpcMemory nodes
    uint16_t addMemoryCheck (uint8_t mode);
    uint16_t addMemoryForget();
    // MdDump
    uint16_t addAreaSweepCheck(AreaSweepType type);
    // MD_gemini
    uint16_t addDecoratorPercentage(uint8_t pct);
    uint16_t addSelectorPercentage();
    uint16_t addSenseTimeCheck(uint8_t sense_idx, uint32_t max_elapsed_ms);
    uint16_t addActionSetDead();
    uint16_t addActionDespawn();
    // MD_deepseek
    uint16_t addAggroLevelCheck    (NpcAggroLevel level);
    uint16_t addSetAggroLevel      (NpcAggroLevel level);
    uint16_t addNpcCombatStateCheck(NpcCombatState state);
    uint16_t addSetNpcCombatState  (NpcCombatState state);
    // MD_z
    uint16_t addDecoratorMood      (NpcMood mood);
    uint16_t addDecoratorAwareness (AwarenessState state);
    uint16_t addDecoratorTimerAuto (uint8_t timer_id, uint32_t duration_ms,
                                    bool only_increase = false);
    uint16_t addActionTimerRandom  (uint8_t timer_id, uint32_t min_ms, uint32_t max_ms);
    uint16_t addActionSquadNotify  (SquadSignal signal);
    uint16_t addConditionSquadSignal(SquadSignal signal);
    uint16_t addConditionAnySenseWithinTime(uint32_t time_ms, bool specific = false,
                                            uint8_t sense_idx = 0);
    uint16_t addActionExpireTimer  (uint8_t timer_id);
    uint16_t addTargetFlagCheck    (uint8_t bit_idx, bool check_set = true);
    uint16_t addSetLocomotionState (LocomotionState state);
    // MD_arch Pattern 4
    uint16_t addDecoratorNamedBranch(const char* branch_name, bool inverted = false);
    uint16_t addDecoratorNamedBranch(uint32_t name_hash,      bool inverted = false);
    // MD_grok
    uint16_t addActionIdleTime(uint32_t duration_ms);
    uint16_t addConditionHaveTarget();
    uint16_t addConditionHaveNextTarget();
    uint16_t addActionTriggerSound(NpcSoundEvent ev);
    uint16_t addConditionIsSenseActivationAbove(uint8_t sense_idx, SenseThresholdQualifier q);
    uint16_t addConditionHasAnySenseBeenAbove(SenseThresholdQualifier q);
    uint16_t addActionSuspiciousItemDoneStage(SuspiciousItemStage stage);
    uint16_t addConditionSuspiciousItemBTPriority(uint8_t min_intensity);
    uint16_t addConditionLastTimeSquadNotified(SquadSignal signal, uint8_t time_threshold_s);
    uint16_t addDecoratorAggressionEscalation();

    // ── Batch 2 factories ─────────────────────────────────────────────────────
    uint16_t addConditionIsDead();
    uint16_t addConditionIsInVent(uint8_t char_type = 0);
    uint16_t addConditionCanBreakout();
    uint16_t addConditionIsBackstage();
    uint16_t addConditionIsPartOfNPCGroup();
    uint16_t addConditionAnotherAlienIsAttacking();
    uint16_t addConditionHasSearchedPos();
    uint16_t addConditionHasDoneSuspectMoveTo();
    uint16_t addActionSwitchToNextTarget();
    uint16_t addActionDoneSystematicSearch();

    // ── Batch 4 ───────────────────────────────────────────────────────────────
    uint16_t addConditionEventAOccuredAfterB(EventType a, EventType b);

    // ── Batch 5 ───────────────────────────────────────────────────────────────
    uint16_t addConditionSquadDoingEscalation();
    uint16_t addConditionSquadDoingSuspiciousWarning();
    uint16_t addDecoratorSquadSearch();

    // ── Batch 6 ───────────────────────────────────────────────────────────────
    uint16_t addConditionSuspiciousItemShouldDoStage(SuspiciousItemStage stage);
    uint16_t addConditionSuspiciousItemIsWithinDistance(float max_dist_m);
    uint16_t addConditionSuspiciousItemFirstGroupMember();
    uint16_t addConditionSuspiciousItemGroupAllowedToProgress();
    uint16_t addConditionSuspiciousItemGroupMembersRoutingTo();
    uint16_t addConditionSuspiciousItemWaitForGroupRouting();

    // ── Batch 7 ───────────────────────────────────────────────────────────────
    uint16_t addSIReactionCheck (SuspiciousItemReaction reaction);
    uint16_t addSetSIReaction   (SuspiciousItemReaction reaction);
    uint16_t addAmbushTypeCheck (AmbushType type);
    uint16_t addSetAmbushType   (AmbushType type);
    uint16_t addNoiseTypeCheck  (NoiseType type);
    uint16_t addSetNoiseType    (NoiseType type);

    // ── Batch 8 ───────────────────────────────────────────────────────────────
    uint16_t addConditionSuspiciousItemValid();
    uint16_t addActionConsumeSuspiciousItem();
    uint16_t addActionForceMoveToSI();

    // ── Batch 9 ───────────────────────────────────────────────────────────────
    uint16_t addRelationshipTrustCheck(uint8_t threshold, uint8_t mode = 0);
    uint16_t addRelationshipFearCheck (uint8_t threshold, uint8_t mode = 0);

    // ── Batch 10 ──────────────────────────────────────────────────────────────
    uint16_t addConditionIsAnySenseActivationAbove(SenseThresholdQualifier q);
    uint16_t addActionMoveThroughTarget(LocomotionTargetSpeed speed = LocomotionTargetSpeed::Fast);
    uint16_t addActionAdjustMenace(float delta, uint8_t mode = 0);

    // ── Batch 11 ──────────────────────────────────────────────────────────────
    uint16_t addConditionAngleToTarget(float angle_degrees);
    uint16_t addConditionShouldSuspend();
    uint16_t addActionSuspendSelf();
    uint16_t addConditionTargetDistLOS(float max_dist_m);

    // ── Batch 12 ──────────────────────────────────────────────────────────────
    uint16_t addConditionTargetRoutingDistance(float max_dist_m);
    uint16_t addConditionAlienIsAllowed(AlienActionType action);
    uint16_t addDecoratorLockVent(uint8_t vent_id);

    // ── Batch 16 ──────────────────────────────────────────────────────────────
    uint16_t addSequenceIgnoreChildFail();

    // ── Batch 15 ──────────────────────────────────────────────────────────────
    uint16_t addConditionIsEnemyOfTarget();
    uint16_t addActionForceIdle();
    uint16_t addConditionHasValidRouteToTarget();

    // ── Batch 17 ──────────────────────────────────────────────────────────────
    uint16_t addConditionIsCharacterClass(CharacterClass cls);
    uint16_t addConditionIsInCover();
    uint16_t addConditionShouldUseCover();
    uint16_t addActionMoveToCover();
    uint16_t addActionIdleInCover();

    // ── Batch 18 ──────────────────────────────────────────────────────────────
    uint16_t addBehaviourMoodSetCheck(BehaviourMoodSet mood_set);
    uint16_t addSetBehaviourMoodSet(BehaviourMoodSet mood_set);
    uint16_t addViewconeTypeCheck(ViewconeType vtype);
    uint16_t addSetViewconeType(ViewconeType vtype);
    uint16_t addSensoryTypeCheck(SensoryType stype);
    uint16_t addSetSensoryType(SensoryType stype);

    // ── Batch 19 ──────────────────────────────────────────────────────────────
    uint16_t addConditionCurrentWeaponIsEquipped();
    uint16_t addConditionCurrentWeaponNeedsReloading();
    uint16_t addConditionHasMeleeAttackAvailable();
    uint16_t addActionWeaponEquip();
    uint16_t addActionRangedShoot();
    uint16_t addConditionHasObjective();
    uint16_t addConditionBehaviourMoodSetAbove(BehaviourMoodSet threshold);

    // ── Batch 20 ──────────────────────────────────────────────────────────────
    uint16_t addActionSuccess();
    uint16_t addActionFail();
    uint16_t addDecoratorTimer(uint8_t timer_id, uint32_t duration_ms);
    uint16_t addActionIdle();
    uint16_t addConditionTargetIsWithinDistance(float max_dist_m);
    uint16_t addConditionHasAWeapon();
    uint16_t addConditionShouldProcessSuspiciousItem();

    // ── Batch 21 ──────────────────────────────────────────────────────────────
    uint16_t addActionMoveToTarget(LocomotionTargetSpeed speed = LocomotionTargetSpeed::Fast);
    uint16_t addActionMakeAggressive();
    uint16_t addConditionAllowedToAttackTarget();
    uint16_t addActionDead();
    uint16_t addActionMeleeAttack(uint8_t attack_type = 0);
    uint16_t addConditionIsCurrentCoverValid();

    // ── Batch 22 ──────────────────────────────────────────────────────────────
    uint16_t addConditionLastTimeSensed(uint8_t sense_idx, uint32_t max_elapsed_ms);
    uint16_t addActionPerformRole(NpcRole role);
    uint16_t addConditionWasSenseThresholdLastIncreaseActivationAbove(uint8_t sense_idx,
                                                                       SenseThresholdQualifier q);
    uint16_t addConditionTargetsWeaponHasAmmo();
    uint16_t addConditionTargetsWeaponHasProperty(uint8_t weapon_type);
    uint16_t addConditionHasSearchedMostRecentSensedPosition();

    // ── Batch 23 ──────────────────────────────────────────────────────────────
    uint16_t addActionIdleTimeFacingTarget(uint32_t duration_ms);
    uint16_t addActionIdleTimeFacingTargetMostRecentSensedPosition(uint32_t duration_ms);
    uint16_t addActionIdleTimeFacingSuspiciousItem(uint32_t duration_ms);
    uint16_t addActionRangedAim();
    uint16_t addActionSuspiciousItemReaction(SuspiciousItemReaction reaction);
    uint16_t addActionSuspectTargetResponse();

    // ── Batch 24 ──────────────────────────────────────────────────────────────
    uint16_t addActionRequestCover();
    uint16_t addConditionHasValidCoverToChangeTo();

    // ── Batch 25 ──────────────────────────────────────────────────────────────
    uint16_t addConditionHasScript();
    uint16_t addActionScript(uint32_t script_name_hash);

    // ── Batch 26 ──────────────────────────────────────────────────────────────
    uint16_t addConditionLastTimeSearchedWithinTime(uint32_t max_elapsed_ms);
    uint16_t addConditionAllowedToSearch();
    uint16_t addConditionHasDoneSuspectResponseMoveTo();
    uint16_t addConditionHasDoneSuspectResponseWithinTime(uint32_t max_elapsed_ms);
    uint16_t addConditionHasKilltrap();
    uint16_t addConditionHasMeleeAttackAvailableOrIsAttacking();
    uint16_t addConditionIsBranchActive(uint32_t branch_name_hash);
    uint16_t addConditionIsRequestingCover();
    uint16_t addConditionAllowedToPursueTarget();

    // ── Batch 27 ──────────────────────────────────────────────────────────────
    uint16_t addActionBreakout();
    uint16_t addActionMoveToMostRecentSensedPosition();
    uint16_t addActionMoveToNearestStandingPointToTarget();
    uint16_t addActionMoveToAttackTarget();
    uint16_t addActionChangeCover();

    // ── Batch 28 ──────────────────────────────────────────────────────────────
    uint16_t addConditionIsInTargetsWeaponRange(float max_dist_m);
    uint16_t addConditionIsCoverExposed();
    uint16_t addConditionHasLostTarget(uint32_t max_elapsed_ms);
    uint16_t addActionUpdateLastKnownPosition();
    uint16_t addActionRequestInvestigate();
    uint16_t addActionMarkTargetLost();
    uint16_t addActionForceRetreat();
    uint16_t addActionHoldPosition();

    // ── Batch 29 ──────────────────────────────────────────────────────────────
    uint16_t addConditionNpcDevelopmentStageAbove(NpcDevelopmentStage stage);
    uint16_t addConditionNpcHasAbility(uint16_t ability_mask);
    uint16_t addConditionIsHostileToPlayer();
    uint16_t addActionPerformAmbush();
    uint16_t addActionStartSearch();
    uint16_t addActionCallForHelp();
    uint16_t addActionTauntTarget();
    uint16_t addActionSurrenderSelf();

    // ── Batch 30 ──────────────────────────────────────────────────────────────
    uint16_t addConditionEventCountAbove(uint8_t min_count);
    uint16_t addConditionSpatialMemoryCountAbove(uint8_t min_count);
    uint16_t addConditionMotivationTicksAbove(uint8_t min_ticks);
    uint16_t addConditionHasVisualHistory();
    uint16_t addActionPursueTarget();
    uint16_t addActionCircleTarget();
    uint16_t addActionBackOff();
    uint16_t addActionCrouchMove();
    uint16_t addActionVault();

    // ── Batch 31 ──────────────────────────────────────────────────────────────
    uint16_t addConditionHasVentCloseToAlien(float radius_m);
    uint16_t addConditionHasFlankedVentCloseToPlayer(float radius_m);
    uint16_t addConditionTargetIsOnlyAccessibleCrouching();
    uint16_t addConditionAngleNPCToTargetsAimLessThan(float angle_deg);

    // ── Batch 32 ──────────────────────────────────────────────────────────────
    uint16_t addConditionHasToken(uint32_t token_id);
    uint16_t addActionReleaseToken(uint32_t token_id);

    // ── Batch 33 ──────────────────────────────────────────────────────────────
    uint16_t addActionAbortMeleeAttack();
    uint16_t addActionGetOutOfTheWay();
    uint16_t addActionHitTargetAndRun();
    uint16_t addActionMoveInDirection(uint8_t direction = 0);
    uint16_t addActionSuspend();
    uint16_t addActionTakeStep();
    uint16_t addActionThreatAware();
    uint16_t addActionThreatEscalation();
    uint16_t addConditionCanShootNow();
    uint16_t addConditionCheckHealthState(uint8_t threshold_pct);
    uint16_t addConditionHasGroupAwarenessState(AwarenessState state);
    uint16_t addConditionHasMeleeBlockAvailable();
    uint16_t addConditionHasMeleeCounterAttackAvailable();
    uint16_t addConditionLastTimeTargetShotAtMe(uint32_t max_elapsed_ms);
    uint16_t addConditionTargetIsInWeaponRange();
    uint16_t addConditionTargetIsTargetingMe();
    uint16_t addConditionTargetIsUsingMeleeAttack();
    uint16_t addDecoratorLoop(uint32_t count = 0);

    // ── Batch 34 ──────────────────────────────────────────────────────────────
    uint16_t addActionForceSearch();
    uint16_t addActionResetSearchJobs();
    uint16_t addActionSetFrameFlag(uint8_t bit_idx);
    uint16_t addActionSetGaugeAmount(GaugeType gauge, float value);
    uint16_t addConditionHasSenseActivationBeenAbove(uint8_t sense_idx, SenseThresholdQualifier q);
    uint16_t addConditionIsCoverTooClose(float min_dist_m);
    uint16_t addConditionIsInCombatArea();
    uint16_t addConditionMostRecentSenseActivationHasBeenAbove(SenseThresholdQualifier q);
    uint16_t addConditionNeedsToGetOutOfTheWay();
    uint16_t addConditionObjectiveIsInCombatArea();
    uint16_t addConditionObjectiveIsWithinDistance(float max_dist_m);
    uint16_t addConditionShouldFollow();
    uint16_t addConditionTargetIsInCombatArea();
    uint16_t addConditionTargetIsWithinAggroRadius(float radius_m);
    uint16_t addConditionTargetNearestStandPointIsWithinDistance(float max_dist_m);
    uint16_t addDecoratorSetSenseSet(uint8_t sense_set_id);
    uint16_t addDecoratorSuspiciousItemInProgress();
    uint16_t addSelectorLinear();
    uint16_t addSequenceLinear();

    // ── Batch 35 ──────────────────────────────────────────────────────────────
    uint16_t addActionApplyPrimaryDamageControlResponse();
    uint16_t addActionBrokenCover();
    uint16_t addActionMoveToObjective();
    uint16_t addActionSetLogicCharacterFlags(uint8_t bit_idx, bool do_set);
    uint16_t addConditionAllowedToDoSuspiciousWarning();
    uint16_t addConditionCanTakeStep();
    uint16_t addConditionGameIsDifficulty();
    uint16_t addConditionHasAnySenseBeenAboveWithinTime(SenseThresholdQualifier q, uint32_t time_ms);
    uint16_t addConditionIsCorpseTrap();
    uint16_t addConditionIsGaugeAmountBelow(GaugeType gauge, float threshold);
    uint16_t addConditionLastTimeWasAbleToShootTarget(uint32_t max_elapsed_ms);
    uint16_t addConditionRequiresPrimaryDamageControlResponse(uint8_t threshold_pct = 25);
    uint16_t addConditionTargetIsPlayer();
    uint16_t addConditionTargetIsWithinDistanceUnobscured(float max_dist_m);
    uint16_t addConditionTargetLogicCharacterFlags(uint8_t bit_idx, bool check_set = true);

    void addChild(uint16_t parent, uint16_t child);
    void setRoot (uint16_t node);

    bool isValid() const { return m_root != INVALID && m_nodeCount > 0; }

    // as: caller's already-resolved AgentState* for entity e (nullable --
    // AgentState is only present on save-loaded entities, never guaranteed).
    // Threaded through from the caller instead of re-fetched per-node:
    // AgentState was the single largest facade-accessor contributor by
    // STATIC SITE COUNT inside this VM (145 of 436 audited sites across
    // bt_vm_core/ai/ext.inc -- a count of distinct call sites in source,
    // NOT a measured share of runtime traffic) -- the caller
    // (AISystem::Update) already holds this exact pointer before calling
    // tick() at all. CORRECTION (2026-09-07, real measurement): a
    // real-population GATE 1 re-check found this fix's actual runtime
    // contribution was concentrated in a one-time SPAWN BURST (10446 of
    // the traffic in the first 50 ticks after a 512-NPC spawn), not
    // ongoing steady-state cost -- steady-state BT-path traffic measured
    // ~0 once trees settled, with the ~2910 calls/tick steady-state total
    // coming from elsewhere in the facade. Do not cite a "55%" or similar
    // steady-state-share figure for this fix; it does not hold up against
    // a real profile. Verified ABI-safe: BTActionFunc (the one signature
    // that crosses the hot-reload .so boundary) is untouched -- it's a
    // separate dispatch path (nd.action(ctx, e) at BTNodeType::Action)
    // that this fix does not touch. tick() itself has exactly one live
    // caller (ai_system.h), itself never hot-reloaded.
    BTStatus tick(md::EngineContext& ctx, MdEntity e, AgentState* as, uint32_t nowMs);
    void     reset();

private:
    BTNode   m_nodes   [MAX_NODES];
    uint16_t m_children[MAX_CHILDREN];
    BTState  m_state   [MAX_NODES];

    uint16_t m_nodeCount  = 0;
    uint16_t m_childCount = 0;
    uint16_t m_root       = INVALID;

    // Every add*() factory did `uint16_t i = m_nodeCount++;` then wrote
    // m_nodes[i] unconditionally -- MAX_NODES was declared but never
    // enforced, an out-of-bounds write for any tree (hand-built or JSON-
    // loaded) with more than MAX_NODES total nodes. Centralized here so the
    // ~250 call sites across behavior_tree.cpp/bt_factories.cpp all get the
    // same bound: reuse the last valid slot (aliasing, not memory-safe-fixing
    // the tree's logic) rather than corrupt adjacent memory.
    uint16_t allocNodeIdx();
};
