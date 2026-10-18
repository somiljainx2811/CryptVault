// fs_ops: the "unrestricted side" of CryptVault - a plain Linux file
// explorer engine. Like vault_store, it is deliberately independent of
// the UI (no GLFW/wgpu in here at all), so the same code drives the GPU
// explorer screen AND the terminal commands (src/cli), and can be
// tested headless (see explorer_test.cpp).
//
// What lives here: directory listing/sorting, freedesktop "places"
// (XDG user dirs + mounted volumes), copy/move/rename/mkdir, and a
// freedesktop.org-Trash-spec-compatible trash (so files trashed from
// CryptVault show up in GNOME Files / Dolphin's trash and vice versa).
//
// What does NOT live here: anything that touches vault contents. A
// vault directory is, from this side, just an opaque directory that
// happens to be recognizable (IsVaultDir) so the UI can badge it and
// route it to the restricted side - this module never reads
// vault.meta or manifest.enc.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cvfs {

struct Entry {
    std::string name;
    std::string path;  // absolute
    bool is_dir = false;      // follows symlinks (a link to a directory is a directory here)
    bool is_symlink = false;
    bool is_hidden = false;   // dotfile
    bool is_vault = false;    // directory that looks like a CryptVault vault
    uint64_t size = 0;        // bytes (0 for directories)
    int64_t mtime = 0;        // unix seconds
    uint32_t mode = 0;        // st_mode permission bits (0 if unknown)

    // UI-only (eased hover amount for the tile grid) - same pattern as
    // vaultstore::TreeNode::hover_amount, so DrawTileGrid<T> can draw
    // either kind of item.
    float hover_amount = 0.0f;
};

enum class SortKey { Name, Size, Modified, Type };

struct ListOptions {
    bool show_hidden = false;
    SortKey sort = SortKey::Name;
    bool reverse = false;
    bool dirs_first = true;
};

// Lists `path`. Returns false (with `error` set) if the directory can't
// be read. Individual unreadable entries are skipped, not fatal.
bool ListDir(const std::string& path, const ListOptions& options, std::vector<Entry>& out,
             std::string& error);

// Recursive, case-insensitive substring search by file name under `root`.
// Entry::name is the path relative to `root` (so results read like
// "docs/2024/report.pdf"); Entry::path is absolute. Symlinked folders are
// not followed, vault folders are listed but never descended into, and
// hidden folders are skipped unless `show_hidden`. Bounded so a search of
// "/" can't run away: stops after `max_results` matches or ~150k visited
// entries and sets `truncated`.
bool Search(const std::string& root, const std::string& query, bool show_hidden, size_t max_results,
            std::vector<Entry>& out, bool& truncated, std::string& error);

// Looks like a CryptVault vault: a directory holding both vault.meta and
// manifest.enc. A cheap existence check - never opens either file.
bool IsVaultDir(const std::string& path);

// "~" / "~/x" -> $HOME-based, then made absolute and lexically
// normalized (no symlink resolution - what you typed is what you see,
// like a shell). Empty input means $HOME.
std::string ExpandPath(const std::string& input);

// Parent directory ("/" stays "/").
std::string ParentPath(const std::string& path);

// Splits an absolute path into breadcrumb segments: "/home/a/b" ->
// {"/", "home", "a", "b"}.
std::vector<std::string> PathSegments(const std::string& path);

// --- formatting ----------------------------------------------------------
std::string HumanSize(uint64_t bytes);                         // "2.4 MB"
std::string FormatMode(uint32_t mode, bool is_dir, bool is_link);  // "drwxr-xr-x"
std::string FormatTime(int64_t unix_seconds);                  // "2026-10-15 14:03" (local)
// Natural, case-insensitive ordering: "file2" < "file10".
bool NaturalLess(const std::string& a, const std::string& b);

// --- places (sidebar / `cryptvault places`) -------------------------------
struct Place {
    std::string name;
    std::string path;
    std::string kind;  // "home" | "xdg" | "root" | "mount" | "trash"
    uint64_t total_bytes = 0;  // mounts/root only (0 = unknown)
    uint64_t free_bytes = 0;
};
std::vector<Place> Places();

// --- operations ------------------------------------------------------------
// All return false with `error` set on failure, and never overwrite an
// existing destination (callers pick a free name with UniqueName first
// if they want "paste, keep both" semantics).
bool MakeDir(const std::string& path, std::string& error);
bool MakeFile(const std::string& path, std::string& error);  // like `touch` but refuses to clobber
bool Rename(const std::string& path, const std::string& new_name, std::string& error);
bool Copy(const std::string& from, const std::string& to, std::string& error);
bool Move(const std::string& from, const std::string& to, std::string& error);
// Irreversible. Refuses "/" and $HOME themselves.
bool DeletePermanent(const std::string& path, std::string& error);

// "name", then "name (copy)", "name (copy 2)", ... - first one not
// already present in `dir`. Keeps the extension at the end for files.
std::string UniqueName(const std::string& dir, const std::string& name);

// --- trash (freedesktop.org Trash spec 1.0) -------------------------------------
// Files on the home filesystem go to the home trash. Files on any OTHER
// mounted volume (USB drive, second disk, ...) are trashed on that same
// volume - in $topdir/.Trash/$uid or $topdir/.Trash-$uid - so trashing is a
// rename, not a slow copy of a big file onto your home drive, and the same
// Trash view shows both. (If a volume's trash can't be created, e.g. a
// read-only mount, it falls back to copying into the home trash.)
// GNOME Files / Dolphin / `gio trash` read and write the same locations.
struct TrashItem {
    std::string trashed_name;    // name inside <trash>/files
    std::string payload_path;    // absolute path of the trashed file itself
    std::string trash_root;      // the Trash directory that holds it
    std::string original_path;   // where it came from (absolute)
    std::string deletion_date;   // as stored: YYYY-MM-DDThh:mm:ss
    uint64_t size = 0;
    bool is_dir = false;
    bool on_volume = false;      // true for a non-home trash (per-volume)
};
std::string TrashDir();  // the home trash: $XDG_DATA_HOME/Trash or ~/.local/share/Trash
bool TrashPut(const std::string& path, std::string& error);
// Items from the home trash and every mounted volume's trash, newest first.
bool TrashList(std::vector<TrashItem>& out, std::string& error);
// `handle` is either a bare name (item in the home trash) or the absolute
// payload_path of any listed item (needed for volume trashes). Anything that
// doesn't match a real trash item is rejected - it can't be used to reach
// arbitrary files.
// Restore puts it back at its original location (fails, without overwriting,
// if something is there now).
bool TrashRestore(const std::string& handle, std::string& error);
// Permanently removes ONE item (payload + its .trashinfo).
bool TrashDelete(const std::string& handle, std::string& error);
// Empties the home trash and every volume trash.
bool TrashEmpty(std::string& error);

// Fills an Entry (size/mtime/mode/flags) for one existing path; name is its
// base name. Used to show trash payloads as ordinary files.
bool StatEntry(const std::string& path, Entry& out);

}  // namespace cvfs
