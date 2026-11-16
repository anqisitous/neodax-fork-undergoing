// nx_clean.cpp — 「消えたはずのものが残っている」を、状態から選んだ手順で消す。
//
// なぜ単にタブを消さないか:
//   残滓は 1 種類ではない。所有者を失った面 / 重複 / 退避コピー側にすり替わった
//   解決先 / 壊れた追記。どれを消すかより、どの順でどのコマンドを呼ぶかで
//   結果が変わる (例: WAL の末尾が壊れていると、正しい値を書いても読めない)。
//
// この実装の構造 (選択を状態から導く):
//   observe  : nx_residual / nx_state で事実を集める
//   classify : 見落としの種類 (nx_miss_kind_t) と安全度 (nx_residual_risk_t) を出す
//   choose   : 種類と安全度から、呼ぶコマンド列を選ぶ
//   execute  : 選んだコマンドを、既存の実装と同じ関数で実行する
//   verify   : もう一度 observe し、残滓が 0 かで判定する
//
// コマンドの実装は増やさない。clean は既存の部品 (persist/delete/advise) を
// 選んで組み合わせるだけにする。これで「clean と既存コマンドの二重実装」を
// 作らない。

#include "nx_clean.hpp"
#include "nx_residual.hpp"
#include "nx_project.h"
#include "nx_store_facts.hpp"
/* plan の読み取りは任意。ヘッダは弱い依存としてだけ使う。
   plan が読めない環境 (gui がリンクされていない単体 CLI) でも
   clean は従来通り動く。 */
#if defined(__has_include)
#  if __has_include("nx_node.hpp")
#    include "nx_node.hpp"
#    define NX_CLEAN_CAN_READ_PLAN 1
#  endif
#endif
#ifndef NX_CLEAN_CAN_READ_PLAN
#  define NX_CLEAN_CAN_READ_PLAN 0
#endif

#include "../nx_deepai.h"
#include "../nx_leveldb.h"
#include "../nx_state.h"
#include "../attach/nx_appfix.h"

#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int nx_advise_tabs(const char *source, const char *state_json,
                              const char *check_id, FILE *out);
extern "C" int nx_advise_tab_can_delete(const char *state_json, const char *id,
                                        char *why_out, size_t why_cap);

