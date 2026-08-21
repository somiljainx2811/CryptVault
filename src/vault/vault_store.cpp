#include "vault/vault_store.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace vaultstore {

namespace fs = std::filesystem;

namespace {

// sodium_init() is safe to call more than once (it no-ops after the
// first successful call) but isn't itself thread-safe on that first
// call - a std::once_flag makes every public entry point below safe
// to call from any thread without the caller needing to know to
// initialize libsodium first.
std::once_flag g_sodium_init_flag;
bool g_sodium_ok = false;
void EnsureSodiumInit() {
    std::call_once(g_sodium_init_flag, [] { g_sodium_ok = (sodium_init() >= 0); });
}

std::string ToHex(const unsigned char* data, size_t len) {
    static const char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = kHexDigits[data[i] >> 4];
        out[2 * i + 1] = kHexDigits[data[i] & 0x0F];
    }
    return out;
}

std::string RandomBlobId() {
    unsigned char raw[16];
    randombytes_buf(raw, sizeof(raw));
    return ToHex(raw, sizeof(raw));
}

// Reads a whole file into memory. Returns false (rather than
// throwing) on any failure - every caller in this file treats "can't
// read a vault file" as an ordinary, reportable error, not a
// programming bug.
bool ReadWholeFile(const fs::path& path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = "can't open " + path.string();
        return false;
    }
    std::streamsize size = f.tellg();
    if (size < 0) {
        error = "can't read size of " + path.string();
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) {
        error = "read failed for " + path.string();
        return false;
    }
    return true;
}

// Writes `data` to `path` atomically: write to a sibling temp file,
// then rename over the real path. On POSIX (and NTFS) a rename onto
// an existing file within the same directory is atomic, so a crash or
// power loss mid-write leaves either the old file or the new one
// intact, never a half-written one - important for manifest.enc,
// which is rewritten on every single vault mutation.
bool WriteWholeFileAtomic(const fs::path& path, const std::vector<uint8_t>& data, std::string& error) {
    fs::path tmp_path = path;
    tmp_path += ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "can't open " + tmp_path.string() + " for writing";
            return false;
        }
        if (!data.empty() && !f.write(reinterpret_cast<const char*>(data.data()),
                                       static_cast<std::streamsize>(data.size()))) {
            error = "write failed for " + tmp_path.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        error = "couldn't finalize " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// --- Tiny binary serializer for TreeNode --------------------------------
// A hand-rolled format rather than pulling in a JSON/protobuf
// dependency - the schema is small and stable enough (and only ever
// read/written by this file) that a dependency would cost more than
// it'd save. Not a general-purpose format: no versioning beyond the
// vault.meta-level format version, fixed field order. Little-endian
// throughout, which is fine for every platform this app targets
// (x86_64/ARM64 desktops) but would need explicit byte-swapping to
// ever run on a big-endian target - flagging that rather than
// silently assuming it doesn't matter.

void WriteU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void WriteU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void WriteString(std::vector<uint8_t>& out, const std::string& s) {
    WriteU32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

bool ReadU32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& out) {
    if (pos + 4 > in.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) out |= static_cast<uint32_t>(in[pos + i]) << (8 * i);
    pos += 4;
    return true;
}
bool ReadU64(const std::vector<uint8_t>& in, size_t& pos, uint64_t& out) {
    if (pos + 8 > in.size()) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(in[pos + i]) << (8 * i);
    pos += 8;
    return true;
}
bool ReadString(const std::vector<uint8_t>& in, size_t& pos, std::string& out) {
    uint32_t len = 0;
    if (!ReadU32(in, pos, len)) return false;
    if (pos + len > in.size()) return false;
    out.assign(reinterpret_cast<const char*>(in.data() + pos), len);
    pos += len;
    return true;
}

void SerializeNode(std::vector<uint8_t>& out, const TreeNode& node) {
    WriteString(out, node.name);
    out.push_back(node.is_folder ? 1 : 0);
    if (node.is_folder) {
        WriteU32(out, static_cast<uint32_t>(node.children.size()));
        for (const TreeNode& child : node.children) {
            SerializeNode(out, child);
        }
    } else {
        WriteString(out, node.blob_id);
        WriteU64(out, node.size);
    }
}

bool DeserializeNode(const std::vector<uint8_t>& in, size_t& pos, TreeNode& node) {
    if (!ReadString(in, pos, node.name)) return false;
    if (pos >= in.size()) return false;
    node.is_folder = (in[pos] != 0);
    ++pos;
    if (node.is_folder) {
        uint32_t count = 0;
        if (!ReadU32(in, pos, count)) return false;
        node.children.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!DeserializeNode(in, pos, node.children[i])) return false;
        }
    } else {
        if (!ReadString(in, pos, node.blob_id)) return false;
        if (!ReadU64(in, pos, node.size)) return false;
    }
    return true;
}

