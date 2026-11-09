#include "SpeechText.h"

#include <cctype>

namespace speech {
namespace {

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'';
}

std::string lowerAscii(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s)
        r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string collapseWs(std::string s) {
    std::string r;
    r.reserve(s.size());
    bool pending = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) { pending = true; continue; }
        if (pending && !r.empty()) r += ' ';
        pending = false;
        r += c;
    }
    return r;
}

bool startsWithAt(std::string_view s, size_t i, std::string_view pat) {
    return i + pat.size() <= s.size() && s.substr(i, pat.size()) == pat;
}

void pushClause(std::vector<Clause>& out, std::string& buf, bool terminal) {
    std::string t = collapseWs(buf);
    buf.clear();
    if (t.empty()) return;
    if (!out.empty() && !out.back().terminal) {
        out.back().text += " " + t;
        out.back().terminal = terminal;
    } else {
        out.push_back({std::move(t), terminal});
    }
}

std::string digitsToWords(std::string_view digits) {
    static const char* kOnes[] = {"",        "one",       "two",      "three",
                                  "four",    "five",      "six",      "seven",
                                  "eight",   "nine",      "ten",      "eleven",
                                  "twelve",  "thirteen",  "fourteen", "fifteen",
                                  "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* kTens[] = {"",      "",       "twenty", "thirty",
                                  "forty", "fifty",  "sixty",  "seventy",
                                  "eighty","ninety"};
    if (digits.size() > 9) return std::string(digits);
    long long v = 0;
    for (char c : digits) v = v * 10 + (c - '0');
    auto three = [&](int n) {
        std::string s;
        int h = n / 100;
        if (h) { s += kOnes[h]; s += " hundred"; }
        int rest = n % 100;
        if (rest) {
            if (!s.empty()) s += ' ';
            if (rest < 20) s += kOnes[rest];
            else {
                s += kTens[rest / 10];
                if (rest % 10) { s += ' '; s += kOnes[rest % 10]; }
            }
        }
        return s;
    };
    std::string r;
    long long billions = v / 1000000000; v %= 1000000000;
    long long millions = v / 1000000;    v %= 1000000;
    long long thousands = v / 1000;      v %= 1000;
    if (billions) r += three(static_cast<int>(billions)) + " billion";
    if (millions) { if (!r.empty()) r += ' '; r += three(static_cast<int>(millions)) + " million"; }
    if (thousands) { if (!r.empty()) r += ' '; r += three(static_cast<int>(thousands)) + " thousand"; }
    if (v || r.empty()) { if (!r.empty()) r += ' '; r += three(static_cast<int>(v)); }
    return r;
}

}  // namespace

std::string spokenForm(std::string_view tok) {
    std::string t{tok};
    if (t == "..." || t == "…" || t == "。。。") return " ... ";
    if (t == "~") return " ";
    if (t == "--" || t == "—" || t == "–") return " — ";
    if (t == "\\(" || t == "\\[" || t == "\\{" || t == "\\\"" || t == "\\'")
        return " ";
    if (t == "\\)" || t == "\\]" || t == "\\}" || t == "\\\\" || t == "\\" ||
        t == "$" || t == "\\$" || t == "`" || t == "``" || t == "```" ||
        t == "'" || t == "\"" || t == "''" || t == "*" || t == "**" ||
        t == "***" || t == "_" || t == "__" || t == "#" || t == "##" ||
        t == "###" || t == ">" || t == "-" || t == "+")
        return "";
    if (t.size() == 1 && t[0] == '%') return " percent";
    std::string low = lowerAscii(t);
    if (low == "e.g.") return " for example ";
    if (low == "i.e.") return " that is ";
    if (low == "etc.") return " etcetera ";
    if (low == "vs.") return " versus ";
    if (low == "approx.") return " approximately ";
    if (low == "ca.") return " circa ";
    if (low == "n/a" || low == "n.a.") return " not applicable ";
    if (low == "ok") return " okay ";
    if (low == "b/c") return " because ";
    if (low == "w/o") return " without ";
    if (low == "w/" || low == "w//") return " with ";
    if (low == "b4") return " before ";
    if (t == "°c" || t == "℃") return " degrees Celsius ";
    if (t == "°f" || t == "℉") return " degrees Fahrenheit ";
    if (t == "°") return " degrees ";
    if (t.size() == 1 && std::isdigit(static_cast<unsigned char>(t[0])))
        return digitsToWords(t);
    return t;
}

