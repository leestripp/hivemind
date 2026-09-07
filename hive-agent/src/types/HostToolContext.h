/**
 * @file hive-brain/HostToolContext.h
 * @brief Minimal file-I/O facade passed to host-registered tools.
 *
 * Why this exists: the library's full FileIO class lives in the
 * implementation namespace. We don't want to drag its header into the
 * public API. So hosts get this tiny interface with the operations any
 * custom tool realistically needs (read/write file, list directory,
 * grep). The library adapts it to its internal FileIO.
 *
 * Implementations may block on the underlying I/O. Host tools are
 * called on the agent's worker thread, same as built-in tools.
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hive {

class HostToolContext {
public:
    virtual ~HostToolContext() = default;

    virtual std::string read_file(const std::filesystem::path& p) = 0;
    virtual void        write_file(const std::filesystem::path& p,
                                   const std::string& content) = 0;
    virtual std::vector<std::string>
        list_directory(const std::filesystem::path& p) = 0;

    /// Project root as configured on the Agent.
    [[nodiscard]] virtual const std::filesystem::path& project_root() const = 0;
};

}  // namespace hive
