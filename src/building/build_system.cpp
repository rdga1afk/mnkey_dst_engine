#include <monkey_dust/building/build_system.h>
#include <monkey_dust/nav/nav_system.h>
#include <monkey_dust/render/particle_soa.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

static bool ParseInt(const char* p, const char* key, int& out) {
    const char* f = strstr(p, key); if (!f) return false;
    f += strlen(key);
    while (*f && (*f=='"'||*f==':'||*f==' ')) f++;
    if (!*f) return false;
    out = (int)strtol(f, nullptr, 10); return true;
}
static bool ParseFloat(const char* p, const char* key, float& out) {
    const char* f = strstr(p, key); if (!f) return false;
    f += strlen(key);
    while (*f && (*f=='"'||*f==':'||*f==' ')) f++;
    if (!*f) return false;
    out = (float)strtod(f, nullptr); return true;
}
static bool ParseStr(const char* p, const char* key, char* out, int max) {
    const char* f = strstr(p, key); if (!f) return false;
    f += strlen(key);
    const char* q = strchr(f, '"'); if (!q) return false; q++;
    int i = 0;
    while (*q && *q!='"' && i < max-1) out[i++] = *q++;
    out[i] = '\0'; return true;
}
static int ParseItemArray(const char* pos, const char* limit, ItemStack* out, int max_slots) {
    int count = 0; const char* cur = pos;
    while (count < max_slots) {
        const char* item_pos = strstr(cur, "\"item_id\"");
        if (!item_pos || (limit && item_pos >= limit)) break;
        int id=0, amt=0;
        ParseInt(item_pos, "\"item_id\"", id);
        const char* amt_pos = strstr(item_pos, "\"amount\"");
        if (!amt_pos || (limit && amt_pos >= limit)) break;
        ParseInt(amt_pos, "\"amount\"", amt);
        out[count].item_id = (uint32_t)id;
        out[count].amount  = amt;
        count++; cur = amt_pos + 8;
    }
    return count;
}

BuildSystem::BuildSystem() {
    memset(defs_, 0, sizeof(defs_));
    for (int x = 0; x < MAX_GRID; ++x)
        for (int z = 0; z < MAX_GRID; ++z)
            grid_[x][z] = MdEntity::Null();
}

int BuildSystem::LoadFromFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[BuildSystem] Cannot open: %s\n", path); return 0; }
    static char buf[16384];
    int bytes = (int)fread(buf, 1, sizeof(buf)-1, f); fclose(f); buf[bytes]='\0';
    def_count_ = 0; const char* cursor = buf;
    while (def_count_ < MAX_BUILDING_DEFS) {
        const char* id_pos = strstr(cursor, "\"id\""); if (!id_pos) break;
        const char* next_id = strstr(id_pos+4, "\"id\"");
        BuildingDef& d = defs_[def_count_]; memset(&d, 0, sizeof(d));
        int id_val=0;
        if (!ParseInt(id_pos, "\"id\"", id_val)) { cursor=id_pos+4; continue; }
        d.id = (uint32_t)id_val;
        ParseStr(id_pos, "\"name\"", d.name, sizeof(d.name));
        int sx=1, sz=1;
        ParseInt(id_pos, "\"size_x\"", sx); ParseInt(id_pos, "\"size_z\"", sz);
        d.size_x=sx; d.size_z=sz;
        const char* cost_pos = strstr(id_pos, "\"build_cost\"");
        if (cost_pos && (!next_id || cost_pos < next_id)) {
            const char* cost_end = strstr(cost_pos, "]");
            d.build_cost_count = ParseItemArray(cost_pos, cost_end, d.build_cost, PROD_MAX_SLOTS);
        }
        const char* prod_pos = strstr(id_pos, "\"production\"");
        if (prod_pos && (!next_id || prod_pos < next_id)) {
            const char* after = prod_pos+12;
            while (*after==' '||*after==':'||*after=='\t') after++;
            if (*after!='n') {
                const char* inp_pos = strstr(prod_pos, "\"inputs\"");
                if (inp_pos && (!next_id || inp_pos < next_id)) {
                    const char* inp_end = strstr(inp_pos, "]");
                    d.chain.input_count = ParseItemArray(inp_pos, inp_end, d.chain.inputs, PROD_MAX_SLOTS);
                }
                const char* out_pos = strstr(prod_pos, "\"outputs\"");
                if (out_pos && (!next_id || out_pos < next_id)) {
                    const char* out_end = strstr(out_pos, "]");
                    d.chain.output_count = ParseItemArray(out_pos, out_end, d.chain.outputs, PROD_MAX_SLOTS);
                }
                float ts=30.0f; int sc=0;
                ParseFloat(prod_pos, "\"time_s\"", ts);
                ParseInt(prod_pos, "\"storage_capacity\"", sc);
                d.chain.time_s=ts; d.chain.storage_capacity=sc;
                d.chain.max_output_per_cycle=0;
                for (int oi=0; oi<d.chain.output_count; ++oi)
                    d.chain.max_output_per_cycle += d.chain.outputs[oi].amount;
                d.chain.state=ProductionState::IDLE; d.chain.valid=true;
            }
        }
        d.loaded=true; def_count_++; cursor=id_pos+4;
    }
    fprintf(stdout, "[BuildSystem] Loaded %d defs from %s\n", def_count_, path);
    return def_count_;
}

