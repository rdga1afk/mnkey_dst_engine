#if defined(MD_RENDER_BACKEND_OGRENEXT)
#include <monkey_dust/render/backend/ogre_next_material_bridge.h>

namespace md::render_backend {

// Sets what's actually wireable today -- see the header's own comment for
// the two real, found-not-guessed blockers (packed metallic_rough texture
// channels, no retained MdTexture source path) that block real texture
// binding. Scalar/workflow-only for now: a flat, textureless PBR surface,
// not a silently-wrong texture assignment.
void ConvertModelMaterialToPbsDatablock(const MdModel& model, Ogre::HlmsPbsDatablock* datablock) {
    if (!datablock) return;

    datablock->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);

    // No texture path retained on MdModel/MdTexture (blocker #2) -- fall
    // back to flat mid-grey diffuse + a neutral metallic/roughness pair
    // rather than reading model.albedo/model.metallic_rough's pixel data
    // (MdTexture doesn't expose that either -- it's an opaque GPU handle,
    // see md_texture.h). model.valid is checked only to avoid doing this
    // for a model that failed to load in the first place.
    if (!model.valid) return;

    datablock->setDiffuse(Ogre::Vector3(0.5f, 0.5f, 0.5f));
    datablock->setMetalness(0.0f);
    datablock->setRoughness(0.5f);

    // TODO(OGRE-NEXT-BACKEND-STAGE-2.2): real albedo/metallic_rough texture
    // binding blocked on the two issues documented in
    // ogre_next_material_bridge.h -- requires (a) retaining MdModel's
    // source path so Ogre::TextureGpuManager can load its own copy, and
    // (b) either a texture-split preprocessing pass or a custom Hlms Pbs
    // shader variant to read metallic_rough's packed G/B channels into
    // OGRE-Next's separate PBSM_ROUGHNESS/PBSM_METALLIC texture slots.
}

}  // namespace md::render_backend

#endif  // MD_RENDER_BACKEND_OGRENEXT
