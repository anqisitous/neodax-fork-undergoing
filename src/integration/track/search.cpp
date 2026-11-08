// search.cpp — neodax の検索になるネイティブ検索の実装

#include "search.hpp"

#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nx {

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

std::string lower_copy(const std::string &s) {
    std::string o = s;
    for (char &c : o) c = lower_ascii(c);
    return o;
}

std::string base_name(const std::string &p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string join_path(const std::string &a, const std::string &b) {
    if (a.empty() || a == ".") return a.empty() ? b : a + "/" + b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool is_word_char(unsigned char c) {
    return std::isalnum(c) || c == '_';
}

bool looks_binary(const std::string &head) {
    size_t n = head.size() < 8000 ? head.size() : 8000;
    for (size_t i = 0; i < n; i++)
        if (head[i] == '\0') return true;
    return false;
}

// 改行で分割する。\r は落とし、行番号は 1 始まり。
std::vector<std::string> split_lines(const std::string &data) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(256);
    for (char c : data) {
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

std::vector<std::string> split_globs(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool glob_match(const std::string &glob, const std::string &path) {
    std::string target = glob.find('/') == std::string::npos ? base_name(path) : path;
    if (fnmatch(glob.c_str(), target.c_str(), FNM_CASEFOLD) == 0) return true;
    // "plist" のような拡張子だけの指定も受ける。
    if (glob.find('*') == std::string::npos && glob.find('?') == std::string::npos) {
        size_t dot = target.find_last_of('.');
        if (dot != std::string::npos && lower_copy(target.substr(dot + 1)) == lower_copy(glob))
            return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------- TextQuery

TextQuery::TextQuery(const SeekOptions &o)
    : literal_(!o.regex_mode), ignore_case_(o.ignore_case), word_(o.word) {
    if (literal_) {
        needle_ = o.ignore_case ? lower_copy(o.pattern) : o.pattern;
        if (needle_.empty()) error_ = "検索語が空です";
        return;
    }
    std::string pat = o.pattern;
    if (word_) pat = "\\b(?:" + pat + ")\\b";
    try {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (o.ignore_case) flags |= std::regex::icase;
        re_ = std::regex(pat, flags);
    } catch (const std::regex_error &e) {
        error_ = std::string("正規表現が不正です: ") + e.what();
    }
}

bool TextQuery::match(const std::string &line, size_t *col) const {
    if (!error_.empty()) return false;
    if (!literal_) {
        std::smatch m;
        if (!std::regex_search(line, m, re_)) return false;
        if (col) *col = (size_t)m.position(0);
        return true;
    }
    std::string hay = ignore_case_ ? lower_copy(line) : line;
    size_t from = 0;
    while (from <= hay.size()) {
        size_t p = hay.find(needle_, from);
        if (p == std::string::npos) return false;
        if (!word_) { if (col) *col = p; return true; }
        bool left_ok = (p == 0) || !is_word_char((unsigned char)hay[p - 1]);
        size_t e = p + needle_.size();
        bool right_ok = (e >= hay.size()) || !is_word_char((unsigned char)hay[e]);
        if (left_ok && right_ok) { if (col) *col = p; return true; }
        from = p + 1;
    }
    return false;
}

bool TextQuery::prefilter(const std::string &data) const {
    if (!error_.empty()) return false;
    if (!literal_ || needle_.empty()) return true;
    if (ignore_case_) return lower_copy(data).find(needle_) != std::string::npos;
    return data.find(needle_) != std::string::npos;
}

// ---------------------------------------------------------------- FileFilter

FileFilter::FileFilter(const SeekOptions &o)
    : globs_(split_globs(o.includes)), binary_(o.binary),
      max_bytes_((long)o.max_file_mb * 1024L * 1024L) {}

bool FileFilter::accept_path(const std::string &path) const {
    if (globs_.empty()) return true;
    for (const auto &g : globs_)
        if (glob_match(g, path)) return true;
    return false;
}

bool FileFilter::accept_content(const std::string &head) const {
    return binary_ || !looks_binary(head);
}

// ---------------------------------------------------------------- Corpus

std::vector<std::string> Corpus::collect(const std::string &root) const {
    std::vector<std::string> out;
    struct stat st;
    if (stat(root.c_str(), &st) != 0) return out;
    if (!S_ISDIR(st.st_mode)) { out.push_back(root); return out; }

    struct Item { std::string path; int depth; };
    std::vector<Item> stack{{root, 0}};
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        DIR *d = opendir(it.path.c_str());
        if (!d) continue;
        while (struct dirent *e = readdir(d)) {
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
            std::string p = join_path(it.path, e->d_name);
            struct stat es;
            if (lstat(p.c_str(), &es) != 0) continue;
            if (S_ISLNK(es.st_mode)) continue;              // 環を作らない
            if (S_ISDIR(es.st_mode)) {
                if (it.depth < max_depth_ && std::strcmp(e->d_name, ".git") != 0)
                    stack.push_back({p, it.depth + 1});
            } else if (S_ISREG(es.st_mode)) {
                out.push_back(p);
            }
        }
        closedir(d);
    }
    return out;
}

// ---------------------------------------------------------------- Searcher

Searcher::Searcher(const SeekOptions &o, FILE *out)
    : opt_(o), query_(o), filter_(o), out_(out ? out : stdout) {}

bool Searcher::timed_out(long long deadline_ms) const {
    return opt_.time_ms > 0 && now_ms() > deadline_ms;
}

void Searcher::emit(const SeekHit &hit) const {
    if (opt_.files_only) return;
    // 設定ファイルは 1 行が巨大なことがある。行番号は残し、表示だけ切る。
    const size_t kMax = 4096;
    if (hit.text.size() <= kMax) {
        std::fprintf(out_, "%s:%ld:%s\n", hit.path.c_str(), hit.line, hit.text.c_str());
    } else {
        std::fprintf(out_, "%s:%ld:%.*s…(%zu bytes)\n", hit.path.c_str(), hit.line,
                     (int)kMax, hit.text.c_str(), hit.text.size());
    }
}

void Searcher::scan_file(const std::string &path, long long deadline_ms) {
    if (timed_out(deadline_ms)) { time_up_ = true; return; }
    if (!filter_.accept_path(path)) return;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (filter_.too_large((long)st.st_size)) return;

    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    // 先頭だけ読んでバイナリを判定する。巨大な DB を丸ごと読まない。
    char headbuf[8000];
    in.read(headbuf, sizeof headbuf);
    std::string head(headbuf, (size_t)in.gcount());
    files_++;
    if (!filter_.accept_content(head)) { skipped_binary_++; return; }
    in.clear();
    in.seekg(0);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!query_.prefilter(data)) return;

    std::vector<std::string> lines = split_lines(data);
    bool file_printed = false;
    long last_ctx = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        if ((i & 1023) == 0 && timed_out(deadline_ms)) { time_up_ = true; return; }
        size_t col = 0;
        if (!query_.match(lines[i], &col)) continue;
        hits_++;
        if (opt_.files_only) {
            if (!file_printed) { std::fprintf(out_, "%s\n", path.c_str()); file_printed = true; matched_++; }
            if (opt_.limit > 0 && hits_ >= opt_.limit) { truncated_ = true; return; }
            break;
        }
        long from = (long)i - opt_.context;
        if (from < 0) from = 0;
        long to = (long)i + opt_.context;
        if (to >= (long)lines.size()) to = (long)lines.size() - 1;
        if (from <= last_ctx) from = last_ctx + 1;
        for (long j = from; j <= to; j++) {
            SeekHit h;
            h.path = path;
            h.line = j + 1;
            h.text = lines[(size_t)j];
            if (j == (long)i) h.col = col;
            emit(h);
            last_ctx = j;
        }
        if (!file_printed) { matched_++; file_printed = true; }
        if (opt_.limit > 0 && hits_ >= opt_.limit) { truncated_ = true; return; }
    }
}

int Searcher::run() {
    if (!query_.ok()) {
        std::fprintf(out_, "%s\n", query_.error().c_str());
        return 2;
    }
    long long t0 = now_ms();
    long long deadline = opt_.time_ms > 0 ? t0 + opt_.time_ms : 0;

    // ファイル 1 つならそのまま、ディレクトリなら列挙して走査する。
    std::vector<std::string> paths = Corpus(opt_.max_depth).collect(opt_.root);
    for (const auto &p : paths) {
        scan_file(p, deadline);
        if (truncated_) break;
        if (time_up_) break;
    }

    long long elapsed = now_ms() - t0;
    std::fprintf(out_, "seek: pattern=\"%s\" root=%s files=%ld matched=%ld hits=%ld",
                 opt_.pattern.c_str(), opt_.root.c_str(), files_, matched_, hits_);
    if (skipped_binary_) std::fprintf(out_, " binary_skipped=%ld", skipped_binary_);
    std::fprintf(out_, " elapsed=%lldms", elapsed);
    if (truncated_) std::fprintf(out_, " [limit %d]", opt_.limit);
    if (time_up_) std::fprintf(out_, " [time %dms]", opt_.time_ms);
    std::fprintf(out_, "\n");
    return hits_ > 0 ? 0 : 1;
}

} // namespace nx

extern "C" int nx_seek(const char *pattern, const char *root, const char *includes,
                       int flags, int context, int limit, int time_ms, FILE *out) {
    nx::SeekOptions o;
    o.pattern = pattern ? pattern : "";
    o.root = (root && *root) ? root : ".";
    o.includes = includes ? includes : "";
    o.ignore_case = (flags & 1) != 0;
    o.regex_mode = (flags & 2) != 0;
    o.binary = (flags & 4) != 0;
    o.files_only = (flags & 8) != 0;
    o.word = (flags & 16) != 0;
    o.context = context > 0 ? context : 0;
    o.limit = limit >= 0 ? limit : 1000;
    o.time_ms = time_ms > 0 ? time_ms : 15000;
    nx::Searcher s(o, out);
    return s.run();
}
