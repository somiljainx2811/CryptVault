#include "explorer/fs_ops.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include <filesystem>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cvfs {

namespace {

std::string HomeDir() {
    const char* home = std::getenv("HOME");
    return (home && *home) ? home : "/";
}

std::string Errno(const std::string& what, const std::error_code& ec) {
    return what + ": " + ec.message();
}

bool IsDirFollow(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool LexicallyInside(const fs::path& child, const fs::path& parent) {
    fs::path c = child.lexically_normal();
    fs::path p = parent.lexically_normal();
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (pi->empty()) continue;  // trailing slash element
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

std::string PercentEncodePath(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string PercentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ExtensionOf(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return LowerAscii(name.substr(dot + 1));
}

uint64_t DirSizeRecursive(const fs::path& p) {
    std::error_code ec;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && !it->is_symlink(e2)) total += it->file_size(e2);
    }
    return total;
}

// Fills size/mtime/mode for one entry. Follows symlinks for size/mtime
// (a link to a 2 GB file shows 2 GB), but falls back to the link itself
// when it's broken so a dangling symlink still lists.
void Stat(Entry& e) {
#ifndef _WIN32
    struct stat st {};
    if (::stat(e.path.c_str(), &st) != 0) {
        if (::lstat(e.path.c_str(), &st) != 0) return;
    }
    e.mtime = static_cast<int64_t>(st.st_mtime);
    e.mode = static_cast<uint32_t>(st.st_mode & 07777);
    e.size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
#else
    std::error_code ec;
    if (!e.is_dir) e.size = fs::file_size(e.path, ec);
    if (ec) e.size = 0;
#endif
}

}  // namespace

// --- listing ---------------------------------------------------------------

bool IsVaultDir(const std::string& path) {
    std::error_code ec;
    fs::path p(path);
    return fs::is_regular_file(p / "vault.meta", ec) && fs::is_regular_file(p / "manifest.enc", ec);
}

bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == '0') ++i2;
            while (j2 < b.size() && b[j2] == '0') ++j2;
            size_t ie = i2, je = j2;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            size_t la = ie - i2, lb = je - j2;
            if (la != lb) return la < lb;
            int c = a.compare(i2, la, b, j2, lb);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j) return (a.size() - i) < (b.size() - j);
    return a < b;  // stable tiebreak: "A" before "a"
}

bool ListDir(const std::string& path, const ListOptions& options, std::vector<Entry>& out,
             std::string& error) {
    out.clear();
    std::error_code ec;
    fs::path dir(path);
    if (!fs::is_directory(dir, ec)) {
        error = ec ? Errno(path, ec) : path + ": not a directory";
        return false;
    }
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error = Errno(path, ec);
        return false;
    }
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        Entry e;
        e.name = it->path().filename().string();
        e.is_hidden = !e.name.empty() && e.name[0] == '.';
        if (e.is_hidden && !options.show_hidden) continue;
        e.path = it->path().string();
        std::error_code e2;
        e.is_symlink = it->is_symlink(e2);
        e.is_dir = IsDirFollow(it->path());
        e.is_vault = e.is_dir && IsVaultDir(e.path);
        Stat(e);
        out.push_back(std::move(e));
    }

    auto cmp = [&](const Entry& x, const Entry& y) {
        if (options.dirs_first && x.is_dir != y.is_dir) return x.is_dir;  // never reversed
        bool less = false;
        switch (options.sort) {
            case SortKey::Name:
                less = NaturalLess(x.name, y.name);
                break;
            case SortKey::Size:
                if (x.size != y.size) { less = x.size < y.size; break; }
                less = NaturalLess(x.name, y.name);
                break;
            case SortKey::Modified:
                if (x.mtime != y.mtime) { less = x.mtime < y.mtime; break; }
                less = NaturalLess(x.name, y.name);
                break;
            case SortKey::Type: {
                std::string ex = ExtensionOf(x.name), ey = ExtensionOf(y.name);
                if (ex != ey) { less = ex < ey; break; }
                less = NaturalLess(x.name, y.name);
                break;
            }
        }
        return options.reverse ? (!less && (x.name != y.name)) : less;
    };
    std::stable_sort(out.begin(), out.end(), cmp);
    return true;
}

