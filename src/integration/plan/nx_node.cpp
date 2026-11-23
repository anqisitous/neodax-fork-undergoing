#include "nx_node.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* CLI の唯一の入口 (gui/neodax-ui.mm)。GUI のボタンもこれを呼ぶ。
   ここを経由することで、ノードと GUI の挙動がずれない。
   neodax の中核 (plan/) から neodax-ui のシンボルを参照するため、
   gui がリンクされていない単体 CLI では弱参照として扱う。 */
extern "C" int nx_cli_dispatch_public(int argc, char **argv) __attribute__((weak));

namespace nx {
namespace node {

namespace {

// ------------------------------------------------------------------
// 結果キャッシュ
//
// ノードは同じ観測 (tabs / keys / advise) を何度も呼ぶ。呼ぶたびに
// mkstemp + dup2 + dispatch + 全出力の読み戻しをすると、木のノード数だけ
// 同じコストを払う。読み取り専用コマンドに限り、同じ引数 + 同じ状態なら
// 前の結果を返す。状態を書き換えるコマンドを呼んだら全部捨てる。
// ------------------------------------------------------------------

std::unordered_map<std::string, NodeResult> &result_cache() {
    static std::unordered_map<std::string, NodeResult> cache;
    return cache;
}

bool cache_enabled() {
    const char *v = std::getenv("NX_NODE_CACHE");
    return !(v && v[0] == '0' && v[1] == '\0');
}

bool has_flag(const std::vector<std::string> &args, const char *flag) {
    for (const auto &a : args)
        if (a == flag) return true;
    return false;
}

bool mutating_args(const std::vector<std::string> &args) {
    if (args.empty()) return true;

    // 状態・キャッシュ・成果物を書くコマンド/フラグ。
    // 名前を知らないコマンドは「書くかもしれない」側に倒す。
    static const char *const writes[] = {
        "--apply", "--live", "--write", "--force", "--bake", "--emit",
        "--run", "--record", "--fix", "--delete", "--patch", "--kill",
        "--no-store-sync", "--save", nullptr
    };
    for (int i = 0; writes[i]; i++)
        if (has_flag(args, writes[i])) return true;

    // --state を読み、状態を書き換えないコマンドだけ。
    // probe / display のように副作用 (attach・画面) を持つものは入れない。
    static const char *const readonly[] = {
        "tabs", "dump", "keys", "tab-keys", "cut-status",
        "advise", "check", "plan", "arena", nullptr
    };
    for (int i = 0; readonly[i]; i++)
        if (args[0] == readonly[i]) return false;
    return true;
}

bool state_arg(const std::vector<std::string> &args, std::string &out) {
    for (std::size_t i = 1; i < args.size(); i++) {
        if (args[i] == "--state" && i + 1 < args.size()) { out = args[i + 1]; return true; }
        if (args[i].compare(0, 8, "--state=") == 0) { out = args[i].substr(8); return true; }
    }
    return false;
}

std::string one_stat(const struct stat &st) {
    long nsec = 0;
#if defined(__APPLE__)
    nsec = st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    nsec = st.st_mtim.tv_nsec;
#endif
    char buf[256];
    std::snprintf(buf, sizeof buf, "%llu:%lld:%lld:%ld:%lld",
                  (unsigned long long)st.st_ino,
                  (long long)st.st_size,
                  (long long)st.st_mtime,
                  nsec,
                  (long long)st.st_mode);
    return buf;
}

std::string state_fingerprint(const std::string &path) {
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0) return std::string();

    std::string fp = one_stat(st);
    if (!S_ISDIR(st.st_mode)) return fp;

