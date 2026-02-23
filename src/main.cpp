#include <algorithm>
#include <cctype>
#include <clocale>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <set>
#include <map>
#include <unordered_map>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>
#include <wchar.h>
#include <wctype.h>
#include <cwctype>

#ifndef BOOKR_VERSION_STR
#define BOOKR_VERSION_STR "0.0.0"
#endif

namespace bookr {
inline constexpr const char* VERSION = BOOKR_VERSION_STR;
}

namespace {

constexpr int kMinCols = 70;
constexpr int kMinRows = 14;
constexpr int kMaxTextWidth = 80;

std::string run_command(const std::string& cmd);
std::string decode_text_bytes(const std::string& bytes);
std::string normalize_text(const std::string& raw_bytes);
std::string normalize_heading_title(const std::string& raw_line);
std::string canonical_note_id(const std::string& raw);
std::size_t utf8_decode_one(const std::string& s, std::size_t i, char32_t& cp);
int codepoint_width(char32_t cp);
char32_t to_lower_cp(char32_t cp);
bool is_word_codepoint(char32_t cp);
void append_utf8(std::string& out, char32_t cp);
void adjust_scroll(int selected, int visible_rows, int& scroll);
bool copy_to_clipboard(const std::string& text);

struct TermiosGuard {
    termios orig{};
    bool active{false};

    void enable_raw() {
        if (tcgetattr(STDIN_FILENO, &orig) != 0) {
            std::perror("tcgetattr");
            std::exit(1);
        }
        termios raw = orig;
        raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_oflag |= OPOST;
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            std::perror("tcsetattr");
            std::exit(1);
        }
        active = true;
    }

    ~TermiosGuard() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
        }
    }
};

struct ScreenGuard {
    bool active{false};

    void enter_fullscreen() {
        if (active) return;
        std::cout << "\033[?1049h\033[?25l\033[?1000h\033[?1006h";
        std::cout.flush();
        active = true;
    }

    void leave_fullscreen() {
        if (!active) return;
        std::cout << "\033[?1006l\033[?1000l\033[?25h\033[?1049l";
        std::cout.flush();
        active = false;
    }

    ~ScreenGuard() {
        leave_fullscreen();
    }
};

struct WinSize {
    int rows{24};
    int cols{80};
};

struct ReaderLayout {
    int left{0};
    int top{0};
    int text_cols{80};
    int page_rows{20};
};

struct TocEntry {
    int source_line_idx{0};
    int level{1};
    std::string title;
};

struct SearchJob {
    bool active{false};
    std::string query;
    std::size_t next_line{0};
    std::vector<int> hits;
};

struct WrappedText {
    std::vector<std::string> lines;
    std::vector<int> source_line_by_wrapped;
    std::vector<int> first_wrapped_by_source;
};

struct MetadataHeading {
    int level{2};
    std::string title;
    std::string src_path;
    std::string src_fragment;
};

struct FootnoteEntry {
    std::string id;
    std::string text;
    int source_line{0};
};

enum Key {
    KEY_NONE = -1,
    KEY_ARROW_UP = 1000,
    KEY_ARROW_DOWN,
    KEY_ARROW_LEFT,
    KEY_ARROW_RIGHT,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_MOUSE_WHEEL_UP,
    KEY_MOUSE_WHEEL_DOWN
};

WinSize get_winsize() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        return {static_cast<int>(ws.ws_row), static_cast<int>(ws.ws_col)};
    }
    return {};
}

ReaderLayout compute_layout(const WinSize& ws) {
    ReaderLayout layout{};
    const int min_side_padding = 2;
    const int effective_width = std::max(1, std::min(ws.cols - 6, kMaxTextWidth));
    layout.text_cols = effective_width;
    layout.left = std::max(min_side_padding, (ws.cols - layout.text_cols) / 2);
    layout.page_rows = std::max(1, ws.rows - 2);
    layout.top = 0;
    return layout;
}

void move_cursor(int row, int col) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    std::cout << "\033[" << row << ";" << col << "H";
}

std::string base_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string fit_left(const std::string& text, int width) {
    if (width <= 0) return "";

    auto display_width = [](const std::string& s) -> int {
        int w = 0;
        std::size_t i = 0;
        while (i < s.size()) {
            char32_t cp = 0;
            const std::size_t n = utf8_decode_one(s, i, cp);
            w += std::max(0, codepoint_width(cp));
            i += std::max<std::size_t>(1, n);
        }
        return w;
    };

    auto truncate_to_width = [&](const std::string& s, int max_w) -> std::string {
        if (max_w <= 0) return "";
        std::size_t i = 0;
        int w = 0;
        std::size_t end = 0;
        while (i < s.size()) {
            char32_t cp = 0;
            const std::size_t n = utf8_decode_one(s, i, cp);
            const int cw = std::max(0, codepoint_width(cp));
            if (w + cw > max_w) break;
            w += cw;
            i += std::max<std::size_t>(1, n);
            end = i;
        }
        return s.substr(0, end);
    };

    const int w = display_width(text);
    if (w <= width) {
        return text + std::string(static_cast<std::size_t>(width - w), ' ');
    }
    return truncate_to_width(text, width);
}

std::string fit_with_right(const std::string& left, const std::string& right, int width) {
    if (width <= 0) return "";

    auto display_width = [](const std::string& s) -> int {
        int w = 0;
        std::size_t i = 0;
        while (i < s.size()) {
            char32_t cp = 0;
            const std::size_t n = utf8_decode_one(s, i, cp);
            w += std::max(0, codepoint_width(cp));
            i += std::max<std::size_t>(1, n);
        }
        return w;
    };
    auto truncate_to_width = [&](const std::string& s, int max_w) -> std::string {
        if (max_w <= 0) return "";
        std::size_t i = 0;
        int w = 0;
        std::size_t end = 0;
        while (i < s.size()) {
            char32_t cp = 0;
            const std::size_t n = utf8_decode_one(s, i, cp);
            const int cw = std::max(0, codepoint_width(cp));
            if (w + cw > max_w) break;
            w += cw;
            i += std::max<std::size_t>(1, n);
            end = i;
        }
        return s.substr(0, end);
    };

    const int min_gap = 1;
    const std::string right_fit = truncate_to_width(right, width);
    const int right_w = display_width(right_fit);
    if (right_w >= width) return right_fit;

    const int max_left = width - right_w - min_gap;
    if (max_left < 0) {
        return right_fit;
    }
    const std::string left_trim = truncate_to_width(left, max_left);
    const int left_w = display_width(left_trim);
    const int spaces = width - left_w - right_w;
    return left_trim + std::string(static_cast<std::size_t>(std::max(1, spaces)), ' ') + right_fit;
}

std::string current_hhmm() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return std::string(buf);
}

std::string query_battery_compact() {
    try {
        const std::string out = run_command("pmset -g batt");
        bool ac = out.find("AC Power") != std::string::npos;
        std::string pct = "--";
        const std::size_t pos = out.find('%');
        if (pos != std::string::npos) {
            std::size_t start = pos;
            while (start > 0 && std::isdigit(static_cast<unsigned char>(out[start - 1])) != 0) {
                --start;
            }
            if (start < pos) {
                pct = out.substr(start, pos - start + 1);
            }
        }
        if (ac) {
            return pct == "--" ? "AC" : (pct + " AC");
        }
        return pct;
    } catch (...) {
        return "--";
    }
}

std::string battery_compact_cached() {
    static std::string cached = "--";
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 15) {
        cached = query_battery_compact();
        last = now;
    }
    return cached;
}

std::string read_first_line_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::string line;
    std::getline(in, line);
    return line;
}

std::string battery_percent_linux() {
    for (const char* p : {"/sys/class/power_supply/BAT0/capacity",
                          "/sys/class/power_supply/BAT1/capacity"}) {
        const std::string raw = read_first_line_file(p);
        if (raw.empty()) continue;
        std::string digits;
        for (char c : raw) {
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                digits.push_back(c);
            }
        }
        if (!digits.empty()) {
            return digits + "%";
        }
    }
    return "";
}

std::string get_system_info() {
    const std::string hhmm = current_hhmm();
    std::string battery = battery_percent_linux();
    if (battery.empty()) {
        battery = battery_compact_cached();
    }
    if (battery.empty()) {
        battery = "--";
    }
    return "[ " + hhmm + " | 🔋 " + battery + " ]";
}

std::string trim_ascii(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && static_cast<unsigned char>(s[start]) <= 32U) ++start;
    std::size_t end = s.size();
    while (end > start && static_cast<unsigned char>(s[end - 1]) <= 32U) --end;
    return s.substr(start, end - start);
}

bool starts_with_ci_ascii(const std::string& s, const std::string& prefix);

bool is_ascii_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

std::string strip_token_punct(const std::string& token) {
    auto is_punct = [](char c) -> bool {
        switch (c) {
            case '.':
            case ',':
            case ':':
            case ';':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '"':
            case '\'':
                return true;
            default:
                return false;
        }
    };

    std::size_t start = 0;
    while (start < token.size() && is_punct(token[start])) ++start;
    std::size_t end = token.size();
    while (end > start && is_punct(token[end - 1])) --end;
    return token.substr(start, end - start);
}

bool is_page_locator_word(const std::string& token) {
    const std::string t = strip_token_punct(token);
    return starts_with_ci_ascii(t, "page") ||
           t == "стр" || t == "Стр" || t == "СТР" ||
           t == "страница" || t == "Страница" || t == "СТРАНИЦА";
}

bool is_line_locator_word(const std::string& token) {
    const std::string t = strip_token_punct(token);
    return starts_with_ci_ascii(t, "line") ||
           t == "строка" || t == "Строка" || t == "СТРОКА";
}

bool is_pdf_locator_line(const std::string& raw_line) {
    const std::string line = trim_ascii(raw_line);
    if (line.empty()) return false;

    std::istringstream ss(line);
    std::vector<std::string> tokens;
    std::string tok;
    while (ss >> tok) {
        tokens.push_back(tok);
    }
    if (tokens.size() < 4) return false;

    for (std::size_t i = 0; i + 3 < tokens.size(); ++i) {
        if (!is_page_locator_word(tokens[i])) continue;

        const std::string page_num = strip_token_punct(tokens[i + 1]);
        if (!is_ascii_digits(page_num)) continue;

        for (std::size_t k = i + 2; k + 1 < tokens.size(); ++k) {
            if (!is_line_locator_word(tokens[k])) continue;
            const std::string line_num = strip_token_punct(tokens[k + 1]);
            if (is_ascii_digits(line_num)) return true;
        }
    }
    return false;
}

bool starts_with_ci_ascii(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

std::string lower_ext(const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string ext = path.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

std::string read_file_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    in.seekg(0, std::ios::beg);
    std::ostringstream ss;
    if (sz > 0 && sz < static_cast<std::streamoff>(256 * 1024 * 1024)) {
        std::string tmp;
        tmp.resize(static_cast<std::size_t>(sz));
        in.read(&tmp[0], sz);
        tmp.resize(static_cast<std::size_t>(in.gcount()));
        return tmp;
    }
    ss << in.rdbuf();
    return ss.str();
}

std::string run_command(const std::string& cmd) {
    std::string out;
    out.reserve(256 * 1024);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to run command: " + cmd);
    }

    char buf[4096];
    while (true) {
        std::size_t n = std::fread(buf, 1, sizeof(buf), pipe);
        if (n > 0) out.append(buf, n);
        if (n < sizeof(buf)) break;
    }

    int rc = pclose(pipe);
    if (rc != 0) {
        throw std::runtime_error("Command failed: " + cmd);
    }
    return out;
}

bool command_exists(const char* name) {
    if (!name || !name[0]) return false;
    std::string cmd = "which ";
    cmd += name;
    cmd += " > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string collapse_spaces_ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (char c : s) {
        if (static_cast<unsigned char>(c) <= 32U) {
            if (!prev_space) out.push_back(' ');
            prev_space = true;
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    return trim_ascii(out);
}

std::string xml_unescape_basic(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        if (s.compare(i, 5, "&amp;") == 0) {
            out.push_back('&');
            i += 4;
        } else if (s.compare(i, 4, "&lt;") == 0) {
            out.push_back('<');
            i += 3;
        } else if (s.compare(i, 4, "&gt;") == 0) {
            out.push_back('>');
            i += 3;
        } else if (s.compare(i, 6, "&quot;") == 0) {
            out.push_back('"');
            i += 5;
        } else if (s.compare(i, 6, "&apos;") == 0) {
            out.push_back('\'');
            i += 5;
        } else if (s.compare(i, 6, "&#160;") == 0) {
            out.push_back(' ');
            i += 5;
        } else if (s.compare(i, 5, "&#39;") == 0) {
            out.push_back('\'');
            i += 4;
        } else {
            out.push_back('&');
        }
    }
    return out;
}

std::string strip_xml_tags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            out.push_back(' ');
            continue;
        }
        if (!in_tag) out.push_back(c);
    }
    return out;
}

std::string normalize_match_key(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    std::size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        const std::size_t n = utf8_decode_one(s, i, cp);
        i += n;
        cp = to_lower_cp(cp);
        if (is_word_codepoint(cp)) {
            append_utf8(out, cp);
            prev_space = false;
        } else {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        }
    }
    return trim_ascii(out);
}

std::string to_lower_ascii_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool looks_like_notes_heading(const std::string& raw_line) {
    const std::string line = trim_ascii(raw_line);
    if (line.empty() || line.size() > 64) return false;
    const std::string low = to_lower_ascii_copy(line);
    return starts_with_ci_ascii(low, "notes") ||
           starts_with_ci_ascii(low, "footnotes") ||
           starts_with_ci_ascii(low, "endnotes") ||
           line.rfind("Примечан", 0) == 0 ||
           line.rfind("примечан", 0) == 0 ||
           line.rfind("ПРИМЕЧАН", 0) == 0 ||
           line.rfind("Комментар", 0) == 0 ||
           line.rfind("комментар", 0) == 0 ||
           line.rfind("КОММЕНТАР", 0) == 0;
}

