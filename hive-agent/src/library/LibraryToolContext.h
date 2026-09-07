/**
 * @file LibraryToolContext.h
 * @brief Internal adapter that exposes the library's FileIO through the
 *        public HostToolContext interface. Implementation lives in
 *        AgentImpl.cpp (Phase 2 will back it with the real FileIO).
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "types/HostToolContext.h"
#include "types/AgentConfig.h"

namespace hive::detail {

class LibraryToolContext : public hive::HostToolContext {
public:
    explicit LibraryToolContext(hive::AgentConfig cfg);

    std::string read_file(const std::filesystem::path& p) override;
    void        write_file(const std::filesystem::path& p,
                           const std::string& content) override;
    std::vector<std::string>
        list_directory(const std::filesystem::path& p) override;

    [[nodiscard]] const std::filesystem::path& project_root() const override
    {
        return cfg_.project_root;
    }

private:
    hive::AgentConfig cfg_;
};

}  // namespace hive::detail