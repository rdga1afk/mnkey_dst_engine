// Our own small data files (game/data/...) that the binary carries inside itself (#201 R5): the release opens no game/data path.
// DataPath("game/data/x") returns a path the loaders can open: the embedded copy is written once into ~/.cache/monkeydust, or - for development only,
// with MD_DEV_FILES=1 - the file in the source tree. A path that is not embedded is returned unchanged.
#pragma once
#include <cstddef>
#include <string>

namespace md {

struct EmbeddedFile { const char* path; const unsigned char* data; size_t size; };
void RegisterEmbeddedFiles(const EmbeddedFile* files, size_t n);   // called by the generated scripts/embed_data.py table at start-up

// The bytes of an embedded file (nullptr when not embedded), for --licenses.
const unsigned char* EmbeddedFileBytes(const char* path, size_t* size);

bool DevFilesEnabled();                  // MD_DEV_FILES=1 in the environment (always false when built with MD_NO_DEV_PATHS)
std::string DataPath(const char* rel);
// An optional development-only file (a pre-built copy of something the run time can also build): its path when MD_DEV_FILES=1 and it exists, else "" (nothing is opened).
std::string DevFile(const char* rel);

// Repo-relative data path with an ordered list of roots (migration: the game's own files live in the tools checkout, the private ones in main):
// the first root where `rel` exists wins: 1. the working directory (main's private root), 2. the tools root (`MD_TOOLS_ROOT` in the environment, else "tools").
// In a tools-only checkout the working directory IS the tools root, so step 1 finds everything. Nothing found: `rel` is returned unchanged (the caller's own error handling).
std::string RepoPath(const char* rel);
std::string RepoPathIn(const char* rel, const std::string& private_root, const std::string& tools_root);   // the same lookup with explicit roots (unit test)

// the two generated placeholder textures, as the DDS bytes (tools/gen_flat_normal_dds.py; solid white 2048x2048 BC3)
std::string FlatNormalDds();
std::string MissingDiffuseDds();

} // namespace md
