---
id: kb-engine-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-10-22
repo: engine
tags: [engine, readme, kenshi, sdl-gpu, terrain, public-repo]
summary: "Public engine README (EN + UA): native Linux engine that renders Kenshi's world from the player's own install; status, goal to 2026-12-31, build, KENSHI_DIR, mods, architecture, module map, legal"
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
- **Not a playable game yet.** No NPCs, combat, trade or UI from Kenshi. An earlier prototype game layer (AI, combat, saves, building, Flare) was removed on 2026-10-17; the new game logic will be written from scratch, following facts about Kenshi's own systems.
- **Not a Kenshi replacement or a remake.** Kenshi is © Lo-Fi Games. This is an independent, unofficial engine.
- **There is nothing to download and play.** The game executable lives in a separate private repository; this repository is the engine library.

### Status (2026-10-22)

| Area | State |
|---|---|
| FCS reader and writer (`.base`/`.mod` v15–v17, `mods.cfg` load order, record merge) | **Done.** Writer (v16 / v17) re-creates every installed file byte for byte (22.10). Checked on a real install: 20 481 records, 394 766 values, 0 mismatches against an independent dump. Biomes come from the merged data (mod overrides applied) |
| Heightmap from `fullmap.tif` (16385², uint16) | **Done.** 68 161 536 heights, 0 differences. Near the camera a 1.8 m window (Kenshi's own grid), streamed on a background thread; further away every 2nd sample (3.6 m) |
| `biomemap`, `blendmap`, `ambientmap`, ground textures | **Done.** Read from the install; textures are found by file name the way Ogre does it |
| **Ground shader** | **Done.** Follows Kenshi's `terrainfp4.hlsl`: per-pixel biomes from `blendMap` (up to four plus the cell's fifth slot), per-pixel cliff weight, Kenshi's default texture quality (one mip down for landscape layers), Kenshi units |
| **Ground filtering** | **Done.** Mip level chosen from the pixel's footprint (analytic ray differentials) instead of distance only. This removed ground shimmer in motion: reprojection residual 4.9 → 0.5. Setting: footprint mip (default) / anisotropic ×4 / ×16 (Kenshi's own filter) |
| **Terrain LOD** | **Done, on by default.** Quadtree by screen-space error as in Kenshi's terrain plugin: thresholds 5 / 35 px with a 750 → 1500 m ramp, geomorph, skirts, per-vertex normals; patch size 16 ("Small"), 64 ("Large") as a setting |
| **Lighting, sky, water** | **Done (22.10).** Kenshi's main light in the ground resolve with fixed exposure (fog and auto exposure behind switches); **Kenshi's sky (SkyX)** rendered into a panorama a few times per second and sampled every frame (default on); **Kenshi's water shader** (three flow-driven normal layers, colour map, sun highlight, depth-based alpha; no reflections yet) at Kenshi's water level 1.5 m — about 30 % of the world, mostly ocean; cost within measurement noise |
| **Towns** (Ogre `.mesh` from zone files) | **Done, off by default (22.10).** Buildings read from Kenshi's zone files, nested building parts and roofs, streamed 3×3 zones on a worker thread with a 3 ms per-frame budget, back faces culled per Kenshi material, duplicate vertices welded. Off by default until the route p99 with buildings is ≤ 16.7 ms (a Hub street still costs about +4–6 ms) |
| **Foliage** (trees, bushes, rocks, grass) | **Done, off by default (22.10).** Placement follows Kenshi's own scatter (per-layer random generator and formulas checked against the game), trees / bushes / rocks streamed by zone, grass in a 50 m ring around the camera; terrain-textured rocks drawn like Kenshi's. Cost +0.4…1.5 ms, no loading hitches in continuous flight. Settings: on / off, view range, grass density |
| Steam library auto-detection | Not yet: set `KENSHI_DIR` |
| Workshop mods | Code path exists, **not yet tested on real Workshop data** |
| **Performance, 1920×1080, HD 520** | Fixed flight route **p99 ≈ 15.0 ms** (19.10.2026; 19 ms on 17.10, 30–31 ms on 13.10) — **the 60 FPS goal (≤ 16.7 ms) is met on the route**. **New (22.10):** «Ground quality» setting — *Fast motion* (default: normal ground filtering, 0.75 resolution only while moving, switch without hitches), anisotropic ×16 + dynamic resolution, or ×16 + softer ground reuse; frame cap **«auto»: 30 FPS when idle, 60 when moving** (fan almost silent when idle). Helped by: GPU occlusion culling (HZB), **ground shading reused across frames** (refreshed every 16 frames by default; Settings: 4 / 8 / 16), the terrain resolve no longer writing depth, and dynamic resolution only while the camera moves (0.75, native when still). Known tail: continuous flight at 50 m/s, p99 ≈ 18.9 ms. Long-term target: 99 FPS on the landscape |
| **Code clean-up** (Oct 2026) | **Done (19.10):** ≈ 85 000 lines of own code removed across the three repositories (old prototype game layer, OpenGL leftovers, effects for stronger GPUs that never ran on the HD 520, an unused 200 MB ground cache), plus 2 × 131 000 lines of font arrays and the 8 700-line GL loader. Audit: clang-tidy, cppcheck, ASan / UBSan, code coverage; every change checked against pixel-exact reference frames. The engine and tools libraries are now built on GitHub (laptop build 19 → 7 min) |

