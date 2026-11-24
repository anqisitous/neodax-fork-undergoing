#include "nx_editor_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace nxedit {
namespace {

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool is_source_ext(const std::string &ext) {
    static const std::unordered_set<std::string> ok = {
        ".java", ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
        ".m", ".mm", ".swift", ".rs", ".py", ".js", ".jsx", ".ts", ".tsx",
        ".go", ".cs", ".kt", ".kts", ".scala", ".rb", ".php"
    };
    return ok.count(ext) != 0;
}

std::string shell_quote(const std::string &s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
}

std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

// 文字列・コメントを読み飛ばして、対応する括弧の位置を返す。
// src 内のバイト位置で扱う。UTF-8 の非 ASCII は括弧として扱わない。
std::unordered_map<size_t, size_t> match_braces(const std::string &src) {
    std::unordered_map<size_t, size_t> match;
    std::vector<size_t> stack;
    bool line_comment = false, block_comment = false;
    char quote = 0;
    bool esc = false;
    for (size_t i = 0; i < src.size(); i++) {
        char c = src[i];
        if (line_comment) {
            if (c == '\n') line_comment = false;
            continue;
        }
        if (block_comment) {
            if (c == '*' && i + 1 < src.size() && src[i + 1] == '/') {
                block_comment = false;
                i++;
            }
            continue;
        }
        if (quote) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && i + 1 < src.size()) {
            if (src[i + 1] == '/') { line_comment = true; i++; continue; }
            if (src[i + 1] == '*') { block_comment = true; i++; continue; }
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{') stack.push_back(i);
        else if (c == '}') {
            if (!stack.empty()) {
                size_t open = stack.back();
                stack.pop_back();
                match[open] = i;
            }
        }
    }
    return match;
}

bool is_word_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

std::string word_before(const std::string &src, size_t pos) {
    size_t b = pos;
    while (b > 0 && std::isspace((unsigned char)src[b - 1])) b--;
    size_t e = b;
    while (b > 0 && is_word_char(src[b - 1])) b--;
    return src.substr(b, e - b);
}

bool is_control_word(const std::string &w) {
    static const std::unordered_set<std::string> bad = {
        "if", "for", "while", "switch", "catch", "synchronized", "return",
        "throw", "new", "else", "do", "try", "assert", "yield", "await",
        "sizeof", "decltype", "alignof", "static_assert"
    };
    return bad.count(w) != 0;
}

} // namespace

std::vector<std::string> split_tab(const std::string &s) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t p = s.find('\t', a);
        if (p == std::string::npos) {
            out.push_back(s.substr(a));
            break;
        }
        out.push_back(s.substr(a, p - a));
        a = p + 1;
    }
    return out;
}

bool parse_where(const std::string &s, std::string &file, int &line, int &column) {
    size_t c2 = s.rfind(':');
    if (c2 == std::string::npos || c2 == 0) return false;
    size_t c1 = s.rfind(':', c2 - 1);
    if (c1 == std::string::npos || c1 == 0) return false;
    file = s.substr(0, c1);
    try {
        line = std::stoi(s.substr(c1 + 1, c2 - c1 - 1));
        column = std::stoi(s.substr(c2 + 1));
    } catch (...) {
        return false;
    }
    return line > 0 && column > 0;
}

bool Project::open(const std::string &root, const std::string &map_path, std::string &err) {
    root_ = fs::absolute(root).lexically_normal().string();
    files_.clear();
    nodes_.clear();
    symbols_.clear();

    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        err = "not a directory: " + root_;
        return false;
    }
    scan_files();
    if (files_.empty()) {
        err = "no source files under: " + root_;
        return false;
    }

    std::string text, map_err;
    if (!map_path.empty()) {
        std::ifstream in(map_path);
        if (!in) {
            err = "cannot read map: " + map_path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    } else {
        text = run_review_map(map_err);
    }

    if (!text.empty()) load_map_text(text, err);
    if (nodes_.empty()) {
        // reviewer が無い / JDK が無い場合でも、マーカーだけは拾う。
        for (const SourceFile &f : files_) {
            for (size_t i = 0; i < f.lines.size(); i++) {
                const std::string &ln = f.lines[i];
                std::string key;
                if (ln.find("TODO") != std::string::npos) key = "todo";
                else if (ln.find("ここを実装") != std::string::npos) key = "stub";
                else if (ln.find("task1") != std::string::npos) key = "task1";
                else if (ln.find("task2") != std::string::npos) key = "task2";
                else if (ln.find("task3") != std::string::npos) key = "task3";
                if (key.empty()) continue;
                Node n;
                n.key = key;
                n.file = f.path;
                n.line = (int)i + 1;
                n.column = 1;
                n.name = f.path + ":" + std::to_string(n.line);
                n.body_start = n.line;
                n.body_end = n.line;
                n.confidence = "low";
                nodes_.push_back(n);
            }
        }
    }

    scan_symbols();
    return true;
}

