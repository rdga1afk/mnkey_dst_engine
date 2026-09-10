#pragma once
// OGRE_NEXT_BACKEND_IMPLEMENTATION.md §2.2 — MdModel -> Ogre::HlmsPbsDatablock
// material conversion, PARTIAL by design (see the two documented blockers
// below, found by reading real OGRE-Next headers, not assumed).
//
// Only compiled under MD_RENDER_BACKEND_OGRENEXT (CMake option
// MD_RENDER_BACKEND=OGRENEXT, default is SDLGPU and never sees this file's
// body) -- same pattern as the render_backend/stubs/*.h headers.
#if defined(MD_RENDER_BACKEND_OGRENEXT)

#include <OgreHlmsPbsDatablock.h>
#include <monkey_dust/render/model_manager.h>

namespace md::render_backend {

// Real monkey_dust source data for a loaded glTF model: MdModel (see
// model_manager.h) has exactly two textures -- `albedo` and `metallic_rough`
// (glTF metallicRoughnessTexture convention: G=roughness, B=metallic, packed
// into ONE texture). There is no normal-map field anywhere in this
// codebase's model/prop/terrain material structs (checked model_manager.h,
// prop_mesh.h, terrain_shading_projected.h directly -- every "normal" hit
// is a per-vertex geometric normal, not a normal-map texture) -- the
// OGRE_NEXT_BACKEND_IMPLEMENTATION.md §2.2 "albedo, normal map, metallic,
// roughness" 4-field assumption does not match this project's real data;
// this bridge only converts what actually exists: albedo + packed
// metallic_rough.
//
// TWO REAL BLOCKERS found by reading OGRE-Next's actual Hlms Pbs headers
// (Components/Hlms/Pbs/include/OgreHlmsPbsPrerequisites.h,
// OgreHlmsPbsDatablock.h) -- not guessed, not yet resolved:
//
//   1. PACKED-TEXTURE CHANNEL MISMATCH. OGRE-Next's PbsTextureTypes enum
//      has PBSM_METALLIC and PBSM_ROUGHNESS as two SEPARATE texture slots
//      (`setTexture(PbsTextureTypes, ...)`, one texture object per slot).
//      There is no public channel-select/swizzle API to say "read the G
//      channel of texture X for roughness, B channel of the SAME texture X
//      for metallic" -- every swizzle/channel option found in the header
//      (rrra diffuse swizzle, detail-map R/G/B/A channel split) applies to
//      a DIFFERENT feature, not PBSM_METALLIC/PBSM_ROUGHNESS. To feed our
//      one packed metallic_rough texture into this API as-is would require
//      either (a) a texture-split preprocessing pass (extract G-channel and
//      B-channel into two standalone textures, a real new asset-pipeline
//      step, not free) or (b) writing a custom Hlms Pbs shader variant that
//      reads both channels from one bound texture (a much deeper
//      OGRE-Next customization, likely out of scope for a comparison
//      backend). NEITHER is implemented here -- ConvertModelMaterial()
//      below sets the workflow/scalar defaults and the diffuse (albedo)
//      texture only; metallic/roughness texture binding is a documented
//      TODO, not silently faked.
//
//   2. NO RETAINED SOURCE PATH. Ogre::HlmsPbsDatablock::setTexture() takes
//      a texture NAME that Ogre resolves through its own
//      Ogre::TextureGpuManager resource system -- it does not accept an
//      already-uploaded SDL_GPU texture handle. Our MdTexture (md_texture.h)
//      only stores { id, w, h, sdl_tex, sdl_sampler } -- no source file
//      path -- and ModelManager::Load() (model_manager.h) takes a path
//      argument but does not retain it in the resulting MdModel. Wiring
//      real textures through this bridge therefore needs model_manager.h's
//      MdModel to gain a stored path (or an equivalent lookup), which is a
//      change to existing, shared (non-OGRE-Next-specific) engine code --
//      flagged here, not made unilaterally as a side effect of this file.
//
// What DOES work today, verified against real OGRE-Next headers: workflow
// selection (setWorkflow(HlmsPbsDatablock::MetallicWorkflow)) and scalar
// constant fallbacks (setDiffuse/setMetalness/setRoughness) -- these take
// plain float/Vector3 values, no texture/resource-manager involvement, and
// unblock a visible (if textureless/flat-shaded) comparison render before
// the two blockers above are resolved.
void ConvertModelMaterialToPbsDatablock(const MdModel& model, Ogre::HlmsPbsDatablock* datablock);

}  // namespace md::render_backend

#endif  // MD_RENDER_BACKEND_OGRENEXT
