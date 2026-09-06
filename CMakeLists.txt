cmake_minimum_required(VERSION 3.20)
project(monkey_dust_engine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── third_party — engine repo тепер self-contained ───────────────────────────
# Всі залежності живуть в engine/third_party/ (субмодулі engine repo).
# При standalone build (engine CI): CMAKE_CURRENT_SOURCE_DIR = engine/
# При monorepo build:               CMAKE_CURRENT_SOURCE_DIR = root/engine/
# В обох випадках third_party/ резолвиться правильно.
set(MD_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# Додаємо compile targets тільки якщо ще не визначені (при monorepo build
# root може додати їх раніше — захищаємо від подвійного add_subdirectory).
if(NOT TARGET glm)
    add_subdirectory(${MD_THIRD_PARTY}/glm EXCLUDE_FROM_ALL)
endif()
# flecs — task #8. Vendored in B2 as a standalone, parallel dependency;
# B3.4 makes it monkey_dust_engine's actual ECS backend (see md_registry.h),
# replacing entt::registry.
if(NOT TARGET flecs)
    add_subdirectory(${MD_THIRD_PARTY}/flecs EXCLUDE_FROM_ALL)
endif()
# FLECS_SANITIZE (flecs 4.1.6, flecs.h ~L92) enables expensive internal
# consistency checks — catches stale-pointer/use-after-structural-change
# bugs (the B3.4 class) immediately instead of as an intermittent crash.
# PUBLIC so it propagates to every TU that includes flecs.h through the
# `flecs` target, not just flecs.c's own compilation.
target_compile_definitions(flecs PUBLIC $<$<CONFIG:Debug>:FLECS_SANITIZE>)
# NOTE (Phase 2.1 concurrency audit, 2026-07-23): tests/test_job_graph_stress.cpp
# + ThreadSanitizer confirmed a real data race in flecs's own
# world->info.queries_ran_total counter (bare `++` via ecs_os_linc, flecs.h
# ~L3944-3956, when FLECS_ACCURATE_COUNTERS is undefined — the current
# state). Tried enabling FLECS_ACCURATE_COUNTERS here to fix it (routes the
# increment through a real atomic, ecs_os_lainc/posix_lainc using
# __sync_add_and_fetch) — but this vendored flecs 4.1.6's OWN
# ecs_query_iter (flecs.c:87183) has a pre-existing type-mismatch bug: it
# calls ecs_os_linc (expects int64_t*) on ecs_query_t::eval_count, which is
# declared int32_t — enabling the define does not compile as shipped. NOT
# fixing this here: it requires patching vendored third-party source
# (flecs.c), out of this audit's scope without an explicit decision to do
# so. See the audit report for the full writeup — the race itself is
# confirmed benign (stats-only counter, never read by query-correctness
# logic, no entity/component data corruption), just real UB.
if(NOT TARGET Recast)
    set(RECASTNAVIGATION_DEMO     OFF CACHE BOOL "" FORCE)
    set(RECASTNAVIGATION_TESTS    OFF CACHE BOOL "" FORCE)
    set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${MD_THIRD_PARTY}/recastnavigation)
endif()
if(NOT TARGET ozz_base)
    set(ozz_build_tools    OFF CACHE BOOL "" FORCE)
    set(ozz_build_fbx      OFF CACHE BOOL "" FORCE)
    set(ozz_build_gltf     OFF CACHE BOOL "" FORCE)
    set(ozz_build_samples  OFF CACHE BOOL "" FORCE)
    set(ozz_build_howtos   OFF CACHE BOOL "" FORCE)
    set(ozz_build_tests    OFF CACHE BOOL "" FORCE)
    set(ozz_build_data     OFF CACHE BOOL "" FORCE)
    add_subdirectory(${MD_THIRD_PARTY}/ozz-animation)
endif()
if(NOT TARGET Jolt)
    set(TARGET_UNIT_TESTS       OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD      OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES          OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS     OFF CACHE BOOL "" FORCE)
    set(CPP_EXCEPTIONS_ENABLED  OFF CACHE BOOL "" FORCE)
    # monkey_dust uses Jolt's CPU-side physics only, never its built-in
    # hair/cloth GPU compute-shader renderer -- these upstream Jolt
    # defaults are all ON and, on Linux, JPH_USE_VK unconditionally emits
    # an HLSL->SPIR-V compile command via a guessed `dxc` path
    # (Jolt.cmake: "string(REPLACE glslc dxc ...)", no existence check,
    # unlike the DX12 branch's graceful `if(NOT DXC_COMPILER)` fallback)
    # that hard-fails the BUILD (not configure) step whenever the real
    # DirectXShaderCompiler isn't installed -- true on every plain Linux
    # dev machine without a manually-installed dxc. A stale manual
    # -DJPH_USE_VK=OFF in the long-lived build/ dir's cache had been
    # masking this on this exact machine; any fresh clone/worktree hits
    # it. Disabling explicitly here so it can never regress silently.
    # Only JPH_USE_VK is unsafe here: on Linux it's the sole compute-backend
    # branch NOT gated behind a platform check (JPH_USE_DX12/MTL are
    # WIN32/APPLE-only internally in Jolt.cmake and stay inert here
    # regardless of value; JPH_USE_CPU_COMPUTE needs no shader compiler).
    set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
    add_subdirectory(${MD_THIRD_PARTY}/JoltPhysics/Build)
    set_target_properties(Jolt PROPERTIES PRECOMPILE_HEADERS "")
endif()

# Lua 5.4: prefer static build (via FetchContent in root CMakeLists, MD_STATIC_DEPS=ON).
# Fall back to system dynamic library when MD_STATIC_DEPS=OFF.
if(NOT MD_STATIC_DEPS)
    # Cross-platform system Lua discovery (fallback only).
    find_library(MD_LUA54_LIB NAMES lua5.4 lua-5.4 QUIET)
    find_path   (MD_LUA54_INC lua5.4/lua.h QUIET)
    if(MD_LUA54_LIB AND MD_LUA54_INC)
        set(LUA_LIBRARIES   "${MD_LUA54_LIB}")
        set(LUA_INCLUDE_DIR "${MD_LUA54_INC}")
        message(STATUS "[monkey_dust] Lua 5.4 (system dynamic): ${MD_LUA54_LIB}")
    else()
        find_package(Lua 5.4 REQUIRED)
        message(STATUS "[monkey_dust] Lua 5.4 (find_package): ${LUA_LIBRARIES}")
    endif()
    # LUA_INCLUDE_DIR already set; LUA_LIBRARIES points to .so
endif()
# When MD_STATIC_DEPS=ON: LUA_LIBRARIES=lua54_static, LUA_INCLUDE_DIR set in root CMakeLists.

file(GLOB_RECURSE ENGINE_SRC src/*.cpp)
# stb_image_impl, glad_impl, cgltf_impl must NOT be in the engine static lib —
# each binary (game, editor, demo) compiles them directly to avoid duplicate symbols
# when the same impl appears in both the engine lib and the binary.
list(FILTER ENGINE_SRC EXCLUDE REGEX "stb_image_impl\\.cpp$")
list(FILTER ENGINE_SRC EXCLUDE REGEX "glad_impl\\.cpp$")
list(FILTER ENGINE_SRC EXCLUDE REGEX "cgltf_impl\\.cpp$")
# MaskedOcclusionCulling files need specific per-file compile flags (see below).
list(FILTER ENGINE_SRC EXCLUDE REGEX "vendor/moc/.*\\.cpp$")
# transform_soa_simd_avx2.cpp needs its own unconditional -mavx2/-mfma (or
# /arch:AVX2) regardless of build type/config — same reasoning as MOC below.
list(FILTER ENGINE_SRC EXCLUDE REGEX "transform_soa_simd_avx2\\.cpp$")
# embedded_fonts.cpp moved to tools/ — only editor needs it, not the engine lib.
list(FILTER ENGINE_SRC EXCLUDE REGEX "vendor/fonts/.*\\.cpp$")
add_library(monkey_dust_engine STATIC ${ENGINE_SRC})

# ── Unity Build — 110 engine .cpp → ~14 batched TU ──────────────────────────
# Clean build: 110 compiles → ~14. Incremental: файл потрапляє в один batch,
# перебудовується тільки той batch (не всі 110).
# BATCH_SIZE=8: збалансовано між compile parallelism та batch size.
# Виключення: MaskedOcclusionCulling (різні ISA flags — вже виключено вище).
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    set_target_properties(monkey_dust_engine PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 4
    )
    # Exclude files with ODR conflicts or duplicate static helpers.
    # flare/: item_loader/enemy_loader/power_loader define static ChainOpen().
    # combat/: engine + game both implement PowerManager — unity batch collides with game .o.
    # NOTE: explicit list (not GLOB) to avoid CMake re-configure on every ninja run.
    set(_UNITY_EXCLUDE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/flare/item_loader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/flare/enemy_loader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/flare/power_loader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/flare/map_loader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/combat/power_manager.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/combat/hit_zones.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/combat/power_def.cpp"
        # physics/ragdoll.cpp: its own JPH_DEBUG_RENDERER-guarded Jolt/Skeleton
        # includes are sensitive to which files land in the same unity batch —
        # observed a real "undefined symbol: JPH::SkeletonPose::SetSkeleton"
        # link failure (libJolt.a DOES define it) purely from an unrelated
        # engine/src/ file-count change shifting batch composition (2026-08-08).
        "${CMAKE_CURRENT_SOURCE_DIR}/src/physics/ragdoll.cpp"
        # hot/: multiple files define static s_mtime() — each must be its own TU.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/hot/gameplay_module.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/hot/editor_module.cpp"
    )
    # Filter to only existing files (safe if some are absent)
    foreach(_f ${_UNITY_EXCLUDE_FILES})
        if(EXISTS "${_f}")
            set_source_files_properties("${_f}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        endif()
    endforeach()
    message(STATUS "[monkey_dust] engine Unity Build: ENABLED (batch=4, flare/+combat/ excluded)")
endif()

# ── MaskedOcclusionCulling (Intel, Apache 2.0) ────────────────────────────────
# Main .cpp must be compiled WITHOUT AVX2/SSE4 — it's a dispatcher that
# detects CPU features at runtime and creates the appropriate implementation.
# AVX2 .cpp must be compiled WITH -mavx2 for the AVX2 implementation.
set(MOC_BASE_CPP   src/vendor/moc/MaskedOcclusionCulling.cpp)
set(MOC_AVX2_CPP   src/vendor/moc/MaskedOcclusionCullingAVX2.cpp)
set(MOC_AVX512_CPP src/vendor/moc/MaskedOcclusionCullingAVX512.cpp)
target_sources(monkey_dust_engine PRIVATE
    ${MOC_BASE_CPP} ${MOC_AVX2_CPP} ${MOC_AVX512_CPP})
if(NOT MSVC)
    # GCC 13: always_inline intrinsics require matching ISA flags even in Debug.
    # Base: needs -mxsave (_xgetbv) + -msse4.1 (_mm_mullo_epi32); no AVX.
    set_source_files_properties(${MOC_BASE_CPP} PROPERTIES
        COMPILE_FLAGS "-msse4.1 -mxsave -mno-avx -mno-avx2")
    # AVX2: needs -mfma (FMA instructions used alongside AVX2 in MOC).
    set_source_files_properties(${MOC_AVX2_CPP} PROPERTIES
        COMPILE_FLAGS "-mavx2 -mfma")
    # AVX512 stub: same as base.
    set_source_files_properties(${MOC_AVX512_CPP} PROPERTIES
        COMPILE_FLAGS "-msse4.1 -mxsave -mno-avx -mno-avx2")
else()
    set_source_files_properties(${MOC_BASE_CPP} PROPERTIES
        COMPILE_FLAGS "/arch:SSE2")
    set_source_files_properties(${MOC_AVX2_CPP} PROPERTIES
        COMPILE_FLAGS "/arch:AVX2")
    set_source_files_properties(${MOC_AVX512_CPP} PROPERTIES
        COMPILE_FLAGS "/arch:SSE2")
endif()

# ── transform_soa_simd_avx2.cpp (Phase 2.2/3 audit) ──────────────────────────
# Same per-TU-flags pattern as MOC above: this file must ALWAYS have AVX2
# available regardless of CMAKE_BUILD_TYPE, so tests/test_transform_soa_simd_
# parity.cpp can call md::simd::ComputeDistSqAVX2 directly and compare it
# against the scalar reference on any build config, not just Release/MSVC
# (the only configs that currently get a global AVX2 flag — see the
# CMAKE_BUILD_TYPE STREQUAL "Release" block further down).
set(TRANSFORM_SOA_AVX2_CPP src/world/transform_soa_simd_avx2.cpp)
target_sources(monkey_dust_engine PRIVATE ${TRANSFORM_SOA_AVX2_CPP})
if(NOT MSVC)
    set_source_files_properties(${TRANSFORM_SOA_AVX2_CPP} PROPERTIES
        COMPILE_FLAGS "-mavx2 -mfma")
else()
    set_source_files_properties(${TRANSFORM_SOA_AVX2_CPP} PROPERTIES
        COMPILE_FLAGS "/arch:AVX2")
endif()

add_library(monkey_dust::engine ALIAS monkey_dust_engine)

target_include_directories(monkey_dust_engine
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC  ${MD_THIRD_PARTY}/ozz-animation/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    # Third-party (PUBLIC so consumers also see them)
    PUBLIC  ${MD_THIRD_PARTY}/flecs
    PUBLIC  ${MD_THIRD_PARTY}/glm
    PRIVATE ${MD_THIRD_PARTY}/recastnavigation/Recast/Include
    PRIVATE ${MD_THIRD_PARTY}/recastnavigation/Detour/Include
    PRIVATE ${MD_THIRD_PARTY}/recastnavigation/DetourCrowd/Include
    PUBLIC  ${MD_THIRD_PARTY}/JoltPhysics
    PRIVATE ${LUA_INCLUDE_DIR}
    # Vendor: glad, stb headers — PUBLIC so game/ and tools/ can find them too
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor
)

# ── Granite migration M2 (engine/src/render/granite_backend.cpp) ────────────
# GRANITE_ROOT resolved + granite-vulkan target built in root CMakeLists.txt
# BEFORE this add_subdirectory(engine) call. granite_backend.cpp is an empty
# TU (no Granite symbols referenced) when MD_USE_GRANITE is undefined, so
# engine/ compiles and links for the default USE_SDL3 path with zero Granite
# dependency when USE_GRANITE=OFF (docs/GRANITE_MIGRATION_PLAN_M0_M6.md M2's
# own constraint).
if(USE_GRANITE AND TARGET granite-vulkan)
    target_include_directories(monkey_dust_engine PRIVATE
        "${GRANITE_ROOT}/vulkan"
        "${GRANITE_ROOT}/event"
        "${GRANITE_ROOT}/application/events"
        "${GRANITE_ROOT}/util"
        "${GRANITE_ROOT}/threading")
    target_compile_definitions(monkey_dust_engine PUBLIC MD_USE_GRANITE)
    target_link_libraries(monkey_dust_engine PUBLIC granite-vulkan granite-threading granite-util)
endif()

if(NOT USE_SDL3)
    message(FATAL_ERROR
        "monkey_dust requires -DUSE_SDL3=ON. "
        "The Raylib/OpenGL backend has been removed (see CLAUDE.md П.1-П.3).")
endif()

# When engine is built standalone (not via root CMakeLists), find SDL3 here.
# Root CMakeLists already calls find_package(SDL3) before add_subdirectory(engine).
if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
    find_package(SDL3 REQUIRED CONFIG)
endif()

# RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7.1): render
# backend selection. Only SDLGPU is real and wired into game_init.cpp's
# runtime backend creation -- the other 5 compile an empty stub class
# (engine/include/monkey_dust/render/backend/stubs/, validated for real by
# render_backend_stub_compile_check.cpp) implementing IRenderBackend, but
# are not otherwise used (§9: real embedding is a separate future phase).
set(MD_RENDER_BACKEND "SDLGPU" CACHE STRING
    "Render backend: SDLGPU, GODOT, FILAMENT, BGFX, DILIGENT, OGRENEXT")
set_property(CACHE MD_RENDER_BACKEND PROPERTY STRINGS
    SDLGPU GODOT FILAMENT BGFX DILIGENT OGRENEXT)
target_compile_definitions(monkey_dust_engine PUBLIC MD_RENDER_BACKEND_${MD_RENDER_BACKEND})

# SDL3 + SDL_GPU is the only REAL rendering backend (see MD_RENDER_BACKEND
# above for the other 5's compile-only stub scaffolding).
# GLM provides math types; MD_LOG_NO_RAYLIB replaces TraceLog with fprintf.
target_compile_definitions(monkey_dust_engine PUBLIC USE_SDL3 USE_GLM MD_LOG_NO_RAYLIB MD_SDL_GPU)
if(MD_STATIC_DEPS)
    target_link_libraries(monkey_dust_engine PUBLIC SDL3::SDL3-static)
else()
    target_link_libraries(monkey_dust_engine PUBLIC SDL3::SDL3)
endif()
target_include_directories(monkey_dust_engine PRIVATE
    # MOC vendor: .cpp files include "MaskedOcclusionCulling.h" relatively
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vendor/moc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/moc)
target_include_directories(monkey_dust_engine PUBLIC
    # miniaudio.h copied from raylib/src/external/ → engine/src/audio/external/miniaudio.h
    # Raylib submodule no longer needed.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio)

if(MD_STATIC_DEPS)
    set(_LUA_LINK lua54_static)
else()
    set(_LUA_LINK ${LUA_LIBRARIES})
endif()
target_link_libraries(monkey_dust_engine PRIVATE
    Recast Detour DetourCrowd
    Jolt
    ${_LUA_LINK}
    ozz_animation
    ozz_animation_offline
    ozz_base
)
target_link_libraries(monkey_dust_engine PUBLIC flecs)
# gameplay_module.cpp uses dlopen/dlclose (POSIX dl)
if(UNIX AND NOT APPLE)
    target_link_libraries(monkey_dust_engine PRIVATE dl)
endif()

if(USE_GLM AND NOT USE_SDL3)
    # In SDL3 path USE_GLM is already set above; this handles explicit -DUSE_GLM
    # without USE_SDL3 (uncommon but supported).
    target_compile_definitions(monkey_dust_engine PUBLIC USE_GLM)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR MONKEY_DUST_EDITOR)
    target_compile_definitions(monkey_dust_engine PUBLIC DEBUG)
endif()
# Vulkan validation layers (gpu_device.cpp) are opt-in via a narrower flag than
# DEBUG above — DEBUG also turns on for MONKEY_DUST_EDITOR=ON (the normal dev
# build with editor panels), which is unrelated to wanting validation-layer
# overhead on every pipeline creation. Real CMAKE_BUILD_TYPE=Debug only.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(monkey_dust_engine PUBLIC MD_GPU_VALIDATION)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(monkey_dust_engine PRIVATE /O2 /arch:AVX2)
    else()
        # Phase 3 (audit): -march=x86-64-v2 alone has NO AVX2 — README.md's
        # own documented target hardware is "Intel HD 520 (Skylake AVX2)",
        # so transform_soa.cpp's #ifdef __AVX2__ BulkComputeDistSq/
        # BulkComputeLOD path was silently compiling to its SSE2/scalar
        # fallback on every Linux/Mac Release build, while Windows (MSVC
        # /arch:AVX2 above) got the real vectorized path — an unintentional
        # per-platform behavior/perf split, not a deliberate choice. Chose
        # aligning flags over runtime CPU dispatch: the project's own
        # documented minimum hardware already guarantees AVX2/FMA, so there
        # is no compatibility need runtime dispatch would serve, and this is
        # zero runtime overhead vs a dispatch branch on every call.
        target_compile_options(monkey_dust_engine PRIVATE -O2 -march=x86-64-v2 -mavx2 -mfma)
    endif()
endif()

# ── Warning hygiene (Phase 4.3 audit) ────────────────────────────────────────
# PRIVATE: only monkey_dust_engine's OWN sources get these flags, not the
# vendored third_party/ subdirectories added above (flecs/Jolt/recast/ozz
# each configure their own warning levels; recastnavigation's own CMakeLists
# already sets -Wall/-Werror for its own targets independently of this).
# Not -Werror here — this is a count-and-report exercise (see audit report),
# not a "fix everything now" change; escalating to -Werror is a follow-up
# decision once the current warning list has been triaged.
if(MSVC)
    target_compile_options(monkey_dust_engine PRIVATE /W4)
else()
    target_compile_options(monkey_dust_engine PRIVATE -Wall -Wextra)
endif()

# ── Precompiled Header (PCH) ─────────────────────────────────────────────────
# Vulkan perf pattern: avoid redundant preprocessing of heavy stdlib/SDL/entt
# headers in every TU. PCH compiles them once → 40-60% faster incremental builds.
# Note: PCH is NOT applied to MaskedOcclusionCulling (different ISA flags).
# Not applied to vendor impl files (already excluded from ENGINE_SRC above).
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_precompile_headers(monkey_dust_engine PRIVATE
        <cstdint> <cstdlib> <cstring> <cstdio> <cmath> <cstddef>
    )
    message(STATUS "[monkey_dust] PCH: ENABLED (stdlib only, ~2MB)")
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
# Only build engine-standalone tests when this is the top-level project.
# When engine/ is a subdirectory of monkey_dust, tests are in the parent's tests/.
option(MONKEY_DUST_BUILD_TESTS "Build engine tests" OFF)
if(MONKEY_DUST_BUILD_TESTS AND CMAKE_PROJECT_NAME STREQUAL "monkey_dust_engine")
    add_subdirectory(tests)
endif()