void Project::scan_files() {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::path &p = it->path();
        std::string name = p.filename().string();
        if (name.empty()) continue;
        if (it->is_directory(ec)) {
            if (name == ".git" || name == "node_modules" || name == "build" ||
                name == "bin" || name == "target" || name == ".build") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (!is_source_ext(lower(p.extension().string()))) continue;
        std::error_code sec;
        auto sz = fs::file_size(p, sec);
        if (sec || sz > 2 * 1024 * 1024) continue;

        std::ifstream in(p);
        if (!in) continue;
        std::vector<std::string> lines;
        std::string ln;
        while (std::getline(in, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(ln);
        }
        SourceFile f;
        f.id = (int)files_.size() + 1;
        f.path = fs::relative(p, root_, ec).lexically_normal().string();
        f.lines = std::move(lines);
        files_.push_back(std::move(f));
    }
    std::sort(files_.begin(), files_.end(), [](const SourceFile &a, const SourceFile &b) {
        return a.path < b.path;
    });
    for (size_t i = 0; i < files_.size(); i++) files_[i].id = (int)i + 1;
}

bool Project::load_map_text(const std::string &text, std::string &err) {
    bool in_map = false;
    for (const std::string &raw : split_lines(text)) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## fix map", 0) == 0) { in_map = true; continue; }
        if (!in_map) continue;
        if (line.empty()) {
            if (!nodes_.empty()) break;
            continue;
        }
        if (line.rfind("key\t", 0) == 0) continue;
        if (line.rfind("## ", 0) == 0) break;
        std::vector<std::string> c = split_tab(line);
        if (c.size() < 3) continue;
        Node n;
        n.key = c[0];
        n.name = c[1];
        std::string body;
        if (!parse_where(c[2], n.file, n.line, n.column)) continue;
        if (c.size() > 3) body = c[3];
        size_t dash = body.find('-');
        if (dash != std::string::npos) {
            try {
                n.body_start = std::stoi(body.substr(0, dash));
                n.body_end = std::stoi(body.substr(dash + 1));
            } catch (...) {}
        }
        if (c.size() > 4) n.contract = c[4];
        if (c.size() > 5) n.expect = c[5];
        if (c.size() > 6) n.confidence = c[6];
        if (n.file.empty()) continue;
        nodes_.push_back(std::move(n));
    }
    if (nodes_.empty()) err = "fix map has no rows";
    return !nodes_.empty();
}

std::string Project::run_review_map(std::string &err) const {
    std::vector<std::string> candidates = {
        "tools/java-review/neodax-java-review",
        "../tools/java-review/neodax-java-review",
        "../../tools/java-review/neodax-java-review"
    };
    std::string tool;
    for (const auto &c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) { tool = c; break; }
    }
    if (tool.empty()) {
        err = "neodax-java-review not found (use --map)";
        return {};
    }
    std::string cmd = shell_quote(tool) + " " + shell_quote(root_) + " --map 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) {
        err = "cannot run reviewer";
        return {};
    }
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    if (rc != 0 || out.empty()) {
        err = "reviewer returned no map";
        return {};
    }
    return out;
}

const SourceFile *Project::file_by_id(int id) const {
    if (id < 1 || id > (int)files_.size()) return nullptr;
    return &files_[(size_t)id - 1];
}

