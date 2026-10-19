/*
 * depflow.c — 静的依存グラフ追跡 (Static Dependency Flow)
 *
 * 目的
 * ─────
 *   バイナリ全体を「同じコードが毎回変わらない」前提で静的に読み、
 *   ある出力点（例: 将棋AIの最終着手を返すret）が使っている変数を
 *   有限回だけ逆向きに辿って、それぞれの出どころを
 *     定数 / 呼び出し元から受け取った引数 / グローバル領域への書き込み
 *     (xrefで列挙) / 外部関数の戻り値 / ヒープ上の値(別名不明で打ち切り)
 *   のどれかに分類する。加えて、関数の実行順が
 *     - 呼び出しグラフ上の依存で一意に決まる順序か
 *     - pthread_create / dispatch_async 等で起動される並列実行か
 *   のどちらかを判定し、並列起動点には起動辺、待ち合わせ
 *   （pthread_join / dispatch_group_wait 等）には合流辺を引く。
 *
 * 材料
 * ─────
 *   各関数のNR (nr_ctx_t, decomp.c の nr_lift_func を dax_nr_lift_func_raw
 *   経由で取得) が持つ def-use (dst/src1/src2/src3) と、NR_CALL の
 *   callee_func_idx が、このパスの一次データ。関数をまたいだ辺は
 *   callgraph.c と同じ「call命令のaddrからcalleeへ」のエッジを使う。
 *
 * 限界（静的に決まらないもの。隠さずに「不明」として残す）
 * ─────────────────────────────────────────────────────────
 *   - 関数ポインタ経由の間接呼び出し（NR_INDIRECT_CALL）
 *   - objc_msgSend 等の動的ディスパッチ
 *   - ヒープ上の値の別名（load元が静的に一意定まらない場合）
 *   これらは DEPFLOW_UNKNOWN として出力に残し、黙って消さない。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dax.h"
#include "dax_guard.h"
#include "nr_types.h"

/* ═══════════════════════════════════════════════════════════════════
 * §1  依存辺の種別と「出どころ」の分類
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    DEPFLOW_CONST = 0,     /* 即値 */
    DEPFLOW_PARAM,         /* 呼び出し元から渡された引数 (x0-x7相当) */
    DEPFLOW_GLOBAL,        /* __DATA等への書き込み元 (xrefで特定) */
    DEPFLOW_EXTCALL,       /* 外部(未定義)シンボルの戻り値 */
    DEPFLOW_LOCAL_DEF,     /* 同一関数内の先行def (さらに辿れる) */
    DEPFLOW_CALLEE_RET,    /* 既知の内部関数の戻り値 (さらに辿れる) */
    DEPFLOW_RECURSION_CUT, /* コールグラフSCC: 上限深さで打ち切り */
    DEPFLOW_UNKNOWN        /* 間接呼び出し/動的ディスパッチ/別名不明 */
} depflow_origin_t;