namespace {

const char *miss_name(nx_miss_kind_t k) {
    switch (k) {
        case NX_MISS_ORPHAN_REF: return "orphan-ref";
        case NX_MISS_DUPLICATE:  return "duplicate";
        case NX_MISS_STORE_SPLIT:return "store-split";
        case NX_MISS_WAL_DAMAGE: return "wal-damage";
        default: return "none";
    }
}
/* 永続層の外枠が健全かだけを見る。値は書かない。
 *
 * 実装は nx_store_facts (plan/nx_store_facts) に置いた。
 * ここはその結果を clean の都合の形に写すだけの薄い層。
 * 「キーが生きているか」の判断を 2 箇所に持たないため、この層では
 * 一切推測しない。以前は list_keys で退避を数えていたが、あれは
 * seq も削除レコードも見ないので、tombstone 後も永久に「残っている」
 * と答えていた (clean を何度でも走らせる原因)。 */
struct store_facts_t {
    int  is_dir;
    int  resolve_backup;      /* 解決先が退避コピーにすり替わっている */
    int  backup_exists;       /* 退避キーが seq 上まだ生きている */
    int  n_backup;
    unsigned long long max_seq;
};

store_facts_t read_store(const char *path) {
    store_facts_t f;
    std::memset(&f, 0, sizeof(f));
    nx::StoreFacts s = nx::store_observe(path);
    f.is_dir = s.is_dir;
    f.resolve_backup = s.backup.resolved ? 1 : 0;
    /* 生きている退避の数。0 か 1 しかないが、以後複数になっても同じ形で扱える。 */
    f.n_backup = s.backup.exists ? 1 : 0;
    f.backup_exists = f.n_backup;
    f.max_seq = s.max_seq;
    return f;
}
struct step_t {
    const char *id;      /* "persist" / "live-sync" / "purge-backup" / "repair-seq" */
    const char *cmd;     /* 対応する既存コマンド (記録用)                          */
    const char *why;
    double      risk;
};

/* 1 つ先を試して、だめなら別手に切り替えるための試行を積む器。 */
struct attempt_t {
    int    stage;
    std::string name;
    std::string result;
};

} // namespace
/* ---- analyze の予算 (フレーム) ----
 *
 * 時間をかける、というのは「何フレーム待ってよいか」で表す。
 * ゲーム AI が 1 手に使えるフレーム数を決めるのと同じ考え方で、
 * analyze が 1 回の判断に使える「観測の回数」を制限する。
 *
 * なぜフレームか:
 *   観測 (await) は 1 回ごとに状態を読み直す。何回読んだかを数えないと
 *   「調べ尽くすまで終わらない」コマンドになる。回数で切れば必ず終わる。
 *
 * 効率化 (画像処理の考え方):
 *   全画素を等しく調べない。まず粗い解像度で当たりをつけ、
/* ---- フレーム (ゲームと同じ数え方) ----
 *
 * ゲームでは「1 フレーム進む」たびに、その瞬間に決められることを全部決める。
 * ここも同じで、**wait する地点 (観測を投げる所) が 1 フレーム**。
 *
 * 大事なのは、1 フレームで**並列に進められる観測はまとめて投げる**こと。
 *   ・同じフレームで読めるものは同時に読み、結果をまとめて次の判断材料にする。
 *   ・並列にした分だけフレームを消費しない (待ち時間は同じだから)。
 *   ・だから「1 フレームで何を並列に進めるか」を決めるのが予想の本体。
 *
 * 予想の進め方:
 *   各フレームで、今ある候補 (疑い) を全部並べ、
 *   「この候補はどの観測で消せる/確定できるか」を見て、
 *   独立に投げられる観測を 1 束にして投げる。
 *   結果で候補が絞れれば、次のフレームでさらに絞る。
 *   予算が尽きたら、その時点で残った候補を結論にする。
 *
 * 粗い/細かい、という別の数え方はしない。全部この 1 つのモデルで表す。
 */
/* ---- 1 フレームが「どれだけ動いたか」----
 *
 * フレームを投げる目的は、状態についての理解が**前に進むこと**。
 * 投げた本数だけ数えても意味がない (動かない観測を 100 回投げても
 * 理解は進まないし、時間だけ使う)。
 *
 * だから各フレームに、動いた量を持たせる:
 *   seq_before/seq_after : 永続層の seq がどう変わったか (raw)
 *   fingerprint          : ディレクトリの指紋が変わったか (raw)
 *   resolved             : その観測で確定した候補数 (semantic)
 *   opened               : その観測で新しく出てきた疑い (semantic)
 *   verdict_changed      : 結論が変わったか (最重要)
 *
 * 「動いた」の判定は semantic を優先する。seq が増えても
 * 結論が変わらなければ、理解は進んでいない。
 */
struct FrameMove {
    unsigned long long seq_before = 0;
    unsigned long long seq_after  = 0;
    unsigned long long fp_before  = 0;
    unsigned long long fp_after   = 0;
    int  resolved        = 0;   /* 確定した候補 */
    int  opened          = 0;   /* 新たに出た疑い */
    int  cut             = 0;   /* 計算だけで切った */
    bool verdict_changed = false;

    /* 理解が進んだか。semantic を見る。 */
    bool progress() const {
        return verdict_changed || resolved > 0 || opened > 0 || cut > 0;
    }
    /* 状態そのものが動いたか (raw)。書き込みがあったかの確認用。 */
    bool state_moved() const {
        return seq_after != seq_before || fp_after != fp_before;
    }
};

struct FrameBudget {
    int total;         /* 1 手に使える総フレーム */
    int used;          /* 使ったフレーム */
    int total_probes;  /* まとめて投げた観測の総数 */
    int total_cut;     /* 計算だけで切った総数 */
    int total_resolved;/* 確定した候補の総数 */
    FrameMove last;    /* 直前のフレームでどれだけ動いたか */
    int idle_frames;   /* 何も動かなかったフレーム数 (連続) */

