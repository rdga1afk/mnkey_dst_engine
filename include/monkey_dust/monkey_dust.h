#pragma once
// monkey_dust engine — umbrella header
// Include only what is needed to reduce compile times.
// Individual subsystem headers preferred in source files.

#include <monkey_dust/ecs/engine_context.h>
#include <monkey_dust/platform/math_types.h>
#include <monkey_dust/ai/behavior_tree.h>
#include <monkey_dust/ai/bt_action_registry.h>
#include <monkey_dust/nav/nav_system.h>
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/save/save_system.h>
#include <monkey_dust/scripting/lua_system.h>
