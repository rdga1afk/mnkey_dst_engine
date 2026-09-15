#pragma once
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <cstdint>
#include <cstring>

// ── MaterialDesc ──────────────────────────────────────────────────────────────
// OGRE v1 + O3DE materialtype-inspired declarative material descriptor.
// Supports single-level parent inheritance: derived types inherit all fields
// from the parent and override only what they declare.
//
// JSON format:
//   {
//     "name":            "npc_skinned",
//     "parent":          "base_pbr",            // optional — inherits parent fields
//     "vert_shader":     "shaders/pbr.vert.spv", // optional if parent has one
//     "frag_shader":     "shaders/pbr.frag.spv", // optional if parent has one
//     "blend":           "opaque",              // "opaque"|"alpha"|"additive"
//     "cull":            "back",                // "back"|"front"|"none"
//     "topology":        "triangles",           // "triangles"|"lines"|"points"
//     "depth_write":     true,
//     "depth_test":      true,
//     "version":         1,
//     "shader_features": ["SF_Skinned", "SF_Shadows"]  // optional bitmask
//   }
//
// Usage:
//   MaterialDesc desc;
//   MaterialDesc::LoadFromJson(desc, json_str);   // resolves parent automatically
//   GpuPipeline::Desc pd = MaterialDesc::BuildPipelineDesc(desc, layout);
//
// Registration (for base types used as parents):
//   MaterialTypeRegistry::Get().Register(desc);

static constexpr int MD_MAT_MAX_TYPES  = 32;
static constexpr int MD_MAT_NAME_LEN   = 64;
static constexpr int MD_MAT_SHADER_LEN = 128;

enum class MatBlend : uint8_t { Opaque = 0, Alpha = 1, Additive = 2 };
enum class MatCull  : uint8_t { Back   = 0, Front = 1, None     = 2 };

struct MaterialDesc {
    char        name        [MD_MAT_NAME_LEN]    = {};
    char        parent_name [MD_MAT_NAME_LEN]    = {};  // empty = no parent
    char        vert_shader [MD_MAT_SHADER_LEN]  = {};
    char        frag_shader [MD_MAT_SHADER_LEN]  = {};
    uint32_t    shader_features = SF_None;               // bitmask ShaderFeature
    MatBlend    blend           = MatBlend::Opaque;
    MatCull     cull            = MatCull::Back;
    GpuTopology topology        = GpuTopology::TRIANGLES;
    uint8_t     version         = 1;
    bool        depth_write     = true;
    bool        depth_test      = true;
    uint8_t     _pad[1]         = {};

    // Parse from JSON string (strstr-based, no external library).
    // Resolves parent via MaterialTypeRegistry before applying own fields.
    // Required: "name". If no parent: also "vert_shader" + "frag_shader".
    static bool LoadFromJson(MaterialDesc& out, const char* json) noexcept;

    // Build a GpuPipeline::Desc from this descriptor.
    // Caller provides vertex layout; shader_features is propagated to d.shader_features.
    static GpuPipeline::Desc BuildPipelineDesc(const MaterialDesc& md,
                                               const GpuVertexLayout& layout) noexcept;
};

// ── MaterialTypeRegistry ──────────────────────────────────────────────────────
// Singleton store for named base material types used as parents.
// Register base types at init before loading derived materials.
// Header-only; flat static array — no malloc.
class MaterialTypeRegistry {
public:
    static MaterialTypeRegistry& Get() {
        static MaterialTypeRegistry inst;
        return inst;
    }

    bool Register(const MaterialDesc& desc) noexcept {
        if (!desc.name[0]) return false;
        for (int i = 0; i < count_; ++i) {
            if (strcmp(types_[i].name, desc.name) == 0) {
                types_[i] = desc;
                return true;
            }
        }
        if (count_ >= MD_MAT_MAX_TYPES) return false;
        types_[count_++] = desc;
        return true;
    }

    const MaterialDesc* Find(const char* name) const noexcept {
        if (!name || !name[0]) return nullptr;
        for (int i = 0; i < count_; ++i)
            if (strcmp(types_[i].name, name) == 0) return &types_[i];
        return nullptr;
    }

    void Clear() noexcept { count_ = 0; }
    int  Count() const noexcept { return count_; }

private:
    MaterialTypeRegistry() = default;
    MaterialDesc types_[MD_MAT_MAX_TYPES] = {};
    int          count_ = 0;
};
