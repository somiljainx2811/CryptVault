#include "cli/cli.h"

#include <fcntl.h>
#include <sodium.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include "explorer/fs_ops.h"
#include "platform/open_file.h"
#include "vault/vault_store.h"

namespace fs = std::filesystem;

namespace cvcli {
namespace {

constexpr const char* kVersion = "0.2.0";

// Exit codes (documented in --help so scripts can branch on them).
constexpr int kOk = 0, kFail = 1, kUsage = 2, kUnlockFailed = 3;

// --- tiny output helpers -----------------------------------------------------

bool UseColor() {
    static const bool use = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
    return use;
}
std::string Paint(const std::string& s, const char* code) {
    return UseColor() ? std::string("\x1b[") + code + "m" + s + "\x1b[0m" : s;
}
int Err(const std::string& msg, int code = kFail) {
    std::fprintf(stderr, "cryptvault: %s\n", msg.c_str());
    return code;
}
int Usage(const std::string& msg) {
    std::fprintf(stderr, "cryptvault: %s\nTry 'cryptvault --help'.\n", msg.c_str());
    return kUsage;
}

const char* kHelp = R"CVEOF(CryptVault %s - a native file explorer with encrypted vaults

USAGE
  cryptvault [PATH]                  open the explorer window at PATH (default: ~)
  cryptvault gui [--vaults] [PATH]   same, optionally starting on the vault side
  cryptvault COMMAND ...             run in the terminal, no window

FILES (unrestricted side)
  ls [-a] [-l] [-S|-t|-X] [-r] [PATH...]   list directories; vaults are marked
  info PATH                                details for one file/folder
  places                                   home, XDG folders, mounted volumes
  find QUERY [DIR] [-a]                    search file names under DIR (default .), case-insensitive
  open PATH                                open with the desktop default app
  trash put PATH...                        move to the freedesktop Trash
  trash list | restore NAME | empty        manage the Trash (all drives; NAME as shown by list)

VAULTS (restricted side)  - NAME is a registered vault name or a vault directory
  vault list                               registered vaults
  vault create NAME [--path DIR]           new vault (default DIR: ~/CryptVaultData/NAME)
  vault register NAME DIR | forget NAME    add/remove from the list (never deletes data)
  vault ls   NAME[:/dir]                   list inside a vault
  vault tree NAME[:/dir]                   whole tree
  vault add  NAME[:/dir] PATH... [--delete-originals]
                                           encrypt files/folders into the vault
  vault put  NAME:/dir/file                encrypt stdin as that file
  vault get  NAME:/dir/file [-o OUT]       decrypt to stdout (or OUT, mode 0600)
  vault mkdir NAME:/dir | rm NAME:/path | mv NAME:/path NEWNAME
  vault passwd NAME                        change the password

PASSWORDS  read from the terminal without echo, or non-interactively from
  --password-fd N | --password-file FILE | $CRYPTVAULT_PASSWORD (least safe:
  visible to your process environment).

ENVIRONMENT  CRYPTVAULT_PLATFORM=x11|wayland   force the window's display backend
             NO_COLOR                           disable colored output

OTHER   completion bash|zsh     print a shell completion script
        --version, --help

EXIT CODES  0 ok | 1 error | 2 usage | 3 wrong password or unreadable vault

Files in vaults are decrypted only in memory; nothing stays unlocked between
commands. 'vault get -o' and 'cryptvault open' on decrypted data write
plaintext to disk - that is your call, not the vault's.
)CVEOF";

// --- passwords -----------------------------------------------------------------

struct PasswordSource {
    int fd = -1;
    std::string file;
};

bool ReadLineFromFd(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) return !out.empty() || n == 0 ? true : false;
        if (c == '\n') return true;
        if (c != '\r') out.push_back(c);
    }
}

// Prompts on /dev/tty (so it works even when stdin/stdout are piped).
bool PromptPassword(const std::string& prompt, std::string& out) {
    int tty = ::open("/dev/tty", O_RDWR);
    if (tty < 0) return false;
    struct termios old {}, noecho {};
    bool have_term = ::tcgetattr(tty, &old) == 0;
    ssize_t ignored = ::write(tty, prompt.data(), prompt.size());
    (void)ignored;
    if (have_term) {
        noecho = old;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(tty, TCSAFLUSH, &noecho);
    }
    bool ok = ReadLineFromFd(tty, out);
    if (have_term) ::tcsetattr(tty, TCSAFLUSH, &old);
    ignored = ::write(tty, "\n", 1);
    ::close(tty);
    return ok;
}

// `prompt` is shown only for the interactive path. Non-interactive
// sources are consumed in order: fd, file, env.
bool GetPassword(const PasswordSource& src, const std::string& prompt, std::string& out, std::string& error) {
    if (src.fd >= 0) {
        if (!ReadLineFromFd(src.fd, out)) { error = "could not read password from fd"; return false; }
        return true;
    }
    if (!src.file.empty()) {
        std::ifstream f(src.file);
        if (!f || !std::getline(f, out)) { error = "could not read password file " + src.file; return false; }
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }
    if (const char* env = std::getenv("CRYPTVAULT_PASSWORD")) {
        out = env;
        return true;
    }
    if (PromptPassword(prompt, out)) return true;
    error = "no terminal to prompt on; use --password-fd, --password-file or CRYPTVAULT_PASSWORD";
    return false;
}