static const char *depflow_origin_name(depflow_origin_t o) {
    switch (o) {
        case DEPFLOW_CONST:         return "const";
        case DEPFLOW_PARAM:         return "param";
        case DEPFLOW_GLOBAL:        return "global";
        case DEPFLOW_EXTCALL:       return "extcall";
        case DEPFLOW_LOCAL_DEF:     return "local-def";
        case DEPFLOW_CALLEE_RET:    return "callee-ret";
        case DEPFLOW_RECURSION_CUT: return "recursion-cut";
        default:                    return "unknown";
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * §2  並列/順序 API の分類
 *
 *   NeoDAXのシンボル解決(dax_sym_name)が通る名前だけを対象にする。
 *   __stubs/la_symbol_ptr経由の未定義シンボルは macho.c が
 *   SEC_TYPE_PLT として区別しているが、indirect symbol table から
 *   実名を引く処理が元実装に無いため、まずシンボルテーブルに乗って
 *   いる名前（動的リンクでなく静的に解決できる範囲）で判定する。
 *   indirect symbol解決を足す場合はmacho.cの拡張が別途要る。
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    PAR_NONE = 0,
    PAR_SPAWN,   /* pthread_create, dispatch_async, dispatch_async_f,
                    NSThread detachNewThreadSelector 等: 起動辺 */
    PAR_JOIN     /* pthread_join, dispatch_group_wait, dispatch_semaphore_wait,
                    pthread_mutex_lock 等: 合流辺(順序を強制) */
} depflow_par_kind_t;

static depflow_par_kind_t depflow_classify_call(const char *name) {
    static const char *spawn_names[] = {
        "pthread_create", "dispatch_async", "dispatch_async_f",
        "dispatch_barrier_async", "dispatch_apply", "dispatch_apply_f",
        "NSThread", "detachNewThreadSelector", NULL
    };
    static const char *join_names[] = {
        "pthread_join", "dispatch_group_wait", "dispatch_semaphore_wait",
        "pthread_mutex_lock", "pthread_cond_wait", "dispatch_sync",
        "dispatch_barrier_sync", NULL
    };
    int i;
    if (!name || !name[0]) return PAR_NONE;
    for (i = 0; spawn_names[i]; i++)
        if (strstr(name, spawn_names[i])) return PAR_SPAWN;
    for (i = 0; join_names[i]; i++)
        if (strstr(name, join_names[i])) return PAR_JOIN;
    return PAR_NONE;
}

/* ═══════════════════════════════════════════════════════════════════
 * §3  関数単位のNRキャッシュ
 *
 *   バイナリ全体を辿る間、同じ関数のNRを何度も作り直さないよう
 *   ロードして使い回す。nr_ctx_tはサイズが大きい(NR_MAX_STMTS=3072)ため
 *   ヒープ確保で、使い終わったらdax_depflow_free_cacheでまとめて解放。
 * ═══════════════════════════════════════════════════════════════════ */

#define DEPFLOW_MAX_FUNCS_CACHE DAX_MAX_FUNCTIONS

typedef struct {
    nr_ctx_t *ctx[DEPFLOW_MAX_FUNCS_CACHE];
    int       cached;
} depflow_cache_t;

static nr_ctx_t *depflow_get_ctx(dax_binary_t *bin, depflow_cache_t *cache, int func_idx) {
    if (func_idx < 0 || func_idx >= bin->nfunctions || func_idx >= DEPFLOW_MAX_FUNCS_CACHE)
        return NULL;
    if (!cache->ctx[func_idx]) {
        cache->ctx[func_idx] = (nr_ctx_t *)dax_nr_lift_func_raw(bin, func_idx);
    }
    return cache->ctx[func_idx];
}

static void depflow_free_cache(depflow_cache_t *cache) {
    int i;
    for (i = 0; i < DEPFLOW_MAX_FUNCS_CACHE; i++) {
        if (cache->ctx[i]) { free(cache->ctx[i]); cache->ctx[i] = NULL; }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * §4  xrefからグローバル書き込み元を探す
 *
 *   dax_xref_build()が作るxref配列から、addr宛てのcall以外のxref
 *   (つまり実行アドレスではなくデータアドレスへの参照、またはSTORE時に
 *   使われるADRP/ADD/LDRが指す__DATA領域)を探す。
 *   NRのaddr_hintはNR側がbase名しか持たないため、ここでは
 *   「そのグローバルアドレスをターゲットとするxrefのうちis_call==0」を
 *   列挙するところまでに留める(厳密なストア元命令の特定はNR_STOREの
 *   src1アドレス注釈があればそれを優先)。
 * ═══════════════════════════════════════════════════════════════════ */

static int depflow_find_global_writers(dax_binary_t *bin, uint64_t global_addr,
                                        uint64_t *out_froms, int max_out) {
    dax_xref_t matches[64];
    int n, i, cnt = 0;
    if (!bin->xrefs) return 0;
    n = dax_xref_find_to(bin, global_addr, matches, 64);
    for (i = 0; i < n && cnt < max_out; i++) {
        if (!matches[i].is_call) {
            out_froms[cnt++] = matches[i].from;
        }
    }
    return cnt;
}

/* ═══════════════════════════════════════════════════════════════════
 * §5  単一NR変数の逆向き分類 (再帰、深さ制限つき)
 *
 *   var_idxはctx->vars[]のインデックス。同一関数内であれば、
 *   nr_stmt_tを末尾から走査して「dst==var_idxの最後のstmt」を
 *   そのdefとみなす（NRは出現順にSSA版を切っているため、末尾最新の
 *   defを拾えば version_map と整合する）。
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    dax_binary_t    *bin;
    depflow_cache_t *cache;
    dax_opts_t      *opts;
    FILE            *out;
    int              max_depth;
    int              indent_color;
} depflow_walk_t;

static void depflow_indent(FILE *out, int depth) {
    int i;
    for (i = 0; i < depth; i++) fputs("  ", out);
}

static void depflow_walk_var(depflow_walk_t *w, int func_idx, nr_ctx_t *ctx,
                              int var_idx, int depth, int *visited_funcs, int nvisited);

/* callee_func_idx側の「戻り値」を辿る: calleeのNR_RETのsrc1を使う。
 * calleeに複数のretがある場合は最初に見つかったものだけを辿る
 * (将棋AI等の単一returnパターンを想定; 複数returnがある関数は
 *  DEPFLOW_UNKNOWN寄りになるが、それ自体を欠落として出力に残す)。 */
static void depflow_walk_callee_return(depflow_walk_t *w, int callee_idx,
                                        int depth, int *visited_funcs, int nvisited) {
    nr_ctx_t *cctx;
    int i;
    /* 再帰/相互再帰の検出: すでに訪問済みの関数なら打ち切る */
    for (i = 0; i < nvisited; i++) {
        if (visited_funcs[i] == callee_idx) {
            depflow_indent(w->out, depth);
            fprintf(w->out, "<- [%s] func[%d] (再帰: 既訪問、深さ%dで打ち切り)\n",
                    depflow_origin_name(DEPFLOW_RECURSION_CUT), callee_idx, depth);
            return;
        }
    }
    cctx = depflow_get_ctx(w->bin, w->cache, callee_idx);
    if (!cctx) {
        depflow_indent(w->out, depth);
        fprintf(w->out, "<- [%s] func[%d] (NR構築失敗)\n",
                depflow_origin_name(DEPFLOW_UNKNOWN), callee_idx);
        return;
    }
    for (i = cctx->nstmts - 1; i >= 0; i--) {
        if (cctx->stmts[i].op == NR_RET && cctx->stmts[i].src1 >= 0) {
            int new_visited[64];
            int nv = nvisited < 63 ? nvisited : 63;
            memcpy(new_visited, visited_funcs, sizeof(int) * nv);
            new_visited[nv++] = callee_idx;
            depflow_indent(w->out, depth);
            fprintf(w->out, "<- [%s] func[%d] の戻り値 (ret時のsrc1を追跡)\n",
                    depflow_origin_name(DEPFLOW_CALLEE_RET), callee_idx);
            depflow_walk_var(w, callee_idx, cctx, cctx->stmts[i].src1,
                              depth + 1, new_visited, nv);
            return;
        }
    }
    depflow_indent(w->out, depth);
    fprintf(w->out, "<- [%s] func[%d] (ret文が見つからない/値を返さない)\n",
            depflow_origin_name(DEPFLOW_UNKNOWN), callee_idx);
}

static void depflow_walk_var(depflow_walk_t *w, int func_idx, nr_ctx_t *ctx,
                              int var_idx, int depth, int *visited_funcs, int nvisited) {
    int i;
    const char *base;

    if (depth > w->max_depth) {
        depflow_indent(w->out, depth);
        fprintf(w->out, "<- [%s] 深さ上限%dに到達、打ち切り\n",
                depflow_origin_name(DEPFLOW_RECURSION_CUT), w->max_depth);
        return;
    }
    if (var_idx < 0 || var_idx >= ctx->nvars) {
        depflow_indent(w->out, depth);
        fprintf(w->out, "<- [%s] var_idx不正(定数展開済みの可能性)\n",
                depflow_origin_name(DEPFLOW_CONST));
        return;
    }
    base = ctx->vars[var_idx].base;

    /* 引数レジスタ(x0-x7 / w0-w7 / edi,esi...)で、かつ同一関数内に
     * その変数へのdefが一つも無ければ「呼び出し元からの引数」とみなす。
     * 判定は「version==0 かつ定義stmtが見当たらない」で行う。 */

    for (i = ctx->nstmts - 1; i >= 0; i--) {
        nr_stmt_t *s = &ctx->stmts[i];
        if (s->dst != var_idx) continue;

        depflow_indent(w->out, depth);

        switch (s->op) {
        case NR_CALL:
            if (s->callee_func_idx >= 0) {
                fprintf(w->out, "<- [%s] %s := func[%d](%s) の呼び出し @0x%llx\n",
                        depflow_origin_name(DEPFLOW_CALLEE_RET), base,
                        s->callee_func_idx, s->callee_name,
                        (unsigned long long)s->addr);
                depflow_walk_callee_return(w, s->callee_func_idx, depth + 1,
                                            visited_funcs, nvisited);
            } else {
                /* callee_func_idx不明 = 未定義シンボル(外部関数)の戻り値 */
                fprintf(w->out, "<- [%s] %s := %s() の戻り値 @0x%llx (外部/未解決シンボル)\n",
                        depflow_origin_name(DEPFLOW_EXTCALL), base,
                        s->callee_name[0] ? s->callee_name : "?",
                        (unsigned long long)s->addr);
            }
            return;

        case NR_INDIRECT_CALL:
            fprintf(w->out, "<- [%s] %s := 間接呼び出し @0x%llx (関数ポインタ先は静的に未解決)\n",
                    depflow_origin_name(DEPFLOW_UNKNOWN), base,
                    (unsigned long long)s->addr);
            return;

        case NR_LOAD: {
            /* src1が指すアドレスが__DATA等の既知グローバルなら xref で
             * 書き込み元を列挙。アドレスが定数化できない場合はヒープ別名
             * 不明としてUNKNOWN。has_immかつ非ゼロimmをグローバル候補とする。 */
            if (s->has_imm && s->imm != 0) {
                uint64_t froms[16];
                int n = depflow_find_global_writers(w->bin, s->imm, froms, 16);
                fprintf(w->out, "<- [%s] %s := *0x%llx のロード @0x%llx",
                        depflow_origin_name(DEPFLOW_GLOBAL), base,
                        (unsigned long long)s->imm, (unsigned long long)s->addr);
                if (n > 0) {
                    int k;
                    fprintf(w->out, "  (書き込み元: ");
                    for (k = 0; k < n; k++)
                        fprintf(w->out, "%s0x%llx", k ? ", " : "", (unsigned long long)froms[k]);
                    fprintf(w->out, ")\n");
                } else {
                    fprintf(w->out, "  (xrefから書き込み元が見つからず; 初期化データの可能性)\n");
                }
            } else {
                fprintf(w->out, "<- [%s] %s := ヒープ/スタック経由ロード @0x%llx (別名不明のため打ち切り)\n",
                        depflow_origin_name(DEPFLOW_UNKNOWN), base,
                        (unsigned long long)s->addr);
            }
            return;
        }

        case NR_PHI:
            fprintf(w->out, "<- [%s] %s は分岐合流点のPHI (経路ごとに両辺を辿る)\n",
                    depflow_origin_name(DEPFLOW_LOCAL_DEF), base);
            if (s->src1 >= 0) depflow_walk_var(w, func_idx, ctx, s->src1, depth + 1, visited_funcs, nvisited);
            if (s->src2 >= 0) depflow_walk_var(w, func_idx, ctx, s->src2, depth + 1, visited_funcs, nvisited);
            return;

        default:
            /* 算術/論理/代入など: src1,src2,src3を辿る。即値のみならCONST。 */
            if (s->src1 < 0 && s->src2 < 0 && s->src3 < 0) {
                fprintf(w->out, "<- [%s] %s := imm 0x%llx @0x%llx\n",
                        depflow_origin_name(DEPFLOW_CONST), base,
                        (unsigned long long)s->imm, (unsigned long long)s->addr);
                return;
            }
            fprintf(w->out, "<- [%s] %s := %s演算 @0x%llx\n",
                    depflow_origin_name(DEPFLOW_LOCAL_DEF), base,
                    base, (unsigned long long)s->addr);
            if (s->src1 >= 0) depflow_walk_var(w, func_idx, ctx, s->src1, depth + 1, visited_funcs, nvisited);
            if (s->src2 >= 0) depflow_walk_var(w, func_idx, ctx, s->src2, depth + 1, visited_funcs, nvisited);
            if (s->src3 >= 0) depflow_walk_var(w, func_idx, ctx, s->src3, depth + 1, visited_funcs, nvisited);
            return;
        }
    }

    /* 関数内にdefが見つからない = 呼び出し元から渡された引数とみなす */
    depflow_indent(w->out, depth);
    fprintf(w->out, "<- [%s] %s (関数内にdefなし: 呼び出し元の引数と推定)\n",
            depflow_origin_name(DEPFLOW_PARAM), base);
}

/* ═══════════════════════════════════════════════════════════════════
 * §6  並列/順序辺の列挙 (全関数走査)
 * ═══════════════════════════════════════════════════════════════════ */

static void depflow_print_parallel_edges(dax_binary_t *bin, depflow_cache_t *cache, FILE *out) {
    int fi, i;
    fprintf(out, "\n═══ 並列/順序 API 検出 ═══\n");
    for (fi = 0; fi < bin->nfunctions && fi < DEPFLOW_MAX_FUNCS_CACHE; fi++) {
        nr_ctx_t *ctx = depflow_get_ctx(bin, cache, fi);
        if (!ctx) continue;
        for (i = 0; i < ctx->nstmts; i++) {
            nr_stmt_t *s = &ctx->stmts[i];
            depflow_par_kind_t k;
            if (s->op != NR_CALL && s->op != NR_INDIRECT_CALL) continue;
            k = depflow_classify_call(s->callee_name);
            if (k == PAR_SPAWN) {
                fprintf(out, "  func[%d] @0x%llx: 起動辺 -> %s (別経路で並列に走り得る)\n",
                        fi, (unsigned long long)s->addr, s->callee_name);
            } else if (k == PAR_JOIN) {
                fprintf(out, "  func[%d] @0x%llx: 合流辺 -> %s (ここまでの順序を強制)\n",
                        fi, (unsigned long long)s->addr, s->callee_name);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * §7  公開API
 * ═══════════════════════════════════════════════════════════════════ */

int dax_depflow_build(dax_binary_t *bin, dax_opts_t *opts) {
    int si;
    DAX_GUARD_BIN_RET(bin, -1);
    (void)opts;

    /* depflowはNR(def-use)を関数単位で読むため、bin->functionsが
     * 未構築だと何も辿れずに空振りする。--depflowだけ指定して
     * -f/-C/-Lを付け忘れても動くよう、ここで自前で関数検出をかける。
     * main.cのcfg/loopsパスと同じ手順(セクションごとにガードしてから
     * dax_func_detectを呼ぶ)をなぞっている。既にbin->nfunctions>0なら
     * (別のパスが先に検出済みなら)二重に検出し直さない。 */
    if (bin->nfunctions == 0) {
        for (si = 0; si < bin->nsections && si < DAX_MAX_SECTIONS; si++) {
            dax_section_t *sec = &bin->sections[si];
            if (sec->type != SEC_TYPE_CODE) continue;
            if (sec->size == 0) continue;
            if (sec->offset > bin->size) continue;
            if (sec->size > bin->size - sec->offset) continue; /* overflow防止 */
            dax_func_detect(bin, bin->data + sec->offset,
                             (size_t)sec->size, sec->vaddr, sec);
        }
        dax_clamp_counts(bin);
    }

    if (!bin->xrefs) dax_xref_build(bin);
    return 0;
}

int dax_depflow_trace(dax_binary_t *bin, dax_opts_t *opts,
                       int func_idx, const char *var_base, int version,
                       int max_depth, FILE *out) {
    depflow_cache_t cache;
    depflow_walk_t w;
    nr_ctx_t *ctx;
    int i, target_var = -1;
    int visited[64];

    DAX_GUARD_BIN_RET(bin, -1);
    if (!dax_func_idx_ok(bin, func_idx) || !out) return -1;
    memset(&cache, 0, sizeof(cache));

    ctx = depflow_get_ctx(bin, &cache, func_idx);
    if (!ctx) { depflow_free_cache(&cache); return -1; }

    if (var_base && var_base[0]) {
        for (i = 0; i < ctx->nvars; i++) {
            if (strcmp(ctx->vars[i].base, var_base) == 0 &&
                (version < 0 || ctx->vars[i].version == version)) {
                target_var = i; /* 複数版あれば最後に見つかったもの=最新版 */
            }
        }
    } else {
        /* 指定なし: 最後のNR_RETのsrc1を出力点とする */
        for (i = ctx->nstmts - 1; i >= 0; i--) {
            if (ctx->stmts[i].op == NR_RET && ctx->stmts[i].src1 >= 0) {
                target_var = ctx->stmts[i].src1;
                break;
            }
        }
    }

    if (target_var < 0) {
        fprintf(out, "追跡対象の変数が見つかりません (func[%d], base=\"%s\")\n",
                func_idx, var_base ? var_base : "(ret)");
        depflow_free_cache(&cache);
        return -1;
    }

    w.bin = bin; w.cache = &cache; w.opts = opts; w.out = out;
    w.max_depth = max_depth > 0 ? max_depth : 32;

    fprintf(out, "═══ 静的依存トレース: func[%d] var=%s (深さ上限%d) ═══\n",
            func_idx, ctx->vars[target_var].base, w.max_depth);
    visited[0] = func_idx;
    depflow_walk_var(&w, func_idx, ctx, target_var, 0, visited, 1);

    depflow_free_cache(&cache);
    return 0;
}

int dax_depflow_print_all(dax_binary_t *bin, dax_opts_t *opts, FILE *out) {
    depflow_cache_t cache;
    int fi;
    DAX_GUARD_BIN_RET(bin, -1);
    if (!out) return -1;
    memset(&cache, 0, sizeof(cache));

    fprintf(out, "\n══════════════ depflow — 静的依存グラフ ══════════════\n");

    /* opts->depflow_target_func が指定されていれば、その関数のretから
     * のみトレースする。未指定なら呼ばれている形跡の無い(= xrefのcall
     * 対象になっていない)関数を「出力候補(エントリに近い/末端)」とみなし、
     * 全部についてretトレースを出す。 */
    if (opts && opts->depflow_target_func[0]) {
        int fi2;
        for (fi2 = 0; fi2 < bin->nfunctions; fi2++) {
            if (strcmp(bin->functions[fi2].name, opts->depflow_target_func) == 0) {
                int depth = opts->depflow_depth > 0 ? opts->depflow_depth : 32;
                dax_depflow_trace(bin, opts, fi2,
                                   opts->depflow_target_var[0] ? opts->depflow_target_var : NULL,
                                   -1, depth, out);
                break;
            }
        }
    } else {
        for (fi = 0; fi < bin->nfunctions && fi < DEPFLOW_MAX_FUNCS_CACHE; fi++) {
            int depth = opts && opts->depflow_depth > 0 ? opts->depflow_depth : 16;
            dax_depflow_trace(bin, opts, fi, NULL, -1, depth, out);
        }
    }

    depflow_print_parallel_edges(bin, &cache, out);
    depflow_free_cache(&cache);
    return 0;
}

int dax_depflow_write_dot(dax_binary_t *bin, dax_opts_t *opts, const char *path) {
    depflow_cache_t cache;
    FILE *f;
    int fi, i;
    DAX_GUARD_BIN_RET(bin, -1);
    if (!path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    memset(&cache, 0, sizeof(cache));

    fprintf(f, "digraph depflow {\n  rankdir=RL;\n  node [shape=box,fontsize=10];\n");
    for (fi = 0; fi < bin->nfunctions && fi < DEPFLOW_MAX_FUNCS_CACHE; fi++) {
        nr_ctx_t *ctx = depflow_get_ctx(bin, &cache, fi);
        if (!ctx) continue;
        for (i = 0; i < ctx->nstmts; i++) {
            nr_stmt_t *s = &ctx->stmts[i];
            if (s->op != NR_CALL || s->callee_func_idx < 0) continue;
            fprintf(f, "  \"f%d\" -> \"f%d\" [label=\"0x%llx\"];\n",
                    fi, s->callee_func_idx, (unsigned long long)s->addr);
        }
        if (opts) {
            for (i = 0; i < ctx->nstmts; i++) {
                nr_stmt_t *s = &ctx->stmts[i];
                depflow_par_kind_t k;
                if (s->op != NR_CALL && s->op != NR_INDIRECT_CALL) continue;
                k = depflow_classify_call(s->callee_name);
                if (k == PAR_SPAWN)
                    fprintf(f, "  \"f%d\" -> \"%s\" [style=dashed,color=blue,label=\"spawn\"];\n",
                            fi, s->callee_name);
                else if (k == PAR_JOIN)
                    fprintf(f, "  \"f%d\" -> \"%s\" [style=dotted,color=red,label=\"join\"];\n",
                            fi, s->callee_name);
            }
        }
    }
    fprintf(f, "}\n");
    fclose(f);
    depflow_free_cache(&cache);
    return 0;
}
