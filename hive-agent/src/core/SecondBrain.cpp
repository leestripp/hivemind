#include "SecondBrain.h"

#include <db/Database.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using OmniNet::DB::Database;

namespace hive {
namespace {

std::string trim(std::string_view v) {
    const auto first = v.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = v.find_last_not_of(" \t\r\n");
    return std::string(v.substr(first, last - first + 1));
}

std::string to_lower(std::string_view v) {
    std::string out(v);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::vector<std::string> split_csv(std::string_view v) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= v.size()) {
        const auto comma = v.find(',', pos);
        const auto part = trim(v.substr(
            pos, comma == std::string_view::npos ? comma : comma - pos));
        if (!part.empty()) out.push_back(part);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return out;
}

std::vector<std::string> parse_tag_list(std::string_view v) {
    const std::string trimmed = trim(v);
    v = trimmed;
    if (v.size() >= 2 && v.front() == '[' && v.back() == ']')
        v = v.substr(1, v.size() - 2);
    std::vector<std::string> out;
    for (auto& tag : split_csv(v)) {
        if (tag.size() >= 2 &&
            ((tag.front() == '"' && tag.back() == '"') ||
             (tag.front() == '\'' && tag.back() == '\'')))
            tag = tag.substr(1, tag.size() - 2);
        if (!tag.empty()) out.push_back(std::move(tag));
    }
    return out;
}

std::string join_csv(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += values[i];
    }
    return out;
}

std::string yaml_quote(std::string_view v) {
    std::string out = "\"";
    for (const char c : v) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string yaml_unquote(std::string_view v) {
    const std::string trimmed = trim(v);
    v = trimmed;
    if (v.size() < 2 || v.front() != '"' || v.back() != '"')
        return std::string(v);
    std::string out;
    out.reserve(v.size() - 2);
    for (size_t i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == '\\' && i + 2 < v.size() &&
            (v[i + 1] == '"' || v[i + 1] == '\\'))
            ++i;
        out += v[i];
    }
    return out;
}

std::string read_file_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file_atomic(const fs::path& path, std::string_view text) {
    static std::atomic<uint64_t> sequence{0};
    fs::path temp = path;
    temp += ".tmp." + std::to_string(::getpid()) + "." +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    try {
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + temp.string());
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            if (!out) throw std::runtime_error("write failed: " + temp.string());
        }
        fs::rename(temp, path);
    } catch (...) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw;
    }
}

std::pair<int64_t, int64_t> file_stat(const fs::path& path) {
    const auto size = static_cast<int64_t>(fs::file_size(path));
    const auto mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(
        fs::last_write_time(path).time_since_epoch()).count();
    return {mtime, size};
}

BrainNoteMeta row_from(const std::vector<std::string>& values) {
    BrainNoteMeta row;
    if (values.size() >= 8) {
        row.id = values[0];
        row.folder = values[1];
        row.title = values[2];
        row.tags = values[3];
        row.created = values[4];
        row.updated = values[5];
        try { row.mtime = std::stoll(values[6]); } catch (...) { row.mtime = 0; }
        try { row.size = std::stoll(values[7]); } catch (...) { row.size = 0; }
    }
    return row;
}

void check_step_result(Database& db) {
    const int rc = sqlite3_errcode(db.handle());
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string("second brain SQLite step failed: ") +
                                 sqlite3_errmsg(db.handle()));
}

constexpr const char* kNoteColumns =
    "id, folder, title, tags, created, updated, mtime, size";

int bounded_limit(int limit) {
    return std::clamp(limit, 1, 1000);
}

}  // namespace

class SecondBrain::FileLock {
public:
    explicit FileLock(const fs::path& path) {
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(),
                                    "second brain: open lock " + path.string());
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            const int error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(error, std::generic_category(),
                                    "second brain: acquire lock");
        }
    }
    ~FileLock() {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            (void)::close(fd_);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
private:
    int fd_ = -1;
};

