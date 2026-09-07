#include "core/FileIO.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <unordered_set>
#include <vector>
#include <cstdlib>

// Project root markers to search for
static const std::unordered_set<std::string> project_markers = {
    "CMakeLists.txt", "Makefile", "package.json", "requirements.txt",
    "Cargo.toml", "go.mod", "Pipfile", "pom.xml", "build.gradle",
    "setup.py", "pyproject.toml", "Cargo.toml"
};

std::filesystem::path FileIO::find_project_root(const std::filesystem::path& start_path) {
    std::filesystem::path current = std::filesystem::absolute(start_path);

    // If start path is a file, go to its parent directory
    if (std::filesystem::is_regular_file(current)) {
        current = current.parent_path();
    }

    // Walk up the directory tree looking for project markers
    while (true) {
        // Check if any project marker exists in current directory
        for (const auto& marker : project_markers) {
            if (std::filesystem::exists(current / marker)) {
                return current;
            }
        }

        // Also check for common top-level files
        if (std::filesystem::exists(current / "src") ||
            std::filesystem::exists(current / "include") ||
            std::filesystem::exists(current / "lib")) {
            return current;
        }

        // Move to parent directory
        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            // Reached root, return original path
            return std::filesystem::absolute(start_path);
        }
        current = parent;
    }
}

FileIO::FileIO(const std::filesystem::path& base_path,
               std::string agent_name)
    : project_path(base_path)
    , agent_name_(std::move(agent_name)) {
    // Validate that the project path exists
    if (!std::filesystem::exists(base_path)) {
        throw std::invalid_argument("Project path does not exist: " + base_path.string());
    }
    if (!std::filesystem::is_directory(base_path)) {
        throw std::invalid_argument("Project path is not a directory: " + base_path.string());
    }

    // Ensure the project path is absolute and canonical
    if (!project_path.is_absolute()) {
        project_path = std::filesystem::absolute(project_path);
    }
    project_path = std::filesystem::canonical(project_path);
}

std::string FileIO::read_file_content(const std::filesystem::path& filePath) {
    // Check if path exists - OS handles permissions
    if (!std::filesystem::exists(filePath)) {
        throw std::runtime_error("[FileIO] File does not exist: " + filePath.string());
    }

    // Check file size before reading
    const size_t max_file_size = 10 * 1024 * 1024; // 10MB limit
    try {
        auto file_size = std::filesystem::file_size(filePath);
        if (file_size > max_file_size) {
            throw std::runtime_error("[FileIO] File too large: " + std::to_string(file_size) + " bytes (max: " + std::to_string(max_file_size) + ")");
        }
    } catch (const std::filesystem::filesystem_error&) {
        // If we can't get size, try to read anyway (might be special file)
    }

    std::ifstream fileStream(filePath);
    if (!fileStream.is_open()) {
        throw std::runtime_error("[FileIO] Could not open file: " + filePath.string());
    }
    std::stringstream buffer;
    buffer << fileStream.rdbuf();
    return buffer.str();
}

void FileIO::write_file_content(const std::filesystem::path& filePath, const std::string& content) {
    // Ensure directory exists
    try {
        std::filesystem::create_directories(filePath.parent_path());
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error("[FileIO] Failed to create directories for: " + filePath.string() + " - " + e.what());
    }

    std::ofstream file(filePath);
    if (!file.is_open()) {
        throw std::runtime_error("[FileIO] Failed to write to file: " + filePath.string());
    }
    file << content;
}

void FileIO::create_directory(const std::filesystem::path& dirPath) {
    // Create the directory - OS handles permissions
    try {
        if (std::filesystem::exists(dirPath)) {
            if (!std::filesystem::is_directory(dirPath)) {
                throw std::runtime_error("[FileIO] Path exists but is not a directory: " + dirPath.string());
            }
            // Directory already exists, that's fine
            return;
        }
        std::filesystem::create_directories(dirPath);
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error("[FileIO] Failed to create directory: " + dirPath.string() + " - " + e.what());
    }
}

// Getters
const std::filesystem::path& FileIO::get_project_path() const {
    return project_path;
}

std::vector<std::string> FileIO::get_file_list() const {
    std::vector<std::string> file_paths;

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(project_path,
            std::filesystem::directory_options::skip_permission_denied)) {

            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            std::filesystem::path parent_path = entry.path().parent_path();

            // Skip ignored files
            file_paths.push_back(entry.path().string());
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error("File system error while scanning directory: " + std::string(e.what()));
    }

    // Sort for consistent output
    std::sort(file_paths.begin(), file_paths.end());

    return file_paths;
}

// Helper function to count lines in a file efficiently
static size_t count_file_lines(const std::filesystem::path& file_path) {
    try {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return 0;
        }
        // Count newlines by seeking to end and reading
        file.seekg(0, std::ios::end);
        if (!file.good()) {
            return 0;
        }
        std::streampos file_size = file.tellg();
        if (file_size == 0) {
            return 0; // Empty file
        }
        file.seekg(0, std::ios::beg);

        // For small files, just count newlines directly
        if (file_size < 1024 * 1024) { // Less than 1MB
            size_t line_count = 0;
            char ch;
            while (file.get(ch)) {
                if (ch == '\n') {
                    line_count++;
                }
            }
            // If file doesn't end with newline, count last line
            if (file_size > 0 && !file.eof()) {
                line_count++;
            }
            return line_count;
        }

        // For larger files, sample every 1KB and estimate
        size_t estimated_lines = 0;
        size_t bytes_read = 0;
        const size_t sample_size = 1024;
        file.seekg(0, std::ios::beg);
        while (file.peek() != EOF) {
            file.ignore(sample_size, '\n');
            if (file.gcount() > 0) {
                estimated_lines++;
                bytes_read += file.gcount();
            }
        }
        // Scale up based on total file size
        if (bytes_read > 0) {
            return (estimated_lines * file_size) / bytes_read;
        }
        return estimated_lines;
    } catch (...) {
        return 0;
    }
}