bool Search(const std::string& root, const std::string& query, bool show_hidden, size_t max_results,
            std::vector<Entry>& out, bool& truncated, std::string& error) {
    out.clear();
    truncated = false;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        error = root + ": not a directory";
        return false;
    }
    std::string needle = LowerAscii(query);
    constexpr size_t kMaxVisited = 150000;
    size_t visited = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        error = Errno(root, ec);
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (++visited > kMaxVisited) { truncated = true; break; }
        std::string name = it->path().filename().string();
        bool hidden = !name.empty() && name[0] == '.';
        std::error_code e2;
        bool is_dir = it->is_directory(e2);
        bool is_link = it->is_symlink(e2);
        if (hidden && !show_hidden) {
            if (is_dir) it.disable_recursion_pending();
            continue;
        }
        bool vault = is_dir && IsVaultDir(it->path().string());
        if (is_link || vault) it.disable_recursion_pending();  // never follow links, never enter vaults
        if (needle.empty() || LowerAscii(name).find(needle) != std::string::npos) {
            Entry e;
            e.path = it->path().string();
            e.name = fs::relative(it->path(), root, e2).string();
            e.is_hidden = hidden;
            e.is_symlink = is_link;
            e.is_dir = is_dir;
            e.is_vault = vault;
            Stat(e);
            out.push_back(std::move(e));
            if (out.size() >= max_results) { truncated = true; break; }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return NaturalLess(a.name, b.name); });
    return true;
}

// --- paths -----------------------------------------------------------------

std::string ExpandPath(const std::string& input) {
    std::string s = input;
    if (s.empty() || s == "~") {
        s = HomeDir();
    } else if (s.rfind("~/", 0) == 0) {
        s = HomeDir() + s.substr(1);
    }
    std::error_code ec;
    fs::path p(s);
    if (p.is_relative()) {
        p = fs::current_path(ec) / p;
    }
    p = p.lexically_normal();
    std::string out = p.string();
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::string ParentPath(const std::string& path) {
    fs::path p = fs::path(path).lexically_normal();
    std::string s = p.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s == "/" || s.empty()) return "/";
    fs::path parent = fs::path(s).parent_path();
    return parent.empty() ? "/" : parent.string();
}

std::vector<std::string> PathSegments(const std::string& path) {
    std::vector<std::string> segs;
    fs::path p = fs::path(path).lexically_normal();
    for (const auto& part : p) {
        std::string s = part.string();
        if (s.empty()) continue;
        segs.push_back(s);
    }
    return segs;
}

// --- formatting ------------------------------------------------------------

std::string HumanSize(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0) {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof buf, v < 10.0 ? "%.1f %s" : "%.0f %s", v, kUnits[u]);
    }
    return buf;
}

std::string FormatMode(uint32_t mode, bool is_dir, bool is_link) {
    std::string s(10, '-');
    s[0] = is_link ? 'l' : (is_dir ? 'd' : '-');
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        if (mode & (1u << (8 - i))) s[1 + i] = rwx[i];
    }
    if (mode & 04000) s[3] = (s[3] == 'x') ? 's' : 'S';
    if (mode & 02000) s[6] = (s[6] == 'x') ? 's' : 'S';
    if (mode & 01000) s[9] = (s[9] == 'x') ? 't' : 'T';
    return s;
}

std::string FormatTime(int64_t unix_seconds) {
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tmv {};
#ifndef _WIN32
    localtime_r(&t, &tmv);
#else
    localtime_s(&tmv, &t);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

// --- places ----------------------------------------------------------------

namespace {

std::string DecodeMountEscapes(const std::string& s) {
    // /proc/self/mountinfo encodes space/tab/newline/backslash as \040 \011 \012 \134.
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() + 0 && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 3), nullptr, 8));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