SecondBrain::SecondBrain(fs::path brain_root)
    : root_(std::move(brain_root)), db_(std::make_unique<Database>()) {
    std::error_code ec;
    for (const auto& folder : folders()) fs::create_directories(root_ / folder, ec);
    fs::create_directories(root_ / ".index", ec);
    if (ec)
        throw std::runtime_error("second brain: cannot create folders under " +
                                 root_.string() + ": " + ec.message());

    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    db_->open((root_ / ".index" / "brain.db").string());
    db_->setBusyTimeout(std::chrono::seconds(5));
    db_->enableWAL(true);
    db_->execute(
        "CREATE TABLE IF NOT EXISTS brain_notes("
        "id TEXT PRIMARY KEY, folder TEXT NOT NULL, title TEXT NOT NULL,"
        "tags TEXT NOT NULL, created TEXT NOT NULL, updated TEXT NOT NULL,"
        "mtime INTEGER NOT NULL, size INTEGER NOT NULL)");
    db_->execute(
        "CREATE VIRTUAL TABLE IF NOT EXISTS brain_fts USING fts5("
        "id UNINDEXED, title, tags, body,"
        "tokenize='porter unicode61')");

    // Version 2 adds block-sequence YAML tag parsing. Invalidate caches
    // written by version 1: their file size/mtime still match, so an ordinary
    // reconciliation would otherwise retain the incorrectly empty tags.
    auto version_stmt = db_->prepare("PRAGMA user_version");
    const int version = version_stmt.step()
                            ? static_cast<int>(version_stmt.columnInt64(0))
                            : 0;
    if (version < 2) {
        db_->transaction([&] {
            db_->execute("DELETE FROM brain_fts");
            db_->execute("DELETE FROM brain_notes");
            db_->execute("PRAGMA user_version = 2");
        });
    }
}

SecondBrain::~SecondBrain() = default;

const std::vector<std::string>& SecondBrain::folders() {
    static const std::vector<std::string> value = {
        "inbox", "projects", "areas", "resources", "archives"};
    return value;
}

bool SecondBrain::is_valid_folder(std::string_view folder) {
    const auto& values = folders();
    return std::find(values.begin(), values.end(), folder) != values.end();
}

fs::path SecondBrain::folder_path(std::string_view folder) const {
    return root_ / std::string(folder);
}

fs::path SecondBrain::note_path(std::string_view folder,
                                std::string_view id) const {
    return folder_path(folder) / (std::string(id) + ".md");
}

fs::path SecondBrain::find_note_file(std::string_view id) const {
    const bool safe = !id.empty() && std::all_of(id.begin(), id.end(),
        [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
    if (!safe) return {};
    for (const auto& folder : folders()) {
        const auto path = note_path(folder, id);
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) return path;
    }
    return {};
}

BrainNote SecondBrain::capture(std::string_view folder, std::string_view title,
                               const std::vector<std::string>& tags,
                               std::string_view body) {
    if (!is_valid_folder(folder))
        throw std::invalid_argument("second brain: invalid folder '" +
                                    std::string(folder) + "'");
    if (body.empty()) throw std::invalid_argument("second brain: body is empty");

    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();

    BrainNote note;
    note.folder = std::string(folder);
    note.title = std::string(title);
    note.tags = tags;
    note.body = std::string(body);
    note.links = extract_links(note.body);
    if (note.title.empty()) {
        std::istringstream lines(note.body);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("# ", 0) == 0) {
                note.title = trim(std::string_view(line).substr(2));
                break;
            }
        }
        if (note.title.empty()) note.title = "untitled";
    }
    note.created = now_iso8601();
    note.updated = note.created;

    const std::string base = slugify(note.title);
    note.id = base;
    for (int suffix = 2; !find_note_file(note.id).empty(); ++suffix) {
        if (suffix > 100000)
            throw std::runtime_error("second brain: cannot find free id for " + base);
        note.id = base + "-" + std::to_string(suffix);
    }
    write_and_index(note);
    return note;
}

