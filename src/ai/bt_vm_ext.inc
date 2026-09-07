        // ── Batch 7: SuspiciousItemReaction / AmbushType / NoiseType ─────────
        case BTNodeType::SIReactionCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->si_reaction == static_cast<SuspiciousItemReaction>(nd.data & 0xFFu))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::SetSIReaction: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            as->si_reaction = static_cast<SuspiciousItemReaction>(nd.data & 0xFFu);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::AmbushTypeCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->ambush_type == static_cast<AmbushType>(nd.data & 0xFFu))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::SetAmbushType: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            as->ambush_type = static_cast<AmbushType>(nd.data & 0xFFu);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::NoiseTypeCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->last_noise_type == static_cast<NoiseType>(nd.data & 0xFFu))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::SetNoiseType: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            as->last_noise_type = static_cast<NoiseType>(nd.data & 0xFFu);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        // ── Batch 8: SI lifecycle ─────────────────────────────────────────────
        case BTNodeType::ConditionSuspiciousItemValid: {
            NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nm || nm->event_count == 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (nm->events[0].investigation_stage != SuspiciousItemStage::SearchArea)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ActionConsumeSuspiciousItem: {
            NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (nm && nm->event_count > 0) {
                for (uint8_t i = 0; i < nm->event_count - 1u; ++i)
                    nm->events[i] = nm->events[i + 1];
                nm->event_count--;
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::ActionForceMoveToSI: {
            NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nm || nm->event_count == 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
            if (as) { as->frame_flags |= (1ull << ff::COULD_DO_SUSPECT_TARGET_RESPONSE_MOVETO); }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        // ── Batch 9 (Step 5): Inter-NPC relationship nodes ────────────────────
        case BTNodeType::RelationshipTrustCheck:
        case BTNodeType::RelationshipFearCheck: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* ben = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!ben) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(ben->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            NpcRelationshipComponent* rel = MdRegistry::Get().Handle(e).try_get_mut<NpcRelationshipComponent>();
            if (!rel) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t threshold = static_cast<uint8_t>((nd.data >> 8) & 0xFFu);
            uint8_t mode      = static_cast<uint8_t>(nd.data & 0x1u);
            uint8_t val = (nd.type == BTNodeType::RelationshipTrustCheck)
                          ? rel->GetTrust(e, tgt) : rel->GetFear(e, tgt);
            bool ok = (mode == 0) ? (val >= threshold) : (val < threshold);
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 10: MD_z.md BEHAVIOR XML patterns ──────────────────────────────
        case BTNodeType::ConditionIsAnySenseActivationAbove: {
            SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto q = static_cast<SenseThresholdQualifier>(nd.data & 0xFFu);
            bool found = false;
            for (uint8_t si = 0; si < MAX_SENSES && !found; ++si) {
                float act = sc->activation[si];
                switch (q) {
                case SenseThresholdQualifier::Trace:     found = act > 0.f;               break;
                case SenseThresholdQualifier::Lower:     found = act >= sc->threshold_lo;  break;
                case SenseThresholdQualifier::Activated: found = act >= sc->threshold_hi;  break;
                case SenseThresholdQualifier::Upper:     found = act >= 1.0f;              break;
                }
            }
            result = found ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionMoveThroughTarget: {
            if (as) {
                as->frame_flags  |= (1ull << ff::SHOULD_MOVE_THROUGH_TARGET);
                as->target_speed  = static_cast<LocomotionTargetSpeed>(nd.data & 0x3u);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ActionAdjustMenace: {
            uint8_t mode        = static_cast<uint8_t>(nd.data & 0x3u);
            float   delta_fixed = static_cast<float>((nd.data >> 2) & 0xFFu);
            DirectorSystem::Get().AdjustMenace(delta_fixed / 10.f, mode);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        // ── Batch 11: BEHAVIOR XML patterns (P2+P8+P10) ──────────────────────
        case BTNodeType::ConditionAngleToTarget: {
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!wt || !sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            // NPC forward from rot_y (right-hand Y-up: fwd = (sin, 0, cos)).
            // Use exact sinf/cosf + sqrtf: this condition evaluates once per entity
            // per tick, not per-triangle, so precision > speed here.
            float fwd_x = sinf(wt->rot_y);
            float fwd_z = cosf(wt->rot_y);
            float dx = sc->last_known_x - wt->x;
            float dz = sc->last_known_z - wt->z;
            float len_sq = dx * dx + dz * dz;
            if (len_sq < 1e-6f) { result = BTStatus::Success; pc = nd.parent; continue; }
            float dot = (fwd_x * dx + fwd_z * dz) / sqrtf(len_sq);
            float angle_deg = acosf(dot < -1.f ? -1.f : dot > 1.f ? 1.f : dot) * (180.f / 3.14159265f);
            result = (angle_deg <= static_cast<float>(nd.data)) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionShouldSuspend: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::IS_SUSPENDED) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionSuspendSelf: {
            if (as) as->lcflags.set(lcf::IS_SUSPENDED);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ConditionTargetDistLOS: {
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!wt || !sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx = sc->last_known_x - wt->x;
            float dz = sc->last_known_z - wt->z;
            float dist_sq = dx * dx + dz * dz;
            float max_dist = static_cast<float>(nd.data) / 10.f;
            bool dist_ok = dist_sq <= max_dist * max_dist;
            bool los_ok  = sc->activation[static_cast<int>(SenseType::Visual)] >= sc->threshold_lo;
            result = (dist_ok && los_ok) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 12 VM cases ─────────────────────────────────────────────────

        // P3: ConditionTargetRoutingDistance — nav distance via PathCache, Euclidean fallback
        case BTNodeType::ConditionTargetRoutingDistance: {
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!wt || !sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float max_dist = static_cast<float>(nd.data) / 10.f;
            uint32_t ks = PathCache::PosKey(wt->x, wt->z);
            uint32_t ke = PathCache::PosKey(sc->last_known_x, sc->last_known_z);
            float    now_s = static_cast<float>(nowMs) / 1000.f;
            float    nav_dist;
            {
                float tmp_verts[MAX_PATH_LEN * 3];
                int   path_len = 0;
                if (PathCache::Get().Get(ks, ke, now_s, tmp_verts, path_len) && path_len > 1) {
                    nav_dist = 0.f;
                    for (int pi = 0; pi + 1 < path_len; ++pi) {
                        float pdx = tmp_verts[(pi + 1) * 3 + 0] - tmp_verts[pi * 3 + 0];
                        float pdz = tmp_verts[(pi + 1) * 3 + 2] - tmp_verts[pi * 3 + 2];
                        nav_dist += md::fsqrt(pdx * pdx + pdz * pdz);
                    }
                } else {
                    float dx = sc->last_known_x - wt->x;
                    float dz = sc->last_known_z - wt->z;
                    nav_dist = md::fsqrt(dx * dx + dz * dz);
                }
            }
            result = (nav_dist <= max_dist) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // P6: ConditionAlienIsAllowed — check AlienConfigPreset::allowed_actions bitmask
        case BTNodeType::ConditionAlienIsAllowed: {
            auto action = static_cast<AlienActionType>(nd.data & 0xFFu);
            const AlienConfigPreset& preset = AlienConfigPreset::Get(
                DirectorSystem::Get().GetActiveConfig());
            uint8_t bit = static_cast<uint8_t>(action);
            bool allowed = (preset.allowed_actions & (uint16_t(1u) << bit)) != 0u;
            result = allowed ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // P7: DecoratorLockVent — exclusive vent access gate
        case BTNodeType::DecoratorLockVent: {
            if (result == BTStatus::Running) {
                uint8_t vent_id = static_cast<uint8_t>(nd.data & 0x7u);
                if (!VentLockTable::Get().TryLock(vent_id, e)) {
                    result = BTStatus::Failure; pc = nd.parent; continue;
                }
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart]; continue;
                }
                VentLockTable::Get().Release(e);
                result = BTStatus::Success; pc = nd.parent; continue;
            }
            // returning from child — release lock and propagate result
            VentLockTable::Get().Release(e);
            pc = nd.parent; continue;
        }

        // ── Batch 15: HIGH-priority patterns from MD engine analysis ─────────
        // ConditionIsEnemyOfTarget — AllianceMatrix::IsEnemy(self.alliance_group, target.alliance_group)
        case BTNodeType::ConditionIsEnemyOfTarget: {
            AgentBlackboard*   ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab || !as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity target    = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(target)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentState* tas  = MdRegistry::Get().Handle(target).try_get_mut<AgentState>();
            if (!tas) { result = BTStatus::Failure; pc = nd.parent; continue; }
            bool enemy = AllianceMatrix::Get().IsEnemy(
                static_cast<AllianceGroup>(as->alliance_group),
                static_cast<AllianceGroup>(tas->alliance_group));
            result = enemy ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ActionForceIdle — always Running (skip this BT tick, re-enter next tick)
        case BTNodeType::ActionForceIdle: {
            result = BTStatus::Running; pc = nd.parent; continue;
        }

        // ── Batch 17 ─────────────────────────────────────────────────────────
        // ConditionIsCharacterClass — Success if as->character_class == (CharacterClass)data
        case BTNodeType::ConditionIsCharacterClass: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->character_class == static_cast<CharacterClass>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        // ConditionIsInCover — Success if lcf::IS_IN_COVER set
        case BTNodeType::ConditionIsInCover: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::IS_IN_COVER)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        // ConditionShouldUseCover — Success if aggro_level >= Warning (3)
        case BTNodeType::ConditionShouldUseCover: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (static_cast<uint8_t>(as->aggro_level) >=
                      static_cast<uint8_t>(NpcAggroLevel::Warning))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        // ActionMoveToCover — set ff::SHOULD_MOVE_TO_COVER; always Success
        case BTNodeType::ActionMoveToCover: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_MOVE_TO_COVER);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        // ActionIdleInCover — mark as in cover + set combat state; always Running
        case BTNodeType::ActionIdleInCover: {
            if (as) {
                as->lcflags.set(lcf::IS_IN_COVER);
                as->combat_state = NpcCombatState::EnteredCover;
            }
            result = BTStatus::Running; pc = nd.parent; continue;
        }

        // ── Batch 18 ─────────────────────────────────────────────────────────
        // BehaviourMoodSetCheck — Success if as->behaviour_mood_set == (BehaviourMoodSet)data
        case BTNodeType::BehaviourMoodSetCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->behaviour_mood_set == static_cast<BehaviourMoodSet>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        // SetBehaviourMoodSet — write as->behaviour_mood_set; always Success
        case BTNodeType::SetBehaviourMoodSet: {
            if (as) as->behaviour_mood_set = static_cast<BehaviourMoodSet>(nd.data);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        // ViewconeTypeCheck — Success if as->viewcone_type == (ViewconeType)data
        case BTNodeType::ViewconeTypeCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->viewcone_type == static_cast<ViewconeType>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        // SetViewconeType — write as->viewcone_type; always Success
        case BTNodeType::SetViewconeType: {
            if (as) as->viewcone_type = static_cast<ViewconeType>(nd.data);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        // SensoryTypeCheck — Success if as->last_sensory_type == (SensoryType)data
        case BTNodeType::SensoryTypeCheck: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->last_sensory_type == static_cast<SensoryType>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        // SetSensoryType — write as->last_sensory_type; always Success
        case BTNodeType::SetSensoryType: {
            if (as) as->last_sensory_type = static_cast<SensoryType>(nd.data);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        // ConditionHasValidRouteToTarget — PathCache::Get from self to target position
        case BTNodeType::ConditionHasValidRouteToTarget: {
            AgentBlackboard*      ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            if (!ab || !wt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity          target = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(target)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* twt   = MdRegistry::Get().Handle(target).try_get_mut<WorldTransform>();
            if (!twt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint32_t ks = PathCache::PosKey(wt->x,  wt->z);
            uint32_t ke = PathCache::PosKey(twt->x, twt->z);
            float  dummy_pts[3] = {};
            int    dummy_len    = 0;
            bool   has_path = PathCache::Get().Get(ks, ke,
                                  static_cast<float>(nowMs) * 0.001f,
                                  dummy_pts, dummy_len);
            result = has_path ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 19: Weapon System ──────────────────────────────────────────
        case BTNodeType::ConditionCurrentWeaponIsEquipped: {
            WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (!wc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = wc->is_equipped ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionCurrentWeaponNeedsReloading: {
            WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (!wc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = wc->needs_reload ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionHasMeleeAttackAvailable: {
            WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (!wc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = wc->melee_available ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ActionWeaponEquip: {
            WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (wc) wc->is_equipped = true;
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::ActionRangedShoot: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_RANGED_SHOOT);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::ConditionHasObjective: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = bb_get_bool(*ab, HAS_OBJECTIVE_BB_KEY)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionBehaviourMoodSetAbove: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (static_cast<uint8_t>(as->behaviour_mood_set) >
                      static_cast<uint8_t>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 20 ─────────────────────────────────────────────────────────

        case BTNodeType::ActionSuccess:
            result = BTStatus::Success; pc = nd.parent; continue;

        case BTNodeType::ActionFail:
            result = BTStatus::Failure; pc = nd.parent; continue;

        case BTNodeType::DecoratorTimer: {
            uint8_t  tid    = static_cast<uint8_t>(nd.data >> 24);
            uint32_t dur_ms = nd.data & 0x00FFFFFFu;
            if (result == BTStatus::Running) {
                if (as && tid < MAX_AGENT_TIMERS) {
                    if (as->timers[tid] == 0u)
                        as->timers[tid] = static_cast<uint64_t>(nowMs) + dur_ms;
                    if (static_cast<uint64_t>(nowMs) >= as->timers[tid]) {
                        as->timers[tid] = 0u;
                        result = BTStatus::Success; pc = nd.parent; continue;
                    }
                }
                if (nd.childCount > 0) {
                    pc = m_children[nd.childStart]; result = BTStatus::Running; continue;
                }
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            if (as && tid < MAX_AGENT_TIMERS) as->timers[tid] = 0u;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionIdle:
            result = BTStatus::Running; goto exit_loop;

        case BTNodeType::ConditionTargetIsWithinDistance: {
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!wt || !sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx    = sc->last_known_x - wt->x;
            float dz    = sc->last_known_z - wt->z;
            float max_d = static_cast<float>(nd.data) / 10.f;
            result = (dx * dx + dz * dz <= max_d * max_d) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasAWeapon: {
            const WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (!wc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (wc->is_equipped || wc->weapon_type != 0) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionShouldProcessSuspiciousItem: {
            const NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nm) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (nm->event_count > 0) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 21 ─────────────────────────────────────────────────────────

        case BTNodeType::ActionMoveToTarget: {
            if (as) {
                as->frame_flags |= (1ull << ff::SHOULD_MOVE_TO_TARGET);
                as->target_speed = static_cast<LocomotionTargetSpeed>(nd.data & 0x03u);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ActionMakeAggressive: {
            if (as) {
                as->aggro_level = NpcAggroLevel::NoLimit;
                as->frame_flags |= (1ull << ff::SHOULD_MOVE_TO_TARGET);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ConditionAllowedToAttackTarget: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            bool aggro_ok = (static_cast<uint8_t>(as->aggro_level) >=
                             static_cast<uint8_t>(NpcAggroLevel::Warning));
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            bool enemy = false;
            if (ab) {
                const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
                if (en) {
                    MdEntity target     = MdRegistry::Get().FromIndex(en->val.e);
                    const AgentState* tas   = MdRegistry::Get().Valid(target)
                        ? MdRegistry::Get().Handle(target).try_get_mut<AgentState>() : nullptr;
                    if (tas) {
                        enemy = AllianceMatrix::Get().IsEnemy(
                            static_cast<AllianceGroup>(as->alliance_group),
                            static_cast<AllianceGroup>(tas->alliance_group));
                    }
                }
            }
            result = (aggro_ok && enemy) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionDead: {
            if (as) {
                as->lcflags.set(lcf::IS_DEAD);
                as->frame_flags |= (1ull << ff::SHOULD_PLAY_DEATH);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ActionMeleeAttack: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_MELEE_ATTACK);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsCurrentCoverValid: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::IS_IN_COVER) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 22 ─────────────────────────────────────────────────────────

        case BTNodeType::ConditionLastTimeSensed: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t  idx       = static_cast<uint8_t>(nd.data >> 24);
            uint32_t max_ms    = nd.data & 0x00FFFFFFu;
            if (idx >= MAX_SENSES) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint32_t last = sc->last_activated_ms[idx];
            result = (last != 0u && (nowMs - last) <= max_ms) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionPerformRole: {
            auto role = static_cast<NpcRole>(static_cast<uint8_t>(nd.data));
            bool ok   = RoleRegistry::Get().claim(role, 0u, e);
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionWasSenseThresholdLastIncreaseActivationAbove: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t sense_idx = static_cast<uint8_t>((nd.data >> 8) & 0xFu);
            auto    q         = static_cast<SenseThresholdQualifier>(nd.data & 0xFFu);
            if (sense_idx >= MAX_SENSES) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float act = sc->activation[sense_idx];
            bool  ok  = false;
            switch (q) {
                case SenseThresholdQualifier::Trace:     ok = (act > 0.0f);              break;
                case SenseThresholdQualifier::Lower:     ok = (act >= sc->threshold_lo); break;
                case SenseThresholdQualifier::Activated: ok = (act >= sc->threshold_hi); break;
                case SenseThresholdQualifier::Upper:     ok = (act >= 1.0f);             break;
            }
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionTargetsWeaponHasAmmo: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity target       = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(target)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WeaponComponent* wc = MdRegistry::Get().Handle(target).try_get_mut<WeaponComponent>();
            if (!wc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (!wc->needs_reload) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionTargetsWeaponHasProperty: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity target       = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(target)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WeaponComponent* wc = MdRegistry::Get().Handle(target).try_get_mut<WeaponComponent>();
            if (!wc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (wc->weapon_type == static_cast<uint8_t>(nd.data)) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasSearchedMostRecentSensedPosition: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::HAS_SEARCHED_RECENT_SENSED_POS) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 23 ─────────────────────────────────────────────────────────

        case BTNodeType::ActionIdleTimeFacingTarget: {
            if (st.timer == 0u) st.timer = nowMs + nd.data;
            if (static_cast<int32_t>(nowMs - st.timer) >= 0) {
                st.timer = 0u;
                result = BTStatus::Success;
            } else {
                if (as) as->frame_flags |= (1ull << ff::SHOULD_FACE_TARGET);
                result = BTStatus::Running;
            }
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionIdleTimeFacingTargetMostRecentSensedPosition: {
            if (st.timer == 0u) st.timer = nowMs + nd.data;
            if (static_cast<int32_t>(nowMs - st.timer) >= 0) {
                st.timer = 0u;
                result = BTStatus::Success;
            } else {
                if (as) as->frame_flags |= (1ull << ff::SHOULD_FACE_LAST_KNOWN_POS);
                result = BTStatus::Running;
            }
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionIdleTimeFacingSuspiciousItem: {
            if (st.timer == 0u) st.timer = nowMs + nd.data;
            if (static_cast<int32_t>(nowMs - st.timer) >= 0) {
                st.timer = 0u;
                result = BTStatus::Success;
            } else {
                if (as) as->frame_flags |= (1ull << ff::SHOULD_FACE_SI_POS);
                result = BTStatus::Running;
            }
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionRangedAim: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_RANGED_AIM);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionSuspiciousItemReaction: {
            if (as) {
                as->si_reaction = static_cast<SuspiciousItemReaction>(static_cast<uint8_t>(nd.data));
                as->frame_flags |= (1ull << ff::SI_REACTION_SET);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ActionSuspectTargetResponse: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_DO_SUSPECT_TARGET_RESPONSE);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        // ── Batch 24 ─────────────────────────────────────────────────────────

        case BTNodeType::ActionRequestCover: {
            if (as) as->frame_flags |= (1ull << ff::REQUESTING_COVER);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ConditionHasValidCoverToChangeTo: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (!as->lcflags.test(lcf::IS_IN_COVER)) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 25 ─────────────────────────────────────────────────────────

        case BTNodeType::ConditionHasScript: {
            const LuaScriptComponent* lsc = MdRegistry::Get().Handle(e).try_get_mut<LuaScriptComponent>();
            result = (lsc && lsc->script_id != 0u) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionScript: {
            const char* name = BTLuaScriptRegistry::Get().Lookup(nd.data);
            if (!name) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = LuaSystem::Get().CallAction(name, e);
            pc = nd.parent; continue;
        }

        // ── Batch 26 ─────────────────────────────────────────────────────────

        case BTNodeType::ConditionLastTimeSearchedWithinTime: {
            if (!as || as->last_searched_ms == 0u) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (nowMs - as->last_searched_ms <= nd.data) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionAllowedToSearch: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->awareness >= AwarenessState::Suspicious &&
                      !as->lcflags.test(lcf::IS_DEAD)) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasDoneSuspectResponseMoveTo: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::IS_DONE_SUSPECT_RESPONSE_MOVETO)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasDoneSuspectResponseWithinTime: {
            if (!as || as->event_ts[3] == 0u) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (nowMs - as->event_ts[3] <= nd.data) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasKilltrap: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = as->lcflags.test(lcf::HAS_KILLTRAP) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasMeleeAttackAvailableOrIsAttacking: {
            const WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            bool melee_ready   = (wc && wc->melee_available);
            bool is_attacking  = (as && (as->frame_flags & (1ull << ff::SHOULD_MELEE_ATTACK)));
            result = (melee_ready || is_attacking) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsBranchActive: {
            result = NamedBranchRegistry::Get().IsActive(nd.data) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsRequestingCover: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->frame_flags & (1ull << ff::REQUESTING_COVER)) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionAllowedToPursueTarget: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (static_cast<uint8_t>(as->aggro_level) >=
                      static_cast<uint8_t>(NpcAggroLevel::Warning))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 27 ─────────────────────────────────────────────────────────

        case BTNodeType::ActionBreakout: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_BREAKOUT);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionMoveToMostRecentSensedPosition: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_MOVE_TO_LAST_KNOWN);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionMoveToNearestStandingPointToTarget: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_MOVE_NEAR_TARGET);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionMoveToAttackTarget: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_ATTACK_MOVE);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionChangeCover: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_CHANGE_COVER);
            result = BTStatus::Running; goto exit_loop;
        }

        // ── Batch 28 ─────────────────────────────────────────────────────────

        case BTNodeType::ConditionIsInTargetsWeaponRange: {
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!wt || !sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx    = sc->last_known_x - wt->x;
            float dz    = sc->last_known_z - wt->z;
            float max_d = static_cast<float>(nd.data) / 10.f;
            result = (dx * dx + dz * dz <= max_d * max_d) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsCoverExposed: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!as || !sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            bool in_cover = as->lcflags.test(lcf::IS_IN_COVER);
            bool visible  = sc->activation[0] >= sc->threshold_hi;
            result = (in_cover && visible) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasLostTarget: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint32_t last_ms = sc->last_activated_ms[0];
            if (last_ms == 0u) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint32_t elapsed = nowMs - last_ms;
            result = (elapsed > nd.data) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionUpdateLastKnownPosition: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_UPDATE_LAST_KNOWN);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionRequestInvestigate: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_INVESTIGATE);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionMarkTargetLost: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_MARK_TARGET_LOST);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionForceRetreat: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_FORCE_RETREAT);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionHoldPosition: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_HOLD_POSITION);
            result = BTStatus::Running; goto exit_loop;
        }

        // ── Batch 29 ─────────────────────────────────────────────────────────

        case BTNodeType::ConditionNpcDevelopmentStageAbove: {
            const NpcDevelopmentComponent* nc = MdRegistry::Get().Handle(e).try_get_mut<NpcDevelopmentComponent>();
            if (!nc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto threshold = static_cast<NpcDevelopmentStage>(static_cast<uint16_t>(nd.data));
            result = (nc->stage > threshold) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionNpcHasAbility: {
            const NpcDevelopmentComponent* nc = MdRegistry::Get().Handle(e).try_get_mut<NpcDevelopmentComponent>();
            if (!nc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = nc->HasAbility(static_cast<uint16_t>(nd.data)) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionIsHostileToPlayer: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            bool enemy = AllianceMatrix::Get().IsEnemy(
                static_cast<AllianceGroup>(as->alliance_group),
                AllianceGroup::Player);
            result = enemy ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionPerformAmbush: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_PERFORM_AMBUSH);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionStartSearch: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_START_SEARCH);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionCallForHelp: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_CALL_FOR_HELP);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionTauntTarget: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_TAUNT_TARGET);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionSurrenderSelf: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_SURRENDER);
            result = BTStatus::Running; goto exit_loop;
        }

        // ── Batch 30 ─────────────────────────────────────────────────────────

        case BTNodeType::ConditionEventCountAbove: {
            const NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nm) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (nm->event_count > static_cast<uint8_t>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionSpatialMemoryCountAbove: {
            const NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
            if (!nm) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (nm->spatial_count > static_cast<uint8_t>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionMotivationTicksAbove: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->motivation_ticks > static_cast<uint8_t>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasVisualHistory: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (sc->last_activated_ms[0] > 0u) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionPursueTarget: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_PURSUE_TARGET);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionCircleTarget: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_CIRCLE_TARGET);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionBackOff: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_BACK_OFF);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionCrouchMove: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_CROUCH_MOVE);
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ActionVault: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_VAULT);
            result = BTStatus::Running; goto exit_loop;
        }

        // ── Batch 31: VentRegistry conditions ────────────────────────────────
        case BTNodeType::ConditionHasVentCloseToAlien: {
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            if (!wt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float radius_m = static_cast<float>(nd.data) / 10.f;
            result = VentRegistry::Get().AnyWithinRadius(wt->x, wt->z, radius_m)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasFlankedVentCloseToPlayer: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc || sc->last_activated_ms[0] == 0u) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float radius_m = static_cast<float>(nd.data) / 10.f;
            result = VentRegistry::Get().AnyWithinRadius(sc->last_known_x, sc->last_known_z, radius_m)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ConditionTargetIsOnlyAccessibleCrouching: {
            AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity target = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(target)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentState* tas = MdRegistry::Get().Handle(target).try_get_mut<AgentState>();
            if (!tas) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = tas->lcflags.test(lcf::IS_IN_VENT) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 35: remaining LOW-priority nodes ────────────────────────────
        case BTNodeType::ActionApplyPrimaryDamageControlResponse: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_APPLY_DAMAGE_CONTROL);
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionBrokenCover: {
            if (as) {
                as->lcflags.clear(lcf::IS_IN_COVER);
                as->frame_flags |= (1ull << ff::SHOULD_BREAKOUT);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::ActionMoveToObjective: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (as) as->frame_flags |= (1ull << ff::SHOULD_MOVE_TO_OBJECTIVE);
            if (ab) {
                const BlackboardEntry* ex = bb_find(*ab, SQUAD_TX_BB_KEY);
                const BlackboardEntry* ez = bb_find(*ab, SQUAD_TZ_BB_KEY);
                if (ex && ez) {
                    NavAgent* nav = MdRegistry::Get().Handle(e).try_get_mut<NavAgent>();
                    if (nav) { nav->target_x = ex->val.f; nav->target_z = ez->val.f; }
                }
            }
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionSetLogicCharacterFlags: {
            if (as) {
                uint8_t bit = static_cast<uint8_t>((nd.data >> 8) & 0x3Fu);
                as->lcflags.assign(bit, (nd.data & 0x1u) != 0u);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ConditionAllowedToDoSuspiciousWarning: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->aggro_level >= NpcAggroLevel::Warning &&
                      as->awareness  >= AwarenessState::Suspicious &&
                      !as->lcflags.test(lcf::IS_DEAD))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionCanTakeStep: {
            if (!as || as->lcflags.test(lcf::IS_DEAD)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            const BlackboardEntry* en = ab ? bb_find(*ab, TARGET_ENTITY_BB_KEY) : nullptr;
            bool has_target = (en && MdRegistry::Get().Valid(MdRegistry::Get().FromIndex(en->val.e)));
            result = (as->awareness >= AwarenessState::Suspicious && has_target)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionGameIsDifficulty: {
            result = BTStatus::Success; pc = nd.parent; continue;  // no difficulty system
        }
        case BTNodeType::ConditionHasAnySenseBeenAboveWithinTime: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto     q       = static_cast<SenseThresholdQualifier>((nd.data >> 24) & 0xFFu);
            uint32_t time_ms = nd.data & 0x00FFFFFFu;
            bool     found   = false;
            for (int si = 0; si < static_cast<int>(SenseType::COUNT); ++si) {
                if (sc->last_activated_ms[si] == 0u) continue;
                if ((nowMs - sc->last_activated_ms[si]) > time_ms) continue;
                float act = sc->activation[si]; bool ok = false;
                switch (q) {
                    case SenseThresholdQualifier::Trace:     ok = act > 0.f;              break;
                    case SenseThresholdQualifier::Lower:     ok = act >= sc->threshold_lo; break;
                    case SenseThresholdQualifier::Activated: ok = act >= sc->threshold_hi; break;
                    case SenseThresholdQualifier::Upper:     ok = act >= 1.0f;             break;
                }
                if (ok) { found = true; break; }
            }
            result = found ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionIsCorpseTrap: {
            result = (as && as->lcflags.test(lcf::IS_CORPSE_TRAP))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionIsGaugeAmountBelow: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto  gt  = static_cast<GaugeType>((nd.data >> 24) & 0xFFu);
            float thr = static_cast<float>(nd.data & 0x00FFFFFFu) / 1000.f;
            result = (as->gauges.get(gt) < thr) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionLastTimeWasAbleToShootTarget: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc || sc->last_activated_ms[0] == 0u) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = ((nowMs - sc->last_activated_ms[0]) <= nd.data)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionRequiresPrimaryDamageControlResponse: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->hp_pct <= static_cast<uint8_t>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetIsPlayer: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentState* tas = MdRegistry::Get().Handle(tgt).try_get_mut<AgentState>();
            result = (tas && tas->lcflags.test(lcf::IS_PLAYER))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetIsWithinDistanceUnobscured: {
            const WorldTransform* self_wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const SenseComponent* sc      = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            const AgentBlackboard* ab     = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!self_wt || !ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* twt = MdRegistry::Get().Handle(tgt).try_get_mut<WorldTransform>();
            if (!twt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx = twt->x - self_wt->x, dz = twt->z - self_wt->z;
            float max_d = static_cast<float>(nd.data) / 10.f;
            bool dist_ok = (dx*dx + dz*dz <= max_d*max_d);
            bool los_ok  = (!sc || sc->activation[0] >= sc->threshold_lo);
            result = (dist_ok && los_ok) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetLogicCharacterFlags: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentState* tas = MdRegistry::Get().Handle(tgt).try_get_mut<AgentState>();
            if (!tas) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t bit      = static_cast<uint8_t>((nd.data >> 8) & 0x3Fu);
            bool    check_set = (nd.data & 0x1u) != 0u;
            bool    flag_val  = tas->lcflags.test(bit);
            result = (flag_val == check_set) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        // ── Batch 34: RE MEDIUM-priority nodes ───────────────────────────────
        case BTNodeType::ActionForceSearch: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_FORCE_SEARCH);
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionResetSearchJobs: {
            if (as) {
                as->lcflags.clear(lcf::HAS_SEARCHED_RECENT_SENSED_POS);
                as->last_searched_ms = 0u;
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::ActionSetFrameFlag: {
            if (as && nd.data < 64u) as->frame_flags |= (1ull << static_cast<uint8_t>(nd.data));
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::ActionSetGaugeAmount: {
            if (as) {
                auto   gt  = static_cast<GaugeType>((nd.data >> 16) & 0xFFu);
                float  val = static_cast<float>(nd.data & 0xFFFFu) / 1000.f;
                as->gauges.set(gt, val < 0.f ? 0.f : val > 1.f ? 1.f : val);
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ConditionHasSenseActivationBeenAbove: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            uint8_t  idx = static_cast<uint8_t>((nd.data >> 8) & 0xFFu);
            auto     q   = static_cast<SenseThresholdQualifier>(nd.data & 0xFFu);
            if (idx >= static_cast<uint8_t>(SenseType::COUNT) ||
                sc->last_activated_ms[idx] == 0u) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float act = sc->activation[idx];
            bool  ok  = false;
            switch (q) {
                case SenseThresholdQualifier::Trace:     ok = act > 0.f;              break;
                case SenseThresholdQualifier::Lower:     ok = act >= sc->threshold_lo; break;
                case SenseThresholdQualifier::Activated: ok = act >= sc->threshold_hi; break;
                case SenseThresholdQualifier::Upper:     ok = act >= 1.0f;             break;
            }
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionIsCoverTooClose: {
            if (!as || !as->lcflags.test(lcf::IS_IN_COVER)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* self_wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!self_wt || !ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* twt = MdRegistry::Get().Handle(tgt).try_get_mut<WorldTransform>();
            if (!twt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx = twt->x - self_wt->x, dz = twt->z - self_wt->z;
            float min_d = static_cast<float>(nd.data) / 10.f;
            result = (dx*dx + dz*dz <= min_d*min_d) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionIsInCombatArea: {
            result = (as && as->awareness >= AwarenessState::Aware)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionMostRecentSenseActivationHasBeenAbove: {
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            if (!sc) { result = BTStatus::Failure; pc = nd.parent; continue; }
            auto q = static_cast<SenseThresholdQualifier>(nd.data & 0xFFu);
            // Find sense with most recent activation timestamp
            uint32_t best_ms = 0u; int best_i = -1;
            for (int si = 0; si < static_cast<int>(SenseType::COUNT); ++si) {
                if (sc->last_activated_ms[si] > best_ms) { best_ms = sc->last_activated_ms[si]; best_i = si; }
            }
            if (best_i < 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float act = sc->activation[best_i]; bool ok = false;
            switch (q) {
                case SenseThresholdQualifier::Trace:     ok = act > 0.f;              break;
                case SenseThresholdQualifier::Lower:     ok = act >= sc->threshold_lo; break;
                case SenseThresholdQualifier::Activated: ok = act >= sc->threshold_hi; break;
                case SenseThresholdQualifier::Upper:     ok = act >= 1.0f;             break;
            }
            result = ok ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionNeedsToGetOutOfTheWay: {
            result = (as && (as->frame_flags & (1ull << ff::SHOULD_GET_OUT_OF_WAY)))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionObjectiveIsInCombatArea: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, SQUAD_ACTIVITY_BB_KEY);
            // AttackMove = 9; squad committed to attack → objective is "in combat area"
            result = (en && en->val.i >= 9) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionObjectiveIsWithinDistance: {
            const WorldTransform* wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!wt || !ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* ex = bb_find(*ab, SQUAD_TX_BB_KEY);
            const BlackboardEntry* ez = bb_find(*ab, SQUAD_TZ_BB_KEY);
            if (!ex || !ez) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx = ex->val.f - wt->x, dz = ez->val.f - wt->z;
            float max_d = static_cast<float>(nd.data) / 10.f;
            result = (dx*dx + dz*dz <= max_d*max_d) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionShouldFollow: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, SQUAD_ACTIVITY_BB_KEY);
            result = (en && en->val.i >= 9) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetIsInCombatArea: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentState* tas = MdRegistry::Get().Handle(tgt).try_get_mut<AgentState>();
            result = (tas && tas->awareness >= AwarenessState::Aware)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetIsWithinAggroRadius: {
            const WorldTransform* self_wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!self_wt || !ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* twt = MdRegistry::Get().Handle(tgt).try_get_mut<WorldTransform>();
            if (!twt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx = twt->x - self_wt->x, dz = twt->z - self_wt->z;
            float r = static_cast<float>(nd.data) / 10.f;
            result = (dx*dx + dz*dz <= r*r) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetNearestStandPointIsWithinDistance: {
            const WorldTransform* self_wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!self_wt || !ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* twt = MdRegistry::Get().Handle(tgt).try_get_mut<WorldTransform>();
            if (!twt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx = twt->x - self_wt->x, dz = twt->z - self_wt->z;
            // +1m offset approximates nearest standing point around target body
            float max_d = static_cast<float>(nd.data) / 10.f + 1.0f;
            result = (dx*dx + dz*dz <= max_d*max_d) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::DecoratorSetSenseSet: {
            // Sense sets not implemented — passthrough, always runs child
            if (result == BTStatus::Running) {
                if (nd.childCount > 0) { pc = m_children[nd.childStart]; continue; }
                result = BTStatus::Success; pc = nd.parent; continue;
            }
            pc = nd.parent; continue;
        }
        case BTNodeType::DecoratorSuspiciousItemInProgress: {
            if (result == BTStatus::Running) {
                const NpcMemoryComponent* nm = MdRegistry::Get().Handle(e).try_get_mut<NpcMemoryComponent>();
                if (!nm || nm->event_count == 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
                if (nd.childCount > 0) { pc = m_children[nd.childStart]; continue; }
                result = BTStatus::Success; pc = nd.parent; continue;
            }
            pc = nd.parent; continue;
        }

        case BTNodeType::SelectorLinear: {
            if (result == BTStatus::Running) {
                st.currentChild = 0;
                if (nd.childCount == 0) { result = BTStatus::Failure; pc = nd.parent; continue; }
                pc = m_children[nd.childStart]; continue;
            }
            if (result == BTStatus::Success) { pc = nd.parent; continue; }
            if (++st.currentChild < nd.childCount) {
                pc = m_children[nd.childStart + st.currentChild];
                result = BTStatus::Running; continue;
            }
            result = BTStatus::Failure; pc = nd.parent; continue;
        }
        case BTNodeType::SequenceLinear: {
            if (result == BTStatus::Running) {
                st.currentChild = 0;
                if (nd.childCount == 0) { result = BTStatus::Success; pc = nd.parent; continue; }
                pc = m_children[nd.childStart]; continue;
            }
            if (result == BTStatus::Failure) { pc = nd.parent; continue; }
            if (++st.currentChild < nd.childCount) {
                pc = m_children[nd.childStart + st.currentChild];
                result = BTStatus::Running; continue;
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        // ── Batch 33: RE HIGH-priority combat nodes ───────────────────────────
        case BTNodeType::ActionAbortMeleeAttack: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_ABORT_MELEE);
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionGetOutOfTheWay: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_GET_OUT_OF_WAY);
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionHitTargetAndRun: {
            if (as) as->lcflags.set(lcf::SHOULD_HIT_AND_RUN);
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionMoveInDirection: {
            if (as) {
                as->move_direction = static_cast<uint8_t>(nd.data & 0x3u);
                as->frame_flags |= (1ull << ff::SHOULD_MOVE_IN_DIRECTION);
            }
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionSuspend: {
            if (as) as->lcflags.set(lcf::IS_SUSPENDED);
            result = BTStatus::Success; pc = nd.parent; continue;
        }
        case BTNodeType::ActionTakeStep: {
            if (as) as->frame_flags |= (1ull << ff::SHOULD_TAKE_STEP);
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionThreatAware: {
            if (as) {
                as->frame_flags |= (1ull << ff::SHOULD_THREAT_AWARE);
                as->lcflags.set(lcf::DOING_THREAT_ANIM);
            }
            result = BTStatus::Running; goto exit_loop;
        }
        case BTNodeType::ActionThreatEscalation: {
            if (as) {
                as->frame_flags |= (1ull << ff::SHOULD_THREAT_ESCALATE);
                if (as->aggro_level < NpcAggroLevel::NoLimit)
                    as->aggro_level = static_cast<NpcAggroLevel>(
                        static_cast<uint8_t>(as->aggro_level) + 1u);
            }
            result = BTStatus::Running; goto exit_loop;
        }

        case BTNodeType::ConditionCanShootNow: {
            const WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (!wc || !wc->is_equipped || wc->needs_reload) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            const BlackboardEntry* en = ab ? bb_find(*ab, TARGET_ENTITY_BB_KEY) : nullptr;
            if (!en || !MdRegistry::Get().Valid(MdRegistry::Get().FromIndex(en->val.e))) {
                result = BTStatus::Failure; pc = nd.parent; continue;
            }
            const SenseComponent* sc = MdRegistry::Get().Handle(e).try_get_mut<SenseComponent>();
            result = (!sc || sc->activation[0] >= 0.5f) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionCheckHealthState: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->hp_pct <= static_cast<uint8_t>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionHasGroupAwarenessState: {
            if (!as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (as->awareness >= static_cast<AwarenessState>(nd.data))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionHasMeleeBlockAvailable: {
            const WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (!wc || !as) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = (wc->melee_available && as->combat_state == NpcCombatState::Blocking)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionHasMeleeCounterAttackAvailable: {
            const WeaponComponent* wc = MdRegistry::Get().Handle(e).try_get_mut<WeaponComponent>();
            if (!wc || !wc->melee_available) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            const BlackboardEntry* en = ab ? bb_find(*ab, TARGET_ENTITY_BB_KEY) : nullptr;
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentState* tas = MdRegistry::Get().Handle(tgt).try_get_mut<AgentState>();
            result = (tas && (tas->frame_flags & (1ull << ff::SHOULD_MELEE_ATTACK)))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionLastTimeTargetShotAtMe: {
            if (!as || as->last_shot_at_ms == 0u) { result = BTStatus::Failure; pc = nd.parent; continue; }
            result = ((nowMs - as->last_shot_at_ms) <= nd.data)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetIsInWeaponRange: {
            const WorldTransform* self_wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!self_wt || !ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* twt = MdRegistry::Get().Handle(tgt).try_get_mut<WorldTransform>();
            if (!twt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            float dx = twt->x - self_wt->x, dz = twt->z - self_wt->z;
            const WeaponComponent* twc = MdRegistry::Get().Handle(tgt).try_get_mut<WeaponComponent>();
            float range = twc ? twc->attack_range : 1.5f;
            result = (dx*dx + dz*dz <= range*range) ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetIsTargetingMe: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            // IS_TARGETED is set on self by CombatSystem when tgt->Combat.target == e
            result = (as && as->lcflags.test(lcf::IS_TARGETED))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::ConditionTargetIsUsingMeleeAttack: {
            const AgentBlackboard* ab = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity tgt = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(tgt)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const AgentState* tas = MdRegistry::Get().Handle(tgt).try_get_mut<AgentState>();
            result = (tas && (tas->frame_flags & (1ull << ff::SHOULD_MELEE_ATTACK)))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
        case BTNodeType::DecoratorLoop: {
            if (result == BTStatus::Running) {
                // NOTE: this branch is reached both on a genuine first entry
                // AND on every external Tick() that resumes an in-flight
                // multi-tick child (the stackless VM re-descends from m_root
                // every call) -- the two are indistinguishable here, exactly
                // like the sibling Repeat node (bt_vm_core.inc). Resetting
                // st.counter=0 unconditionally (as this used to) means an
                // N>0 loop wrapping any child that ever returns Running loses
                // its iteration count every tick and can never reach N via
                // count, contradicting "loops child N times". st.counter is
                // zero-initialized by the BehaviorTree ctor and only ever
                // touched in the completion branches below, matching Repeat.
                if (nd.childCount > 0) { pc = m_children[nd.childStart]; continue; }
                result = BTStatus::Success; pc = nd.parent; continue;
            }
            if (result == BTStatus::Failure) { st.counter = 0; pc = nd.parent; continue; }
            // Child succeeded: check loop count
            if (nd.data > 0u && ++st.counter >= static_cast<uint16_t>(nd.data)) {
                st.counter = 0;
                result = BTStatus::Success; pc = nd.parent; continue;
            }
            // Loop again -- fresh state for the child's new iteration.
            if (nd.childCount > 0) {
                uint16_t ci = m_children[nd.childStart];
                m_state[ci] = BTState{};
                pc = ci; result = BTStatus::Running; continue;
            }
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        // ── Batch 32: TokenRegistry ───────────────────────────────────────────
        case BTNodeType::ConditionHasToken: {
            result = MdTokenRegistry::Get().AcquireToken(nd.data, e)
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }

        case BTNodeType::ActionReleaseToken: {
            MdTokenRegistry::Get().ReleaseToken(nd.data, e);
            result = BTStatus::Success; pc = nd.parent; continue;
        }

        case BTNodeType::ConditionAngleNPCToTargetsAimLessThan: {
            const WorldTransform* self_wt = MdRegistry::Get().Handle(e).try_get_mut<WorldTransform>();
            AgentBlackboard*      ab      = MdRegistry::Get().Handle(e).try_get_mut<AgentBlackboard>();
            if (!self_wt || !ab) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const BlackboardEntry* en = bb_find(*ab, TARGET_ENTITY_BB_KEY);
            if (!en) { result = BTStatus::Failure; pc = nd.parent; continue; }
            MdEntity target = MdRegistry::Get().FromIndex(en->val.e);
            if (!MdRegistry::Get().Valid(target)) { result = BTStatus::Failure; pc = nd.parent; continue; }
            const WorldTransform* twt = MdRegistry::Get().Handle(target).try_get_mut<WorldTransform>();
            if (!twt) { result = BTStatus::Failure; pc = nd.parent; continue; }
            // target's forward direction from rot_y
            float tf_x = md::fsin_fast(twt->rot_y);
            float tf_z = md::fcos_fast(twt->rot_y);
            // direction from target to self (NPC)
            float dx = self_wt->x - twt->x;
            float dz = self_wt->z - twt->z;
            float len_sq = dx * dx + dz * dz;
            if (len_sq < 1e-6f) { result = BTStatus::Success; pc = nd.parent; continue; }
            float inv_len = md::frsqrt(len_sq);
            float dot = (tf_x * dx + tf_z * dz) * inv_len;
            float angle_deg = acosf(dot < -1.f ? -1.f : dot > 1.f ? 1.f : dot)
                              * (180.f / 3.14159265f);
            result = (angle_deg <= static_cast<float>(static_cast<uint8_t>(nd.data)))
                     ? BTStatus::Success : BTStatus::Failure;
            pc = nd.parent; continue;
        }
