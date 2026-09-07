        // MD_deepseek: AggroLevelCheck / SetAggroLevel
        case BTNodeType::AggroLevelCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->aggro_level == static_cast<NpcAggroLevel>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::SetAggroLevel: {
            if (as) as->aggro_level = static_cast<NpcAggroLevel>(nd.data);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // MD_deepseek: NpcCombatStateCheck / SetNpcCombatState
        case BTNodeType::NpcCombatStateCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->combat_state == static_cast<NpcCombatState>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::SetNpcCombatState: {
            if (as) as->combat_state = static_cast<NpcCombatState>(nd.data);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // ── MD_z VM cases ─────────────────────────────────────────────────

        // Z1: DecoratorMood — run child only when as->mood matches
        case BTNodeType::DecoratorMood: {
            if (result == BTStatus::Running) {
                if (!as || as->mood != static_cast<NpcMood>(nd.data)) {
                    result = BTStatus::Failure; pc = nd.parent; continue;
                }
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            pc = nd.parent; continue;  // propagate child result as-is
        }

        // Z2: DecoratorAwareness — run child only when as->awareness matches
        case BTNodeType::DecoratorAwareness: {
            if (result == BTStatus::Running) {
                if (!as || as->awareness != static_cast<AwarenessState>(nd.data)) {
                    result = BTStatus::Failure; pc = nd.parent; continue;
                }
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            pc = nd.parent; continue;  // propagate child result as-is
        }

        // Z3: DecoratorTimerAuto — start timer on every entry, propagate child result
        case BTNodeType::DecoratorTimerAuto: {
            if (result == BTStatus::Running) {
                if (as) {
                    uint8_t  tid    = static_cast<uint8_t>(nd.data >> 24);
                    uint32_t dur_ms = nd.data & 0x00FFFFFFu;
                    if (tid < MAX_AGENT_TIMERS) {
                        uint64_t new_end = static_cast<uint64_t>(nowMs) + dur_ms;
                        if ((nd.flags & 0x01u) && as->timers[tid] > new_end) {
                            // only_increase: leave longer deadline intact
                        } else {
                            as->timers[tid] = new_end;
                        }
                    }
                }
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                result = BTStatus::Success; pc = nd.parent; continue;
            }
            pc = nd.parent; continue;  // propagate child result as-is
        }

        // Z4: ActionTimerRandom — start timer with LCG-random duration [min_ms, max_ms]
        case BTNodeType::ActionTimerRandom: {
            if (as) {
                uint8_t  tid   = static_cast<uint8_t>(nd.data >> 24);
                uint32_t max_u = (nd.data >> 12) & 0xFFFu;   // 12 bits, unit=100ms
                uint32_t min_u =  nd.data        & 0xFFFu;
                if (tid < MAX_AGENT_TIMERS) {
                    uint32_t range = (max_u > min_u) ? (max_u - min_u) : 0u;
                    uint32_t rng   = static_cast<uint32_t>(e.ToIntegral()) ^ nowMs;
                    rng = rng * 1664525u + 1013904223u;
                    uint32_t dur_u   = min_u + (range > 0u ? rng % range : 0u);
                    uint64_t new_end = static_cast<uint64_t>(nowMs)
                                     + static_cast<uint64_t>(dur_u) * 100u;
                    as->timers[tid] = new_end;
                }
            }
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // Z5: ActionSquadNotify — broadcast SquadSignal to entity's squad channel
        case BTNodeType::ActionSquadNotify: {
            SquadMemberComponent* sm = MdRegistry::Get().Handle(e).try_get_mut<SquadMemberComponent>();
            if (sm) {
                auto sig = static_cast<SquadSignal>(nd.data & 0xFFu);
                uint32_t raw_id = static_cast<uint32_t>(e.ToIntegral() & 0xFFFFFFFFu);
                SquadSignalBus::Get().Set(sm->squad_id, sig, raw_id, nowMs);
            }
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // Z6: ConditionSquadSignal — check squad channel for expected signal
        case BTNodeType::ConditionSquadSignal: {
            SquadMemberComponent* sm = MdRegistry::Get().Handle(e).try_get_mut<SquadMemberComponent>();
            if (!sm) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto expected = static_cast<SquadSignal>(nd.data & 0xFFu);
            result = (SquadSignalBus::Get().GetSignal(sm->squad_id) == expected)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // Z7: ConditionAnySenseWithinTime — any (or specific) sense fired within time_ms
        case BTNodeType::ConditionAnySenseWithinTime: {
            SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint32_t time_ms  = nd.data & 0x0FFFFFFFu;
            uint8_t  sense_idx= static_cast<uint8_t>((nd.data >> 28) & 0xFu);
            bool     specific = (nd.flags != 0u);
            bool     ok       = false;
            if (specific) {
                uint32_t ts = sc->last_activated_ms[sense_idx];
                ok = (ts != 0u && static_cast<uint32_t>(nowMs) - ts <= time_ms);
            } else {
                for (uint8_t ii = 0; ii < MAX_SENSES && !ok; ++ii) {
                    uint32_t ts = sc->last_activated_ms[ii];
                    ok = (ts != 0u && static_cast<uint32_t>(nowMs) - ts <= time_ms);
                }
            }
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // Z8: ActionExpireTimer — immediately mark timer slot as expired
        case BTNodeType::ActionExpireTimer: {
            if (as) {
                uint8_t tid = static_cast<uint8_t>(nd.data & 0x1Fu);
                if (tid < MAX_AGENT_TIMERS)
                    as->timers[tid] = 1u;  // past-deadline: nowMs >= 1 always true
            }
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // Z9: TargetFlagCheck — check lcflags on target entity from blackboard
        case BTNodeType::TargetFlagCheck: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity target = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(target)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            AgentState*  as_t   = MdRegistry::Get().Handle(target).try_get_mut<AgentState>();
            if (!as_t) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t bit_idx = static_cast<uint8_t>(nd.data & 0x3Fu);
            bool    is_set  = as_t->lcflags.test(bit_idx);
            result = (nd.flags == 0u ? is_set : !is_set)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // Z10: SetLocomotionState — write as->locomotion_state
        case BTNodeType::SetLocomotionState: {
            if (as) as->locomotion_state = static_cast<LocomotionState>(nd.data);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // MD_arch P4: DecoratorNamedBranch — gate child on NamedBranchRegistry
        case BTNodeType::DecoratorNamedBranch: {
            if (result == BTStatus::Running) {
                // first entry: check branch
                bool active   = NamedBranchRegistry::Get().IsActive(nd.data);
                bool inverted = (nd.flags & 1u) != 0u;
                bool gate_ok  = inverted ? !active : active;
                if (!gate_ok) { result = BTStatus::Failure; pc = nd.parent; continue; }
                if (nd.childStart == INVALID || nd.childCount == 0) {
                    result = BTStatus::Success; pc = nd.parent; continue;
                }
                pc = m_children[nd.childStart]; continue;
            }
            // returning from child — propagate as-is
            pc = nd.parent; continue;
        }

        // G1: ActionIdleTime — wait duration_ms like Wait but named for MD clarity
        case BTNodeType::ActionIdleTime: {
            BTState& st = m_state[pc];
            if (st.timer == 0u)
                st.timer = nowMs + nd.data;
            if (static_cast<int32_t>(nowMs - st.timer) >= 0) {
                st.timer = 0u;
                result = BTStatus::Success;
            } else {
                result = BTStatus::Running;
            }
            pc = nd.parent; continue;
        }

        // G2: ConditionHaveTarget — Success if "target_entity" bb key holds a valid entity
        case BTNodeType::ConditionHaveTarget: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            result = MdRegistry::Get().Valid(tgt) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // G3: ConditionHaveNextTarget — Success if "next_target_entity" bb key holds valid entity
        case BTNodeType::ConditionHaveNextTarget: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, NEXT_TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            result = MdRegistry::Get().Valid(tgt) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // G4: ActionTriggerSound — fire NpcSoundBus event; always Success
        case BTNodeType::ActionTriggerSound: {
            NpcSoundBus::Get().Fire(static_cast<NpcSoundEvent>(nd.data),
                                    static_cast<uint32_t>(e.ToIntegral() & 0xFFFFFFFFu));
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // G5: ConditionIsSenseActivationAbove — current activation vs qualifier threshold
        case BTNodeType::ConditionIsSenseActivationAbove: {
            SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t sense_idx = static_cast<uint8_t>((nd.data >> 8) & 0xFu);
            if (sense_idx >= MAX_SENSES) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto    q         = static_cast<SenseThresholdQualifier>(nd.data & 0xFFu);
            float   act       = sc->activation[sense_idx];
            bool    ok        = false;
            switch (q) {
                case SenseThresholdQualifier::Trace:     ok = (act > 0.0f);               break;
                case SenseThresholdQualifier::Lower:     ok = (act >= sc->threshold_lo);  break;
                case SenseThresholdQualifier::Activated: ok = (act >= sc->threshold_hi);  break;
                case SenseThresholdQualifier::Upper:     ok = (act >= 1.0f);              break;
            }
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // G6: ConditionHasAnySenseBeenAbove — any sense ever reached qualifier (last_activated_ms set)
        case BTNodeType::ConditionHasAnySenseBeenAbove: {
            SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto q = static_cast<SenseThresholdQualifier>(nd.data & 0xFFu);
            bool ok = false;
            for (uint8_t ii = 0; ii < MAX_SENSES && !ok; ++ii) {
                switch (q) {
                    case SenseThresholdQualifier::Trace:
                    case SenseThresholdQualifier::Lower:
                        ok = (sc->activation[ii] > 0.0f); break;
                    case SenseThresholdQualifier::Activated:
                    case SenseThresholdQualifier::Upper:
                        ok = (sc->last_activated_ms[ii] != 0u); break;
                }
            }
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // G7: ActionSuspiciousItemDoneStage — stamp investigation_stage on most-recent event
        case BTNodeType::ActionSuspiciousItemDoneStage: {
            NpcMemoryComponent* nmc = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nmc || nmc->event_count == 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
            nmc->events[nmc->event_count - 1].investigation_stage =
                static_cast<SuspiciousItemStage>(nd.data);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // G8: ConditionSuspiciousItemBTPriority — any event intensity >= min_intensity
        case BTNodeType::ConditionSuspiciousItemBTPriority: {
            NpcMemoryComponent* nmc = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nmc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t min_i = static_cast<uint8_t>(nd.data & 0xFFu);
            bool    ok    = false;
            for (uint8_t ii = 0; ii < nmc->event_count && !ok; ++ii)
                ok = (nmc->events[ii].intensity >= min_i);
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // G9: ConditionLastTimeSquadNotified — Success if squad channel signal matches and is recent
        case BTNodeType::ConditionLastTimeSquadNotified: {
            SquadMemberComponent* sm = MdRegistry::Get().Handle(e).try_get_mut<SquadMemberComponent>();
            if (!sm) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto    expected  = static_cast<SquadSignal>((nd.data >> 8) & 0xFFu);
            uint8_t thresh_s  = static_cast<uint8_t>(nd.data & 0xFFu);
            const SquadChannel& ch = SquadSignalBus::Get().GetChannel(sm->squad_id);
            bool ok = (ch.signal == expected) &&
                      (static_cast<uint32_t>(nowMs) - ch.timestamp) / 1000u <= static_cast<uint32_t>(thresh_s);
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // G10: DecoratorAggressionEscalation — pass through if aggro_level != None
        case BTNodeType::DecoratorAggressionEscalation: {
            if (result == BTStatus::Running) {
                if (!as || as->aggro_level == NpcAggroLevel::None) {
                    result = BTStatus::Failure; pc = nd.parent; continue;
                }
                if (nd.childStart == INVALID || nd.childCount == 0) {
                    result = BTStatus::Success; pc = nd.parent; continue;
                }
                pc = m_children[nd.childStart]; continue;
            }
            pc = nd.parent; continue;
        }

        // ── Batch 2: MD BEHAVIOR XML patterns ────────────────────────────────

        case BTNodeType::ConditionIsDead: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::IS_DEAD) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsInVent: {
            uint8_t char_type = static_cast<uint8_t>(nd.data & 0x3u);
            bool owner_ok = true, target_ok = true;
            if (char_type == 0 || char_type == 2) {
                owner_ok = as && as->lcflags.test(lcf::IS_IN_VENT);
            }
            if (char_type == 1 || char_type == 2) {
                AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
                const BlackboardEntry* ben = ab ? bb_find(*ab, TARGET_ENTITY_BB_KEY) : nullptr;
                MdEntity tgt = ben ? MdRegistry::Get().FromIndex(ben->val.e) : MdEntity::Null();
                if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
                AgentState* tas = MdRegistry::Get().Handle(tgt).try_get_mut<AgentState>();
                target_ok = tas && tas->lcflags.test(lcf::IS_IN_VENT);
            }
            result = (owner_ok && target_ok) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionCanBreakout: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            bool ok = as->lcflags.test(lcf::SHOULD_BREAKOUT) &&
                     !as->lcflags.test(lcf::DONE_BREAKOUT);
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsBackstage: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::CLOSE_TO_BACKSTAGE) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsPartOfNPCGroup: {
            result = MdRegistry::Get().Handle(e).try_get_mut<SquadMemberComponent>()
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionAnotherAlienIsAttacking: {
            // Stalk = closest role to "attacking/pursuing"; Flanker = second approach vector
            const auto& rr = RoleRegistry::Get();
            bool another_stalk   = !rr.could_perform(NpcRole::Stalk)   &&
                                   !rr.is_performing(NpcRole::Stalk,   e);
            bool another_flanker = !rr.could_perform(NpcRole::Flanker) &&
                                   !rr.is_performing(NpcRole::Flanker, e);
            result = (another_stalk || another_flanker) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasSearchedPos: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = bb_get_bool(*ab, HAS_SEARCHED_POS_BB_KEY)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasDoneSuspectMoveTo: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = bb_get_bool(*ab, DONE_SUSPECT_MOVETO_BB_KEY)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionSwitchToNextTarget: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* next = bb_find(*ab, NEXT_TARGET_ENTITY_BB_KEY);
            if (!next) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint32_t next_val = next->val.e;
            // Write target_entity = next entity; clear next_target_entity (type=3 = entity slot)
            if (BlackboardEntry* tgt = bb_insert(*ab, TARGET_ENTITY_BB_KEY, 3))
                tgt->val.e = next_val;
            if (BlackboardEntry* nxt = bb_insert(*ab, NEXT_TARGET_ENTITY_BB_KEY, 3))
                nxt->val.e = MdEntity::Null().ToIntegral();
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionDoneSystematicSearch: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (ab) bb_set_bool(*ab, DONE_SYSTEMATIC_SEARCH_BB_KEY, true);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // ── Batch 4: EventOrder ───────────────────────────────────────────────

        case BTNodeType::ConditionEventAOccuredAfterB: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t ea = static_cast<uint8_t>((nd.data >> 8) & 0x3u);
            uint8_t eb = static_cast<uint8_t>(nd.data & 0x3u);
            if (ea >= MAX_EVENT_TYPES || eb >= MAX_EVENT_TYPES) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            // A must have occurred (ts != 0) and be strictly newer than B
            result = (as->event_ts[ea] != 0u && as->event_ts[ea] > as->event_ts[eb])
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 5: Squad extensions ─────────────────────────────────────────

        case BTNodeType::ConditionSquadDoingEscalation: {
            SquadMemberComponent* smc = MdRegistry::Get().Handle(e).try_get_mut<SquadMemberComponent>();
            if (!smc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (SquadSignalBus::Get().GetSignal(smc->squad_id) == SquadSignal::Escalating)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionSquadDoingSuspiciousWarning: {
            SquadMemberComponent* smc = MdRegistry::Get().Handle(e).try_get_mut<SquadMemberComponent>();
            if (!smc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (SquadSignalBus::Get().GetSignal(smc->squad_id) == SquadSignal::SuspiciousWarn)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::DecoratorSquadSearch:
            if (result == BTStatus::Running) {
                SquadMemberComponent* smc = MdRegistry::Get().Handle(e).try_get_mut<SquadMemberComponent>();
                if (smc) SquadSignalBus::Get().Set(smc->squad_id,
                                                   SquadSignal::Warning,
                                                   static_cast<uint32_t>(e.ToIntegral()),
                                                   nowMs);
                if (nd.childCount > 0) { pc = m_children[nd.childStart]; result = BTStatus::Running; continue; }
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            pc = nd.parent; continue; // propagate child result

        // ── Batch 6: SuspiciousItem Group system ──────────────────────────────

        case BTNodeType::ConditionSuspiciousItemShouldDoStage: {
            NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nm || nm->event_count == 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto expected = static_cast<SuspiciousItemStage>(nd.data & 0xFFu);
            result = (nm->events[0].investigation_stage == expected)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionSuspiciousItemIsWithinDistance: {
            NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            SenseComponent*     sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!nm || nm->event_count == 0 || !sc) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            float max_m  = static_cast<float>(nd.data) * 0.01f;
            float dx     = sc->last_known_x - nm->events[0].x;
            float dz     = sc->last_known_z - nm->events[0].z;
            result = (dx * dx + dz * dz <= max_m * max_m)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionSuspiciousItemFirstGroupMember: {
            SuspiciousItemGroupComponent* gc = MdRegistry::Get().Handle(e).try_get_mut<SuspiciousItemGroupComponent>();
            if (!gc || gc->group_id == SuspiciousItemGroupComponent::INVALID_GROUP) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            result = SuspiciousItemGroupRegistry::Get().IsFirstMember(
                         gc->group_id, static_cast<uint32_t>(e.ToIntegral()))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionSuspiciousItemGroupAllowedToProgress: {
            SuspiciousItemGroupComponent* gc = MdRegistry::Get().Handle(e).try_get_mut<SuspiciousItemGroupComponent>();
            if (!gc || gc->group_id == SuspiciousItemGroupComponent::INVALID_GROUP) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            result = SuspiciousItemGroupRegistry::Get().IsAllowedToProgress(gc->group_id)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionSuspiciousItemGroupMembersRoutingTo: {
            SuspiciousItemGroupComponent* gc = MdRegistry::Get().Handle(e).try_get_mut<SuspiciousItemGroupComponent>();
            if (!gc || gc->group_id == SuspiciousItemGroupComponent::INVALID_GROUP) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            result = SuspiciousItemGroupRegistry::Get().AllRoutingTo(gc->group_id)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionSuspiciousItemWaitForGroupRouting: {
            SuspiciousItemGroupComponent* gc = MdRegistry::Get().Handle(e).try_get_mut<SuspiciousItemGroupComponent>();
            if (!gc || gc->group_id == SuspiciousItemGroupComponent::INVALID_GROUP) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            result = SuspiciousItemGroupRegistry::Get().WaitingForRouting(gc->group_id)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