std::string FileIO::generate_project_tree() const {
    std::ostringstream tree_stream;
    tree_stream << "Project structure:\n";

    try {
        // Store all paths to process
        std::vector<std::filesystem::path> dirs_to_process;
        std::vector<std::filesystem::path> files_to_process;

        // Collect directories and files
        for (const auto& entry : std::filesystem::recursive_directory_iterator(project_path,
            std::filesystem::directory_options::skip_permission_denied)) {

            if (!entry.is_directory() && !entry.is_regular_file()) {
                continue;
            }

            std::string filename = entry.path().filename().string();

            // Add to appropriate collection
            if (entry.is_directory()) {
                dirs_to_process.push_back(entry.path());
            } else {
                // Include all regular files (no extension filtering)
                files_to_process.push_back(entry.path());
            }
        }

        // Sort paths for consistent output
        std::sort(dirs_to_process.begin(), dirs_to_process.end());
        std::sort(files_to_process.begin(), files_to_process.end());

        // Generate directory tree structure with proper indentation
        for (const auto& dir_path : dirs_to_process) {
            int depth = 0;
            auto current_path = dir_path;

            // Calculate depth relative to target directory
            while (current_path != project_path && !current_path.empty()) {
                depth++;
                current_path = current_path.parent_path();
            }

            std::string indent(depth * 2, ' ');
            std::string dir_name = dir_path.filename().string();
            if (!dir_name.empty()) {
                tree_stream << indent << "├── " << dir_name << "/\n";
            }
        }

        // Add files with proper indentation
        for (const auto& file_path : files_to_process) {
            int depth = 0;
            auto current_path = file_path.parent_path();

            // Calculate depth relative to target directory
            while (current_path != project_path && !current_path.empty()) {
                depth++;
                current_path = current_path.parent_path();
            }

            std::string indent(depth * 2, ' ');
            std::string filename = file_path.filename().string();
            size_t line_count = count_file_lines(file_path);
            tree_stream << indent << "├── " << filename << " (" << line_count << " lines)\n";
        }

    } catch (const std::filesystem::filesystem_error& e) {
        tree_stream << "[Error generating tree: " << e.what() << "]\n";
    }

    return tree_stream.str();
}

std::string FileIO::generate_directory_tree(const std::filesystem::path& target_path, int max_depth) const {
    std::ostringstream tree_stream;

    // Resolve the target path
    std::filesystem::path resolved_path;
    if (target_path.is_absolute()) {
        resolved_path = std::filesystem::absolute(target_path);
    } else {
        resolved_path = std::filesystem::absolute(project_path / target_path);
    }

    // Check if path exists and is a directory - OS handles permissions
    if (!std::filesystem::exists(resolved_path)) {
        return "[Error: Directory does not exist: " + resolved_path.string() + "]\n";
    }

    if (!std::filesystem::is_directory(resolved_path)) {
        return "[Error: Path is not a directory: " + resolved_path.string() + "]\n";
    }

    tree_stream << "Directory tree: " << resolved_path.string() << "\n";

    try {
        // Recursive lambda to generate tree
        std::function<void(const std::filesystem::path&, int)> generate_tree =
            [&](const std::filesystem::path& current_path, int depth) {
                // Check max depth if specified
                if (max_depth >= 0 && depth > max_depth) {
                    return;
                }

                // Get directory contents
                std::vector<std::filesystem::path> dirs;
                std::vector<std::filesystem::path> files;

                try {
                    for (const auto& entry : std::filesystem::directory_iterator(current_path,
                            std::filesystem::directory_options::skip_permission_denied)) {
                        std::string filename = entry.path().filename().string();

                        // Skip hidden files/directories (starting with .)
                        if (!filename.empty() && filename[0] == '.') {
                            continue;
                        }

                        if (entry.is_directory()) {
                            dirs.push_back(entry.path());
                        } else if (entry.is_regular_file()) {
                            // Include all regular files (no extension filtering)
                            files.push_back(entry.path());
                        }
                    }
                } catch (const std::filesystem::filesystem_error&) {
                    // Skip directories we can't read
                    return;
                }

                // Sort for consistent output
                std::sort(dirs.begin(), dirs.end());
                std::sort(files.begin(), files.end());

                // Calculate indent
                std::string indent(depth * 2, ' ');

                // Show directories first
                for (size_t i = 0; i < dirs.size(); i++) {
                    bool is_last = (i == dirs.size() - 1) && files.empty();
                    tree_stream << indent << (is_last ? "└── " : "├── ") << dirs[i].filename().string() << "/\n";
                    generate_tree(dirs[i], depth + 1);
                }

                // Then files
                for (size_t i = 0; i < files.size(); i++) {
                    bool is_last = (i == files.size() - 1);
                    std::string filename = files[i].filename().string();
                    std::string line_count_str = "";
                    try {
                        size_t line_count = count_file_lines(files[i]);
                        line_count_str = " (" + std::to_string(line_count) + " lines)";
                    } catch (...) {
                        // Ignore errors getting line count
                    }
                    tree_stream << indent << (is_last ? "└── " : "├── ") << filename << line_count_str << "\n";
                }
            };

        generate_tree(resolved_path, 0);

    } catch (const std::filesystem::filesystem_error& e) {
        tree_stream << "[Error generating tree: " << e.what() << "]\n";
    }

    return tree_stream.str();
}