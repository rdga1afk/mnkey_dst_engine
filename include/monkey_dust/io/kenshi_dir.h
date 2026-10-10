// Where the player's own Kenshi install is, and a way to prefer its files over the repo's offline copies. The engine ships no Kenshi
// data: a file found here is read straight from the install, and when it is missing the caller's fallback path is used.
#pragma once
#include <string>

namespace md {

// $KENSHI_DIR if set and non-empty (even if it points nowhere: that disables the install on purpose), else "tmp_/kenshi" when it
// holds data/gamedata.base, else "".
std::string KenshiDir();

// "<KenshiDir()>/data/newland/land/<name>" if that file exists, else `fallback`. Logs the choice once per name.
std::string KenshiLandFile(const char* name, const char* fallback);

} // namespace md
