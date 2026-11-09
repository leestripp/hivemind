#pragma once

// Shared agent second brain: human-editable Markdown is the source of truth;
// SQLite FTS5 is only a rebuildable search cache. One instance addresses one
// workspace root; separate agent processes coordinate through a file lock.

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OmniNet::DB { class Database; }

namespace hive {

struct BrainNote {
    std::string id;
    std::string folder = "inbox";
    std::string title;
    std::vector<std::string> tags;
    std::string created;
    std::string updated;
    std::vector<std::string> links;
    std::string body;
};

struct BrainNoteMeta {
    std::string id;
    std::string folder;
    std::string title;
    std::string tags;
    std::string created;
    std::string updated;
    int64_t mtime = 0;
    int64_t size = 0;
};

struct BrainSearchHit {
    BrainNoteMeta note;
    std::string snippet;
    double rank = 0.0;
};

class SecondBrain {
public:
    explicit SecondBrain(std::filesystem::path brain_root);
    ~SecondBrain();

    SecondBrain(const SecondBrain&) = delete;
    SecondBrain& operator=(const SecondBrain&) = delete;

    static const std::vector<std::string>& folders();
    static bool is_valid_folder(std::string_view folder);

    BrainNote capture(std::string_view folder, std::string_view title,
                      const std::vector<std::string>& tags,
                      std::string_view body);
    std::optional<BrainNote> read(std::string_view id) const;
    bool update(std::string_view id,
                const std::optional<std::string>& title,
                const std::optional<std::vector<std::string>>& tags,
                const std::optional<std::string>& body);
    bool append(std::string_view id, std::string_view markdown);
    bool remove(std::string_view id);
    bool move(std::string_view id, std::string_view new_folder);
    std::vector<std::string> backlinks(std::string_view id) const;
    std::vector<BrainNoteMeta> list(std::string_view folder = {},
                                    std::string_view tag = {},
                                    int limit = 100) const;
    std::vector<BrainSearchHit> search(std::string_view query,
                                       int limit = 20) const;

    struct ReindexResult { int indexed = 0; int removed = 0; };
    ReindexResult reindex();

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path folder_path(std::string_view folder) const;
    std::filesystem::path note_path(std::string_view folder,
                                    std::string_view id) const;

private:
    class FileLock;

    std::filesystem::path find_note_file(std::string_view id) const;
    std::optional<BrainNote> read_locked(std::string_view id) const;
    std::vector<std::string> backlinks_locked(std::string_view id) const;
    ReindexResult reindex_locked() const;
    void write_and_index(const BrainNote& note);

    void upsert_index(const BrainNoteMeta& row, std::string_view body) const;
    void delete_index(std::string_view id) const;
    std::vector<BrainNoteMeta> all_index() const;
    std::vector<BrainNoteMeta> list_index(std::string_view folder,
                                          int limit) const;
    std::vector<BrainSearchHit> search_index(std::string_view fts_query,
                                             int limit) const;

    static std::optional<BrainNote> parse_file(
        const std::filesystem::path& path, std::string_view folder);
    static std::vector<std::string> extract_links(std::string_view body);
    static std::string render_file(const BrainNote& note);
    static std::string slugify(std::string_view title);
    static std::string sanitise_fts_query(std::string_view query);
    static std::string now_iso8601();

    std::filesystem::path root_;
    std::unique_ptr<OmniNet::DB::Database> db_;
    mutable std::recursive_mutex mu_;
};

}  // namespace hive
