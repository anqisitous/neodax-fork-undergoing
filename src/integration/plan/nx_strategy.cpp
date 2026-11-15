// nx_strategy.cpp — 「観測 → テンプレ」で介入戦略を一般探索する
//
// アプリ固有の名前をここに入れない。対象シンボルから事実を集め、
// テンプレ表をリスク昇順に当てはめる。実行は nx_recipe に委譲する。
//
// 一般則 (この順で安全):
//   data < edge(one) < edge(all) < entry_ret

#include "nx_strategy.hpp"
#include "nx_recipe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

static const char *kLibRel =
    "Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib";

static std::string lib_of(const char *app) {
    std::string s = app ? app : "";
    if (s.empty()) return s;
    if (s.back() == '/') s.pop_back();
    return s + "/" + kLibRel;
}

/* 入口の命令語から「条件分岐で始まるか」を判定する (AArch64)。
 * CBZ/CBNZ/TBZ/TBNZ は上位ビットで見分けられる。 */
static int first_is_cond_branch(unsigned long w) {
    /* CBZ  : 1011 0100 / CBNZ: 1011 0101
       TBZ  : 0011 0110 / TBNZ: 0011 0111 */
    unsigned long top8 = (w >> 24) & 0xFF;
    return (top8 == 0xB4 || top8 == 0xB5 || top8 == 0x36 || top8 == 0x37);
}

} // namespace

extern "C" int nx_strategy_observe(const char *app, const char *symbol,
                                   nx_facts_t *f, FILE *out) {
    if (!out) out = stdout;
    if (!app || !symbol || !f) return 1;
    memset(f, 0, sizeof(*f));
    snprintf(f->symbol, sizeof f->symbol, "%s", symbol);

    std::string lib = lib_of(app);
    nx_site_t site;
    if (nx_recipe_locate_symbol(lib.c_str(), symbol, &site) != 0) {
        fprintf(out, "  [observe] symbol を解決できません: %s\n", symbol);
        return 1;
    }
    f->vaddr = site.vaddr;
    f->off = site.off;

    nx_site_t callers[64];
    int n = nx_recipe_find_callers(lib.c_str(), site.vaddr, callers, 64);
    f->n_callers = n;
    for (int i = 0; i < n && i < 64; i++) {
        f->caller_off[i] = callers[i].off;
        f->caller_va[i] = callers[i].vaddr;
    }
    unsigned long w = 0;
    if (nx_recipe_read_word(lib.c_str(), site.off, &w) == 0) {
        f->first_word = w;
        f->first_is_cond_branch = first_is_cond_branch(w);
    }

    fprintf(out, "=== observe: %s ===\n", symbol);
    fprintf(out, "  vaddr=0x%llx  off=0x%llx  first=0x%08lx%s\n",
            f->vaddr, f->off, f->first_word,
            f->first_is_cond_branch ? " (cond-branch entry)" : "");
    fprintf(out, "  callers=%d\n", f->n_callers);
    for (int i = 0; i < f->n_callers && i < 8; i++)
        fprintf(out, "    BL @ 0x%llx (off 0x%llx)\n",
                f->caller_va[i], f->caller_off[i]);
    return 0;
}

extern "C" int nx_strategy_plan(const nx_facts_t *f, nx_choice_t *choices, int max) {
    if (!f || !choices || max <= 0) return 0;
    int n = 0;

    auto add = [&](nx_tpl_t t, const char *name, double risk, int ok,
                   const char *why) {
        if (n >= max) return;
        choices[n].tpl = t;
        snprintf(choices[n].name, sizeof choices[n].name, "%s", name);
        choices[n].risk = risk;
        choices[n].applicable = ok;
        snprintf(choices[n].why, sizeof choices[n].why, "%s", why);
        n++;
    };

    /* data: フラグ/定数の位置が分かっている時だけ。ここでは未観測なので保留。 */
    add(NX_TPL_DATA, "data", 0.10, 0,
        "フラグ/定数の位置が未観測 (hint があれば最優先)");

    /* edge(one): 呼び出し元が 1 つだけなら、その 1 本を切るのが最も精密。 */
    if (f->n_callers == 1)
        add(NX_TPL_EDGE_ONE, "edge_one", 0.15, 1,
            "呼び出し元が 1 つ。その BL だけを NOP にすれば副作用は残る");
    else
        add(NX_TPL_EDGE_ONE, "edge_one", 0.15, 0,
            "呼び出し元が 1 つではない (精密な 1 本指定ができない)");

    /* edge(all): 少数の呼び出しをまとめて切る。 */
    if (f->n_callers >= 1 && f->n_callers <= 8)
        add(NX_TPL_EDGE_ALL, "edge_all", 0.30, 1,
            "呼び出し元が少数。全 BL を NOP にしても本体は無傷");
    else
        add(NX_TPL_EDGE_ALL, "edge_all", 0.30, 0,
            f->n_callers == 0 ? "呼び出し元が無い (エッジが切れない)"
                              : "呼び出し元が多すぎる (影響が広い)");

    /* entry_ret: 最後の手段。本体ごと消えるので副作用依存で落ちやすい。 */
    add(NX_TPL_ENTRY_RET, "entry_ret", 0.70, 1,
        "最終手段。本体の副作用・初期化まで消えるため落ちる危険");

    /* risk 昇順に並べる (同じなら追加順)。 */
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++)
            if (choices[k].risk < choices[i].risk) {
                nx_choice_t t = choices[i]; choices[i] = choices[k]; choices[k] = t;
            }
    return n;
}