std::vector<int> collect_notes_heading_lines(const std::vector<std::string>& source_lines) {
    std::vector<int> heads;
    for (int i = 0; i < static_cast<int>(source_lines.size()); ++i) {
        if (looks_like_notes_heading(source_lines[static_cast<std::size_t>(i)])) {
            heads.push_back(i);
        }
    }
    return heads;
}

const FootnoteEntry* find_best_footnote_entry(const std::vector<FootnoteEntry>& footnotes,
                                              const std::string& marker_id,
                                              int current_source_line,
                                              const std::vector<int>& notes_heads) {
    const std::string id = canonical_note_id(marker_id);
    if (id.empty()) return nullptr;

    std::vector<const FootnoteEntry*> candidates;
    for (const auto& note : footnotes) {
        if (note.id == id) {
            candidates.push_back(&note);
        }
    }
    if (candidates.empty()) return nullptr;

    if (!notes_heads.empty()) {
        std::vector<const FootnoteEntry*> region_candidates;
        for (const FootnoteEntry* n : candidates) {
            if (n->source_line >= notes_heads.front()) {
                region_candidates.push_back(n);
            }
        }
        if (!region_candidates.empty()) {
            candidates.swap(region_candidates);
        }
    }

    const FootnoteEntry* best_forward = nullptr;
    int best_forward_dist = 0;
    for (const FootnoteEntry* n : candidates) {
        if (n->source_line < current_source_line) continue;
        const int d = n->source_line - current_source_line;
        if (best_forward == nullptr || d < best_forward_dist) {
            best_forward = n;
            best_forward_dist = d;
        }
    }
    if (best_forward != nullptr) return best_forward;

    const FootnoteEntry* best_any = nullptr;
    int best_any_dist = 0;
    for (const FootnoteEntry* n : candidates) {
        const int d = std::abs(n->source_line - current_source_line);
        if (best_any == nullptr || d < best_any_dist) {
            best_any = n;
            best_any_dist = d;
        }
    }
    return best_any;
}

std::string zip_read_text(const std::string& archive_path, const std::string& inner_path) {
    return run_command("unzip -p " + shell_escape(archive_path) + " " + shell_escape(inner_path));
}

std::string dirname_posix(const std::string& p) {
    const std::size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return "";
    return p.substr(0, pos);
}

std::string join_posix(const std::string& base, const std::string& rel) {
    if (base.empty()) return rel;
    if (rel.empty()) return base;
    if (rel[0] == '/') return rel.substr(1);
    return base + "/" + rel;
}

std::string normalize_posix_path(const std::string& in) {
    if (in.empty()) return in;
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i <= in.size(); ++i) {
        const char c = (i < in.size()) ? in[i] : '/';
        if (c == '/') {
            if (cur == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.push_back('/');
        out += parts[i];
    }
    return out;
}

void split_href_ref(const std::string& href, std::string& path_part, std::string& fragment_part) {
    const std::size_t hash = href.find('#');
    if (hash == std::string::npos) {
        path_part = href;
        fragment_part.clear();
    } else {
        path_part = href.substr(0, hash);
        fragment_part = href.substr(hash + 1);
    }
}

std::string find_attr_value(const std::string& tag, const std::string& attr_name) {
    const std::string key = attr_name + "=";
    std::size_t p = tag.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    if (p >= tag.size()) return "";
    const char q = tag[p];
    if (q != '"' && q != '\'') return "";
    ++p;
    const std::size_t e = tag.find(q, p);
    if (e == std::string::npos) return "";
    return tag.substr(p, e - p);
}

std::vector<MetadataHeading> parse_epub_nav_xhtml(const std::string& xml,
                                                  const std::string& nav_doc_dir) {
    std::vector<MetadataHeading> out;
    std::size_t p = 0;
    int ol_depth = 0;
    while (p < xml.size()) {
        const std::size_t tag_start = xml.find('<', p);
        if (tag_start == std::string::npos) break;
        const std::size_t tag_end = xml.find('>', tag_start + 1);
        if (tag_end == std::string::npos) break;
        const std::string tag = xml.substr(tag_start, tag_end - tag_start + 1);
        if (tag.rfind("<ol", 0) == 0 || tag.rfind("<OL", 0) == 0) {
            ++ol_depth;
        } else if (tag.rfind("</ol", 0) == 0 || tag.rfind("</OL", 0) == 0) {
            ol_depth = std::max(0, ol_depth - 1);
        } else if (tag.rfind("<a", 0) == 0 || tag.rfind("<A", 0) == 0) {
            const std::string href = find_attr_value(tag, "href");
            const std::size_t close = xml.find("</a>", tag_end + 1);
            if (close == std::string::npos) {
                p = tag_end + 1;
                continue;
            }
            const std::string raw = xml.substr(tag_end + 1, close - (tag_end + 1));
            std::string title = collapse_spaces_ascii(xml_unescape_basic(strip_xml_tags(raw)));
            if (!title.empty()) {
                MetadataHeading h;
                h.level = std::max(1, std::min(4, ol_depth > 0 ? ol_depth : 1));
                h.title = title;
                std::string path_part;
                std::string fragment_part;
                split_href_ref(href, path_part, fragment_part);
                h.src_path = normalize_posix_path(join_posix(nav_doc_dir, path_part));
                h.src_fragment = fragment_part;
                out.push_back(h);
            }
            p = close + 4;
            continue;
        }
        p = tag_end + 1;
    }
    return out;
}

std::vector<MetadataHeading> parse_epub_ncx(const std::string& xml,
                                            const std::string& ncx_doc_dir) {
    std::vector<MetadataHeading> out;
    std::size_t p = 0;
    int depth = 0;
    std::vector<std::string> src_stack;
    src_stack.reserve(32);
    while (p < xml.size()) {
        const std::size_t tag_start = xml.find('<', p);
        if (tag_start == std::string::npos) break;
        const std::size_t tag_end = xml.find('>', tag_start + 1);
        if (tag_end == std::string::npos) break;
        const std::string tag = xml.substr(tag_start, tag_end - tag_start + 1);
        if (tag.rfind("<navPoint", 0) == 0 || tag.rfind("<navpoint", 0) == 0) {
            ++depth;
            src_stack.emplace_back();
        } else if (tag.rfind("</navPoint", 0) == 0 || tag.rfind("</navpoint", 0) == 0) {
            depth = std::max(0, depth - 1);
            if (!src_stack.empty()) src_stack.pop_back();
        } else if (tag.rfind("<content", 0) == 0 || tag.rfind("<CONTENT", 0) == 0) {
            std::string src = find_attr_value(tag, "src");
            if (!src.empty() && !src_stack.empty()) {
                src_stack.back() = src;
            }
        } else if (tag.rfind("<text", 0) == 0 || tag.rfind("<TEXT", 0) == 0) {
            const std::size_t close = xml.find("</text>", tag_end + 1);
            if (close == std::string::npos) {
                p = tag_end + 1;
                continue;
            }
            std::string title = collapse_spaces_ascii(xml_unescape_basic(xml.substr(tag_end + 1, close - (tag_end + 1))));
            if (!title.empty()) {
                MetadataHeading h;
                h.level = std::max(1, std::min(4, depth > 0 ? depth : 1));
                h.title = title;
                std::string src = src_stack.empty() ? "" : src_stack.back();
                std::string path_part;
                std::string fragment_part;
                split_href_ref(src, path_part, fragment_part);
                h.src_path = normalize_posix_path(join_posix(ncx_doc_dir, path_part));
                h.src_fragment = fragment_part;
                out.push_back(h);
            }
            p = close + 7;
            continue;
        }
        p = tag_end + 1;
    }
    return out;
}

std::vector<MetadataHeading> read_epub_metadata_headings(const std::string& path) {
    std::vector<MetadataHeading> headings;
    if (!command_exists("unzip")) return headings;
    try {
        const std::string container = zip_read_text(path, "META-INF/container.xml");
        const std::size_t full_path_pos = container.find("full-path=");
        if (full_path_pos == std::string::npos) return headings;
        const std::size_t q1 = container.find_first_of("\"'", full_path_pos);
        if (q1 == std::string::npos) return headings;
        const char q = container[q1];
        const std::size_t q2 = container.find(q, q1 + 1);
        if (q2 == std::string::npos) return headings;
        const std::string opf_path = container.substr(q1 + 1, q2 - (q1 + 1));
        const std::string opf = zip_read_text(path, opf_path);
        const std::string opf_dir = dirname_posix(opf_path);

        std::string nav_href;
        std::string toc_id;
        std::size_t p = 0;
        while (true) {
            const std::size_t item = opf.find("<item", p);
            if (item == std::string::npos) break;
            const std::size_t end = opf.find('>', item + 1);
            if (end == std::string::npos) break;
            const std::string tag = opf.substr(item, end - item + 1);
            const std::string props = find_attr_value(tag, "properties");
            if (props.find("nav") != std::string::npos) {
                nav_href = find_attr_value(tag, "href");
            }
            p = end + 1;
        }

        const std::size_t spine_pos = opf.find("<spine");
        if (spine_pos != std::string::npos) {
            const std::size_t spine_end = opf.find('>', spine_pos + 1);
            if (spine_end != std::string::npos) {
                toc_id = find_attr_value(opf.substr(spine_pos, spine_end - spine_pos + 1), "toc");
            }
        }

        std::string ncx_href;
        if (!toc_id.empty()) {
            p = 0;
            while (true) {
                const std::size_t item = opf.find("<item", p);
                if (item == std::string::npos) break;
                const std::size_t end = opf.find('>', item + 1);
                if (end == std::string::npos) break;
                const std::string tag = opf.substr(item, end - item + 1);
                if (find_attr_value(tag, "id") == toc_id) {
                    ncx_href = find_attr_value(tag, "href");
                    break;
                }
                p = end + 1;
            }
        }

        if (!nav_href.empty()) {
            const std::string nav_path = normalize_posix_path(join_posix(opf_dir, nav_href));
            const std::string nav = zip_read_text(path, nav_path);
            headings = parse_epub_nav_xhtml(nav, dirname_posix(nav_path));
        }
        if (headings.empty() && !ncx_href.empty()) {
            const std::string ncx_path = normalize_posix_path(join_posix(opf_dir, ncx_href));
            const std::string ncx = zip_read_text(path, ncx_path);
            headings = parse_epub_ncx(ncx, dirname_posix(ncx_path));
        }
    } catch (...) {
    }
    return headings;
}

bool read_epub_opf(const std::string& path, std::string& opf_dir, std::string& opf_xml) {
    try {
        const std::string container = zip_read_text(path, "META-INF/container.xml");
        const std::size_t full_path_pos = container.find("full-path=");
        if (full_path_pos == std::string::npos) return false;
        const std::size_t q1 = container.find_first_of("\"'", full_path_pos);
        if (q1 == std::string::npos) return false;
        const char q = container[q1];
        const std::size_t q2 = container.find(q, q1 + 1);
        if (q2 == std::string::npos) return false;
        const std::string opf_path = container.substr(q1 + 1, q2 - (q1 + 1));
        opf_dir = dirname_posix(opf_path);
        opf_xml = zip_read_text(path, opf_path);
        return true;
    } catch (...) {
        return false;
    }
}

std::unordered_map<std::string, std::string> parse_epub_manifest(const std::string& opf_xml) {
    std::unordered_map<std::string, std::string> id_to_href;
    std::size_t p = 0;
    while (true) {
        const std::size_t item = opf_xml.find("<item", p);
        if (item == std::string::npos) break;
        const std::size_t end = opf_xml.find('>', item + 1);
        if (end == std::string::npos) break;
        const std::string tag = opf_xml.substr(item, end - item + 1);
        const std::string id = find_attr_value(tag, "id");
        const std::string href = find_attr_value(tag, "href");
        if (!id.empty() && !href.empty()) {
            id_to_href[id] = href;
        }
        p = end + 1;
    }
    return id_to_href;
}

std::vector<std::string> parse_epub_spine_idrefs(const std::string& opf_xml) {
    std::vector<std::string> out;
    std::size_t p = 0;
    while (true) {
        const std::size_t item = opf_xml.find("<itemref", p);
        if (item == std::string::npos) break;
        const std::size_t end = opf_xml.find('>', item + 1);
        if (end == std::string::npos) break;
        const std::string tag = opf_xml.substr(item, end - item + 1);
        const std::string idref = find_attr_value(tag, "idref");
        if (!idref.empty()) {
            out.push_back(idref);
        }
        p = end + 1;
    }
    return out;
}

std::vector<std::string> build_epub_spine_paths(const std::string& opf_dir, const std::string& opf_xml) {
    std::vector<std::string> paths;
    const auto manifest = parse_epub_manifest(opf_xml);
    const auto spine = parse_epub_spine_idrefs(opf_xml);
    for (const std::string& idref : spine) {
        const auto it = manifest.find(idref);
        if (it == manifest.end()) continue;
        const std::string p = normalize_posix_path(join_posix(opf_dir, it->second));
        if (!p.empty()) paths.push_back(p);
    }
    return paths;
}

bool load_epub_text_from_spine(const std::string& path,
                               std::string& out_text,
                               std::unordered_map<std::string, int>& source_line_by_path) {
    std::string opf_dir;
    std::string opf_xml;
    if (!read_epub_opf(path, opf_dir, opf_xml)) return false;
    const std::vector<std::string> spine_paths = build_epub_spine_paths(opf_dir, opf_xml);
    if (spine_paths.empty()) return false;
    if (!command_exists("pandoc") || !command_exists("unzip")) return false;

    std::string text;
    int source_line = 0;
    for (std::size_t i = 0; i < spine_paths.size(); ++i) {
        const std::string& part = spine_paths[i];
        source_line_by_path[part] = source_line;
        try {
            std::string plain = run_command("unzip -p " + shell_escape(path) + " " + shell_escape(part) +
                                            " | pandoc -f html -t plain");
            plain = normalize_text(plain);
            if (!text.empty()) {
                text += "\n\n";
                source_line += 2;
            }
            text += plain;
            source_line += static_cast<int>(std::count(plain.begin(), plain.end(), '\n')) + 1;
        } catch (...) {
            return false;
        }
    }
    out_text = text;
    return !out_text.empty();
}

int count_lines_rough_html_prefix(const std::string& html_prefix) {
    std::string out;
    out.reserve(html_prefix.size());
    bool in_tag = false;
    std::string tag;
    for (char ch : html_prefix) {
        if (!in_tag) {
            if (ch == '<') {
                in_tag = true;
                tag.clear();
            } else {
                out.push_back(ch);
            }
        } else {
            if (ch == '>') {
                in_tag = false;
                std::string low = to_lower_ascii_copy(trim_ascii(tag));
                if (starts_with_ci_ascii(low, "br") ||
                    starts_with_ci_ascii(low, "/p") ||
                    starts_with_ci_ascii(low, "/div") ||
                    starts_with_ci_ascii(low, "/li") ||
                    starts_with_ci_ascii(low, "/h") ||
                    starts_with_ci_ascii(low, "/section") ||
                    starts_with_ci_ascii(low, "/tr") ||
                    starts_with_ci_ascii(low, "/blockquote")) {
                    out.push_back('\n');
                }
            } else {
                tag.push_back(ch);
            }
        }
    }
    out = xml_unescape_basic(out);
    out = normalize_text(out);
    return static_cast<int>(std::count(out.begin(), out.end(), '\n'));
}

int resolve_epub_target_line(const std::string& epub_path,
                             const std::string& src_path,
                             const std::string& src_fragment,
                             const std::unordered_map<std::string, int>& source_line_by_epub_path,
                             std::unordered_map<std::string, int>& cache) {
    const std::string path_norm = normalize_posix_path(src_path);
    const auto pit = source_line_by_epub_path.find(path_norm);
    if (pit == source_line_by_epub_path.end()) return -1;
    const int base = pit->second;
    if (src_fragment.empty()) return base;

    const std::string key = path_norm + "#" + src_fragment;
    const auto cit = cache.find(key);
    if (cit != cache.end()) return cit->second;

    int resolved = base;
    try {
        const std::string html = zip_read_text(epub_path, path_norm);
        const std::string f1 = "id=\"" + src_fragment + "\"";
        const std::string f2 = "id='" + src_fragment + "'";
        const std::string f3 = "name=\"" + src_fragment + "\"";
        const std::string f4 = "name='" + src_fragment + "'";
        std::size_t pos = html.find(f1);
        if (pos == std::string::npos) pos = html.find(f2);
        if (pos == std::string::npos) pos = html.find(f3);
        if (pos == std::string::npos) pos = html.find(f4);
        if (pos != std::string::npos) {
            const std::string prefix = html.substr(0, pos);
            resolved = base + count_lines_rough_html_prefix(prefix);
        }
    } catch (...) {
    }
    cache[key] = resolved;
    return resolved;
}

std::vector<MetadataHeading> read_fb2_metadata_headings(const std::string& path) {
    std::vector<MetadataHeading> out;
    try {
        const std::string xml = decode_text_bytes(read_file_all(path));
        int section_depth = 0;
        int title_depth = 0;
        int body_depth = 0;
        int notes_body_depth = -1;
        std::string title_buf;

        std::size_t p = 0;
        while (p < xml.size()) {
            const std::size_t tag_start = xml.find('<', p);
            if (tag_start == std::string::npos) {
                if (title_depth > 0 && p < xml.size()) {
                    title_buf.append(xml.substr(p));
                }
                break;
            }
            if (title_depth > 0 && tag_start > p) {
                title_buf.append(xml.substr(p, tag_start - p));
                title_buf.push_back(' ');
            }

            const std::size_t tag_end = xml.find('>', tag_start + 1);
            if (tag_end == std::string::npos) break;
            std::string tag = xml.substr(tag_start + 1, tag_end - tag_start - 1);
            bool closing = false;
            if (!tag.empty() && tag[0] == '/') {
                closing = true;
                tag.erase(tag.begin());
            }
            while (!tag.empty() && static_cast<unsigned char>(tag.back()) <= 32U) tag.pop_back();
            const bool self_closing = !tag.empty() && tag.back() == '/';

            std::size_t i = 0;
            while (i < tag.size() && static_cast<unsigned char>(tag[i]) <= 32U) ++i;
            std::size_t j = i;
            while (j < tag.size()) {
                const unsigned char c = static_cast<unsigned char>(tag[j]);
                if (!(std::isalnum(c) || c == ':' || c == '_' || c == '-')) break;
                ++j;
            }
            std::string name = tag.substr(i, j - i);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            const std::size_t colon = name.find_last_of(':');
            if (colon != std::string::npos) {
                name = name.substr(colon + 1);
            }

            if (name == "body") {
                if (closing) {
                    if (body_depth == notes_body_depth) {
                        notes_body_depth = -1;
                    }
                    body_depth = std::max(0, body_depth - 1);
                } else if (!self_closing) {
                    ++body_depth;
                    std::string body_name = find_attr_value(tag, "name");
                    if (body_name.empty()) body_name = find_attr_value(tag, "type");
                    std::transform(body_name.begin(), body_name.end(), body_name.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    if (notes_body_depth < 0 &&
                        (body_name == "notes" || body_name == "note" ||
                         body_name == "comments" || body_name == "comment")) {
                        notes_body_depth = body_depth;
                    }
                }
            } else if (name == "section") {
                if (closing) {
                    section_depth = std::max(0, section_depth - 1);
                } else if (!self_closing) {
                    ++section_depth;
                }
            } else if (name == "title" && section_depth > 0 && notes_body_depth < 0) {
                if (closing) {
                    if (title_depth > 0) {
                        --title_depth;
                        if (title_depth == 0) {
                            std::string t = collapse_spaces_ascii(xml_unescape_basic(strip_xml_tags(title_buf)));
                            t = normalize_heading_title(t);
                            if (!t.empty()) {
                                MetadataHeading h;
                                h.level = std::max(1, std::min(4, section_depth));
                                h.title = t;
                                if (out.empty() || out.back().title != h.title) {
                                    out.push_back(h);
                                }
                            }
                            title_buf.clear();
                        }
                    }
                } else if (!self_closing) {
                    ++title_depth;
                    if (title_depth == 1) {
                        title_buf.clear();
                    }
                }
            }

            p = tag_end + 1;
        }
    } catch (...) {
    }
    return out;
}

std::vector<MetadataHeading> read_book_metadata_headings(const std::string& path) {
    const std::string ext = lower_ext(path);
    if (ext == "epub") {
        return read_epub_metadata_headings(path);
    }
    if (ext == "fb2") {
        return read_fb2_metadata_headings(path);
    }
    return {};
}

std::string to_text(const std::string& path) {
    const std::string ext = lower_ext(path);
    if (ext == "txt") {
        return read_file_all(path);
    }
    if (ext == "fb2" || ext == "epub") {
        if (!command_exists("pandoc")) {
            throw std::runtime_error("pandoc is not installed. Install it: brew install pandoc");
        }
        return run_command("pandoc -t plain " + shell_escape(path));
    }
    if (ext == "pdf") {
        if (!command_exists("pdftotext")) {
            throw std::runtime_error("pdftotext is not installed. Install poppler: brew install poppler");
        }
        // Faster conversion and cleaner plain text for pagination/search.
        return run_command("pdftotext -q -enc UTF-8 -nopgbrk " + shell_escape(path) + " -");
    }
    throw std::runtime_error("Unsupported file type: ." + ext);
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6U) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12U) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18U) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12U) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string decode_utf16_to_utf8(const std::string& bytes, bool little_endian) {
    std::string out;
    out.reserve(bytes.size());

    auto read_u16 = [&](std::size_t pos) -> uint16_t {
        const uint8_t b0 = static_cast<uint8_t>(bytes[pos]);
        const uint8_t b1 = static_cast<uint8_t>(bytes[pos + 1]);
        if (little_endian) {
            return static_cast<uint16_t>(b0 | (static_cast<uint16_t>(b1) << 8U));
        }
        return static_cast<uint16_t>((static_cast<uint16_t>(b0) << 8U) | b1);
    };

    std::size_t i = 2;
    while (i + 1 < bytes.size()) {
        const uint16_t w1 = read_u16(i);
        i += 2;

        if (w1 >= 0xD800 && w1 <= 0xDBFF) {
            if (i + 1 < bytes.size()) {
                const uint16_t w2 = read_u16(i);
                if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                    i += 2;
                    const char32_t cp = 0x10000U + (((w1 - 0xD800U) << 10U) | (w2 - 0xDC00U));
                    append_utf8(out, cp);
                    continue;
                }
            }
            append_utf8(out, U'?');
            continue;
        }

        if (w1 >= 0xDC00 && w1 <= 0xDFFF) {
            append_utf8(out, U'?');
            continue;
        }

        append_utf8(out, static_cast<char32_t>(w1));
    }

    return out;
}