void FillSpace(Place& p) {
#ifndef _WIN32
    struct statvfs sv {};
    if (::statvfs(p.path.c_str(), &sv) == 0) {
        p.total_bytes = static_cast<uint64_t>(sv.f_blocks) * sv.f_frsize;
        p.free_bytes = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
    }
#else
    (void)p;
#endif
}

std::string XdgConfigHome() {
    const char* x = std::getenv("XDG_CONFIG_HOME");
    return (x && *x) ? x : HomeDir() + "/.config";
}

// Reads ~/.config/user-dirs.dirs ("XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"").
std::vector<std::pair<std::string, std::string>> XdgUserDirs() {
    static const std::pair<const char*, const char*> kKnown[] = {
        {"XDG_DESKTOP_DIR", "Desktop"},     {"XDG_DOCUMENTS_DIR", "Documents"},
        {"XDG_DOWNLOAD_DIR", "Downloads"},  {"XDG_MUSIC_DIR", "Music"},
        {"XDG_PICTURES_DIR", "Pictures"},   {"XDG_VIDEOS_DIR", "Videos"},
    };
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::pair<std::string, std::string>> configured;
    std::ifstream f(XdgConfigHome() + "/user-dirs.dirs");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (val.rfind("$HOME", 0) == 0) val = HomeDir() + val.substr(5);
        configured.emplace_back(key, val);
    }
    for (const auto& k : kKnown) {
        std::string path = HomeDir() + "/" + k.second;
        for (const auto& c : configured) {
            if (c.first == k.first) path = c.second;
        }
        out.emplace_back(k.second, path);
    }
    return out;
}

}  // namespace

std::vector<Place> Places() {
    std::vector<Place> places;
    std::string home = HomeDir();
    places.push_back({"Home", home, "home"});
    for (const auto& d : XdgUserDirs()) {
        std::error_code ec;
        if (d.second != home && fs::is_directory(d.second, ec)) {
            places.push_back({d.first, d.second, "xdg"});
        }
    }
    places.push_back({"Trash", TrashDir() + "/files", "trash"});

    Place root{"File System", "/", "root"};
    FillSpace(root);
    places.push_back(root);

#ifndef _WIN32
    static const std::set<std::string> kPseudo = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2", "securityfs", "debugfs",
        "tracefs", "configfs", "fusectl", "mqueue", "hugetlbfs", "pstore", "bpf", "autofs",
        "binfmt_misc", "selinuxfs", "efivarfs", "rpc_pipefs", "nsfs", "ramfs", "squashfs",
        "fuse.portal", "fuse.gvfsd-fuse", "fuse.gvfs-fuse-daemon", "overlay"};
    std::ifstream mi("/proc/self/mountinfo");
    std::string line;
    std::set<std::string> seen = {"/"};
    while (std::getline(mi, line)) {
        std::istringstream ss(line);
        std::vector<std::string> f;
        std::string tok;
        while (ss >> tok) f.push_back(tok);
        size_t dash = 0;
        for (size_t i = 6; i < f.size(); ++i) {
            if (f[i] == "-") { dash = i; break; }
        }
        if (dash == 0 || dash + 1 >= f.size() || f.size() < 5) continue;
        std::string mp = DecodeMountEscapes(f[4]);
        std::string fstype = f[dash + 1];
        if (kPseudo.count(fstype) || seen.count(mp)) continue;
        bool user_visible = mp.rfind("/run/media/", 0) == 0 || mp.rfind("/media/", 0) == 0
                            || mp.rfind("/mnt/", 0) == 0 || mp == "/home" || mp == "/boot";
        if (!user_visible) continue;
        seen.insert(mp);
        Place p;
        p.path = mp;
        p.kind = "mount";
        p.name = (mp == "/home" || mp == "/boot") ? mp.substr(1) : fs::path(mp).filename().string();
        FillSpace(p);
        places.push_back(std::move(p));
    }
