#include "hivemind/Settings.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

Settings load_settings(int argc, char* argv[]) {
    Settings s;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--bind" || arg == "-b") {
            if (i + 1 < argc) {
                s.address = argv[++i];
            }
        } else if (arg == "--port" || arg == "-p") {
            if (i + 1 < argc) {
                try {
                    s.port = std::stoi(argv[++i]);
                } catch (const std::exception&) {
                    std::cerr << "[settings] invalid port: " << argv[i] << "\n";
                }
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: hivemind [--bind ADDR] [--port N] [--help]\n";
            std::exit(0);
        }
    }

    return s;
}

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> available_models() {
    std::vector<std::string> out;
    auto path = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."} / ".codemax" / "settings" / "settings.json";
    if (!fs::exists(path)) return out;

    try {
        auto content = read_file(path);
        auto j = nlohmann::json::parse(content);
        if (j.contains("models") && j["models"].is_object()) {
            for (auto& [name, _] : j["models"].items())
                out.push_back(name);
        }
    } catch (const std::exception& e) {
        std::cerr << "[settings] warning: could not read models from "
                  << path << ": " << e.what() << "\n";
    }
    return out;
}

bool model_exists(const std::string& name) {
    auto models = available_models();
    return std::find(models.begin(), models.end(), name) != models.end();
}
