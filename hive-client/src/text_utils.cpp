#include "text_utils.h"

#include <glibmm/markup.h>

#include <cctype>
#include <ctime>
#include <sstream>

namespace hive::text {

bool starts_with(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}

std::string strip_tool_transcript(const std::string& body) {
    std::string out;
    std::istringstream in(body);
    std::string line;
    int blank_run = 0;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        const auto ws = trimmed.find_first_not_of(" \t");
        if (ws != std::string::npos) trimmed = trimmed.substr(ws);
        if (starts_with(trimmed, "[tool:")) continue;
        if (starts_with(trimmed, "\xe2\x86\x92")) continue;   // "→" result line
        if (trimmed.empty()) { ++blank_run; if (blank_run > 1) continue; }
        else blank_run = 0;
        out += line;
        out += '\n';
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

std::vector<Seg> split_segments(const std::string& text) {
    std::vector<Seg> segs;
    std::istringstream in(text);
    std::string line, buf;
    bool code = false;
    auto flush = [&] {
        while (!buf.empty() && buf.back() == '\n') buf.pop_back();
        if (!buf.empty()) segs.push_back({code, buf});
        buf.clear();
    };
    while (std::getline(in, line)) {
        if (starts_with(line, "```")) {
            flush();
            code = !code;
            continue;
        }
        buf += line;
        buf += '\n';
    }
    flush();
    return segs;
}

std::string inline_markup(const std::string& line) {
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        if (starts_with(line.substr(i), "**")) {
            auto j = line.find("**", i + 2);
            if (j != std::string::npos) {
                out += "<b>" + Glib::Markup::escape_text(line.substr(i + 2, j - i - 2)) + "</b>";
                i = j + 2;
                continue;
            }
        }
        if (line[i] == '`') {
            auto j = line.find('`', i + 1);
            if (j != std::string::npos) {
                out += "<span font_family='monospace' background='#88888830'>"
                     + Glib::Markup::escape_text(line.substr(i + 1, j - i - 1)) + "</span>";
                i = j + 1;
                continue;
            }
        }
        if (line[i] == '*' && i + 1 < line.size() && line[i + 1] != '*' && line[i + 1] != ' ') {
            auto j = line.find('*', i + 1);
            if (j != std::string::npos) {
                out += "<i>" + Glib::Markup::escape_text(line.substr(i + 1, j - i - 1)) + "</i>";
                i = j + 1;
                continue;
            }
        }
        out += Glib::Markup::escape_text(std::string(1, line[i]));
        ++i;
    }
    return out;
}

std::string prose_to_markup(const std::string& text) {
    std::string out;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!first) out += '\n';
        first = false;
        if (starts_with(line, "### ")) {
            out += "<span size='large' weight='bold'>" + inline_markup(line.substr(4)) + "</span>";
        } else if (starts_with(line, "## ")) {
            out += "<span size='large' weight='bold'>" + inline_markup(line.substr(3)) + "</span>";
        } else if (starts_with(line, "# ")) {
            out += "<span size='x-large' weight='bold'>" + inline_markup(line.substr(2)) + "</span>";
        } else if (starts_with(line, "- ")) {
            out += "\xe2\x80\xa2 " + inline_markup(line.substr(2));   // "• "
        } else {
            out += inline_markup(line);
        }
    }
    return out;
}

std::string format_msg_time(const std::string& iso) {
    if (iso.size() < 16) return {};
    std::string hm = iso.substr(11, 5);
    std::string date = iso.substr(0, 10);
    // Timestamps are UTC by design — compare against the UTC date, NOT
    // localtime (pre-fix a 23:30 UTC message read at 09:30 +10 local was
    // labelled "today" only if the LOCAL date matched, i.e. wrong near
    // midnight for anyone off Greenwich).
    std::time_t t = std::time(nullptr);
    std::tm ut{};
    if (gmtime_r(&t, &ut) == nullptr) return {};
    char today[11];
    std::strftime(today, sizeof today, "%Y-%m-%d", &ut);
    if (date == today) return hm + " UTC";
    // Older: short date + time. Guard strptime — a malformed date used to
    // render as "Jan 00" from the zeroed tm.
    std::tm tm{};
    if (strptime(date.c_str(), "%Y-%m-%d", &tm) == nullptr)
        return date + " " + hm + " UTC";
    char buf[32];
    if (std::strftime(buf, sizeof buf, "%b %d", &tm) == 0)
        return date + " " + hm + " UTC";
    return std::string(buf) + ", " + hm + " UTC";
}

int palette_index(const std::string& name) {
    return (int)(std::hash<std::string>{}(name) % 6);
}

std::string agent_name_of(const nlohmann::json& agents, const std::string& id) {
    for (const auto& a : agents) {
        if (!a.is_object()) continue;
        if (a.value("id", "") == id) return a.value("name", id);
    }
    return id;
}

std::string url_escape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out += (char)ch;
        else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
    }
    return out;
}

} // namespace hive::text