#endif
    return places;
}

// --- operations ------------------------------------------------------------

bool MakeDir(const std::string& path, std::string& error) {
    std::error_code ec;
    if (fs::exists(fs::symlink_status(path, ec))) {
        error = path + ": already exists";
        return false;
    }
    if (!fs::create_directory(path, ec) || ec) {
        error = Errno(path, ec ? ec : std::make_error_code(std::errc::io_error));
        return false;
    }
    return true;
}

bool MakeFile(const std::string& path, std::string& error) {
#ifndef _WIN32
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    ::close(fd);
    return true;
#else
    std::error_code ec;
    if (fs::exists(path, ec)) { error = path + ": already exists"; return false; }
    std::ofstream f(path);
    if (!f) { error = path + ": cannot create"; return false; }
    return true;
#endif
}

bool Rename(const std::string& path, const std::string& new_name, std::string& error) {
    if (new_name.empty() || new_name == "." || new_name == ".." || new_name.find('/') != std::string::npos) {
        error = "invalid name";
        return false;
    }
    fs::path from(path);
    fs::path to = from.parent_path() / new_name;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(to, ec))) {
        error = to.string() + ": already exists";
        return false;
    }
    fs::rename(from, to, ec);
    if (ec) {
        error = Errno(path, ec);
        return false;
    }
    return true;
}

bool Copy(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::path src(from), dst(to);
    if (!fs::exists(fs::symlink_status(src, ec))) {
        error = from + ": no such file or directory";
        return false;
    }
    if (fs::exists(fs::symlink_status(dst, ec))) {
        error = to + ": already exists";
        return false;
    }
    if (IsDirFollow(src) && !fs::is_symlink(src, ec) && LexicallyInside(dst, src)) {
        error = "cannot copy a folder into itself";
        return false;
    }
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        error = Errno(from, ec);
        std::error_code ignore;
        if (fs::exists(fs::symlink_status(dst, ignore))) fs::remove_all(dst, ignore);  // no half-copies
        return false;
    }
    return true;
}

bool DeletePermanent(const std::string& path, std::string& error) {
    std::error_code ec;
    fs::path p = fs::path(path).lexically_normal();
    std::string s = p.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s == "/" || s == HomeDir()) {
        error = "refusing to delete " + s;
        return false;
    }
    if (!fs::exists(fs::symlink_status(p, ec))) {
        error = path + ": no such file or directory";
        return false;
    }
    fs::remove_all(p, ec);
    if (ec) {
        error = Errno(path, ec);
        return false;
    }
    return true;
}

bool Move(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::path src(from), dst(to);
    if (!fs::exists(fs::symlink_status(src, ec))) {
        error = from + ": no such file or directory";
        return false;
    }
    if (fs::exists(fs::symlink_status(dst, ec))) {
        error = to + ": already exists";
        return false;
    }
    if (IsDirFollow(src) && !fs::is_symlink(src, ec) && LexicallyInside(dst, src)) {
        error = "cannot move a folder into itself";
        return false;
    }
    fs::rename(src, dst, ec);
    if (!ec) return true;
    // Different filesystem (EXDEV) or similar: copy, then remove the
    // original only once the copy fully succeeded.
    if (ec != std::make_error_code(std::errc::cross_device_link)) {
        error = Errno(from, ec);
        return false;
    }
    if (!Copy(from, to, error)) return false;
    fs::remove_all(src, ec);
    if (ec) {
        error = "copied, but could not remove the original: " + ec.message();
        return false;
    }
    return true;
}

std::string UniqueName(const std::string& dir, const std::string& name) {
    std::error_code ec;
    auto taken = [&](const std::string& n) { return fs::exists(fs::symlink_status(fs::path(dir) / n, ec)); };
    if (!taken(name)) return name;
    std::string stem = name, ext;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int i = 1;; ++i) {
        std::string candidate = stem + (i == 1 ? " (copy)" : " (copy " + std::to_string(i) + ")") + ext;
        if (!taken(candidate)) return candidate;
    }
}

