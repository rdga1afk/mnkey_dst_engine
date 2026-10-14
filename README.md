---
id: kb-engine-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-10-13
repo: engine
tags: [engine, readme, kenshi, sdl-gpu, terrain, public-repo]
summary: "Public engine README (EN + UA): native Linux engine that renders Kenshi's world from the player's own install; status, goal, build, KENSHI_DIR, mods, architecture, module map, legal"
---

# monkey_dust Engine

**A native Linux C++17 engine that renders the world of [Kenshi](https://lofigames.com/) from your own installed copy of the game, aimed at weak integrated GPUs (test platform: Intel HD 520).**

[English](#english) · [Українською](#українською) · [Docs site](https://rdga1afk.github.io/mnkey_dst_engine/)

---

## English

> ### You need Kenshi
> This engine contains **no Kenshi data** and never will. It reads the game's files (FCS `.base`/`.mod`, heightmap, maps, later meshes and textures) at startup from **your own legal copy** of Kenshi (Steam or GOG), the same way [OpenMW](https://openmw.org/) works with Morrowind. Nothing is copied into this repository and the engine never writes to the Kenshi folder.

### What this is
- A **rendering engine for Kenshi's world on Linux**, native (no Wine/Proton at runtime), SDL3 + SDL_GPU (Vulkan).
- Its niche is **weak hardware**: the Intel HD 520 laptop iGPU is the test platform (the 60 FPS goal and its gates are in Status below).
- Content is edited with Kenshi's own editor, the **Forgotten Construction Set (FCS)**. Mods load in Kenshi's own order.

### What this is not (yet)
- **Not a playable game.** No NPCs, combat, trade or UI from Kenshi yet. Game logic comes later, step by step.
- **Not a Kenshi replacement or a remake.** Kenshi is © Lo-Fi Games. This is an independent, unofficial engine.
- **There is nothing to download and play.** The game executable lives in a separate private repository; this repository is the engine library.

### Status (2026-10-13)

| Area | State |
|---|---|
| FCS reader (`.base`/`.mod` v15–v17, `mods.cfg` load order, record merge) | **Done.** Checked on a real install: 20 481 records, 394 766 values, 0 mismatches against an independent dump. Biomes come from the merged data (mod overrides applied) |
| Heightmap from `fullmap.tif` (16385², uint16) | **Done.** 68 161 536 heights, 0 differences. Near the camera a 1.8 m window (Kenshi's own grid), streamed on a background thread; further away every 2nd sample (3.6 m) |
| `biomemap`, `blendmap`, `ambientmap`, ground textures | **Done.** Read from the install; textures are found by file name the way Ogre does it |
| **Ground shader** | **Ported from [Project Okran](https://github.com/brayniac6-glitch/Project-Okran) (MIT)**, following Kenshi's `terrainfp4.hlsl`: per-pixel biomes from `blendMap` (up to four plus the cell's fifth slot), per-pixel cliff weight, Kenshi's default texture quality (one mip down for landscape layers), Kenshi units |
| **Terrain LOD** | **Done, on by default.** Quadtree by screen-space error as in Kenshi's terrain plugin: thresholds 5 / 35 px with a 750 → 1500 m ramp, geomorph, skirts, per-vertex normals; patch size 16 ("Small"), 64 ("Large") as a setting |
| Lighting, sky, water like Kenshi's | Not yet: frames are darker than Kenshi. Next: port Kenshi's lighting chain (sun, irradiance cube maps, auto exposure) |
| Towns (Ogre `.mesh`), foliage | Planned, Nov 2026 |
| Steam library auto-detection | Not yet: set `KENSHI_DIR` |
| Workshop mods | Code path exists, **not yet tested on real Workshop data** |
| **Performance, native 1920×1080, HD 520** | Frame interval at three fixed points **23.6 / 19.4 / 23.6 ms**; fixed flight route **p99 30–31 ms** (13.10.2026). Goal: ≤ 16.7 ms. This is the main open problem |

### Goal by ~2026-12-10
**Kenshi's world on Linux at a stable 60 FPS on an HD 520**, native 1920×1080, no resolution scaling: terrain, sky, water, towns and foliage read from the Kenshi folder, looking like Kenshi.
- **"Stable"** = frame-interval p99 ≤ 16.7 ms on a fixed flight route over the world (town, canyon, desert, horizon; loading hitches count), plus a mean ≤ 16.7 ms at three fixed points and near the Hub.
- Week-4 gate (2026-11-07): terrain alone ≤ 12 ms.
- Also by then (in the game and tools repositories): a character editor that matches Kenshi's, and AMD FSR 2 as an optional player setting. The goal itself is measured without upscaling.

### Requirements
| | Minimum (target) |
|---|---|
| OS | Linux (developed on Arch Linux) |
| GPU | Vulkan, Intel HD 520 (Skylake Gen9) or better; Mesa ANV is the reference driver |
| CPU | x86-64 with AVX2 |
| RAM | the game process uses about 540 MB at a terrain point (13.10.2026); towns and foliage will add to it |
| Kenshi | Any legal install (Steam / GOG). Only its data files are read |

### Build
```bash
# Engine library (this repository)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON
ninja -C build monkey_dust_engine
ninja -C build md_tests            # small smoke tests
```
Dependencies: SDL3, gaia-ecs, Recast/Detour, ozz-animation, Jolt Physics, miniaudio, Lua 5.4 (vendored or via CMake).
Shaders (GLSL → SPIR-V) live in the game repository; the library loads pre-compiled SPIR-V at runtime.

### Pointing the engine at Kenshi
```bash
export KENSHI_DIR="$HOME/.local/share/Steam/steamapps/common/Kenshi"   # the folder that contains data/gamedata.base
```
- `md::KenshiDir()` returns `$KENSHI_DIR` (an empty value disables the install on purpose).
- `md::KenshiLandFile(name, fallback)` returns `<KENSHI_DIR>/data/newland/land/<name>` if it exists, else the fallback; the choice is logged once.
- Tests that need real game data run only when `KENSHI_DIR` is set.

### Mods
Mods are read exactly as Kenshi reads them: `data/mods.cfg` order, then `mods/` and Steam Workshop folders; records are merged in load order (`monkey_dust/io/fcs_gamedata.h`). Edit content with Kenshi's FCS. Workshop mods are not yet verified on real data.

### Architecture

**Three repositories:**

| Repository | Visibility | Licence | Contents |
|---|---|---|---|
| [`mnkey_dst_engine`](https://github.com/rdga1afk/mnkey_dst_engine) (this) | public | MIT | Engine library: Kenshi data readers, renderer, world, ECS, physics, AI |
| [`mnkey_dst_tools`](https://github.com/rdga1afk/mnkey_dst_tools) | public | GPL-3.0 | Editor, QA/measurement scripts, offline converters |
| `mnkey_dst` | private | proprietary | Game executable, shaders, tests, research. Includes the two above as submodules |

**Data flow at startup:**
```
Kenshi install (read-only, KENSHI_DIR)
 ├─ data/gamedata.base, *.mod, mods.cfg ─► io/fcs, io/fcs_gamedata ─► world/BiomeRegistry (biomes, merged with mods)
 ├─ data/newland/land/fullmap.tif ───────► io/tiff ─► world/TerrainAtlas (3.6 m) + world/TerrainLodTree (1.8 m window near the camera)
 └─ data/newland/land/*.png, textures ───► io/kenshi_dir (KenshiLandFile, lookup by file name) ─► terrain maps and layers
                                                │
        render: terrain LOD → 8-byte G-buffer → ground resolve (Kenshi's ground shader) ─► SDL_GPU / Vulkan
```

**Module map** (`include/monkey_dust/<module>/`):

| Module | What it does |
|---|---|
| `io/` | **Kenshi data:** FCS reader and merge (`fcs`, `fcs_gamedata`), uncompressed TIFF (`tiff`), install lookup (`kenshi_dir`) |
| `world/` | Terrain atlas and queries (`TerrainQuery`), terrain LOD tree (`TerrainLodTree`, Kenshi's screen-space error rule), biomes, feature scatter, weather, factions, world simulation |
| `render/` | GPU HAL over SDL_GPU, terrain LOD renderer and 8-byte terrain G-buffer, deferred lighting, EVSM cascaded shadows, SSAO, bloom, GPU skinning, materials, per-pass on/off switch (`RenderPassGraph`) for measurement, RenderDoc in-app capture hook |
| `ecs/`, `components/` | gaia-ecs behind the `MdRegistry`/`MdEntity` facade; POD components |
| `physics/` | Jolt Physics (`JoltWorld`, `CharacterVirtual`), ragdoll |
| `nav/` | Recast/Detour navmesh, DetourCrowd, async path cache |
| `ai/` | Stackless behaviour-tree VM, sense system, director, utility scorer |
| `combat/`, `building/`, `save/`, `scripting/`, `audio/` | Damage/hit zones, build grid, versioned saves, Lua 5.4 sandbox, miniaudio |
| `platform/` | Window/input/timing, job graph, INI/CVar, logging, AVX2 helpers |

Conventions: C++17; POD components; `gpu_pipeline_safety_check.h` rejects pipeline layouts known to crash the Intel Gen9 ANV driver before they are created. The full subsystem reference is on the [docs site](https://rdga1afk.github.io/mnkey_dst_engine/).

### Repository layout
```
include/monkey_dust/   public headers, one folder per module (table above)
src/                   implementation, same folders
tests/                 small plain-C++ smoke tests (the full GTest suite is in the private game repo)
third_party/           vendored libraries, each with its own licence
index.html             documentation site (GitHub Pages)
```

### Legal and third-party code
- **Kenshi** is a trademark of Lo-Fi Games. This project is not affiliated with or endorsed by Lo-Fi Games. No game files are distributed; you need your own copy.
- Third-party code and its licences: [THIRD_PARTY.md](THIRD_PARTY.md).
- Kenshi's own code (shaders, executable) is never copied. Where parity is needed, behaviour is ported from [Project Okran](https://github.com/brayniac6-glitch/Project-Okran) (MIT, attributed in [THIRD_PARTY.md](THIRD_PARTY.md)) or re-implemented from facts.
- **Licence:** MIT, see [LICENSE](LICENSE). The game executable and its sources are proprietary and not part of this repository.

---

## Українською

> ### Потрібна Kenshi
> Рушій **не містить даних Kenshi** і не міститиме. Під час запуску він читає файли гри (FCS `.base`/`.mod`, карту висот, карти, згодом моделі й текстури) з **вашої власної легальної копії** Kenshi (Steam або GOG), так само як [OpenMW](https://openmw.org/) працює з Morrowind. Нічого не копіюється в цей репозиторій, і рушій нічого не пише в теку Kenshi.

### Що це
- **Рушій, що рендерить світ Kenshi під Linux**, нативно (без Wine/Proton під час роботи), SDL3 + SDL_GPU (Vulkan).
- Ніша — **слабке залізо**: вбудована Intel HD 520 — тестова платформа (ціль 60 FPS і її ворота — у розділі «Стан» нижче).
- Контент редагується власним редактором Kenshi, **Forgotten Construction Set (FCS)**. Моди вантажаться в порядку Kenshi.

### Чим це (поки) не є
- **Це не гра, в яку можна грати.** NPC, бою, торгівлі й інтерфейсу з Kenshi ще немає. Ігрова логіка — пізніше, поступово.
- **Це не заміна і не ремейк Kenshi.** Kenshi © Lo-Fi Games. Це незалежний неофіційний рушій.
- **Завантажити й зіграти поки нічого.** Виконуваний файл гри — в окремому приватному репозиторії; тут бібліотека рушія.

### Стан (13.10.2026)

| Напрям | Стан |
|---|---|
| Читач FCS (`.base`/`.mod` v15–v17, порядок `mods.cfg`, злиття записів) | **Готово.** Перевірено на справжній інсталяції: 20 481 запис, 394 766 значень, 0 розбіжностей з незалежним дампом. Біоми — зі злитих даних (з модами) |
| Карта висот з `fullmap.tif` (16385², uint16) | **Готово.** 68 161 536 висот, 0 відмінностей. Біля камери — вікно 1,8 м (сітка самої Kenshi), підвантажується у фоновому потоці; далі — кожен 2-й відлік (3,6 м) |
| `biomemap`, `blendmap`, `ambientmap`, текстури ґрунту | **Готово.** Читаються з інсталяції; текстури шукаються за іменем файлу, як в Ogre |
| **Шейдер ґрунту** | **Перенесено з [Project Okran](https://github.com/brayniac6-glitch/Project-Okran) (MIT)** за `terrainfp4.hlsl` Kenshi: біоми для кожного пікселя за `blendMap` (до чотирьох + п'ятий слот клітинки), вага кручі для кожного пікселя, якість текстур Kenshi за замовчуванням (шари ландшафту на один mip нижче), одиниці Kenshi |
| **LOD ландшафту** | **Готово, увімкнено за замовчуванням.** Квадродерево за екранною похибкою, як у плагіні ландшафту Kenshi: пороги 5 / 35 px з переходом 750 → 1500 м, геоморф, спідниці, нормалі у вершинах; розмір патча 16 («Small»), 64 («Large») — налаштування |
| Світло, небо, вода, як у Kenshi | Ще ні: кадри темніші за Kenshi. Далі — перенос ланцюга світла Kenshi (сонце, кубічні карти опромінення, автоекспозиція) |
| Міста (Ogre `.mesh`), рослини | План на листопад 2026 |
| Автопошук бібліотеки Steam | Ще ні: задайте `KENSHI_DIR` |
| Моди з Workshop | Код є, **на справжніх даних Workshop ще не перевірено** |
| **Продуктивність, нативні 1920×1080, HD 520** | Інтервал кадру в трьох фіксованих точках **23,6 / 19,4 / 23,6 мс**; фіксований маршрут польоту **p99 30–31 мс** (13.10.2026). Мета — ≤ 16,7 мс. Це головна відкрита проблема |

### Мета до ~10.12.2026
**Світ Kenshi під Linux зі стабільними 60 FPS на HD 520**, нативні 1920×1080 без масштабування роздільності: земля, небо, вода, міста й рослини з теки Kenshi, на вигляд як у Kenshi.
- **«Стабільні»** — p99 інтервалу кадру ≤ 16,7 мс на фіксованому маршруті польоту над світом (місто, каньйон, пустеля, обрій; ривки підвантаження теж рахуються), плюс середнє ≤ 16,7 мс у трьох фіксованих точках і біля Хабу.
- Ворота 4-го тижня (07.11.2026): лише земля ≤ 12 мс.
- До того ж строку (у репозиторіях гри й інструментів): редактор персонажів, як у Kenshi, і AMD FSR 2 як налаштування гравця. Сама мета міряється без масштабування.

### Вимоги
| | Мінімум (ціль) |
|---|---|
| ОС | Linux (розробка на Arch Linux) |
| GPU | Vulkan, Intel HD 520 (Skylake Gen9) або краща; еталонний драйвер Mesa ANV |
| CPU | x86-64 з AVX2 |
| ОЗП | процес гри займає близько 540 МБ у точці ландшафту (13.10.2026); міста й рослини додадуть |
| Kenshi | Будь-яка легальна інсталяція (Steam / GOG). Читаються лише файли даних |

### Збірка
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON
ninja -C build monkey_dust_engine
ninja -C build md_tests
```
Залежності: SDL3, gaia-ecs, Recast/Detour, ozz-animation, Jolt Physics, miniaudio, Lua 5.4.
Шейдери (GLSL → SPIR-V) лежать у репозиторії гри; бібліотека вантажить готовий SPIR-V під час роботи.

### Як вказати теку Kenshi
```bash
export KENSHI_DIR="$HOME/.local/share/Steam/steamapps/common/Kenshi"   # тека, де лежить data/gamedata.base
```
- `md::KenshiDir()` повертає `$KENSHI_DIR` (порожнє значення навмисно вимикає інсталяцію).
- `md::KenshiLandFile(name, fallback)` повертає `<KENSHI_DIR>/data/newland/land/<name>`, якщо файл є, інакше запасний шлях; вибір пишеться в лог один раз.
- Тести, яким потрібні справжні дані, запускаються лише з `KENSHI_DIR`.

### Моди
Моди читаються так само, як у Kenshi: порядок з `data/mods.cfg`, далі теки `mods/` і Steam Workshop; записи зливаються в порядку завантаження (`monkey_dust/io/fcs_gamedata.h`). Контент редагується у FCS. Моди з Workshop на справжніх даних ще не перевірені.

### Архітектура

**Три репозиторії:**

| Репозиторій | Доступ | Ліцензія | Вміст |
|---|---|---|---|
| [`mnkey_dst_engine`](https://github.com/rdga1afk/mnkey_dst_engine) (цей) | публічний | MIT | Бібліотека рушія: читачі даних Kenshi, рендер, світ, ECS, фізика, AI |
| [`mnkey_dst_tools`](https://github.com/rdga1afk/mnkey_dst_tools) | публічний | GPL-3.0 | Редактор, скрипти замірів і QA, офлайн-конвертери |
| `mnkey_dst` | приватний | власницька | Виконуваний файл гри, шейдери, тести, дослідження. Два репо вище — його підмодулі |

**Потік даних під час запуску:** див. схему в англійському розділі вище: інсталяція Kenshi (лише читання) → `io/` → `world/` (атлас 3,6 м + вікно 1,8 м біля камери) → `render/` (LOD → 8-байтовий G-буфер → шейдер ґрунту Kenshi) → SDL_GPU / Vulkan.

**Модулі** (`include/monkey_dust/<модуль>/`):

| Модуль | Що робить |
|---|---|
| `io/` | **Дані Kenshi:** читач і злиття FCS, TIFF без стиснення, пошук інсталяції |
| `world/` | Атлас висот і запити (`TerrainQuery`), дерево LOD ландшафту (`TerrainLodTree`, правило екранної похибки Kenshi), біоми, розстановка об'єктів, погода, фракції, симуляція світу |
| `render/` | GPU HAL над SDL_GPU, рендер LOD ландшафту й 8-байтовий G-буфер землі, відкладене освітлення, каскадні тіні EVSM, SSAO, bloom, скінінг на GPU, матеріали, вмикання/вимикання проходів (`RenderPassGraph`) для замірів, хук захоплення RenderDoc |
| `ecs/`, `components/` | gaia-ecs за фасадом `MdRegistry`/`MdEntity`; POD-компоненти |
| `physics/` | Jolt Physics, ragdoll |
| `nav/` | Навмеш Recast/Detour, DetourCrowd, асинхронний кеш шляхів |
| `ai/` | VM дерев поведінки без стеку, сенси, режисер, оцінка корисності |
| `combat/`, `building/`, `save/`, `scripting/`, `audio/` | Шкода й зони влучання, будівництво, збереження з версіями, пісочниця Lua 5.4, miniaudio |
| `platform/` | Вікно, ввід, час, граф задач, INI/CVar, лог, AVX2 |

### Правове
- **Kenshi** — торгова марка Lo-Fi Games. Проєкт не пов'язаний з Lo-Fi Games і не схвалений ними. Файли гри не поширюються; потрібна власна копія.
- Сторонній код і його ліцензії: [THIRD_PARTY.md](THIRD_PARTY.md).
- Власний код Kenshi (шейдери, виконуваний файл) не копіюється. Де потрібен паритет, поведінка переноситься з [Project Okran](https://github.com/brayniac6-glitch/Project-Okran) (MIT, з атрибуцією в [THIRD_PARTY.md](THIRD_PARTY.md)) або відтворюється за фактами.
- **Ліцензія:** MIT, див. [LICENSE](LICENSE). Виконуваний файл гри і його код — власницькі й не входять до цього репозиторію.