/* 選択テンプレを recipe テキストに落とす (P4: clone/resign/verify/rollback,
 * P3: 範囲契約, P2: 再観測)。 */
namespace {

static int write_recipe_for(const char *app, const nx_facts_t *f,
                            const nx_choice_t *c, const char *path, FILE *out) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(out, "  [search] cannot write %s\n", path); return 1; }
    fprintf(fp, "NXRECIPE 1\n");
    fprintf(fp, "app=%s\n", app);
    fprintf(fp, "work=%s.neodax-work\n", app);
    fprintf(fp, "lib=%s\n", kLibRel);
    fprintf(fp, "complete=1\n");
    fprintf(fp, "step clone\n");

    if (c->tpl == NX_TPL_EDGE_ONE || c->tpl == NX_TPL_EDGE_ALL) {
        int lim = (c->tpl == NX_TPL_EDGE_ONE) ? 1 : f->n_callers;
        unsigned long long lo = 0, hi = 0;
        for (int i = 0; i < lim; i++) {
            unsigned long long off = f->caller_off[i];
            /* BL -> NOP (AArch64): 1f 20 03 d5 */
            fprintf(fp, "step patch mode=edge_nop sym=%s vaddr=0x%llx off=0x%llx "
                        "hi=0x%llx hex=1f2003d5\n",
                    f->symbol, f->caller_va[i], off, off + 4);
            fprintf(fp, "step observe-stub off=0x%llx hex=1f2003d5\n", off);
            if (i == 0 || off < lo) lo = off;
            if (off + 4 > hi) hi = off + 4;
        }
        fprintf(fp, "step verify-range lo=0x%llx hi=0x%llx\n", lo, hi);
    } else {
        /* entry_ret: mov w0,#0 ; ret */
        fprintf(fp, "step patch mode=entry_ret sym=%s vaddr=0x%llx off=0x%llx "
                    "hi=0x%llx hex=00008052c0035fd6\n",
                f->symbol, f->vaddr, f->off, f->off + 8);
        fprintf(fp, "step verify-range lo=0x%llx hi=0x%llx\n", f->off, f->off + 8);
        fprintf(fp, "step observe-stub off=0x%llx hex=00008052c0035fd6\n", f->off);
    }
    fprintf(fp, "step resign\n");
    fprintf(fp, "step verify-sign\n");
    fprintf(fp, "onfail rollback\n");
    fclose(fp);
    return 0;
}

} // namespace

/* ================================================================== */
/* 有界な失敗メモリ — 「分岐パス確率」で間違い指数を出す             */
/* ================================================================== */
/*
 * 思想は既存コードと合わせる:
 *   - 保存は learn.cpp と同じ ~/.neodax 配下、タブ区切り、対象は最小限。
 *   - 確率は nx_plan の Confidence と同じ Laplace 平滑化 (成功+1)/(n+2)。
 *   - 枝刈りは「前提を満たさない枝を落とす」 nx_plan::simulate と同じ発想で、
 *     落とすのは容量超過かつ間違いが起きた時だけ。毎回はやらない。
 *
 * 指数 (雑でよい):
 *   err      = (bad+1)/(ok+bad+2)        その分岐の誤り率
 *   path_p   = (ok+bad+1)/(S+2)          その分岐を辿る確率
 *   idx      = err * path_p              分岐を辿った確率つき誤り指数
 *   直近のミスを起こした分岐は idx を 2 倍して優先的に落とす。
 */

namespace {

struct MemEntry {
    std::string app, symbol, branch;
    long ok = 0, bad = 0;
    int pruned = 0;
    long order = 0;   /* 挿入順 (古いものを物理削除する時に使う) */
};

static void make_dirs(const std::string &p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur.push_back(p[i]);
        if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0700);
    }
    mkdir(p.c_str(), 0700);
}

