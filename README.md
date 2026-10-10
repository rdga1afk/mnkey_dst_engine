---
id: kb-engine-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-10-10
repo: engine
tags: [engine, readme, kenshi, sdl-gpu, terrain, public-repo]
summary: "Public engine README (English; Ukrainian in README.uk.md): native Linux engine that renders Kenshi's world from the player's own install; status, goal, build, KENSHI_DIR, mods, architecture, module map, legal"
---

# monkey_dust Engine

**A native Linux C++17 engine that renders the world of [Kenshi](https://lofigames.com/) from your own installed copy of the game, built to run at 60 FPS on weak integrated GPUs (target: Intel HD 520).**

**English** · [Українською](README.uk.md) · [Docs site](https://rdga1afk.github.io/mnkey_dst_engine/)

---

> ### You need Kenshi
> This engine contains **no Kenshi data** and never will. It reads the game's files (FCS `.base`/`.mod`, heightmap, maps, later meshes and textures) at startup from **your own legal copy** of Kenshi (Steam or GOG), the same way [OpenMW](https://openmw.org/) works with Morrowind. Nothing is copied into this repository and the engine never writes to the Kenshi folder.

## What this is
- A **rendering engine for Kenshi's world on Linux**, native (no Wine/Proton at runtime), SDL3 + SDL_GPU (Vulkan).
- Its niche is **weak hardware**: full 1920×1080 at 60 FPS on an Intel HD 520 laptop iGPU.
- Content is edited with Kenshi's own editor, the **Forgotten Construction Set (FCS)**. Mods load in Kenshi's own order.

## What this is not (yet)
- **Not a playable game.** No NPCs, combat, trade or UI from Kenshi yet. Game logic comes later, step by step.
- **Not a Kenshi replacement or a remake.** Kenshi is © Lo-Fi Games. This is an independent, unofficial engine.
- **There is nothing to download and play.** The game executable lives in a separate private repository; this repository is the engine library.

## Status (2026-10-10)

| Area | State |
|---|---|
| FCS reader (`.base`/`.mod` v15–v17, `mods.cfg` load order, record merge) | **Done.** Checked on a real install: 20 481 records, 394 766 values, 0 mismatches against an independent dump |
| Biomes from FCS `BIOMES` records | **Done.** Table byte-identical to the previous offline table |
| Heightmap from `fullmap.tif` (16385², uint16) | **Done.** 68 161 536 heights, 0 differences from the offline copy |
| `biomemap.png`, `blendmap.png`, `overlaymaps/ambientmap.png` | **Done.** Read from the install, pixel-identical to the offline copies |
| Terrain renderer (Kenshi 6-neighbour normals, 1.8 m grid parity, slope/cliff layers, ambient map) | **Works**, visual parity in progress |
| Per-pixel biome blending by `blendMap` (as in Kenshi) | Next (milestone M2.2) |
| Towns (Ogre `.mesh`), foliage | Planned for Nov 2026 |
| Steam library auto-detection | Not yet: set `KENSHI_DIR` |
| Workshop mods | Code path exists, **not yet tested on real Workshop data** |
| **Performance, 1080p, terrain-only frames, HD 520** | **51–66 ms** per frame today. Target ≤ 16.7 ms. This is the main open problem |

## Goal by ~2026-12-10
**Kenshi's world on Linux at 60 FPS on an HD 520**, native 1920×1080: terrain + towns + foliage, no NPCs.
Acceptance: everything is read from the Kenshi folder; GPU p95 ≤ 16.7 ms at five fixed locations (three terrain points, one large town, one dense foliage area); no seams, no flicker.
Week-4 gate (2026-11-07): terrain alone ≤ 12 ms at the reference point, or the scope is revisited.

## Requirements
| | Minimum (target) |
|---|---|
| OS | Linux (developed on Arch Linux) |
| GPU | Vulkan, Intel HD 520 (Skylake Gen9) or better; Mesa ANV is the reference driver |
| CPU | x86-64 with AVX2 |
| RAM | not measured yet |
| Kenshi | Any legal install (Steam / GOG). Only its data files are read |

## Build
```bash
# Engine library (this repository)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON
ninja -C build monkey_dust_engine
ninja -C build md_tests            # small smoke tests
```
Dependencies: SDL3, gaia-ecs, Recast/Detour, ozz-animation, Jolt Physics, miniaudio, Lua 5.4 (vendored or via CMake).
Shaders (GLSL → SPIR-V) live in the game repository; the library loads pre-compiled SPIR-V at runtime.

## Pointing the engine at Kenshi
```bash
export KENSHI_DIR="$HOME/.local/share/Steam/steamapps/common/Kenshi"   # the folder that contains data/gamedata.base
```
- `md::KenshiDir()` returns `$KENSHI_DIR` (an empty value disables the install on purpose).
- `md::KenshiLandFile(name, fallback)` returns `<KENSHI_DIR>/data/newland/land/<name>` if it exists, else the fallback; the choice is logged once.
- Tests that need real game data run only when `KENSHI_DIR` is set.

## Mods
Mods are read exactly as Kenshi reads them: `data/mods.cfg` order, then `mods/` and Steam Workshop folders; records are merged in load order (`monkey_dust/io/fcs_gamedata.h`). Edit content with Kenshi's FCS. Workshop mods are not yet verified on real data.

## Architecture

**Three repositories:**

| Repository | Visibility | Licence | Contents |
|---|---|---|---|
| [`mnkey_dst_engine`](https://github.com/rdga1afk/mnkey_dst_engine) (this) | public | MIT | Engine library: Kenshi data readers, renderer, world, ECS, physics, AI |
| [`mnkey_dst_tools`](https://github.com/rdga1bot/mnkey_dst_tools) | public | GPL-3.0 | Editor, QA/measurement scripts, offline converters |
| `mnkey_dst` | private | proprietary | Game executable, shaders, tests, research. Includes the two above as submodules |

**Data flow at startup:**
```
Kenshi install (read-only, KENSHI_DIR)
 ├─ data/gamedata.base, *.mod, mods.cfg ─► io/fcs, io/fcs_gamedata ─► world/BiomeRegistry (biomes)
 ├─ data/newland/land/fullmap.tif ───────► io/tiff ─► world/TerrainAtlas (heights, 1.8 m → 3.6 m grid)
 └─ data/newland/land/*.png ─────────────► io/kenshi_dir (KenshiLandFile) ─► terrain textures
                                                │
                       render/terrain (G-buffer → deferred resolve) ─► SDL_GPU / Vulkan
```

**Module map** (`include/monkey_dust/<module>/`):

| Module | What it does |
|---|---|
| `io/` | **Kenshi data:** FCS reader and merge (`fcs`, `fcs_gamedata`), uncompressed TIFF (`tiff`), install lookup (`kenshi_dir`) |
| `world/` | Terrain atlas and queries (`TerrainQuery`), fixed-depth terrain quadtree, biomes, feature scatter, weather, factions, world simulation |
| `render/` | GPU HAL over SDL_GPU, G-buffer + deferred lighting, EVSM cascaded shadows, SSAO, bloom, GPU skinning, materials, per-pass on/off switch (`RenderPassGraph`) for measurement |
| `ecs/`, `components/` | gaia-ecs behind the `MdRegistry`/`MdEntity` facade; POD components |
| `physics/` | Jolt Physics (`JoltWorld`, `CharacterVirtual`), ragdoll |
| `nav/` | Recast/Detour navmesh, DetourCrowd, async path cache |
| `ai/` | Stackless behaviour-tree VM, sense system, director, utility scorer |
| `combat/`, `building/`, `save/`, `scripting/`, `audio/` | Damage/hit zones, build grid, versioned saves, Lua 5.4 sandbox, miniaudio |
| `platform/` | Window/input/timing, job graph, INI/CVar, logging, AVX2 helpers |
| `flare/` | Older isometric tile renderer and loaders, kept from the project's earlier direction; not used by the Kenshi world path |

Conventions: C++17; POD components; `gpu_pipeline_safety_check.h` rejects pipeline layouts known to crash the Intel Gen9 ANV driver before they are created. The full subsystem reference is on the [docs site](https://rdga1afk.github.io/mnkey_dst_engine/).

## Repository layout
```
include/monkey_dust/   public headers, one folder per module (table above)
src/                   implementation, same folders
tests/                 small plain-C++ smoke tests (the full GTest suite is in the private game repo)
third_party/           vendored libraries, each with its own licence
index.html             documentation site (GitHub Pages)
```

## Legal and third-party code
- **Kenshi** is a trademark of Lo-Fi Games. This project is not affiliated with or endorsed by Lo-Fi Games. No game files are distributed; you need your own copy.
- **Project Okran** ([MIT](https://github.com/brayniac6-glitch/Project-Okran)): the FCS reader, mod load order, record merge and TIFF reader are C++ ports of the *behaviour* of Okran's Rust code. Details and the licence text: [THIRD_PARTY.md](THIRD_PARTY.md).
- Kenshi's shader code is never copied; where parity is needed, only formulas are re-implemented.
- **Licence:** MIT, see [LICENSE](LICENSE). The game executable and its sources are proprietary and not part of this repository.