    FrameBudget(int t) : total(t), used(0), total_probes(0),
                         total_cut(0), total_resolved(0), idle_frames(0) {}

    bool left() const { return used < total; }

    /* 1 フレーム進める。
       parallel : まとめて投げた観測数
       mv       : このフレームでどれだけ動いたか
       戻り値   : 進めたら true。予算切れ、または「動かない」が
                  続いて打ち切るなら false。 */
    bool step(int parallel, const FrameMove &mv) {
        if (!left()) return false;
        used++;
        if (parallel > 0) total_probes += parallel;
        total_cut += mv.cut;
        total_resolved += mv.resolved;
        if (mv.progress()) idle_frames = 0;
        else idle_frames++;
        last = mv;
        return true;
    }

    /* これ以上投げても動かないと分かっているか。
       2 フレーム続けて何も動かなければ、理解は進まない。
       時間をかけても無駄なので打ち切る。 */
    bool exhausted() const { return idle_frames >= 2; }
};

/* 状態パスから leveldb のディレクトリを得る。 */
static const char *dir_for(const char *state_path) {
    return state_path ? state_path : "";
}

/* フレームの前後で状態が動いたかを見るための指紋。
   値そのものではなく「変わったか」だけを見るので、軽い。 */
static unsigned long long frame_fingerprint(const char *state_path) {
    unsigned long long h = 1469598103934665603ULL;
    struct stat st;
    if (!state_path || stat(state_path, &st) != 0) return 0;
    h = (h ^ (unsigned long long)st.st_mtime) * 1099511628211ULL;
    h = (h ^ (unsigned long long)st.st_size) * 1099511628211ULL;
    return h ? h : 1;
}
static int budget_for(double risk_overall) {
    int f = 2;
    if (risk_overall >= 0.9) f += 4;
    else if (risk_overall >= 0.5) f += 2;
    if (f > 8) f = 8;
    return f;
}

/* ---- 直列の計算だけで切る (hardcode) ----
 *
 * 観測 (wait) をせずに消せる候補は、先に全部消す。計算だけなので待ちがない。
 * ゲームでいう「合法手のうち、明らかに負ける手を先に落とす」に当たる。
 *
 * ここで切れるものを、あえて観測で確かめない。
 * 確かめる必要が無いものを確かめると、フレームを無駄に使う。
 *
 * 切る規則 (決め打ち):
 *   1. 観測値が既に手元にある候補は、観測しない
 *      (同じ状態を 2 度読まない)
 *   2. 前提が成り立たない候補は、観測しない
 *      (例: 退避キーが死んでいれば stale-backup は起こりえない)
 *   3. 他の候補の結論で決まる候補は、観測しない
 *      (例: seq が桁違いなら、他の原因はどうでもよい)
 *
 * 直列に計算するだけで、候補は急速に減る。減った分だけ
 * 残りのフレームを深い観測に使える。
 */
struct Candidate {
    const char *id;
    bool        alive;   /* まだ疑わしいか */
    const char *cut_why; /* 切った理由 (切っていなければ nullptr) */
};

/* 計算だけで候補を切る。wait しない。返り値は切った数。 */
static int serial_cut(Candidate *c, int n, bool backup_alive,
                      bool seq_poisoned, bool local_miss) {
    int cut = 0;
    for (int i = 0; i < n; i++) {
        if (!c[i].alive) continue;
        const char *why = nullptr;

        /* 規則 2: 前提が成り立たない */
        if (std::strcmp(c[i].id, "stale-backup") == 0 && !backup_alive)
            why = "退避が死んでいる (前提が偽)";
        else if (std::strcmp(c[i].id, "repair-seq") == 0 && !seq_poisoned)
            why = "seq は正常 (前提が偽)";
        /* 規則 3: 他の結論で決まる */
        else if (seq_poisoned && std::strcmp(c[i].id, "repair-seq") != 0)
            why = "seq が壊れているので他は後回し";
        /* 規則 1: 既に観測済みで問題が無い */
        else if (!local_miss &&
                 (std::strcmp(c[i].id, "persist") == 0 ||
                  std::strcmp(c[i].id, "live-sync") == 0))
            why = "残滓の観測で既に問題なし";

        if (why) { c[i].alive = false; c[i].cut_why = why; cut++; }
    }
    return cut;
}