    // LevelDB は既存ファイルへ追記する。ディレクトリの mtime だけでは
    // 中身の変化を取り逃がすので、直下の各ファイルの stat も混ぜる。
    DIR *d = opendir(path.c_str());
    if (!d) return fp;
    std::vector<std::string> entries;
    while (struct dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        std::string child = path + "/" + e->d_name;
        struct stat cs;
        if (stat(child.c_str(), &cs) != 0) continue;
        entries.push_back(std::string(e->d_name) + "\x1f" + one_stat(cs));
    }
    closedir(d);
    std::sort(entries.begin(), entries.end());
    fp += "\x1e";
    for (const auto &e : entries) { fp += e; fp += '\n'; }
    return fp;
}

// 同じ引数 + 同じ状態のときだけ同じ結果になる。状態が無い/読めない
// コマンドはキャッシュしない (外の変化を取り逃がさない)。
std::string cache_key(const std::vector<std::string> &args) {
    std::string state;
    if (!state_arg(args, state)) return std::string();
    std::string fp = state_fingerprint(state);
    if (fp.empty()) return std::string();

    std::string key;
    for (const auto &a : args) { key += a; key += '\x1f'; }
    key += fp;
    return key;
}

/* stdout を一時ファイルへ逃がして dispatch を呼び、出力を回収する。
   GUI の nx_gui_exec と同じやり方 (実装を 2 つ持たない)。 */
NodeResult capture(std::vector<std::string> args) {
    NodeResult r;
    r.rc = 1;
    if (args.empty()) { r.err = "コマンド名が無い"; return r; }

    if (!nx_cli_dispatch_public) {
        r.err = "この実行ファイルにはコマンド入口が無い (neodax-ui から使う)";
        return r;
    }

    auto &cache = result_cache();
    const bool mutating = mutating_args(args);
    if (mutating) cache.clear();   // 書き込み前の読みを残さない

    bool use_cache = !mutating && cache_enabled();
    std::string key;
    if (use_cache) {
        key = cache_key(args);
        if (key.empty()) use_cache = false;
    }
    if (use_cache) {
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    /* dispatch は argv[1] をコマンド名として見る。 */
    std::vector<char *> av;
    av.reserve(args.size() + 2);
    av.push_back(const_cast<char *>("neodax-ui"));
    for (auto &a : args) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);

    fflush(stdout);
    char tmp[] = "/tmp/neodax-node-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { r.err = "一時ファイルを作れない"; return r; }

    int saved = dup(fileno(stdout));
    if (saved < 0) { close(fd); unlink(tmp); r.err = "stdout を退避できない"; return r; }
    dup2(fd, fileno(stdout));
    close(fd);

    r.rc = nx_cli_dispatch_public((int)av.size() - 1, av.data());

    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    FILE *fp = fopen(tmp, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n > 0) {
            r.out.resize((size_t)n);
            size_t got = fread(&r.out[0], 1, (size_t)n, fp);
            r.out.resize(got);
        }
        fclose(fp);
    }
    unlink(tmp);

    // 書き込み後は、その前の読みを残さない。
    if (mutating) cache.clear();
    if (use_cache && r.rc == 0) {
        if (cache.size() >= 128) cache.clear();
        cache.emplace(std::move(key), r);
    }

    if (r.rc != 0) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "rc=%d", r.rc);
        r.err = buf;
        /* 出力に理由が書かれていることが多いので、末尾を err に足す。 */
        if (!r.out.empty()) {
            std::string tail = r.out;
            if (tail.size() > 200) tail = tail.substr(tail.size() - 200);
            while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r'))
                tail.pop_back();
            if (!tail.empty()) r.err += ": " + tail;
        }
    }
    return r;
}

} // namespace

NodeResult run(const std::vector<std::string> &argv) {
    return capture(argv);
}

NodeResult run(const char *line) {
    std::vector<std::string> args;
    if (line) {
        std::istringstream is(line);
        std::string w;
        while (is >> w) args.push_back(w);
    }
    return capture(args);
}

NodeResult run_with_state(const std::vector<std::string> &argv, const char *state_path) {
    std::vector<std::string> a = argv;
    if (state_path && *state_path) {
        bool has = false;
        for (auto &x : a) if (x == "--state") { has = true; break; }
        if (!has) { a.push_back("--state"); a.push_back(state_path); }
    }
    return capture(a);
}

std::vector<std::string> lines(const std::string &s) {
    std::vector<std::string> v;
    std::istringstream is(s);
    std::string ln;
    while (std::getline(is, ln)) v.push_back(ln);
    return v;
}

void cache_clear() { result_cache().clear(); }
std::size_t cache_size() { return result_cache().size(); }

std::string field(const std::string &out, const char *key, const char *dflt) {
    if (!key) return dflt ? dflt : "";
    const size_t klen = std::strlen(key);
    for (const auto &ln : lines(out)) {
        if (ln.size() < klen || ln.compare(0, klen, key) != 0) continue;
        size_t p = ln.find('\t');
        if (p == std::string::npos) return ln.substr(klen);
        size_t q = ln.find('\t', p + 1);
        if (q == std::string::npos) return ln.substr(p + 1);
        return ln.substr(p + 1, q - p - 1);
    }
    return dflt ? dflt : "";
}

} // namespace node
} // namespace nx
