// open_file.h - launches a real file on disk with whatever
// application the OS has associated with its type (double-click
// behavior, same as Explorer/Finder). Used for opening a vault file:
// see DrawVaultFolderScreen's double-click handling in app_ui.cpp,
// which decrypts the file to a temporary location first (this header
// only knows about real, already-decrypted files on disk - it has no
// idea what a "vault" is).
//
// Same split as folder_picker.h: one header, a Windows and a Linux
// implementation (folder_picker_win.cpp/folder_picker_linux.cpp's
// sibling files) - no macOS implementation because this project
// doesn't build for macOS at all yet.
#pragma once

#include <string>

namespace platform {

// Asks the OS to open `path` with whichever application is
// registered for its file type - non-blocking (returns once the
// request has been handed off, not once the other application has
// actually opened it). Returns false if the OS refused/failed to even
// launch anything (e.g. no association exists and the OS has no
// generic fallback) - `error`, if provided a non-null pointer, gets a
// short human-readable reason.
bool OpenFileWithDefaultApp(const std::string& path, std::string* error = nullptr);

}  // namespace platform