struct Scrub {  // wipes a password string on scope exit
    std::string& s;
    ~Scrub() { if (!s.empty()) sodium_memzero(s.data(), s.size()); }
};

// --- vault registry (same file + format the GUI uses) -------------------------

fs::path RegistryPath() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".cryptvault" / "known_vaults.txt";
}

std::vector<std::pair<std::string, std::string>> LoadRegistry() {
    std::vector<std::pair<std::string, std::string>> v;
    std::ifstream f(RegistryPath());
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string name = line.substr(0, tab), path = line.substr(tab + 1);
        if (!name.empty() && !path.empty()) v.emplace_back(name, path);
    }
    return v;
}

bool SaveRegistry(const std::vector<std::pair<std::string, std::string>>& v, std::string& error) {
    std::error_code ec;
    fs::create_directories(RegistryPath().parent_path(), ec);
    std::string tmp = RegistryPath().string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { error = "cannot write " + tmp; return false; }
        for (const auto& e : v) f << e.first << '\t' << e.second << '\n';
    }
    fs::rename(tmp, RegistryPath(), ec);  // atomic replace: a crash can't leave a half-written list
    if (ec) { error = "cannot update " + RegistryPath().string() + ": " + ec.message(); return false; }
    return true;
}

bool ValidVaultName(const std::string& n) {
    return !n.empty() && n.find_first_of("\t\n/:") == std::string::npos && n != "." && n != "..";
}

// Resolves "NAME", "/some/dir" or "NAME:/sub/path" (also "/some/dir:/sub").
struct VaultRef {
    std::string display_name;
    std::string dir;
    std::string inner;  // path inside the vault, "" = root
};

bool ResolveVaultRef(const std::string& arg, VaultRef& out, std::string& error) {
    auto registry = LoadRegistry();
    auto lookup = [&](const std::string& s, VaultRef& r) {
        for (const auto& e : registry) {
            if (e.first == s) { r.display_name = e.first; r.dir = e.second; return true; }
        }
        std::string p = cvfs::ExpandPath(s);
        if (cvfs::IsVaultDir(p)) { r.display_name = fs::path(p).filename().string(); r.dir = p; return true; }
        return false;
    };
    VaultRef r;
    if (lookup(arg, r)) { out = r; return true; }
    size_t colon = arg.find(':');
    if (colon != std::string::npos && lookup(arg.substr(0, colon), r)) {
        r.inner = arg.substr(colon + 1);
        out = r;
        return true;
    }
    error = "no vault named or located at '" + (colon == std::string::npos ? arg : arg.substr(0, colon))
            + "' (see 'cryptvault vault list')";
    return false;
}

std::vector<std::string> SplitInner(const std::string& inner, bool& ok) {
    ok = true;
    std::vector<std::string> parts;
    std::stringstream ss(inner);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { ok = false; return parts; }
        parts.push_back(seg);
    }
    return parts;
}

vaultstore::TreeNode* FindChild(vaultstore::TreeNode& dir, const std::string& name) {
    for (auto& c : dir.children) if (c.name == name) return &c;
    return nullptr;
}

// Walks `parts` from the vault root. Null if any segment is missing or
// a non-final segment isn't a folder.
vaultstore::TreeNode* Walk(vaultstore::Vault& v, const std::vector<std::string>& parts) {
    vaultstore::TreeNode* cur = &v.root();
    for (const auto& p : parts) {
        if (!cur->is_folder) return nullptr;
        cur = FindChild(*cur, p);
        if (!cur) return nullptr;
    }
    return cur;
}

std::unique_ptr<vaultstore::Vault> Unlock(const VaultRef& ref, const PasswordSource& pw, int& code) {
    std::string password, error;
    Scrub scrub{password};
    if (!GetPassword(pw, "Password for vault '" + ref.display_name + "': ", password, error)) {
        code = Err(error);
        return nullptr;
    }
    auto vault = vaultstore::OpenVault(ref.dir, password, error);
    if (!vault) {
        // Wrong password and corruption are deliberately indistinguishable (see vault_store.h).
        code = Err("could not unlock '" + ref.display_name + "': " + error, kUnlockFailed);
        return nullptr;
    }
    return vault;
}

// --- argument plumbing -----------------------------------------------------------

struct Args {
    std::vector<std::string> pos;
    std::set<std::string> flags;                  // bare flags, e.g. "-a"
    std::map<std::string, std::string> opts;      // valued, e.g. "--path" -> DIR
    PasswordSource pw;
    std::string bad;                              // first unknown option, if any
};

