/*
 * depflow.c â éçä¾å­ã°ã©ãè¿½è·¡ (Static Dependency Flow)
 *
 * ç®ç
 * âââââ
 *   ãã¤ããªå¨ä½ããåãã³ã¼ããæ¯åå¤ãããªããåæã§éçã«èª­ã¿ã
 *   ããåºåç¹ï¼ä¾: å°æ£AIã®æçµçæãè¿ãretï¼ãä½¿ã£ã¦ããå¤æ°ã
 *   æéåã ãéåãã«è¾¿ã£ã¦ãããããã®åºã©ããã
 *     å®æ° / å¼ã³åºãåããåãåã£ãå¼æ° / ã°ã­ã¼ãã«é åã¸ã®æ¸ãè¾¼ã¿
 *     (xrefã§åæ) / å¤é¨é¢æ°ã®æ»ãå¤ / ãã¼ãä¸ã®å¤(å¥åä¸æã§æã¡åã)
 *   ã®ã©ããã«åé¡ãããå ãã¦ãé¢æ°ã®å®è¡é ã
 *     - å¼ã³åºãã°ã©ãä¸ã®ä¾å­ã§ä¸æã«æ±ºã¾ãé åºã
 *     - pthread_create / dispatch_async ç­ã§èµ·åãããä¸¦åå®è¡ã
 *   ã®ã©ã¡ãããå¤å®ããä¸¦åèµ·åç¹ã«ã¯èµ·åè¾ºãå¾ã¡åãã
 *   ï¼pthread_join / dispatch_group_wait ç­ï¼ã«ã¯åæµè¾ºãå¼ãã
 *
 * ææ
 * âââââ
 *   åé¢æ°ã®NR (nr_ctx_t, decomp.c ã® nr_lift_func ã dax_nr_lift_func_raw
 *   çµç±ã§åå¾) ãæã¤ def-use (dst/src1/src2/src3) ã¨ãNR_CALL ã®
 *   callee_func_idx ãããã®ãã¹ã®ä¸æ¬¡ãã¼ã¿ãé¢æ°ãã¾ããã è¾ºã¯
 *   callgraph.c ã¨åããcallå½ä»¤ã®addrããcalleeã¸ãã®ã¨ãã¸ãä½¿ãã
 *
 * éçï¼éçã«æ±ºã¾ããªããã®ãé ããã«ãä¸æãã¨ãã¦æ®ãï¼
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 *   - é¢æ°ãã¤ã³ã¿çµç±ã®éæ¥å¼ã³åºãï¼NR_INDIRECT_CALLï¼
 *   - objc_msgSend ç­ã®åçãã£ã¹ããã
 *   - ãã¼ãä¸ã®å¤ã®å¥åï¼loadåãéçã«ä¸æå®ã¾ããªãå ´åï¼
 *   ãããã¯ DEPFLOW_UNKNOWN ã¨ãã¦åºåã«æ®ããé»ã£ã¦æ¶ããªãã
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dax.h"
#include "dax_guard.h"
#include "nr_types.h"

/* âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 * Â§1  ä¾å­è¾ºã®ç¨®å¥ã¨ãåºã©ãããã®åé¡
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

typedef enum {
    DEPFLOW_CONST = 0,     /* å³å¤ */
    DEPFLOW_PARAM,         /* å¼ã³åºãåããæ¸¡ãããå¼æ° (x0-x7ç¸å½) */
    DEPFLOW_GLOBAL,        /* __DATAç­ã¸ã®æ¸ãè¾¼ã¿å (xrefã§ç¹å®) */
    DEPFLOW_EXTCALL,       /* å¤é¨(æªå®ç¾©)ã·ã³ãã«ã®æ»ãå¤ */
    DEPFLOW_LOCAL_DEF,     /* åä¸é¢æ°åã®åè¡def (ããã«è¾¿ãã) */
    DEPFLOW_CALLEE_RET,    /* æ¢ç¥ã®åé¨é¢æ°ã®æ»ãå¤ (ããã«è¾¿ãã) */
    DEPFLOW_RECURSION_CUT, /* ã³ã¼ã«ã°ã©ãSCC: ä¸éæ·±ãã§æã¡åã */
    DEPFLOW_UNKNOWN        /* éæ¥å¼ã³åºã/åçãã£ã¹ããã/å¥åä¸æ */
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

/* âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 * Â§2  ä¸¦å/é åº API ã®åé¡
 *
 *   NeoDAXã®ã·ã³ãã«è§£æ±º(dax_sym_name)ãéãååã ããå¯¾è±¡ã«ããã
 *   __stubs/la_symbol_ptrçµç±ã®æªå®ç¾©ã·ã³ãã«ã¯ macho.c ã
 *   SEC_TYPE_PLT ã¨ãã¦åºå¥ãã¦ããããindirect symbol table ãã
 *   å®åãå¼ãå¦çãåå®è£ã«ç¡ããããã¾ãã·ã³ãã«ãã¼ãã«ã«ä¹ã£ã¦
 *   ããååï¼åçãªã³ã¯ã§ãªãéçã«è§£æ±ºã§ããç¯å²ï¼ã§å¤å®ããã
 *   indirect symbolè§£æ±ºãè¶³ãå ´åã¯macho.cã®æ¡å¼µãå¥éè¦ãã
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

typedef enum {
    PAR_NONE = 0,
    PAR_SPAWN,   /* pthread_create, dispatch_async, dispatch_async_f,
                    NSThread detachNewThreadSelector ç­: èµ·åè¾º */
    PAR_JOIN     /* pthread_join, dispatch_group_wait, dispatch_semaphore_wait,
                    pthread_mutex_lock ç­: åæµè¾º(é åºãå¼·å¶) */
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

/* âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 * Â§3  é¢æ°åä½ã®NRã­ã£ãã·ã¥
 *
 *   ãã¤ããªå¨ä½ãè¾¿ãéãåãé¢æ°ã®NRãä½åº¦ãä½ãç´ããªããã
 *   ã­ã¼ããã¦ä½¿ãåããnr_ctx_tã¯ãµã¤ãºãå¤§ãã(NR_MAX_STMTS=3072)ãã
 *   ãã¼ãç¢ºä¿ã§ãä½¿ãçµãã£ããdax_depflow_free_cacheã§ã¾ã¨ãã¦è§£æ¾ã
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

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

/* âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 * Â§4  xrefããã°ã­ã¼ãã«æ¸ãè¾¼ã¿åãæ¢ã
 *
 *   dax_xref_build()ãä½ãxreféåãããaddrå®ã¦ã®callä»¥å¤ã®xref
 *   (ã¤ã¾ãå®è¡ã¢ãã¬ã¹ã§ã¯ãªããã¼ã¿ã¢ãã¬ã¹ã¸ã®åç§ãã¾ãã¯STOREæã«
 *   ä½¿ãããADRP/ADD/LDRãæã__DATAé å)ãæ¢ãã
 *   NRã®addr_hintã¯NRå´ãbaseåããæããªããããããã§ã¯
 *   ããã®ã°ã­ã¼ãã«ã¢ãã¬ã¹ãã¿ã¼ã²ããã¨ããxrefã®ãã¡is_call==0ãã
 *   åæããã¨ããã¾ã§ã«çãã(å³å¯ãªã¹ãã¢åå½ä»¤ã®ç¹å®ã¯NR_STOREã®
 *   src1ã¢ãã¬ã¹æ³¨éãããã°ãããåªå)ã
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

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

/* âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 * Â§5  åä¸NRå¤æ°ã®éåãåé¡ (åå¸°ãæ·±ãå¶éã¤ã)
 *
 *   var_idxã¯ctx->vars[]ã®ã¤ã³ããã¯ã¹ãåä¸é¢æ°åã§ããã°ã
 *   nr_stmt_tãæ«å°¾ããèµ°æ»ãã¦ãdst==var_idxã®æå¾ã®stmtãã
 *   ãã®defã¨ã¿ãªãï¼NRã¯åºç¾é ã«SSAçãåã£ã¦ãããããæ«å°¾ææ°ã®
 *   defãæ¾ãã° version_map ã¨æ´åããï¼ã
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

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
static void depflow_walk_def(depflow_walk_t *w, int func_idx, nr_ctx_t *ctx,
                              nr_stmt_t *s, int depth, int *visited_funcs, int nvisited);
static void depflow_walk_param_sites(depflow_walk_t *w, int func_idx,
                                      const char *base, int depth,
                                      int *visited_funcs, int nvisited);

/* callee_func_idxå´ã®ãæ»ãå¤ããè¾¿ã: calleeã®NR_RETã®src1ãä½¿ãã
 * calleeã«è¤æ°ã®retãããå ´åã¯æåã«è¦ã¤ãã£ããã®ã ããè¾¿ã
 * (å°æ£AIç­ã®åä¸returnãã¿ã¼ã³ãæ³å®; è¤æ°returnãããé¢æ°ã¯
 *  DEPFLOW_UNKNOWNå¯ãã«ãªãããããèªä½ãæ¬ è½ã¨ãã¦åºåã«æ®ã)ã */
