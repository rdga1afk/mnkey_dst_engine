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
- `engine/src/io/tiff.cpp` and `include/monkey_dust/io/tiff.h` are a C++ port of the behaviour of Okran's `src/io/tiff.rs`
  (uncompressed greyscale little-endian TIFF, 8/16 bit, strips, windowed reads by seeking).
- `engine/src/io/ogre_chunk.cpp`, `ogre_mesh.cpp`, `ogre_skeleton.cpp`, `kenshi_resources.cpp` and their headers (`include/monkey_dust/io/ogre_chunk.h`,
  `ogre_mesh.h`, `ogre_skeleton.h`, `kenshi_resources.h`) are C++ ports of the behaviour of Okran's `src/io/ogre.rs` (chunk reader),
  `ogre_mesh.rs` (`.mesh`), `ogre_skeleton.rs` (`.skeleton`) and `resources.rs` (file-name lookup).
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

## AMD FidelityFX FSR 2 (MIT)

- Source: <https://github.com/GPUOpen-Effects/FidelityFX-FSR2>, tag `v2.2.1`, commit `1680d1edd5c034f88ebbbb793d8b88f8842cf804`.
- Used for: the FSR 2 upscaler of #156 (the compute passes of `src/ffx-fsr2-api/shaders` and the pass logic of `ffx_fsr2.cpp` are ported to SDL_GPU). The copy in `third_party/fsr2/` is `src/ffx-fsr2-api` without `dx12/` and the licence file; no AMD binary is run.
- Licence: `third_party/fsr2/LICENSE.txt` (MIT, Copyright (c) 2022-2023 Advanced Micro Devices, Inc.).

## AMD FidelityFX FSR 1 (MIT)

- Source: <https://github.com/GPUOpen-Effects/FidelityFX-FSR>, tag `v1.0.2`, commit `a21ffb8f6c13233ba336352bdff293894c706575`.
- Used for: the EASU (edge-adaptive spatial upsampling) and RCAS (robust contrast-adaptive sharpening) passes of the upscaler of #156 (`ffx_fsr1.h`, with `ffx_a.h` the portability layer). The copy in `third_party/fsr1/` is `ffx_a.h`, `ffx_fsr1.h` and the licence; nothing of it is built or run from there, the passes are ported to SDL_GPU compute (`shaders/fsr1/`).
- Licence: `third_party/fsr1/LICENSE.txt` (MIT, Copyright (c) 2021 Advanced Micro Devices, Inc.).