// `valued`: options that take an argument. `known`: all accepted flags.
Args Parse(const std::vector<std::string>& in, const std::set<std::string>& known,
           const std::set<std::string>& valued) {
    Args a;
    bool dashdash = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string& s = in[i];
        if (dashdash || s.empty() || s[0] != '-' || s == "-") { a.pos.push_back(s); continue; }
        if (s == "--") { dashdash = true; continue; }
        std::string key = s, val;
        bool has_val = false;
        size_t eq = s.find('=');
        if (s.rfind("--", 0) == 0 && eq != std::string::npos) { key = s.substr(0, eq); val = s.substr(eq + 1); has_val = true; }
        if (key == "--password-fd" || key == "--password-file") {
            if (!has_val) { if (i + 1 >= in.size()) { a.bad = key + " needs a value"; return a; } val = in[++i]; }
            if (key == "--password-fd") a.pw.fd = std::atoi(val.c_str()); else a.pw.file = val;
            continue;
        }
        if (valued.count(key)) {
            if (!has_val) { if (i + 1 >= in.size()) { a.bad = key + " needs a value"; return a; } val = in[++i]; }
            a.opts[key] = val;
            continue;
        }
        if (known.count(key)) { a.flags.insert(key); continue; }
        // Clustered short flags: -la
        if (s.size() > 2 && s[1] != '-') {
            bool all = true;
            for (size_t k = 1; k < s.size(); ++k) if (!known.count(std::string("-") + s[k])) all = false;
            if (all) { for (size_t k = 1; k < s.size(); ++k) a.flags.insert(std::string("-") + s[k]); continue; }
        }
        a.bad = s;
        return a;
    }
    return a;
}

#define PARSE(var, known, valued)                                              \
    Args var = Parse(rest, known, valued);                                     \
    if (!var.bad.empty()) return Usage("unknown or incomplete option: " + var.bad)

// --- files side ----------------------------------------------------------------

std::string ColorName(const cvfs::Entry& e) {
    if (e.is_vault) return Paint(e.name, "1;35");
    if (e.is_dir) return Paint(e.name, "1;34");
    if (e.is_symlink) return Paint(e.name, "36");
    if (e.mode & 0111) return Paint(e.name, "32");
    return e.name;
}

int CmdLs(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{"-a", "-l", "-S", "-t", "-X", "-r", "-h"}), {});
    cvfs::ListOptions lo;
    lo.show_hidden = a.flags.count("-a");
    lo.reverse = a.flags.count("-r");
    if (a.flags.count("-S")) lo.sort = cvfs::SortKey::Size;
    else if (a.flags.count("-t")) lo.sort = cvfs::SortKey::Modified;
    else if (a.flags.count("-X")) lo.sort = cvfs::SortKey::Type;
    bool longfmt = a.flags.count("-l");
    std::vector<std::string> targets = a.pos.empty() ? std::vector<std::string>{"."} : a.pos;
    int rc = kOk;
    for (size_t t = 0; t < targets.size(); ++t) {
        std::string path = cvfs::ExpandPath(targets[t]);
        std::error_code ec;
        if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {  // `ls file` prints the file
            targets[t] = path;
            std::printf("%s\n", path.c_str());
            continue;
        }
        std::vector<cvfs::Entry> es;
        std::string error;
        if (!cvfs::ListDir(path, lo, es, error)) { rc = Err(error); continue; }
        if (targets.size() > 1) std::printf("%s%s:\n", t ? "\n" : "", targets[t].c_str());
        for (const auto& e : es) {
            if (longfmt) {
                std::printf("%s %8s %s %s%s%s\n", cvfs::FormatMode(e.mode, e.is_dir, e.is_symlink).c_str(),
                            e.is_dir ? "-" : cvfs::HumanSize(e.size).c_str(), cvfs::FormatTime(e.mtime).c_str(),
                            ColorName(e).c_str(), e.is_dir ? "/" : "", e.is_vault ? "  [vault]" : "");
            } else {
                std::printf("%s%s\n", ColorName(e).c_str(), e.is_dir ? "/" : "");
            }
        }
    }
    return rc;
}

int CmdFind(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{"-a"}), {});
    if (a.pos.empty() || a.pos.size() > 2) return Usage("find needs QUERY and optionally a DIR");
    std::string root = cvfs::ExpandPath(a.pos.size() > 1 ? a.pos[1] : ".");
    std::vector<cvfs::Entry> found;
    bool truncated = false;
    std::string error;
    if (!cvfs::Search(root, a.pos[0], a.flags.count("-a") > 0, 5000, found, truncated, error)) return Err(error);
    for (const auto& e : found) std::printf("%s%s\n", e.path.c_str(), e.is_dir ? "/" : "");
    if (truncated) std::fprintf(stderr, "cryptvault: results truncated (narrow the query or the folder)\n");
    return found.empty() ? kFail : kOk;   // like grep: 1 when nothing matched
}