void nx_clean_opts_default(nx_clean_opts_t *o) {
    if (!o) return;
    o->live = 1;
    o->dry_run = 0;
    o->notes_out = nullptr;
    o->app = nullptr;
    o->store = nullptr;
    o->key = nullptr;
}

/* plan が言っている問題を読み取る (任意)。
   analyze の材料の 1 つ。読めなければ 0 件として扱い、自前の観測で進む。

   ここで使うのは「予想するコマンド」だけ。plan は状態を書かないので、
   analyze の途中で状態を変えないことが保証される。 */
struct plan_read_t {
    int   n_findings;
    int   n_mapped;
    bool  read_ok;
};

/* 予算を渡して読み取り、使ったフレーム数を返す。
   予算が無ければ読まない (時間をかけない)。 */
static plan_read_t read_plan_findings(const char *state_path, FILE *out,
                                      FrameBudget *fb) {
    plan_read_t r;
    r.n_findings = 0; r.n_mapped = 0; r.read_ok = false;

    /* フレームは呼び側 (観測の前後を知っている側) で進める。
       ここでは予算が残っているかだけを見て、無ければ読まない。 */
    if (fb && !fb->left()) {
        std::fprintf(out, "  [analyze] フレーム予算切れ — plan を読まない\n");
        return r;
    }
#if NX_CLEAN_CAN_READ_PLAN
    std::vector<std::string> a = {"plan"};
    if (state_path && *state_path) { a.push_back("--state"); a.push_back(state_path); }
    nx::NodeResult pr = nx::node::run(a);
    if (!pr.ok()) {
        std::fprintf(out, "  [analyze] plan は読めない — 自前の観測だけで進む\n");
        return r;
    }
    r.read_ok = true;
    std::fprintf(out, "  [analyze] plan の finding:\n");
    for (const auto &ln : nx::node::lines(pr.out)) {
        if (ln.find("evidence:") != std::string::npos) {
            std::fprintf(out, "    %s\n", ln.c_str());
            continue;
        }
        /* "  <id> sev=.. fixable=..  <summary>" */
        if (ln.size() < 3 || ln[0] != ' ' || ln[1] != ' ') continue;
        std::string body = ln.substr(2);
        size_t sp = body.find(' ');
        if (sp == std::string::npos) continue;
        std::string fid = body.substr(0, sp);
        if (fid.empty() || body.find("sev=") == std::string::npos) continue;
        r.n_findings++;
        /* plan が出す finding id と fix の段を 1:1 で対応づける。 */
        const char *maps_to = nullptr;
        if (fid == "orphan-object")         maps_to = "persist";
        else if (fid == "store-divergence")  maps_to = "live-sync";
        else if (fid == "wal-torn")          maps_to = "repair-seq";
        else if (fid == "seq-poisoned")      maps_to = "repair-seq";
        else if (fid == "stale-backup")      maps_to = "purge-backup";
        if (maps_to) r.n_mapped++;
        std::fprintf(out, "    %-18s -> %s\n", fid.c_str(),
                     maps_to ? maps_to : "(段が無い)");
    }
    if (r.n_findings == 0) std::fprintf(out, "    なし\n");
#else
    (void)state_path;
    std::fprintf(out, "  [analyze] このビルドでは plan を読めない\n");
#endif
    return r;
}

