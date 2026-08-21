// vault_store: real, on-disk, encrypted vault storage. This is the
// actual security-relevant engine behind CryptVault, deliberately
// kept independent of the UI (no GLFW/wgpu/AppState in here at all -
// see vault_store_test.cpp for a from-first-principles exercise of
// this file alone) so it can be reasoned about, tested, and reviewed
// on its own before anything gets wired to a screen.
//
// --- Design summary ---------------------------------------------------
//
// A vault is a real directory on disk:
//
//   <vault-dir>/
//     vault.meta       - small header: format version, Argon2id salt
//                         and cost params, and the vault's Master Key
//                         (MK) wrapped (encrypted) under a key derived
//                         from the password. NOT otherwise sensitive
//                         by itself - it doesn't contain any vault
//                         content or names, just enough to (a) know
//                         whether an entered password is right and
//                         (b) recover MK if it is.
//     manifest.enc      - the entire folder tree (real names,
//                         structure, which blob file backs each entry)
//                         encrypted as one blob under MK. This is what
//                         makes names/structure private, not just file
//                         contents - see OpenVault().
//     blobs/<id>.bin     - one independently-encrypted blob per file,
//                         named by a random 16-byte hex id that has no
//                         relationship to the real filename. Read/
//                         written independently of the rest of the
//                         vault, so opening/saving one file doesn't
//                         require touching (or even fully decrypting)
//                         anything else.
//
// Two-layer key model (why ChangePassword is cheap):
//   password --Argon2id(salt)--> KEK ("key-encryption key")
//   KEK --AEAD-decrypt--> MK ("master key", random, generated once at
//                         creation, never derived from the password)
//   MK is what actually encrypts manifest.enc and every blob.
// Changing the password only re-derives a new KEK (new random salt)
// and re-wraps the *same* MK under it - manifest.enc and every blob
// are untouched. Without this, a password change would mean
// decrypting and re-encrypting the entire vault.
//
// Cipher: XChaCha20-Poly1305 (crypto_aead_xchacha20poly1305_ietf) for
// everything - AEAD (detects tampering/corruption, not just
// decrypts), and its 24-byte nonces are large enough to generate
// randomly per-encryption without a realistic collision risk (unlike
// AES-GCM's 96-bit nonces, which need a counter or similar to be safe
// at scale). KDF: Argon2id (crypto_pwhash) at libsodium's INTERACTIVE
// cost by default - deliberately not MODERATE/SENSITIVE, since this
// runs synchronously on the UI thread today (see the note on
// CreateVault/OpenVault below); revisit if unlock is moved off-thread.
//
// --- Threat model - what this does and doesn't defend against ---------
// Defends against: someone with access to the vault directory (a
// stolen drive, a synced backup, another user on a shared machine)
// reading file contents, names, or folder structure without the
// password. Does NOT defend against: malware already running as you
// while a vault is unlocked (it can just read the decrypted content,
// or your password, the same way you can), or someone watching you
// type the password. No software-only vault can defend against the
// first of those - it's a fundamental limit, not a gap in this
// implementation.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vaultstore {

// One entry in a vault's folder tree, decrypted and held in memory
// only while its vault is open (see Vault::root()). Mutating this
// tree directly (e.g. renaming node.name) does NOT persist anything -
// go through Vault's methods below, which both update the tree AND
// re-encrypt+rewrite manifest.enc, so the two can't drift apart.
struct TreeNode {
    std::string name;
    bool is_folder = true;

    // Files only: which blobs/<blob_id>.bin holds this file's
    // encrypted content, and its plaintext size (so the UI can show
    // "2.4 GB" etc. without decrypting anything).
    std::string blob_id;
    uint64_t size = 0;

    // Folders only.
    std::vector<TreeNode> children;