int CmdInfo(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), {});
    if (a.pos.size() != 1) return Usage("info takes exactly one PATH");
    std::string path = cvfs::ExpandPath(a.pos[0]);
    std::error_code ec;
    fs::file_status ls = fs::symlink_status(path, ec);
    if (!fs::exists(ls)) return Err(path + ": no such file or directory");
    std::vector<cvfs::Entry> parent;
    std::string error;
    cvfs::ListOptions lo; lo.show_hidden = true;
    cvfs::Entry self;
    if (cvfs::ListDir(cvfs::ParentPath(path), lo, parent, error)) {
        for (auto& e : parent) if (e.path == path) self = e;
    }
    if (self.path.empty()) {  // "/" or unreadable parent
        self.path = path; self.name = path; self.is_dir = fs::is_directory(path, ec); self.is_vault = self.is_dir && cvfs::IsVaultDir(path);
    }
    std::printf("Name:      %s\nPath:      %s\nType:      %s%s\n", self.name.c_str(), self.path.c_str(),
                self.is_vault ? "CryptVault vault (locked - use 'cryptvault vault ...')" : (self.is_dir ? "directory" : "file"),
                self.is_symlink ? " (symlink)" : "");
    if (self.is_symlink) std::printf("Target:    %s\n", fs::read_symlink(path, ec).string().c_str());
    if (!self.is_dir) std::printf("Size:      %s (%llu bytes)\n", cvfs::HumanSize(self.size).c_str(), (unsigned long long)self.size);
    std::printf("Modified:  %s\nMode:      %s (%04o)\n", cvfs::FormatTime(self.mtime).c_str(),
                cvfs::FormatMode(self.mode, self.is_dir, self.is_symlink).c_str(), self.mode);
    if (self.is_dir && !self.is_vault) {
        std::vector<cvfs::Entry> kids;
        if (cvfs::ListDir(path, lo, kids, error)) std::printf("Entries:   %zu\n", kids.size());
    }
    return kOk;
}

int CmdPlaces() {
    for (const auto& p : cvfs::Places()) {
        std::string extra = p.total_bytes ? "  (" + cvfs::HumanSize(p.free_bytes) + " free of " + cvfs::HumanSize(p.total_bytes) + ")" : "";
        std::printf("%-14s %s%s\n", p.name.c_str(), p.path.c_str(), extra.c_str());
    }
    return kOk;
}

int CmdOpen(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), {});
    if (a.pos.size() != 1) return Usage("open takes exactly one PATH");
    std::string path = cvfs::ExpandPath(a.pos[0]);
    if (cvfs::IsVaultDir(path)) return Err("that is a vault; use 'cryptvault gui --vaults' or 'cryptvault vault ls'");
    std::string error;
    return platform::OpenFileWithDefaultApp(path, &error) ? kOk : Err(error);
}

int CmdTrash(const std::vector<std::string>& rest) {
    if (rest.empty()) return Usage("trash needs: put | list | restore | empty");
    std::string sub = rest[0];
    std::vector<std::string> tail(rest.begin() + 1, rest.end());
    std::string error;
    if (sub == "put") {
        if (tail.empty()) return Usage("trash put needs at least one PATH");
        int rc = kOk;
        for (const auto& p : tail) if (!cvfs::TrashPut(p, error)) rc = Err(error);
        return rc;
    }
    if (sub == "list") {
        std::vector<cvfs::TrashItem> items;
        if (!cvfs::TrashList(items, error)) return Err(error);
        for (const auto& i : items) {
            // Items on other drives need their full trash path as the handle ("restore" accepts it).
            std::printf("%s  %8s  %-24s %s\n", i.deletion_date.c_str(), i.is_dir ? "dir" : cvfs::HumanSize(i.size).c_str(),
                        (i.on_volume ? i.payload_path : i.trashed_name).c_str(), i.original_path.c_str());
        }
        if (items.empty()) std::fprintf(stderr, "Trash is empty.\n");
        return kOk;
    }
    if (sub == "restore") {
        if (tail.size() != 1) return Usage("trash restore needs the NAME shown by 'trash list'");
        return cvfs::TrashRestore(tail[0], error) ? kOk : Err(error);
    }
    if (sub == "empty") return cvfs::TrashEmpty(error) ? kOk : Err(error);
    return Usage("unknown trash command '" + sub + "'");
}

// --- vault side ----------------------------------------------------------------

int CmdVaultList() {
    auto reg = LoadRegistry();
    if (reg.empty()) std::fprintf(stderr, "No vaults registered. Create one with: cryptvault vault create NAME\n");
    for (const auto& e : reg) {
        std::error_code ec;
        bool present = cvfs::IsVaultDir(e.second);
        std::printf("%-20s %s%s\n", e.first.c_str(), e.second.c_str(), present ? "" : "   (missing on disk)");
    }
    return kOk;
}

int CmdVaultCreate(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), (std::set<std::string>{"--path"}));
    if (a.pos.size() != 1) return Usage("vault create needs NAME");
    const std::string& name = a.pos[0];
    if (!ValidVaultName(name)) return Err("vault names can't be empty or contain / : or tabs");
    auto reg = LoadRegistry();
    for (const auto& e : reg) if (e.first == name) return Err("a vault named '" + name + "' is already registered");
    std::string dir;
    if (a.opts.count("--path")) dir = cvfs::ExpandPath(a.opts["--path"]);
    else { const char* h = std::getenv("HOME"); dir = std::string(h ? h : ".") + "/CryptVaultData/" + name; }

    std::string pw1, pw2, error;
    Scrub s1{pw1}, s2{pw2};
    if (!GetPassword(a.pw, "New password: ", pw1, error)) return Err(error);
    if (pw1.empty()) return Err("empty passwords are refused");
    if (a.pw.fd < 0 && a.pw.file.empty() && !std::getenv("CRYPTVAULT_PASSWORD")) {
        if (!GetPassword(a.pw, "Confirm password: ", pw2, error)) return Err(error);
        if (pw1 != pw2) return Err("passwords did not match");
    }
    std::error_code ec;
    fs::create_directories(fs::path(dir).parent_path(), ec);
    auto vault = vaultstore::CreateVault(dir, pw1, error);
    if (!vault) return Err("could not create vault: " + error);
    reg.emplace_back(name, dir);
    if (!SaveRegistry(reg, error)) return Err(error);
    std::printf("Created vault '%s' at %s\n", name.c_str(), dir.c_str());
    return kOk;
}