std::string decode_text_bytes(const std::string& bytes) {
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return bytes.substr(3);
    }

    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return decode_utf16_to_utf8(bytes, true);
    }

    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        return decode_utf16_to_utf8(bytes, false);
    }

    return bytes;
}

std::string normalize_text(const std::string& raw_bytes) {
    const std::string text = decode_text_bytes(raw_bytes);
    std::string normalized;
    normalized.reserve(text.size());
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\t') {
            normalized.append("    ");
            continue;
        }
        normalized.push_back(c);
    }

    std::string out;
    out.reserve(normalized.size());
    std::size_t start = 0;
    while (start <= normalized.size()) {
        std::size_t end = normalized.find('\n', start);
        if (end == std::string::npos) end = normalized.size();
        const std::string line = normalized.substr(start, end - start);
        if (!is_pdf_locator_line(line)) {
            out.append(line);
            if (end < normalized.size()) out.push_back('\n');
        }
        if (end == normalized.size()) break;
        start = end + 1;
    }
    return out;
}

std::size_t utf8_decode_one(const std::string& s, std::size_t i, char32_t& cp) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if ((b0 & 0x80U) == 0) {
        cp = static_cast<char32_t>(b0);
        return 1;
    }
    if ((b0 & 0xE0U) == 0xC0U && i + 1 < s.size()) {
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if ((b1 & 0xC0U) == 0x80U) {
            cp = static_cast<char32_t>(((b0 & 0x1FU) << 6U) | (b1 & 0x3FU));
            return 2;
        }
    }
    if ((b0 & 0xF0U) == 0xE0U && i + 2 < s.size()) {
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U) {
            cp = static_cast<char32_t>(((b0 & 0x0FU) << 12U) | ((b1 & 0x3FU) << 6U) | (b2 & 0x3FU));
            return 3;
        }
    }
    if ((b0 & 0xF8U) == 0xF0U && i + 3 < s.size()) {
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
        if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U && (b3 & 0xC0U) == 0x80U) {
            cp = static_cast<char32_t>(((b0 & 0x07U) << 18U) |
                                       ((b1 & 0x3FU) << 12U) |
                                       ((b2 & 0x3FU) << 6U) |
                                       (b3 & 0x3FU));
            return 4;
        }
    }
    cp = U'?';
    return 1;
}

int codepoint_width(char32_t cp) {
    if (cp == U'\t') return 4;
    if (cp < 0x20U) return 0;
    const int w = wcwidth(static_cast<wchar_t>(cp));
    return w >= 0 ? w : 1;
}

bool is_space_codepoint(char32_t cp) {
    return cp == U' ' || cp == U'\t';
}

std::vector<std::string> wrap_lines(const std::string& text, int width) {
    std::vector<std::string> lines;
    lines.reserve(std::max<std::size_t>(64, text.size() / 40));
    if (width < 1) width = 1;

    std::size_t paragraph_start = 0;
    while (paragraph_start <= text.size()) {
        std::size_t paragraph_end = text.find('\n', paragraph_start);
        if (paragraph_end == std::string::npos) {
            paragraph_end = text.size();
        }

        if (paragraph_end == paragraph_start) {
            lines.emplace_back();
        } else {
            std::size_t i = paragraph_start;
            while (i < paragraph_end) {
                int current_width = 0;
                std::size_t j = i;
                std::size_t last_space_pos = std::string::npos;

                while (j < paragraph_end) {
                    char32_t cp = 0;
                    const std::size_t bytes = utf8_decode_one(text, j, cp);
                    const int w = codepoint_width(cp);
                    if (current_width + w > width) break;
                    current_width += w;
                    j += bytes;
                    if (is_space_codepoint(cp)) {
                        last_space_pos = j;
                    }
                }

                if (j == i) {
                    char32_t cp = 0;
                    j += utf8_decode_one(text, j, cp);
                } else if (j < paragraph_end && last_space_pos != std::string::npos && last_space_pos > i) {
                    j = last_space_pos;
                }

                std::size_t trimmed_end = j;
                while (trimmed_end > i && text[trimmed_end - 1] == ' ') {
                    --trimmed_end;
                }
                lines.push_back(text.substr(i, trimmed_end - i));

                i = j;
                while (i < paragraph_end && text[i] == ' ') {
                    ++i;
                }
            }
        }

        if (paragraph_end == text.size()) break;
        paragraph_start = paragraph_end + 1;
    }

    if (lines.empty()) {
        lines.emplace_back();
    }

    return lines;
}

std::vector<std::string> split_source_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
        if (start == text.size()) {
            lines.emplace_back();
            break;
        }
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

