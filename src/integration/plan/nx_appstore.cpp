// nx_appstore.cpp — 直前に起動した app の分析を app ごとに保存する

#include "nx_appstore.hpp"
#include "nx_recipe.hpp"
#include "nx_template.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include <algorithm>

#include <dirent.h>
#include <libproc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

std::string home() {
    const char *h = getenv("HOME");
    return h ? h : "";
}
std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}
bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
long long fsize(const std::string &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return -1;
    return (long long)st.st_size;
}
void mkdirs(const std::string &p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur.push_back(p[i]);
        if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0700);
    }
    mkdir(p.c_str(), 0700);
}
std::string safe(const std::string &s) {
    std::string o;
    for (char c : s)
        o.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_');
    return o;
}
std::string hash8(const std::string &s) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[16];
    snprintf(b, sizeof b, "%08llx", h & 0xffffffffull);
    return b;
}

std::vector<pid_t> list_pids() {
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) return {};
    int n = bytes / (int)sizeof(pid_t) + 32;
    std::vector<pid_t> v((size_t)n);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, v.data(), n * (int)sizeof(pid_t));
    if (bytes <= 0) return {};
    v.resize((size_t)bytes / sizeof(pid_t));
    return v;
}

std::string path_of(pid_t p) {
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    int n = proc_pidpath(p, buf, sizeof buf);
    return n > 0 ? std::string(buf, (size_t)n) : "";
}

