---
id: kb-engine-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-09-17
repo: engine
tags: [engine, readme, sdl-gpu, rendering, ecs, public-repo]
summary: "Public engine/ README: feature list (rendering/AI/ECS/physics/terrain/nav/audio/scripting), build, repo layout"
---

# monkey_dust Engine — C++17 Game Engine (SDL_GPU/Vulkan)

Open-source C++17 game engine library for a Flare-inspired isometric RPG sandbox.
Built around **[SDL3](https://github.com/libsdl-org/SDL) + SDL\_GPU (Vulkan)**,
**[gaia-ecs](https://github.com/richardbiely/gaia-ecs) ECS**, a custom stackless
**Behavior Tree VM**, **[ozz-animation](https://github.com/guillaumeblanc/ozz-animation)**,
and **[Jolt Physics](https://github.com/jrouwe/JoltPhysics)**.

> **Render backend: SDL3/SDL_GPU, permanently.**

> **Full documentation →** [rdga1afk.github.io/mnkey\_dst\_engine/monkey\_dust\_docs.html](https://rdga1afk.github.io/mnkey_dst_engine/monkey_dust_docs.html)

---

## Features

### Rendering — SDL\_GPU / Vulkan
| System | Details |
|--------|---------|
| Tile map renderer | FLARE-inspired isometric tiles; TINST stride=36; `uint64_t` depth sort (eliminates Z-fight at tile edges); billboard + flat-XZ; fringe layers; `fadeOverlapTile` (player under roof = 35 % alpha); 4-phase pipeline (`SetObjectLayerIdx`) |
| OIT | 2-MRT weighted blended OIT: RGBA16F accum + R8G8B8A8 revealage; **compute composite** (avoids Intel HD 520 driver crash with 2+ fragment samplers); depth test against opaque scene |
| Deferred lighting | GBuffer 2-RT (RT0=albedo+rough, RT1=oct-normal+metallic+flags); single fullscreen `DeferredLightingSystem::DrawAmbientPass` → RGBA16F hdr\_color (M57; replaced the earlier separate PointLightSystem/StripLightSystem icosphere/capsule-SDF passes, both removed as dead code); ACES tonemapping |
| Cascaded Shadow Maps | 3-cascade **EVSM** soft shadows; texel-snap (eliminates shimmer); 10 % cascade blend overlap; GPU compute culling (`shadow_cull.comp`) |
| SSAO | Half-res R8 compute pass; 16-tap hemisphere kernel |
| GPU skinning | AnimationSoA; SSBO skeletal bones (MAX\_BONES=64, reference rig uses 30 of 64); compute dispatch |
| Particles | ParticleSoA CPU-sim; SMOKE/SPARK/BLOOD types |
| Material system | O3DE-inspired: JSON → `GpuPipeline::Desc`; **parent inheritance** (`"parent": "base_pbr"`); `shader_features` bitmask; `MaterialTypeRegistry` (MAX=32) |
| `GpuDevice` thread-safety | `AcquireCommandBuffer`/`Submit`/`SubmitAndAcquireFence`/`BeginFrame` are `std::mutex`-guarded (2026-09-12) — safe to call from a background loader thread concurrently with the main render thread's per-frame submissions; also serializes the underlying Vulkan queue submission, which requires external synchronization regardless of this class's own bookkeeping |
| Terrain geometry + shading | **Sole geometry system:** flat fixed-depth tiling (`terrain_quadtree.vert/.frag`, plain GLSL — Slang removed 2026-07-26; geometry simplified 2026-09-04): `TerrainQuadtree::SelectVisible` walks a fixed depth (`kFlatLodDepth=3`) out to `kFlatMaxRenderDistance=3000 m` — no adaptive subdivision, no geomorph blend, no skirts, no stitched-IBO seam handling (removed after RMSE-verified pixel-identical output at 2.3–3.6× lower render cost than the old adaptive quadtree); one shared filled index buffer per node depth. Two candidate replacements were explored and deleted after regressions/redesign didn't clear re-verification: `TerrainProjectedGrid` (TPG, 2026-09-12 → deleted 2026-09-17) and a Delaunay/TIN adaptive mesh (deleted 2026-09-18) — see `CLAUDE_HISTORY.md` for both post-mortems. Ground shading (`TerrainShadingProjected`, screen-space G-buffer resolve) is shared and independent of geometry; matches the original reference material behaviour (same material at all tiers, no POM/normal-mapping split), per-pixel dominant-weight selection (base/slope/cliff/grass/dirt/road) |
| Zone-corner cliff bake (2026-09-16, redesigned 2026-09-17) | Load-time compute-bake (`terrain_zone_corner_bake.comp`) precomputes the 4-corner tent-blend cliff shading for 1446/4225 real biome-convergent zone corners into a packed atlas — 1 atlas lookup instead of up to ~12 live texture samples for flagged near-range pixels. Original version baked each 128px tile across the full 460.8m zone-cell (~3.6m/texel) and regressed the same day (smeared cliff colour); redesigned to bake a static 48m window centred on the grid vertex instead (~9.6× finer texel density, same atlas memory budget), live per-zone fallback outside the window — live-verified (magenta shader diagnostic confirmed engagement before trusting any screenshot diff) at the original regression's exact location, no smearing |
| Bake/live `cliff_w` single-source-of-truth (2026-09-24) | `TerrainRenderer::InitSteepnessSmoothed` loads a small (2048²) per-biome-radius smoothed-steepness companion texture (`md_ground_steepness_smoothed.png`, `tools/md_bake_ground_layers.py`); `terrain_shading_common.glsl`'s live `cliff_w` samples it instead of deriving steepness from the raw per-pixel normal, so it agrees with the offline flat-ground bake's own classification by construction — fixes a structural bake/live disagreement (measured 31%→11.6% after the fix) that showed as a soft dark smear on steep terrain. Also removed a fully dead normal-texture mip-chain (task #556) whose LOD-selection had been provably always-0 since a later fixed-depth quadtree migration — see `docs/BAKE_GROUND.md` §4.8 (main repo) |
| `ToroidalUpdate` primitive (2026-09-17) | `engine/include/monkey_dust/world/toroidal_update.h` — reusable wrap-addressed-cache helper: splits a camera-origin shift into ≤4 axis-aligned quad regions needing re-render, world-to-texel addressed. General-purpose, not terrain-specific; for any future toroidal cache (detail atlas, shadow/GI clipmaps) |
| GPU pipeline resource-safety validator (2026-09-17) | `engine/include/monkey_dust/render/gpu_pipeline_safety_check.h` — static `Validate{Compute,Graphics}PipelineDesc()` checks resource-category combinations against known-unsafe patterns on Intel Gen9 ANV (e.g. storage buffer + samplers/storage-texture in one compute pipeline — the exact combo that crashed `libvulkan_intel.so` twice during the zone-corner bake's development) BEFORE `Create()` runs, not via a live crash |
| Render-pass diagnostic | `RenderPassGraph::Register`/`SetEnabled`/`IsEnabled` — permanent, default-enabled per-pass on/off toggle (`md.set_render_pass_enabled`) wired into `draw_scene`/`draw_sky`/`draw_npc_forward`/`draw_terrain_props`/`draw_water`/`draw_hud`/`draw_player_cloth`/`draw_player_hair`/`cull_prepass`/`evsm_shadow`; used with `md.get_gpu_ms()` (`sync_timing_`) to decompose per-frame GPU cost live, no rebuild required |

### AI — Behavior Tree VM
- Stackless BT VM (`behavior_tree.h`, 373 lines core VM) + `bt_types.h` (987 lines — all enums/structs: BTNodeType, BTNode, BTState, etc.) + `bt_factories.cpp` (868 lines — Batch 2–35 factory methods) — 30+ node types, zero heap allocations
- Extended AI patterns (C1–C20) — MotivationType · LogicCharacterFlags · AgentTimerSlot · GaugeType · AwarenessState · AlertnessState · NpcMood · NpcRole · WithdrawState · EntityStateFlag
- BTNodePool — flat 32 KB arena bump-allocator
- BTSystem — 3-phase tick (frame\_flags reset → hint expiry → tree tick)
- BTJsonLoader — recursive strstr parser; 11 enum tables; no external JSON library
- NPC Archetypes (M54) — Guard/Alien/Vendor `BuildXxxBT()` helpers; breadth-first child ordering
- SenseSystem (M55) — `SenseSystemUpdate`; visual cone (dist+half-angle); audio linear 15 m falloff; rising-edge → `last_activated_ms` / `last_known_x/z`
- CombatDispatch (M56) — `ff::SHOULD_MELEE_ATTACK`/`SHOULD_RANGED_SHOOT`; target from AgentBlackboard; CalcDamage+RollHitZone; IS_DEAD on kill
- DirectorSystem — menace gauge `[0..1]`; 4 DirectorStages; `NpcConfig::bt_stage[4][32]` per-stage BT overrides
- UtilityScorer — Echo-inspired goal utility; motivation inertia bonus
- NpcMemoryComponent — `SpatialMemory[8]` + `events[8]` POD; no heap
- NpcInteractionComponent (M58) — `dialog_faction_id` + `interaction_range` (2.5 m) + `cooldown_ms`; 20 bytes
- FlowDurableTrigger — ref-counted durable triggers with duration decay

### ECS — gaia-ecs
Backed by [gaia-ecs](https://github.com/richardbiely/gaia-ecs) 1.0.0 (archetype-based, vendored
single-header in `third_party/gaia-ecs/`) behind the `MdRegistry`/`MdEntity` facade — no call site
touches gaia-ecs directly. `AllianceMatrix` and `NpcRelationshipComponent` use real
relation pairs (`(HostileWith/FriendlyWith, group)`, `(Trust/Fear, other)`) instead of fixed
arrays/matrices. 41 engine-side components in `engine/include/monkey_dust/components/`, incl.:
`WorldTransform` (via `ai_agent.h`) · `AIAgent` · `Health` · `Combat` · `Renderable` · `Building` ·
`Inventory` · `ProjectileComponent` · `SenseComponent` · `AgentState` · `NpcMemoryComponent` ·
`BehaviorTreeComponent` (`bt_component.h`/`bt_components.h`) · `FlareSpriteAnim` ·
`NpcInteractionComponent` · plus survival-sim-migration additions: `StatSheet`, `Equipment`, `Faction`,
`Squad`, `RaceDef`, `NpcNeeds`, `NpcRelationship`, `BleedComponent`, `BountyComponent`,
`InjuryState`, `MorphComponent`, `PrisonerComponent`, `ScheduleComponent`, `StealthComponent`,
`WeaponComponent`, and more.

gaia-ecs is the engine's sole ECS backend, replacing flecs entirely on 2026-09-10 after a
6-phase strangler-fig migration (relations, sorting, the JobGraph scheduler adapter
`gaia_sched_adapter.h`, and the standalone editor's `EcsReflectBridge` — by-name component
resolution, originally verified safe across the editor-panel `dlopen`/`dlclose` hot-reload
cycle that existed at the time; that hot-reload mechanism was removed entirely 2026-09-17,
so the by-name resolution now just runs once at startup) and full verification: 1629/1629
gtest, 3/3 `scenario_*`, live 40 s smoke (game+editor), ASan+UBSan clean through a real
`--exec` scenario, `.mdsave` v11 confirmed cross-backend compatible in both directions.
Measured: logic tick ~2.4× faster and far less noisy than flecs (0.7 ms stdev=0 vs
1.7 ms with real variance, 512 NPC), RSS parity, full build ~4× slower (gaia.h's 86k-line
single-include amalgamation has no PCH coverage — a real, accepted cost). Both backends
briefly coexisted behind a `-DMD_ECS_GAIA` build flag during the migration; flecs and the
flag were removed once the gaia default proved stable, since maintaining two backends
simultaneously was judged not worth the complexity for a solo project.

Two root-cause defects found in gaia-ecs's own core during this migration were reported upstream
and **fixed by the maintainer**: [#42](https://github.com/richardbiely/gaia-ecs/issues/42)
(archetype-move data corruption, fixed in `57603da`) and
[#43](https://github.com/richardbiely/gaia-ecs/issues/43) (`rem_from_entities` iterator
invalidation, fixed in `4c74879f`). A third real bug found — a confirmed data race in concurrent
`.each()` calls, [#39](https://github.com/richardbiely/gaia-ecs/issues/39) — is structurally closed
off in this codebase's own `JobGraph` dispatch (unconditionally sequential, never
concurrent) independent of upstream status; see `job_graph.cpp` for the full writeup.

### Physics & Animation
- **Jolt Physics** — `JoltWorld`: `CharacterVirtual` (max_bodies=512); `TempAllocatorImpl` 8 MB
- **ozz-animation** — `OzzAnimator`: Init/Blend/Sample/BlendAdditive; T2 async LOD tier, skeleton built runtime from GLB
- **DetourCrowd** — `CrowdSystem` ORCA; MAX_AGENTS=512
- **Foot IK** (M59) — analytic 2-bone foot placement; TerrainQuery ray-cast

### Terrain
- `TerrainQuery` singleton — single source of truth for terrain heights/normals/slopes
- `TerrainAtlas` — real reference elevation data (`world_hmap.r16`, raw uint16, 8256×8256 tiled, 64×64 zones × 129×129 verts/zone — matches the source game's own in-engine resolution); O(1) RAM lookup; dirty-zone partial save (editor brush) via a sparse `_edits.r32` overlay on top of the read-only base
- **No procedural terrain generation** — removed entirely (2026-07-19): real reference zone data covers every in-bounds chunk, so the old noise-fallback chain (missing-zone → neighbor-clone → `SimplexNoise2`/`FBM2` synthesis, plus the `TerrainMaster`/`md_master_hmap` macro-geography guide layer it depended on) was provably unreachable in real gameplay. `SimplexNoise2`/`FBM2` remain as generic noise primitives (test fixtures, `force_noise` mode) but no longer back any real-terrain code path.
- **`TerrainAtlas_SmoothBoundaries()`** — N=15 kernel blends zone boundary heights (eliminates the source game's fullmap 22m+ height-jump seams → NdotL cliffs)
- **Flat fixed-depth LOD** (2026-09-04, replaces the earlier adaptive quadtree + `BuildLodIboStitched()` seam-stitch) — every visible node renders at the same fixed depth (`kFlatLodDepth=3`), so the T-junction/seam problem the old stitched-IBO approach solved no longer arises by construction; validated pixel-identical (RMSE=0.0) against the old adaptive output across 4 diverse locations before the ~300-line adaptive-subdivision/geomorph/skirt code path was deleted. **This is `TerrainQuadtree`, the sole current runtime geometry system** — both explored replacements (`TerrainProjectedGrid`/TPG, screen-space projected grid; a Delaunay/TIN adaptive mesh) were deleted after regressions/redesign didn't clear re-verification (2026-09-17 and 2026-09-18 respectively); see `CLAUDE_HISTORY.md` for both post-mortems.
- Cross-chunk normal stitching via atlas (no file I/O, no seam artefacts)
- `TerrainGen_Build` / `TerrainGen_Upload` — worker-thread mesh gen + GPU upload
- **Clutter** — dense procedural ground-clutter placement (`ClutterGen`) removed entirely 2026-09-05; `FeatureScatterSystem` (757 real `features.dat` placements) is the sole scatter-object source
- **`PoissonScatter`** — Bridson O(n) Poisson Disk Sampling for prop placement; min-distance guarantee; slope + embed constraints; deterministic seed; used for mixed rock+vegetation scatter

### Navigation
- Recast/Detour integration; async SPSC pathfinding worker
- `PathCache` with spinlock (`_mm_pause()`); `MAX_PATH_LEN=64`
- NavMesh rebuild enqueue (`EnqueueRebuild`) from BuildSystem/editor

### Audio — miniaudio
- `AudioSystem`: SFX pool (MAX\_SFX=32, BSS); music + ambience streaming
- `AudioHandle` = opaque int — no miniaudio types leak to public headers
- Lua API: `md_play_sfx` / `md_play_music` / `md_play_ambience` / `md_stop_*` / `md_set_volume`

### Scripting — Lua 5.4
- 8 MB custom allocator; io/os/package/debug sandboxed out
- `LuaEventBus` (MAX\_HANDLERS=64 BSS); `FlowGraph` FNV-1a node IDs; ring buffer
- `AgentBlackboard` (MAX\_ENTRIES=24; FNV-1a keys)
- **`MdEventScheduler`** — Flare EventComponent-inspired: OneShot / Cooldown / Delayed / Repeating / FireOnLoad / FireOnClear; `RequestFire()` for manual cooldown trigger

### World Simulation
- `WorldSimulation` 1 Hz tick: `FactionState[8]` + `TradeRoute[32]`; gold/prosperity/aggression/population economy
- `FactionSystem` — relation matrix `[-100..100]`; JSON loader
- `BuildSystem` — 200×200 grid; `ProductionChain` (inputs → outputs, timed cycles)
- **`SettlementPlacer`** — procedural settlement generation: stratified grid sampling + flatness scoring + biome weights; greedy 500 m minimum gap between sites; `FactionVoronoi` assignment by nearest faction seed; returns `SettlementCandidate[]` sorted by score
- `SaveSystem` v10 — CRC32 header; async save; `AgentState` + `NpcMemoryComponent` inline per NPC record; `WorldSimulation` faction/trade tail
- **`SaveVersionChain`** — O3DE-inspired post-load version converter chain (`Register(from,to,fn)` + `RunUpgrades`); eliminates growing `is_vN` branches for future versions
- **`MdStatusRegistry`** — Flare CampaignManager-inspired string-based status flags; `GetAllCSV`/`SetAllCSV` for human-readable saves; FNV-1a hashed IDs; MAX=128
- **`MdPrefabRegistry`** — data-driven NPC archetypes via `data/prefabs/prefabs.json`; `MdPrefab` {name, bt\_template, hp, wander\_radius, combat\_profile}; MAX=32

### Config & Features
- **`MdIniReader`** — zelda3-inspired INI parser; sections, `key=value`, ParseBool (0/1/yes/no/true/false/on/off), `!include`, `monkey_dust.user.ini` override fallback
- **`MdFeature`** uint32 bitmask — 8 accessibility toggles (DisableLowHealthBeep, SkipIntroOnKeypress, ShowFps, etc.); `MdFeaturesLoad(ini)` from `[Features]` INI section
- **`MdModuleRegistry`** — O3DE Gem-inspired plug-in lifecycle for `tools/` targets; `Register/Load/Unload/UnloadAll` (reverse order); `data/modules/*.module.json` metadata

### GPU Dev Tooling
- **`GpuPipeline::Reload()`** — invalidates SPV cache entry + destroys + re-reads SPIR-V from disk; safe to call mid-session for hot-reload
- **`MdSpvCache_Invalidate(path)`** — evicts one SPIR-V cache entry by glsl path (e.g. `"shaders/char_hair.frag"`)
- **`MdSpvCache_Shutdown()`** — releases all cached bytecode at exit
- `SkinMesh::LoadGLB` validates NORMAL / JOINTS / WEIGHTS attributes and emits `[SkinMesh] WARN` if missing (missing normals → `normalize(0)` → NaN → white fragments)
- Hair shader (`char_hair.vert/.frag`) — Lambert only (Kajiya-Kay removed: V≈−L → normalize(0) → NaN); depth bias `gl_Position.z -= 0.0003*w` prevents Z-fighting against head geometry

### Performance
- AVX2 `BulkComputeDistSq` / `BulkComputeLOD` (`_mm256_fmadd_ps`, `alignas(64)` SoA)
- `MD_HOT` / `MD_LIKELY` / `MD_UNLIKELY` / `MD_FORCE_INLINE` compiler hints (`md_hints.h`)
- TINST dirty-flag — skip GPU upload when tiles unchanged
- TransformSoA faction dirty — range upload (not full 65536-slot buffer)
- Hot-reload file watcher (POSIX `stat` + `SDL_Thread`); live BT JSON reload
- **`TS_ComputeGroundAlbedo`** ground shading rewritten on Granite's (`github.com/Themaister/Granite`) branchless `mediump` splat-blend formula (2026-08-28): confirmed real ~12.4ms/frame cost at 1920×1056 via randomized A/B, root cause was 873-instruction old zone/cliff/detail formula hard-failing SIMD16 register allocation on Intel Gen9 ANV (permanently capped at SIMD8). New formula compiles to 265 instructions, `0:0 spills:fills`, and now succeeds through SIMD32 — verified live via `INTEL_DEBUG=fs`. Measured real FPS win (34-47 → 50-60 FPS). Old formula kept as `TS_ComputeGroundAlbedo_ZoneLegacy` for a planned phase-2 reintroduction of real per-region reference ground data on top of the new baseline.

---

## Target Hardware

Intel HD 520 (Skylake, AVX2) · Vulkan via SDL\_GPU · 4–8 GB shared RAM · 1280×720 · 60 FPS

---

## Build

```bash
# Static library only
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON
ninja -C build monkey_dust_engine
```

> **Shaders** live in the parent [`monkey_dust`](https://github.com/rdga1afk/mnkey_dst) game repository (`shaders/` + `scripts/compile_shaders.sh`).
> The engine library itself is shader-agnostic — it loads pre-compiled SPIR-V at runtime via `GpuPipeline::Create(desc)`.

**Dependencies** (bring your own or via CMake FetchContent):
`SDL3` · `gaia-ecs` (sole ECS backend) · `Recast/Detour` · `ozz-animation` · `JoltPhysics` · `miniaudio` · `Lua 5.4`

> **ImGui is NOT an engine dependency.** Dear ImGui and all extensions (imnodes, imgui-node-editor, ImGuiColorTextEdit, imguizmo, imgui-flame-graph, imgui-command-palette) live in `tools/third_party/`. The engine library has zero UI dependencies and is split-ready.

---

## Tests

This submodule's own test target is a small plain-C++ (no GTest) smoke-test pair:

```bash
ninja -C build md_tests          # meta-target, depends on flare_ini_parser + flare_tile_map
./build/tests/flare_ini_parser
./build/tests/flare_tile_map
```

> The large GTest suite (1830 tests: 1651 gtest + 179 behavior, gaia-ecs backend — FNV · AgentBlackboard · FlowGraph ·
> DirectorSystem · BT VM · Batch 3–31 · M47–M59 · O3DE-1–4 · ZLD-1–2 · FL-3–4 · REF-1–8 ·
> FX-R1–9 · FX-AI1–6, etc.) lives in the private parent
> [`monkey_dust`](https://github.com/rdga1afk/mnkey_dst) game repo's `tests/` directory, not in
> this engine submodule.

---

## Repository Layout

```
engine/
  include/monkey_dust/     ← public headers (install target)
    ai/                    ← BT VM, director, sense, utility scorer
    audio/                 ← AudioSystem
    building/              ← BuildSystem, ProductionChain
    combat/                ← damage_calc, hit_zones, power_def
    compat/                ← md_dirent.h (POSIX dirent shim)
    components/            ← 41 ECS components
    ecs/                   ← Registry (gaia::ecs::World singleton)
    editor/                ← EditorPanelRegistry (MAX_PANELS=16)
    flare/                 ← tile map, sprite animation, renderer
    hot/                   ← gameplay_module.h (libgameplay.so BT-binding hot-reload; the separate editor-panel .so hot-reload was removed 2026-09-17)
    math/                  ← md_fast_math.h, sin_lut.h (rsqrtps fast-math helpers)
    platform/math_types.h  ← Vec3/Mat4 (GLM switch -DUSE_GLM)
    nav/                   ← PathCache, CrowdSystem
    net/                   ← ReplaySnapshot (16B npc state + 1048B frame ring)
    nodegraph/             ← PCG node graph (noise/scatter/terrain tile gen)
    physics/               ← JoltWorld, Ragdoll
    platform/              ← input/audio/window/md_fs/md_log/md_hints/timing_system
    render/                ← GPU HAL, ring buffer, shadow, SSAO …
    save/                  ← SaveSystem v10 · SaveVersionChain
    scripting/             ← LuaSystem, LuaEventBus, FlowGraph
    spatial/               ← world_bvh.h
    tools/                 ← graphics_settings.h, hot_reload.h (editor-adjacent, engine-owned)
    world/                 ← FactionSystem, WorldSimulation, TransformSoA, SettlementPlacer, MdStatusRegistry, MdPrefabRegistry …
    prefab/                ← MdPrefabRegistry (data-driven NPC archetypes)
    module/                ← MdModuleRegistry (plug-in lifecycle)
  src/                     ← implementation units
  tests/                   ← Google Test suite
```

---

## License

MIT — see [LICENSE](LICENSE).

The game itself (`monkey_dust` executable and `game/` sources) is proprietary and not part of this repository.
