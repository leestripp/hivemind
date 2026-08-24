#include "hivemind/SecretStore.h"

#include "hivemind/Vault.h"

#include "hivemind/DbUtil.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>

using namespace hivemind::db;
namespace fs = std::filesystem;

namespace hivemind {

namespace {

/// AES-256-GCM: blob = 12-byte IV || ciphertext || 16-byte tag.
std::string encrypt_gcm(const std::string& key, const std::string& plain) {
    unsigned char iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    std::string out(sizeof(iv) + plain.size() + 16, '\0');
    int len = 0, total = 0;
    unsigned char* ct = reinterpret_cast<unsigned char*>(&out[12]);
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           iv) == 1 &&
        EVP_EncryptUpdate(ctx, ct, &len,
                          reinterpret_cast<const unsigned char*>(plain.data()),
                          (int)plain.size()) == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, ct + total, &len) == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
                                   &out[12 + plain.size()]) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) throw std::runtime_error("AES-GCM encrypt failed");
    std::memcpy(&out[0], iv, sizeof(iv));
    out.resize(12 + total + 16);
    return out;
}

std::string decrypt_gcm(const std::string& key, const std::string& blob) {
    if (blob.size() < 12 + 16)
        throw std::runtime_error("secret blob too short");
    const auto* iv  = reinterpret_cast<const unsigned char*>(blob.data());
    const auto* ct  = iv + 12;
    const int ctlen = (int)blob.size() - 12 - 16;
    const auto* tag = ct + ctlen;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    std::string plain(ctlen, '\0');
    int len = 0, total = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           iv) == 1 &&
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&plain[0]),
                          &len, ct, ctlen) == 1;
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                   const_cast<unsigned char*>(tag)) == 1;
    // GCM: final returns > 0 only if the tag verifies — tamper/wrong key fails.
    ok = ok && EVP_DecryptFinal_ex(ctx, nullptr, &len) > 0;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) throw std::runtime_error("AES-GCM decrypt failed (bad key/blob)");
    plain.resize(total);
    return plain;
}

std::string hex_encode(const unsigned char* p, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2*i]   = H[p[i] >> 4];
        s[2*i+1] = H[p[i] & 15];
    }
    return s;
}

std::string hex_decode(const std::string& s) {
    std::string out(s.size() / 2, '\0');
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out[i/2] = (char)(std::stoi(s.substr(i, 2), nullptr, 16));
    return out;
}

} // namespace

SecretStore::SecretStore() {
    const char* home = std::getenv("HOME");
    fs::path base = fs::path(home ? home : ".") / ".local/share/hivemind";
    db_path_  = base / "secrets.db";
    key_path_ = base / "secrets.key";
    fs::create_directories(base);
}

SecretStore::SecretStore(fs::path db, fs::path key_file)
    : db_path_(std::move(db)),
      key_path_(key_file.empty() ? db_path_.parent_path() / "secrets.key"
                                 : std::move(key_file)) {
    fs::create_directories(db_path_.parent_path());
}

bool SecretStore::valid_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    if (!std::isalpha((unsigned char)n[0]) && n[0] != '_') return false;
    for (unsigned char c : n)
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

const std::string& SecretStore::master_key() {
    if (!master_key_.empty()) return master_key_;
    std::ifstream f(key_path_, std::ios::binary);
    if (f) {
        std::string hex((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        while (!hex.empty() && (hex.back() == '\n' || hex.back() == ' '))
            hex.pop_back();
        if (hex.size() != 64)
            throw std::runtime_error("secrets.key corrupt (want 64 hex chars)");
        master_key_ = hex_decode(hex);
        return master_key_;
    }
    // First use: generate + persist 0600 (auth.db trust model).
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        throw std::runtime_error("RAND_bytes failed for master key");
    master_key_.assign(reinterpret_cast<char*>(buf), sizeof(buf));
    {
        std::ofstream kf(key_path_, std::ios::binary | std::ios::trunc);
        kf << hex_encode(buf, sizeof(buf)) << "\n";
    }
    ::chmod(key_path_.c_str(), 0600);
    return master_key_;
}

sqlite3* SecretStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        throw std::runtime_error("SecretStore: cannot open db");
    }
    ensure_schema(db);
    return db;
}

void SecretStore::ensure_schema(sqlite3* db) {
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS secrets ("
        "  name TEXT PRIMARY KEY,"
        "  blob BLOB NOT NULL,"
        "  created_at TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS grants ("
        "  secret_name TEXT NOT NULL,"
        "  agent_id TEXT NOT NULL,"
        "  PRIMARY KEY (secret_name, agent_id));",
        "SecretStore schema");
    // Phase 22 migration: external vault source on pre-existing DBs.
    bool has_src = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(secrets)",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* n = col_text(st, 1);
            if (n && std::string_view{n} == "source") { has_src = true; break; }
        }
        sqlite3_finalize(st);
    }
    if (!has_src)
        exec_sql(db,
            "ALTER TABLE secrets ADD COLUMN source TEXT NOT NULL DEFAULT ''",
            "SecretStore source migration");
}