/* 直前に起動した GUI app を 1 つ選ぶ。 */
pid_t latest_app(std::string &apppath, std::string &exepath) {
    std::vector<pid_t> pids = list_pids();
    pid_t best = 0;
    unsigned long long bestt = 0;
    for (pid_t p : pids) {
        if (p <= 0 || p == getpid()) continue;
        std::string exe = path_of(p);
        if (exe.empty()) continue;
        /* 入れ子の .app (helper) ではなく、最初の .app コンポーネントを本体とする。
         * "X.app.neodax-work" のような作業コピーも 1 つの本体として扱う。 */
        std::string root;
        size_t pos = 0;
        while (pos < exe.size()) {
            size_t slash = exe.find('/', pos);
            std::string comp = exe.substr(pos, (slash == std::string::npos ? exe.size() : slash) - pos);
            size_t ap = comp.find(".app");
            if (ap != std::string::npos &&
                (ap + 4 == comp.size() || comp[ap + 4] == '.')) {
                root = exe.substr(0, pos + comp.size());
                break;
            }
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        if (root.empty()) continue;
        if (exe.find("/Contents/MacOS/", root.size()) == std::string::npos) continue;
        if (exe.rfind("/System/", 0) == 0) continue;
        struct proc_bsdinfo bi;
        if (proc_pidinfo(p, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi)
            continue;
        unsigned long long t = (unsigned long long)bi.pbi_start_tvsec * 1000000ull +
                               (unsigned long long)bi.pbi_start_tvusec;
        if (t > bestt) { bestt = t; best = p; apppath = root; exepath = exe; }
    }
    return best;
}

/* 最近起動した app を新しい順に集める (GUI の選択リスト用)。 */
struct AppEntry { unsigned long long t; std::string app, exe; };

static void collect_apps(std::vector<AppEntry> &out) {
    std::vector<pid_t> pids = list_pids();
    for (pid_t p : pids) {
        if (p <= 0 || p == getpid()) continue;
        std::string exe = path_of(p);
        if (exe.empty()) continue;
        std::string root;
        size_t pos = 0;
        while (pos < exe.size()) {
            size_t slash = exe.find('/', pos);
            std::string comp = exe.substr(pos, (slash == std::string::npos ? exe.size() : slash) - pos);
            size_t ap = comp.find(".app");
            if (ap != std::string::npos &&
                (ap + 4 == comp.size() || comp[ap + 4] == '.')) {
                root = exe.substr(0, pos + comp.size());
                break;
            }
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        if (root.empty()) continue;
        if (exe.find("/Contents/MacOS/", root.size()) == std::string::npos) continue;
        if (exe.rfind("/System/", 0) == 0) continue;
        struct proc_bsdinfo bi;
        if (proc_pidinfo(p, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi) continue;
        unsigned long long t = (unsigned long long)bi.pbi_start_tvsec * 1000000ull +
                               (unsigned long long)bi.pbi_start_tvusec;
        out.push_back({t, root, exe});
    }
    std::sort(out.begin(), out.end(),
              [](const AppEntry &a, const AppEntry &b) { return a.t > b.t; });
    /* 同じ app は最新の 1 件だけ */
    std::vector<AppEntry> uniq;
    for (const auto &e : out) {
        bool seen = false;
        for (const auto &u : uniq) if (u.app == e.app) { seen = true; break; }
        if (!seen) uniq.push_back(e);
    }
    out.swap(uniq);
}

static std::vector<AppEntry> g_recent;
static bool g_recent_loaded = false;

static void load_recent() {
    if (g_recent_loaded) return;
    collect_apps(g_recent);
    g_recent_loaded = true;
}

/* 指定 app のメイン実行ファイル (Contents/MacOS の最初の通常ファイル)。 */
static std::string main_exe(const std::string &app) {
    std::string d = join(app, "Contents/MacOS");
    DIR *dp = opendir(d.c_str());
    if (!dp) return "";
    std::string found;
    while (struct dirent *e = readdir(dp)) {
        if (e->d_name[0] == '.') continue;
        std::string p = join(d, e->d_name);
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { found = p; break; }
    }
    closedir(dp);
    return found;
}

std::string app_dir(const std::string &app) {
    std::string name = app;
    size_t s = name.find_last_of('/');
    if (s != std::string::npos) name = name.substr(s + 1);
    if (name.size() > 4 && name.substr(name.size() - 4) == ".app")
        name = name.substr(0, name.size() - 4);
    return join(home() + "/.neodax/apps", safe(name) + "_" + hash8(app));
}

int count_frameworks(const std::string &app) {
    std::string d = join(app, "Contents/Frameworks");
    DIR *dp = opendir(d.c_str());
    if (!dp) return 0;
    int n = 0;
    while (struct dirent *e = readdir(dp)) {
        if (e->d_name[0] == '.') continue;
        n++;
    }
    closedir(dp);
    return n;
}

/* 分析は「内容が変わったか」を安定して比較できる事実だけにする。 */
std::string analyze(const std::string &app, const std::string &exe) {
    std::string s;
    s += "app=" + app + "\n";

    std::string lib = join(app, "Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib");
    bool have_lib = exists(lib);
    if (!have_lib) lib = exe;
    s += "lib_size=" + std::to_string(fsize(lib)) + "\n";
    s += "frameworks=" + std::to_string(count_frameworks(app)) + "\n";

    struct { const char *name; const char *label; } syms[] = {
        {"ShouldCheckLicense", "license_should_check"},
        {"AdobeGCInvokeClient", "gc_invoke_client"},
        {"licensingglue12_GLOBAL__N_112InvokeGoCart", "gc_spawn_root"},
        {"OnLogFileCallback", "ngl_log_callback"},
    };
    for (const auto &sy : syms) {
        nx_site_t site{};
        bool ok = nx_recipe_locate_symbol(lib.c_str(), sy.name, &site) == 0;
        char b[64];
        if (ok) snprintf(b, sizeof b, "0x%llx", site.vaddr);
        else snprintf(b, sizeof b, "absent");
        s += std::string(sy.label) + "=" + b + "\n";
    }
    return s;
}

bool read_all(const std::string &p, std::string &out) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
bool write_all(const std::string &p, const std::string &s) {
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

std::string now_iso() {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tmv);
    return b;
}

void append_history(const std::string &dir, const std::string &changed, const std::string &fixed) {
    std::string p = join(dir, "history.tsv");
    FILE *f = fopen(p.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s\t%s\t%s\n", now_iso().c_str(), changed.c_str(), fixed.c_str());
    fclose(f);
}

} // namespace

extern "C" int nx_app_show(FILE *out) {
    if (!out) out = stdout;
    std::string app, exe;
    pid_t pid = latest_app(app, exe);
    if (!pid) { fprintf(out, "直前に起動した app が見つかりません\n"); return 1; }
    std::string dir = app_dir(app);
    fprintf(out, "=== latest app ===\n");
    fprintf(out, "pid  = %d\n", (int)pid);
    fprintf(out, "app  = %s\n", app.c_str());
    fprintf(out, "exe  = %s\n", exe.c_str());
    fprintf(out, "dir  = %s\n", dir.c_str());
    std::string prev;
    if (read_all(join(dir, "after.txt"), prev))
        fprintf(out, "baseline = あり (%zu bytes)\n", prev.size());
    else
        fprintf(out, "baseline = なし (次回 --analyze で作成)\n");
    return 0;
}

static int analyze_impl(const std::string &app, const std::string &exe, int fix, FILE *out) {
    std::string dir = app_dir(app);
    mkdirs(dir);
    write_all(join(dir, "app.path"), app + "\n");
    std::string cur = analyze(app, exe);
    write_all(join(dir, "analysis.txt"), cur);

    fprintf(out, "=== app 分析 ===\n");
    fprintf(out, "app = %s\n", app.c_str());
    fprintf(out, "dir = %s\n", dir.c_str());
    fprintf(out, "%s", cur.c_str());

    std::string prev;
    bool have_prev = read_all(join(dir, "after.txt"), prev);
    if (!have_prev) {
        write_all(join(dir, "after.txt"), cur);
        append_history(dir, "baseline", "no");
        fprintf(out, "初回: baseline を作成しました (修正なし)\n");
        return 0;
    }
    if (prev == cur) {
        append_history(dir, "same", "no");
        fprintf(out, "前回と同じ: 修正しません\n");
        return 0;
    }

    fprintf(out, "前回と違う: 修正します\n");
    std::string fixed = "no";
    if (fix) {
        /* 既知の入口だけを切る。既に work コピーへ入っていれば再 clone しない。 */
        if (cur.find("license_should_check=0x") != std::string::npos) {
            const std::string lib_rel =
                "/Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib";
            std::string wlib = app + ".neodax-work" + lib_rel;
            nx_site_t ws{};
            unsigned long w0 = 0;
            bool already =
                nx_recipe_locate_symbol(wlib.c_str(), "ShouldCheckLicense", &ws) == 0 &&
                nx_recipe_read_word(wlib.c_str(), ws.off, &w0) == 0 &&
                w0 == 0x52800000u;
            if (already) {
                fixed = "already";
            } else if (nx_cut(app.c_str(), "ShouldCheckLicense", 1, out) == 0) {
                fixed = "ShouldCheckLicense";
            }
        }
    }
    write_all(join(dir, "after.txt"), cur);
    append_history(dir, "changed", fixed);
    fprintf(out, "修正: %s\n", fixed == "no" ? "なし" : fixed.c_str());
    return 0;
}

extern "C" int nx_app_analyze(int fix, FILE *out) {
    if (!out) out = stdout;
    std::string app, exe;
    pid_t pid = latest_app(app, exe);
    if (!pid) { fprintf(out, "直前に起動した app が見つかりません\n"); return 1; }
    return analyze_impl(app, exe, fix, out);
}

extern "C" int nx_app_analyze_path(const char *app, int fix, FILE *out) {
    if (!out) out = stdout;
    if (!app || !*app) { fprintf(out, "app: --path required\n"); return 2; }
    if (!exists(app)) { fprintf(out, "app not found: %s\n", app); return 2; }
    std::string exe = main_exe(app);
    return analyze_impl(app, exe, fix, out);
}

extern "C" int nx_app_show_path(const char *app, FILE *out) {
    if (!out) out = stdout;
    if (!app || !*app) app = "";
    std::string dir = app_dir(app);
    fprintf(out, "=== app ===\napp = %s\ndir = %s\n", app, dir.c_str());
    std::string prev;
    if (read_all(join(dir, "after.txt"), prev))
        fprintf(out, "baseline = あり (%zu bytes)\n", prev.size());
    else
        fprintf(out, "baseline = なし\n");
    return 0;
}

extern "C" int nx_app_recent_count(void) {
    load_recent();
    return (int)g_recent.size();
}

extern "C" const char *nx_app_recent_path(int i) {
    load_recent();
    if (i < 0 || i >= (int)g_recent.size()) return "";
    return g_recent[i].app.c_str();
}

extern "C" const char *nx_app_recent_exe(int i) {
    load_recent();
    if (i < 0 || i >= (int)g_recent.size()) return "";
    return g_recent[i].exe.c_str();
}

extern "C" int nx_app_list(FILE *out) {
    if (!out) out = stdout;
    std::string root = home() + "/.neodax/apps";
    DIR *d = opendir(root.c_str());
    if (!d) { fprintf(out, "  (保存された app はありません)\n"); return 0; }
    fprintf(out, "=== apps (%s) ===\n", root.c_str());
    int n = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string dir = join(root, e->d_name);
        std::string app, after;
        read_all(join(dir, "app.path"), app);
        while (!app.empty() && (app.back() == '\n' || app.back() == '\r')) app.pop_back();
        bool has_after = read_all(join(dir, "after.txt"), after);
        fprintf(out, "  %-28s %s  %s\n", e->d_name,
                has_after ? "baseline" : "no-baseline", app.c_str());
        n++;
    }
    closedir(d);
    if (!n) fprintf(out, "  (none)\n");
    return 0;
}

extern "C" void nx_app_recent_refresh(void) {
    g_recent.clear();
    g_recent_loaded = false;
}