const SourceFile *Project::file_by_path(const std::string &path) const {
    std::string want = fs::path(path).lexically_normal().string();
    for (const auto &f : files_)
        if (f.path == want) return &f;
    // map が絶対パス・別表記でも、末尾一致で拾う。
    for (const auto &f : files_) {
        if (want.size() >= f.path.size() &&
            want.compare(want.size() - f.path.size(), f.path.size(), f.path) == 0)
            return &f;
    }
    return nullptr;
}

int Project::file_id(const std::string &path) const {
    const SourceFile *f = file_by_path(path);
    return f ? f->id : 0;
}

std::string Project::range_text(const std::string &file, int start_line, int end_line) const {
    const SourceFile *f = file_by_path(file);
    if (!f || start_line < 1 || end_line < start_line) return {};
    int a = std::max(1, start_line);
    int b = std::min((int)f->lines.size(), end_line);
    std::string out;
    for (int i = a; i <= b; i++) {
        out += f->lines[(size_t)i - 1];
        out += '\n';
    }
    return out;
}

std::string Project::file_text(int id) const {
    const SourceFile *f = file_by_id(id);
    if (!f) return {};
    std::string out;
    for (const auto &ln : f->lines) { out += ln; out += '\n'; }
    return out;
}

bool Project::replace_lines(const std::string &file, int start_line, int end_line,
                            const std::string &text, std::string &err) {
    SourceFile *f = nullptr;
    for (auto &x : files_) {
        if (x.path == file) { f = &x; break; }
    }
    if (!f) { err = "file not found: " + file; return false; }
    if (start_line < 1 || end_line < start_line || start_line > (int)f->lines.size()) {
        err = "bad range: " + file + ":" + std::to_string(start_line) + "-" +
              std::to_string(end_line);
        return false;
    }
    std::vector<std::string> repl = split_lines(text);
    int a = start_line - 1;
    int b = std::min((int)f->lines.size(), end_line);
    f->lines.erase(f->lines.begin() + a, f->lines.begin() + b);
    f->lines.insert(f->lines.begin() + a, repl.begin(), repl.end());
    return true;
}

bool Project::write_file(const std::string &file, std::string &err) {
    for (const auto &f : files_) {
        if (f.path != file) continue;
        fs::path p = fs::path(root_) / f.path;
        std::ofstream out(p, std::ios::binary);
        if (!out) { err = "cannot write: " + p.string(); return false; }
        for (const auto &ln : f.lines) out << ln << '\n';
        if (!out) { err = "write failed: " + p.string(); return false; }
        return true;
    }
    err = "file not found: " + file;
    return false;
}

const Symbol *Project::symbol_at(const std::string &file, int line, int column) const {
    for (const auto &s : symbols_) {
        if (s.file != file || s.name_line != line) continue;
        // 名前の端は含めない。内部だけを「セマンティックな編集中」とみなす。
        if (column > s.name_column && column < s.name_column + s.name_length)
            return &s;
    }
    return nullptr;
}

void Project::node_range(const Node &n, int &start_line, int &end_line) const {
    start_line = n.body_start > 0 ? n.body_start : n.line;
    end_line = n.body_end >= start_line ? n.body_end : start_line;
    if (n.body_start > 0 && n.body_end >= n.body_start) return;
    if (const Symbol *s = symbol_at(n.file, n.line, n.column)) {
        start_line = s->start_line;
        end_line = s->end_line;
    }
}

std::vector<Node> Project::filtered(const std::string &query) const {
    std::string q = lower(trim(query));
    if (q.empty()) return nodes_;
    std::vector<Node> out;
    for (const Node &n : nodes_) {
        std::string hay = lower(n.key + "\t" + n.name + "\t" + n.file + "\t" +
                               std::to_string(n.line) + "\t" + n.contract);
        if (hay.find(q) != std::string::npos) out.push_back(n);
    }
    return out;
}