int CmdVaultRegister(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), {});
    if (a.pos.size() != 2) return Usage("vault register needs NAME DIR");
    if (!ValidVaultName(a.pos[0])) return Err("invalid vault name");
    std::string dir = cvfs::ExpandPath(a.pos[1]), error;
    if (!cvfs::IsVaultDir(dir)) return Err(dir + " doesn't look like a vault (no vault.meta/manifest.enc)");
    auto reg = LoadRegistry();
    for (const auto& e : reg) if (e.first == a.pos[0]) return Err("a vault named '" + a.pos[0] + "' is already registered");
    reg.emplace_back(a.pos[0], dir);
    return SaveRegistry(reg, error) ? kOk : Err(error);
}

int CmdVaultForget(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), {});
    if (a.pos.size() != 1) return Usage("vault forget needs NAME");
    auto reg = LoadRegistry();
    auto it = std::remove_if(reg.begin(), reg.end(), [&](const auto& e) { return e.first == a.pos[0]; });
    if (it == reg.end()) return Err("no vault named '" + a.pos[0] + "'");
    reg.erase(it, reg.end());
    std::string error;
    if (!SaveRegistry(reg, error)) return Err(error);
    std::printf("Removed '%s' from the list. The encrypted data on disk was NOT touched.\n", a.pos[0].c_str());
    return kOk;
}

// Common prologue: parse "NAME[:/inner]" + unlock.
struct Opened {
    VaultRef ref;
    std::unique_ptr<vaultstore::Vault> vault;
    std::vector<std::string> inner;
};
bool OpenRef(const std::string& arg, const PasswordSource& pw, Opened& o, int& rc) {
    std::string error;
    if (!ResolveVaultRef(arg, o.ref, error)) { rc = Err(error); return false; }
    bool ok;
    o.inner = SplitInner(o.ref.inner, ok);
    if (!ok) { rc = Err("'..' isn't allowed in vault paths"); return false; }
    o.vault = Unlock(o.ref, pw, rc);
    return o.vault != nullptr;
}

void PrintTree(const vaultstore::TreeNode& n, const std::string& indent) {
    for (const auto& c : n.children) {
        std::printf("%s%s%s\n", indent.c_str(), c.is_folder ? Paint(c.name, "1;34").c_str() : c.name.c_str(), c.is_folder ? "/" : "");
        if (c.is_folder) PrintTree(c, indent + "  ");
    }
}

int CmdVaultLs(const std::vector<std::string>& rest, bool tree) {
    PARSE(a, (std::set<std::string>{"-l"}), {});
    if (a.pos.size() != 1) return Usage(tree ? "vault tree needs NAME[:/dir]" : "vault ls needs NAME[:/dir]");
    Opened o; int rc = kOk;
    if (!OpenRef(a.pos[0], a.pw, o, rc)) return rc;
    vaultstore::TreeNode* dir = Walk(*o.vault, o.inner);
    if (!dir) return Err("no such path in vault: /" + o.ref.inner);
    if (!dir->is_folder) { std::printf("%s\n", dir->name.c_str()); return kOk; }
    if (tree) { PrintTree(*dir, ""); return kOk; }
    for (const auto& c : dir->children) {
        if (a.flags.count("-l")) std::printf("%c %8s %s%s\n", c.is_folder ? 'd' : '-', c.is_folder ? "-" : cvfs::HumanSize(c.size).c_str(), c.name.c_str(), c.is_folder ? "/" : "");
        else std::printf("%s%s\n", c.name.c_str(), c.is_folder ? "/" : "");
    }
    return kOk;
}

bool ReadWholeFile(const fs::path& p, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { error = p.string() + ": cannot open"; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (f.bad()) { error = p.string() + ": read error"; return false; }
    return true;
}

// Recursively encrypts `src` into `parent`. Symlinks are skipped (never
// followed - following one could silently pull in files from elsewhere).
int AddRecursive(vaultstore::Vault& v, vaultstore::TreeNode& parent, const fs::path& src, std::vector<fs::path>& added, int& count) {
    std::error_code ec;
    std::string error;
    std::string name = src.filename().string();
    if (fs::is_symlink(src, ec)) { std::fprintf(stderr, "skipping symlink: %s\n", src.c_str()); return kOk; }
    if (fs::is_directory(src, ec)) {
        vaultstore::TreeNode* folder = FindChild(parent, name);
        if (!folder) {
            if (!v.AddFolder(parent, name, error)) return Err(name + ": " + error);
            folder = FindChild(parent, name);
        }
        int rc = kOk;
        std::vector<fs::path> kids;
        for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) kids.push_back(it->path());
        std::sort(kids.begin(), kids.end());
        for (const auto& k : kids) { int r = AddRecursive(v, *folder, k, added, count); if (r != kOk) rc = r; }
        return rc;
    }
    if (!fs::is_regular_file(src, ec)) { std::fprintf(stderr, "skipping non-regular file: %s\n", src.c_str()); return kOk; }
    if (FindChild(parent, name)) return Err(name + ": already exists in the vault (not overwritten)");
    std::vector<uint8_t> data;
    if (!ReadWholeFile(src, data, error)) return Err(error);
    bool ok = v.AddFile(parent, name, data, error);
    if (!data.empty()) sodium_memzero(data.data(), data.size());
    if (!ok) return Err(name + ": " + error);
    added.push_back(src);
    ++count;
    return kOk;
}

