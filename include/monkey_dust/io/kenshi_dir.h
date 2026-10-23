// Where the player's own Kenshi install is, and a way to prefer its files over the repo's offline copies. The engine ships no Kenshi
// data: a file found here is read straight from the install, and when it is missing the caller's fallback path is used.
#pragma once
#include <string>
#include <vector>

namespace md {

// $KENSHI_DIR if set and non-empty (even if it points nowhere: that disables the install on purpose), else "tmp_/kenshi" when it
// holds data/gamedata.base, else the path saved by the first-run picker (~/.config/monkeydust/kenshi_dir), else the player's Steam install
// (native Steam, Flatpak Steam, Steam library folders from libraryfolders.vdf), else "". The result is found once and kept.
std::string KenshiDir();

// The Steam-installed Kenshi folders that exist under `home` (a folder counts when it holds data/gamedata.base), best first; for tests and the picker.
std::vector<std::string> FindSteamKenshi(const std::string& home);

// ~/.config/monkeydust/kenshi_dir: the folder the player picked once. SaveKenshiDir returns false when the folder has no data/gamedata.base or the file cannot be written.
bool SaveKenshiDir(const std::string& dir, const std::string& home);

// The folder saved by the first-run picker under `home` ("" when none, or when it is no longer a Kenshi folder). A folder saved under the old name ~/.config/monkey_dust is copied to ~/.config/monkeydust once.
std::string SavedKenshiDir(const std::string& home);

// First-run picker support. KenshiFolderNeedsPrompt: no KENSHI_DIR, no dev copy and no folder saved yet (the window must be shown). DetectedKenshiDir: the saved
// folder, else the first Steam one, else "" (what the window pre-fills). SetKenshiDir: the confirmed folder for this process (call after SaveKenshiDir; beats everything).
// IsKenshiFolder: the folder holds data/gamedata.base.
bool KenshiFolderNeedsPrompt();
std::string DetectedKenshiDir();
void SetKenshiDir(const std::string& dir);
bool IsKenshiFolder(const std::string& dir);

// "<KenshiDir()>/data/newland/land/<name>" if that file exists, else `fallback`. Logs the choice once per name.
std::string KenshiLandFile(const char* name, const char* fallback);

} // namespace md