void Project::scan_symbols() {
    const std::regex class_re(R"(\b(class|interface|enum|record|struct)\s+([A-Za-z_$][A-Za-z0-9_$]*))");
    const std::regex call_re(R"(([A-Za-z_$][A-Za-z0-9_$]*)\s*\()");
    std::unordered_set<std::string> seen;

    for (const SourceFile &f : files_) {
        std::string src;
        std::vector<size_t> line_start;
        for (const auto &ln : f.lines) {
            line_start.push_back(src.size());
            src += ln;
            src += '\n';
        }
        if (src.empty()) continue;
        auto braces = match_braces(src);

        auto line_of = [&](size_t pos) -> int {
            auto it = std::upper_bound(line_start.begin(), line_start.end(), pos);
            return (int)(it - line_start.begin());
        };
        auto col_of = [&](size_t pos) -> int {
            int ln = line_of(pos);
            if (ln < 1 || ln > (int)line_start.size()) return 1;
            return (int)(pos - line_start[(size_t)ln - 1]) + 1;
        };
        auto add = [&](Symbol::Kind kind, const std::string &name,
                       size_t name_pos, size_t open_brace) {
            auto it = braces.find(open_brace);
            if (it == braces.end()) return;
            Symbol s;
            s.kind = kind;
            s.name = name;
            s.file = f.path;
            s.start_line = line_of(name_pos);
            s.end_line = line_of(it->second);
            s.name_line = line_of(name_pos);
            s.name_column = col_of(name_pos);
            s.name_length = (int)name.size();
            std::string key = s.file + ":" + std::to_string(s.start_line) + ":" +
                              std::to_string(s.name_column);
            if (!seen.insert(key).second) return;
            symbols_.push_back(std::move(s));
        };

        for (std::sregex_iterator it(src.begin(), src.end(), class_re), end; it != end; ++it) {
            std::smatch m = *it;
            size_t after = (size_t)m.position(0) + (size_t)m.length(0);
            size_t brace = src.find('{', after);
            size_t semi = src.find(';', after);
            if (brace == std::string::npos) continue;
            if (semi != std::string::npos && semi < brace) continue;
            size_t name_pos = (size_t)m.position(2);
            add(Symbol::ClassLike, m.str(2), name_pos, brace);
        }

        for (std::sregex_iterator it(src.begin(), src.end(), call_re), end; it != end; ++it) {
            std::smatch m = *it;
            std::string name = m.str(1);
            if (is_control_word(name)) continue;
            size_t name_pos = (size_t)m.position(1);
            // 呼び出し・メンバアクセス・new を除外する。
            std::string prev = word_before(src, name_pos);
            if (prev == "new" || prev == "return" || prev == "throw" || prev == "delete") continue;
            size_t p = name_pos;
            while (p > 0 && std::isspace((unsigned char)src[p - 1])) p--;
            if (p > 0 && (src[p - 1] == '.' || src[p - 1] == '>' || src[p - 1] == '='))
                continue;
            size_t open = src.find('(', name_pos + name.size());
            if (open == std::string::npos) continue;
            // 対応する ')' を探す。
            int depth = 0;
            size_t close = std::string::npos;
            bool q = false, esc = false;
            for (size_t i = open; i < src.size(); i++) {
                char c = src[i];
                if (q) {
                    if (esc) esc = false;
                    else if (c == '\\') esc = true;
                    else if (c == '"') q = false;
                    continue;
                }
                if (c == '"') { q = true; continue; }
                if (c == '(') depth++;
                else if (c == ')') {
                    depth--;
                    if (depth == 0) { close = i; break; }
                }
            }
            if (close == std::string::npos) continue;
            size_t j = close + 1;
            while (j < src.size() && std::isspace((unsigned char)src[j])) j++;
            if (src.compare(j, 6, "throws") == 0) {
                size_t b = src.find('{', j);
                size_t s = src.find(';', j);
                if (b == std::string::npos) continue;
                if (s != std::string::npos && s < b) continue;
                j = b;
            }
            if (j >= src.size() || src[j] != '{') continue;
            // 宣言行の先頭に制御構文や代入があるものは除外。
            size_t line_pos = line_start[(size_t)line_of(name_pos) - 1];
            std::string head = src.substr(line_pos, name_pos - line_pos);
            if (head.find('=') != std::string::npos) continue;
            add(Symbol::Function, name, name_pos, j);
        }
    }

    std::sort(symbols_.begin(), symbols_.end(), [](const Symbol &a, const Symbol &b) {
        if (a.file != b.file) return a.file < b.file;
        if (a.start_line != b.start_line) return a.start_line < b.start_line;
        return a.name_column < b.name_column;
    });
}

} // namespace nxedit
