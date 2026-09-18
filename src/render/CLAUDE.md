# engine/src/render/ — CLAUDE.md

SDL_GPU HAL only — новий рендер-код через `GpuPipeline`/`GpuCommandBuffer`/
`GpuVertexBuffer` (`gpu_hal.h`), ніколи raw GL. SPIR-V: нові шейдери →
`scripts/compile_shaders.sh` → `.spv`; `#version 460`.

## SSBO binding map (канонічна, звіряти перед новим SSBO)
```
0  TransformSoA xzyr        4  finalBoneMatrices (AnimationSoA)
1  NPC cull vis             5  animState per-NPC
2  NPC indirect             6  shadow_vis
3  faction (uint/NPC)       7  shadow_indirect
8  AmbientProbe[64]
```
Fragment samplers/storage-buffers (set=2) — кожна категорія (семплери,
SSBO) послідовно від 0, без розривів між категоріями — навіть якщо
розрив "зайнятий" ресурсом іншого типу. Порушення = мовчазний нуль,
БЕЗ Vulkan validation помилки. SSBO — ПІСЛЯ всіх семплерів у нумерації.

## Intel HD 520 — ПЕРЕВІРЯТИ ПЕРЕД КОЖНИМ GPU PR
Повний чеклист: `cat CLAUDE.md` (корінь) §Intel HD 520. Найкритичніше:
- Depth format ТІЛЬКИ `D32_FLOAT` — `D24_UNORM` = GPU hang (Gen9 ANV).
- `texture(..., lod_bias>0)` = GPU hang (i915 ecode 9:1:85dffffb).
- `frag_uniform_bufs` МУСИТЬ = кількості `set=3 binding=N` у SPIR-V —
  розбіжність = silent garbage → NaN → білі фрагменти.
- OIT composite: ТІЛЬКИ compute path, fragment+2 samplers = crash.
- Mesa shader cache corruption після crash під час компіляції шейдера:
  `rm -rf ~/.cache/mesa_shader_cache/`.

## Термейн-рендер (Ogre-quadtree geomorph+skirts, з 2026-08-19)
`TerrainQuadtree`/`TerrainQuadtreeRenderer` — ПОТОЧНИЙ дефолтний
геометричний шлях (гра й редактор), замінив GEOCLIPMAP
(`TerrainClipmapCache/Renderer`, видалено) і non-indexed patch-grid
(`TerrainPatchGrid/Renderer`, видалено) після dual-run A/B. Indexed
(спільний index buffer, PTC-reuse), без персистентного tree-стану —
`SelectVisible` рахує видимі вузли з камери+frustum щокадру.
`TerrainWorldHeightmap` — незмінне джерело висот (окремо від quadtree).
`TerrainShadingProjected` (Варіант A, screen-space G-buffer resolve) —
незмінна незалежно від геометрії. Формула ground-layer шейдингу:
`shaders/terrain_shading_common.glsl`.

**Стратегічний напрямок (2026-09-18, рішення власника):**
`TerrainProjectedGrid` (TPG) і `TerrainTinMesh`/`TerrainTinRenderer`
(TIN) — ОБИДВА ВИДАЛЕНІ повністю (TPG 2026-09-17: 2 непочинені
регресії; TIN 2026-09-18: обидва шви зрештою виправлені, але FPS-
паритет з `TerrainQuadtree`, не виграш, означав, що 4-кроковий
bake→stitch→load→draw pipeline не окупав себе проти `TerrainQuadtree`'s
1-кроку — `docs/TIN_ETAP2_PLAN.md` Stage 5/6). `TerrainQuadtree` —
ЄДИНА термейн-система, історичні журнали обох спроб лишаються
(`docs/TERRAIN_PROJECTED_GRID.md`, `docs/TIN_ETAP2_PLAN.md`). Деталі:
`CLAUDE_STATE.md` (корінь репо), `docs/CLAUDE_RENDER.md`,
`docs/CLAUDE_TERRAIN_SEAM.md`.

## GPU Debug — обов'язковий порядок перед фіксом шейдера
```
1. fragColor = vec4(N*0.5+0.5, 1.0)   ← нормалі OK?
2. fragColor = vec4(uf.some_color.rgb, 1.0)  ← UBO читається правильно?
3. fragColor = vec4(1,0,1,1)          ← шейдер взагалі запускається?
```
RenderDoc: `SDL_GPU_DEBUGMODE=1 renderdoccmd capture -w -d <repo> <bin>`.
Hot-reload шейдерів: `bash tools/shader_watch.sh` + `/reload-shaders`.

Глибокий довідник: `docs/CLAUDE_RENDER.md`, `docs/CLAUDE_SDL_GPU_PREP_MD.md`.