WrappedText wrap_lines_with_map(const std::vector<std::string>& source_lines, int width) {
    WrappedText out;
    const int effective_width = std::max(1, std::min(width, kMaxTextWidth));
    out.first_wrapped_by_source.resize(source_lines.size(), -1);
    out.lines.reserve(std::max<std::size_t>(64, source_lines.size() * 2));
    out.source_line_by_wrapped.reserve(out.lines.capacity());

    for (int src = 0; src < static_cast<int>(source_lines.size()); ++src) {
        const std::string& paragraph = source_lines[static_cast<std::size_t>(src)];
        const std::size_t paragraph_end = paragraph.size();
        int first_for_source = -1;

        if (paragraph.empty()) {
            first_for_source = static_cast<int>(out.lines.size());
            out.lines.emplace_back();
            out.source_line_by_wrapped.push_back(src);
        } else {
            std::size_t i = 0;
            // Normalize inconsistent left indentation from converters (epub/fb2/pdf).
            while (i < paragraph_end && paragraph[i] == ' ') {
                ++i;
            }
            while (i < paragraph_end) {
                int current_width = 0;
                std::size_t j = i;
                std::size_t last_space_pos = std::string::npos;

                while (j < paragraph_end) {
                    char32_t cp = 0;
                    const std::size_t bytes = utf8_decode_one(paragraph, j, cp);
                    const int w = codepoint_width(cp);
                    if (current_width + w > effective_width) break;
                    current_width += w;
                    j += bytes;
                    if (is_space_codepoint(cp)) {
                        last_space_pos = j;
                    }
                }

                if (j == i) {
                    char32_t cp = 0;
                    j += utf8_decode_one(paragraph, j, cp);
                } else if (j < paragraph_end && last_space_pos != std::string::npos && last_space_pos > i) {
                    j = last_space_pos;
                }

                std::size_t trimmed_end = j;
                while (trimmed_end > i && paragraph[trimmed_end - 1] == ' ') {
                    --trimmed_end;
                }
                if (first_for_source < 0) {
                    first_for_source = static_cast<int>(out.lines.size());
                }
                out.lines.push_back(paragraph.substr(i, trimmed_end - i));
                out.source_line_by_wrapped.push_back(src);

                i = j;
                while (i < paragraph_end && paragraph[i] == ' ') {
                    ++i;
                }
            }
        }

        if (first_for_source < 0) {
            first_for_source = static_cast<int>(out.lines.size());
            out.lines.emplace_back();
            out.source_line_by_wrapped.push_back(src);
        }
        out.first_wrapped_by_source[static_cast<std::size_t>(src)] = first_for_source;
    }

    if (out.lines.empty()) {
        out.lines.emplace_back();
        out.source_line_by_wrapped.push_back(0);
    }
    return out;
}

bool is_word_codepoint(char32_t cp) {
    if (cp == U'_') return true;
    if (cp <= 0x7FU) {
        return std::isalnum(static_cast<unsigned char>(cp)) != 0;
    }
    return std::iswalnum(static_cast<wint_t>(cp)) != 0;
}

char32_t to_lower_cp(char32_t cp) {
    if (cp <= 0x7FU) {
        return static_cast<char32_t>(std::tolower(static_cast<unsigned char>(cp)));
    }
    return static_cast<char32_t>(std::towlower(static_cast<wint_t>(cp)));
}

void decode_utf8_to_cps(const std::string& s,
                        std::vector<char32_t>& cps,
                        std::vector<std::size_t>& byte_offsets,
                        bool lower) {
    cps.clear();
    byte_offsets.clear();
    cps.reserve(s.size());
    byte_offsets.reserve(s.size() + 1);

    std::size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        const std::size_t bytes = utf8_decode_one(s, i, cp);
        byte_offsets.push_back(i);
        cps.push_back(lower ? to_lower_cp(cp) : cp);
        i += bytes;
    }
    byte_offsets.push_back(s.size());
}

std::vector<std::pair<std::size_t, std::size_t>> find_word_matches_in_line(const std::string& line,
                                                                            const std::string& query) {
    std::vector<std::pair<std::size_t, std::size_t>> hits;
    if (query.empty()) return hits;

    std::vector<char32_t> line_cps;
    std::vector<std::size_t> line_offsets;
    decode_utf8_to_cps(line, line_cps, line_offsets, true);

    std::vector<char32_t> q_cps;
    std::vector<std::size_t> q_offsets;
    decode_utf8_to_cps(query, q_cps, q_offsets, true);
    if (q_cps.empty()) return hits;

    const std::size_t n = line_cps.size();
    const std::size_t m = q_cps.size();
    if (m > n) return hits;

    for (std::size_t i = 0; i + m <= n; ++i) {
        bool eq = true;
        for (std::size_t k = 0; k < m; ++k) {
            if (line_cps[i + k] != q_cps[k]) {
                eq = false;
                break;
            }
        }
        if (!eq) continue;

        const bool left_ok = (i == 0) || !is_word_codepoint(line_cps[i - 1]);
        const bool right_ok = (i + m >= n) || !is_word_codepoint(line_cps[i + m]);
        if (left_ok && right_ok) {
            hits.push_back({line_offsets[i], line_offsets[i + m]});
        }
    }
    return hits;
}

void print_line_with_highlight(const std::string& line, const std::string& query) {
    auto print_with_note_links = [&](const std::string& s) {
        std::size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '[') {
                std::size_t j = i + 1;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) ++j;
                if (j > i + 1 && j < s.size() && s[j] == ']') {
                    std::cout << "\033[4;36m" << s.substr(i, j - i + 1) << "\033[0m";
                    i = j + 1;
                    continue;
                }
            }
            std::cout << s[i];
            ++i;
        }
    };

    if (query.empty()) {
        print_with_note_links(line);
        return;
    }
    const auto hits = find_word_matches_in_line(line, query);
    if (hits.empty()) {
        std::cout << line;
        return;
    }

    std::size_t cursor = 0;
    for (const auto& h : hits) {
        if (h.first > cursor) {
            std::cout << line.substr(cursor, h.first - cursor);
        }
        std::cout << "\033[30;43m" << line.substr(h.first, h.second - h.first) << "\033[0m";
        cursor = h.second;
    }
    if (cursor < line.size()) {
        std::cout << line.substr(cursor);
    }
}

void clear_screen() {
    // Soft redraw to reduce flicker: move cursor home, then clear lines as we print.
    std::cout << "\033[H";
}

void draw_small_window(const WinSize& ws) {
    clear_screen();
    const int row = std::max(1, ws.rows / 2 - 1);
    move_cursor(row, 1);
    std::cout << fit_left(" Window too small for reader ", ws.cols);
    std::ostringstream info;
    info << " Current: " << ws.cols << "x" << ws.rows << "  Required: " << kMinCols << "x" << kMinRows;
    move_cursor(row + 1, 1);
    std::cout << fit_left(info.str(), ws.cols);
    move_cursor(row + 2, 1);
    std::cout << fit_left(" Resize terminal or press q to quit ", ws.cols);
    std::cout.flush();
}

void draw_page(const std::vector<std::string>& lines,
               int top,
               const WinSize& ws,
               const ReaderLayout& layout,
               const std::string& title,
               const std::string& search_query,
               const std::string& bottom_override) {
    clear_screen();

    const int total = static_cast<int>(lines.size());
    const int cur = std::min(top + 1, std::max(1, total));
    const int percent = total <= 1 ? 0 : ((cur - 1) * 100 / (total - 1));
    const int page_lines = std::max(1, layout.page_rows);
    const int page_index = ((cur - 1) / page_lines) + 1;
    const int page_total = std::max(1, (total + page_lines - 1) / page_lines);

    std::ostringstream right_stream;
    right_stream << "page " << page_index << "/" << page_total << "  " << percent << "%";
    const std::string right = right_stream.str();

    std::string left = " " + title + " ";
    if (ws.cols > 0) {
        const int max_left = std::max(0, ws.cols - static_cast<int>(right.size()) - 1);
        left = fit_left(left, max_left);
    }
    std::string top_line = fit_left(left + right, ws.cols);
    std::cout << "\033[7m" << top_line << "\033[m\033[K\r\n";

    const int left_padding = std::max(0, (ws.cols - kMaxTextWidth) / 2);
    for (int r = 0; r < layout.page_rows; ++r) {
        const int idx = top + r;
        std::cout << std::string(static_cast<std::size_t>(left_padding), ' ');
        if (idx < static_cast<int>(lines.size())) {
            print_line_with_highlight(lines[static_cast<std::size_t>(idx)], search_query);
        }
        std::cout << "\033[K";
        if (r < layout.page_rows - 1) {
            std::cout << "\r\n";
        }
    }

    std::cout << "\r\n";
    std::string bottom_left = bottom_override.empty()
        ? " <- prev page  -> next page  / search  up/down or [/] matches  t toc  f footnotes  c copy paragraph  q quit "
        : bottom_override;
    std::string bottom_right = get_system_info();
    std::string bottom = fit_with_right(bottom_left, bottom_right, ws.cols);
    std::cout << "\033[7m" << bottom << "\033[m\033[K";
    std::cout.flush();
}

void search_job_start(SearchJob& job, const std::string& query) {
    job.active = !query.empty();
    job.query = query;
    job.next_line = 0;
    job.hits.clear();
}

bool search_job_step(SearchJob& job, const std::vector<std::string>& lines, std::size_t chunk_lines) {
    if (!job.active) return false;
    if (job.query.empty()) {
        job.active = false;
        return true;
    }
    if (job.next_line >= lines.size()) {
        job.active = false;
        return true;
    }

    const std::size_t end = std::min(lines.size(), job.next_line + std::max<std::size_t>(1, chunk_lines));
    for (std::size_t i = job.next_line; i < end; ++i) {
        if (!find_word_matches_in_line(lines[i], job.query).empty()) {
            job.hits.push_back(static_cast<int>(i));
        }
    }
    job.next_line = end;
    if (job.next_line >= lines.size()) {
        job.active = false;
    }
    return true;
}

int search_progress_percent(const SearchJob& job, const std::vector<std::string>& lines) {
    if (lines.empty()) return 100;
    if (!job.active) return 100;
    return static_cast<int>((job.next_line * 100U) / lines.size());
}

bool read_search_query(const WinSize& ws, std::string& query) {
    std::string input;

    while (true) {
        std::string line = "/" + input + "_";
        move_cursor(ws.rows, 1);
        std::cout << "\033[7m" << fit_left(line, ws.cols) << "\033[m";
        std::cout.flush();

        unsigned char c = 0;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            query = input;
            return true;
        }
        if (c == 27) {
            return false;
        }
        if (c == 127 || c == 8) {
            if (!input.empty()) input.pop_back();
            continue;
        }
        if (c >= 32) {
            input.push_back(static_cast<char>(c));
        }
    }
}

int read_key() {
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return KEY_NONE;
    }
    if (c != '\x1b') {
        return c;
    }

    unsigned char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] == '<') {
            std::string payload;
            payload.reserve(24);
            while (payload.size() < 32) {
                unsigned char ch = 0;
                if (read(STDIN_FILENO, &ch, 1) != 1) return KEY_NONE;
                payload.push_back(static_cast<char>(ch));
                if (ch == 'M' || ch == 'm') break;
            }

            int b = 0;
            int x = 0;
            int y = 0;
            char state = 0;
            if (std::sscanf(payload.c_str(), "%d;%d;%d%c", &b, &x, &y, &state) == 4) {
                (void)x;
                (void)y;
                (void)state;
                if (b == 64) return KEY_MOUSE_WHEEL_UP;
                if (b == 65) return KEY_MOUSE_WHEEL_DOWN;
            }
            return KEY_NONE;
        }

        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return KEY_HOME;
                    case '4': return KEY_END;
                    case '5': return KEY_PAGE_UP;
                    case '6': return KEY_PAGE_DOWN;
                    case '7': return KEY_HOME;
                    case '8': return KEY_END;
                    default: return KEY_NONE;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return KEY_ARROW_UP;
                case 'B': return KEY_ARROW_DOWN;
                case 'C': return KEY_ARROW_RIGHT;
                case 'D': return KEY_ARROW_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                default: return KEY_NONE;
            }
        }
    }

    return '\x1b';
}

bool is_roman_heading_token(const std::string& raw_line) {
    std::string line = trim_ascii(raw_line);
    if (line.empty() || line.size() > 20) return false;

    while (!line.empty() &&
           (line.back() == '.' || line.back() == ':' || line.back() == ')' || line.back() == ']')) {
        line.pop_back();
    }
    if (line.empty()) return false;
    if (line.find(' ') != std::string::npos || line.find('\t') != std::string::npos) return false;

    int roman_chars = 0;
    for (char c : line) {
        const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u == 'I' || u == 'V' || u == 'X' || u == 'L' || u == 'C' || u == 'D' || u == 'M') {
            ++roman_chars;
            continue;
        }
        return false;
    }
    return roman_chars >= 1;
}

bool looks_like_heading_line(const std::string& raw_line) {
    const std::string line = trim_ascii(raw_line);
    if (line.empty()) return false;
    if (line.size() > 100) return false;
    if (line[0] == '#') return true;

    if (starts_with_ci_ascii(line, "chapter ") ||
        starts_with_ci_ascii(line, "part ") ||
        starts_with_ci_ascii(line, "section ") ||
        starts_with_ci_ascii(line, "prologue") ||
        starts_with_ci_ascii(line, "epilogue") ||
        starts_with_ci_ascii(line, "introduction") ||
        starts_with_ci_ascii(line, "preface")) {
        return true;
    }

    if (starts_with_ci_ascii(line, "глава ") ||
        starts_with_ci_ascii(line, "часть ") ||
        starts_with_ci_ascii(line, "раздел ") ||
        starts_with_ci_ascii(line, "пролог") ||
        starts_with_ci_ascii(line, "эпилог") ||
        starts_with_ci_ascii(line, "введение") ||
        starts_with_ci_ascii(line, "предисловие")) {
        return true;
    }

    if (is_roman_heading_token(line)) {
        return true;
    }

    int letters = 0;
    bool all_caps = true;
    for (char ch : line) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalpha(u)) {
            ++letters;
            if (std::islower(u)) all_caps = false;
        } else if (!(std::isdigit(u) || std::isspace(u) || ch == '-' || ch == ':' || ch == '.')) {
            all_caps = false;
        }
    }
    if (letters >= 4 && line.size() <= 50 && all_caps) {
        return true;
    }

    return false;
}

