// Exercises vault_store end-to-end against the real filesystem in a
// temp directory - no mocking of libsodium or the filesystem. Prints
// each check as it runs and exits non-zero on the first failure, so
// `./vault_store_test && echo ALL PASSED` is a meaningful thing to
// run in CI or by hand.
#include "vault/vault_store.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& description) {
    ++g_checks;
    if (condition) {
        std::printf("  [ok] %s\n", description.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", description.c_str());
    }
}

std::vector<uint8_t> StringToBytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
std::string BytesToString(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

// Reads every regular file under `dir` (recursively) into one
// concatenated buffer - used to sanity-check that a plaintext needle
// (a real filename, a real file's content) is nowhere on disk at all,
// not just absent from one specific file.
std::vector<uint8_t> SlurpDirectory(const fs::path& dir) {
    std::vector<uint8_t> all;
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<uint8_t> contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        all.insert(all.end(), contents.begin(), contents.end());
    }
    return all;
}

bool ContainsSubsequence(const std::vector<uint8_t>& haystack, const std::string& needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

}  // namespace

int main() {
    fs::path test_root = fs::temp_directory_path() / "cryptvault_store_test";
    std::error_code ec;
    fs::remove_all(test_root, ec);  // start clean even if a previous run crashed mid-test
    fs::create_directories(test_root, ec);
    fs::path vault_path = test_root / "MySecretVault";

    const std::string kPassword = "correct horse battery staple";
    const std::string kWrongPassword = "definitely not it";
    const std::string kSecretFolderName = "Tax Documents 2025";
    const std::string kSecretFileName = "social_security_number.txt";
    const std::string kSecretContent = "This plaintext must never appear unencrypted on disk.";

    std::printf("=== 1. Create a vault, add a folder and a file ===\n");
    std::string error;
    std::string folder_blob_hint;  // not used for lookups, just a readability aid below
    {
        auto vault = vaultstore::CreateVault(vault_path.string(), kPassword, error);
        Check(vault != nullptr, "CreateVault succeeds: " + error);
        if (!vault) return 1;

        Check(vault->root().children.empty(), "fresh vault has an empty root");

        bool added_folder = vault->AddFolder(vault->root(), kSecretFolderName, error);
        Check(added_folder, "AddFolder succeeds: " + error);

        vaultstore::TreeNode& folder = vault->root().children.back();
        bool added_file =
            vault->AddFile(folder, kSecretFileName, StringToBytes(kSecretContent), error);
        Check(added_file, "AddFile succeeds: " + error);

        Check(fs::exists(vault_path / "vault.meta"), "vault.meta exists on disk");
        Check(fs::exists(vault_path / "manifest.enc"), "manifest.enc exists on disk");
        Check(fs::exists(vault_path / "blobs") && !fs::is_empty(vault_path / "blobs"),
              "blobs/ exists and has at least one file");
    }  // vault destructed here - master key should be scrubbed, nothing left open

    std::printf("\n=== 2. Confirm nothing readable is sitting on disk ===\n");
    {
        std::vector<uint8_t> everything = SlurpDirectory(vault_path);
        Check(!ContainsSubsequence(everything, kSecretFolderName),
              "folder name is NOT present in plaintext anywhere on disk");
        Check(!ContainsSubsequence(everything, kSecretFileName),
              "file name is NOT present in plaintext anywhere on disk");
        Check(!ContainsSubsequence(everything, kSecretContent),
              "file content is NOT present in plaintext anywhere on disk");

        // The actual on-disk blob filename should be an opaque hex
        // id, not the real name.
        bool found_named_blob = false;
        for (auto& entry : fs::directory_iterator(vault_path / "blobs")) {
            if (entry.path().filename().string().find("social_security") != std::string::npos) {
                found_named_blob = true;
            }
        }
        Check(!found_named_blob, "blob filename on disk doesn't leak the real filename");
    }

    std::printf("\n=== 3. Wrong password is rejected cleanly ===\n");
    {
        std::string open_error;
        auto vault = vaultstore::OpenVault(vault_path.string(), kWrongPassword, open_error);
        Check(vault == nullptr, "OpenVault with the wrong password returns nullptr");
        Check(!open_error.empty(), "a non-empty error is reported: \"" + open_error + "\"");
    }

    std::printf("\n=== 4. Correct password reopens it with everything intact ===\n");
    {
        std::string open_error;
        auto vault = vaultstore::OpenVault(vault_path.string(), kPassword, open_error);
        Check(vault != nullptr, "OpenVault with the correct password succeeds: " + open_error);
        if (!vault) return 1;

        Check(vault->root().children.size() == 1, "root has exactly one child after reopening");
        if (!vault->root().children.empty()) {
            const vaultstore::TreeNode& folder = vault->root().children[0];
            Check(folder.name == kSecretFolderName, "folder name decrypted correctly: \"" + folder.name + "\"");
            Check(folder.is_folder, "it's still a folder");
            Check(folder.children.size() == 1, "folder has exactly one child");
            if (!folder.children.empty()) {
                const vaultstore::TreeNode& file = folder.children[0];
                Check(file.name == kSecretFileName, "file name decrypted correctly: \"" + file.name + "\"");

                std::vector<uint8_t> content;
                std::string read_error;
                bool read_ok = vault->ReadFile(file, content, read_error);
                Check(read_ok, "ReadFile succeeds: " + read_error);
                Check(BytesToString(content) == kSecretContent, "file content decrypted correctly");
            }
        }
    }

    std::printf("\n=== 5. Change password, confirm blobs untouched, old password rejected ===\n");
    {
        fs::path blobs_dir = vault_path / "blobs";
        fs::file_time_type blob_mtime_before;
        for (auto& entry : fs::directory_iterator(blobs_dir)) {
            blob_mtime_before = fs::last_write_time(entry.path());
        }

        std::string open_error;
        auto vault = vaultstore::OpenVault(vault_path.string(), kPassword, open_error);
        Check(vault != nullptr, "reopen before changing password: " + open_error);
        if (!vault) return 1;

        std::string change_error;
        bool changed = vault->ChangePassword(kPassword, "a brand new password entirely", change_error);
        Check(changed, "ChangePassword succeeds: " + change_error);

        for (auto& entry : fs::directory_iterator(blobs_dir)) {
            Check(fs::last_write_time(entry.path()) == blob_mtime_before,
                  "blob file untouched by password change (same mtime) - confirms only vault.meta was rewritten");
        }
    }
    {
        std::string open_error;
        auto vault = vaultstore::OpenVault(vault_path.string(), kPassword, open_error);
        Check(vault == nullptr, "OLD password no longer works after ChangePassword");
    }
    {
        std::string open_error;
        auto vault = vaultstore::OpenVault(vault_path.string(), "a brand new password entirely", open_error);
        Check(vault != nullptr, "NEW password works after ChangePassword: " + open_error);
        if (vault) {
            std::vector<uint8_t> content;
            std::string read_error;
            bool read_ok = vault->root().children.size() == 1 && !vault->root().children[0].children.empty()
                && vault->ReadFile(vault->root().children[0].children[0], content, read_error);
            Check(read_ok && BytesToString(content) == kSecretContent,
                  "file content still reads correctly after password change");
        }
    }

    std::printf("\n=== 6. Rename and delete ===\n");
    {
        std::string open_error;
        auto vault = vaultstore::OpenVault(vault_path.string(), "a brand new password entirely", open_error);
        Check(vault != nullptr, "reopen for rename/delete test: " + open_error);
        if (!vault) return 1;

        vaultstore::TreeNode& folder = vault->root().children[0];
        std::string rename_error;
        bool renamed = vault->Rename(folder, "Tax Documents 2026", rename_error);
        Check(renamed, "Rename succeeds: " + rename_error);
        Check(folder.name == "Tax Documents 2026", "in-memory tree reflects the rename");

        // Reopen fresh to make sure the rename was actually persisted
        // to manifest.enc, not just held in memory.
        std::string reopen_error;
        auto reopened = vaultstore::OpenVault(vault_path.string(), "a brand new password entirely", reopen_error);
        Check(reopened != nullptr, "reopen after rename: " + reopen_error);
        if (reopened) {
            Check(!reopened->root().children.empty() && reopened->root().children[0].name == "Tax Documents 2026",
                  "rename persisted across close/reopen");
        }

        std::string delete_error;
        std::string blob_id = folder.children[0].blob_id;
        bool deleted = vault->Delete(vault->root(), folder.name, delete_error);
        Check(deleted, "Delete succeeds: " + delete_error);
        Check(vault->root().children.empty(), "root is empty after deleting the only folder");
        Check(!fs::exists(vault_path / "blobs" / (blob_id + ".bin")),
              "the file's blob was actually removed from disk, not just unlinked from the tree");
    }

    std::printf("\n=== Summary: %d/%d checks passed ===\n", g_checks - g_failures, g_checks);
    fs::remove_all(test_root, ec);
    return g_failures == 0 ? 0 : 1;
}
