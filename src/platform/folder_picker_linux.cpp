// folder_picker_linux.cpp - Linux implementation of
// platform::PickFolder.
//
// Unlike Windows, there's no single OS-blessed folder-picker API to
// call into directly - "native" on Linux desktops means whichever
// dialog toolkit the user's desktop environment actually uses (GTK's
// on GNOME, Qt's on KDE, etc.), and linking against either of those
// directly would mean a much heavier build dependency than this
// project has anywhere else (no GTK/Qt dependency exists in
// CMakeLists.txt today). The pragmatic, widely-used middle ground -
// what a lot of cross-platform native apps do - is to shell out to
// whichever small, purpose-built CLI dialog tool is already installed
// (zenity on GNOME-ish systems, kdialog on KDE-ish ones): both pop up
// a real native-looking folder picker and print the chosen path to
// stdout, with no toolkit linkage needed on our side at all.
//
// If neither is installed, this returns false - same as a cancelled
// dialog - so the caller (DrawCreateVaultWizard) falls back to its
// existing manual text-entry field rather than erroring out.
#include "platform/folder_picker.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace platform {

namespace {

// Runs `command` and returns its stdout with the trailing newline (if
// any) stripped, or std::nullopt if the command couldn't be run at
// all (not found, or a genuine popen failure) - NOT if it ran and the
// user just cancelled (both zenity and kdialog exit non-zero with
// empty stdout on cancel, which naturally falls out as an empty-but-
// present string here, distinct from nullopt).
std::string* RunCommandCaptureStdout(const std::string& command, std::string& out) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return nullptr;
    }
    std::array<char, 512> buffer{};
    out.clear();
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out += buffer.data();
    }
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return &out;
}

// Single-quotes `s` for safe inclusion in a shell command line (the
// standard POSIX-shell trick: close the quote, escape a literal
// single quote, reopen the quote). Needed because `title` and
// `initial_path` both end up interpolated into a command string
// below rather than passed as separate argv entries (popen() only
// runs through a shell, there's no argv-array popen variant in the
// standard library) - without this, a vault name containing e.g. an
// apostrophe would break out of the intended argument.
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

bool PickFolder(const std::string& title, const std::string& initial_path, std::string& out_path) {
    std::string result;

    if (CommandExists("zenity")) {
        std::string cmd = "zenity --file-selection --directory --title=" + ShellQuote(title);
        if (!initial_path.empty()) {
            cmd += " --filename=" + ShellQuote(initial_path + "/");
        }
        cmd += " 2>/dev/null";
        if (RunCommandCaptureStdout(cmd, result) && !result.empty()) {
            out_path = result;
            return true;
        }
        return false;  // ran, user cancelled (or a real path came back empty, which shouldn't happen)
    }

    if (CommandExists("kdialog")) {
        std::string cmd = "kdialog --title " + ShellQuote(title) + " --getexistingdirectory "
                           + ShellQuote(!initial_path.empty() ? initial_path : ".");
        cmd += " 2>/dev/null";
        if (RunCommandCaptureStdout(cmd, result) && !result.empty()) {
            out_path = result;
            return true;
        }
        return false;
    }

    // Neither is installed - genuinely no native dialog available
    // (rather than "the user cancelled"), but the caller treats these
    // identically, so there's no separate signal for it.
    return false;
}

bool PickFile(const std::string& title, const std::string& filter_label,
              const std::vector<std::string>& filter_extensions, std::string& out_path) {
    std::string result;

    // "*.png *.jpg *.gif" - zenity's --file-filter pattern syntax
    // (space-separated globs), built once and reused for both tools
    // below since kdialog's --getopenfilename accepts the same
    // "Label (*.ext *.ext) syntax.
    std::string pattern;
    if (filter_extensions.empty()) {
        pattern = "*";
    } else {
        for (size_t i = 0; i < filter_extensions.size(); ++i) {
            if (i) pattern += " ";
            pattern += "*." + filter_extensions[i];
        }
    }

    if (CommandExists("zenity")) {
        std::string cmd = "zenity --file-selection --title=" + ShellQuote(title) + " --file-filter="
                           + ShellQuote(filter_label + " | " + pattern) + " 2>/dev/null";
        if (RunCommandCaptureStdout(cmd, result) && !result.empty()) {
            out_path = result;
            return true;
        }
        return false;
    }

    if (CommandExists("kdialog")) {
        std::string cmd = "kdialog --title " + ShellQuote(title) + " --getopenfilename . "
                           + ShellQuote(pattern + "|" + filter_label) + " 2>/dev/null";
        if (RunCommandCaptureStdout(cmd, result) && !result.empty()) {
            out_path = result;
            return true;
        }
        return false;
    }

    return false;
}

}  // namespace platform