int CmdVaultAdd(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{"--delete-originals"}), {});
    if (a.pos.size() < 2) return Usage("vault add needs NAME[:/dir] and at least one PATH");
    Opened o; int rc = kOk;
    if (!OpenRef(a.pos[0], a.pw, o, rc)) return rc;
    vaultstore::TreeNode* dir = Walk(*o.vault, o.inner);
    if (!dir || !dir->is_folder) return Err("no such folder in vault: /" + o.ref.inner);
    std::vector<fs::path> added;
    int count = 0;
    for (size_t i = 1; i < a.pos.size(); ++i) {
        fs::path src = cvfs::ExpandPath(a.pos[i]);
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(src, ec))) { rc = Err(a.pos[i] + ": no such file or directory"); continue; }
        int r = AddRecursive(*o.vault, *dir, src, added, count);
        if (r != kOk) rc = r;
    }
    std::printf("Encrypted %d file(s) into '%s'.\n", count, o.ref.display_name.c_str());
    if (a.flags.count("--delete-originals")) {
        if (rc != kOk) {
            std::fprintf(stderr, "Not deleting originals: some items failed.\n");
        } else {
            // Plain unlink: on SSDs/CoW filesystems this is NOT a secure erase.
            for (const auto& p : added) { std::error_code ec; fs::remove(p, ec); }
            std::printf("Removed %zu original file(s) (plain delete, not a secure wipe).\n", added.size());
        }
    }
    return rc;
}

int CmdVaultPut(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), {});
    if (a.pos.size() != 1) return Usage("vault put needs NAME:/dir/file (data comes from stdin)");
    Opened o; int rc = kOk;
    // stdin carries the data, so the password must not come from stdin; the tty prompt uses /dev/tty.
    if (!OpenRef(a.pos[0], a.pw, o, rc)) return rc;
    if (o.inner.empty()) return Err("give a file path inside the vault, e.g. NAME:/notes/todo.txt");
    std::string fname = o.inner.back();
    o.inner.pop_back();
    vaultstore::TreeNode* dir = Walk(*o.vault, o.inner);
    if (!dir || !dir->is_folder) return Err("parent folder doesn't exist (create it with 'vault mkdir')");
    if (FindChild(*dir, fname)) return Err(fname + ": already exists");
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    std::string error;
    bool ok = o.vault->AddFile(*dir, fname, data, error);
    if (!data.empty()) sodium_memzero(data.data(), data.size());
    return ok ? kOk : Err(error);
}

int CmdVaultGet(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{"--force"}), (std::set<std::string>{"-o"}));
    if (a.pos.size() != 1) return Usage("vault get needs NAME:/dir/file");
    Opened o; int rc = kOk;
    if (!OpenRef(a.pos[0], a.pw, o, rc)) return rc;
    vaultstore::TreeNode* node = Walk(*o.vault, o.inner);
    if (!node || node->is_folder) return Err("not a file in this vault: /" + o.ref.inner);
    std::vector<uint8_t> data;
    std::string error;
    if (!o.vault->ReadFile(*node, data, error)) return Err(error);
    int code = kOk;
    if (a.opts.count("-o")) {
        std::string out = cvfs::ExpandPath(a.opts["-o"]);
        int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | (a.flags.count("--force") ? O_TRUNC : O_EXCL), 0600);
        if (fd < 0) code = Err(out + ": " + std::strerror(errno) + (errno == EEXIST ? " (use --force to overwrite)" : ""));
        else {
            size_t off = 0;
            while (off < data.size()) {
                ssize_t n = ::write(fd, data.data() + off, data.size() - off);
                if (n <= 0) { code = Err(out + ": write failed"); break; }
                off += static_cast<size_t>(n);
            }
            ::close(fd);
        }
    } else {
        if (isatty(STDOUT_FILENO)) code = Err("refusing to dump file contents to a terminal; redirect (> file), pipe, or use -o");
        else if (!data.empty() && std::fwrite(data.data(), 1, data.size(), stdout) != data.size()) code = Err("write to stdout failed");
        else std::fflush(stdout);
    }
    if (!data.empty()) sodium_memzero(data.data(), data.size());
    return code;
}

