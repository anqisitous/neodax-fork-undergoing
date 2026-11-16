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

#include "../nx_deepai.h"
#include "../nx_leveldb.h"
#include "../nx_state.h"
#include "../attach/nx_appfix.h"

#include <cstdio>
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

/* ストアの外枠 (書き込み基盤) が健全かだけを見る。値は書かない。 */
struct store_facts_t {
    int  is_dir;
    int  resolve_backup;      /* 解決先が退避コピーにすり替わっている */
    unsigned long long max_seq;
};

store_facts_t read_store(const char *path) {
    store_facts_t f;
    std::memset(&f, 0, sizeof(f));
    char dir[4096];
    std::snprintf(dir, sizeof dir, "%s", path);
    f.is_dir = 0;
    /* ディレクトリかどうかは CURRENT の有無で判定する (leveldb の正準識別) */
    char cur[4200];
    std::snprintf(cur, sizeof cur, "%s/CURRENT", path);
    FILE *fp = std::fopen(cur, "rb");
    if (fp) { std::fclose(fp); f.is_dir = 1; }
    if (!f.is_dir) {
        char *sl = std::strrchr(dir, '/');
        if (sl) *sl = '\0';
    }
    size_t klen = 0;
    char *k = nx_state_find_key(dir, "workspace-tabs", &klen);
    if (k) {
        if (klen >= 10 && std::memcmp(k + klen - 10, ".nx-backup", 10) == 0)
            f.resolve_backup = 1;
        std::free(k);
    }
    f.max_seq = nx_state_max_seq(dir, "workspace-tabs");
    return f;
}

/* 選んだ段。命令名は既存コマンドと同じ名前を使い、対応を読み手に明示する。 */
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

void nx_clean_opts_default(nx_clean_opts_t *o) {
    if (!o) return;
    o->live = 1;
    o->dry_run = 0;
    o->notes_out = nullptr;
    o->app = nullptr;
    o->store = nullptr;
    o->key = nullptr;
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

    if (before.n_refs == 0 && before.n_dups == 0 && !sf.resolve_backup &&
        sf.max_seq < 100000000ULL) {
        std::fprintf(out, "  [clean] 残滓なし\n");
        nx_residual_free(&before);
        std::free(state);
        return 0;
    }

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
    if (sf.resolve_backup) {
        steps.push_back({"purge-backup", "delete", "解決先が退避コピー。本体を消すと退避側が復活源になる",
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
    std::free(state);
    if (prune_rc != 0 || !pruned) {
        std::fprintf(out, "  [clean] failed to build pruned state (rc=%d)\n", prune_rc);
        std::free(pruned);
        return 2;
    }

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