const BuildingDef* BuildSystem::GetDef(uint32_t id) const {
    for (int i=0; i<def_count_; ++i) if (defs_[i].id==id) return &defs_[i];
    return nullptr;
}

MdEntity BuildSystem::Place(float wx, float wz, uint32_t def_id, Inventory& player_inv) {
    const BuildingDef* def = GetDef(def_id); if (!def) return MdEntity::Null();
    SnapToGrid(wx, wz); int gx, gz; WorldToGrid(wx, wz, gx, gz);
    for (int dx=0; dx<def->size_x; ++dx)
        for (int dz=0; dz<def->size_z; ++dz)
            if (IsCellOccupied(gx+dx, gz+dz)) return MdEntity::Null();
    for (int i=0; i<def->build_cost_count; ++i)
        if (player_inv.Count(def->build_cost[i].item_id) < def->build_cost[i].amount) return MdEntity::Null();
    for (int i=0; i<def->build_cost_count; ++i)
        player_inv.Take(def->build_cost[i].item_id, def->build_cost[i].amount);
    auto& reg = MdRegistry::Get(); auto e = reg.Create();
    Building b={}; b.def_id=def_id; b.grid_x=gx; b.grid_z=gz;
    b.size_x=def->size_x; b.size_z=def->size_z;
    b.chain=def->chain; b.progress_s=0.0f; b.active=def->chain.valid;
    reg.Handle(e).emplace<Building>(b);
    float cx=wx+(def->size_x-1)*0.5f, cz=wz+(def->size_z-1)*0.5f;
    reg.Handle(e).emplace<WorldTransform>(cx, 0.0f, cz, 0.0f);
    if (def->chain.valid) { Inventory inv={}; inv.Clear(); reg.Handle(e).emplace<Inventory>(inv); }
    for (int dx=0; dx<def->size_x; ++dx)
        for (int dz=0; dz<def->size_z; ++dz)
            grid_[gx+dx][gz+dz]=e;
    if (NavSystem::Get().IsReady()) {
        float obs_verts[12]={wx-0.5f,0,wz-0.5f,wx-0.5f+(float)def->size_x,0,wz-0.5f,
            wx-0.5f+(float)def->size_x,0,wz-0.5f+(float)def->size_z,wx-0.5f,0,wz-0.5f+(float)def->size_z};
        int obs_tris[6]={0,1,2,0,2,3};
        if (!NavSystem::Get().EnqueueRebuild(cx,cz,obs_verts,4,obs_tris,2))
            MD_LOG(MD_LOG_WARNING, "[BuildSystem] NavMesh queue full");
    }
    return e;
}