static std::string mem_path() {
    const char *h = getenv("HOME");
    std::string base = h ? h : "";
    base += "/.neodax";
    make_dirs(base);
    return base + "/strategy-memory.tsv";
}

static std::vector<MemEntry> mem_load() {
    std::vector<MemEntry> v;
    std::ifstream in(mem_path());
    if (!in) return v;
    std::string line;
    long ord = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::string cur;
        for (char c : line) {
            if (c == '\t') { f.push_back(cur); cur.clear(); }
            else cur += c;
        }
        f.push_back(cur);
        if (f.size() < 5) continue;
        MemEntry e;
        e.app = f[0]; e.symbol = f[1]; e.branch = f[2];
        e.ok = atol(f[3].c_str()); e.bad = atol(f[4].c_str());
        e.pruned = (f.size() > 5) ? atoi(f[5].c_str()) : 0;
        e.order = ord++;
        v.push_back(e);
    }
    return v;
}

static void mem_save(const std::vector<MemEntry> &v) {
    std::ofstream o(mem_path(), std::ios::trunc);
    if (!o) return;
    o << "# neodax strategy memory: app\tsymbol\tbranch\tok\tbad\tpruned\n";
    for (const auto &e : v)
        o << e.app << "\t" << e.symbol << "\t" << e.branch << "\t"
          << e.ok << "\t" << e.bad << "\t" << e.pruned << "\n";
}

static MemEntry *mem_find(std::vector<MemEntry> &v, const char *app,
                          const char *symbol, const char *branch) {
    for (auto &e : v)
        if (e.app == (app ? app : "") && e.symbol == (symbol ? symbol : "") &&
            e.branch == (branch ? branch : ""))
            return &e;
    return nullptr;
}

/* 1 エントリの間違い指数 (雑)。S はその symbol の総試行。 */
static double mem_index(const MemEntry &e, long S, const std::string &ref) {
    double err = (double)(e.bad + 1) / (double)(e.ok + e.bad + 2);
    double path_p = (double)(e.ok + e.bad + 1) / (double)(S + 2);
    double idx = err * path_p;
    if (!ref.empty() && e.branch == ref) idx *= 2.0;   /* 直近のミスを優先 */
    return idx;
}

} // namespace

extern "C" int nx_strategy_record(const char *app, const char *symbol,
                                   const char *branch, int ok) {
    std::vector<MemEntry> v = mem_load();
    MemEntry *e = mem_find(v, app, symbol, branch);
    if (!e) {
        MemEntry n;
        n.app = app ? app : "";
        n.symbol = symbol ? symbol : "";
        n.branch = branch ? branch : "";
        n.order = v.empty() ? 0 : v.back().order + 1;
        v.push_back(n);
        e = &v.back();
    }
    if (ok) e->ok++; else e->bad++;
    /* 削除は粘着。一度落とした分岐は、明示的に戻さない限り復活させない。
     * (探索側も pruned を候補から外すので、勝手に再試行されない) */

    int pruned_now = 0;
    if (!ok) {
        long active = 0, total = (long)v.size();
        for (const auto &x : v)
            if (x.app == (app ? app : "") && x.symbol == (symbol ? symbol : "") &&
                !x.pruned)
                active++;
        /* 間違い指数の再計算は、容量超過かつ間違ったタイミングのみ。 */
        if (active > NX_ACTIVE_MAX || total > NX_TOTAL_MAX) {
            long S = 0;
            for (const auto &x : v)
                if (x.app == (app ? app : "") && x.symbol == (symbol ? symbol : ""))
                    S += x.ok + x.bad;
            std::string ref = branch ? branch : "";

            /* (1) 作業用が上限を超えている時だけ、直近のミスの分岐を先に落とす。 */
            if (active > NX_ACTIVE_MAX) {
                for (auto &x : v)
                    if (x.app == (app ? app : "") && x.symbol == (symbol ? symbol : "") &&
                        x.branch == ref && !x.pruned) {
                        x.pruned = 1;
                        active--;
                        pruned_now++;
                    }
            }

            /* (2) まだ作業用が超過。間違い指数の大きい順に落とす。
             *     指数 = 誤り率 × 分岐パス確率 (参考: nx_plan の Confidence と同じ Laplace)。 */
            while (active > NX_ACTIVE_MAX) {
                MemEntry *worst = nullptr;
                double worst_idx = -1.0;
                for (auto &x : v) {
                    if (x.app != (app ? app : "") || x.symbol != (symbol ? symbol : ""))
                        continue;
                    if (x.pruned) continue;
                    double idx = mem_index(x, S, ref);
                    if (idx > worst_idx) { worst_idx = idx; worst = &x; }
                }
                if (!worst) break;
                worst->pruned = 1;
                active--, pruned_now++;
            }

            /* (3) 全体が 200 を超えたら、指数の大きい順に物理削除する。
             *     (ここで total が実際に減る。pruned は指数が高いので先に消える) */
            while ((long)v.size() > NX_TOTAL_MAX) {
                long worst = -1;
                double worst_idx = -1.0;
                for (long i = 0; i < (long)v.size(); i++) {
                    double idx = mem_index(v[i], S, ref);
                    if (v[i].pruned) idx += 1.0;   /* pruned を先に */
                    if (idx > worst_idx) { worst_idx = idx; worst = i; }
                }
                if (worst < 0) break;
                v.erase(v.begin() + worst);
            }
        }
    }
    mem_save(v);
    return pruned_now > 0 ? 1 : 0;
}

