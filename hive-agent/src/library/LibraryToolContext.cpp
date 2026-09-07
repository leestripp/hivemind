/**
 * @file LibraryToolContext.cpp
 *
 * Phase 1 stub: minimal file ops against the configured project_root.
 * Phase 2 will route these through the library's full FileIO (which has
 * security checks like credential-pattern blocking, etc.).
 */
#include "LibraryToolContext.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hive::detail {

LibraryToolContext::LibraryToolContext(hive::AgentConfig cfg)
    : cfg_(std::move(cfg))
{
}

std::string LibraryToolContext::read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("read_file failed: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void LibraryToolContext::write_file(const std::filesystem::path& p,
                                    const std::string& content)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("write_file failed: " + p.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::vector<std::string>
LibraryToolContext::list_directory(const std::filesystem::path& p)
{
    std::vector<std::string> out;
    for (const auto& e : std::filesystem::directory_iterator(p)) {
        out.push_back(e.path().string());
    }
    return out;
}

}  // namespace hive::detail