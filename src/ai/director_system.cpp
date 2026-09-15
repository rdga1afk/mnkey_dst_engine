#include <monkey_dust/ai/director_system.h>
#include <monkey_dust/ai/utility_scorer.h>
#include <monkey_dust/ai/fnv.h>
#include <monkey_dust/platform/md_hints.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// strstr-based JSON parser — no external JSON libs (project rule).

DirectorSystem& DirectorSystem::Get() {
    static DirectorSystem inst;
    return inst;
}

const DirectorProfile* DirectorSystem::GetCurrentProfile() const {
    if (profile_count_ == 0) return nullptr;
    return &profiles_[profile_idx_];
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

static float read_json_float(const char* block, const char* key) {
    const char* p = strstr(block, key);
    if (!p) return 0.f;
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') ++p;
    return static_cast<float>(atof(p));
}

static int read_json_int(const char* block, const char* key) {
    const char* p = strstr(block, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') ++p;
    return atoi(p);
}

static const char* read_json_str(const char* block, const char* key, char* out, int max) {
    const char* p = strstr(block, key);
    if (!p) { out[0] = '\0'; return block; }
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') ++p;
    if (*p == '"') ++p;
    int i = 0;
    while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
    out[i] = '\0';
    return p;
}

// ── Init / Load ───────────────────────────────────────────────────────────────

void DirectorSystem::Init(const char* json_path) {
    profile_count_ = 0;
    profile_idx_   = 0;
    menace_        = 0.f;
    stage_         = DirectorStage::Unaware;

    FILE* f = fopen(json_path, "rb");
    if (!f) {
        MD_LOG(MD_LOG_WARNING, "DirectorSystem: cannot open '%s' — using defaults", json_path);
        // Fallback default profile so Tick() is safe even without a file.
        DirectorProfile& def = profiles_[0];
        memset(&def, 0, sizeof(def));
        strncpy(def.name, "default", sizeof(def.name) - 1);
        def.gauge_fill_rate    = 0.05f;
        def.hunt_timeout_min_s = 30.f;
        def.hunt_timeout_max_s = 90.f;
        def.ambush_wait_s      = 15.f;
        def.trap_trigger_s     = 5.f;
        def.sweep_radius_m     = 20.f;
        def.max_menaces        = 1;
        profile_count_         = 1;
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 32768) {
        fclose(f);
        MD_LOG(MD_LOG_WARNING, "DirectorSystem: '%s' too large or empty", json_path);
        return;
    }
    char* buf = static_cast<char*>(malloc(static_cast<size_t>(sz) + 1));
    if (!buf) { fclose(f); return; }
    fread(buf, 1, static_cast<size_t>(sz), f);
    buf[sz] = '\0';
    fclose(f);

    // Iterate "name" keys inside the profiles array
    const char* p = buf;
    while (profile_count_ < MAX_PROFILES) {
        const char* name_key = strstr(p, "\"name\"");
        if (!name_key) break;

        // Find the enclosing object: scan back to '{', forward to '}'
        const char* obj_start = name_key;
        while (obj_start > buf && *obj_start != '{') --obj_start;
        const char* obj_end = strstr(name_key, "}");
        if (!obj_end) obj_end = buf + sz;

        size_t block_len = static_cast<size_t>(obj_end - obj_start);
        if (block_len > 1023) block_len = 1023;
        char block[1024] = {};
        memcpy(block, obj_start, block_len);

        DirectorProfile& pr = profiles_[profile_count_];
        memset(&pr, 0, sizeof(pr));

        read_json_str(block, "\"name\"",            pr.name, sizeof(pr.name));
        pr.gauge_fill_rate    = read_json_float(block, "\"gauge_fill_rate\"");
        pr.hunt_timeout_min_s = read_json_float(block, "\"hunt_timeout_min_s\"");
        pr.hunt_timeout_max_s = read_json_float(block, "\"hunt_timeout_max_s\"");
        pr.ambush_wait_s      = read_json_float(block, "\"ambush_wait_s\"");
        pr.trap_trigger_s     = read_json_float(block, "\"trap_trigger_s\"");
        pr.sweep_radius_m     = read_json_float(block, "\"sweep_radius_m\"");
        pr.max_menaces        = read_json_int  (block, "\"max_menaces\"");

        if (pr.gauge_fill_rate <= 0.f) pr.gauge_fill_rate = 0.05f;

        ++profile_count_;
        p = obj_end + 1;
    }

    free(buf);
    MD_LOG(MD_LOG_INFO, "DirectorSystem: loaded %d profiles from '%s'", profile_count_, json_path);
}

// ── SetProfile ────────────────────────────────────────────────────────────────

void DirectorSystem::SetProfile(const char* name) {
    for (int i = 0; i < profile_count_; ++i) {
        if (strncmp(profiles_[i].name, name, sizeof(profiles_[0].name)) == 0) {
            profile_idx_ = i;
            return;
        }
    }
    MD_LOG(MD_LOG_WARNING, "DirectorSystem: profile '%s' not found", name);
}

// ── LoadConfig ────────────────────────────────────────────────────────────────

bool DirectorSystem::LoadConfig(const char* json_path) {
    FILE* f = fopen(json_path, "rb");
    if (!f) {
        MD_LOG(MD_LOG_WARNING, "DirectorSystem: no config at '%s' — using defaults", json_path);
        return false;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }
    char* buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    auto rf = [&](const char* k, float def) {
        const char* p = strstr(buf, k); if (!p) return def;
        p += strlen(k);
        while (*p == '"' || *p == ':' || *p == ' ') ++p;
        return (float)atof(p);
    };
    auto ri = [&](const char* k, int def) {
        const char* p = strstr(buf, k); if (!p) return def;
        p += strlen(k);
        while (*p == '"' || *p == ':' || *p == ' ') ++p;
        return atoi(p);
    };

    config_.menace_fill_rate          = rf("menace_fill_rate",          config_.menace_fill_rate);
    config_.menace_drain_rate         = rf("menace_drain_rate",         config_.menace_drain_rate);
    config_.stage_suspicious_thresh   = rf("stage_suspicious_thresh",   config_.stage_suspicious_thresh);
    config_.stage_hunting_thresh      = rf("stage_hunting_thresh",      config_.stage_hunting_thresh);
    config_.stage_intense_thresh      = rf("stage_intense_thresh",      config_.stage_intense_thresh);
    config_.escalation_mult           = rf("escalation_mult",           config_.escalation_mult);
    config_.spawn_radius_m            = rf("spawn_radius_m",            config_.spawn_radius_m);
    config_.despawn_radius_m          = rf("despawn_radius_m",          config_.despawn_radius_m);
    config_.max_spawns_per_wave       = ri("max_spawns_per_wave",       config_.max_spawns_per_wave);
    config_.spawn_cooldown_s          = rf("spawn_cooldown_s",          config_.spawn_cooldown_s);
    config_.max_active_threats        = ri("max_active_threats",        config_.max_active_threats);
    config_.chase_break_time_s        = rf("chase_break_time_s",        config_.chase_break_time_s);
    config_.search_timeout_s          = rf("search_timeout_s",          config_.search_timeout_s);
    config_.search_grid_size_m        = rf("search_grid_size_m",        config_.search_grid_size_m);
    config_.patrol_radius_m           = rf("patrol_radius_m",           config_.patrol_radius_m);
    config_.alert_propagation_range_m = rf("alert_propagation_range_m", config_.alert_propagation_range_m);
    config_.flee_hp_fraction          = rf("flee_hp_fraction",          config_.flee_hp_fraction);
    config_.backup_request_range_m    = rf("backup_request_range_m",    config_.backup_request_range_m);
    config_.ambush_setup_time_s       = rf("ambush_setup_time_s",       config_.ambush_setup_time_s);
    config_.trap_trigger_s            = rf("trap_trigger_s",            config_.trap_trigger_s);
    config_.awareness_decay_s         = rf("awareness_decay_s",         config_.awareness_decay_s);
    config_.hearing_decay_s           = rf("hearing_decay_s",           config_.hearing_decay_s);
    config_.visual_decay_s            = rf("visual_decay_s",            config_.visual_decay_s);
    config_.min_quiet_s               = rf("min_quiet_s",               config_.min_quiet_s);
    config_.max_idle_menace           = rf("max_idle_menace",           config_.max_idle_menace);
    config_.director_tick_s           = rf("director_tick_s",           config_.director_tick_s);

    free(buf);
    return true;
}

// ── Tick ──────────────────────────────────────────────────────────────────────

static constexpr uint32_t K_MENACE = md::fnv1a("menace");
static constexpr uint32_t K_STAGE  = md::fnv1a("director_stage");

// ── M52: ChooseMotivation ─────────────────────────────────────────────────────

MotivationType DirectorSystem::ChooseMotivation(AgentState& as,
                                                 uint32_t now_ms) noexcept {
    MotivationType chosen = UtilityScorer::SelectMotivation(as, now_ms);
    if (chosen == as.motivation) {
        // Same motivation: grow inertia counter, cap at 255 to prevent overflow.
        if (as.motivation_ticks < 255u) ++as.motivation_ticks;
    } else {
        // Motivation changed: commit and reset inertia so the new choice starts fresh.
        as.motivation       = chosen;
        as.motivation_ticks = 0;
    }
    return chosen;
}

void DirectorSystem::AdjustMenace(float delta, uint8_t mode) {
    if      (mode == 1) menace_  = delta;
    else if (mode == 2) menace_ -= delta;
    else                menace_ += delta;
    menace_ = menace_ < 0.f ? 0.f : menace_ > 1.f ? 1.f : menace_;
}

void DirectorSystem::Tick(float dt) {
    if (profile_count_ == 0) return;
    const DirectorProfile& pr = profiles_[profile_idx_];

    // 1. Find max visual activation — stop early once max_menaces NPCs found.
    float max_activation = 0.f;
    auto& reg = MdRegistry::Get();
    {
        // B3.4: flecs query.each() has no early-exit, so this now always
        // scans every SenseComponent entity instead of stopping once
        // pr.max_menaces threat sources are found — max_activation ends
        // up identical either way (a simple running max), just computed
        // with a bit more work on ticks with many sensed entities.
        static auto q_menace = reg.Raw().query().all<SenseComponent&>();
        MdEach(q_menace, [&](MdEntity, SenseComponent& sc) {
            if (sc.activation[0] > max_activation)
                max_activation = sc.activation[0];
        });
    }

    // 2. Accumulate/decay menace using DirectorConfig rates.
    if (MD_UNLIKELY(max_activation > 0.3f)) {
        menace_ += config_.menace_fill_rate * dt;
    } else {
        menace_ -= config_.menace_drain_rate * dt;
        if (menace_ < config_.max_idle_menace)
            menace_ = config_.max_idle_menace;
    }
    menace_ = menace_ < 0.f ? 0.f : menace_ > 1.f ? 1.f : menace_;

    // 3. Update stage using config thresholds.
    if      (menace_ < config_.stage_suspicious_thresh) stage_ = DirectorStage::Unaware;
    else if (menace_ < config_.stage_hunting_thresh)    stage_ = DirectorStage::Suspicious;
    else if (menace_ < config_.stage_intense_thresh)    stage_ = DirectorStage::Hunting;
    else                                                 stage_ = DirectorStage::Intense;

    // 4. Broadcast to all AgentBlackboards — skip if nothing changed.
    // Threshold 0.005 avoids per-tick broadcast for tiny float drift.
    const bool menace_changed = (menace_ - last_bc_menace_) > 0.005f ||
                                (last_bc_menace_ - menace_) > 0.005f;
    const bool stage_changed  = stage_ != last_bc_stage_;
    if (menace_changed || stage_changed) {
        // Broadcast only to entities that have an AgentBlackboard (cold component).
        static auto q_bcast = reg.Raw().query().all<AgentBlackboard&>();
        MdEach(q_bcast, [&](MdEntity, AgentBlackboard& bb) {
            bb_set_float(bb, K_MENACE, menace_);
            bb_set_int  (bb, K_STAGE,  static_cast<int32_t>(stage_));
        });
        last_bc_menace_ = menace_;
        last_bc_stage_  = stage_;
    }
}

// ── VBfA Alliance Group Priority Scheduling ───────────────────────────────────
void DirectorSystem::RebuildGroupBudgets(const uint32_t* faction_ids, int count) {
    group_count_ = (count > MAX_FACTION_GROUPS) ? MAX_FACTION_GROUPS : count;
    int prio = GROUP_PRIORITY_START;
    for (int i = 0; i < group_count_; ++i, prio -= GROUP_PRIORITY_STEP) {
        group_budgets_[i].faction_id   = faction_ids[i];
        group_budgets_[i].priority     = prio;
        group_budgets_[i].unit_cap     = GROUP_UNIT_CAP;
        group_budgets_[i].units_active = 0;
    }
}

bool DirectorSystem::TryActivateUnit(uint32_t faction_id) {
    for (int i = 0; i < group_count_; ++i) {
        if (group_budgets_[i].faction_id == faction_id) {
            if (group_budgets_[i].units_active >= group_budgets_[i].unit_cap)
                return false;
            ++group_budgets_[i].units_active;
            return true;
        }
    }
    return true;  // faction not in table → no cap
}

void DirectorSystem::ResetGroupCounts() {
    for (int i = 0; i < group_count_; ++i)
        group_budgets_[i].units_active = 0;
}
