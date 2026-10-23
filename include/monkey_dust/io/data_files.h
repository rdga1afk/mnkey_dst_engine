// Our own small data files (game/data/...) that the binary carries inside itself (#201 R5): the release opens no game/data path.
// DataPath("game/data/x") returns a path the loaders can open: the embedded copy is written once into ~/.cache/monkeydust, or - for development only,
// with MD_DEV_FILES=1 - the file in the source tree. A path that is not embedded is returned unchanged.
#pragma once
#include <cstddef>
#include <string>

namespace md {

struct EmbeddedFile { const char* path; const unsigned char* data; size_t size; };
void RegisterEmbeddedFiles(const EmbeddedFile* files, size_t n);   // called by the generated scripts/embed_data.py table at start-up

bool DevFilesEnabled();                  // MD_DEV_FILES=1 in the environment (always false when built with MD_NO_DEV_PATHS)
std::string DataPath(const char* rel);
// An optional development-only file (a pre-built copy of something the run time can also build): its path when MD_DEV_FILES=1 and it exists, else "" (nothing is opened).
std::string DevFile(const char* rel);

// the two generated placeholder textures, as the DDS bytes (tools/gen_flat_normal_dds.py; solid white 2048x2048 BC3)
std::string FlatNormalDds();
std::string MissingDiffuseDds();

} // namespace md
