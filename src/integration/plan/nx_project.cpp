#include "nx_project.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>

/* パスがディレクトリとして存在するか。 */
static int is_dir(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* パスがファイルとして存在するか。 */
static int is_file(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* 親までのフォルダを順に作る。既にあれば何もしない。 */
static int mkdirs(const char *path) {
    if (!path || !*path) return -1;
    std::string p(path);
    for (size_t i = 1; i < p.size(); i++) {
        if (p[i] != '/') continue;
        p[i] = '\0';
        if (!is_dir(p.c_str())) mkdir(p.c_str(), 0755);
        p[i] = '/';
    }
    if (!is_dir(p.c_str()) && mkdir(p.c_str(), 0755) != 0 && !is_dir(p.c_str()))
        return -1;
    return 0;
}

/* FNV-1a。path の指紋。 */
static unsigned long long fnv(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

int nx_project_default_path(const char *state_path, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    const char *home = std::getenv("HOME");
    if (!home) return -1;
    const char *src = state_path ? state_path : "default";
    /* 状態の場所からプロジェクト名を作る。同じ状態なら同じ場所。 */
    const char *base = std::strrchr(src, '/');
    base = base ? base + 1 : src;
    std::snprintf(out, cap, "%s/.neodax/projects/%s_%08llx",
                  home, base && *base ? base : "default",
                  fnv(src) & 0xffffffffULL);
    return 0;
}

int nx_project_can_write(const char *path) {
    if (!path || !*path) return -2;
    if (is_dir(path)) return 0;            /* 既にある */
    /* 一番近い既存の親まで遡って、そこに書けるかを見る。
       無い親は mkdirs で作るので「作れば書ける」。 */
    std::string p(path);
    while (true) {
        size_t slash = p.find_last_of('/');
        if (slash == std::string::npos || slash == 0) break;
        p = p.substr(0, slash);
        if (is_dir(p.c_str())) return 1;   /* 作れば書ける */
    }
    return -1;
}

/* ---- TSV を 1 行ずつ読む ---- */
struct LineSink {
    FILE *out;
    int n;
};

/* 分析の内容を key\tvalue に正規化した文字列にする。
   並びの違いで「違う」と判定しないよう、行をソートする。 */
static std::string normalize(const char *tsv) {
    std::vector<std::string> lines;
    if (!tsv) return "";
    const char *p = tsv;
    while (*p) {
        const char *e = std::strchr(p, '\n');
        std::string one = e ? std::string(p, e - p) : std::string(p);
        if (!one.empty()) lines.push_back(one);
        if (!e) break;
        p = e + 1;
    }
    std::sort(lines.begin(), lines.end());
    std::string s;
    for (auto &l : lines) { s += l; s += '\n'; }
    return s;
}

static int write_all(const char *path, const char *data) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return -1;
    size_t n = std::strlen(data);
    size_t w = std::fwrite(data, 1, n, f);
    std::fclose(f);
    return w == n ? 0 : -1;
}

int nx_project_save(const char *path, const char *source,
                    const char *analysis_tsv, FILE *out) {
    char def[4096];
    if (!path || !*path) {
        if (nx_project_default_path(source, def, sizeof def) != 0) return -1;
        path = def;
    }

    /* 1. 書けるか先に確かめる。まだ書かない。 */
    int cw = nx_project_can_write(path);
    if (cw < 0) return -2;

    /* 2. path を確定させる。 */
    if (mkdirs(path) != 0) return -3;

    std::string norm = normalize(analysis_tsv);

    /* 3. 同じ分析が既にあるか。あれば書かない。 */
    std::string apath = std::string(path) + "/analysis.tsv";
    FILE *rf = std::fopen(apath.c_str(), "rb");
    if (rf) {
        std::string old;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, rf)) > 0) old.append(buf, n);
        std::fclose(rf);
        std::string onorm = normalize(old.c_str());
        if (!onorm.empty() && onorm == norm) {
            if (out) std::fprintf(out, "  [project] 同じ分析が既にある — 書かない\n");
            return 1;
        }
    }

    /* 4. 書く。 */
    if (write_all(apath.c_str(), norm.c_str()) != 0) return -4;

    /* project.tsv: どの状態を見たか */
    char pj[8192];
    std::snprintf(pj, sizeof pj, "kind\tsource\nsource\t%s\n", source ? source : "");
    write_all((std::string(path) + "/project.tsv").c_str(), pj);

    /* history.tsv: 追記のみ */
    std::string hp = std::string(path) + "/history.tsv";
    FILE *hf = std::fopen(hp.c_str(), "ab");
    if (hf) {
        std::fprintf(hf, "save\t%ld\t%s\n", (long)time(nullptr),
                     source ? source : "");
        std::fclose(hf);
    }

    if (out) std::fprintf(out, "  [project] 保存: %s\n", path);
    return 0;
}

int nx_project_save_frame(const char *path, int frame,
                          unsigned long long seq_before,
                          unsigned long long seq_after,
                          int resolved, int opened, int cut,
                          int progressed, FILE *out) {
    char def[4096];
    if (!path || !*path) {
        if (nx_project_default_path(nullptr, def, sizeof def) != 0) return -1;
        path = def;
    }
    if (nx_project_can_write(path) < 0) return -2;
    if (mkdirs(path) != 0) return -3;

    std::string fp = std::string(path) + "/frames.tsv";
    int need_header = !is_file(fp.c_str());
    FILE *f = std::fopen(fp.c_str(), "ab");
    if (!f) return -4;
    if (need_header)
        std::fprintf(f, "frame\tseq_before\tseq_after\tresolved\topened\tcut\tprogressed\n");
    std::fprintf(f, "%d\t%llu\t%llu\t%d\t%d\t%d\t%d\n",
                 frame, seq_before, seq_after, resolved, opened, cut, progressed);
    std::fclose(f);
    (void)out;
    return 0;
}

int nx_project_show(const char *path, FILE *out) {
    if (!out) out = stdout;
    char def[4096];
    if (!path || !*path) {
        if (nx_project_default_path(nullptr, def, sizeof def) != 0) return -1;
        path = def;
    }
    std::fprintf(out, "project: %s\n", path);
    if (!is_dir(path)) {
        std::fprintf(out, "  (無い)\n");
        return 1;
    }

    const char *files[] = { "project.tsv", "analysis.tsv", "frames.tsv" };
    for (int i = 0; i < 3; i++) {
        std::string fp = std::string(path) + "/" + files[i];
        FILE *f = std::fopen(fp.c_str(), "rb");
        if (!f) continue;
        std::fprintf(out, "--- %s ---\n", files[i]);
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            std::fwrite(buf, 1, n, out);
        std::fclose(f);
    }
    return 0;
}

int nx_project_list(FILE *out) {
    if (!out) out = stdout;
    const char *home = std::getenv("HOME");
    if (!home) return -1;
    std::string dir = std::string(home) + "/.neodax/projects";
    DIR *d = opendir(dir.c_str());
    if (!d) {
        std::fprintf(out, "  (まだ無い: %s)\n", dir.c_str());
        return 1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        if (!is_dir(p.c_str())) continue;
        std::string a = p + "/analysis.tsv";
        struct stat st;
        long sz = stat(a.c_str(), &st) == 0 ? (long)st.st_size : -1;
        std::fprintf(out, "%s\tanalysis=%ld\n", e->d_name, sz);
        n++;
    }
    closedir(d);
    if (n == 0) std::fprintf(out, "  (まだ無い: %s)\n", dir.c_str());
    return 0;
}