static void depflow_walk_callee_return(depflow_walk_t *w, int callee_idx,
                                        int depth, int *visited_funcs, int nvisited) {
    nr_ctx_t *cctx;
    int i;
    /* åå¸°/ç¸äºåå¸°ã®æ¤åº: ãã§ã«è¨ªåæ¸ã¿ã®é¢æ°ãªãæã¡åã */
    for (i = 0; i < nvisited; i++) {
        if (visited_funcs[i] == callee_idx) {
            depflow_indent(w->out, depth);
            fprintf(w->out, "<- [%s] func[%d] (åå¸°: æ¢è¨ªåãæ·±ã%dã§æã¡åã)\n",
                    depflow_origin_name(DEPFLOW_RECURSION_CUT), callee_idx, depth);
            return;
        }
    }
    cctx = depflow_get_ctx(w->bin, w->cache, callee_idx);
    if (!cctx) {
        depflow_indent(w->out, depth);
        fprintf(w->out, "<- [%s] func[%d] (NRæ§ç¯å¤±æ)\n",
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
            fprintf(w->out, "<- [%s] func[%d] ã®æ»ãå¤ (retæã®src1ãè¿½è·¡)\n",
                    depflow_origin_name(DEPFLOW_CALLEE_RET), callee_idx);
            depflow_walk_var(w, callee_idx, cctx, cctx->stmts[i].src1,
                              depth + 1, new_visited, nv);
            return;
        }
    }
    depflow_indent(w->out, depth);
    fprintf(w->out, "<- [%s] func[%d] (retæãè¦ã¤ãããªã/å¤ãè¿ããªã)\n",
            depflow_origin_name(DEPFLOW_UNKNOWN), callee_idx);
}

