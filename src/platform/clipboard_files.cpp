#include "platform/clipboard_files.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace platform {

namespace {
bool Unreserved(unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/'; }
}  // namespace

std::string FileUriFromPath(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if (Unreserved(c)) out += static_cast<char>(c);
        else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 15]; }
    }
    return out;
}

std::string PathFromFileUri(const std::string& uri) {
    std::string u = uri;
    while (!u.empty() && (u.back() == '\r' || u.back() == '\n' || u.back() == ' ')) u.pop_back();
    if (u.rfind("file://", 0) != 0) return "";
    u = u.substr(7);
    if (!u.empty() && u[0] != '/') {          // file://host/path - only the local host is accepted
        size_t slash = u.find('/');
        if (slash == std::string::npos) return "";
        std::string host = u.substr(0, slash);
        if (host != "localhost") return "";
        u = u.substr(slash);
    }
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '%' && i + 2 < u.size() + 0 && std::isxdigit(static_cast<unsigned char>(u[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(u[i + 2]))) {
            out += static_cast<char>(std::stoi(u.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += u[i];
        }
    }
    return out;
}

std::string BuildClipboardPayload(const std::vector<std::string>& paths, bool cut, bool gnome_format) {
    std::string out;
    if (gnome_format) out += cut ? "cut\n" : "copy\n";
    for (size_t i = 0; i < paths.size(); ++i) {
        out += FileUriFromPath(paths[i]);
        if (i + 1 < paths.size() || !gnome_format) out += gnome_format ? "\n" : "\r\n";
    }
    return out;
}

std::vector<std::string> ParseClipboardPayload(const std::string& payload, bool* cut) {
    std::vector<std::string> paths;
    if (cut) *cut = false;
    std::istringstream in(payload);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "cut") { if (cut) *cut = true; continue; }
        if (line == "copy" || line.empty() || line[0] == '#') continue;
        std::string p = PathFromFileUri(line);
        if (!p.empty()) paths.push_back(p);
    }
    return paths;
}

#ifdef _WIN32

bool CopyFilesToSystemClipboard(const std::vector<std::string>&, bool, std::string*) { return false; }
bool ReadFilesFromSystemClipboard(std::vector<std::string>*, bool*) { return false; }

#else

namespace {

std::string FindOnPath(const char* name) {
    const char* path = std::getenv("PATH");
    std::string paths = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= paths.size()) {
        size_t end = paths.find(':', start);
        if (end == std::string::npos) end = paths.size();
        std::string dir = paths.substr(start, end - start);
        if (!dir.empty()) {
            std::string c = dir + "/" + name;
            if (::access(c.c_str(), X_OK) == 0) return c;
        }
        start = end + 1;
    }
    return "";
}

bool UseGnomeFormat() {
    if (const char* f = std::getenv("CRYPTVAULT_CLIPBOARD_FORMAT")) {
        if (std::strcmp(f, "uri-list") == 0) return false;
        if (std::strcmp(f, "gnome") == 0) return true;
    }
    const char* d = std::getenv("XDG_CURRENT_DESKTOP");
    std::string desktop = d ? d : "";
    for (char& c : desktop) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (desktop.find("KDE") != std::string::npos || desktop.find("LXQT") != std::string::npos) return false;
    return true;  // GNOME, Cinnamon, MATE, XFCE, ... all read x-special/gnome-copied-files
}

struct Tool {
    std::vector<std::string> copy_args;   // reads the payload on stdin and takes over the selection
    std::vector<std::string> paste_args;  // writes the payload to stdout
};

bool PickTool(bool gnome, Tool* tool) {
    const std::string mime = gnome ? "x-special/gnome-copied-files" : "text/uri-list";
    if (std::getenv("WAYLAND_DISPLAY")) {
        std::string copy = FindOnPath("wl-copy"), paste = FindOnPath("wl-paste");
        if (!copy.empty() && !paste.empty()) {
            tool->copy_args = {copy, "--type", mime};
            tool->paste_args = {paste, "--no-newline", "--type", mime};
            return true;
        }
    }
    if (std::getenv("DISPLAY")) {
        std::string xclip = FindOnPath("xclip");
        if (!xclip.empty()) {
            tool->copy_args = {xclip, "-selection", "clipboard", "-t", mime, "-i"};
            tool->paste_args = {xclip, "-selection", "clipboard", "-t", mime, "-o"};
            return true;
        }
    }
    return false;
}

