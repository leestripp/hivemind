// src/Files.cpp — shared-workspace path helpers (Phase 18).
#include "hivemind/Files.h"

#include <cctype>

namespace fs = std::filesystem;

namespace hm::files {

std::string sanitize_filename(const std::string& untrusted) {
    // Bare filename only — any directory portion is dropped.
    std::string name = fs::path(untrusted).filename().string();
    std::string out;
    out.reserve(name.size());
    for (unsigned char ch : name) {
        const bool ok = std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
        out += ok ? static_cast<char>(ch) : '_';
    }
    // No hidden files, no "." / ".." — strip leading dots.
    const auto first = out.find_first_not_of('.');
    out = (first == std::string::npos) ? "" : out.substr(first);
    if (out.empty()) out = "file";
    if (out.size() > 128) out = out.substr(0, 128);
    return out;
}

fs::path unique_path(const fs::path& dir, const std::string& name) {
    fs::path p = dir / name;
    if (!fs::exists(p)) return p;
    const fs::path orig{name};
    const std::string stem = orig.stem().string();
    const std::string ext  = orig.extension().string();
    for (int i = 1; i < 1000; ++i) {
        p = dir / (stem + "-" + std::to_string(i) + ext);
        if (!fs::exists(p)) return p;
    }
    return dir / (stem + "-" + std::to_string(
        std::hash<std::string>{}(name) & 0xffffff) + ext);
}

std::optional<fs::path> resolve_confined(const fs::path& root,
                                         const std::string& rel) {
    if (rel.empty()) return fs::weakly_canonical(root);
    const fs::path rp{rel};
    if (rp.is_absolute()) return std::nullopt;

    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec);
    if (ec) return std::nullopt;
    const fs::path target = fs::weakly_canonical(base / rp, ec);
    if (ec || !fs::exists(target)) return std::nullopt;

    // Prefix check on the canonical paths — defeats ".." and symlink hops.
    auto b = base.begin();
    auto t = target.begin();
    for (; b != base.end(); ++b, ++t)
        if (t == target.end() || *t != *b) return std::nullopt;
    return target;
}

std::optional<fs::path> resolve_confined_create(const fs::path& root,
                                                const std::string& rel) {
    if (rel.empty()) return std::nullopt;   // need a real filename
    const fs::path rp{rel};
    if (rp.is_absolute()) return std::nullopt;

    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec);
    if (ec) return std::nullopt;

    const fs::path target = base / rp;
    // Confine the PARENT (must exist); the leaf may be created.
    const fs::path parent = fs::weakly_canonical(target.parent_path(), ec);
    if (ec || !fs::exists(parent)) return std::nullopt;

    auto b = base.begin();
    auto ppar = parent.begin();
    for (; b != base.end(); ++b, ++ppar)
        if (ppar == parent.end() || *ppar != *b) return std::nullopt;

    // If the target already exists, confine it too (symlink defence).
    if (fs::exists(target)) {
        const fs::path ct = fs::weakly_canonical(target, ec);
        if (ec) return std::nullopt;
        b = base.begin();
        auto t = ct.begin();
        for (; b != base.end(); ++b, ++t)
            if (t == ct.end() || *t != *b) return std::nullopt;
        return ct;
    }
    return parent / rp.filename();
}

} // namespace hm::files
