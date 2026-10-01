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
using nlohmann::json;
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
    // Wait up to 5s on a contended write lock instead of
    // failing instantly (concurrent POST bursts otherwise
    // 500 with "database is locked").
    sqlite3_busy_timeout(db, 5000);
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
    // Typed-secrets migration: api_key | login | note.
    bool has_type = false;
    st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(secrets)",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* n = col_text(st, 1);
            if (n && std::string_view{n} == "type") { has_type = true; break; }
        }
        sqlite3_finalize(st);
    }
    if (!has_type)
        exec_sql(db,
            "ALTER TABLE secrets ADD COLUMN type TEXT NOT NULL DEFAULT 'api_key'",
            "SecretStore type migration");
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
    return put_payload(name, "api_key", json{{"value", value}});
}

bool SecretStore::valid_type(const std::string& t) {
    return t == "api_key" || t == "login" || t == "note";
}

bool SecretStore::put_payload(const std::string& name, const std::string& type,
                              const json& payload) {
    if (!valid_name(name) || !valid_type(type)) return false;
    // Per-type validation. env-file delivery is line-based, so only the
    // note type may carry newlines (escaped at render time).
    const auto single_line = [](const std::string& v) {
        return !v.empty() &&
               v.find('\n') == std::string::npos &&
               v.find('\r') == std::string::npos;
    };
    if (type == "api_key") {
        if (!single_line(payload.value("value", ""))) return false;
    } else if (type == "login") {
        if (!single_line(payload.value("password", ""))) return false;
        if (!payload.value("username", "").empty() &&
            !single_line(payload.value("username", ""))) return false;
        if (!payload.value("url", "").empty() &&
            !single_line(payload.value("url", ""))) return false;
    } else {  // note
        if (payload.value("text", "").empty()) return false;
    }
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM secrets WHERE name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        const bool dup = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (dup) { sqlite3_close(db); return false; }
    }
    const std::string blob = encrypt_gcm(master_key(), payload.dump());
    if (sqlite3_prepare_v2(db,
            "INSERT INTO secrets (name, blob, created_at, type)"
            " VALUES (?,?,?,?)", -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        sqlite3_bind_blob(st, 2, blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
        bind_text(st, 3, iso_now());
        bind_text(st, 4, type);
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

std::optional<json> SecretStore::payload_of(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::optional<std::string> blob;
    std::string source, type;
    if (sqlite3_prepare_v2(db,
            "SELECT blob, source, type FROM secrets WHERE name=?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const void* p = sqlite3_column_blob(st, 0);
            blob = std::string(static_cast<const char*>(p),
                               (size_t)sqlite3_column_bytes(st, 0));
            source = col_text(st, 1);
            type = col_text(st, 2);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (type.empty()) type = "api_key";
    // Phase 22: externally-sourced secrets fetch at delivery time — the
    // value never touches the DB. Vault fetch happens AFTER close for the
    // same lock-hygiene reason as decrypt below.
    if (!source.empty()) {
        auto v = hm::vault::fetch(source);
        if (!v) return std::nullopt;
        return json{{"type", "api_key"}, {"value", *v}};
    }
    if (!blob) return std::nullopt;
    // Decrypt AFTER the connection is closed: a crypto throw (wrong key,
    // tampered blob) must not leak a connection holding a shared lock —
    // that silently SQLITE_BUSY-blocks later writes from other connections.
    const std::string plain = decrypt_gcm(master_key(), *blob);
    json payload = json::parse(plain, nullptr, false);
    if (payload.is_discarded() || !payload.is_object())
        payload = json{{"value", plain}};   // legacy raw-string blob
    payload["type"] = type;                 // injected, not stored
    return payload;
}

std::optional<std::string> SecretStore::get(const std::string& name) {
    auto p = payload_of(name);
    if (!p) return std::nullopt;
    const std::string type = p->value("type", "api_key");
    const char* key = type == "login" ? "password"
                    : type == "note"  ? "text" : "value";
    const std::string v = p->value(key, "");
    if (v.empty()) return std::nullopt;
    return v;
}

std::vector<SecretEntry> SecretStore::list() {
    sqlite3* db = open_conn();
    std::vector<SecretEntry> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name, created_at, source, type FROM secrets ORDER BY name",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            SecretEntry e;
            e.name = col_text(st, 0);
            e.created_at = col_text(st, 1);
            e.source = col_text(st, 2);
            e.type = col_text(st, 3);
            if (e.type.empty()) e.type = "api_key";
            out.push_back(std::move(e));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    // Login metadata (username/url) is display-safe but lives inside the
    // encrypted blob — decrypt just to extract it. Crypto failures (bad
    // key, tampered row) must not break listing: metadata stays empty.
    for (auto& e : out) {
        if (e.type != "login" || !e.source.empty()) continue;
        try {
            auto p = payload_of(e.name);
            if (p) {
                e.username = p->value("username", "");
                e.url = p->value("url", "");
            }
        } catch (...) {}
    }
    db = open_conn();
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
        auto p = payload_of(name);
        if (!p) continue;
        std::string upper = name;
        for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
        const std::string base = "HIVE_SECRET_" + upper;
        const std::string type = p->value("type", "api_key");
        const auto push = [&](const char* suffix, const std::string& v) {
            if (!v.empty()) out.emplace_back(base + suffix, v);
        };
        if (type == "login") {
            // Computer-use agents read all three; the browser fill tool
            // uses _USER + _PASS.
            push("_USER", p->value("username", ""));
            push("_PASS", p->value("password", ""));
            push("_URL",  p->value("url", ""));
        } else if (type == "note") {
            // env-file is line-based — escape newlines (agents read the
            // \n escapes fine). True file-mount delivery is a future
            // channel (see put() comment).
            std::string text = p->value("text", "");
            std::string esc;
            esc.reserve(text.size());
            for (char c : text) {
                if (c == '\n') esc += "\\n";
                else if (c != '\r') esc += c;
            }
            push("", esc);
        } else {
            push("", p->value("value", ""));
        }
    }
    return out;
}

} // namespace hivemind
