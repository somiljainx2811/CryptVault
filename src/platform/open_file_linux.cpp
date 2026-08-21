// open_file_linux.cpp - Linux implementation of
// platform::OpenFileWithDefaultApp, via xdg-open - the standard
// freedesktop.org tool that asks whatever the current desktop
// environment is (GNOME, KDE, etc.) to open a file with its
// associated application, the same idea as ShellExecute on Windows.
// Same reasoning as folder_picker_linux.cpp for shelling out instead
// of linking a desktop toolkit directly: xdg-open ships on basically
// every desktop Linux install without this project needing to link
// against GTK/Qt/etc. for something this small.
#include "platform/open_file.h"

#include <cstdlib>

namespace platform {

namespace {
// Duplicated from folder_picker_linux.cpp rather than shared via a
// header - both are a few lines, self-contained, and have no reason
// to stay in sync with each other beyond doing the same generic
// "quote a shell argument safely" job.
std::string ShellQuote(const std::string& s) {
    std::string quoted = "'";
    for (char c : s) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

bool CommandExists(const std::string& name) {
    std::string check = "command -v " + ShellQuote(name) + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}
}  // namespace

bool OpenFileWithDefaultApp(const std::string& path, std::string* error) {
    if (!CommandExists("xdg-open")) {
        if (error) *error = "xdg-open isn't installed";
        return false;
    }
    // Backgrounded (&) and redirected so this call returns
    // immediately rather than blocking until whatever application
    // xdg-open launches exits - matching the "non-blocking" contract
    // in open_file.h. Trade-off: the shell's own exit code (which is
    // all `rc` below reflects) only confirms the background job was
    // *started*, not that xdg-open subsequently found a real handler
    // for this file type - a truly synchronous success/failure read
    // would mean blocking on the launched application itself, which
    // defeats the point of being non-blocking. Good enough for "did
    // we even manage to hand this off to the OS."
    std::string cmd = "xdg-open " + ShellQuote(path) + " >/dev/null 2>&1 &";
    int rc = std::system(cmd.c_str());
    if (rc != 0 && error) {
        *error = "xdg-open couldn't launch a handler for this file";
    }
    return rc == 0;
}

}  // namespace platform