int detect_heading_level(const std::string& raw_line) {
    const std::string line = trim_ascii(raw_line);
    if (line.empty()) return 1;

    if (!line.empty() && line[0] == '#') {
        int level = 0;
        while (level < static_cast<int>(line.size()) && line[static_cast<std::size_t>(level)] == '#') {
            ++level;
        }
        return std::max(1, std::min(4, level));
    }

    if (starts_with_ci_ascii(line, "part ") || starts_with_ci_ascii(line, "часть ")) return 1;
    if (starts_with_ci_ascii(line, "chapter ") || starts_with_ci_ascii(line, "глава ")) return 2;
    if (starts_with_ci_ascii(line, "section ") || starts_with_ci_ascii(line, "раздел ")) return 3;
    if (is_roman_heading_token(line)) return 2;

    std::size_t i = 0;
    while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i])) != 0 || line[i] == '.')) ++i;
    if (i > 0 && i < line.size() && (line[i] == ')' || line[i] == ' ')) {
        int dots = 0;
        for (std::size_t j = 0; j < i; ++j) {
            if (line[j] == '.') ++dots;
        }
        return std::max(1, std::min(4, dots + 1));
    }

    return 2;
}

std::string normalize_heading_title(const std::string& raw_line) {
    std::string line = trim_ascii(raw_line);
    if (line.empty()) return line;

    // Remove markdown heading markers.
    std::size_t pos = 0;
    while (pos < line.size() && line[pos] == '#') ++pos;
    if (pos > 0) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        line = line.substr(pos);
    }

    // Collapse repeated spaces.
    std::string out;
    out.reserve(line.size());
    bool prev_space = false;
    for (char ch : line) {
        const bool is_space = (ch == ' ' || ch == '\t');
        if (is_space) {
            if (!prev_space) out.push_back(' ');
            prev_space = true;
        } else {
            out.push_back(ch);
            prev_space = false;
        }
    }
    return trim_ascii(out);
}

bool parse_footnote_entry_start(const std::string& raw_line, std::string& id, std::string& text) {
    const std::string line = trim_ascii(raw_line);
    if (line.size() < 2) return false;

    auto utf8_space_len = [&](std::size_t pos) -> std::size_t {
        if (pos >= line.size()) return 0;
        const unsigned char b0 = static_cast<unsigned char>(line[pos]);
        if (std::isspace(b0) != 0) return 1;
        if (b0 == 0xC2 && pos + 1 < line.size()) {
            const unsigned char b1 = static_cast<unsigned char>(line[pos + 1]);
            if (b1 == 0xA0) return 2; // NO-BREAK SPACE
        }
        if (b0 == 0xE2 && pos + 2 < line.size()) {
            const unsigned char b1 = static_cast<unsigned char>(line[pos + 1]);
            const unsigned char b2 = static_cast<unsigned char>(line[pos + 2]);
            // En quad..hair space + narrow no-break space
            if (b1 == 0x80 && (b2 >= 0x80 && b2 <= 0x8A)) return 3;
        }
        return 0;
    };

    // [12] text
    if (line[0] == '[') {
        if (line.size() > 4 && line[1] == '^') {
            std::size_t j = 2;
            while (j < line.size() && line[j] != ']') ++j;
            if (j < line.size() && line[j] == ']') {
                id = canonical_note_id(line.substr(2, j - 2));
                if (j + 1 < line.size() && line[j + 1] == ':') {
                    text = trim_ascii(line.substr(j + 2));
                } else {
                    text = trim_ascii(line.substr(j + 1));
                }
                return !id.empty();
            }
        }
        std::size_t i = 1;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])) != 0) {
            ++i;
        }
        if (i <= 1 || i >= line.size() || line[i] != ']') return false;
        id = canonical_note_id(line.substr(1, i - 1));
        text = trim_ascii(line.substr(i + 1));
        return !id.empty();
    }

    // 12) text / 12. text / 12: text / 12 text
    std::size_t i = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])) != 0) {
        ++i;
    }
    if (i == 0 || i >= line.size()) return false;
    id = canonical_note_id(line.substr(0, i));
    if (line[i] == ')' || line[i] == '.' || line[i] == ':') {
        text = trim_ascii(line.substr(i + 1));
        return !id.empty();
    }
    const std::size_t sep_len = utf8_space_len(i);
    if (sep_len > 0) {
        text = trim_ascii(line.substr(i + sep_len));
        return !id.empty() && !text.empty();
    }
    return !id.empty();
}

std::string canonical_note_id(const std::string& raw) {
    std::string id_digits;
    for (char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            id_digits.push_back(c);
        }
    }
    if (!id_digits.empty()) {
        std::size_t p = 0;
        while (p + 1 < id_digits.size() && id_digits[p] == '0') ++p;
        return id_digits.substr(p);
    }

    std::string id_text;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 || c == '_' || c == '-') {
            id_text.push_back(static_cast<char>(std::tolower(u)));
        }
    }
    if (!id_text.empty()) return id_text;
    if (raw.find('*') != std::string::npos) return "*";
    return "";
}

bool superscript_digit_value(char32_t cp, int& d) {
    switch (cp) {
        case U'⁰': d = 0; return true;
        case U'¹': d = 1; return true;
        case U'²': d = 2; return true;
        case U'³': d = 3; return true;
        case U'⁴': d = 4; return true;
        case U'⁵': d = 5; return true;
        case U'⁶': d = 6; return true;
        case U'⁷': d = 7; return true;
        case U'⁸': d = 8; return true;
        case U'⁹': d = 9; return true;
        default: return false;
    }
}

std::vector<FootnoteEntry> parse_footnotes(const std::vector<std::string>& source_lines,
                                           const std::vector<int>& notes_heads,
                                           int notes_section_start) {
    std::vector<FootnoteEntry> notes;
    int scan_start = 0;
    if (!notes_heads.empty()) scan_start = std::max(scan_start, std::max(0, notes_heads.front()));
    if (notes_section_start >= 0) scan_start = std::max(scan_start, notes_section_start);
    struct StartItem {
        int line_idx{0};
        std::string id;
        std::string first_text;
    };
    std::vector<StartItem> starts;
    for (int i = scan_start; i < static_cast<int>(source_lines.size()); ++i) {
        std::string id;
        std::string text;
        if (parse_footnote_entry_start(source_lines[static_cast<std::size_t>(i)], id, text)) {
            starts.push_back({i, id, text});
        }
    }

    for (int si = 0; si < static_cast<int>(starts.size()); ++si) {
        const StartItem& cur = starts[static_cast<std::size_t>(si)];
        const std::string canon = canonical_note_id(cur.id);
        if (canon.empty()) continue;
        const int end_line = (si + 1 < static_cast<int>(starts.size()))
            ? starts[static_cast<std::size_t>(si + 1)].line_idx
            : static_cast<int>(source_lines.size());

        std::string full = trim_ascii(cur.first_text);
        for (int ln = cur.line_idx + 1; ln < end_line; ++ln) {
            const std::string part = source_lines[static_cast<std::size_t>(ln)];
            if (!full.empty()) full.push_back('\n');
            full += part;
        }
        full = trim_ascii(full);
        if (!full.empty()) {
            notes.push_back({canon, full, cur.line_idx});
        }
    }

    // Fallback: patterns like "12" on one line and footnote text on following lines.
    for (int i = scan_start; i + 1 < static_cast<int>(source_lines.size()); ++i) {
        const std::string first = trim_ascii(source_lines[static_cast<std::size_t>(i)]);
        const std::string id = canonical_note_id(first);
        if (id.empty() || first.size() != id.size()) continue;

        int j = i + 1;
        while (j < static_cast<int>(source_lines.size()) &&
               trim_ascii(source_lines[static_cast<std::size_t>(j)]).empty()) {
            ++j;
        }
        if (j >= static_cast<int>(source_lines.size())) continue;
        std::string full;
        while (j < static_cast<int>(source_lines.size())) {
            const std::string line = source_lines[static_cast<std::size_t>(j)];
            const std::string t = trim_ascii(line);
            if (t.empty()) break;
            const std::string next_id = canonical_note_id(t);
            if (!next_id.empty() && t.size() == next_id.size() && next_id != id) break;
            if (!full.empty()) full.push_back('\n');
            full += line;
            ++j;
        }
        full = trim_ascii(full);
        if (!full.empty()) notes.push_back({id, full, i});
    }
    return notes;
}

std::set<std::string> collect_footnote_markers_in_page(const std::vector<std::string>& lines,
                                                       int top,
                                                       int page_rows,
                                                       const std::set<std::string>& known_ids) {
    std::set<std::string> out;
    auto add_marker = [&](const std::string& raw) {
        const std::string id = canonical_note_id(raw);
        if (id.empty()) return;
        if (!known_ids.empty() && known_ids.find(id) == known_ids.end()) return;
        out.insert(id);
    };

    const int end = std::min(static_cast<int>(lines.size()), top + std::max(1, page_rows));
    for (int i = std::max(0, top); i < end; ++i) {
        const std::string& s = lines[static_cast<std::size_t>(i)];

        // [12], [^12], [a], [^note-id]
        for (std::size_t p = 0; p < s.size(); ++p) {
            if (s[p] != '[') continue;
            std::size_t j = p + 1;
            if (j < s.size() && s[j] == '^') ++j;
            const std::size_t id_start = j;
            while (j < s.size()) {
                const unsigned char ch = static_cast<unsigned char>(s[j]);
                if (std::isalnum(ch) != 0 || s[j] == '_' || s[j] == '-' || s[j] == '*') {
                    ++j;
                    continue;
                }
                break;
            }
            if (j > id_start && j < s.size() && s[j] == ']') {
                add_marker(s.substr(id_start, j - id_start));
                p = j;
            }
        }

        // Superscript references like ¹².
        std::size_t p = 0;
        while (p < s.size()) {
            char32_t cp = 0;
            const std::size_t n = utf8_decode_one(s, p, cp);
            int d = 0;
            if (!superscript_digit_value(cp, d)) {
                p += std::max<std::size_t>(1, n);
                continue;
            }

            std::string digits;
            digits.push_back(static_cast<char>('0' + d));
            p += std::max<std::size_t>(1, n);
            while (p < s.size()) {
                char32_t cp2 = 0;
                const std::size_t n2 = utf8_decode_one(s, p, cp2);
                int d2 = 0;
                if (!superscript_digit_value(cp2, d2)) break;
                digits.push_back(static_cast<char>('0' + d2));
                p += std::max<std::size_t>(1, n2);
            }
            add_marker(digits);
        }
    }
    return out;
}

std::optional<int> show_footnotes_menu(const WinSize& ws,
                                       const std::vector<std::pair<std::string, std::string>>& items) {
    if (items.empty()) return std::nullopt;
    int selected = 0;
    int scroll = 0;

    while (true) {
        clear_screen();
        const int body_rows = std::max(1, ws.rows - 3);
        adjust_scroll(selected, body_rows, scroll);

        std::ostringstream head;
        head << " FOOTNOTES  " << (selected + 1) << "/" << items.size();
        std::cout << "\033[7m" << fit_left(head.str(), ws.cols) << "\033[m\033[K\r\n";

        for (int r = 0; r < body_rows; ++r) {
            const int idx = scroll + r;
            if (idx < static_cast<int>(items.size())) {
                std::string preview = items[static_cast<std::size_t>(idx)].second;
                std::size_t nl = preview.find('\n');
                if (nl != std::string::npos) preview = preview.substr(0, nl);
                preview = collapse_spaces_ascii(preview);
                std::string line = (idx == selected ? "> [" : "  [") +
                                   items[static_cast<std::size_t>(idx)].first + "] " + preview;
                if (idx == selected) {
                    std::cout << "\033[7m" << fit_left(line, ws.cols) << "\033[m\033[K";
                } else {
                    std::cout << fit_left(line, ws.cols) << "\033[K";
                }
            } else {
                std::cout << fit_left("", ws.cols) << "\033[K";
            }
            if (r < body_rows - 1) std::cout << "\r\n";
        }
        std::cout << "\r\n";
        std::string footer = " up/down or wheel move  enter open  esc/q/f back ";
        std::cout << "\033[7m" << fit_left(footer, ws.cols) << "\033[m\033[K";
        std::cout.flush();

        const int key = read_key();
        if (key == KEY_NONE) continue;
        if (key == '\x1b' || key == 'q' || key == 'f') return std::nullopt;
        if (key == '\r' || key == '\n') return selected;
        if ((key == KEY_ARROW_UP || key == KEY_MOUSE_WHEEL_UP) && selected > 0) {
            --selected;
        } else if ((key == KEY_ARROW_DOWN || key == KEY_MOUSE_WHEEL_DOWN) &&
                   selected + 1 < static_cast<int>(items.size())) {
            ++selected;
        } else if (key == KEY_PAGE_UP) {
            selected = std::max(0, selected - body_rows);
        } else if (key == KEY_PAGE_DOWN) {
            selected = std::min(static_cast<int>(items.size()) - 1, selected + body_rows);
        }
    }
}

