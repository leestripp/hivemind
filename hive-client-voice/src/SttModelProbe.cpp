#include "SttModelProbe.h"

#include <cstdlib>
#include <filesystem>

namespace hv {

namespace {

void probeFile(SttModelFile& f, const std::string& dir,
               const std::string& name, std::uintmax_t minBytes) {
    namespace fs = std::filesystem;
    f.name = name;
    f.path = dir + "/" + name;
    std::error_code ec;
    const auto sz = fs::file_size(f.path, ec);  // regular files only; dirs -> error
    f.present = !ec;
    f.sizeBytes = ec ? 0 : sz;
    f.plausible = f.present && f.sizeBytes >= minBytes;
}

std::string describe(const SttModelFile& f) {
    if (!f.present) return f.name + " MISSING";
    if (!f.plausible)
        return f.name + " TRUNCATED (" + std::to_string(f.sizeBytes) +
               " bytes)";
    return f.name + " " + std::to_string(f.sizeBytes / (1024 * 1024)) +
           "MB ok";
}

}  // namespace

std::string sttModelDir() {
    const char* h = std::getenv("HOME");
    const std::string home = (h && *h) ? h : ".";
    return home + "/.local/share/hive-client-voice/models";
}

SttModelStatus probeSttModels(const std::string& dir,
                              std::uintmax_t minBytes) {
    SttModelStatus st;
    st.dir = dir;
    std::error_code ec;
    st.dirExists = std::filesystem::is_directory(dir, ec);
    probeFile(st.partial, dir, kSttModelPartial, minBytes);
    probeFile(st.final, dir, kSttModelFinal, minBytes);
    return st;
}

std::string SttModelStatus::summary() const {
    return "dir=" + dir + (dirExists ? " (exists)" : " (MISSING)") +
           " partial=" + describe(partial) + " final=" + describe(final);
}

std::string SttModelStatus::userMessage() const {
    if (ready()) return {};
    std::string msg = "Speech models not usable in " + dir + ":";
    for (const SttModelFile* f : {&partial, &final}) {
        if (f->plausible) continue;
        msg += "\n  \xe2\x80\xa2 " + f->name;
        msg += f->present ? " — truncated (" +
                                std::to_string(f->sizeBytes) +
                                " bytes, expected \xe2\x89\xa5 10 MB)"
                          : " — missing";
    }
    msg += "\nFetch them with:";
    for (const SttModelFile* f : {&partial, &final}) {
        if (f->plausible) continue;
        msg += std::string("\n  mkdir -p \"") + dir + "\" && curl -fSL -o \"" +
               f->path + "\" " + kSttModelBaseUrl + "/" + f->name;
    }
    return msg;
}

}  // namespace hv
