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
