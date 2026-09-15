#include <monkey_dust/flare/flare_anim_system.h>
#include <monkey_dust/flare/sprite_animation.h>
#include <monkey_dust/flare/sprite_resolver.h>
#include <monkey_dust/flare/billboard_renderer.h>
#include <monkey_dust/components/flare_actor.h>
#include <monkey_dust/components/flare_sprite_anim.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>

namespace md::flare {

FlareAnimSystem& FlareAnimSystem::Get() {
    static FlareAnimSystem inst;
    return inst;
}

int FlareAnimSystem::GetOrCreateSlot(const char* full_path) {
    for (int i = 0; i < n_atlases_; ++i)
        if (strcmp(atlas_paths_[i], full_path) == 0) return i;
    if (n_atlases_ >= MAX_ATLAS) {
        MD_LOG(MD_LOG_WARNING,
               "[FlareAnimSystem] atlas limit reached (%d), %s → slot 0",
               MAX_ATLAS, full_path);
        return 0;
    }
    int slot = n_atlases_++;
    strncpy(atlas_paths_[slot], full_path, sizeof(atlas_paths_[0]) - 1);
    atlas_paths_[slot][sizeof(atlas_paths_[0]) - 1] = '\0';
    BillboardRenderer::Get().LoadSpriteAtlas(full_path, slot);
    return slot;
}

void FlareAnimSystem::Init() {
    if (inited_) return;
    n_atlases_ = 0;
    memset(atlas_paths_, 0, sizeof(atlas_paths_));

    BillboardRenderer::Get().Init();

    auto& reg = MdRegistry::Get();
    static auto q_p3_flare_anim_system_1 = reg.Raw().query().all<FlareActorComponent&>().all<FlareSpriteAnim&>();
    MdEach(q_p3_flare_anim_system_1, [&](FlareActorComponent& fac, FlareSpriteAnim& sa) {
        if (sa.atlas_slot >= 0) return;
        const SpriteCategoryEntry* e =
            SpriteResolver::Get().Resolve(fac.category);
        if (!e) return;
        sa.atlas_slot = (int8_t)GetOrCreateSlot(e->atlas_full_path);
    });

    inited_ = true;
    MD_LOG(MD_LOG_INFO, "[FlareAnimSystem] Init complete: %d atlas(es)", n_atlases_);
}

void FlareAnimSystem::Tick(float dt_ms) {
    if (!inited_) return;
    auto& reg = MdRegistry::Get();
    static auto q_p3_flare_anim_system_2 = reg.Raw().query().all<FlareActorComponent>().all<FlareSpriteAnim&>().all<NavAgent>().all<WorldTransform>();
    MdEach(q_p3_flare_anim_system_2, [&](const FlareActorComponent&,
                 FlareSpriteAnim& sa,
                 const NavAgent& nav,
                 const WorldTransform& tr)
    {
        if (!sa.anim.set) return;
        const SpriteAnimSet& set = *sa.anim.set;

        bool moving = (nav.path_idx < nav.path_len);
        if (moving) {
            float next_x     = nav.path[nav.path_idx * 3];
            float next_z     = nav.path[nav.path_idx * 3 + 2];
            sa.anim.dir      = ComputeDirection8(next_x - tr.x, next_z - tr.z);

            int run_idx = FindClip(set, "run");
            if (run_idx < 0) run_idx = FindClip(set, "move");
            if (run_idx >= 0) SetClip(sa.anim, run_idx);
        } else {
            int stance = FindClip(set, "stance");
            if (stance >= 0) SetClip(sa.anim, stance);
        }

        TickAnim(sa.anim, dt_ms);
    });
}

void FlareAnimSystem::SubmitBillboards(float tile_world_size) {
    if (!inited_) return;
    auto& reg = MdRegistry::Get();
    static auto q_p3_flare_anim_system_3 = reg.Raw().query().all<FlareSpriteAnim>().all<WorldTransform>();
    MdEach(q_p3_flare_anim_system_3, [&](const FlareSpriteAnim& sa, const WorldTransform& tr) {
        if (sa.atlas_slot < 0 || !sa.anim.set) return;
        const SpriteFrame* f = CurrentFrame(sa.anim);
        if (!f || f->w == 0 || f->h == 0) return;

        int   slot = sa.atlas_slot;
        float aw   = (float)BillboardRenderer::Get().AtlasWidth(slot);
        float ah   = (float)BillboardRenderer::Get().AtlasHeight(slot);
        if (aw <= 0.0f || ah <= 0.0f) return;

        // Flare UV formula (stbi flip active): v_gl = 1 - y_file / atlas_h
        float u0 = f->x           / aw;
        float v0 = 1.0f - (f->y + f->h) / ah;   // bottom-left in GL space
        float u1 = (f->x + f->w) / aw;
        float v1 = 1.0f - f->y           / ah;   // top-right in GL space

        // 96 px = 1 world unit (isometric convention, CLAUDE.md Y-extents)
        float scale = tile_world_size / 96.0f;
        float w = f->w * scale;
        float h = f->h * scale;

        BillboardInstance inst;
        inst.x         = tr.x;
        inst.y         = h * 0.5f;   // lift center to ground level
        inst.z         = tr.z;
        inst.width     = w;
        inst.height    = h;
        inst.u0        = u0;
        inst.v0        = v0;
        inst.u1        = u1;
        inst.v1        = v1;
        inst.r = inst.g = inst.b = inst.a = 255;
        inst.atlas_idx = (uint8_t)slot;

        BillboardRenderer::Get().Submit(inst);
    });
}

void FlareAnimSystem::Shutdown() {
    BillboardRenderer::Get().Shutdown();
    n_atlases_ = 0;
    inited_    = false;
}

} // namespace md::flare
