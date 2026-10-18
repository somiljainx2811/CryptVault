// Headless checks for the explorer engine (src/explorer/fs_ops.*).
// Runs entirely inside a temp dir with HOME/XDG_DATA_HOME redirected,
// so it never touches the real home directory or real trash.
#include "explorer/fs_ops.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++g_pass; }                                              \
        else { ++g_fail; std::printf("  [FAIL] %s (line %d)\n", #cond, __LINE__); } \
    } while (0)

static void Write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

int main() {
    fs::path root = fs::temp_directory_path() / ("cvfs_test_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "home");
    setenv("HOME", (root / "home").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "home/.local/share").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "home/.config").c_str(), 1);
    std::string work = (root / "work").string();
    std::string err;

    std::printf("=== listing & sorting ===\n");
    Write(root / "work/file10.txt", "0123456789");
    Write(root / "work/file2.txt", "ab");
    Write(root / "work/.hidden", "x");
    Write(root / "work/B.md", "");
    fs::create_directories(root / "work/zdir");
    fs::create_symlink("file2.txt", root / "work/link");
    fs::create_symlink("nowhere", root / "work/dangling");
    std::vector<cvfs::Entry> es;
    cvfs::ListOptions lo;
    CHECK(cvfs::ListDir(work, lo, es, err));
    CHECK(es.size() == 6);                       // .hidden excluded
    CHECK(es[0].name == "zdir" && es[0].is_dir);  // dirs first
    auto idx = [&](const std::string& n) { for (size_t i = 0; i < es.size(); ++i) if (es[i].name == n) return (int)i; return -1; };
    CHECK(idx("file2.txt") < idx("file10.txt"));  // natural order
    CHECK(idx("B.md") < idx("dangling"));         // case-insensitive
    CHECK(es[idx("link")].is_symlink && es[idx("link")].size == 2);  // follows link for size
    CHECK(es[idx("dangling")].is_symlink);        // broken link still lists
    lo.show_hidden = true;
    CHECK(cvfs::ListDir(work, lo, es, err) && es.size() == 7);
    lo.show_hidden = false; lo.sort = cvfs::SortKey::Size; lo.reverse = true;
    CHECK(cvfs::ListDir(work, lo, es, err) && es[0].is_dir);  // dirs-first isn't reversed
    CHECK(!cvfs::ListDir(work + "/nope", cvfs::ListOptions{}, es, err) && !err.empty());

    std::printf("=== vault detection ===\n");
    Write(root / "work/myvault/vault.meta", "m");
    Write(root / "work/myvault/manifest.enc", "m");
    CHECK(cvfs::IsVaultDir(work + "/myvault"));
    CHECK(!cvfs::IsVaultDir(work + "/zdir"));
    CHECK(cvfs::ListDir(work, cvfs::ListOptions{}, es, err));
    CHECK(es[idx("myvault")].is_vault);

    std::printf("=== search ===\n");
    Write(root / "work/deep/er/Needle-file.txt", "n");
    Write(root / "work/deep/.hid/needle-hidden.txt", "n");
    Write(root / "work/myvault/needle-inside-vault.txt", "n");
    std::vector<cvfs::Entry> found; bool trunc = false;
    CHECK(cvfs::Search(work, "NEEDLE", false, 100, found, trunc, err));
    CHECK(found.size() == 1 && found[0].name == "deep/er/Needle-file.txt" && !trunc);   // case-insens, relative, no hidden, no vault interior
    CHECK(cvfs::Search(work, "needle", true, 100, found, trunc, err) && found.size() == 2);
    CHECK(cvfs::Search(work, "", false, 3, found, trunc, err) && found.size() == 3 && trunc);   // result cap
    CHECK(!cvfs::Search(work + "/nope", "x", false, 3, found, trunc, err));

    std::printf("=== paths & formatting ===\n");
    CHECK(cvfs::ExpandPath("~") == (root / "home").string());
    CHECK(cvfs::ExpandPath("~/a/../b") == (root / "home/b").string());
    CHECK(cvfs::ParentPath("/") == "/" && cvfs::ParentPath("/a/b") == "/a" && cvfs::ParentPath("/a") == "/");
    CHECK(cvfs::PathSegments("/home/a/b").size() == 4 && cvfs::PathSegments("/home/a/b")[0] == "/");
    CHECK(cvfs::HumanSize(0) == "0 B" && cvfs::HumanSize(1536) == "1.5 KB" && cvfs::HumanSize(5ull << 30) == "5.0 GB");
    CHECK(cvfs::FormatMode(0755, true, false) == "drwxr-xr-x");
    CHECK(cvfs::FormatMode(04755, false, false) == "-rwsr-xr-x");
    CHECK(cvfs::NaturalLess("a9", "a10") && !cvfs::NaturalLess("a10", "a9") && cvfs::NaturalLess("a", "B"));

    std::printf("=== mkdir / touch / rename / copy / move ===\n");
    CHECK(cvfs::MakeDir(work + "/new", err));
    CHECK(!cvfs::MakeDir(work + "/new", err));       // no clobber
    CHECK(cvfs::MakeFile(work + "/new/a.txt", err));
    CHECK(!cvfs::MakeFile(work + "/new/a.txt", err));
    CHECK(cvfs::Rename(work + "/new/a.txt", "b.txt", err) && fs::exists(root / "work/new/b.txt"));
    CHECK(!cvfs::Rename(work + "/new/b.txt", "x/y", err));  // no path separators
    CHECK(cvfs::Copy(work + "/new", work + "/copy", err) && fs::exists(root / "work/copy/b.txt"));
    CHECK(!cvfs::Copy(work + "/new", work + "/copy", err));  // dest exists
    CHECK(!cvfs::Copy(work + "/new", work + "/new/inner", err));  // into itself
    CHECK(cvfs::Move(work + "/copy", work + "/moved", err) && !fs::exists(root / "work/copy") && fs::exists(root / "work/moved/b.txt"));
    CHECK(cvfs::UniqueName(work, "moved") == "moved (copy)");
    CHECK(cvfs::UniqueName(work, "file2.txt") == "file2 (copy).txt");
    CHECK(cvfs::DeletePermanent(work + "/moved", err) && !fs::exists(root / "work/moved"));
    CHECK(!cvfs::DeletePermanent("/", err));
    CHECK(!cvfs::DeletePermanent((root / "home").string(), err));

    // Trash listings also include volume trashes of whatever is mounted on the
    // machine running the test; only look at items this test created.
    auto ListMine = [&](std::vector<cvfs::TrashItem>& mine, std::string& e) {
        std::vector<cvfs::TrashItem> all;
        bool ok = cvfs::TrashList(all, e);
        mine.clear();
        for (auto& it : all) {
            if (it.original_path.rfind(root.string(), 0) == 0) mine.push_back(it);
        }
        return ok;
    };

    std::printf("=== trash (freedesktop spec) ===\n");
    Write(root / "work/t/doc with space & %.txt", "hello");
    CHECK(cvfs::TrashPut(work + "/t/doc with space & %.txt", err));
    CHECK(!fs::exists(root / "work/t/doc with space & %.txt"));
    Write(root / "work/t/doc with space & %.txt", "second");   // same name again
    CHECK(cvfs::TrashPut(work + "/t/doc with space & %.txt", err));
    std::vector<cvfs::TrashItem> items;
    CHECK(ListMine(items, err) && items.size() == 2);
    CHECK(items.size() == 2 && items[0].original_path == work + "/t/doc with space & %.txt");  // percent round-trip
    CHECK(fs::exists(root / "home/.local/share/Trash/info/doc with space & %.txt.trashinfo"));
    std::ifstream info(root / "home/.local/share/Trash/info/doc with space & %.txt.trashinfo");
    std::string l1, l2, l3; std::getline(info, l1); std::getline(info, l2); std::getline(info, l3);
    CHECK(l1 == "[Trash Info]" && l2.rfind("Path=", 0) == 0 && l2.find("%20") != std::string::npos && l3.rfind("DeletionDate=", 0) == 0);
    CHECK(cvfs::TrashRestore("doc with space & %.txt", err) && fs::exists(root / "work/t/doc with space & %.txt"));
    CHECK(!cvfs::TrashRestore("doc with space & %.txt.1", err));   // original now occupied -> refuses, no overwrite
    CHECK(!cvfs::TrashRestore("../etc", err));
    Write(root / "work/dirtrash/inner/f", "1");
    CHECK(cvfs::TrashPut(work + "/dirtrash", err));
    CHECK(ListMine(items, err) && items.size() == 2);
    CHECK(cvfs::TrashDelete("dirtrash", err) && !fs::exists(root / "home/.local/share/Trash/files/dirtrash")
          && !fs::exists(root / "home/.local/share/Trash/info/dirtrash.trashinfo"));
    CHECK(!cvfs::TrashDelete("../x", err) && !cvfs::TrashDelete("dirtrash", err));
    {   // TrashEmpty empties EVERY trash, including real volume trashes on this machine:
        // only exercise it when the test's own items are all there is.
        std::vector<cvfs::TrashItem> all;
        cvfs::TrashList(all, err);
        ListMine(items, err);  // refresh: the earlier listing predates TrashDelete("dirtrash")
        if (all.size() == items.size()) {
            CHECK(cvfs::TrashEmpty(err) && ListMine(items, err) && items.empty());
        } else {
            for (auto& it : items) CHECK(cvfs::TrashDelete(it.payload_path, err));
            std::printf("  [skip] TrashEmpty (machine has other trash); deleted this test's items one by one\n");
        }
    }
    CHECK(!cvfs::TrashPut((root / "home").string(), err));

    std::printf("=== per-volume trash ===\n");
    {
        // /dev/shm is its own filesystem on Linux, i.e. a stand-in for "another drive".
        struct stat st_a {}, st_b {};
        bool distinct = ::stat("/dev/shm", &st_a) == 0 && ::stat(root.c_str(), &st_b) == 0 && st_a.st_dev != st_b.st_dev;
        fs::path vol = fs::path("/dev/shm") / ("cvfs_vol_" + std::to_string(::getpid()));
        std::error_code vec;
        if (!distinct || (fs::create_directories(vol / "data", vec), vec)) {
            std::printf("  [skip] no second filesystem available at /dev/shm\n");
        } else {
            Write(vol / "data/vol file.txt", "on another drive");
            std::string vpath = (vol / "data/vol file.txt").string();
            CHECK(cvfs::TrashPut(vpath, err));
            std::string vroot = "/dev/shm/.Trash-" + std::to_string(::getuid());
            CHECK(fs::exists(vroot + "/files/vol file.txt"));                      // trashed ON that volume (a rename, not a copy)
            CHECK(!fs::exists(root / "home/.local/share/Trash/files/vol file.txt")); // ...and not copied to the home drive
            std::ifstream vi(vroot + "/info/vol file.txt.trashinfo");
            std::string v1, v2; std::getline(vi, v1); std::getline(vi, v2);
            CHECK(v2.rfind("Path=cvfs_vol_", 0) == 0);                             // relative to the volume's mount point, per spec
            std::vector<cvfs::TrashItem> vitems;
            CHECK(cvfs::TrashList(vitems, err));
            const cvfs::TrashItem* mine = nullptr;
            for (auto& it : vitems) if (it.original_path == vpath) mine = &it;
            CHECK(mine && mine->on_volume && mine->size == 16);                    // listed, with the absolute original path
            CHECK(!cvfs::TrashRestore("vol file.txt", err));                       // bare names only address the home trash
            CHECK(!cvfs::TrashRestore("/etc/passwd", err));                        // arbitrary paths are not trash items
            if (mine) {
                CHECK(cvfs::TrashRestore(mine->payload_path, err) && fs::exists(vpath) && !fs::exists(vroot + "/files/vol file.txt"));
                CHECK(cvfs::TrashPut(vpath, err));
                std::vector<cvfs::TrashItem> again; cvfs::TrashList(again, err);
                for (auto& it : again) if (it.original_path == vpath) mine = &it;
                CHECK(cvfs::TrashDelete(mine->payload_path, err) && !fs::exists(vroot + "/files/vol file.txt"));
            }
            fs::remove_all(vol, vec);
            fs::remove(vroot + "/files", vec); fs::remove(vroot + "/info", vec); fs::remove(vroot, vec);   // only removes if empty
        }
    }

    std::printf("=== places ===\n");
    fs::create_directories(root / "home/Downloads");
    auto places = cvfs::Places();
    bool has_home = false, has_dl = false, has_root = false;
    for (auto& p : places) { has_home |= p.kind == "home"; has_dl |= p.name == "Downloads"; has_root |= p.path == "/" && p.total_bytes > 0; }
    CHECK(has_home && has_dl && has_root);

    fs::remove_all(root);
    std::printf("\n=== Summary: %d/%d checks passed ===\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