void BuildSystem::Demolish(MdEntity e, Inventory& player_inv) {
    auto& reg = MdRegistry::Get();
    if (!reg.Valid(e) || !(reg.Handle(e).has<Building>())) return;
    auto& b = reg.Handle(e).get_mut<Building>();
    const BuildingDef* def = GetDef(b.def_id);
    for (int dx=0; dx<b.size_x; ++dx)
        for (int dz=0; dz<b.size_z; ++dz) {
            int gx=b.grid_x+dx, gz=b.grid_z+dz;
            if (gx>=0&&gx<MAX_GRID&&gz>=0&&gz<MAX_GRID) grid_[gx][gz]=MdEntity::Null();
        }
    if (def) for (int i=0; i<def->build_cost_count; ++i) {
        int refund=def->build_cost[i].amount/2;
        if (refund>0) player_inv.Add(def->build_cost[i].item_id, refund);
    }
    if (NavSystem::Get().IsReady()) {
        const auto& tr = reg.Handle(e).get_mut<WorldTransform>();
        NavSystem::Get().EnqueueRebuild(tr.x, tr.z, nullptr, 0, nullptr, 0);
    }
    reg.Destroy(e);
}

void BuildSystem::RebuildGridFromEntities() {
    for (int x=0; x<MAX_GRID; ++x) for (int z=0; z<MAX_GRID; ++z) grid_[x][z]=MdEntity::Null();
    auto& reg = MdRegistry::Get();
#if defined(MD_ECS_GAIA)
    static auto q_p3_build_system_1 = reg.Raw().query().all<Building>();
#else
    static auto q_p3_build_system_1 = reg.Raw().query<Building>();
#endif
    MdEach(q_p3_build_system_1, [&](MdEntity e, const Building& b) {
        for (int dx=0; dx<b.size_x; ++dx) for (int dz=0; dz<b.size_z; ++dz) {
            int gx=b.grid_x+dx, gz=b.grid_z+dz;
            if (gx>=0&&gx<MAX_GRID&&gz>=0&&gz<MAX_GRID) grid_[gx][gz]=e;
        }
    });
    MD_LOG(MD_LOG_INFO, "[BuildSystem] Grid rebuilt from entities");
}

void BuildSystem::Tick(float dt_s) {
    auto& reg = MdRegistry::Get();
#if defined(MD_ECS_GAIA)
    static auto q_p3_build_system_2 = reg.Raw().query().all<Building&>().all<Inventory&>();
#else
    static auto q_p3_build_system_2 = reg.Raw().query<Building, Inventory>();
#endif
    MdEach(q_p3_build_system_2, [&](MdEntity be, Building& b, Inventory& inv) {
        if (!b.active || !b.chain.valid) return;
        if (b.chain.storage_capacity > 0) {
            int stored=0;
            for (int i=0; i<b.chain.output_count; ++i) stored+=inv.Count(b.chain.outputs[i].item_id);
            if (stored >= b.chain.storage_capacity) { b.chain.state=ProductionState::FULL; b.progress_s=0; return; }
        }
        if (b.chain.input_count > 0) {
            for (int i=0; i<b.chain.input_count; ++i)
                if (inv.Count(b.chain.inputs[i].item_id) < b.chain.inputs[i].amount) {
                    b.chain.state=ProductionState::NO_INPUT; b.progress_s=0; return; }
        }
        b.chain.state=ProductionState::RUNNING;
        b.progress_s += dt_s;
        if (b.progress_s < b.chain.time_s) return;
        for (int i=0; i<b.chain.input_count; ++i) inv.Take(b.chain.inputs[i].item_id, b.chain.inputs[i].amount);
        for (int i=0; i<b.chain.output_count; ++i) inv.Add(b.chain.outputs[i].item_id, b.chain.outputs[i].amount);
        b.progress_s=0;
        if (b.chain.cycles_done < 255) ++b.chain.cycles_done;
        // A-2: repeat / Permanent_Job logic
        if (b.chain.repeat_job) {
            if (b.chain.queued_cycles > 0) {
                --b.chain.queued_cycles;
                b.chain.state = (b.chain.queued_cycles == 0)
                              ? ProductionState::IDLE : ProductionState::QUEUED;
            }
            // queued_cycles==0 && repeat_job → Permanent_Job: stay RUNNING next tick
        } else {
            b.chain.state = ProductionState::IDLE;
        }
        if ((reg.Handle(be).has<WorldTransform>())) {
            const auto& btr = reg.Handle(be).get_mut<WorldTransform>();
            ParticleSoA::Get().Emit(btr.x, btr.y+1.5f, btr.z, 0.3f,1.5f,0.3f,180,180,180,200,1.5f,0.12f,3,ParticleType::SMOKE);
        }
    });
}
