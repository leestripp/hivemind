#pragma once

#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

using json = nlohmann::json;

class FileIO {
private:
    std::filesystem::path project_path;
    std::string agent_name_;

public:
    // Find project root by looking for project markers
    static std::filesystem::path find_project_root(const std::filesystem::path& start_path);

    explicit FileIO(const std::filesystem::path& base_path,
                    std::string agent_name = {});

    // File operations - simplified model (trust Linux permissions):
    // - Read: any accessible file
    // - Write: any accessible location (OS handles permissions)
    // - Delete: any accessible file (OS handles permissions)

    // Read a file - throws if not accessible or too large
    std::string read_file_content(const std::filesystem::path& filePath);

    // Write a file - throws if not accessible
    void write_file_content(const std::filesystem::path& filePath, const std::string& content);

    // Create directory - throws if not accessible
    void create_directory(const std::filesystem::path& dirPath);

    // File listing and tree generation
    std::vector<std::string> get_file_list() const;
    std::string generate_project_tree() const;
    std::string generate_directory_tree(const std::filesystem::path& target_path, int max_depth = -1) const;

    // Getters
    const std::filesystem::path& get_project_path() const;
    const std::string& get_agent_name() const noexcept { return agent_name_; }
};