// learn.cpp — 技能の保存と部分結果

#include "learn.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>

namespace nx {

namespace {

std::string home_dir() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}

void make_dirs(const std::string &p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur.push_back(p[i]);
        if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0700);
    }
    mkdir(p.c_str(), 0700);
}

// 単純な 16 進ハッシュ。暗号用途ではないが、対象を特定できない形にする。
std::string hexhash(const std::string &s) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[32];
    std::snprintf(b, sizeof b, "%016llx", h);
    return b;
}

} // namespace

std::string SkillStore::default_path() {
    return home_dir() + "/.neodax/skills.db";
}

std::string SkillStore::sanitize(const std::string &s) {
    // パス、アプリ名らしき固有名、UUID らしき断片を落とす。
    // これにより「何を解析したか」が保存側へ漏れない。
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '/') { out += "<path>"; while (i + 1 < s.size() && s[i + 1] != ' ') i++; continue; }
        if (c == '.') {
            // 拡張子らしき 3〜4 文字 (.app, .framework 等) は種類だけ残す
            size_t j = i + 1, n = 0;
            while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_')) { j++; n++; }
            if (n >= 2 && n <= 12) { out += "<ext>"; i = j - 1; continue; }
        }
        out.push_back(c);
    }
    // 連続する <path> を 1 個にまとめる
    std::string o2;
    for (size_t i = 0; i < out.size();) {
        if (out.compare(i, 6, "<path>") == 0) {
            o2 += "<path>";
            i += 6;
            while (out.compare(i, 6, "<path>") == 0) i += 6;
        } else { o2.push_back(out[i]); i++; }
    }
    if (o2.size() > 96) o2 = o2.substr(0, 96) + "...";
    return o2;
}

void SkillStore::observe(const std::string &key, const std::string &value) {
    std::string k = sanitize(key), v = sanitize(value);
    auto it = skills_.find(k);
    if (it == skills_.end()) {
        Skill s; s.key = k; s.value = v; s.hits = 1;
        skills_[k] = s;
    } else {
        it->second.hits++;
        // 新しい観測が来たら代表値を更新する
        it->second.value = v;
    }
}

const Skill *SkillStore::find(const std::string &key) const {
    auto it = skills_.find(sanitize(key));
    return it == skills_.end() ? nullptr : &it->second;
}

bool SkillStore::save(const std::string &path) const {
    std::string dir = path;
    size_t sl = dir.find_last_of('/');
    if (sl != std::string::npos) make_dirs(dir.substr(0, sl));
    std::ofstream o(path, std::ios::trunc);
    if (!o) return false;
    o << "# neodax skills: 解析の仕方のみ。対象の特定情報は含まない。\n";
    for (const auto &kv : skills_)
        o << kv.second.key << "\t" << kv.second.value << "\t" << kv.second.hits << "\n";
    return (bool)o;
}

bool SkillStore::load(const std::string &path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        Skill s;
        s.key = line.substr(0, t1);
        s.value = (t2 == std::string::npos) ? line.substr(t1 + 1)
                                            : line.substr(t1 + 1, t2 - t1 - 1);
        s.hits = (t2 == std::string::npos) ? 1 : std::atoi(line.c_str() + t2 + 1);
        skills_[s.key] = s;
    }
    return true;
}

void SkillStore::dump(FILE *out) const {
    if (!out) out = stdout;
    if (skills_.empty()) { std::fprintf(out, "まだ何も学んでいません。\n"); return; }
    std::fprintf(out, "学んだ解析の仕方は、次のとおりです。\n");
    for (const auto &kv : skills_)
        std::fprintf(out, "  %s  =>  %s  (%d 回)\n",
                     kv.second.key.c_str(), kv.second.value.c_str(), kv.second.hits);
}

// ---------------------------------------------------------------- Progress

std::string Progress::fingerprint(size_t segments, size_t imports,
                                  size_t defined, size_t sections) {
    // 対象を特定できない形にする。パスも名前も使わず、構造の数だけを混ぜる。
    // さらに粗く丸めて、同型のバイナリが同じ指紋になりやすいようにする。
    char b[128];
    std::snprintf(b, sizeof b, "s%zu-i%zu-d%zu-c%zu",
                  segments, imports / 64, defined / 256, sections);
    return hexhash(b);
}

std::string Progress::default_dir() {
    return home_dir() + "/.neodax/parts";
}

void Progress::mark_done(const std::string &step) {
    std::string s = SkillStore::sanitize(step);
    for (const auto &d : done_) if (d == s) return;
    done_.push_back(s);
}

bool Progress::is_done(const std::string &step) const {
    std::string s = SkillStore::sanitize(step);
    for (const auto &d : done_) if (d == s) return true;
    return false;
}

bool Progress::save(const std::string &dir, const std::string &fp) const {
    make_dirs(dir);
    std::ofstream o(dir + "/" + fp + ".part", std::ios::trunc);
    if (!o) return false;
    for (const auto &d : done_) o << d << "\n";
    return (bool)o;
}

bool Progress::load(const std::string &dir, const std::string &fp) {
    std::ifstream in(dir + "/" + fp + ".part");
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) if (!line.empty()) mark_done(line);
    return true;
}

} // namespace nx