int CmdVaultMkdir(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{"-p"}), {});
    if (a.pos.size() != 1) return Usage("vault mkdir needs NAME:/dir");
    Opened o; int rc = kOk;
    if (!OpenRef(a.pos[0], a.pw, o, rc)) return rc;
    if (o.inner.empty()) return Err("give a folder path, e.g. NAME:/Photos");
    vaultstore::TreeNode* cur = &o.vault->root();
    std::string error;
    for (size_t i = 0; i < o.inner.size(); ++i) {
        vaultstore::TreeNode* next = FindChild(*cur, o.inner[i]);
        bool last = i + 1 == o.inner.size();
        if (next) {
            if (!next->is_folder) return Err(o.inner[i] + " is a file");
            if (last && !a.flags.count("-p")) return Err("already exists: /" + o.ref.inner);
        } else {
            if (!last && !a.flags.count("-p")) return Err("parent doesn't exist (use -p)");
            if (!o.vault->AddFolder(*cur, o.inner[i], error)) return Err(error);
            next = FindChild(*cur, o.inner[i]);
        }
        cur = next;
    }
    return kOk;
}

int CmdVaultRm(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{"-r", "-f"}), {});
    if (a.pos.size() != 1) return Usage("vault rm needs NAME:/path");
    Opened o; int rc = kOk;
    if (!OpenRef(a.pos[0], a.pw, o, rc)) return rc;
    if (o.inner.empty()) return Err("refusing to remove the vault root; delete the vault directory yourself if you mean it");
    std::string leaf = o.inner.back();
    o.inner.pop_back();
    vaultstore::TreeNode* parent = Walk(*o.vault, o.inner);
    vaultstore::TreeNode* node = parent ? FindChild(*parent, leaf) : nullptr;
    if (!node) return Err("no such path in vault: /" + o.ref.inner);
    if (node->is_folder && !node->children.empty() && !a.flags.count("-r")) return Err("folder isn't empty (use -r)");
    std::string error;
    return o.vault->Delete(*parent, leaf, error) ? kOk : Err(error);
}

int CmdVaultMv(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), {});
    if (a.pos.size() != 2) return Usage("vault mv needs NAME:/path NEWNAME (rename in place)");
    Opened o; int rc = kOk;
    if (!OpenRef(a.pos[0], a.pw, o, rc)) return rc;
    vaultstore::TreeNode* node = Walk(*o.vault, o.inner);
    if (!node || o.inner.empty()) return Err("no such path in vault: /" + o.ref.inner);
    std::string error;
    return o.vault->Rename(*node, a.pos[1], error) ? kOk : Err(error);
}

int CmdVaultPasswd(const std::vector<std::string>& rest) {
    PARSE(a, (std::set<std::string>{}), {});
    if (a.pos.size() != 1) return Usage("vault passwd needs NAME");
    VaultRef ref; std::string error;
    if (!ResolveVaultRef(a.pos[0], ref, error)) return Err(error);
    std::string oldp, newp, again;
    Scrub s1{oldp}, s2{newp}, s3{again};
    if (!PromptPassword("Current password: ", oldp)) return Err("this command needs an interactive terminal");
    auto vault = vaultstore::OpenVault(ref.dir, oldp, error);
    if (!vault) return Err("could not unlock: " + error, kUnlockFailed);
    if (!PromptPassword("New password: ", newp) || !PromptPassword("Confirm new password: ", again)) return Err("no terminal");
    if (newp.empty()) return Err("empty passwords are refused");
    if (newp != again) return Err("passwords did not match");
    if (!vault->ChangePassword(oldp, newp, error)) return Err(error);
    std::printf("Password changed. File contents were not re-encrypted (the master key was re-wrapped).\n");
    return kOk;
}

int CmdVault(const std::vector<std::string>& rest) {
    if (rest.empty()) return Usage("vault needs a subcommand (list, create, ls, add, get, ...)");
    std::string sub = rest[0];
    std::vector<std::string> tail(rest.begin() + 1, rest.end());
    if (sub == "list") return CmdVaultList();
    if (sub == "create") return CmdVaultCreate(tail);
    if (sub == "register") return CmdVaultRegister(tail);
    if (sub == "forget") return CmdVaultForget(tail);
    if (sub == "ls") return CmdVaultLs(tail, false);
    if (sub == "tree") return CmdVaultLs(tail, true);
    if (sub == "add") return CmdVaultAdd(tail);
    if (sub == "put") return CmdVaultPut(tail);
    if (sub == "get" || sub == "cat") return CmdVaultGet(tail);
    if (sub == "mkdir") return CmdVaultMkdir(tail);
    if (sub == "rm") return CmdVaultRm(tail);
    if (sub == "mv") return CmdVaultMv(tail);
    if (sub == "passwd") return CmdVaultPasswd(tail);
    return Usage("unknown vault command '" + sub + "'");
}

// --- completion scripts (also installed as files by CMake) --------------------

