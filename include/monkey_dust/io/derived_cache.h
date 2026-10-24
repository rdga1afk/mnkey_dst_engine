// Run-time cache of data derived from the player's install: ~/.cache/monkeydust/<input-hash>/<name>. Never committed, never shipped (#201 R3e).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md {

// FNV-1a over the size and mtime of each file: changes when the install changes.
uint64_t HashFiles(const std::vector<std::string>& files);
// Full path of <cache>/<key hex>/<name> (the file may not exist); "" when there is no home folder.
std::string CachePath(uint64_t key, const char* name);
// Reads <cache>/<key hex>/<name> into `out`; false when absent.
bool CacheRead(uint64_t key, const char* name, std::vector<uint8_t>& out);
// Writes the file (atomically, via a temp name); false when the cache folder cannot be written. Failure is not an error: the data is simply rebuilt next launch.
bool CacheWrite(uint64_t key, const char* name, const uint8_t* data, size_t n);

// Deletes the files of artifacts that this process asked for under another (older) key, then empty folders; artifacts it never asked for are left alone. Call once startup is done. Returns the number of files removed.
int CacheCollectGarbage();

} // namespace md