### Goals
**By ~2026-11-08: Kenshi's world on Linux at a stable 60 FPS on an HD 520**, native 1920×1080: terrain, sky, water, towns and foliage read from the Kenshi folder, looking like Kenshi (most of it is in place, see Status; what remains is getting towns and foliage within budget).
- **"Stable"** = frame-interval p99 ≤ 16.7 ms on a fixed flight route over the world (town, canyon, desert, horizon; loading hitches count), plus a mean ≤ 16.7 ms at three fixed points and near the Hub.

**By ~2026-11-20:** **a minimal playable core** on top of it: a character in the world with Kenshi's mesh, skeleton and animations, click-to-move on Kenshi's own navigation mesh, a squad, a few NPCs from town data, the simplest melee combat. Partial and rough on purpose; a full Kenshi replacement is a multi-year effort.

**By 2026-12-31 (preliminary):** game shell (launcher with mods, main menu on Kenshi's own UI layouts), new game with the character editor, deeper combat and medicine, NPC behaviour and factions, minimal inventory and trade, saves.

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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build monkey_dust_engine
ninja -C build md_tests            # small smoke tests
```
Dependencies: SDL3, gaia-ecs, ozz-animation, Jolt Physics, miniaudio, Lua 5.4, AMD FSR 1 (vendored or via CMake). Recast/Detour is vendored for the planned character movement.
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
| [`mnkey_dst_engine`](https://github.com/rdga1afk/mnkey_dst_engine) (this) | public | MIT | Engine library: Kenshi data readers, renderer, world, ECS, physics |
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
| `world/` | Terrain atlas and queries (`TerrainQuery`), terrain LOD tree (`TerrainLodTree`, Kenshi's screen-space error rule), terrain streaming, biomes, feature scatter, weather |
| `render/` | GPU HAL over SDL_GPU, terrain LOD renderer and 8-byte terrain G-buffer, deferred lighting, cascaded shadows (off by default), HZB occlusion culling, dynamic resolution, FSR 1, SSAO, bloom, GPU skinning, materials, per-pass on/off switch (`RenderPassGraph`) for measurement, RenderDoc in-app capture hook |
| `ecs/`, `components/` | gaia-ecs behind the `MdRegistry`/`MdEntity` facade; POD components (transform hierarchy, renderable, animator, morphs) |
| `physics/` | Jolt Physics (`JoltWorld`, `CharacterVirtual`), ragdoll |
| `scripting/`, `audio/` | Lua 5.4 sandbox (measurement and test scenarios), miniaudio |
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
- Kenshi's own code (shaders, executable) is never copied. Where parity is needed, behaviour is ported from permissively licensed open-source code (attributed in [THIRD_PARTY.md](THIRD_PARTY.md)) or re-implemented from facts.
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
- **Це ще не гра, в яку можна грати.** NPC, бою, торгівлі й інтерфейсу з Kenshi немає. Ранній прототип ігрового шару (ШІ, бій, збереження, будівництво, Flare) видалено 17.10.2026; нову ігрову логіку пишемо з нуля за фактами про системи самої Kenshi.
- **Це не заміна і не ремейк Kenshi.** Kenshi © Lo-Fi Games. Це незалежний неофіційний рушій.
- **Завантажити й зіграти поки нічого.** Виконуваний файл гри — в окремому приватному репозиторії; тут бібліотека рушія.

### Стан (22.10.2026)

| Напрям | Стан |
|---|---|
| Читач і запис FCS (`.base`/`.mod` v15–v17, порядок `mods.cfg`, злиття записів) | **Готово.** Запис (v16 / v17) відтворює кожен встановлений файл байт у байт (22.10). Перевірено на справжній інсталяції: 20 481 запис, 394 766 значень, 0 розбіжностей з незалежним дампом. Біоми — зі злитих даних (з модами) |
| Карта висот з `fullmap.tif` (16385², uint16) | **Готово.** 68 161 536 висот, 0 відмінностей. Біля камери — вікно 1,8 м (сітка самої Kenshi), підвантажується у фоновому потоці; далі — кожен 2-й відлік (3,6 м) |
| `biomemap`, `blendmap`, `ambientmap`, текстури ґрунту | **Готово.** Читаються з інсталяції; текстури шукаються за іменем файлу, як в Ogre |
| **Шейдер ґрунту** | **Готово.** За `terrainfp4.hlsl` Kenshi: біоми для кожного пікселя за `blendMap` (до чотирьох + п'ятий слот клітинки), вага кручі для кожного пікселя, якість текстур Kenshi за замовчуванням (шари ландшафту на один mip нижче), одиниці Kenshi |
| **Фільтрація ґрунту** | **Готово.** Рівень mip вибирається за слідом пікселя (аналітичні похідні променя), а не лише за відстанню. Це прибрало мерехтіння ґрунту в русі: залишок репроєкції 4,9 → 0,5. Налаштування: mip за слідом (за замовчуванням) / анізотропія ×4 / ×16 (фільтр самої Kenshi) |
| **LOD ландшафту** | **Готово, увімкнено за замовчуванням.** Квадродерево за екранною похибкою, як у плагіні ландшафту Kenshi: пороги 5 / 35 px з переходом 750 → 1500 м, геоморф, спідниці, нормалі у вершинах; розмір патча 16 («Small»), 64 («Large») — налаштування |
| **Світло, небо, вода** | **Готово (22.10).** Основне світло Kenshi в resolve ґрунту з фіксованою експозицією (туман і автоекспозиція — за перемикачами); **небо Kenshi (SkyX)** рахується в панораму кілька разів на секунду, щокадру — лише вибірка (за замовчуванням увімкнено); **шейдер води Kenshi** (три шари нормалей за течією, карта кольору, відблиск сонця, прозорість від глибини; відображень поки нема) на рівні води Kenshi 1,5 м — близько 30 % світу, переважно океан; ціна в межах шуму замірів |
| **Міста** (Ogre `.mesh` з файлів зон) | **Готово, за замовчуванням вимкнено (22.10).** Будівлі з файлів зон Kenshi, вкладені частини й дахи, стрімінг зон 3×3 у фоновому потоці з бюджетом 3 мс на кадр, відсікання задніх граней за матеріалами Kenshi, зварювання однакових вершин. Вимкнено, доки p99 маршруту з будівлями не стане ≤ 16,7 мс (вулиця Хабу досі коштує ≈ +4–6 мс) |
| **Рослини** (дерева, кущі, каміння, трава) | **Готово, за замовчуванням вимкнено (22.10).** Розташування — за розсіюванням самої Kenshi (генератор випадкових чисел і формули звірено з грою), дерева / кущі / каміння підвантажуються за зонами, трава — колом 50 м навколо камери; скелі з текстурою землі — як у Kenshi. Ціна +0,4…1,5 мс, без ривків у безперервному польоті. Налаштування: увімк. / вимк., дальність, густина трави |
| Автопошук бібліотеки Steam | Ще ні: задайте `KENSHI_DIR` |
| Моди з Workshop | Код є, **на справжніх даних Workshop ще не перевірено** |
| **Продуктивність, 1920×1080, HD 520** | Фіксований маршрут польоту **p99 ≈ 15,0 мс** (19.10.2026; 17.10 — 19 мс, 13.10 — 30–31 мс) — **мету 60 FPS (≤ 16,7 мс) на маршруті досягнуто**. Допомогли: відсікання невидимого на GPU (HZB), **перевикористання освітленого ґрунту між кадрами** (оновлення раз на 16 кадрів за замовчуванням; Settings: 4 / 8 / 16), resolve ґрунту більше не пише глибину, динамічна роздільність лише в русі (0,75, нативна, коли камера стоїть). Відомий хвіст: безперервний політ 50 м/с, p99 ≈ 18,9 мс. Довгострокова мета — 99 FPS на ландшафті. **Нове (22.10):** налаштування «Ground quality» — *Fast motion* (за замовчуванням: звичайна фільтрація ґрунту, роздільність 0,75 лише в русі, перемикання без ривків), анізотропна ×16 + динамічна роздільність або ×16 + м'якше перевикористання ґрунту; стеля кадрів **«авто»: 30 FPS у спокої, 60 у русі** (у спокої вентилятор майже не чути) |
| **Чистка коду** (жовтень 2026) | **Завершено (19.10):** ≈ 85 000 рядків власного коду прибрано в трьох репозиторіях (старий ігровий прототип, залишки OpenGL, ефекти для сильніших GPU, що на HD 520 не вмикались, невикористаний кеш ґрунту на 200 МБ), плюс 2 × 131 000 рядків масивів шрифтів і завантажувач GL на 8 700. Аудит: clang-tidy, cppcheck, ASan / UBSan, покриття коду; кожна зміна звірена з еталонними кадрами до пікселя. Рушій і бібліотеки tools тепер збираються на GitHub (збірка на ноутбуці 19 → 7 хв) |

### Мета
**До ~08.11.2026: світ Kenshi під Linux зі стабільними 60 FPS на HD 520**, нативні 1920×1080: земля, небо, вода, міста й рослини з теки Kenshi, на вигляд як у Kenshi (більшість уже є, див. «Стан»; лишилося вкласти міста й рослини в бюджет).
- **«Стабільні»** — p99 інтервалу кадру ≤ 16,7 мс на фіксованому маршруті польоту над світом (місто, каньйон, пустеля, обрій; ривки підвантаження теж рахуються), плюс середнє ≤ 16,7 мс у трьох фіксованих точках і біля Хабу.

**До ~20.11.2026:** **найменше граюче ядро** поверх світу: персонаж у світі з мешем, скелетом і анімаціями Kenshi, рух кліком по власній навігаційній сітці Kenshi, загін, кілька NPC з даних міст, найпростіший ближній бій. Свідомо частково й сиро; повна заміна Kenshi — робота на кілька років.

**До 31.12.2026 (попередньо):** оболонка гри (запуск із модами, головне меню на оригінальних макетах UI Kenshi), нова гра з редактором персонажа, глибший бій і медицина, поведінка NPC і фракції, мінімальні інвентар і торгівля, збереження.

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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build monkey_dust_engine
ninja -C build md_tests
```
Залежності: SDL3, gaia-ecs, ozz-animation, Jolt Physics, miniaudio, Lua 5.4, AMD FSR 1. Recast/Detour лежить у `third_party` для запланованого руху персонажів.
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
| [`mnkey_dst_engine`](https://github.com/rdga1afk/mnkey_dst_engine) (цей) | публічний | MIT | Бібліотека рушія: читачі даних Kenshi, рендер, світ, ECS, фізика |
| [`mnkey_dst_tools`](https://github.com/rdga1afk/mnkey_dst_tools) | публічний | GPL-3.0 | Редактор, скрипти замірів і QA, офлайн-конвертери |
| `mnkey_dst` | приватний | власницька | Виконуваний файл гри, шейдери, тести, дослідження. Два репо вище — його підмодулі |

**Потік даних під час запуску:** див. схему в англійському розділі вище: інсталяція Kenshi (лише читання) → `io/` → `world/` (атлас 3,6 м + вікно 1,8 м біля камери) → `render/` (LOD → 8-байтовий G-буфер → шейдер ґрунту Kenshi) → SDL_GPU / Vulkan.

**Модулі** (`include/monkey_dust/<модуль>/`):

| Модуль | Що робить |
|---|---|
| `io/` | **Дані Kenshi:** читач і злиття FCS, TIFF без стиснення, пошук інсталяції |
| `world/` | Атлас висот і запити (`TerrainQuery`), дерево LOD ландшафту (`TerrainLodTree`, правило екранної похибки Kenshi), підвантаження ландшафту, біоми, розстановка об'єктів, погода |
| `render/` | GPU HAL над SDL_GPU, рендер LOD ландшафту й 8-байтовий G-буфер землі, відкладене освітлення, каскадні тіні (за замовчуванням вимкнено), відсікання невидимого HZB, динамічна роздільність, FSR 1, SSAO, bloom, скінінг на GPU, матеріали, вмикання/вимикання проходів (`RenderPassGraph`) для замірів, хук захоплення RenderDoc |
| `ecs/`, `components/` | gaia-ecs за фасадом `MdRegistry`/`MdEntity`; POD-компоненти (ієрархія трансформацій, рендер, аніматор, морфи) |
| `physics/` | Jolt Physics, ragdoll |
| `scripting/`, `audio/` | Пісочниця Lua 5.4 (сценарії замірів і тестів), miniaudio |
| `platform/` | Вікно, ввід, час, граф задач, INI/CVar, лог, AVX2 |

### Правове
- **Kenshi** — торгова марка Lo-Fi Games. Проєкт не пов'язаний з Lo-Fi Games і не схвалений ними. Файли гри не поширюються; потрібна власна копія.
- Сторонній код і його ліцензії: [THIRD_PARTY.md](THIRD_PARTY.md).
- Власний код Kenshi (шейдери, виконуваний файл) не копіюється. Де потрібен паритет, поведінка переноситься з відкритого коду з дозвільною ліцензією (атрибуція в [THIRD_PARTY.md](THIRD_PARTY.md)) або відтворюється за фактами.
- **Ліцензія:** MIT, див. [LICENSE](LICENSE). Виконуваний файл гри і його код — власницькі й не входять до цього репозиторію.
