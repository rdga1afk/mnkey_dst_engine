// SPIR-V compiled into the binary (#201 R2). The game build generates a table {path, bytes, size} of shaders/spirv/*.spv and registers it before any pipeline is created;
// the HAL reads a shader from disk first (development: scripts/compile_shaders.sh output) and falls back to the embedded copy when the file is not there.
// MD_SHADERS_EMBEDDED_ONLY=1 never touches shaders/ (release behaviour, used by the release check).
#pragma once
#include <cstddef>

namespace md {
struct EmbeddedSpirv { const char* path; const unsigned char* data; size_t size; };   // path as MakeSpvPath builds it: "shaders/spirv/<name>.spv"
void RegisterEmbeddedSpirv(const EmbeddedSpirv* table, size_t count);
}