void show_footnote_view(const WinSize& ws, const std::string& id, const std::string& text) {
    const int body_rows = std::max(1, ws.rows - 3);
    const std::vector<std::string> wrapped = wrap_lines(text, std::max(20, ws.cols - 4));
    int top = 0;
    while (true) {
        clear_screen();
        std::ostringstream head;
        head << " FOOTNOTE [" << id << "] ";
        std::cout << "\033[7m" << fit_left(head.str(), ws.cols) << "\033[m\033[K\r\n";
        for (int r = 0; r < body_rows; ++r) {
            const int idx = top + r;
            std::string line = (idx < static_cast<int>(wrapped.size())) ? ("  " + wrapped[static_cast<std::size_t>(idx)]) : "";
            std::cout << fit_left(line, ws.cols) << "\033[K";
            if (r < body_rows - 1) std::cout << "\r\n";
        }
        std::cout << "\r\n";
        std::string footer = " up/down scroll  c copy footnote  esc/q/f back ";
        std::cout << "\033[7m" << fit_left(footer, ws.cols) << "\033[m\033[K";
        std::cout.flush();

        const int key = read_key();
        if (key == KEY_NONE) continue;
        if (key == '\x1b' || key == 'q' || key == 'f') return;
        if ((key == KEY_ARROW_UP || key == KEY_MOUSE_WHEEL_UP) && top > 0) {
            --top;
        } else if ((key == KEY_ARROW_DOWN || key == KEY_MOUSE_WHEEL_DOWN) &&
                   top + body_rows < static_cast<int>(wrapped.size())) {
            ++top;
        } else if (key == KEY_PAGE_UP) {
            top = std::max(0, top - body_rows);
        } else if (key == KEY_PAGE_DOWN) {
            top = std::min(std::max(0, static_cast<int>(wrapped.size()) - body_rows), top + body_rows);
        } else if (key == 'c') {
            copy_to_clipboard(text);
        }
    }
}

std::vector<TocEntry> parse_toc(const std::vector<std::string>& source_lines,
                                const std::vector<int>& first_wrapped_by_source) {
    std::vector<TocEntry> toc;
    toc.reserve(std::max<std::size_t>(16, source_lines.size() / 120));
    int last_source = -1000;
    std::string last_title;
    int last_level = -1;
    for (int i = 0; i < static_cast<int>(source_lines.size()); ++i) {
        if (looks_like_heading_line(source_lines[static_cast<std::size_t>(i)])) {
            if (i < 0 || i >= static_cast<int>(first_wrapped_by_source.size())) continue;
            const int wrapped_line = first_wrapped_by_source[static_cast<std::size_t>(i)];
            if (wrapped_line < 0) continue;
            TocEntry e;
            e.source_line_idx = i;
            e.level = detect_heading_level(source_lines[static_cast<std::size_t>(i)]);
            e.title = normalize_heading_title(source_lines[static_cast<std::size_t>(i)]);
            if (e.title.empty()) continue;
            const bool near_duplicate = (i - last_source < 2) &&
                                        e.level == last_level &&
                                        e.title == last_title;
            if (near_duplicate) continue;
            toc.push_back(e);
            last_source = i;
            last_title = e.title;
            last_level = e.level;
        }
    }
    return toc;
}

std::vector<TocEntry> map_metadata_toc_to_wrapped(const std::vector<MetadataHeading>& headings,
                                                  const std::vector<std::string>& source_lines,
                                                  const std::vector<int>& first_wrapped_by_source,
                                                  const std::unordered_map<std::string, int>& source_line_by_epub_path,
                                                  const std::string& epub_path,
                                                  std::unordered_map<std::string, int>& epub_target_line_cache) {
    std::vector<TocEntry> toc;
    if (headings.empty()) return toc;

    std::vector<std::string> source_keys;
    source_keys.reserve(source_lines.size());
    for (const auto& s : source_lines) {
        source_keys.push_back(normalize_match_key(s));
    }

    int cursor = 0;
    int matched = 0;
    for (const auto& h : headings) {
        const std::string title = normalize_heading_title(h.title);
        if (title.empty()) continue;

        if (!h.src_path.empty() && !source_line_by_epub_path.empty() && !epub_path.empty()) {
            const int src_line = resolve_epub_target_line(epub_path,
                                                          h.src_path,
                                                          h.src_fragment,
                                                          source_line_by_epub_path,
                                                          epub_target_line_cache);
            if (src_line >= 0 && src_line < static_cast<int>(first_wrapped_by_source.size())) {
                const int line = first_wrapped_by_source[static_cast<std::size_t>(src_line)];
                if (line >= 0) {
                    TocEntry e;
                    e.source_line_idx = src_line;
                    e.level = std::max(1, std::min(4, h.level));
                    e.title = title;
                    if (toc.empty() || (e.source_line_idx >= toc.back().source_line_idx &&
                                        !(toc.back().source_line_idx == e.source_line_idx && toc.back().title == e.title))) {
                        toc.push_back(e);
                        ++matched;
                        cursor = std::max(cursor, src_line + 1);
                        continue;
                    }
                }
            }
        }

        const std::string key = normalize_match_key(title);
        if (key.empty()) continue;

        int best_idx = -1;
        int best_score = -1; // 2 exact, 1 contains
        for (int i = cursor; i < static_cast<int>(source_lines.size()); ++i) {
            const std::string& s = source_keys[static_cast<std::size_t>(i)];
            if (s.empty()) continue;
            int score = 0;
            if (s == key) score = 2;
            if (score > best_score) {
                best_score = score;
                best_idx = i;
                if (score == 2) break;
            }
        }
        if (best_idx < 0 || best_idx >= static_cast<int>(first_wrapped_by_source.size())) {
            continue;
        }

        const int line = first_wrapped_by_source[static_cast<std::size_t>(best_idx)];
        if (line < 0) continue;

        TocEntry e;
        e.source_line_idx = best_idx;
        e.level = std::max(1, std::min(4, h.level));
        e.title = title;
        if (!toc.empty() && e.source_line_idx < toc.back().source_line_idx) {
            continue;
        }
        if (!toc.empty() && toc.back().source_line_idx == e.source_line_idx && toc.back().title == e.title) {
            continue;
        }
        toc.push_back(e);
        cursor = best_idx + 1;
        ++matched;
    }

    // If metadata mapping quality is low, prefer fallback heuristic TOC.
    const int min_required = std::max(4, static_cast<int>(headings.size() / 3));
    if (matched < min_required) {
        toc.clear();
    }
    return toc;
}

void adjust_scroll(int selected, int visible_rows, int& scroll);

int nearest_toc_index(const std::vector<TocEntry>& toc, int current_source_line) {
    if (toc.empty()) return -1;
    for (int i = 0; i < static_cast<int>(toc.size()); ++i) {
        if (toc[static_cast<std::size_t>(i)].source_line_idx >= current_source_line) return i;
    }
    return static_cast<int>(toc.size()) - 1;
}

std::optional<int> show_toc_menu(const WinSize& ws, const std::vector<TocEntry>& toc, int current_source_line) {
    if (toc.empty()) {
        return std::nullopt;
    }

    int selected = nearest_toc_index(toc, current_source_line);
    if (selected < 0) selected = 0;
    int scroll = 0;

    while (true) {
        clear_screen();
        const int body_rows = std::max(1, ws.rows - 3);
        adjust_scroll(selected, body_rows, scroll);

        int current_idx = 0;
        for (int i = 0; i < static_cast<int>(toc.size()); ++i) {
            if (toc[static_cast<std::size_t>(i)].source_line_idx <= current_source_line) current_idx = i;
        }

        std::ostringstream head;
        head << " TABLE OF CONTENTS  " << (selected + 1) << "/" << toc.size();
        std::cout << "\033[7m" << fit_left(head.str(), ws.cols) << "\033[m\033[K\r\n";
        const int title_w = std::max(8, ws.cols - 8);
        for (int r = 0; r < body_rows; ++r) {
            const int idx = scroll + r;
            if (idx < static_cast<int>(toc.size())) {
                const bool is_sel = (idx == selected);
                const bool is_cur = (idx == current_idx);
                const TocEntry& entry = toc[static_cast<std::size_t>(idx)];
                const std::string indent(static_cast<std::size_t>(std::max(0, entry.level - 1) * 2), ' ');
                std::string title = indent + entry.title;
                if (static_cast<int>(title.size()) > title_w) {
                    if (title_w > 3) title = title.substr(0, static_cast<std::size_t>(title_w - 3)) + "...";
                    else title = title.substr(0, static_cast<std::size_t>(title_w));
                }
                std::string num = std::to_string(idx + 1);
                if (num.size() < 2) num = " " + num;
                std::string row = (is_sel ? "> " : "  ");
                row += (is_cur ? "* " : "  ");
                row += num + " " + title;
                if (is_sel) {
                    std::cout << "\033[7m" << fit_left(row, ws.cols) << "\033[m\033[K";
                } else {
                    std::cout << fit_left(row, ws.cols) << "\033[K";
                }
            } else {
                std::cout << fit_left("", ws.cols) << "\033[K";
            }
            if (r < body_rows - 1) std::cout << "\r\n";
        }

        std::cout << "\r\n";
        std::string footer = " up/down move  pgup/pgdn fast  enter jump  esc/q/t close ";
        std::cout << "\033[7m" << fit_left(footer, ws.cols) << "\033[m\033[K";
        std::cout.flush();

        const int key = read_key();
        if (key == KEY_NONE) continue;
        if (key == 'q' || key == 't' || key == '\x1b') {
            return std::nullopt;
        }
        if (key == '\r' || key == '\n') {
            return toc[static_cast<std::size_t>(selected)].source_line_idx;
        }
        if ((key == KEY_ARROW_UP || key == KEY_MOUSE_WHEEL_UP) && selected > 0) {
            --selected;
        } else if ((key == KEY_ARROW_DOWN || key == KEY_MOUSE_WHEEL_DOWN) &&
                   selected + 1 < static_cast<int>(toc.size())) {
            ++selected;
        } else if (key == KEY_PAGE_UP) {
            selected = std::max(0, selected - body_rows);
        } else if (key == KEY_PAGE_DOWN) {
            selected = std::min(static_cast<int>(toc.size()) - 1, selected + body_rows);
        } else if (key == KEY_HOME) {
            selected = 0;
        } else if (key == KEY_END) {
            selected = static_cast<int>(toc.size()) - 1;
        }
    }
}

std::string config_path();
std::string state_path();
bool ensure_bookr_home(std::string& error);

bool save_reader_position(const std::string& path, int top, std::string& error) {
    if (!ensure_bookr_home(error)) {
        return false;
    }
    std::vector<std::pair<std::string, int>> items;
    {
        std::ifstream in(state_path());
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const std::string p = line.substr(0, tab);
            try {
                const int v = std::stoi(line.substr(tab + 1));
                items.push_back({p, v});
            } catch (...) {
            }
        }
    }

    bool updated = false;
    for (auto& it : items) {
        if (it.first == path) {
            it.second = top;
            updated = true;
            break;
        }
    }
    if (!updated) {
        items.push_back({path, top});
    }

    std::ofstream out(state_path(), std::ios::trunc);
    if (!out) {
        error = "Cannot write reader state: " + state_path();
        return false;
    }
    for (const auto& it : items) {
        out << it.first << "\t" << it.second << "\n";
    }
    return true;
}

std::optional<int> load_reader_position(const std::string& path) {
    std::ifstream in(state_path());
    if (!in) {
        const char* home = std::getenv("HOME");
        if (home && home[0]) {
            const std::string legacy_a = std::string(home) + "/.bookr/state";
            const std::string legacy_b = std::string(home) + "/.bookr_state";
            for (const std::string& legacy : {legacy_a, legacy_b}) {
                std::ifstream legacy_in(legacy);
                if (legacy_in) {
                    std::string err;
                    if (ensure_bookr_home(err)) {
                        std::ofstream out(state_path(), std::ios::trunc);
                        out << legacy_in.rdbuf();
                        out.close();
                        in.open(state_path());
                        if (in) break;
                    }
                }
            }
        }
    }
    if (!in) return std::nullopt;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        if (line.substr(0, tab) != path) continue;
        try {
            return std::stoi(line.substr(tab + 1));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool copy_to_clipboard(const std::string& text) {
    FILE* pipe = popen("pbcopy", "w");
    if (!pipe) return false;
    std::fwrite(text.data(), 1, text.size(), pipe);
    return pclose(pipe) == 0;
}

std::string extract_paragraph(const std::vector<std::string>& lines, int top_line) {
    if (lines.empty()) return "";
    int idx = std::min(top_line, static_cast<int>(lines.size()) - 1);
    while (idx < static_cast<int>(lines.size()) &&
           trim_ascii(lines[static_cast<std::size_t>(idx)]).empty()) ++idx;
    if (idx >= static_cast<int>(lines.size())) {
        idx = std::min(top_line, static_cast<int>(lines.size()) - 1);
        while (idx >= 0 && trim_ascii(lines[static_cast<std::size_t>(idx)]).empty()) --idx;
        if (idx < 0) return "";
    }

    int start = idx;
    while (start > 0 &&
           !trim_ascii(lines[static_cast<std::size_t>(start - 1)]).empty()) --start;
    int end = idx;
    while (end + 1 < static_cast<int>(lines.size()) &&
           !trim_ascii(lines[static_cast<std::size_t>(end + 1)]).empty()) ++end;

    std::ostringstream out;
    for (int i = start; i <= end; ++i) {
        out << lines[static_cast<std::size_t>(i)];
        if (i < end) out << "\n";
    }
    return out.str();
}

namespace fs = std::filesystem;

std::string bookr_home_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) {
        return ".config/bookr";
    }
    return std::string(home) + "/.config/bookr";
}

bool ensure_bookr_home(std::string& error) {
    std::error_code ec;
    const fs::path dir(bookr_home_dir());
    if (fs::exists(dir, ec)) {
        if (!fs::is_directory(dir, ec)) {
            error = "Path exists but is not a directory: " + dir.string();
            return false;
        }
        return true;
    }
    if (!fs::create_directories(dir, ec)) {
        error = "Cannot create config directory: " + dir.string();
        return false;
    }
    return true;
}

std::string config_path() {
    return bookr_home_dir() + "/folders";
}

std::string state_path() {
    return bookr_home_dir() + "/state";
}

bool is_supported_book_extension(const std::string& ext) {
    return ext == "txt" || ext == "fb2" || ext == "epub" || ext == "pdf";
}