extern "C" int nx_clean_state(const char *state_path, const nx_clean_opts_t *in_opts,
                              FILE *out) {
    if (!out) out = stdout;
    nx_clean_opts_t opts;
    nx_clean_opts_default(&opts);
    if (in_opts) opts = *in_opts;
    if (!state_path) { std::fprintf(out, "clean: --state required\n"); return 2; }

    /* ---------------- 1 回目の観測 ---------------- */
    char *state = nx_deepai_load_state(state_path);
    if (!state) {
        std::fprintf(out, "  [clean] no tabs state at %s\n", state_path);
        return 2;
    }
    nx_residual_map_t before;
    if (nx_residual_collect(state, &before) != 0) {
        std::fprintf(out, "  [clean] state is not a tabs-shaped JSON\n");
        std::free(state);
        return 2;
    }
    store_facts_t sf = read_store(state_path);
    nx_residual_risk_t risk = nx_residual_risk(&before);
    nx_miss_kind_t miss = nx_residual_miss_kind(&before);
    if (sf.resolve_backup) miss = NX_MISS_STORE_SPLIT;
    /* 退避が残っていて、かつ解決先にはなっていない場合。
       「いまは読まれていない」と「将来も読まれない」は違う。
       退避はアプリが本体を書いた瞬間に復活源になりうるので、
       見落としとして扱う。 */
    else if (sf.backup_exists) miss = NX_MISS_STALE_BACKUP;
    if (sf.max_seq > 0 && sf.max_seq >= 100000000ULL) {
        /* 桁違いの seq が入っていると、以降アプリ側の書き込みが無視される */
        risk.order_sensitive = 1.0;
        if (miss == NX_MISS_NONE) miss = NX_MISS_WAL_DAMAGE;
    }

    std::fprintf(out, "=== clean: 残滓を状態から判定して消す ===\n");
    std::fprintf(out, "source=%s\n", state_path);
    nx_residual_dump(state_path, &before, out);
    std::fprintf(out, "  resolution: %s\n", sf.resolve_backup ? "backup (要修復)" : "canonical");
    std::fprintf(out, "  max seq   : %llu%s\n", sf.max_seq,
                 sf.max_seq >= 100000000ULL ? "  (異常に大きい)" : "");
    std::fprintf(out, "  miss kind : %s\n", miss_name(miss));
    std::fprintf(out, "  risk      : destructive=%.2f order=%.2f live=%.2f subtle=%.2f\n",
                 risk.destructive, risk.order_sensitive, risk.live_sensitive, risk.subtle);
    if (opts.notes_out) std::fprintf(opts.notes_out, "notes=%s\n", miss_name(miss));

    /* ================================================================
     * ここから 2 段に分ける。
     *
     *   analyze : 何が問題かを決める。状態を書かない。安い。
     *   fix     : analyze が出した段だけを実行する。
     *
     * 混ぜない理由:
     *   ・analyze だけで終われる (--dry-run / 問題なし) ときに
     *     plan を読む必要が無い。読むのは「analyze が問題を見つけた」後。
     *   ・fix は analyze の結論だけを見る。fix の中で判定し直さない。
     *
     * plan は analyze の材料の 1 つ。自前の観測 (residual) で足りない
     * ときにだけ読みに行く。無くても analyze は成立する。
     * ================================================================ */

    /* ---------------- analyze 1: 自前の観測で足りるか ---------------- */
    /* miss は nx_residual の観測から出た「見落としの種類」。
       ここが NONE で、退避も残っていなければ、解析する材料が無い。 */
    const bool local_clean =
        (miss == NX_MISS_NONE) && !sf.backup_exists && sf.max_seq < 100000000ULL;

    if (local_clean) {
        /* 問題なし。plan は読まない (読む理由が無いので読まない)。 */
        std::fprintf(out, "  [analyze] 残滓なし — plan は読まない\n");
        nx_residual_free(&before);
        std::free(state);
        return 0;
    }

    /* ---------------- analyze 2: plan を材料として読む (任意) ----------------
       自前の観測で問題が見つかったときだけ、plan に第二の目を求める。
       plan が読めなくても analyze は成立する (自前の観測で進む)。 */
    const double risk_overall =
        std::max(std::max(risk.destructive, risk.order_sensitive),
                 std::max(risk.live_sensitive, risk.subtle));
    FrameBudget fb(budget_for(risk_overall));

    /* ---- 先に計算だけで切る (wait しない) ----
       候補を並べ、前提と他候補の結論だけで消す。
       ここで減った分だけ、残りを深い観測に使える。 */
    Candidate cand[] = {
        { "persist",      true, nullptr },
        { "live-sync",    true, nullptr },
        { "purge-backup", sf.backup_exists != 0, nullptr },
        { "repair-seq",   risk.order_sensitive >= 0.9, nullptr },
    };
    const int ncand = (int)(sizeof(cand) / sizeof(cand[0]));
    const bool seq_poisoned = (sf.max_seq >= 100000000ULL);
    const int ncut = serial_cut(cand, ncand, sf.backup_exists != 0,
                                seq_poisoned, miss != NX_MISS_NONE);
    std::fprintf(out, "  [analyze] 計算だけで %d 件切った (wait なし)\n", ncut);
    for (int i = 0; i < ncand; i++)
        if (!cand[i].alive && cand[i].cut_why)
            std::fprintf(out, "    cut %-12s %s\n", cand[i].id, cand[i].cut_why);

    /* ---- 残った候補だけを、1 フレームにまとめて観測する ----
       フレームには「どれだけ動いたか」を記録する。
       seq と指紋は観測の前後で取り、候補の確定数は観測後に数える。 */
    int alive = 0;
    for (int i = 0; i < ncand; i++) if (cand[i].alive) alive++;

    FrameMove mv;
    mv.seq_before = nx_state_max_seq(dir_for(state_path), "workspace-tabs");
    mv.fp_before = frame_fingerprint(state_path);
    mv.cut = ncut;

    plan_read_t pr = read_plan_findings(state_path, out, &fb);

    mv.seq_after = nx_state_max_seq(dir_for(state_path), "workspace-tabs");
    mv.fp_after = frame_fingerprint(state_path);
    /* この観測でどれだけ候補が確定したか。
       plan が問題を出していれば、対応する段が確定したと数える。 */
    mv.resolved = pr.n_mapped;
    mv.opened = pr.n_findings - pr.n_mapped;
    /* 結論が変わったか (観測前の見立てと、観測後で違うか)。 */
    mv.verdict_changed = (pr.n_findings > 0) != (miss != NX_MISS_NONE);

    fb.step(alive, mv);

    /* frame の動きをプロジェクトに残す (path が決まっているときだけ)。
       保存できなくても analyze は止めない (記録は副次的)。 */
    if (const char *proj = std::getenv("NX_PROJECT")) {
        nx_project_save_frame(proj, fb.used, mv.seq_before, mv.seq_after,
                              mv.resolved, mv.opened, mv.cut,
                              mv.progress() ? 1 : 0, nullptr);
    }

    /* plan が「問題なし」と言っていて、自前の観測も退避だけなら、
       退避の掃除だけが残る。逆に plan が問題を出していれば、
       その finding に対応する段を fix に渡す。 */
    std::fprintf(out, "  [analyze] 自前の観測: %s / plan: %s\n",
                 miss_name(miss),
                 pr.read_ok ? (pr.n_findings ? "問題あり" : "問題なし") : "読めず");
    std::fprintf(out, "  [analyze] frame %d: seq %llu->%llu, resolved=%d opened=%d cut=%d\n",
                 fb.used, mv.seq_before, mv.seq_after, mv.resolved, mv.opened, mv.cut);
    std::fprintf(out, "  [analyze] 動いた=%s (state=%s / 理解=%s) idle=%d\n",
                 mv.progress() ? "yes" : "no",
                 mv.state_moved() ? "changed" : "same",
                 mv.progress() ? "advanced" : "stuck",
                 fb.idle_frames);
    std::fprintf(out, "  [analyze] frames %d/%d, probes %d — %s\n",
                 fb.used, fb.total, fb.total_probes,
                 fb.exhausted() ? "これ以上動かないので打ち切り"
                 : fb.left() ? "余りあり" : "使い切った");


    /* ---------------- 2. 選択: 状態から呼ぶ順を決める ---------------- */
    std::vector<step_t> steps;
    /* 順序の理由:
       seq が汚染されていれば、値は正しくても書いた結果が読まれない。
       基盤 (seq) → 永続層 (persist) → 退避の掃除 → 稼働層 (live-sync)。
       稼働層を最初にすると、次の persist で戻る。 */
    if (risk.order_sensitive >= 0.9) {
        steps.push_back({"repair-seq", "apply --live",
                         "seq が異常値。実測最大+1 で書き直さないと以後の書き込みが読まれない",
                         1.0});
    }
    steps.push_back({"persist", "apply", "残滓を永続層から除く (正準キーへ seq+1)", 0.5});
    /* 退避キーは、解決先になっていなくても消す。
       残しておくと次に本体が書かれた瞬間に復活源になる。 */
    if (miss == NX_MISS_STORE_SPLIT || miss == NX_MISS_STALE_BACKUP) {
        steps.push_back({"purge-backup", "delete",
                         miss == NX_MISS_STORE_SPLIT
                           ? "解決先が退避コピー。本体を消すと退避側が復活源になる"
                           : "退避コピーが残っている。次に本体が書かれると復活源になる",
                         0.6});
    }
    if (opts.live && risk.live_sensitive >= 0.5) {
        steps.push_back({"live-sync", "heal", "永続層だけ直すと次の persist で戻る",
                         risk.live_sensitive});
    }

    std::fprintf(out, "\n  chosen steps (%zu):\n", steps.size());
    for (size_t i = 0; i < steps.size(); i++)
        std::fprintf(out, "    %zu. %-12s cmd=%-12s risk=%.2f  %s\n", i + 1,
                     steps[i].id, steps[i].cmd, steps[i].risk, steps[i].why);

    if (opts.dry_run) {
        std::fprintf(out, "  [clean] dry-run: 何も変更していません\n");
        nx_residual_free(&before);
        std::free(state);
        return 0;
    }

    /* ---------------- 3. 消す実体を 1 度だけ決める ---------------- */
    int *stale = nullptr;
    const int n_stale = nx_residual_stale_indices(&before, &stale);
    std::fprintf(out, "\n  [clean] stale entities: %d\n", n_stale);
    for (int i = 0; i < n_stale; i++)
        std::fprintf(out, "    [%d] %s  %s\n", stale[i], before.entities[stale[i]].kind,
                     before.entities[stale[i]].label);

    char *pruned = nullptr;
    int prune_rc = nx_residual_prune_json(state, stale, n_stale, &pruned);
    std::free(stale);
    nx_residual_free(&before);
    if (n_stale > 0 && (prune_rc != 0 || !pruned)) {
        std::fprintf(out, "  [clean] failed to build pruned state (rc=%d)\n", prune_rc);
        std::free(pruned);
        std::free(state);
        return 2;
    }
    /* 残滓 0 件なら現状をそのまま使う。
       退避の掃除だけが目的のとき、ここで pruned を空のまま進めない。 */
    if (!pruned) pruned = state;   /* 所有権を受ける */
    else std::free(state);

    /* ---------------- 4. 実行 ---------------- */
    std::vector<attempt_t> tried;
    int persist_ok = 0;

    /* ディレクトリ化 (leveldb の書き込み先) */
    char dir[4096];
    std::snprintf(dir, sizeof dir, "%s", state_path);
    {
        char cur[4200];
        std::snprintf(cur, sizeof cur, "%s/CURRENT", state_path);
        FILE *fp = std::fopen(cur, "rb");
        if (fp) std::fclose(fp);
        else {
            char *sl = std::strrchr(dir, '/');
            if (sl) *sl = '\0';
        }
    }

    for (const auto &st : steps) {
        std::fprintf(out, "\n  [stage] %s (%s)\n", st.id, st.cmd);
        int rc = -1;
        if (std::strcmp(st.id, "persist") == 0 || std::strcmp(st.id, "repair-seq") == 0) {
            size_t klen = 0;
            char *key = nx_state_find_key(dir, "workspace-tabs", &klen);
            if (!key) {
                std::fprintf(out, "    no key containing 'workspace-tabs' in %s\n", dir);
            } else {
                /* seq は実測最大+1。桁違いの値は書かない (以後の書き込みを殺すため)。 */
                unsigned long long seq = nx_state_max_seq(dir, "workspace-tabs") + 1;
                rc = nx_leveldb_append_kv(dir, key, klen,
                                          (const unsigned char *)pruned,
                                          std::strlen(pruned), seq, nullptr, out);
                std::fprintf(out, "    wrote %zu bytes (seq=%llu) rc=%d\n",
                             std::strlen(pruned), seq, rc);
                std::free(key);
                /* 書いたつもりを成功と呼ばない: 解決し直して中身を比べる。 */
                char *back = nx_deepai_load_state(dir);
                nx_residual_map_t after;
                int ok = back && nx_residual_collect(back, &after) == 0;
                if (ok) {
                    int *s2 = nullptr;
                    int n2 = nx_residual_stale_indices(&after, &s2);
                    ok = (n2 == 0);
                    std::fprintf(out, "    verify: stale=%d -> %s\n", n2,
                                 ok ? "clean" : "still dirty");
                    std::free(s2);
                    nx_residual_free(&after);
                }
                std::free(back);
                if (rc == 0 && ok) persist_ok = 1;
                else rc = -1;
            }
        } else if (std::strcmp(st.id, "purge-backup") == 0) {
            char **keys = nullptr;
            size_t *lens = nullptr;
            int nb = nx_state_list_keys(dir, "nx-backup", &keys, &lens, 16);
            unsigned long long seq = nx_state_max_seq(dir, nullptr) + 1;
            rc = 0;
            for (int i = 0; i < nb; i++)
                if (nx_leveldb_append_delete(dir, keys[i], lens[i], seq++, nullptr, out) != 0)
                    rc = -1;
            for (int i = 0; i < nb; i++) std::free(keys[i]);
            std::free(keys);
            std::free(lens);
            std::fprintf(out, "    tombstoned %d backup key(s) rc=%d\n", nb, rc);
        } else if (std::strcmp(st.id, "live-sync") == 0) {
            /* 稼働中ストアは既存の heal 経路 (nx_appfix) に任せる。 */
            nx_appfix_profile_t prof = *nx_appfix_default_profile();
            if (opts.app) prof.app_name = opts.app;
            if (opts.store) prof.store_substr = opts.store;
            if (opts.key) prof.key = opts.key;
            rc = nx_appfix_apply(&prof, pruned, std::strlen(pruned), out);
            std::fprintf(out, "    live store rc=%d (reload しない)\n", rc);
        }
        attempt_t a;
        a.stage = (int)tried.size() + 1;
        a.name = st.id;
        a.result = rc == 0 ? "ok" : "failed";
        tried.push_back(a);
        if (rc != 0 && (std::strcmp(st.id, "persist") == 0 ||
                        std::strcmp(st.id, "repair-seq") == 0)) {
            /* 永続層に書けなければ以降は意味が無い。ここで止める。 */
            std::fprintf(out, "    stopping: 永続層に書けていません\n");
            break;
        }
    }

    if (opts.notes_out) {
        for (const auto &t : tried)
            std::fprintf(opts.notes_out, "step%d=%s %s\n", t.stage, t.name.c_str(),
                         t.result.c_str());
    }

    /* ---------------- 5. 再観測 ---------------- */
    char *after_js = nx_deepai_load_state(state_path);
    int clean = 0;
    std::fprintf(out, "\n  === re-observe ===\n");
    if (after_js) {
        nx_residual_map_t after;
        if (nx_residual_collect(after_js, &after) == 0) {
            nx_residual_dump(state_path, &after, out);
            int *s2 = nullptr;
            int n2 = nx_residual_stale_indices(&after, &s2);
            clean = (n2 == 0);
            std::fprintf(out, "  residual ids: %d\n", n2);
            std::free(s2);
            nx_residual_free(&after);
        }
        std::free(after_js);
    }
    store_facts_t sf2 = read_store(state_path);
    if (sf2.resolve_backup) clean = 0;

    std::fprintf(out, "\n  VERDICT=%s\n", clean ? "CLEAN" : "STILL_DIRTY");
    std::fprintf(out, "  persisted=%s  live=%s\n", persist_ok ? "yes" : "no",
                 opts.live ? "synced" : "untouched(--no-live)");
    std::free(pruned);
    return clean ? 0 : 1;
}