std::optional<BrainNote> SecondBrain::read_locked(std::string_view id) const {
    const auto path = find_note_file(id);
    if (path.empty()) return std::nullopt;
    return parse_file(path, path.parent_path().filename().string());
}

std::optional<BrainNote> SecondBrain::read(std::string_view id) const {
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    return read_locked(id);
}

bool SecondBrain::update(std::string_view id,
                         const std::optional<std::string>& title,
                         const std::optional<std::vector<std::string>>& tags,
                         const std::optional<std::string>& body) {
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    auto note = read_locked(id);
    if (!note) return false;
    if (title) note->title = *title;
    if (tags) note->tags = *tags;
    if (body) note->body = *body;
    note->links = extract_links(note->body);
    note->updated = now_iso8601();
    write_and_index(*note);
    return true;
}

bool SecondBrain::append(std::string_view id, std::string_view markdown) {
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    auto note = read_locked(id);
    if (!note) return false;
    if (!note->body.empty() && note->body.back() != '\n') note->body += '\n';
    note->body += markdown;
    note->links = extract_links(note->body);
    note->updated = now_iso8601();
    write_and_index(*note);
    return true;
}

bool SecondBrain::remove(std::string_view id) {
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    const auto path = find_note_file(id);
    if (path.empty()) return false;
    std::error_code ec;
    if (!fs::remove(path, ec) || ec)
        throw std::runtime_error("second brain: cannot delete " + path.string() +
                                 (ec ? ": " + ec.message() : ""));
    delete_index(id);
    return true;
}

bool SecondBrain::move(std::string_view id, std::string_view new_folder) {
    if (!is_valid_folder(new_folder))
        throw std::invalid_argument("second brain: invalid folder '" +
                                    std::string(new_folder) + "'");
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    auto note = read_locked(id);
    if (!note) return false;
    if (note->folder == new_folder) return true;
    const auto old_path = note_path(note->folder, note->id);
    note->folder = std::string(new_folder);
    note->updated = now_iso8601();
    write_and_index(*note);
    std::error_code ec;
    fs::remove(old_path, ec);
    if (ec)
        throw std::runtime_error("second brain: cannot remove old filed note " +
                                 old_path.string() + ": " + ec.message());
    return true;
}

