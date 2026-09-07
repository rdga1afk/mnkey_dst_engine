        case BTNodeType::Selector:
            if (result == BTStatus::Success) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            if (result == BTStatus::Failure) {
                st.currentChild++;
            } else {
                st.currentChild = 0;
            }
            if (st.currentChild >= nd.childCount) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            pc = m_children[nd.childStart + st.currentChild];
            result = BTStatus::Running; continue;

        case BTNodeType::Sequence:
            if (result == BTStatus::Failure) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            if (result == BTStatus::Success) {
                st.currentChild++;
            }
            if (st.currentChild >= nd.childCount) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            pc = m_children[nd.childStart + st.currentChild];
            result = BTStatus::Running; continue;

        case BTNodeType::Condition:
            result = nd.condition(ctx, e) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;

        case BTNodeType::Action:
            result = nd.action(ctx, e);
            if (result == BTStatus::Running) goto exit_loop;
            pc = nd.parent; continue;

        case BTNodeType::Inverter:
            if (result == BTStatus::Running) {
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            result = (result == BTStatus::Success) ? BTStatus::Failure : BTStatus::Success;
            pc = nd.parent; continue;

        case BTNodeType::Repeat:
            if (result == BTStatus::Running) {
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            if (result == BTStatus::Failure) {
                st.counter = 0;
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            st.counter++;
            if (nd.data > 0 && st.counter >= nd.data) {
                st.counter = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            if (nd.childCount > 0) {
                uint16_t ci = m_children[nd.childStart];
                m_state[ci] = BTState{};
                pc = ci; result = BTStatus::Running; continue;
            }
            pc = nd.parent; result = BTStatus::Success; continue;

        case BTNodeType::Wait:
            if (st.timer == 0)
                st.timer = nowMs + nd.data;
            if (static_cast<int32_t>(nowMs - st.timer) >= 0) {
                st.timer = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            result = BTStatus::Running;
            goto exit_loop;

        // ── M21 ──────────────────────────────────────────────────────────────

        case BTNodeType::Branch:
            if (result == BTStatus::Running) {
                // Re-evaluated every tick: persistent interrupt gate
                if (nd.childCount == 0 || (nd.condition && !nd.condition(ctx, e))) {
                    pc = nd.parent; result = BTStatus::Failure; continue;
                }
                pc = m_children[nd.childStart];
                result = BTStatus::Running; continue;
            }
            pc = nd.parent; continue;  // propagate child result as-is

        case BTNodeType::TimerStart: {
            if (as) {
                uint8_t  tid    = static_cast<uint8_t>(nd.data >> 24);
                uint32_t dur_ms = nd.data & 0x00FFFFFFu;
                if (tid < MAX_AGENT_TIMERS) {
                    uint64_t new_end = static_cast<uint64_t>(nowMs) + dur_ms;
                    // C12: only_increase — don't shorten an already-running timer
                    if ((nd.flags & 0x01u) && as->timers[tid] > new_end) {
                        // existing deadline is later; leave it untouched
                    } else {
                        as->timers[tid] = new_end;
                    }
                }
            }
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        case BTNodeType::TimerCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t  tid      = static_cast<uint8_t>(nd.data & 0x1Fu);
            if (tid >= MAX_AGENT_TIMERS) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint64_t deadline = as->timers[tid];
            if (deadline != 0u && static_cast<uint64_t>(nowMs) >= deadline) {
                as->timers[tid] = 0u;
                result = BTStatus::Success;
            } else {
                result = BTStatus::Failure;
            }
            pc = nd.parent; continue;
        }

        // Pattern 4: bit-index encoding (lcf::* constants, 0-63)
        case BTNodeType::FlagCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t bit_idx = static_cast<uint8_t>(nd.data & 0x3Fu);
            bool    is_set  = as->lcflags.test(bit_idx);
            result = (nd.flags == 0u ? is_set : !is_set)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::FlagSet: {
            if (as) {
                uint8_t bit_idx = static_cast<uint8_t>(nd.data & 0x3Fu);
                if (nd.flags == 0u) as->lcflags.set  (bit_idx);
                else                as->lcflags.clear(bit_idx);
            }
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        case BTNodeType::SenseCheck: {
            SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t sense_idx = static_cast<uint8_t>(nd.data >> 24);
            float   threshold = static_cast<float>(nd.data & 0x00FFFFFFu) * 0.001f;
            if (sense_idx >= MAX_SENSES) sense_idx = 0u;
            result = (sc->activation[sense_idx] >= threshold)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── AI behavior patterns adaptations ───────────────────────────────────────

        // Pattern 1: check current motivation matches expected
        case BTNodeType::MotivationCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->motivation == static_cast<MotivationType>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // Pattern 1: write motivation to agent state
        case BTNodeType::SetMotivation: {
            if (as) as->motivation = static_cast<MotivationType>(nd.data);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // Pattern 3: delegate to referenced tree
        case BTNodeType::Reference: {
            BehaviorTree* other = static_cast<BehaviorTree*>(nd._padding);
            if (!other) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = other->tick(ctx, e, as, nowMs);
            if (result == BTStatus::Running) goto exit_loop;
            pc = nd.parent; continue;
        }

        // Pattern 6: gauge threshold check
        case BTNodeType::GaugeCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto    gtype = static_cast<GaugeType>(nd.data >> 24);
            float   thr   = static_cast<float>(nd.data & 0x00FFFFFFu) * 0.001f;
            result = (as->gauges.get(gtype) >= thr)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // Pattern 6: gauge write (set to fixed value)
        case BTNodeType::GaugeSet: {
            if (as) {
                auto  gtype = static_cast<GaugeType>(nd.data >> 24);
                float val   = static_cast<float>(nd.data & 0x00FFFFFFu) * 0.001f;
                as->gauges.set(gtype, val);
            }
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // ── AI-11–20 ─────────────────────────────────────────────────────

        // C11: SequenceStateless — always restarts from child 0 on re-entry
        // Batch 16: SequenceIgnoreChildFail — advances on both Success and Failure
        case BTNodeType::SequenceIgnoreChildFail:
            if (result == BTStatus::Success || result == BTStatus::Failure) {
                st.currentChild++;
            }
            if (st.currentChild >= nd.childCount) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            pc = m_children[nd.childStart + st.currentChild];
            result = BTStatus::Running; continue;

        case BTNodeType::SequenceStateless:
            if (result == BTStatus::Failure) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Failure; continue;
            }
            if (result == BTStatus::Success) {
                st.currentChild++;
            } else { // Running — stateless: reset to 0 every entry
                st.currentChild = 0;
            }
            if (st.currentChild >= nd.childCount) {
                st.currentChild = 0;
                pc = nd.parent; result = BTStatus::Success; continue;
            }
            pc = m_children[nd.childStart + st.currentChild];
            result = BTStatus::Running; continue;

        // C13: FrameFlag — read/write frame_flags (cleared each logic tick)
        case BTNodeType::FrameFlagCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t bit_idx = static_cast<uint8_t>(nd.data & 0x3Fu);
            bool    is_set  = (as->frame_flags >> bit_idx) & 1ull;
            result = (nd.flags == 0u ? is_set : !is_set)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::FrameFlagSet: {
            if (as) {
                uint8_t bit_idx = static_cast<uint8_t>(nd.data & 0x3Fu);
                if (nd.flags == 0u) as->frame_flags |=  (1ull << bit_idx);
                else                as->frame_flags &= ~(1ull << bit_idx);
            }
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // C14: WeightedSelector — one-shot weighted child pick per entry
        // st.currentChild uses bit 15 as a "committed" flag (bits 0-1 hold
        // the actual chosen index, only 4 children supported) rather than a
        // sentinel *value* -- BTState is zero-initialized at construction,
        // so a sentinel like 0xFFFF would be indistinguishable from a real
        // "chosen child 0" only if 0 itself were the sentinel; using a
        // separate bit keeps the natural zero-init state correctly reading
        // as "not committed" without any extra reset-time bookkeeping.
        case BTNodeType::WeightedSelector:
            if (result == BTStatus::Success || result == BTStatus::Failure) {
                st.currentChild = 0;
                pc = nd.parent; continue; // propagate child result as-is
            }
            if (st.currentChild & 0x8000u) {
                // Already committed to a child this run (this Running entry
                // is a resumption of that in-flight child on a later
                // external Tick(), not a genuine fresh entry -- the
                // stackless VM re-descends from m_root every call, so
                // Running here is otherwise indistinguishable from first
                // entry). Previously this re-rolled every single time,
                // abandoning the committed child's state and defeating the
                // documented "one-shot weighted child pick per entry" design.
                uint16_t chosen = st.currentChild & 0x7FFFu;
                pc = m_children[nd.childStart + (chosen % nd.childCount)];
                continue;
            }
            // Genuine first entry: pick weighted random child, commit to it.
            {
                if (nd.childCount == 0) {
                    pc = nd.parent; result = BTStatus::Failure; continue;
                }
                uint8_t w[4] = {
                    static_cast<uint8_t>( nd.data        & 0xFFu),
                    static_cast<uint8_t>((nd.data >>  8) & 0xFFu),
                    static_cast<uint8_t>((nd.data >> 16) & 0xFFu),
                    static_cast<uint8_t>((nd.data >> 24) & 0xFFu),
                };
                // LCG step seeded by entity id XOR frame_index
                uint32_t rng = static_cast<uint32_t>(e.ToIntegral())
                               ^ ctx.frame_index;
                rng = rng * 1664525u + 1013904223u;
                uint8_t  nc    = nd.childCount < 4u ? static_cast<uint8_t>(nd.childCount) : 4u;
                uint32_t total = 0;
                for (uint8_t ii = 0; ii < nc; ++ii) total += w[ii];
                if (total == 0u) total = 1u;
                uint32_t roll    = rng % total;
                uint8_t  chosen  = nc - 1u;
                uint32_t acc     = 0u;
                for (uint8_t ii = 0; ii < nc; ++ii) {
                    acc += w[ii];
                    if (roll < acc) { chosen = ii; break; }
                }
                st.currentChild = static_cast<uint16_t>(chosen) | 0x8000u;
                pc = m_children[nd.childStart + (chosen % nd.childCount)];
                result = BTStatus::Running; continue;
            }

        // C15: AwarenessCheck
        case BTNodeType::AwarenessCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->awareness == static_cast<AwarenessState>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // C16: AlertnessCheck
        case BTNodeType::AlertnessCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->alertness == static_cast<AlertnessState>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // C17: MoodCheck
        case BTNodeType::MoodCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->mood == static_cast<NpcMood>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // C18: Role coordination
        case BTNodeType::RoleCheck: {
            auto  role    = static_cast<NpcRole>(nd.data >> 8);
            bool  do_could = (nd.data & 0xFFu) != 0u;
            bool  ok = do_could ? RoleRegistry::Get().could_perform(role)
                                : RoleRegistry::Get().is_performing(role, e);
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::RoleClaim: {
            auto     role     = static_cast<NpcRole>(nd.data & 0xFFu);
            uint32_t query_id = nd.data >> 8;
            // Prefer dynamic query_id from DirectorHintComponent when the hint
            // targets this same role — scopes the claim to the Director's session.
            if (const DirectorHintComponent* hint =
                    MdRegistry::Get().Handle(e).try_get_mut<DirectorHintComponent>()) {
                if (hint->role_pending && hint->suggested_role == role)
                    query_id = hint->query_id;
            }
            result = RoleRegistry::Get().claim(role, query_id, e)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::RoleRelease: {
            auto role = static_cast<NpcRole>(nd.data);
            RoleRegistry::Get().release(role, e);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // C19: WithdrawState
        case BTNodeType::WithdrawCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->withdraw_state == static_cast<WithdrawState>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::SetWithdraw: {
            if (as) as->withdraw_state = static_cast<WithdrawState>(nd.data);
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        // Echo NpcMemory nodes
        case BTNodeType::MemoryCheck: {
            NpcMemoryComponent* mem = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!mem) { result = BTStatus::Failure; pc = nd.parent; continue; }
            bool ok = (nd.data == 0) ? (mem->spatial_count > 0) : (mem->event_count > 0);
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::MemoryForget: {
            NpcMemoryComponent* mem = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (mem) mem->ClearAll();
            result = BTStatus::Success;
            pc = nd.parent; continue;
        }

        case BTNodeType::AreaSweepCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (static_cast<uint8_t>(as->area_sweep_type) == static_cast<uint8_t>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // MD_gemini: DecoratorPercentage — run child with data% probability
        case BTNodeType::DecoratorPercentage:
            if (result == BTStatus::Running) {
                // LCG seeded by entity XOR frame_index (same pattern as WeightedSelector)
                uint32_t rng = static_cast<uint32_t>(e.ToIntegral()) ^ ctx.frame_index;
                rng = rng * 1664525u + 1013904223u;
                uint8_t pct = static_cast<uint8_t>(nd.data & 0xFFu);
                if ((rng % 100u) < pct && nd.childCount > 0) {
                    pc = m_children[nd.childStart];
                    result = BTStatus::Running; continue;
                }
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            pc = nd.parent; continue;  // propagate child result as-is

        // MD_gemini: SelectorPercentage — pick one random child, return its result
        case BTNodeType::SelectorPercentage:
            if (result == BTStatus::Success || result == BTStatus::Failure) {
                pc = nd.parent; continue;  // propagate child result
            }
            {
                if (nd.childCount == 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
                uint32_t rng = static_cast<uint32_t>(e.ToIntegral()) ^ ctx.frame_index;
                rng = rng * 1664525u + 1013904223u;
                uint16_t chosen = static_cast<uint16_t>(rng % nd.childCount);
                pc = m_children[nd.childStart + chosen];
                result = BTStatus::Running; continue;
            }

        // MD_gemini: SenseTimeCheck — Success if sense was triggered recently
        case BTNodeType::SenseTimeCheck: {
            auto* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t  sense_idx      = static_cast<uint8_t>((nd.data >> 24) & 0xFFu);
            uint32_t max_elapsed_ms = nd.data & 0x00FFFFFFu;
            if (sense_idx >= MAX_SENSES) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint32_t elapsed = nowMs - sc->last_activated_ms[sense_idx];
            result = (elapsed <= max_elapsed_ms) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // MD_gemini: ActionSetDead / ActionDespawn — deferred lifecycle via lcf flags
        case BTNodeType::ActionSetDead: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            as->lcflags.set(lcf::IS_DEAD);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ActionDespawn: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            as->lcflags.set(lcf::SHOULD_DESPAWN);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