// --- trash -----------------------------------------------------------------

bool StatEntry(const std::string& path, Entry& e) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(path, ec))) return false;
    e = Entry{};
    e.path = path;
    e.name = fs::path(path).filename().string();
    e.is_hidden = !e.name.empty() && e.name[0] == '.';
    e.is_symlink = fs::is_symlink(path, ec);
    e.is_dir = IsDirFollow(path);
    e.is_vault = e.is_dir && IsVaultDir(path);
    Stat(e);
    return true;
}

std::string TrashDir() {
    const char* x = std::getenv("XDG_DATA_HOME");
    std::string base = (x && *x) ? std::string(x) : HomeDir() + "/.local/share";
    return base + "/Trash";
}

namespace {

struct TrashRoot {
    std::string root;    // the Trash directory (contains files/ and info/)
    std::string topdir;  // volume mount point ("" for the home trash)
};

unsigned long Uid() {
#ifndef _WIN32
    return static_cast<unsigned long>(::getuid());
#else
    return 0;
#endif
}

uint64_t DevOf(const std::string& path) {
#ifndef _WIN32
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_dev) : 0;
#else
    (void)path;
    return 1;
#endif
}

// Highest ancestor of `dir` still on the same filesystem = the volume's
// mount point (what the spec calls $topdir).
std::string TopdirOf(const fs::path& dir) {
    fs::path cur = dir;
    const uint64_t dev = DevOf(cur.string());
    while (cur.has_parent_path() && cur != cur.root_path()) {
        fs::path parent = cur.parent_path();
        if (DevOf(parent.string()) != dev) break;
        cur = parent;
    }
    return cur.string();
}

// Existing trash directory of a volume ("" if it has none): $topdir/.Trash/$uid
// when the shared .Trash is a real sticky directory (spec rule), else
// $topdir/.Trash-$uid.
std::string VolumeTrashPath(const std::string& topdir) {
#ifndef _WIN32
    const std::string uid = std::to_string(Uid());
    struct stat st {};
    std::string shared = topdir + "/.Trash";
    if (::lstat(shared.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && (st.st_mode & S_ISVTX)) {
        return shared + "/" + uid;
    }
    return topdir + "/.Trash-" + uid;
#else
    (void)topdir;
    return "";
#endif
}

bool MakeTrashDirs(const std::string& root, std::string& error) {
    std::error_code ec;
    for (const char* sub : {"files", "info"}) {
        fs::create_directories(root + "/" + sub, ec);
        if (ec) {
            error = Errno(root, ec);
            return false;
        }
    }
#ifndef _WIN32
    ::chmod(root.c_str(), 0700);
#endif
    return true;
}

bool EnsureTrashDirs(std::string& error) { return MakeTrashDirs(TrashDir(), error); }

// Every trash that exists right now: the home trash plus each mounted
// volume's (found via the mount table).
std::vector<TrashRoot> AllTrashRoots() {
    std::vector<TrashRoot> roots;
    std::set<std::string> seen;
    std::error_code ec;
    if (fs::is_directory(TrashDir(), ec)) {
        roots.push_back({TrashDir(), ""});
        seen.insert(TrashDir());
    }
#ifndef _WIN32
    static const std::set<std::string> kPseudo = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2", "securityfs", "debugfs", "tracefs",
        "configfs", "fusectl", "mqueue", "hugetlbfs", "pstore", "bpf", "autofs", "binfmt_misc", "selinuxfs",
        "efivarfs", "rpc_pipefs", "nsfs", "ramfs", "fuse.portal", "fuse.gvfsd-fuse", "overlay"};
    std::ifstream mi("/proc/self/mountinfo");
    std::string line;
    while (std::getline(mi, line)) {
        std::istringstream ss(line);
        std::vector<std::string> f;
        std::string tok;
        while (ss >> tok) f.push_back(tok);
        size_t dash = 0;
        for (size_t i = 6; i < f.size(); ++i) {
            if (f[i] == "-") { dash = i; break; }
        }
        if (dash == 0 || dash + 1 >= f.size() || f.size() < 5) continue;
        std::string mp = DecodeMountEscapes(f[4]);
        if (kPseudo.count(f[dash + 1]) && f[dash + 1] != "tmpfs") continue;  // tmpfs mounts can hold user trash too
        // Two candidate locations per spec; check both.
        std::vector<std::string> candidates = {mp + "/.Trash-" + std::to_string(Uid()),
                                               mp + "/.Trash/" + std::to_string(Uid())};
        for (const std::string& c : candidates) {
            if (seen.count(c) || !fs::is_directory(c + "/info", ec)) continue;
            seen.insert(c);
            roots.push_back({c, mp});
        }
    }
#endif
    return roots;
}