// --- AEAD helpers (XChaCha20-Poly1305-IETF) -----------------------------
// On-disk format for every encrypted blob (manifest.enc and each
// blobs/<id>.bin) is simply [24-byte nonce][ciphertext || 16-byte tag]
// - the nonce doesn't need to be secret, just unique per encryption
// under a given key, and storing it right alongside the ciphertext it
// belongs to is the simplest way to guarantee decrypt always uses the
// matching one.

constexpr size_t kKeyBytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;
constexpr size_t kNonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;

std::vector<uint8_t> AeadEncrypt(const std::vector<uint8_t>& plaintext, const unsigned char key[kKeyBytes]) {
    std::vector<uint8_t> out(kNonceBytes + plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned char* nonce = out.data();
    randombytes_buf(nonce, kNonceBytes);
    unsigned long long ciphertext_len = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + kNonceBytes, &ciphertext_len,
                                                plaintext.data(), plaintext.size(), nullptr, 0, nullptr,
                                                nonce, key);
    out.resize(kNonceBytes + ciphertext_len);
    return out;
}

// Returns false (auth failure - wrong key, or the file was tampered
// with/corrupted) without writing anything useful to `out`.
bool AeadDecrypt(const std::vector<uint8_t>& blob, const unsigned char key[kKeyBytes],
                  std::vector<uint8_t>& out) {
    if (blob.size() < kNonceBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return false;
    }
    const unsigned char* nonce = blob.data();
    const unsigned char* ciphertext = blob.data() + kNonceBytes;
    unsigned long long ciphertext_len = blob.size() - kNonceBytes;
    out.resize(ciphertext_len - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long plaintext_len = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(out.data(), &plaintext_len, nullptr, ciphertext,
                                                          ciphertext_len, nullptr, 0, nonce, key);
    if (rc != 0) {
        out.clear();
        return false;
    }
    out.resize(plaintext_len);
    return true;
}

// --- vault.meta ----------------------------------------------------------
// Fixed-layout binary header, all fields little-endian (see the
// serializer note above). Not itself encrypted - see the file header
// comment on vault_store.h for why that's fine (it carries no vault
// content, just enough to attempt a password and recover the master
// key if it's right).
constexpr char kMagic[8] = {'C', 'V', 'L', 'T', 'v', '1', 0, 0};
constexpr size_t kSaltBytes = crypto_pwhash_SALTBYTES;

struct VaultMeta {
    unsigned char salt[kSaltBytes];
    uint64_t ops_limit = 0;
    uint64_t mem_limit = 0;
    std::vector<uint8_t> wrapped_key;  // AeadEncrypt(master_key, KEK) - nonce+ciphertext+tag
};

std::vector<uint8_t> SerializeMeta(const VaultMeta& meta) {
    std::vector<uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
    out.insert(out.end(), meta.salt, meta.salt + kSaltBytes);
    WriteU64(out, meta.ops_limit);
    WriteU64(out, meta.mem_limit);
    WriteU32(out, static_cast<uint32_t>(meta.wrapped_key.size()));
    out.insert(out.end(), meta.wrapped_key.begin(), meta.wrapped_key.end());
    return out;
}

bool DeserializeMeta(const std::vector<uint8_t>& in, VaultMeta& meta) {
    if (in.size() < sizeof(kMagic) || std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    size_t pos = sizeof(kMagic);
    if (pos + kSaltBytes > in.size()) return false;
    std::memcpy(meta.salt, in.data() + pos, kSaltBytes);
    pos += kSaltBytes;
    if (!ReadU64(in, pos, meta.ops_limit)) return false;
    if (!ReadU64(in, pos, meta.mem_limit)) return false;
    uint32_t wrapped_len = 0;
    if (!ReadU32(in, pos, wrapped_len)) return false;
    if (pos + wrapped_len > in.size()) return false;
    meta.wrapped_key.assign(in.begin() + pos, in.begin() + pos + wrapped_len);
    return true;
}

bool DeriveKek(const std::string& password, const unsigned char salt[kSaltBytes], uint64_t ops_limit,
               uint64_t mem_limit, unsigned char out_kek[kKeyBytes], std::string& error) {
    if (crypto_pwhash(out_kek, kKeyBytes, password.data(), password.size(), salt, ops_limit, mem_limit,
                       crypto_pwhash_ALG_ARGON2ID13) != 0) {
        // Only realistically fails on out-of-memory for the requested
        // mem_limit, not on a "wrong" password (there's no such thing
        // at this stage - any input produces a key, right or wrong).
        error = "password hashing failed (out of memory?)";
        return false;
    }
    return true;
}

fs::path MetaPath(const fs::path& vault_dir) { return vault_dir / "vault.meta"; }
fs::path ManifestPath(const fs::path& vault_dir) { return vault_dir / "manifest.enc"; }
fs::path BlobsDir(const fs::path& vault_dir) { return vault_dir / "blobs"; }
fs::path BlobPath(const fs::path& vault_dir, const std::string& blob_id) {
    return BlobsDir(vault_dir) / (blob_id + ".bin");
}

// Finds every blob_id reachable from `node` (recursing into folders) -
// used by Delete to know which blob files to remove when a folder
// (and everything under it) is deleted.
void CollectBlobIds(const TreeNode& node, std::vector<std::string>& out) {
    if (node.is_folder) {
        for (const TreeNode& child : node.children) {
            CollectBlobIds(child, out);
        }
    } else {
        out.push_back(node.blob_id);
    }
}

}  // namespace

struct Vault::Impl {
    fs::path dir;
    unsigned char master_key[kKeyBytes];
    TreeNode root;

    ~Impl() {
        // Best-effort: scrub the master key from memory once this
        // vault is closed rather than leaving it sitting in freed
        // heap memory. Not a airtight guarantee (the optimizer could
        // still reorder around this in principle, and swapped-out
        // memory is out of our control entirely) but costs nothing
        // and helps against the common case (another process reading
        // this process's freed-but-not-yet-reused heap pages).
        sodium_memzero(master_key, sizeof(master_key));
    }

    bool SaveManifest(std::string& error) {
        std::vector<uint8_t> plaintext;
        SerializeNode(plaintext, root);
        std::vector<uint8_t> encrypted = AeadEncrypt(plaintext, master_key);
        return WriteWholeFileAtomic(ManifestPath(dir), encrypted, error);
    }
};

Vault::Vault() : impl_(std::make_unique<Impl>()) {}
Vault::Vault(Vault&&) noexcept = default;
Vault& Vault::operator=(Vault&&) noexcept = default;
Vault::~Vault() = default;

TreeNode& Vault::root() { return impl_->root; }
const TreeNode& Vault::root() const { return impl_->root; }

bool Vault::AddFolder(TreeNode& parent, const std::string& name, std::string& error) {
    if (!parent.is_folder) {
        error = "not a folder";
        return false;
    }
    for (const TreeNode& child : parent.children) {
        if (child.name == name) {
            error = "\"" + name + "\" already exists";
            return false;
        }
    }
    TreeNode node;
    node.name = name;
    node.is_folder = true;
    parent.children.push_back(std::move(node));
    if (!impl_->SaveManifest(error)) {
        parent.children.pop_back();  // don't leave the in-memory tree ahead of what's on disk
        return false;
    }
    return true;
}

bool Vault::AddFile(TreeNode& parent, const std::string& name, const std::vector<uint8_t>& content,
                     std::string& error) {
    if (!parent.is_folder) {
        error = "not a folder";
        return false;
    }
    for (const TreeNode& child : parent.children) {
        if (child.name == name) {
            error = "\"" + name + "\" already exists";
            return false;
        }
    }
    std::string blob_id = RandomBlobId();
    std::vector<uint8_t> encrypted = AeadEncrypt(content, impl_->master_key);
    std::error_code ec;
    fs::create_directories(BlobsDir(impl_->dir), ec);
    if (!WriteWholeFileAtomic(BlobPath(impl_->dir, blob_id), encrypted, error)) {
        return false;
    }

    TreeNode node;
    node.name = name;
    node.is_folder = false;
    node.blob_id = blob_id;
    node.size = content.size();
    parent.children.push_back(std::move(node));
    if (!impl_->SaveManifest(error)) {
        parent.children.pop_back();
        fs::remove(BlobPath(impl_->dir, blob_id), ec);
        return false;
    }
    return true;
}

bool Vault::ReadFile(const TreeNode& node, std::vector<uint8_t>& out_content, std::string& error) {
    if (node.is_folder) {
        error = "not a file";
        return false;
    }
    std::vector<uint8_t> encrypted;
    if (!ReadWholeFile(BlobPath(impl_->dir, node.blob_id), encrypted, error)) {
        return false;
    }
    if (!AeadDecrypt(encrypted, impl_->master_key, out_content)) {
        error = "blob decryption failed (wrong key, or the file is corrupt/tampered with)";
        return false;
    }
    return true;
}

bool Vault::WriteFile(TreeNode& node, const std::vector<uint8_t>& content, std::string& error) {
    if (node.is_folder) {
        error = "not a file";
        return false;
    }
    std::vector<uint8_t> encrypted = AeadEncrypt(content, impl_->master_key);
    if (!WriteWholeFileAtomic(BlobPath(impl_->dir, node.blob_id), encrypted, error)) {
        return false;
    }
    uint64_t old_size = node.size;
    node.size = content.size();
    if (!impl_->SaveManifest(error)) {
        node.size = old_size;
        return false;
    }
    return true;
}

bool Vault::Rename(TreeNode& node, const std::string& new_name, std::string& error) {
    std::string old_name = node.name;
    node.name = new_name;
    if (!impl_->SaveManifest(error)) {
        node.name = old_name;
        return false;
    }
    return true;
}

bool Vault::Delete(TreeNode& parent, const std::string& child_name, std::string& error) {
    if (!parent.is_folder) {
        error = "not a folder";
        return false;
    }
    auto it = std::find_if(parent.children.begin(), parent.children.end(),
                            [&](const TreeNode& n) { return n.name == child_name; });
    if (it == parent.children.end()) {
        error = "\"" + child_name + "\" not found";
        return false;
    }

    std::vector<std::string> blob_ids;
    CollectBlobIds(*it, blob_ids);

    TreeNode removed = std::move(*it);
    parent.children.erase(it);
    if (!impl_->SaveManifest(error)) {
        // Put it back rather than leaving the in-memory tree ahead of
        // what's on disk - position among siblings doesn't matter
        // much for a failed op like this.
        parent.children.push_back(std::move(removed));
        return false;
    }

    std::error_code ec;
    for (const std::string& blob_id : blob_ids) {
        fs::remove(BlobPath(impl_->dir, blob_id), ec);  // best-effort; manifest is already the source of truth
    }
    return true;
}

bool Vault::ChangePassword(const std::string& old_password, const std::string& new_password,
                            std::string& error) {
    std::vector<uint8_t> meta_bytes;
    if (!ReadWholeFile(MetaPath(impl_->dir), meta_bytes, error)) {
        return false;
    }
    VaultMeta meta;
    if (!DeserializeMeta(meta_bytes, meta)) {
        error = "vault.meta is corrupt";
        return false;
    }

    unsigned char old_kek[kKeyBytes];
    if (!DeriveKek(old_password, meta.salt, meta.ops_limit, meta.mem_limit, old_kek, error)) {
        return false;
    }
    std::vector<uint8_t> unwrapped;
    bool verified = AeadDecrypt(meta.wrapped_key, old_kek, unwrapped);
    sodium_memzero(old_kek, sizeof(old_kek));
    if (!verified) {
        error = "WRONG PASSWORD";
        return false;
    }

    VaultMeta new_meta;
    randombytes_buf(new_meta.salt, kSaltBytes);
    new_meta.ops_limit = meta.ops_limit;
    new_meta.mem_limit = meta.mem_limit;
    unsigned char new_kek[kKeyBytes];
    if (!DeriveKek(new_password, new_meta.salt, new_meta.ops_limit, new_meta.mem_limit, new_kek, error)) {
        return false;
    }
    std::vector<uint8_t> master_key_bytes(impl_->master_key, impl_->master_key + kKeyBytes);
    new_meta.wrapped_key = AeadEncrypt(master_key_bytes, new_kek);
    sodium_memzero(new_kek, sizeof(new_kek));
    sodium_memzero(master_key_bytes.data(), master_key_bytes.size());

    return WriteWholeFileAtomic(MetaPath(impl_->dir), SerializeMeta(new_meta), error);
}

std::unique_ptr<Vault> CreateVault(const std::string& path, const std::string& password, std::string& error) {
    EnsureSodiumInit();
    if (!g_sodium_ok) {
        error = "libsodium failed to initialize";
        return nullptr;
    }

    fs::path dir(path);
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        if (!fs::is_directory(dir, ec)) {
            error = path + " exists and isn't a directory";
            return nullptr;
        }
        if (!fs::is_empty(dir, ec)) {
            error = path + " already exists and isn't empty";
            return nullptr;
        }
    } else {
        if (!fs::create_directories(dir, ec)) {
            error = "couldn't create " + path + ": " + ec.message();
            return nullptr;
        }
    }
    fs::create_directories(BlobsDir(dir), ec);

    VaultMeta meta;
    randombytes_buf(meta.salt, kSaltBytes);
    // INTERACTIVE cost: see the file header's note on why (this runs
    // synchronously today - bump to MODERATE/SENSITIVE if unlock ever
    // moves to a background thread with a spinner, for a real
    // brute-force-resistance improvement).
    meta.ops_limit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
    meta.mem_limit = crypto_pwhash_MEMLIMIT_INTERACTIVE;

    unsigned char kek[kKeyBytes];
    if (!DeriveKek(password, meta.salt, meta.ops_limit, meta.mem_limit, kek, error)) {
        return nullptr;
    }

    unsigned char master_key[kKeyBytes];
    randombytes_buf(master_key, kKeyBytes);
    std::vector<uint8_t> master_key_bytes(master_key, master_key + kKeyBytes);
    meta.wrapped_key = AeadEncrypt(master_key_bytes, kek);
    sodium_memzero(kek, sizeof(kek));

    if (!WriteWholeFileAtomic(MetaPath(dir), SerializeMeta(meta), error)) {
        sodium_memzero(master_key, sizeof(master_key));
        sodium_memzero(master_key_bytes.data(), master_key_bytes.size());
        return nullptr;
    }

    auto vault = std::unique_ptr<Vault>(new Vault());
    vault->impl_->dir = dir;
    std::memcpy(vault->impl_->master_key, master_key, kKeyBytes);
    sodium_memzero(master_key, sizeof(master_key));
    sodium_memzero(master_key_bytes.data(), master_key_bytes.size());
    // Empty root - this is a fresh vault.
    if (!vault->impl_->SaveManifest(error)) {
        return nullptr;
    }
    return vault;
}

