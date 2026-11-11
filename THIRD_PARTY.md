# Third-party code and attribution

This file lists code and designs ported or adapted into this repository from other projects, with their licences.
(Vendored libraries live in `third_party/`, each with its own licence file.)

## Project Okran (MIT)

- Source: <https://github.com/brayniac6-glitch/Project-Okran> (commit `48530b9`, 2026-10-09).
- Used for: `engine/src/io/fcs.cpp`, `fcs_gamedata.cpp` and their headers (`include/monkey_dust/io/fcs.h`,
  `fcs_gamedata.h`) are C++ ports of the behaviour of Okran's `src/io/fcs.rs` (FCS v15/v16/v17 reader),
  `src/io/mods.rs` (load order, `mods.cfg`, mod lookup) and `src/data/gamedata.rs` (record merging). The format
  details (field order, length checks, v17 header tail, merge rules) follow Okran; the code is written in C++ in this
  project's style, nothing is compiled from Okran.
- `tools/navtile/hkt.cpp` (Havok tagfile reader) and `tools/navtile/navtile.cpp` (navtile sections) are C++ ports of the behaviour of Okran's `src/io/hkt.rs` and the section reading of `src/world/navmesh.rs` (nothing compiled from Okran).
- `engine/src/io/tiff.cpp` and `include/monkey_dust/io/tiff.h` are a C++ port of the behaviour of Okran's `src/io/tiff.rs`
  (uncompressed greyscale little-endian TIFF, 8/16 bit, strips, windowed reads by seeking).
- `engine/src/io/ogre_chunk.cpp`, `ogre_mesh.cpp`, `ogre_skeleton.cpp`, `kenshi_resources.cpp` and their headers (`include/monkey_dust/io/ogre_chunk.h`,
  `ogre_mesh.h`, `ogre_skeleton.h`, `kenshi_resources.h`) are C++ ports of the behaviour of Okran's `src/io/ogre.rs` (chunk reader),
  `ogre_mesh.rs` (`.mesh`), `ogre_skeleton.rs` (`.skeleton`) and `resources.rs` (file-name lookup).