const char* kBashCompletion = R"CVEOF(# bash completion for cryptvault - generated by `cryptvault completion bash`
_cryptvault() {
    local cur prev words cword
    _init_completion -s 2>/dev/null || { cur="${COMP_WORDS[COMP_CWORD]}"; prev="${COMP_WORDS[COMP_CWORD-1]}"; cword=$COMP_CWORD; words=("${COMP_WORDS[@]}"); }
    local top="gui ls find info places open trash vault completion --help --version"
    local vsub="list create register forget ls tree add put get mkdir rm mv passwd"
    if [[ $cword -eq 1 ]]; then COMPREPLY=( $(compgen -W "$top" -- "$cur") $(compgen -d -- "$cur") ); return; fi
    case "${words[1]}" in
        vault)
            if [[ $cword -eq 2 ]]; then COMPREPLY=( $(compgen -W "$vsub" -- "$cur") ); return; fi
            case "${words[2]}" in
                create|list) ;;
                *) if [[ $cword -eq 3 ]]; then
                       local names; names=$(cut -f1 "$HOME/.cryptvault/known_vaults.txt" 2>/dev/null)
                       COMPREPLY=( $(compgen -W "$names" -- "$cur") ); return
                   fi ;;
            esac
            COMPREPLY=( $(compgen -f -- "$cur") ) ;;
        trash) if [[ $cword -eq 2 ]]; then COMPREPLY=( $(compgen -W "put list restore empty" -- "$cur") ); else COMPREPLY=( $(compgen -f -- "$cur") ); fi ;;
        completion) COMPREPLY=( $(compgen -W "bash zsh" -- "$cur") ) ;;
        ls) COMPREPLY=( $(compgen -W "-a -l -S -t -X -r" -- "$cur") $(compgen -d -- "$cur") ) ;;
        *) COMPREPLY=( $(compgen -f -- "$cur") ) ;;
    esac
}
complete -F _cryptvault cryptvault
)CVEOF";

const char* kZshCompletion = R"CVEOF(#compdef cryptvault
# zsh completion for cryptvault - generated by `cryptvault completion zsh`
_cryptvault_vaults() {
    local -a names
    names=(${(f)"$(cut -f1 $HOME/.cryptvault/known_vaults.txt 2>/dev/null)"})
    _describe 'vault' names
}
_cryptvault() {
    local -a top vsub
    top=(
        'gui:open the explorer window'
        'ls:list a directory (vaults are marked)'
        'find:search file names under a folder'
        'info:details for a file or folder'
        'places:home, XDG folders, mounted volumes'
        'open:open with the default app'
        'trash:freedesktop Trash (put/list/restore/empty)'
        'vault:encrypted vaults (restricted side)'
        'completion:print a shell completion script'
    )
    vsub=(list create register forget ls tree add put get mkdir rm mv passwd)
    if (( CURRENT == 2 )); then
        _describe 'command' top
        _path_files -/
        return
    fi
    case $words[2] in
        vault)
            if (( CURRENT == 3 )); then compadd -a vsub; return; fi
            case $words[3] in
                create|list) ;;
                *) if (( CURRENT == 4 )); then _cryptvault_vaults; return; fi ;;
            esac
            _files ;;
        trash) (( CURRENT == 3 )) && compadd put list restore empty || _files ;;
        completion) compadd bash zsh ;;
        ls) _arguments '-a[show hidden]' '-l[long format]' '-S[sort by size]' '-t[sort by time]' '-X[sort by type]' '-r[reverse]' '*:directory:_path_files -/' ;;
        *) _files ;;
    esac
}
_cryptvault "$@"
)CVEOF";

}  // namespace

int Run(int argc, char** argv, GuiLaunch& launch) {
    if (sodium_init() < 0) return Err("could not initialise libsodium");
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) return kLaunchGui;
    const std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "--help" || cmd == "-h" || cmd == "help") { std::printf(kHelp, kVersion); return kOk; }
    if (cmd == "--version" || cmd == "-V") { std::printf("cryptvault %s\n", kVersion); return kOk; }

    if (cmd == "gui") {
        for (const auto& a : rest) {
            if (a == "--vaults") launch.vault_side = true;
            else if (!a.empty() && a[0] == '-') return Usage("unknown option " + a);
            else launch.start_path = cvfs::ExpandPath(a);
        }
        return kLaunchGui;
    }
    if (cmd == "ls") return CmdLs(rest);
    if (cmd == "info") return CmdInfo(rest);
    if (cmd == "find") return CmdFind(rest);
    if (cmd == "places") return CmdPlaces();
    if (cmd == "open") return CmdOpen(rest);
    if (cmd == "trash") return CmdTrash(rest);
    if (cmd == "vault") return CmdVault(rest);
    if (cmd == "completion") {
        if (rest.size() == 1 && rest[0] == "bash") { std::fputs(kBashCompletion, stdout); return kOk; }
        if (rest.size() == 1 && rest[0] == "zsh") { std::fputs(kZshCompletion, stdout); return kOk; }
        return Usage("completion needs 'bash' or 'zsh'");
    }

    // `cryptvault ~/Downloads` / `cryptvault .` -> open the window there.
    std::string maybe = cvfs::ExpandPath(cmd);
    std::error_code ec;
    if (fs::is_directory(maybe, ec)) {
        if (cvfs::IsVaultDir(maybe)) launch.vault_side = true;   // pointing at a vault -> restricted side
        else launch.start_path = maybe;
        return kLaunchGui;
    }
    return Usage("unknown command '" + cmd + "'");
}

}  // namespace cvcli