// Runs argv (no shell). If `input` is given it is written to the child's
// stdin; if `output` is given the child's stdout is captured. Waits at most
// timeout_ms; returns the exit status, or -1 on spawn failure / timeout.
int Run(const std::vector<std::string>& args, const std::string* input, std::string* output, int timeout_ms) {
    // Only capture stdout when the caller wants it. wl-copy and xclip fork into the
    // background to keep serving the clipboard and that background process keeps
    // inherited descriptors open - so for "copy" mode stdout goes to /dev/null,
    // otherwise waiting for the pipe to close would never finish.
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    if (::pipe(in_pipe) != 0) return -1;
    if (output && ::pipe(out_pipe) != 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); return -1; }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
    if (output) posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    else posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) {
        if (fd >= 0) posix_spawn_file_actions_addclose(&actions, fd);
    }
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int rc = ::posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(in_pipe[0]);
    if (output) {
        ::close(out_pipe[1]);
        ::fcntl(out_pipe[0], F_SETFL, ::fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);  // never block on a pipe a background child may hold open
    }
    if (rc != 0) { ::close(in_pipe[1]); if (output) ::close(out_pipe[0]); return -1; }
    if (input) {
        size_t off = 0;
        while (off < input->size()) {
            ssize_t n = ::write(in_pipe[1], input->data() + off, input->size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
    }
    ::close(in_pipe[1]);
    std::string captured;
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (output) {
            struct pollfd p{out_pipe[0], POLLIN, 0};
            if (::poll(&p, 1, 20) > 0) {
                char buf[4096];
                ssize_t n = ::read(out_pipe[0], buf, sizeof buf);
                if (n > 0) captured.append(buf, static_cast<size_t>(n));
            }
        } else {
            ::usleep(20000);
        }
        int status = 0;
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (output) {   // drain what is left (non-blocking: stops when the pipe is empty)
                char buf[4096];
                ssize_t n;
                while ((n = ::read(out_pipe[0], buf, sizeof buf)) > 0) captured.append(buf, static_cast<size_t>(n));
                ::close(out_pipe[0]);
                *output = captured;
            }
            if (std::getenv("CRYPTVAULT_DEBUG") && !WIFEXITED(status)) {
                std::fprintf(stderr, "[cryptvault] %s ended by signal %d\n", args[0].c_str(), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            }
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }
    if (std::getenv("CRYPTVAULT_DEBUG")) std::fprintf(stderr, "[cryptvault] %s timed out after %d ms\n", args[0].c_str(), timeout_ms);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (output) ::close(out_pipe[0]);
    return -1;
}

}  // namespace

bool CopyFilesToSystemClipboard(const std::vector<std::string>& paths, bool cut, std::string* hint) {
    if (paths.empty()) return false;
    const bool gnome = UseGnomeFormat();
    Tool tool;
    if (!PickTool(gnome, &tool)) {
        if (hint) *hint = std::getenv("WAYLAND_DISPLAY") ? "INSTALL WL-CLIPBOARD TO COPY FILES TO OTHER APPS"
                                                         : "INSTALL XCLIP TO COPY FILES TO OTHER APPS";
        return false;
    }
    const std::string payload = BuildClipboardPayload(paths, cut, gnome);
    // wl-copy / xclip fork into the background to keep serving the selection; the
    // process we started exits straight away.
    return Run(tool.copy_args, &payload, nullptr, 2000) == 0;
}

bool ReadFilesFromSystemClipboard(std::vector<std::string>* paths, bool* cut) {
    Tool tool;
    for (bool gnome : {UseGnomeFormat(), !UseGnomeFormat()}) {   // try our preferred format, then the other
        if (!PickTool(gnome, &tool)) return false;
        std::string out;
        int status = Run(tool.paste_args, nullptr, &out, 1000);
        if (std::getenv("CRYPTVAULT_DEBUG")) {
            std::fprintf(stderr, "[cryptvault] clipboard read via %s: exit %d, %zu bytes\n", tool.paste_args[0].c_str(), status, out.size());
        }
        if (status != 0 || out.empty()) continue;
        std::vector<std::string> parsed = ParseClipboardPayload(out, cut);
        std::error_code ec;
        std::vector<std::string> existing;
        for (const std::string& p : parsed) {
            if (std::filesystem::exists(std::filesystem::symlink_status(p, ec))) existing.push_back(p);
        }
        if (!existing.empty()) {
            *paths = existing;
            return true;
        }
    }
    return false;
}

#endif

}  // namespace platform