static void depflow_walk_var(depflow_walk_t *w, int func_idx, nr_ctx_t *ctx,
                              int var_idx, int depth, int *visited_funcs, int nvisited) {
    int i;
    const char *base;

    if (depth > w->max_depth) {
        depflow_indent(w->out, depth);
        fprintf(w->out, "<- [%s] æ·±ãä¸é%dã«å°éãæã¡åã\n",
                depflow_origin_name(DEPFLOW_RECURSION_CUT), w->max_depth);
        return;
    }
    if (var_idx < 0 || var_idx >= ctx->nvars) {
        depflow_indent(w->out, depth);
        fprintf(w->out, "<- [%s] var_idxä¸æ­£(å®æ°å±éæ¸ã¿ã®å¯è½æ§)\n",
                depflow_origin_name(DEPFLOW_CONST));
        return;
    }
    base = ctx->vars[var_idx].base;

    /* å¼æ°ã¬ã¸ã¹ã¿(x0-x7 / w0-w7 / edi,esi...)ã§ããã¤åä¸é¢æ°åã«
     * ãã®å¤æ°ã¸ã®defãä¸ã¤ãç¡ããã°ãå¼ã³åºãåããã®å¼æ°ãã¨ã¿ãªãã
     * å¤å®ã¯ãversion==0 ãã¤å®ç¾©stmtãè¦å½ãããªããã§è¡ãã */

    for (i = ctx->nstmts - 1; i >= 0; i--) {
        if (ctx->stmts[i].dst == var_idx) {
            depflow_walk_def(w, func_idx, ctx, &ctx->stmts[i], depth,
                             visited_funcs, nvisited);
            return;
        }
    }

    /* 関数内にdefが見つからない = 呼び出し元から渡された引数。
     * call-siteを列挙して、呼び出し元側でのその引数のdefをさらに辿る。 */
    depflow_indent(w->out, depth);
    fprintf(w->out, "<- [%s] %s (関数内にdefなし: 呼び出し元の引数)\n",
            depflow_origin_name(DEPFLOW_PARAM), base);
    depflow_walk_param_sites(w, func_idx, base, depth,
                             visited_funcs, nvisited);
}

/* depflow_walk_param_sites — PARAMに到達した際、この関数を呼んでいる
 * call-site(xref)を全て列挙し、各呼び出し元での引数レジスタ(x0../rdi..)
 * のdefを辿る。これで「変数がどこから来ているか」が呼び出し元を
 * またいで一本に繋がる。訪問済み関数と深さ上限で有限回に保つ。 */

static int depflow_addr_to_func(dax_binary_t *bin, uint64_t addr) {
    int i;
    if (!bin || !bin->functions || bin->nfunctions <= 0) return -1;
    for (i = 0; i < bin->nfunctions; i++) {
        if (bin->functions[i].start == addr) return i;
        if (addr > bin->functions[i].start && bin->functions[i].end > 0
            && addr < bin->functions[i].end) return i;
    }
    return -1;
}

static void depflow_walk_param_sites(depflow_walk_t *w, int func_idx,
                                      const char *base, int depth,
                                      int *visited_funcs, int nvisited) {
    dax_binary_t *bin = w->bin;
    dax_func_t   *fn;
    int xi, found = 0;

    if (func_idx < 0 || func_idx >= bin->nfunctions) return;
    if (depth + 2 > w->max_depth) return;
    fn = &bin->functions[func_idx];
    if (!bin->xrefs) return;

    for (xi = 0; xi < bin->nxrefs && xi < DAX_MAX_XREFS; xi++) {
        dax_xref_t *xr = &bin->xrefs[xi];
        nr_ctx_t *cctx;
        int caller_fi, vi, di, nv;
        int new_visited[64];

        if (!xr->is_call) continue;
        if (fn->end > 0) {
            if (xr->to < fn->start || xr->to >= fn->end) continue;
        } else {
            if (xr->to != fn->start) continue;
        }

        caller_fi = depflow_addr_to_func(bin, xr->from);
        if (caller_fi < 0 || caller_fi == func_idx) continue;
        cctx = depflow_get_ctx(bin, w->cache, caller_fi);
        if (!cctx) continue;

        /* callerのNRから同一base名の変数(引数レジスタ)を探す。
         * 複数版(SSA)あれば最後の版=call-site直前の状態に最も近い。 */
        vi = -1;
        for (di = 0; di < cctx->nvars; di++)
            if (strcmp(cctx->vars[di].base, base) == 0) vi = di;
        if (vi < 0) continue;

        /* call-siteのアドレス以前にある最後のdefをその引数の値とみなす */
        {
            int def_i = -1;
            for (di = cctx->nstmts - 1; di >= 0; di--) {
                if (cctx->stmts[di].dst != vi) continue;
                if (cctx->stmts[di].addr > xr->from) continue;
                def_i = di;
                break;
            }
            found++;
            nv = nvisited < 63 ? nvisited : 63;
            memcpy(new_visited, visited_funcs, sizeof(int) * nv);
            new_visited[nv++] = caller_fi;

            depflow_indent(w->out, depth + 1);
            if (def_i < 0) {
                fprintf(w->out, "<- 呼び出し元 func[%d] @0x%llx: %s はさらに上位から渡された引数\n",
                        caller_fi, (unsigned long long)xr->from, base);
                depflow_walk_param_sites(w, caller_fi, base, depth + 1,
                                         new_visited, nv);
            } else {
                fprintf(w->out, "<- 呼び出し元 func[%d] のcall-site @0x%llx での %s:\n",
                        caller_fi, (unsigned long long)xr->from, base);
                depflow_walk_def(w, caller_fi, cctx, &cctx->stmts[def_i],
                                 depth + 2, new_visited, nv);
            }
        }
    }
    if (!found) {
        depflow_indent(w->out, depth + 1);
        fprintf(w->out, "<- call-siteが見つからず (エントリポイント由来/間接呼び出しの可能性)\n");
    }
}

