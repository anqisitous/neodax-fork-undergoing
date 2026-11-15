// nx_template.cpp — 解析結果をテンプレートとして保存し、段階的に除去する

#include "nx_template.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nx_recipe.hpp"

namespace {

std::string home() {
    const char *h = getenv("HOME");
    return h ? h : "";
}

std::string path() { return home() + "/.neodax/templates.tsv"; }
std::string cut_dir() { return home() + "/.neodax/cut"; }
std::string recipe_dir() { return home() + "/.neodax/recipes"; }

void ensure_dir(const std::string &d) { mkdir(d.c_str(), 0700); }

std::string safe_name(const std::string &s) {
    std::string o;
    for (char c : s)
        o.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' ? c : '_');
    return o;
}

struct Row {
    std::string kind, target, symbol, vaddr, action, status;
};

std::vector<Row> load() {
    std::vector<Row> rows;
    FILE *f = fopen(path().c_str(), "r");
    if (!f) return rows;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        Row r;
        std::vector<std::string> cols;
        size_t p = 0;
        while (p <= s.size()) {
            size_t q = s.find('\t', p);
            if (q == std::string::npos) { cols.push_back(s.substr(p)); break; }
            cols.push_back(s.substr(p, q - p));
            p = q + 1;
        }
        if (cols.size() < 6) continue;
        r.kind = cols[0]; r.target = cols[1]; r.symbol = cols[2];
        r.vaddr = cols[3]; r.action = cols[4]; r.status = cols[5];
        rows.push_back(r);
    }
    fclose(f);
    return rows;
}

void store(const std::vector<Row> &rows) {
    FILE *f = fopen(path().c_str(), "w");
    if (!f) return;
    fprintf(f, "# kind\ttarget\tsymbol\tvaddr\taction\tstatus\n");
    for (const auto &x : rows)
        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n", x.kind.c_str(), x.target.c_str(),
                x.symbol.c_str(), x.vaddr.c_str(), x.action.c_str(), x.status.c_str());
    fclose(f);
}

bool same(const Row &a, const char *kind, const char *target, const char *symbol) {
    return a.kind == kind && a.target == target && a.symbol == symbol;
}

struct CutState {
    std::string app, symbol, recipe;
    unsigned long long vaddr = 0, off = 0;
    int stage = 0;   // 0=解析済み, 1..7=recipe の完了段
};

std::string state_path(const std::string &app, const std::string &symbol) {
    return cut_dir() + "/" + safe_name(app) + "__" + safe_name(symbol) + ".state";
}

bool load_state(const std::string &app, const std::string &symbol, CutState &s) {
    FILE *f = fopen(state_path(app, symbol).c_str(), "r");
    if (!f) return false;
    char line[2048];
    s.app = app; s.symbol = symbol;
    while (fgets(line, sizeof line, f)) {
        std::string t(line);
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = t.substr(0, eq), v = t.substr(eq + 1);
        if (k == "recipe") s.recipe = v;
        else if (k == "vaddr") s.vaddr = strtoull(v.c_str(), NULL, 0);
        else if (k == "off") s.off = strtoull(v.c_str(), NULL, 0);
        else if (k == "stage") s.stage = atoi(v.c_str());
    }
    fclose(f);
    return !s.recipe.empty();
}

void save_state(const CutState &s) {
    ensure_dir(cut_dir());
    FILE *f = fopen(state_path(s.app, s.symbol).c_str(), "w");
    if (!f) return;
    fprintf(f, "app=%s\nsymbol=%s\nvaddr=0x%llx\noff=0x%llx\nstage=%d\nrecipe=%s\n",
            s.app.c_str(), s.symbol.c_str(), s.vaddr, s.off, s.stage, s.recipe.c_str());
    fclose(f);
}

} // namespace

extern "C" const char *nx_template_path(void) {
    static std::string p = path();
    return p.c_str();
}

extern "C" int nx_template_record(const char *kind, const char *target,
                                  const char *symbol, unsigned long long vaddr,
                                  const char *action, int applied, FILE *out) {
    if (!out) out = stdout;
    if (!kind || !target || !symbol) return 2;
    ensure_dir(home() + "/.neodax");
    std::vector<Row> rows = load();
    char v[32];
    snprintf(v, sizeof v, "0x%llx", vaddr);
    const char *status = applied ? "applied" : "planned";
    for (auto &r : rows) {
        if (!same(r, kind, target, symbol)) continue;
        r.vaddr = v; r.action = action ? action : ""; r.status = status;
        store(rows);
        fprintf(out, "  [template] updated %s/%s @%s (%s)\n", kind, symbol, v, status);
        return 0;
    }
    rows.push_back({kind, target, symbol, v, action ? action : "", status});
    store(rows);
    fprintf(out, "  [template] saved %s/%s @%s (%s)\n", kind, symbol, v, status);
    return 0;
}

extern "C" int nx_template_list(FILE *out) {
    if (!out) out = stdout;
    std::vector<Row> rows = load();
    fprintf(out, "=== templates (%s) ===\n", path().c_str());
    if (rows.empty()) { fprintf(out, "  (none)\n"); return 0; }
    for (const auto &r : rows)
        fprintf(out, "  %-6s %-8s %-26s %-10s %s\n",
                r.kind.c_str(), r.status.c_str(), r.symbol.c_str(), r.vaddr.c_str(),
                r.action.c_str());
    return 0;
}