extern "C" int nx_strategy_is_pruned(const char *app, const char *symbol,
                                      const char *branch, char *why, size_t why_cap) {
    std::vector<MemEntry> v = mem_load();
    MemEntry *e = mem_find(v, app, symbol, branch);
    if (!e) {
        if (why && why_cap) why[0] = 0;
        return 0;
    }
    /* 粘着的に削除された分岐 */
    if (e->pruned) {
        if (why && why_cap)
            snprintf(why, why_cap, "pruned (ok=%ld bad=%ld, 分岐=%s)",
                     e->ok, e->bad, e->branch.c_str());
        return 1;
    }
    /* 安いスキップ: 同じ分岐が失敗>成功なら、次は選ばない。
     * 指数の再計算 (容量超過+ミス時のみ) とは別の、局所的な判定。 */
    if (e->bad > e->ok && e->bad > 0) {
        if (why && why_cap)
            snprintf(why, why_cap, "recent failures (ok=%ld bad=%ld, 分岐=%s)",
                     e->ok, e->bad, e->branch.c_str());
        return 1;
    }
    if (why && why_cap) why[0] = 0;
    return 0;
}

extern "C" int nx_strategy_stats_show(FILE *out) {
    if (!out) out = stdout;
    std::vector<MemEntry> v = mem_load();
    if (v.empty()) { fprintf(out, "まだ失敗履歴はありません (%s)\n", mem_path().c_str()); return 0; }
    fprintf(out, "=== strategy memory (%s) ===\n", mem_path().c_str());
    fprintf(out, "active上限=%d total上限=%d\n", NX_ACTIVE_MAX, NX_TOTAL_MAX);
    for (const auto &e : v) {
        long S = 0;
        for (const auto &x : v)
            if (x.app == e.app && x.symbol == e.symbol) S += x.ok + x.bad;
        double idx = mem_index(e, S, "");
        fprintf(out, "  %-10s ok=%ld bad=%ld idx=%.3f %s  [%s / %s]\n",
                e.branch.c_str(), e.ok, e.bad, idx,
                e.pruned ? "PRUNED" : "active", e.symbol.c_str(), e.app.c_str());
    }
    return 0;
}

/* 選択したテンプレに対応する分岐キー (辺なら具体的な BL アドレスを含む)。 */
static std::string branch_key_of(const nx_facts_t *f, const nx_choice_t *c) {
    char b[128];
    if (c->tpl == NX_TPL_EDGE_ONE) {
        snprintf(b, sizeof b, "edge_one@0x%llx", f->caller_va[0]);
        return b;
    }
    if (c->tpl == NX_TPL_EDGE_ALL) {
        snprintf(b, sizeof b, "edge_all@n=%d", f->n_callers);
        return b;
    }
    return c->name;
}

/* Info.plist から実行ファイル名を取る (プローブ用)。 */
static std::string exe_of(const char *app) {
    std::string plist = std::string(app) + "/Contents/Info.plist";
    std::ifstream in(plist);
    if (!in) return std::string();
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    const std::string key = "CFBundleExecutable";
    size_t p = data.find(key);
    if (p == std::string::npos) return std::string();
    size_t s = data.find("<string>", p);
    size_t e = data.find("</string>", s == std::string::npos ? p : s);
    if (s == std::string::npos || e == std::string::npos) return std::string();
    s += 8;
    std::string name = data.substr(s, e - s);
    if (name.empty()) return std::string();
    return std::string(app) + "/Contents/MacOS/" + name;
}