std::string NowIso() {
    std::time_t t = std::time(nullptr);
    std::tm tmv {};
#ifndef _WIN32
    localtime_r(&t, &tmv);
#else
    localtime_s(&tmv, &t);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tmv);
    return buf;
}

}  // namespace

bool TrashPut(const std::string& path, std::string& error) {
    std::error_code ec;
    fs::path src = fs::path(ExpandPath(path));
    if (!fs::exists(fs::symlink_status(src, ec))) {
        error = path + ": no such file or directory";
        return false;
    }
    std::string s = src.string();
    if (s == "/" || s == HomeDir()) {
        error = "refusing to trash " + s;
        return false;
    }
    for (const TrashRoot& r : AllTrashRoots()) {
        if (LexicallyInside(src, r.root)) {
            error = "already in the trash";
            return false;
        }
    }
    if (!EnsureTrashDirs(error)) return false;

    // Which trash: same filesystem as the home trash -> home trash; otherwise
    // the volume's own trash (falling back to the home trash if it can't be made).
    TrashRoot target{TrashDir(), ""};
    const uint64_t src_dev = DevOf(src.parent_path().string());
    if (src_dev != 0 && src_dev != DevOf(TrashDir())) {
        std::string topdir = TopdirOf(src.parent_path());
        std::string vroot = VolumeTrashPath(topdir);
        std::string ignored;
        if (!vroot.empty() && MakeTrashDirs(vroot, ignored)) target = {vroot, topdir};
    }

    // Claim a unique name by exclusively creating its .trashinfo first
    // (the spec's race-free way to avoid two trashers picking the same name).
    std::string stored_path = target.topdir.empty() ? s : s.substr(target.topdir.size() + 1);  // volume trash: relative to topdir
    std::string base = src.filename().string();
    std::string trashed;
    std::string info_path;
    for (int i = 0;; ++i) {
        trashed = i == 0 ? base : base + "." + std::to_string(i);
        info_path = target.root + "/info/" + trashed + ".trashinfo";
        if (fs::exists(fs::symlink_status(target.root + "/files/" + trashed, ec))) continue;
#ifndef _WIN32
        int fd = ::open(info_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            error = info_path + ": " + std::strerror(errno);
            return false;
        }
        std::string body = "[Trash Info]\nPath=" + PercentEncodePath(stored_path) + "\nDeletionDate=" + NowIso() + "\n";
        bool ok = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
        ::close(fd);
        if (!ok) {
            ::unlink(info_path.c_str());
            error = "could not write " + info_path;
            return false;
        }
        break;
#else
        std::ofstream f(info_path);
        f << "[Trash Info]\nPath=" << PercentEncodePath(stored_path) << "\nDeletionDate=" << NowIso() << "\n";
        break;
#endif
    }
    if (!Move(s, target.root + "/files/" + trashed, error)) {
        std::error_code ignore;
        fs::remove(info_path, ignore);
        return false;
    }
    return true;
}