std::vector<std::string> SecondBrain::backlinks_locked(std::string_view id) const {
    std::vector<std::string> out;
    const std::string target(id);
    for (const auto& folder : folders()) {
        const auto dir = folder_path(folder);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".md")
                continue;
            const auto stem = entry.path().stem().string();
            if (stem == target) continue;
            const auto note = parse_file(entry.path(), folder);
            if (note && std::find(note->links.begin(), note->links.end(), target) !=
                            note->links.end())
                out.push_back(stem);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> SecondBrain::backlinks(std::string_view id) const {
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    return backlinks_locked(id);
}

std::vector<BrainNoteMeta> SecondBrain::list(std::string_view folder,
                                             std::string_view tag,
                                             int limit) const {
    if (!folder.empty() && !is_valid_folder(folder))
        throw std::invalid_argument("second brain: invalid folder '" +
                                    std::string(folder) + "'");
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    limit = bounded_limit(limit);
    auto rows = list_index(folder, tag.empty() ? limit : 0);
    if (!tag.empty()) {
        const std::string wanted(tag);
        std::erase_if(rows, [&](const BrainNoteMeta& row) {
            const auto tags = split_csv(row.tags);
            return std::find(tags.begin(), tags.end(), wanted) == tags.end();
        });
    }
    if (static_cast<int>(rows.size()) > limit) rows.resize(static_cast<size_t>(limit));
    return rows;
}

std::vector<BrainSearchHit> SecondBrain::search(std::string_view query,
                                                int limit) const {
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    (void)reindex_locked();
    const auto fts = sanitise_fts_query(query);
    if (fts.empty()) return {};
    return search_index(fts, bounded_limit(limit));
}

SecondBrain::ReindexResult SecondBrain::reindex() {
    std::lock_guard lk(mu_);
    FileLock lock(root_ / ".lock");
    return reindex_locked();
}

SecondBrain::ReindexResult SecondBrain::reindex_locked() const {
    ReindexResult result;
    std::unordered_map<std::string, BrainNoteMeta> indexed;
    for (const auto& row : all_index()) indexed.emplace(row.id, row);
    std::unordered_set<std::string> seen;

    db_->transaction([&] {
        for (const auto& folder : folders()) {
            const auto dir = folder_path(folder);
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".md")
                    continue;
                const auto stem = entry.path().stem().string();
                if (!seen.insert(stem).second) continue;  // first folder wins
                const auto [mtime, size] = file_stat(entry.path());
                const auto it = indexed.find(stem);
                if (it != indexed.end() && it->second.mtime == mtime &&
                    it->second.size == size && it->second.folder == folder)
                    continue;
                const auto note = parse_file(entry.path(), folder);
                if (!note) continue;
                BrainNoteMeta row;
                row.id = note->id;
                row.folder = note->folder;
                row.title = note->title;
                row.tags = join_csv(note->tags);
                row.created = note->created;
                row.updated = note->updated;
                row.mtime = mtime;
                row.size = size;
                upsert_index(row, note->body);
                ++result.indexed;
            }
        }
        for (const auto& [id, row] : indexed) {
            (void)row;
            if (!seen.contains(id)) {
                delete_index(id);
                ++result.removed;
            }
        }
    });
    return result;
}

void SecondBrain::write_and_index(const BrainNote& note) {
    const auto path = note_path(note.folder, note.id);
    write_file_atomic(path, render_file(note));
    const auto [mtime, size] = file_stat(path);
    BrainNoteMeta row;
    row.id = note.id;
    row.folder = note.folder;
    row.title = note.title;
    row.tags = join_csv(note.tags);
    row.created = note.created;
    row.updated = note.updated;
    row.mtime = mtime;
    row.size = size;
    db_->transaction([&] { upsert_index(row, note.body); });
}

void SecondBrain::upsert_index(const BrainNoteMeta& row,
                               std::string_view body) const {
    {
        auto stmt = db_->prepare(
            "INSERT INTO brain_notes(id, folder, title, tags, created, updated,"
            " mtime, size) VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET folder=excluded.folder,"
            "title=excluded.title, tags=excluded.tags, created=excluded.created,"
            "updated=excluded.updated, mtime=excluded.mtime, size=excluded.size");
        stmt.bind(1, row.id);
        stmt.bind(2, row.folder);
        stmt.bind(3, row.title);
        stmt.bind(4, row.tags);
        stmt.bind(5, row.created);
        stmt.bind(6, row.updated);
        stmt.bind(7, row.mtime);
        stmt.bind(8, row.size);
        (void)stmt.step();
        check_step_result(*db_);
    }
    {
        auto stmt = db_->prepare("DELETE FROM brain_fts WHERE id = ?");
        stmt.bind(1, row.id);
        (void)stmt.step();
        check_step_result(*db_);
    }
    {
        auto stmt = db_->prepare(
            "INSERT INTO brain_fts(id, title, tags, body) VALUES(?, ?, ?, ?)");
        stmt.bind(1, row.id);
        stmt.bind(2, row.title);
        stmt.bind(3, row.tags);
        stmt.bind(4, body);
        (void)stmt.step();
        check_step_result(*db_);
    }
}

void SecondBrain::delete_index(std::string_view id) const {
    {
        auto stmt = db_->prepare("DELETE FROM brain_fts WHERE id = ?");
        stmt.bind(1, std::string(id));
        (void)stmt.step();
        check_step_result(*db_);
    }
    {
        auto stmt = db_->prepare("DELETE FROM brain_notes WHERE id = ?");
        stmt.bind(1, std::string(id));
        (void)stmt.step();
        check_step_result(*db_);
    }
}