/* probe_ms 以上生存したら 1。落ちたら 0。 */
/* プロセスグループごと確実に落とす。
 * AE のように SIGTERM を無視し、子プロセスを残すアプリがあるため、
 * 低レベルに SIGKILL で一括終了させる (雑でよい)。 */
static void kill_tree(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGKILL);   /* 同じプロセスグループ全部 */
    kill(pid, SIGKILL);
    for (int i = 0; i < 20; i++) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || r < 0) break;
        usleep(50 * 1000);
    }
    waitpid(pid, NULL, 0);
}

/* probe_ms 以上生存したら 1。落ちたら 0。
 * 待ち時間が決められない (ms<=0) なら起動しない (即 quit 扱い)。
 * 起動した場合は、生存/終了にかかわらず必ず最後にプロセスグループを落とす。 */
static int probe_alive(const std::string &exe, int ms) {
    if (exe.empty() || ms <= 0) return 0;
    if (ms > 60000) ms = 60000;   /* 待ちすぎない上限 */
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* 自分のプロセスグループを作る。これで kill(-pgid) が全体に届く。 */
        setpgid(0, 0);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execl(exe.c_str(), exe.c_str(), (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);   /* 親側でも設定 (競合回避) */

    int alive = 1, waited = 0;
    while (waited < ms) {
        usleep(100 * 1000);
        waited += 100;
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) { alive = 0; break; }   /* 先に終了 = 失敗 */
        if (r < 0) break;
    }
    /* 生存でも終了済でも、残った子プロセスを巻き込んで必ず落とす。 */
    kill_tree(pid);
    return alive;
}

extern "C" int nx_strategy_search(const char *app, const char *symbol, int apply,
                                  int probe_ms, FILE *out) {
    if (!out) out = stdout;
    nx_facts_t f;
    if (nx_strategy_observe(app, symbol, &f, out) != 0) return 2;

    nx_choice_t ch[8];
    int n = nx_strategy_plan(&f, ch, 8);
    fprintf(out, "=== templates (risk ascending) ===\n");
    for (int i = 0; i < n; i++) {
        std::string bk = branch_key_of(&f, &ch[i]);
        char why[256];
        if (ch[i].applicable && nx_strategy_is_pruned(app, symbol, bk.c_str(), why, sizeof why)) {
            ch[i].applicable = 0;
            snprintf(ch[i].why, sizeof ch[i].why, "%s", why);
        }
        fprintf(out, "  [%d] %-10s risk=%.2f %s  — %s\n", i + 1, ch[i].name,
                ch[i].risk, ch[i].applicable ? "APPLIES" : "skip", ch[i].why);
    }

    int pick = -1;
    for (int i = 0; i < n; i++) if (ch[i].applicable) { pick = i; break; }
    if (pick < 0) { fprintf(out, "  [search] 適用可能なテンプレがありません\n"); return 2; }

    std::string bk = branch_key_of(&f, &ch[pick]);
    fprintf(out, "=== chosen: %s (risk=%.2f, branch=%s) ===\n",
            ch[pick].name, ch[pick].risk, bk.c_str());
    if (!apply) {
        fprintf(out, "  [search] dry-run。--apply で作業コピーに適用します。\n");
        return 0;
    }

    char tmp[] = "/tmp/nx-strategy-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return 2;
    close(fd);
    std::string rp = std::string(tmp) + ".recipe";
    if (write_recipe_for(app, &f, &ch[pick], rp.c_str(), out) != 0) return 2;

    fprintf(out, "=== apply via recipe ===\n");
    int rc = nx_recipe_run(rp.c_str(), 1, 0, out);
    unlink(tmp);
    unlink(rp.c_str());

    int ok = (rc == 0);
    if (ok && probe_ms > 0) {
        std::string work = std::string(app) + ".neodax-work";
        std::string wexe = exe_of(work.c_str());
        fprintf(out, "=== probe: %s を %dms 観測 ===\n", wexe.c_str(), probe_ms);
        int alive = probe_alive(wexe, probe_ms);
        fprintf(out, "   %s\n", alive ? "生存 = 成功" : "終了/異常 = 失敗");
        ok = ok && alive;
    }
    int pruned = nx_strategy_record(app, symbol, bk.c_str(), ok);
    if (pruned)
        fprintf(out, "  [memory] 容量超過 + ミス → 分岐 %s を枝刈りしました\n", bk.c_str());

    /* 念のため、作業コピーの残存プロセスを低レベルに一括削除する。
     * (起動待ちが長引いたり、アプリが終了を無視しても残さない) */
    system("pkill -KILL -f '.neodax-work' 2>/dev/null");
    return ok ? 0 : 1;
}
