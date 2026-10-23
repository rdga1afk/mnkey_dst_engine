// Run-time cache of data derived from the player's install: ~/.cache/monkey_dust/<input-hash>/<name>. Never committed, never shipped (#201 R3e).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md {

// FNV-1a over the size and mtime of each file: changes when the install changes.
uint64_t HashFiles(const std::vector<std::string>& files);
// Reads <cache>/<key hex>/<name> into `out`; false when absent.
bool CacheRead(uint64_t key, const char* name, std::vector<uint8_t>& out);
// Writes the file (atomically, via a temp name); false when the cache folder cannot be written. Failure is not an error: the data is simply rebuilt next launch.
bool CacheWrite(uint64_t key, const char* name, const uint8_t* data, size_t n);

} // namespace md