std::vector<BrainNoteMeta> SecondBrain::all_index() const {
    std::vector<BrainNoteMeta> out;
    auto stmt = db_->prepare(std::string("SELECT ") + kNoteColumns +
                             " FROM brain_notes");
    while (stmt.step()) {
        std::vector<std::string> values;
        for (int i = 0; i < stmt.columnCount(); ++i)
            values.push_back(stmt.columnText(i));
        out.push_back(row_from(values));
    }
    check_step_result(*db_);
    return out;
}

std::vector<BrainNoteMeta> SecondBrain::list_index(std::string_view folder,
                                                   int limit) const {
    std::string sql = std::string("SELECT ") + kNoteColumns + " FROM brain_notes";
    if (!folder.empty()) sql += " WHERE folder = ?";
    sql += " ORDER BY updated DESC, id ASC";
    if (limit > 0) sql += " LIMIT ?";
    auto stmt = db_->prepare(sql);
    int parameter = 1;
    if (!folder.empty()) stmt.bind(parameter++, std::string(folder));
    if (limit > 0) stmt.bind(parameter, limit);
    std::vector<BrainNoteMeta> out;
    while (stmt.step()) {
        std::vector<std::string> values;
        for (int i = 0; i < stmt.columnCount(); ++i)
            values.push_back(stmt.columnText(i));
        out.push_back(row_from(values));
    }
    check_step_result(*db_);
    return out;
}

std::vector<BrainSearchHit> SecondBrain::search_index(std::string_view fts_query,
                                                      int limit) const {
    auto stmt = db_->prepare(
        "SELECT brain_notes.id, brain_notes.folder, brain_notes.title,"
        " brain_notes.tags, brain_notes.created, brain_notes.updated,"
        " brain_notes.mtime, brain_notes.size,"
        " snippet(brain_fts, 3, '**', '**', ' … ', 12),"
        " bm25(brain_fts, 0.0, 10.0, 5.0, 1.0)"
        " FROM brain_fts JOIN brain_notes ON brain_notes.id = brain_fts.id"
        " WHERE brain_fts MATCH ? ORDER BY 10 LIMIT ?");
    stmt.bind(1, std::string(fts_query));
    stmt.bind(2, limit);
    std::vector<BrainSearchHit> out;
    while (stmt.step()) {
        std::vector<std::string> values;
        for (int i = 0; i < stmt.columnCount(); ++i)
            values.push_back(stmt.columnText(i));
        BrainSearchHit hit;
        hit.note = row_from(values);
        if (values.size() >= 10) {
            hit.snippet = values[8];
            try { hit.rank = std::stod(values[9]); } catch (...) {}
        }
        out.push_back(std::move(hit));
    }
    check_step_result(*db_);
    return out;
}