/* 与えられたdefステートメント s (s->dstが定義先) の値の出どころを辿る。
 * walk_varの「末尾def探索」から分離した本体。param継続では
 * 「呼び出し元のcall-site直前の引数def」から直接こちらに入る。 */
static void depflow_walk_def(depflow_walk_t *w, int func_idx, nr_ctx_t *ctx,
                              nr_stmt_t *s, int depth, int *visited_funcs, int nvisited) {
    const char *base = "?";

    if (s->dst >= 0 && s->dst < ctx->nvars) base = ctx->vars[s->dst].base;

    depflow_indent(w->out, depth);

    switch (s->op) {
        case NR_CALL:
            if (s->callee_func_idx >= 0) {
                fprintf(w->out, "<- [%s] %s := func[%d](%s) ã®å¼ã³åºã @0x%llx\n",
                        depflow_origin_name(DEPFLOW_CALLEE_RET), base,
                        s->callee_func_idx, s->callee_name,
                        (unsigned long long)s->addr);
                depflow_walk_callee_return(w, s->callee_func_idx, depth + 1,
                                            visited_funcs, nvisited);
            } else {
                /* callee_func_idxä¸æ = æªå®ç¾©ã·ã³ãã«(å¤é¨é¢æ°)ã®æ»ãå¤ */
                fprintf(w->out, "<- [%s] %s := %s() ã®æ»ãå¤ @0x%llx (å¤é¨/æªè§£æ±ºã·ã³ãã«)\n",
                        depflow_origin_name(DEPFLOW_EXTCALL), base,
                        s->callee_name[0] ? s->callee_name : "?",
                        (unsigned long long)s->addr);
            }
            return;

        case NR_INDIRECT_CALL:
            fprintf(w->out, "<- [%s] %s := éæ¥å¼ã³åºã @0x%llx (é¢æ°ãã¤ã³ã¿åã¯éçã«æªè§£æ±º)\n",
                    depflow_origin_name(DEPFLOW_UNKNOWN), base,
                    (unsigned long long)s->addr);
            return;

        case NR_LOAD: {
            /* src1ãæãã¢ãã¬ã¹ã__DATAç­ã®æ¢ç¥ã°ã­ã¼ãã«ãªã xref ã§
             * æ¸ãè¾¼ã¿åãåæãã¢ãã¬ã¹ãå®æ°åã§ããªãå ´åã¯ãã¼ãå¥å
             * ä¸æã¨ãã¦UNKNOWNãhas_immãã¤éã¼ã­immãã°ã­ã¼ãã«åè£ã¨ããã */
            if (s->has_imm && s->imm != 0) {
                uint64_t froms[16];
                int n = depflow_find_global_writers(w->bin, s->imm, froms, 16);
                fprintf(w->out, "<- [%s] %s := *0x%llx ã®ã­ã¼ã @0x%llx",
                        depflow_origin_name(DEPFLOW_GLOBAL), base,
                        (unsigned long long)s->imm, (unsigned long long)s->addr);
                if (n > 0) {
                    int k;
                    fprintf(w->out, "  (æ¸ãè¾¼ã¿å: ");
                    for (k = 0; k < n; k++)
                        fprintf(w->out, "%s0x%llx", k ? ", " : "", (unsigned long long)froms[k]);
                    fprintf(w->out, ")\n");
                } else {
                    fprintf(w->out, "  (xrefããæ¸ãè¾¼ã¿åãè¦ã¤ããã; åæåãã¼ã¿ã®å¯è½æ§)\n");
                }
            } else {
                fprintf(w->out, "<- [%s] %s := ãã¼ã/ã¹ã¿ãã¯çµç±ã­ã¼ã @0x%llx (å¥åä¸æã®ããæã¡åã)\n",
                        depflow_origin_name(DEPFLOW_UNKNOWN), base,
                        (unsigned long long)s->addr);
            }
            return;
        }

        case NR_PHI:
            fprintf(w->out, "<- [%s] %s ã¯åå²åæµç¹ã®PHI (çµè·¯ãã¨ã«ä¸¡è¾ºãè¾¿ã)\n",
                    depflow_origin_name(DEPFLOW_LOCAL_DEF), base);
            if (s->src1 >= 0) depflow_walk_var(w, func_idx, ctx, s->src1, depth + 1, visited_funcs, nvisited);
            if (s->src2 >= 0) depflow_walk_var(w, func_idx, ctx, s->src2, depth + 1, visited_funcs, nvisited);
            return;

        default:
            /* ç®è¡/è«ç/ä»£å¥ãªã©: src1,src2,src3ãè¾¿ããå³å¤ã®ã¿ãªãCONSTã */
            if (s->src1 < 0 && s->src2 < 0 && s->src3 < 0) {
                fprintf(w->out, "<- [%s] %s := imm 0x%llx @0x%llx\n",
                        depflow_origin_name(DEPFLOW_CONST), base,
                        (unsigned long long)s->imm, (unsigned long long)s->addr);
                return;
            }
            fprintf(w->out, "<- [%s] %s := %sæ¼ç® @0x%llx\n",
                    depflow_origin_name(DEPFLOW_LOCAL_DEF), base,
                    base, (unsigned long long)s->addr);
            if (s->src1 >= 0) depflow_walk_var(w, func_idx, ctx, s->src1, depth + 1, visited_funcs, nvisited);
            if (s->src2 >= 0) depflow_walk_var(w, func_idx, ctx, s->src2, depth + 1, visited_funcs, nvisited);
            if (s->src3 >= 0) depflow_walk_var(w, func_idx, ctx, s->src3, depth + 1, visited_funcs, nvisited);
            return;
        }
    (void)func_idx;
}


/* âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 * Â§6  ä¸¦å/é åºè¾ºã®åæ (å¨é¢æ°èµ°æ»)
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

static void depflow_print_parallel_edges(dax_binary_t *bin, depflow_cache_t *cache, FILE *out) {
    int fi, i;
    fprintf(out, "\nâââ ä¸¦å/é åº API æ¤åº âââ\n");
    for (fi = 0; fi < bin->nfunctions && fi < DEPFLOW_MAX_FUNCS_CACHE; fi++) {
        nr_ctx_t *ctx = depflow_get_ctx(bin, cache, fi);
        if (!ctx) continue;
        for (i = 0; i < ctx->nstmts; i++) {
            nr_stmt_t *s = &ctx->stmts[i];
            depflow_par_kind_t k;
            if (s->op != NR_CALL && s->op != NR_INDIRECT_CALL) continue;
            k = depflow_classify_call(s->callee_name);
            if (k == PAR_SPAWN) {
                fprintf(out, "  func[%d] @0x%llx: èµ·åè¾º -> %s (å¥çµè·¯ã§ä¸¦åã«èµ°ãå¾ã)\n",
                        fi, (unsigned long long)s->addr, s->callee_name);
            } else if (k == PAR_JOIN) {
                fprintf(out, "  func[%d] @0x%llx: åæµè¾º -> %s (ããã¾ã§ã®é åºãå¼·å¶)\n",
                        fi, (unsigned long long)s->addr, s->callee_name);
            }
        }
    }
}

/* âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
 * Â§7  å¬éAPI
 * âââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

int dax_depflow_build(dax_binary_t *bin, dax_opts_t *opts) {
    int si;
    DAX_GUARD_BIN_RET(bin, -1);
    (void)opts;

    /* depflowã¯NR(def-use)ãé¢æ°åä½ã§èª­ããããbin->functionsã
     * æªæ§ç¯ã ã¨ä½ãè¾¿ããã«ç©ºæ¯ãããã--depflowã ãæå®ãã¦
     * -f/-C/-Lãä»ãå¿ãã¦ãåããããããã§èªåã§é¢æ°æ¤åºããããã
     * main.cã®cfg/loopsãã¹ã¨åãæé (ã»ã¯ã·ã§ã³ãã¨ã«ã¬ã¼ããã¦ãã
     * dax_func_detectãå¼ã¶)ããªãã£ã¦ãããæ¢ã«bin->nfunctions>0ãªã
     * (å¥ã®ãã¹ãåã«æ¤åºæ¸ã¿ãªã)äºéã«æ¤åºãç´ããªãã */
    if (bin->nfunctions == 0) {
        for (si = 0; si < bin->nsections && si < DAX_MAX_SECTIONS; si++) {
            dax_section_t *sec = &bin->sections[si];
            if (sec->type != SEC_TYPE_CODE) continue;
            if (sec->size == 0) continue;
            if (sec->offset > bin->size) continue;
            if (sec->size > bin->size - sec->offset) continue; /* overflowé²æ­¢ */
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
                target_var = i; /* è¤æ°çããã°æå¾ã«è¦ã¤ãã£ããã®=ææ°ç */
            }
        }
    } else {
        /* æå®ãªã: æå¾ã®NR_RETã®src1ãåºåç¹ã¨ãã */
        for (i = ctx->nstmts - 1; i >= 0; i--) {
            if (ctx->stmts[i].op == NR_RET && ctx->stmts[i].src1 >= 0) {
                target_var = ctx->stmts[i].src1;
                break;
            }
        }
    }

    if (target_var < 0) {
        fprintf(out, "è¿½è·¡å¯¾è±¡ã®å¤æ°ãè¦ã¤ããã¾ãã (func[%d], base=\"%s\")\n",
                func_idx, var_base ? var_base : "(ret)");
        depflow_free_cache(&cache);
        return -1;
    }

    w.bin = bin; w.cache = &cache; w.opts = opts; w.out = out;
    w.max_depth = max_depth > 0 ? max_depth : 32;

    fprintf(out, "âââ éçä¾å­ãã¬ã¼ã¹: func[%d] var=%s (æ·±ãä¸é%d) âââ\n",
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

    fprintf(out, "\nââââââââââââââ depflow â éçä¾å­ã°ã©ã ââââââââââââââ\n");

    /* opts->depflow_target_func ãæå®ããã¦ããã°ããã®é¢æ°ã®retãã
     * ã®ã¿ãã¬ã¼ã¹ãããæªæå®ãªãå¼ã°ãã¦ããå½¢è·¡ã®ç¡ã(= xrefã®call
     * å¯¾è±¡ã«ãªã£ã¦ããªã)é¢æ°ããåºååè£(ã¨ã³ããªã«è¿ã/æ«ç«¯)ãã¨ã¿ãªãã
     * å¨é¨ã«ã¤ãã¦retãã¬ã¼ã¹ãåºãã */
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