std::unique_ptr<Vault> OpenVault(const std::string& path, const std::string& password, std::string& error) {
    EnsureSodiumInit();
    if (!g_sodium_ok) {
        error = "libsodium failed to initialize";
        return nullptr;
    }

    fs::path dir(path);
    std::vector<uint8_t> meta_bytes;
    if (!ReadWholeFile(MetaPath(dir), meta_bytes, error)) {
        error = "not a vault (no vault.meta): " + path;
        return nullptr;
    }
    VaultMeta meta;
    if (!DeserializeMeta(meta_bytes, meta)) {
        error = "vault.meta is corrupt";
        return nullptr;
    }

    unsigned char kek[kKeyBytes];
    if (!DeriveKek(password, meta.salt, meta.ops_limit, meta.mem_limit, kek, error)) {
        return nullptr;
    }
    unsigned char master_key[kKeyBytes];
    {
        std::vector<uint8_t> unwrapped;
        bool ok = AeadDecrypt(meta.wrapped_key, kek, unwrapped);
        sodium_memzero(kek, sizeof(kek));
        if (!ok || unwrapped.size() != kKeyBytes) {
            error = "WRONG PASSWORD";
            return nullptr;
        }
        std::memcpy(master_key, unwrapped.data(), kKeyBytes);
        sodium_memzero(unwrapped.data(), unwrapped.size());
    }

    std::vector<uint8_t> manifest_encrypted;
    if (!ReadWholeFile(ManifestPath(dir), manifest_encrypted, error)) {
        sodium_memzero(master_key, sizeof(master_key));
        return nullptr;
    }
    std::vector<uint8_t> manifest_plain;
    if (!AeadDecrypt(manifest_encrypted, master_key, manifest_plain)) {
        // Password was right (we got a valid master key above) but
        // the manifest itself doesn't decrypt - genuine corruption,
        // not a wrong-password case.
        error = "manifest.enc is corrupt or tampered with";
        sodium_memzero(master_key, sizeof(master_key));
        return nullptr;
    }

    auto vault = std::unique_ptr<Vault>(new Vault());
    vault->impl_->dir = dir;
    std::memcpy(vault->impl_->master_key, master_key, kKeyBytes);
    sodium_memzero(master_key, sizeof(master_key));

    size_t pos = 0;
    if (!DeserializeNode(manifest_plain, pos, vault->impl_->root)) {
        error = "manifest.enc has an unreadable/corrupt tree";
        return nullptr;
    }
    return vault;
}

}  // namespace vaultstore
