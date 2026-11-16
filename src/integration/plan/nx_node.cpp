#include "nx_node.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <unistd.h>

/* CLI の唯一の入口 (gui/neodax-ui.mm)。GUI のボタンもこれを呼ぶ。
   ここを経由することで、ノードと GUI の挙動がずれない。
   neodax の中核 (plan/) から neodax-ui のシンボルを参照するため、
   gui がリンクされていない単体 CLI では弱参照として扱う。 */
extern "C" int nx_cli_dispatch_public(int argc, char **argv) __attribute__((weak));

namespace nx {
namespace node {

namespace {

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