- `engine/src/world/foliage_scatter.cpp` and `include/monkey_dust/world/foliage_scatter.h` (#120) are a C++ port of the behaviour of Okran's `src/world/foliage.rs` (`Foliage::load`, MT19937 `Mt`, `scatter_layer`, `place`, grass overlay painting and noise) and `src/world/town.rs` (`nearest`). Checked against the decompiled exe for the generator (`F041815`, `F041775`); the ground (heights of `fullmap.tif`, slope metric, overlay window) follows the python stand-in tool, which differs from Okran in the slope metric.
- `private/md_foliage_place.py` (main repo, spike tool, #119) is a Python port of the behaviour of Okran's `src/world/foliage.rs` (`scatter_layer`, `place`, MT19937 `Mt`): layer/zone seeding, per-mesh counts, clusters, slope/altitude rejection, scale/offset/yaw draw order, child clusters. Differences from Okran (checked against the exe, see `docs/research/EXE_PARITY_MAP.tsv`): the slope metric.
- Licence text:

```
MIT License

Copyright (c) 2026 brayniac6-glitch

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Qualcomm Snapdragon Game Super Resolution 1 (BSD-3-Clause)

- Source: <https://github.com/SnapdragonStudios/snapdragon-gsr>, `sgsr/v1/include/glsl/sgsr1_shader_mobile.frag` (RGBA mode).
- Used for: the optional spatial upscale of #198 S1 (`shaders/sgsr1.frag`, `md.set_sgsr1`), a port of that fragment shader to our SDL_GPU sets. Only the licence is copied into `third_party/sgsr1/`.
- Licence: `third_party/sgsr1/LICENSE.txt` (BSD-3-Clause, Copyright (c) 2023, Qualcomm Innovation Center, Inc.).

## SDL3 and Lua (static link, #201 R5, owner 23.10)
- SDL3 `release-3.4.8` (https://github.com/libsdl-org/SDL, zlib licence), commit `d9d5536704d585616d4db3c8ba3c4ff6fc2757e1`; fetched by CMake `FetchContent` into `third_party/SDL3_src` (not committed), built static with `-DMD_STATIC_DEPS=ON`.
- Lua 5.4.8 (https://www.lua.org/ftp/lua-5.4.8.tar.gz, MIT licence), sha256 `4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae` (the value published on lua.org), unpacked unchanged into `third_party/lua54_src`.

## AlexeyShirshov/skills — pdca (MIT, #204)
Process skill only, no code in the binaries. `.claude/skills/pdca/UPSTREAM.md` is the upstream `skills/pdca/SKILL.md` kept verbatim for reference (fetched 24.10.2026), `.claude/skills/pdca/SKILL.md` is our adaptation, `.claude/skills/pdca/LICENSE-UPSTREAM` the licence text. Copyright (c) 2026 Alexey Shirshov, MIT License.


## Tracy profiler client (BSD-3-Clause, profiling preset only)
- Tracy v0.14.1 (https://github.com/wolfpld/tracy, commit `30997d5ca6bb632cc10807a1da8a6d3de0aeeb3c`), fetched by CMake into `third_party/tracy_src` only with `-DMD_TRACY=ON`. Not in the release binary. Captures are never committed or uploaded.

## GFXReconstruct (MIT, dev tool only)
- LunarG GFXReconstruct, tag `vulkan-sdk-1.4.363.0` (https://github.com/LunarG/gfxreconstruct), built by hand in `temp_/gfxr/` for Vulkan capture / replay of the frame-cost scenes. Not linked into the game, not in the release. Captures contain Kenshi data and are never committed or uploaded.

## Vendored libraries (attribution table, 11.11.2026; versions and licence texts read from the pinned sources)
Submodules are pinned by the commit shown; "licence text" is the file in the tree, or "in header" when the text sits at the top of the source.
| library | version / pin | licence | licence text | where used |
|---|---|---|---|---|
| gaia-ecs (single header) | v1.0.0, commit 2843d22a930f3c1efc6b54c995065a1dacd7161a | MIT, © 2021–2026 Richard Biely | `third_party/gaia-ecs/LICENSE` (raw file at the pinned commit 2843d22a, https://raw.githubusercontent.com/richardbiely/gaia-ecs/2843d22a930f3c1efc6b54c995065a1dacd7161a/LICENSE, fetched 11.11.2026); the header embeds robin-hood-hashing (MIT, © 2018–2021 Martin Ankerl, text in header) and tcbrindle/span (Boost 1.0, © 2018 Tristan Brindle, reference in header) | engine ECS |
| glm | 6f14f4792a0cde5d0cf2c910506724d61cb95834 (header version 1.1.0) | Happy Bunny or MIT | `third_party/glm/copying.txt` | maths |
| JoltPhysics | 25b055e3401c8e2ffba5a368bdaea30b45f24b5a (v5.5.0 + 142, header 5.5.1) | MIT, © 2021 Jorrit Rouwe | `third_party/JoltPhysics/LICENSE` | physics |
| MyGUI | 8a05127d7c5bb6772df88185b1f99d8052c379a4 (3.2.2) | MIT, © 2007–2013 MyGUI Developers | `third_party/MyGUI/COPYING.MIT` | in-game UI |
| ozz-animation | 6cbdc790123aa4731d82e255df187b3a8a808256 (0.16.0) | MIT | `third_party/ozz-animation/LICENSE.md` | skeletal animation |
| recastnavigation | 9f4ce64458dfae86e1239c525ddc219c4e9e06f1 (v1.6.0 + 367) | zlib-style, © 2009 Mikko Mononen | `third_party/recastnavigation/License.txt` | navmesh |
| tiny_bvh | 1.6.7 (`include/vendor/tiny_bvh.h`) | MIT, © 2024–2025 Jacco Bikker | in header | ray / BVH |
| stb_image | 2.30 | public domain / MIT (dual, as stated in the file) | in header | image load |
| stb_image_write | 1.16 | public domain / MIT | in header | image write |
| stb_truetype | 1.26 | public domain / MIT | in header | font raster |
| miniaudio | 0.11.24 | public domain or MIT-0 (as stated in the file) | in header | audio |
| Arimo (4 styles) | 1.33, © 2020 The Arimo Project Authors | SIL Open Font License 1.1 (the font's own name table says OFL; an earlier note of Apache was wrong) | `src/vendor/fonts/OFL-Arimo.txt`, copy in `game/data/fonts/OFL-Arimo.txt` — OFL.txt of the upstream repo https://github.com/googlefonts/arimo (branch main, fetched 11.11.2026; its copyright line reads 2026, the vendored font file says 2020) | UI font (`src/vendor/fonts`, `game/data/fonts`) |
| Ubuntu Mono (4 styles) | 0.80, © 2011 Canonical Ltd. | Ubuntu Font Licence 1.0 | `src/vendor/fonts/UFL-UbuntuMono.txt` — LICENCE.txt of the ubuntu-font-family-0.83 archive, https://assets.ubuntu.com/v1/fad7939b-ubuntu-font-family-0.83.zip, fetched 11.11.2026 (the vendored Ubuntu Mono is version 0.80, same licence text) | editor font |
| Poly Haven terrain textures and a rock model | `game/data/textures/terrain/{bark,dirt,grass,rock}.jpg`, `game/data/props/rock_01.glb` (added 17.05.2026) | CC0 1.0 — the Poly Haven licence page https://polyhaven.com/license (read 11.11.2026) states all textures and models are CC0, no attribution required | no licence file needed (CC0); the names of the individual Poly Haven assets were not recorded when the files were added: UNKNOWN | terrain / prop placeholders |

### Tools (editor) libraries
| library | version / pin | licence | licence text |
|---|---|---|---|
| Dear ImGui | ea1c04f7abc31f3089e5b163eef69d933e0a6209 (1.92.8 WIP) | MIT | `tools/third_party/imgui/LICENSE.txt` |
| ImGuiColorTextEdit | ca2f9f1462e3b60e56351bc466acda448c5ea50d (master) | MIT, © 2017 BalazsJako | `third_party/ImGuiColorTextEdit/LICENSE` |
| imgui-command-palette | f8d93959c0c2ae6f52b52ffa641644ceda3e3b94 (master) | MIT, © 2021 hnOsmium0001 | `third_party/imgui-command-palette/LICENSE` |
| imgui-flame-graph | aae0bd9665d1379c511d0b8a64d3752301a327a2 (v1.02) | MIT, © 2019 Sandy | `third_party/imgui-flame-graph/LICENSE` |
| imgui-node-editor | vendored copy (not a submodule) | MIT, © 2019 Michał Cichoń | `third_party/imgui-node-editor/LICENSE` |
| ImGuizmo | 867101c4efd597deb9246d408c068978ec21712b (1.10 + 1) | MIT, © 2016 Cedric Guillemet | `third_party/imguizmo/LICENSE` |
| imnodes | eb36902c892548ef94f88f51ad7e7c9c7058a71c (v0.5 + 38) | MIT, © 2019 Johann Muszynski | `third_party/imnodes/LICENSE.md` |
| hkxparse (tagfile subset, patched for Linux/GCC) | vendored; upstream exyorha/hkxparse | MIT, © 2018–2020 Maria Egorova | `third_party/hkxparse/LICENSE`; our patches: `README_MONKEYDUST.md` |
| Dear ImGui Test Engine | 7be88e7fa267f251c7271a33d72b1aa5ca04a4cb (v1.92.8) | its own licence v1.04 (free for individuals, non-profit, education, open-source derivatives, turnover < 2 M USD; otherwise paid): **not linked in release builds** (`MD_UI_TESTS=OFF`, checked by `scripts/check_release_binary.sh`) | `third_party/imgui_test_engine/imgui_test_engine/LICENSE.txt` |