bool SecretStore::put(const std::string& name, const std::string& value) {
    if (!valid_name(name) || value.empty()) return false;
    // podman --env-file is line-based; multiline secrets (PEMs) need a
    // future file-mount channel — reject here rather than corrupt at exec.
    if (value.find('\n') != std::string::npos ||
        value.find('\r') != std::string::npos) return false;
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM secrets WHERE name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        const bool dup = [&] {
            bind_text(st, 1, name);
            return sqlite3_step(st) == SQLITE_ROW;
        }();
        sqlite3_finalize(st);
        if (dup) { sqlite3_close(db); return false; }
    }
    const std::string blob = encrypt_gcm(master_key(), value);
    if (sqlite3_prepare_v2(db,
            "INSERT INTO secrets (name, blob, created_at) VALUES (?,?,?)",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        sqlite3_bind_blob(st, 2, blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
        bind_text(st, 3, iso_now());
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        sqlite3_close(db);
        return ok;
    }
    sqlite3_close(db);
    return false;
}

bool SecretStore::put_external(const std::string& name,
                               const std::string& source) {
    if (!valid_name(name)) return false;
    if (hm::vault::validate_source(source) != "") return false;
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM secrets WHERE name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        const bool dup = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (dup) { sqlite3_close(db); return false; }
    }
    // blob column is NOT NULL — external rows store an empty blob.
    if (sqlite3_prepare_v2(db,
            "INSERT INTO secrets (name, blob, created_at, source)"
            " VALUES (?,?,?,?)", -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        sqlite3_bind_blob(st, 2, "", 0, SQLITE_STATIC);
        bind_text(st, 3, iso_now());
        bind_text(st, 4, source);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        sqlite3_close(db);
        return ok;
    }
    sqlite3_close(db);
    return false;
}

std::optional<std::string> SecretStore::get(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::optional<std::string> blob;
    std::string source;
    if (sqlite3_prepare_v2(db, "SELECT blob, source FROM secrets WHERE name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const void* p = sqlite3_column_blob(st, 0);
            blob = std::string(static_cast<const char*>(p),
                               (size_t)sqlite3_column_bytes(st, 0));
            source = col_text(st, 1);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    // Phase 22: externally-sourced secrets fetch at delivery time — the
    // value never touches the DB. Vault fetch happens AFTER close for the
    // same lock-hygiene reason as decrypt below.
    if (!source.empty()) return hm::vault::fetch(source);
    // Decrypt AFTER the connection is closed: a crypto throw (wrong key,
    // tampered blob) must not leak a connection holding a shared lock —
    // that silently SQLITE_BUSY-blocks later writes from other connections.
    if (blob) return decrypt_gcm(master_key(), *blob);
    return std::nullopt;
}

std::vector<SecretEntry> SecretStore::list() {
    sqlite3* db = open_conn();
    std::vector<SecretEntry> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name, created_at, source FROM secrets ORDER BY name",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            SecretEntry e;
            e.name = col_text(st, 0);
            e.created_at = col_text(st, 1);
            e.source = col_text(st, 2);
            out.push_back(std::move(e));
        }
        sqlite3_finalize(st);
    }
    for (auto& e : out) {
        if (sqlite3_prepare_v2(db,
                "SELECT agent_id FROM grants WHERE secret_name=?",
                -1, &st, nullptr) == SQLITE_OK) {
            bind_text(st, 1, e.name);
            while (sqlite3_step(st) == SQLITE_ROW)
                e.agent_ids.push_back(col_text(st, 0));
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    return out;
}

bool SecretStore::remove(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool existed = false;
    if (sqlite3_prepare_v2(db, "DELETE FROM secrets WHERE name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        sqlite3_step(st);
        existed = sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM grants WHERE secret_name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return existed;
}

bool SecretStore::exists(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM secrets WHERE name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        found = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return found;
}

bool SecretStore::grant(const std::string& name, const std::string& agent_id) {
    if (!exists(name)) return false;
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO grants (secret_name, agent_id) VALUES (?,?)",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        bind_text(st, 2, agent_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return true;
}

bool SecretStore::revoke(const std::string& name, const std::string& agent_id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool removed = false;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM grants WHERE secret_name=? AND agent_id=?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        bind_text(st, 2, agent_id);
        sqlite3_step(st);
        removed = sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return removed;
}

std::vector<std::string>
SecretStore::grants_for_agent(const std::string& agent_id) {
    sqlite3* db = open_conn();
    std::vector<std::string> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT secret_name FROM grants WHERE agent_id=? ORDER BY secret_name",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, agent_id);
        while (sqlite3_step(st) == SQLITE_ROW)
            out.push_back(col_text(st, 0));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<std::pair<std::string, std::string>>
SecretStore::env_for_agent(const std::string& agent_id) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& name : grants_for_agent(agent_id)) {
        auto v = get(name);
        if (v) {
            std::string upper = name;
            for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
            out.emplace_back("HIVE_SECRET_" + upper, *v);
        }
    }
    return out;
}

} // namespace hivemind