std::vector<Clause> splitClauses(std::string_view raw) {
    std::vector<Clause> out;
    std::string buf;
    const size_t n = raw.size();
    auto flush = [&] { pushClause(out, buf, false); };
    for (size_t i = 0; i < n;) {
        char c = raw[i];
        if (startsWithAt(raw, i, "```")) {
            size_t nl = raw.find('\n', i);
            if (nl == std::string_view::npos) break;
            size_t end = raw.find("```", nl + 1);
            i = (end == std::string_view::npos) ? n : end + 3;
            flush();
            continue;
        }
        if (c == '$' || c == '~' || c == '^' || c == '\\' ||
            (c == '[' && i + 1 < n && raw[i + 1] == '^') ||
            (i + 3 <= n && raw.compare(i, 3, "\xE3\x80\x90") == 0)) {
            size_t j = i + 1;
            while (j < n && !std::isspace(static_cast<unsigned char>(raw[j])) &&
                   !std::ispunct(static_cast<unsigned char>(raw[j])))
                ++j;
            std::string sp = spokenForm(raw.substr(i, j - i));
            if (sp.empty()) flush(); else buf += sp;
            i = j;
            continue;
        }
        if (startsWithAt(raw, i, "http://") || startsWithAt(raw, i, "https://")) {
            size_t j = i;
            while (j < n && !std::isspace(static_cast<unsigned char>(raw[j]))) ++j;
            i = j;
            flush();
            continue;
        }
        if (c == '<') {
            size_t j = i;
            while (j < n && raw[j] != '>') ++j;
            i = (j < n) ? j + 1 : n;
            flush();
            continue;
        }
        if (startsWithAt(raw, i, "…")) {
            buf += " ... "; pushClause(out, buf, true); ++i; continue;
        }
        if (startsWithAt(raw, i, ". . .")) {
            buf += " ... "; pushClause(out, buf, true); i += 5; continue;
        }
        if (startsWithAt(raw, i, "。。。")) {
            buf += " ... "; pushClause(out, buf, true); i += 3; continue;
        }
        const bool cjkTerm =
            (i + 3 <= n &&
             (raw.compare(i, 3, "\xE3\x80\x82") == 0 ||  // 。
              raw.compare(i, 3, "\xEF\xBC\x81") == 0 ||  // ！
              raw.compare(i, 3, "\xEF\xBC\x9F") == 0 ||  // ？
              raw.compare(i, 3, "\xEF\xBC\x9B") == 0 ||  // ；
              raw.compare(i, 3, "\xEF\xBC\x9A") == 0));  // ：
        if (c == '.' || c == '!' || c == '?' || c == ';' || c == ':' ||
            cjkTerm) {
            if (cjkTerm) {
                buf.append(raw, i, 3);
                i += 3;
            } else {
                buf += c;
                ++i;
            }
            pushClause(out, buf, true);
            continue;
        }
        const bool cjkComma =
            (i + 3 <= n && raw.compare(i, 3, "\xEF\xBC\x8C") == 0);  // ，
        if (c == ',' || cjkComma) {
            pushClause(out, buf, false);
            i += cjkComma ? 3 : 1;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) &&
            (i == 0 || !isWordChar(raw[i - 1]))) {
            size_t j = i;
            while (j < n && std::isdigit(static_cast<unsigned char>(raw[j]))) ++j;
            if (j == n || !isWordChar(raw[j])) {
                buf += digitsToWords(raw.substr(i, j - i));
                i = j;
                continue;
            }
        }
        buf += c;
        ++i;
    }
    pushClause(out, buf, true);
    return out;
}

std::vector<std::string> toSentences(const std::vector<Clause>& clauses) {
    std::vector<std::string> out;
    std::string acc;
    auto emit = [&] {
        std::string t = collapseWs(acc);
        acc.clear();
        if (!t.empty()) out.push_back(std::move(t));
    };
    for (const auto& c : clauses) {
        acc += " " + c.text;
        if (c.terminal || acc.size() >= 140) emit();
    }
    emit();
    return out;
}

std::vector<std::string> toSpeechSentences(std::string_view raw) {
    return toSentences(splitClauses(raw));
}

std::string forSpeech(std::string_view raw) {
    const auto sentences = toSpeechSentences(raw);
    std::string out;
    for (const auto& s : sentences) {
        if (!out.empty()) out += ' ';
        out += s;
    }
    return out;
}

std::vector<std::string> toSpeechChunks(const std::string_view raw,
                                        const std::size_t maxBytes) {
    std::vector<std::string> out;
    if (maxBytes == 0) return out;
    const auto continuation = [](const char c) {
        return (static_cast<unsigned char>(c) & 0xc0) == 0x80;
    };
    for (const auto& sentence : toSpeechSentences(raw)) {
        std::string rest = sentence;
        while (rest.size() > maxBytes) {
            std::size_t cut = rest.rfind(' ', maxBytes);
            if (cut == std::string::npos || cut < maxBytes / 2) {
                // No useful word boundary: move left to the start of the
                // UTF-8 sequence containing the byte budget's endpoint.
                cut = maxBytes;
                while (cut > 0 && continuation(rest[cut])) --cut;
                if (cut == 0) {
                    // A code point wider than the budget must remain whole.
                    cut = maxBytes;
                    while (cut < rest.size() && continuation(rest[cut])) ++cut;
                }
            }
            out.push_back(rest.substr(0, cut));
            rest = collapseWs(rest.substr(cut));
        }
        if (!rest.empty()) out.push_back(std::move(rest));
    }
    return out;
}

std::size_t commonPrefixLen(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

std::size_t finalNarrationCredit(const std::string_view final_text,
                                 const std::string_view narrated_live) {
    if (final_text.empty()) return 0;

    // KMP tracks the longest FINAL prefix ending at the current LIVE byte.
    // Unlike a common-prefix comparison, this covers live progress text
    // before the stored final message. Reaching final_text.size() means the
    // whole final message was already queued, even if more live text follows.
    std::vector<std::size_t> fallback(final_text.size(), 0);
    for (std::size_t i = 1, matched = 0; i < final_text.size(); ++i) {
        while (matched > 0 && final_text[i] != final_text[matched])
            matched = fallback[matched - 1];
        if (final_text[i] == final_text[matched]) ++matched;
        fallback[i] = matched;
    }

    std::size_t matched = 0;
    for (const char ch : narrated_live) {
        while (matched > 0 && ch != final_text[matched])
            matched = fallback[matched - 1];
        if (ch == final_text[matched]) ++matched;
        if (matched == final_text.size()) return matched;
    }
    return matched;
}

}  // namespace speech