bool is_supported_book_path(const fs::path& p) {
    if (!fs::is_regular_file(p)) return false;
    return is_supported_book_extension(lower_ext(p.string()));
}

std::vector<std::string> load_library_folders() {
    std::vector<std::string> folders;
    std::ifstream in(config_path());
    if (!in) {
        const char* home = std::getenv("HOME");
        if (home && home[0]) {
            const std::string legacy_a = std::string(home) + "/.bookr/folders";
            const std::string legacy_b = std::string(home) + "/.bookr_folders";
            for (const std::string& legacy : {legacy_a, legacy_b}) {
                std::ifstream legacy_in(legacy);
                if (legacy_in) {
                    std::string err;
                    if (ensure_bookr_home(err)) {
                        std::ofstream out(config_path(), std::ios::trunc);
                        out << legacy_in.rdbuf();
                        out.close();
                        in.open(config_path());
                        if (in) break;
                    }
                }
            }
        }
    }
    if (!in) return folders;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (std::find(folders.begin(), folders.end(), line) == folders.end()) {
            folders.push_back(line);
        }
    }
    return folders;
}

bool save_library_folders(const std::vector<std::string>& folders, std::string& error) {
    if (!ensure_bookr_home(error)) {
        return false;
    }
    std::ofstream out(config_path(), std::ios::trunc);
    if (!out) {
        error = "Cannot write config file: " + config_path();
        return false;
    }
    for (const std::string& folder : folders) {
        out << folder << "\n";
    }
    return true;
}

bool add_library_folder(const std::string& folder_arg, std::string& error) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(fs::path(folder_arg), ec), ec);
    if (ec) {
        error = "Invalid path: " + folder_arg;
        return false;
    }
    if (!fs::exists(p)) {
        error = "Folder does not exist: " + p.string();
        return false;
    }
    if (!fs::is_directory(p)) {
        error = "Not a directory: " + p.string();
        return false;
    }

    std::vector<std::string> folders = load_library_folders();
    const std::string canonical = p.string();
    if (std::find(folders.begin(), folders.end(), canonical) == folders.end()) {
        folders.push_back(canonical);
    }
    return save_library_folders(folders, error);
}

bool remove_library_folder(const std::string& folder_arg, std::string& error) {
    std::vector<std::string> folders = load_library_folders();
    if (folders.empty()) {
        error = "No folders configured.";
        return false;
    }

    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(fs::path(folder_arg), ec), ec);
    const std::string canonical = ec ? folder_arg : p.string();

    const std::size_t old_size = folders.size();
    folders.erase(std::remove_if(folders.begin(), folders.end(), [&](const std::string& s) {
        return s == folder_arg || s == canonical;
    }), folders.end());

    if (folders.size() == old_size) {
        error = "Folder not found in library: " + folder_arg;
        return false;
    }

    return save_library_folders(folders, error);
}

struct BookEntry {
    std::string path;
    std::string name;
    fs::file_time_type mtime{};
};

enum class LibrarySortMode {
    ByName,
    ByDate
};

std::vector<BookEntry> collect_books_from_folder(const std::string& folder, LibrarySortMode sort_mode) {
    std::vector<BookEntry> books;
    std::error_code ec;
    fs::path root(folder);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return books;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::path p = it->path();
        if (is_supported_book_path(p)) {
            BookEntry e;
            e.path = p.string();
            e.name = base_name(e.path);
            std::error_code te;
            e.mtime = fs::last_write_time(p, te);
            if (te) {
                e.mtime = fs::file_time_type::min();
            }
            books.push_back(e);
        }
        it.increment(ec);
    }

    if (sort_mode == LibrarySortMode::ByDate) {
        std::sort(books.begin(), books.end(), [](const BookEntry& a, const BookEntry& b) {
            if (a.mtime != b.mtime) return a.mtime > b.mtime;
            return a.name < b.name;
        });
    } else {
        std::sort(books.begin(), books.end(), [](const BookEntry& a, const BookEntry& b) {
            if (a.name != b.name) return a.name < b.name;
            return a.path < b.path;
        });
    }
    books.erase(std::unique(books.begin(), books.end(), [](const BookEntry& a, const BookEntry& b) {
        return a.path == b.path;
    }), books.end());
    return books;
}

struct LibraryState {
    int folder_idx{0};
    int book_idx{0};
    int folder_scroll{0};
    int book_scroll{0};
    bool focus_folders{true};
};

void clamp_library_state(const std::vector<std::string>& folders,
                         const std::vector<std::vector<BookEntry>>& books_by_folder,
                         LibraryState& st) {
    if (folders.empty()) {
        st.folder_idx = 0;
        st.book_idx = 0;
        st.folder_scroll = 0;
        st.book_scroll = 0;
        return;
    }

    if (st.folder_idx < 0) st.folder_idx = 0;
    if (st.folder_idx >= static_cast<int>(folders.size())) st.folder_idx = static_cast<int>(folders.size()) - 1;

    const std::vector<BookEntry>& books = books_by_folder[static_cast<std::size_t>(st.folder_idx)];
    if (books.empty()) {
        st.book_idx = 0;
        st.book_scroll = 0;
        return;
    }
    if (st.book_idx < 0) st.book_idx = 0;
    if (st.book_idx >= static_cast<int>(books.size())) st.book_idx = static_cast<int>(books.size()) - 1;
}

void adjust_scroll(int selected, int visible_rows, int& scroll) {
    if (visible_rows < 1) {
        scroll = 0;
        return;
    }
    if (selected < scroll) {
        scroll = selected;
    }
    if (selected >= scroll + visible_rows) {
        scroll = selected - visible_rows + 1;
    }
    if (scroll < 0) scroll = 0;
}

void draw_library_browser(const WinSize& ws,
                          const std::vector<std::string>& folders,
                          const std::vector<std::vector<BookEntry>>& books_by_folder,
                          const LibraryState& st,
                          LibrarySortMode sort_mode) {
    clear_screen();

    const int rows = std::max(1, ws.rows - 2);
    const int left_w = std::max(24, ws.cols / 3);
    const int right_w = std::max(10, ws.cols - left_w - 3);

    std::string header = " BOOKR LIBRARY ";
    header = fit_left(header, ws.cols);
    std::cout << "\033[7m" << header << "\033[m\r\n";

    if (folders.empty()) {
        for (int r = 0; r < rows; ++r) {
            if (r == rows / 2 - 1) {
                std::cout << fit_left("No folders configured. Use: bookr --add-folder /path/to/books", ws.cols);
            } else if (r == rows / 2) {
                std::cout << fit_left("Press q to quit.", ws.cols);
            } else {
                std::cout << fit_left("", ws.cols);
            }
            if (r < rows - 1) std::cout << "\r\n";
        }
    } else {
        for (int r = 0; r < rows; ++r) {
            const int fi = st.folder_scroll + r;
            const int bi = st.book_scroll + r;

            std::string left = "";
            if (fi < static_cast<int>(folders.size())) {
                const bool sel = (fi == st.folder_idx);
                left = (sel ? (st.focus_folders ? "> " : "* ") : "  ") +
                       base_name(folders[static_cast<std::size_t>(fi)]);
            }
            left = fit_left(left, left_w);

            const std::vector<BookEntry>& books = books_by_folder[static_cast<std::size_t>(st.folder_idx)];
            std::string right = "";
            if (bi < static_cast<int>(books.size())) {
                const bool sel = (bi == st.book_idx);
                right = (sel ? (!st.focus_folders ? "> " : "* ") : "  ") +
                        books[static_cast<std::size_t>(bi)].name;
            }
            right = fit_left(right, right_w);

            std::cout << left << " | " << right;
            if (r < rows - 1) std::cout << "\r\n";
        }
    }

    std::cout << "\r\n";
    std::ostringstream footer;
    footer << " arrows move  left/right switch pane  enter open  s sort  x remove folder  r reload  q quit  ";
    if (!folders.empty()) {
        const std::vector<BookEntry>& books = books_by_folder[static_cast<std::size_t>(st.folder_idx)];
        footer << "folders " << (st.folder_idx + 1) << "/" << folders.size()
               << "  books " << (books.empty() ? 0 : st.book_idx + 1) << "/" << books.size();
    }
    footer << "  sort:" << (sort_mode == LibrarySortMode::ByName ? "name" : "date");
    std::cout << "\033[7m" << fit_left(footer.str(), ws.cols) << "\033[m";
    std::cout.flush();
}