    // UI-only, never serialized into manifest.enc (see
    // SerializeNode in vault_store.cpp, which deliberately doesn't
    // touch this field) - eased 0..1 hover amount for whichever tile
    // grid is currently displaying this node. Living here rather than
    // in a parallel UI-side array means it naturally survives
    // AddFolder/Delete/Rename (all of which mutate this same tree in
    // place) without the UI needing to keep two data structures in
    // sync.
    float hover_amount = 0.0f;
};

// An opened (unlocked) vault: the decrypted tree, plus everything
// needed to write more blobs/a new manifest back out, and to re-wrap
// the master key on a password change. Never serializes its own master
// key to disk in the clear - see the file header's two-layer key
// model. Move-only (holds real file-descriptor-adjacent state and
// key material - copying it would either be a deep, expensive clone
// or an accidental key-material aliasing bug, so it's disallowed
// rather than picking one silently).
class Vault {
public:
    Vault(Vault&&) noexcept;
    Vault& operator=(Vault&&) noexcept;
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;
    ~Vault();

    // The decrypted folder tree. `root().name` is unused (the vault's
    // display name is a UI-level concern, not stored here); its
    // `children` are the vault's top-level folders/files.
    TreeNode& root();
    const TreeNode& root() const;

    // Adds an empty folder under `parent` (which must be `root()` or
    // one of its descendants, by reference into the same tree - not a
    // copy). Re-encrypts and rewrites manifest.enc before returning.
    // Fails if `parent` isn't a folder, or a child with that name
    // already exists under it.
    bool AddFolder(TreeNode& parent, const std::string& name, std::string& error);

    // Encrypts `content` into a brand-new blob file and adds a file
    // entry for it under `parent`. Re-encrypts and rewrites
    // manifest.enc. Fails under the same conditions as AddFolder.
    bool AddFile(TreeNode& parent, const std::string& name, const std::vector<uint8_t>& content,
                 std::string& error);

    // Decrypts and returns a file entry's content. `node` must be a
    // file (is_folder == false) still present somewhere in this
    // vault's tree.
    bool ReadFile(const TreeNode& node, std::vector<uint8_t>& out_content, std::string& error);

    // Replaces a file entry's content in place (same tree position,
    // same blob_id reused - the old blob is overwritten, not
    // orphaned). Re-encrypts and rewrites manifest.enc (the size
    // changed).
    bool WriteFile(TreeNode& node, const std::vector<uint8_t>& content, std::string& error);

    // Renames a node in place. Fails if a sibling already has that
    // name.
    bool Rename(TreeNode& node, const std::string& new_name, std::string& error);

    // Deletes `child_name` out of `parent` - if it's a folder, every
    // blob anywhere underneath it is deleted too, not just the
    // manifest entry.
    bool Delete(TreeNode& parent, const std::string& child_name, std::string& error);

    // Re-derives a new KEK from `new_password` (fresh random salt)
    // and re-wraps the existing master key under it - see the file
    // header for why this doesn't touch manifest.enc or any blob.
    // `old_password` must be correct or this fails without changing
    // anything.
    bool ChangePassword(const std::string& old_password, const std::string& new_password,
                         std::string& error);

private:
    friend std::unique_ptr<Vault> CreateVault(const std::string&, const std::string&, std::string&);
    friend std::unique_ptr<Vault> OpenVault(const std::string&, const std::string&, std::string&);
    Vault();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Creates a brand-new vault at `path` (a directory - created if
// missing; if it already exists it must be empty, so this never
// silently overwrites something). Returns the opened vault (with an
// empty root) on success, or nullptr with `error` set on failure.
std::unique_ptr<Vault> CreateVault(const std::string& path, const std::string& password, std::string& error);

// Opens an existing vault at `path`. Returns nullptr with `error` set
// if the path isn't a vault this understands, the vault is corrupt,
// or - most commonly - `password` is wrong (which is
// indistinguishable on purpose from "corrupt": an attacker shouldn't
// be able to learn anything about *why* a guess failed).
std::unique_ptr<Vault> OpenVault(const std::string& path, const std::string& password, std::string& error);

}  // namespace vaultstore