std::optional<BrainNote> SecondBrain::parse_file(const fs::path& path,
                                                 std::string_view folder) {
    std::string text;
    try {
        text = read_file_text(path);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    BrainNote note;
    note.id = path.stem().string();  // filename, never frontmatter, owns the id
    note.folder = std::string(folder);
    std::string body = text;
    if (text.rfind("---\n", 0) == 0 || text.rfind("---\r\n", 0) == 0) {
        std::istringstream lines(text);
        std::string line;
        std::getline(lines, line);
        bool closed = false;
        bool has_pending_line = false;
        std::string pending_line;
        while (has_pending_line || std::getline(lines, line)) {
            if (has_pending_line) {
                line = std::move(pending_line);
                pending_line.clear();
                has_pending_line = false;
            }
            const auto cleaned = trim(line);
            if (cleaned == "---") { closed = true; break; }
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const auto key = to_lower(trim(std::string_view(line).substr(0, colon)));
            const auto value = trim(std::string_view(line).substr(colon + 1));
            if (key == "title") note.title = yaml_unquote(value);
            else if (key == "tags") {
                note.tags = parse_tag_list(value);
                // Also accept YAML block sequences. Basic Memory rewrites
                // shared Markdown into this form while indexing it:
                //   tags:
                //   - first-tag
                //   - second-tag
                if (value.empty()) {
                    while (std::getline(lines, line)) {
                        const auto item = trim(line);
                        if (item == "---") { closed = true; break; }
                        if (!item.empty() && item.front() == '-') {
                            auto tags = parse_tag_list(item.substr(1));
                            note.tags.insert(note.tags.end(), tags.begin(), tags.end());
                            continue;
                        }
                        pending_line = line;
                        has_pending_line = true;
                        break;
                    }
                    if (closed) break;
                }
            }
            else if (key == "created") note.created = yaml_unquote(value);
            else if (key == "updated") note.updated = yaml_unquote(value);
        }
        if (closed) {
            std::ostringstream rendered;
            bool first = true;
            while (std::getline(lines, line)) {
                if (!first) rendered << '\n';
                rendered << line;
                first = false;
            }
            body = rendered.str();
            while (body.rfind("\n", 0) == 0 || body.rfind("\r", 0) == 0)
                body.erase(body.begin());
        }
    }
    if (note.title.empty()) {
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("# ", 0) == 0) {
                note.title = trim(std::string_view(line).substr(2));
                break;
            }
        }
        if (note.title.empty()) note.title = note.id;
    }
    note.body = std::move(body);
    note.links = extract_links(note.body);
    return note;
}

std::vector<std::string> SecondBrain::extract_links(std::string_view body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = body.find("[[", pos)) != std::string_view::npos) {
        const auto end = body.find("]]", pos + 2);
        if (end == std::string_view::npos) break;
        auto target = trim(body.substr(pos + 2, end - pos - 2));
        if (const auto bar = target.find('|'); bar != std::string::npos)
            target = trim(std::string_view(target).substr(0, bar));
        if (!target.empty() && std::find(out.begin(), out.end(), target) == out.end())
            out.push_back(std::move(target));
        pos = end + 2;
    }
    return out;
}

std::string SecondBrain::render_file(const BrainNote& note) {
    std::ostringstream out;
    out << "---\nid: " << note.id << "\ntitle: " << yaml_quote(note.title)
        << "\ntags: [";
    for (size_t i = 0; i < note.tags.size(); ++i) {
        if (i) out << ", ";
        const auto& tag = note.tags[i];
        const bool plain = std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        });
        out << (plain ? tag : yaml_quote(tag));
    }
    out << "]\ncreated: " << note.created << "\nupdated: " << note.updated
        << "\n---\n\n" << note.body << '\n';
    return out.str();
}

std::string SecondBrain::slugify(std::string_view title) {
    std::string out;
    out.reserve(title.size());
    bool dash = false;
    for (const unsigned char c : title) {
        if (std::isalnum(c)) {
            out += char(std::tolower(c));
            dash = false;
        } else if (!dash && !out.empty()) {
            out += '-';
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    constexpr size_t kMaxSlug = 48;
    if (out.size() > kMaxSlug) {
        out.resize(kMaxSlug);
        while (!out.empty() && out.back() == '-') out.pop_back();
    }
    return out.empty() ? "untitled" : out;
}

std::string SecondBrain::sanitise_fts_query(std::string_view query) {
    std::vector<std::string> terms;
    std::string current;
    for (const unsigned char c : query) {
        if (std::isalnum(c) || c >= 0x80) {
            current += char(std::tolower(c));
        } else if (!current.empty()) {
            terms.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) terms.push_back(std::move(current));
    std::string out;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i) out += " AND ";
        out += terms[i];
        out += '*';
    }
    return out;
}

std::string SecondBrain::now_iso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

}  // namespace hive