std::optional<std::string> run_library_browser() {
    std::vector<std::string> folders = load_library_folders();
    LibrarySortMode sort_mode = LibrarySortMode::ByName;
    std::vector<std::vector<BookEntry>> books_by_folder;
    books_by_folder.reserve(folders.size());
    for (const std::string& folder : folders) {
        books_by_folder.push_back(collect_books_from_folder(folder, sort_mode));
    }

    ScreenGuard screen;
    screen.enter_fullscreen();
    TermiosGuard term;
    term.enable_raw();

    WinSize ws = get_winsize();
    if (ws.cols < 10) ws.cols = 80;

    LibraryState st;
    bool dirty = true;
    bool in_small_mode = false;

    while (true) {
        WinSize cur = get_winsize();
        if (cur.cols != ws.cols || cur.rows != ws.rows) {
            ws = cur;
            if (ws.cols < 10) ws.cols = 80;
            dirty = true;
        }

        const bool too_small = ws.cols < kMinCols || ws.rows < kMinRows;
        if (too_small) {
            if (dirty || !in_small_mode) {
                draw_small_window(ws);
                dirty = false;
            }
            in_small_mode = true;
            const int key = read_key();
            if (key == 'q') return std::nullopt;
            continue;
        }

        if (dirty || in_small_mode) {
            clamp_library_state(folders, books_by_folder, st);
            const int rows = std::max(1, ws.rows - 2);
            adjust_scroll(st.folder_idx, rows, st.folder_scroll);
            if (!folders.empty()) {
                adjust_scroll(st.book_idx, rows, st.book_scroll);
            }
            draw_library_browser(ws, folders, books_by_folder, st, sort_mode);
            dirty = false;
            in_small_mode = false;
        }

        const int key = read_key();
        if (key == KEY_NONE) continue;
        if (key == 'q') return std::nullopt;

        if (key == 'r') {
            folders = load_library_folders();
            books_by_folder.clear();
            books_by_folder.reserve(folders.size());
            for (const std::string& folder : folders) {
                books_by_folder.push_back(collect_books_from_folder(folder, sort_mode));
            }
            st = LibraryState{};
            dirty = true;
            continue;
        }
        if (key == 's' || key == 'S') {
            sort_mode = (sort_mode == LibrarySortMode::ByName) ? LibrarySortMode::ByDate : LibrarySortMode::ByName;
            books_by_folder.clear();
            books_by_folder.reserve(folders.size());
            for (const std::string& folder : folders) {
                books_by_folder.push_back(collect_books_from_folder(folder, sort_mode));
            }
            st.book_idx = 0;
            st.book_scroll = 0;
            dirty = true;
            continue;
        }
        if (key == 'x' || key == 'X') {
            if (!folders.empty()) {
                std::vector<std::string> updated = folders;
                updated.erase(updated.begin() + st.folder_idx);
                std::string error;
                if (save_library_folders(updated, error)) {
                    folders = updated;
                    books_by_folder.clear();
                    books_by_folder.reserve(folders.size());
                    for (const std::string& folder : folders) {
                        books_by_folder.push_back(collect_books_from_folder(folder, sort_mode));
                    }
                    if (st.folder_idx >= static_cast<int>(folders.size())) {
                        st.folder_idx = std::max(0, static_cast<int>(folders.size()) - 1);
                    }
                    st.book_idx = 0;
                    st.book_scroll = 0;
                    dirty = true;
                }
            }
            continue;
        }

        if (folders.empty()) {
            continue;
        }

        switch (key) {
            case KEY_ARROW_LEFT:
                st.focus_folders = true;
                dirty = true;
                break;
            case KEY_ARROW_RIGHT:
                st.focus_folders = false;
                dirty = true;
                break;
            case KEY_ARROW_UP:
            case KEY_MOUSE_WHEEL_UP:
                if (st.focus_folders) {
                    if (st.folder_idx > 0) {
                        --st.folder_idx;
                        st.book_idx = 0;
                        st.book_scroll = 0;
                    }
                } else if (st.book_idx > 0) {
                    --st.book_idx;
                }
                dirty = true;
                break;
            case KEY_ARROW_DOWN:
            case KEY_MOUSE_WHEEL_DOWN:
                if (st.focus_folders) {
                    if (st.folder_idx + 1 < static_cast<int>(folders.size())) {
                        ++st.folder_idx;
                        st.book_idx = 0;
                        st.book_scroll = 0;
                    }
                } else {
                    const auto& books = books_by_folder[static_cast<std::size_t>(st.folder_idx)];
                    if (st.book_idx + 1 < static_cast<int>(books.size())) {
                        ++st.book_idx;
                    }
                }
                dirty = true;
                break;
            case '\r':
            case '\n': {
                if (st.focus_folders) {
                    st.focus_folders = false;
                    dirty = true;
                } else {
                    const auto& books = books_by_folder[static_cast<std::size_t>(st.folder_idx)];
                    if (!books.empty()) {
                        return books[static_cast<std::size_t>(st.book_idx)].path;
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

int run_reader(const std::string& path) {
    const std::string title = base_name(path);
    const std::string ext = lower_ext(path);
    std::string text;
    try {
        text = normalize_text(to_text(path));
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::cerr << "If you open fb2/epub, install pandoc (brew install pandoc).\n";
        std::cerr << "If you open pdf, install poppler (brew install poppler).\n";
        return 1;
    }

    WinSize ws = get_winsize();
    if (ws.cols < 10) ws.cols = 80;

    std::unordered_map<std::string, int> source_line_by_epub_path;
    std::unordered_map<std::string, int> epub_target_line_cache;
    if (ext == "epub") {
        std::string spine_text;
        if (load_epub_text_from_spine(path, spine_text, source_line_by_epub_path)) {
            text = std::move(spine_text);
        }
    }

    const std::vector<std::string> source_lines = split_source_lines(text);
    const std::vector<MetadataHeading> metadata_headings = read_book_metadata_headings(path);
    const std::vector<int> notes_heading_lines = collect_notes_heading_lines(source_lines);
    const int notes_section_start = notes_heading_lines.empty() ? -1 : notes_heading_lines.front();

    std::vector<FootnoteEntry> footnotes = parse_footnotes(source_lines,
                                                           notes_heading_lines,
                                                           notes_section_start);
    ReaderLayout layout = compute_layout(ws);
    WrappedText wrapped = wrap_lines_with_map(source_lines, layout.text_cols);
    std::vector<std::string> lines = wrapped.lines;
    int top = 0;
    std::vector<TocEntry> toc = map_metadata_toc_to_wrapped(metadata_headings,
                                                            source_lines,
                                                            wrapped.first_wrapped_by_source,
                                                            source_line_by_epub_path,
                                                            (ext == "epub" ? path : std::string()),
                                                            epub_target_line_cache);
    if (toc.empty()) {
        toc = parse_toc(source_lines, wrapped.first_wrapped_by_source);
    }

    if (const std::optional<int> saved = load_reader_position(path); saved.has_value() && *saved > 0) {
        std::cout << "Continue from last position? [y/N]: ";
        std::string ans;
        std::getline(std::cin, ans);
        if (!ans.empty() && (ans[0] == 'y' || ans[0] == 'Y')) {
            top = std::max(0, *saved);
        }
    }

    std::string search_query;
    SearchJob search_job;
    int search_index = -1;
    std::string bottom_message;
    std::chrono::steady_clock::time_point message_expiry{};
    bool has_temporary_message = false;
    auto set_temporary_message = [&](const std::string& msg) {
        bottom_message = msg;
        has_temporary_message = !msg.empty();
        if (has_temporary_message) {
            message_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
    };

    ScreenGuard screen;
    screen.enter_fullscreen();

    TermiosGuard term;
    term.enable_raw();

    bool running = true;
    bool dirty = true;
    bool in_small_mode = false;
    bool needs_reflow = true;

    while (running) {
        if (has_temporary_message &&
            std::chrono::steady_clock::now() >= message_expiry) {
            bottom_message.clear();
            has_temporary_message = false;
            dirty = true;
        }

        WinSize cur = get_winsize();
        if (cur.cols != ws.cols || cur.rows != ws.rows) {
            ws = cur;
            if (ws.cols < 10) ws.cols = 80;
            needs_reflow = true;
            dirty = true;
        }

        const bool too_small = ws.cols < kMinCols || ws.rows < kMinRows;
        if (too_small) {
            if (dirty || !in_small_mode) {
                draw_small_window(ws);
                dirty = false;
            }
            in_small_mode = true;

            const int key = read_key();
            if (key == 'q') {
                running = false;
            }
            continue;
        }

        if (needs_reflow || in_small_mode) {
            layout = compute_layout(ws);
            wrapped = wrap_lines_with_map(source_lines, layout.text_cols);
            lines = wrapped.lines;
            toc = map_metadata_toc_to_wrapped(metadata_headings,
                                              source_lines,
                                              wrapped.first_wrapped_by_source,
                                              source_line_by_epub_path,
                                              (ext == "epub" ? path : std::string()),
                                              epub_target_line_cache);
            if (toc.empty()) {
                toc = parse_toc(source_lines, wrapped.first_wrapped_by_source);
            }
            const int page = std::max(1, layout.page_rows);
            const int max_top = std::max(0, static_cast<int>(lines.size()) - page);
            top = std::min(top, max_top);

            if (!search_query.empty()) {
                search_job_start(search_job, search_query);
                search_index = -1;
            } else {
                search_job.active = false;
                search_job.hits.clear();
                search_index = -1;
            }

            in_small_mode = false;
            needs_reflow = false;
            dirty = true;
        }

        if (search_job.active) {
            search_job_step(search_job, lines, 500);
            if (!search_job.active) {
                if (search_job.hits.empty()) {
                    search_index = -1;
                    set_temporary_message(" no whole-word matches for: " + search_query);
                } else {
                    search_index = 0;
                    const int page = std::max(1, layout.page_rows);
                    const int max_top = std::max(0, static_cast<int>(lines.size()) - page);
                    top = std::min(max_top, search_job.hits[static_cast<std::size_t>(search_index)]);
                    std::ostringstream msg;
                    msg << " found " << search_job.hits.size() << " whole-word matches for: " << search_query;
                    set_temporary_message(msg.str());
                }
            }
            dirty = true;
        }

        if (dirty) {
            std::string ui_message = bottom_message;
            if (search_job.active) {
                std::ostringstream msg;
                msg << " searching '" << search_query << "' "
                    << search_progress_percent(search_job, lines) << "%  hits:" << search_job.hits.size();
                ui_message = msg.str();
            }
            draw_page(lines, top, ws, layout, title, search_query, ui_message);
            dirty = false;
        }

        const int key = read_key();
        if (key == KEY_NONE) continue;

        const int page = std::max(1, layout.page_rows);
        const int max_top = std::max(0, static_cast<int>(lines.size()) - page);
        const int old_top = top;
        const std::string old_message = bottom_message;

        switch (key) {
            case 'q':
                running = false;
                break;
            case KEY_ARROW_RIGHT:
            case KEY_MOUSE_WHEEL_DOWN:
                top = std::min(max_top, top + page);
                bottom_message.clear();
                has_temporary_message = false;
                break;
            case KEY_ARROW_LEFT:
            case KEY_MOUSE_WHEEL_UP:
                top = std::max(0, top - page);
                bottom_message.clear();
                has_temporary_message = false;
                break;
            case 't': {
                int current_source_line = 0;
                if (top >= 0 && top < static_cast<int>(wrapped.source_line_by_wrapped.size())) {
                    current_source_line = wrapped.source_line_by_wrapped[static_cast<std::size_t>(top)];
                }
                const std::optional<int> jump_source = show_toc_menu(ws, toc, current_source_line);
                if (jump_source.has_value()) {
                    int jump_wrapped = 0;
                    if (*jump_source >= 0 &&
                        *jump_source < static_cast<int>(wrapped.first_wrapped_by_source.size())) {
                        jump_wrapped = wrapped.first_wrapped_by_source[static_cast<std::size_t>(*jump_source)];
                    }
                    if (jump_wrapped < 0) jump_wrapped = 0;
                    top = std::min(max_top, jump_wrapped);
                    set_temporary_message(" jumped via table of contents ");
                } else if (toc.empty()) {
                    set_temporary_message(" no table of contents found ");
                }
                break;
            }
            case 'c': {
                const std::string para = extract_paragraph(lines, top);
                if (para.empty()) {
                    set_temporary_message(" no paragraph to copy ");
                    break;
                }
                if (copy_to_clipboard(para)) {
                    set_temporary_message(" paragraph copied to clipboard ");
                } else {
                    set_temporary_message(" clipboard copy failed ");
                }
                break;
            }
            case 'f': {
                if (ext == "fb2") {
                    set_temporary_message(" сноски недоступны на данный момент ");
                    break;
                }
                std::set<std::string> known_note_ids;
                for (const auto& note : footnotes) {
                    if (!note.id.empty()) {
                        known_note_ids.insert(note.id);
                    }
                }
                const std::set<std::string> markers = collect_footnote_markers_in_page(
                    lines, top, layout.page_rows, known_note_ids);
                if (markers.empty()) {
                    set_temporary_message(" no footnote markers on this page ");
                    break;
                }

                int current_source_line = 0;
                if (top >= 0 && top < static_cast<int>(wrapped.source_line_by_wrapped.size())) {
                    current_source_line = wrapped.source_line_by_wrapped[static_cast<std::size_t>(top)];
                }
                std::vector<std::pair<std::string, std::string>> items;
                for (const std::string& id : markers) {
                    const FootnoteEntry* best = find_best_footnote_entry(
                        footnotes, id, current_source_line, notes_heading_lines);
                    if (best != nullptr) {
                        items.push_back({id, best->text});
                    } else {
                        items.push_back({id, "(not found in extracted notes)"});
                    }
                }
                const std::optional<int> chosen_idx = show_footnotes_menu(ws, items);
                if (chosen_idx.has_value() && *chosen_idx >= 0 &&
                    *chosen_idx < static_cast<int>(items.size())) {
                    const auto& chosen = items[static_cast<std::size_t>(*chosen_idx)];
                    if (chosen.second != "(not found in extracted notes)") {
                        show_footnote_view(ws, chosen.first, chosen.second);
                        set_temporary_message(" footnote [" + chosen.first + "] ");
                    } else {
                        show_footnote_view(ws, chosen.first, chosen.second);
                        set_temporary_message(" footnote [" + chosen.first + "] not found ");
                    }
                } else {
                    set_temporary_message(" footnotes closed ");
                }
                dirty = true;
                break;
            }
            case '/': {
                std::string query;
                if (!read_search_query(ws, query)) {
                    set_temporary_message(" search cancelled ");
                    break;
                }
                search_query = query;
                if (search_query.empty()) {
                    search_index = -1;
                    search_job.active = false;
                    search_job.hits.clear();
                    set_temporary_message(" search cleared ");
                    break;
                }
                search_job_start(search_job, search_query);
                search_index = -1;
                set_temporary_message(" searching... ");
                break;
            }
            case KEY_ARROW_DOWN:
            case ']':
                if (search_job.hits.empty()) {
                    set_temporary_message(search_job.active ? " still searching... " : " no active search; press / ");
                    break;
                }
                search_index = (search_index + 1) % static_cast<int>(search_job.hits.size());
                top = std::min(max_top, search_job.hits[static_cast<std::size_t>(search_index)]);
                bottom_message.clear();
                has_temporary_message = false;
                break;
            case KEY_ARROW_UP:
            case '[':
                if (search_job.hits.empty()) {
                    set_temporary_message(search_job.active ? " still searching... " : " no active search; press / ");
                    break;
                }
                search_index = (search_index - 1 + static_cast<int>(search_job.hits.size())) % static_cast<int>(search_job.hits.size());
                top = std::min(max_top, search_job.hits[static_cast<std::size_t>(search_index)]);
                bottom_message.clear();
                has_temporary_message = false;
                break;
            case '\x1b':
                if (search_job.active) {
                    search_job.active = false;
                    search_job.hits.clear();
                    search_index = -1;
                    set_temporary_message(" search cancelled ");
                }
                break;
            default:
                break;
        }

        if (top != old_top || bottom_message != old_message) {
            dirty = true;
        }
    }

    std::string save_error;
    save_reader_position(path, top, save_error);

    std::cout << "\033[2J\033[H";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");

    if (argc == 2) {
        const std::string arg = argv[1];
        if (arg == "--version") {
            std::cout << "Bookr " << bookr::VERSION << "\n";
            return 0;
        }
        if (arg == "--help") {
            std::cout << "Usage: bookr <file>\n";
            std::cout << "       bookr\n";
            std::cout << "       bookr --add-folder <path>\n";
            std::cout << "       bookr --remove-folder <path>\n";
            std::cout << "       bookr --list-folders\n";
            std::cout << "       bookr --help\n";
            std::cout << "       bookr --version\n\n";
            std::cout << "Formats: txt, fb2, epub, pdf\n";
            std::cout << "Controls:\n";
            std::cout << "  -> / wheel down : next page\n";
            std::cout << "  <- / wheel up   : previous page\n";
            std::cout << "  /               : search\n";
            std::cout << "  up/down or [/]  : next / previous match\n";
            std::cout << "  t               : table of contents (if detected)\n";
            std::cout << "  f               : footnotes on current page\n";
            std::cout << "  c               : copy current paragraph to clipboard\n";
            std::cout << "  q               : quit\n\n";
            std::cout << "Dependencies:\n";
            std::cout << "  fb2/epub: pandoc\n";
            std::cout << "  pdf: pdftotext (poppler)\n";
            std::cout << "\nVersioning:\n";
            std::cout << "  --version prints build version.\n";
            std::cout << "\nLibrary mode:\n";
            std::cout << "  Add book folders with --add-folder.\n";
            std::cout << "  Remove folders with --remove-folder.\n";
            std::cout << "  Run `bookr` without args to choose a book from your library.\n";
            return 0;
        }
        if (arg == "--list-folders") {
            const std::vector<std::string> folders = load_library_folders();
            if (folders.empty()) {
                std::cout << "No folders added.\n";
                return 0;
            }
            for (std::size_t i = 0; i < folders.size(); ++i) {
                std::cout << (i + 1) << ". " << folders[i] << "\n";
            }
            return 0;
        }
    }

    if (argc == 3 && std::string(argv[1]) == "--add-folder") {
        std::string error;
        if (!add_library_folder(argv[2], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "Folder added.\n";
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--remove-folder") {
        std::string error;
        if (!remove_library_folder(argv[2], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "Folder removed.\n";
        return 0;
    }

    if (argc == 1) {
        const std::optional<std::string> chosen = run_library_browser();
        if (!chosen.has_value()) {
            return 0;
        }
        return run_reader(*chosen);
    }

    if (argc == 2 && std::string(argv[1]).rfind("--", 0) == 0) {
        std::cerr << "Unknown option: " << argv[1] << "\n";
        std::cerr << "Try: bookr --help\n";
        return 1;
    }
    if (argc >= 2 && std::string(argv[1]) == "--add-folder") {
        std::cerr << "Usage: bookr --add-folder <path>\n";
        return 1;
    }
    if (argc >= 2 && std::string(argv[1]) == "--remove-folder") {
        std::cerr << "Usage: bookr --remove-folder <path>\n";
        return 1;
    }
    if (argc >= 2) {
        return run_reader(argv[1]);
    }

    std::cerr << "Try: bookr --help\n";
    return 1;
}
