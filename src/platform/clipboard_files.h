// clipboard_files: exchange FILES (not text) with other applications through
// the system clipboard, so a file copied in CryptVault can be pasted into
// Dolphin / GNOME Files / Thunar and the other way round. This is the
// substitute for dragging files out of the window, which GLFW cannot start.
//
// GLFW's clipboard only carries plain text, so this uses the standard helper
// commands as optional runtime tools - `wl-copy`/`wl-paste` (wl-clipboard) on
// Wayland, `xclip` on X11 - run without a shell. If neither is installed the
// functions return false and CryptVault keeps working with its own internal
// clipboard.
//
// Format: KDE/LXQt read `text/uri-list`; GNOME, Cinnamon, MATE, XFCE
// (Nautilus/Nemo/Caja/Thunar) read `x-special/gnome-copied-files`
// ("copy" or "cut" on the first line, then file:// URIs). The format is picked
// from $XDG_CURRENT_DESKTOP; CRYPTVAULT_CLIPBOARD_FORMAT=uri-list|gnome
// overrides. The tools can only publish one format at a time.
#pragma once

#include <string>
#include <vector>

namespace platform {

// Publishes `paths` (absolute) as copied (or cut) files. Returns false if no
// helper tool is available or it failed; `hint` then says what to install.
bool CopyFilesToSystemClipboard(const std::vector<std::string>& paths, bool cut, std::string* hint);

// Reads files another app put on the clipboard. Fills `paths` (existing,
// absolute) and `cut`; false if none/unavailable. Blocks for at most ~1 s.
bool ReadFilesFromSystemClipboard(std::vector<std::string>* paths, bool* cut);

// Parsing/formatting helpers, public so they can be unit-tested.
std::string FileUriFromPath(const std::string& path);                 // "/a b" -> "file:///a%20b"
std::string PathFromFileUri(const std::string& uri);                  // "" if not a file:// URI
std::string BuildClipboardPayload(const std::vector<std::string>& paths, bool cut, bool gnome_format);
std::vector<std::string> ParseClipboardPayload(const std::string& payload, bool* cut);

}  // namespace platform
