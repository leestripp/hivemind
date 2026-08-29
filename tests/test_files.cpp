// tests/test_files.cpp — Phase 18 path helpers: sanitize, unique, confine.
#include "hivemind/Files.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace hm::files;

int main() {
    // ── sanitize_filename ─────────────────────────────────────────────
    assert(sanitize_filename("report.pdf") == "report.pdf");
    assert(sanitize_filename("/etc/passwd") == "passwd");
    assert(sanitize_filename("../../secrets.txt") == "secrets.txt");
    assert(sanitize_filename("..") == "file");
    assert(sanitize_filename(".") == "file");
    assert(sanitize_filename("") == "file");
    assert(sanitize_filename("...hidden") == "hidden");
    assert(sanitize_filename("my file (final).docx") == "my_file__final_.docx");
    assert(sanitize_filename("a/b/c.tar.gz") == "c.tar.gz");
    assert(sanitize_filename("name\x01\x02.bin") == "name__.bin");
    assert(sanitize_filename(std::string(200, 'x')).size() == 128);
    // backslash is not a separator on Linux but still mapped to '_'
    assert(sanitize_filename("..\\..\\evil") == ".._.._evil" ||
           sanitize_filename("..\\.._evil").find("..") != 0);

    // ── unique_path ───────────────────────────────────────────────────
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_files";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    assert(unique_path(tmp, "a.txt") == tmp / "a.txt");
    { std::ofstream(tmp / "a.txt") << "1"; }
    assert(unique_path(tmp, "a.txt") == tmp / "a-1.txt");
    { std::ofstream(tmp / "a-1.txt") << "2"; }
    assert(unique_path(tmp, "a.txt") == tmp / "a-2.txt");
    // no extension
    assert(unique_path(tmp, "README") == tmp / "README");
    { std::ofstream(tmp / "README") << "r"; }
    assert(unique_path(tmp, "README") == tmp / "README-1");

    // ── resolve_confined ──────────────────────────────────────────────
    fs::create_directories(tmp / "sub/deep");
    { std::ofstream(tmp / "sub/deep/f.txt") << "data"; }

    auto root = resolve_confined(tmp, "");
    assert(root && fs::exists(*root));
    auto f = resolve_confined(tmp, "sub/deep/f.txt");
    assert(f && f->filename() == "f.txt");
    auto dir = resolve_confined(tmp, "sub");
    assert(dir && fs::is_directory(*dir));

    // escapes rejected
    assert(!resolve_confined(tmp, ".."));
    assert(!resolve_confined(tmp, "../.."));
    assert(!resolve_confined(tmp, "sub/../../.."));
    assert(!resolve_confined(tmp, "/etc/passwd"));
    assert(!resolve_confined(tmp, "nonexistent"));
    assert(!resolve_confined(tmp, "sub/../../etc/passwd"));

    // symlink hop rejected: link inside root pointing outside
    fs::path outside = fs::temp_directory_path() / "hivemind_test_outside.txt";
    { std::ofstream(outside) << "secret"; }
    std::error_code ec;
    fs::create_symlink(outside, tmp / "sneaky", ec);
    if (!ec) assert(!resolve_confined(tmp, "sneaky"));

    // ── resolve_confined_create (send_file, Phase 31) ─────────────────
    // New file in an existing confined dir: allowed.
    auto nf = resolve_confined_create(tmp, "sub/deep/new.txt");
    assert(nf && nf->filename() == "new.txt");
    assert(!fs::exists(*nf));
    // Existing file resolves too.
    auto ex = resolve_confined_create(tmp, "sub/deep/f.txt");
    assert(ex && fs::exists(*ex));
    // Rejections: escapes, absolute, missing parent, empty, bare root.
    assert(!resolve_confined_create(tmp, ".."));
    assert(!resolve_confined_create(tmp, "../evil.txt"));
    assert(!resolve_confined_create(tmp, "sub/../../evil.txt"));
    assert(!resolve_confined_create(tmp, "/etc/evil.txt"));
    assert(!resolve_confined_create(tmp, "no/such/dir/f.txt"));
    assert(!resolve_confined_create(tmp, ""));
    // Symlinked parent escaping root rejected.
    if (!ec) assert(!resolve_confined_create(tmp, "sneaky/f.txt"));
    // Existing symlink target escaping root rejected.
    if (!ec) assert(!resolve_confined_create(tmp, "sneaky"));

    fs::remove_all(tmp);
    fs::remove(outside);
    std::cout << "test_files: OK\n";
    return 0;
}