bool TrashList(std::vector<TrashItem>& out, std::string& error) {
    out.clear();
    std::error_code ec;
    for (const TrashRoot& r : AllTrashRoots()) {
        fs::path info_dir = r.root + "/info";
        if (!fs::is_directory(info_dir, ec)) continue;
        for (fs::directory_iterator it(info_dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".trashinfo") continue;
            TrashItem item;
            item.trashed_name = it->path().stem().string();
            item.trash_root = r.root;
            item.on_volume = !r.topdir.empty();
            std::ifstream f(it->path());
            std::string line, stored;
            while (std::getline(f, line)) {
                if (line.rfind("Path=", 0) == 0) stored = PercentDecode(line.substr(5));
                else if (line.rfind("DeletionDate=", 0) == 0) item.deletion_date = line.substr(13);
            }
            // Volume trashes store paths relative to the volume's mount point.
            item.original_path = (!stored.empty() && stored[0] != '/' && !r.topdir.empty())
                                     ? (r.topdir == "/" ? "/" + stored : r.topdir + "/" + stored) : stored;
            item.payload_path = r.root + "/files/" + item.trashed_name;
            std::error_code e2;
            item.is_dir = fs::is_directory(item.payload_path, e2) && !fs::is_symlink(item.payload_path, e2);
            item.size = item.is_dir ? DirSizeRecursive(item.payload_path)
                                    : (fs::is_regular_file(item.payload_path, e2) ? fs::file_size(item.payload_path, e2) : 0);
            out.push_back(std::move(item));
        }
        ec.clear();
    }
    (void)error;
    std::sort(out.begin(), out.end(), [](const TrashItem& a, const TrashItem& b) {
        return a.deletion_date != b.deletion_date ? a.deletion_date > b.deletion_date : a.payload_path < b.payload_path;
    });
    return true;
}

namespace {

// Resolves a handle against the real trash contents (never trusts it as a path).
bool FindTrashItem(const std::string& handle, TrashItem& found, std::string& error) {
    if (handle.empty()) {
        error = "invalid trash item name";
        return false;
    }
    const bool by_path = handle[0] == '/';
    if (!by_path && (handle.find('/') != std::string::npos || handle == "." || handle == "..")) {
        error = "invalid trash item name";
        return false;
    }
    std::vector<TrashItem> items;
    TrashList(items, error);
    for (const TrashItem& it : items) {
        if (by_path ? it.payload_path == handle : (!it.on_volume && it.trashed_name == handle)) {
            found = it;
            return true;
        }
    }
    error = handle + ": not in the trash";
    return false;
}

}  // namespace

bool TrashRestore(const std::string& handle, std::string& error) {
    TrashItem item;
    if (!FindTrashItem(handle, item, error)) return false;
    if (item.original_path.empty() || item.original_path[0] != '/') {
        error = item.trashed_name + ": trash entry has no usable original path";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(item.original_path).parent_path(), ec);
    if (!Move(item.payload_path, item.original_path, error)) return false;
    fs::remove(item.trash_root + "/info/" + item.trashed_name + ".trashinfo", ec);
    return true;
}

bool TrashDelete(const std::string& handle, std::string& error) {
    TrashItem item;
    if (!FindTrashItem(handle, item, error)) return false;
    std::error_code ec;
    fs::remove_all(item.payload_path, ec);
    if (ec) {
        error = Errno(item.payload_path, ec);
        return false;
    }
    fs::remove(item.trash_root + "/info/" + item.trashed_name + ".trashinfo", ec);
    return true;
}

bool TrashEmpty(std::string& error) {
    std::error_code ec;
    for (const TrashRoot& r : AllTrashRoots()) {
        for (const char* sub : {"files", "info"}) {
            fs::path dir = r.root + "/" + sub;
            if (!fs::is_directory(dir, ec)) continue;
            std::vector<fs::path> children;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
            for (const auto& c : children) {
                fs::remove_all(c, ec);
                if (ec) {
                    error = Errno(c.string(), ec);
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace cvfs
