// folder_picker.h - a native OS "choose a folder" dialog.
//
// One header, two platform-specific implementations
// (folder_picker_win.cpp / folder_picker_linux.cpp - see CMakeLists.txt
// for which one gets compiled where), so callers (see
// DrawCreateVaultWizard in app_ui.cpp) don't need any #ifdef of their
// own. There's no macOS implementation because this project doesn't
// build for macOS at all yet (see CMakeLists.txt's UNIX-AND-NOT-APPLE/
// WIN32 branches) - not an oversight specific to this feature.
#pragma once

#include <string>
#include <vector>

namespace platform {

// Blocks until the user picks a folder or cancels. `title` is shown
// in the dialog's title bar/header. `initial_path` is where the
// dialog should start browsing from if it exists (both
// implementations fall back to a sensible OS default if it doesn't -
// neither treats a bad initial_path as an error).
//
// Returns true and fills out_path with an absolute path on success.
// Returns false (out_path untouched) if the user cancelled, OR if no
// native dialog could be shown at all - see each implementation's own
// comment for when that happens (e.g. neither zenity nor kdialog is
// installed on Linux). Callers should treat both cases the same way:
// fall back to whatever manual-entry path already existed rather than
// erroring out, since "the user cancelled" and "there's no dialog
// available" both just mean "nothing changed, let them try another
// way."
bool PickFolder(const std::string& title, const std::string& initial_path, std::string& out_path);

// Same contract as PickFolder above, but for picking a single
// existing *file* rather than a folder. `filter_label` (e.g. "Images
// and GIFs") and `filter_extensions` (e.g. {"png", "jpg", "gif"} -
// no leading dot) restrict the dialog to matching files; pass an
// empty `filter_extensions` to allow any file. Used by the
// background-picker grid in Settings (see DrawSettingsPanel in
// app_ui.cpp) to let the user choose an image/GIF from anywhere on
// disk, which then gets copied into assets/backgrounds/ - this
// function itself never copies or reads the file, only returns the
// path the user picked.
bool PickFile(const std::string& title, const std::string& filter_label,
              const std::vector<std::string>& filter_extensions, std::string& out_path);

}  // namespace platform