/* --- 段階実行: 解析 → テンプレ保存 → 1 段ずつ clone/patch/検証 --- */

static int cut_start(const char *app, const char *symbol, CutState &s, FILE *out) {
    const std::string lib_rel =
        "/Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib";
    std::string lib = std::string(app) + lib_rel;
    nx_site_t site{};
    if (nx_recipe_locate_symbol(lib.c_str(), symbol, &site) != 0) {
        fprintf(out, "  [cut] symbol not found: %s\n", symbol);
        return 1;
    }
    s.app = app; s.symbol = symbol; s.vaddr = site.vaddr; s.off = site.off; s.stage = 0;

    char act[512];
    snprintf(act, sizeof act, "%s を mov w0,#0; ret に潰す", symbol);
    nx_template_record("cut", app, symbol, site.vaddr, act, 0, out);

    ensure_dir(recipe_dir());
    s.recipe = recipe_dir() + "/" + safe_name(symbol) + ".nxrecipe";
    if (nx_recipe_build(app, symbol, s.recipe.c_str(), out) != 0) {
        fprintf(out, "  [cut] recipe incomplete (not applied)\n");
        return 1;
    }
    save_state(s);
    fprintf(out, "  [cut] stage=0 (analyzed, template saved) recipe=%s\n", s.recipe.c_str());
    fprintf(out, "  [cut] next: cut --step --path \"%s\" --symbol %s\n", app, symbol);
    return 0;
}

static int cut_step(CutState &s, FILE *out) {
    if (s.stage >= 7) {
        fprintf(out, "  [cut] already complete (stage=7)\n");
        return 0;
    }
    int target = s.stage + 1;
    fprintf(out, "  [cut] step %d -> %d\n", s.stage, target);
    if (nx_recipe_run(s.recipe.c_str(), 1, target, out) != 0) {
        fprintf(out, "  [cut] step %d failed (rollback)\n", target);
        return 1;
    }
    s.stage = target;
    save_state(s);
    nx_template_record("cut", s.app.c_str(), s.symbol.c_str(), s.vaddr,
                       "mov w0,#0; ret", s.stage >= 7 ? 1 : 0, out);
    fprintf(out, "  [cut] stage=%d/7\n", s.stage);
    if (s.stage < 7)
        fprintf(out, "  [cut] next: cut --step --path \"%s\" --symbol %s\n",
                s.app.c_str(), s.symbol.c_str());
    return 0;
}

extern "C" int nx_cut(const char *app, const char *symbol, int apply, FILE *out) {
    if (!out) out = stdout;
    if (!app || !symbol) { fprintf(out, "cut: --path and --symbol required\n"); return 2; }
    CutState s;
    if (!load_state(app, symbol, s)) {
        if (cut_start(app, symbol, s, out) != 0) return 1;
    } else {
        fprintf(out, "  [cut] resume stage=%d (%s)\n", s.stage, s.recipe.c_str());
    }
    if (!apply) {
        fprintf(out, "  [cut] planned. --step で 1 段ずつ、--apply で最後まで。\n");
        return 0;
    }
    while (s.stage < 7) {
        if (cut_step(s, out) != 0) return 1;
    }
    return 0;
}

extern "C" int nx_cut_step_cmd(const char *app, const char *symbol, FILE *out) {
    if (!out) out = stdout;
    if (!app || !symbol) { fprintf(out, "cut --step: --path --symbol required\n"); return 2; }
    CutState s;
    if (!load_state(app, symbol, s)) {
        fprintf(out, "  [cut] no checkpoint. run cut --path --symbol first\n");
        return 1;
    }
    return cut_step(s, out);
}

extern "C" int nx_cut_abort(const char *app, const char *symbol, FILE *out) {
    if (!out) out = stdout;
    if (!app || !symbol) { fprintf(out, "cut --abort: --path --symbol required\n"); return 2; }
    if (unlink(state_path(app, symbol).c_str()) == 0)
        fprintf(out, "  [cut] checkpoint removed\n");
    else
        fprintf(out, "  [cut] no checkpoint\n");
    return 0;
}

extern "C" int nx_cut_status(FILE *out) {
    if (!out) out = stdout;
    DIR *d = opendir(cut_dir().c_str());
    if (!d) { fprintf(out, "  (no checkpoints)\n"); return 0; }
    fprintf(out, "=== cut checkpoints (%s) ===\n", cut_dir().c_str());
    int n = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = cut_dir() + "/" + e->d_name;
        FILE *f = fopen(p.c_str(), "r");
        if (!f) continue;
        char line[2048];
        std::string sym, app; int stage = 0;
        while (fgets(line, sizeof line, f)) {
            std::string t(line);
            if (t.rfind("symbol=", 0) == 0) sym = t.substr(7);
            else if (t.rfind("app=", 0) == 0) app = t.substr(4);
            else if (t.rfind("stage=", 0) == 0) stage = atoi(t.c_str() + 6);
        }
        fclose(f);
        while (!sym.empty() && (sym.back() == '\n' || sym.back() == '\r')) sym.pop_back();
        while (!app.empty() && (app.back() == '\n' || app.back() == '\r')) app.pop_back();
        fprintf(out, "  stage=%d/7  %-28s %s\n", stage, sym.c_str(), app.c_str());
        n++;
    }
    closedir(d);
    if (!n) fprintf(out, "  (none)\n");
    return 0;
}
