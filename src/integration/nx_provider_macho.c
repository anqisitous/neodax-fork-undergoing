/*
 * nx_provider_macho.c — 新静的解析フレームワーク (src/core, src/analysis, ...) を
 * nx ABI に適合させる provider。
 *
 * この翻訳単位だけが macho_static.h と各解析ヘッダを include する。
 * nx.h 側 (engine) はこれらを知らない。
 */

#include "nx.h"

#include <stdlib.h>
#include <string.h>

#include "../core/macho_static.h"
#include "../analysis/depflow.h"
#include "../analysis/variable_classifier.h"
#include "../flow/execution_order.h"
#include "../flow/parallel_detection.h"
#include "../tracking/state_tracking.h"

/* bin はポインタ保持: neodax_bridge が embed した macho_binary_t を borrow できる */
typedef struct {
    macho_binary_t *bin;
    int             owned;   /* 1 なら close 時に free する */
} macho_impl_t;

#define IMPL(b)  ((macho_impl_t *)((b)->impl))
#define BIN(b)   (IMPL(b)->bin)

/* ------------------------------------------------------------------ */
/* detection / open / close                                            */
/* ------------------------------------------------------------------ */

static int macho_detect(const uint8_t *data, size_t size) {
    if (!data || size < 4) return 0;
    uint32_t m;
    memcpy(&m, data, 4);
    return (m == MACHO_MAGIC_64 || m == MACHO_MAGIC_64_LE ||
            m == MACHO_MAGIC_32 || m == MACHO_MAGIC_32_LE ||
            m == MACHO_FAT_MAGIC || m == MACHO_FAT_MAGIC_LE);
}

static nx_binary_t *macho_open(const char *path, const uint8_t *data, size_t size) {
    macho_impl_t *impl = (macho_impl_t *)calloc(1, sizeof(*impl));
    if (!impl) return NULL;

    impl->bin = (macho_binary_t *)calloc(1, sizeof(macho_binary_t));
    if (!impl->bin) { free(impl); return NULL; }
    impl->owned = 1;

    macho_init(impl->bin);

    /* 契約: caller が data/size を渡し、macho_free が data を free する */
    impl->bin->data = (uint8_t *)data;
    impl->bin->size = size;
    if (path) strncpy(impl->bin->filepath, path, sizeof(impl->bin->filepath) - 1);

    if (macho_parse(impl->bin, data, size) != 0) {
        impl->bin->data = NULL;   /* data は呼び出し側で free させる */
        macho_free(impl->bin);
        free(impl->bin);
        free(impl);
        return NULL;
    }

    nx_binary_t *b = (nx_binary_t *)calloc(1, sizeof(*b));
    if (!b) {
        macho_free(impl->bin);   /* data もここで free される */
        free(impl->bin);
        free(impl);
        return NULL;
    }
    b->impl = impl;
    b->ops  = nx_provider_macho();   /* nx_engine_new()/nx_provider_name() need this */
    return b;
}

static void macho_close(nx_binary_t *b) {
    if (!b) return;
    macho_impl_t *impl = IMPL(b);
    if (impl) {
        if (impl->owned && impl->bin) {
            macho_free(impl->bin);
            free(impl->bin);
        }
        free(impl);
    }
    free(b);
}

/* neodax_bridge 用: 既存の macho_binary_t を borrow して nx ハンドルにする */
nx_binary_t *nx_macho_wrap(macho_binary_t *bin) {
    if (!bin) return NULL;
    macho_impl_t *impl = (macho_impl_t *)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
    impl->bin = bin;
    impl->owned = 0;
    nx_binary_t *b = (nx_binary_t *)calloc(1, sizeof(*b));
    if (!b) { free(impl); return NULL; }
    b->impl = impl;
    b->ops = nx_provider_macho();
    return b;
}

/* ------------------------------------------------------------------ */
/* accessors                                                           */
/* ------------------------------------------------------------------ */

static nx_arch_t macho_arch(nx_binary_t *b) {
    switch (BIN(b)->arch) {
        case MACHO_ARCH_X86_64:  return NX_ARCH_X86_64;
        case MACHO_ARCH_ARM64:   return NX_ARCH_ARM64;
        case MACHO_ARCH_RISCV64: return NX_ARCH_RISCV64;
        case MACHO_ARCH_X86_32:  return NX_ARCH_X86_32;
        default:                 return NX_ARCH_UNKNOWN;
    }
}

static uint64_t macho_entry(nx_binary_t *b) { return BIN(b)->entry; }
static int      macho_nsections(nx_binary_t *b) { return BIN(b)->nsections; }

static int macho_section(nx_binary_t *b, int i, nx_section_t *out) {
    macho_binary_t *bin = BIN(b);
    if (i < 0 || i >= bin->nsections) return NX_ERR;
    macho_section_t *s = &bin->sections[i];
    memset(out, 0, sizeof(*out));
    strncpy(out->name, s->name, sizeof(out->name) - 1);
    strncpy(out->seg, s->segname, sizeof(out->seg) - 1);
    out->vaddr = s->vaddr;
    out->size = s->size;
    out->file_off = s->file_offset;
    out->flags = s->flags;
    out->exec = s->executable;
    out->writable = s->writable;
    out->readable = s->readable;
    return NX_OK;
}

static int macho_nfuncs(nx_binary_t *b) { return BIN(b)->nfunctions; }

static int macho_func(nx_binary_t *b, int i, nx_func_t *out) {
    macho_binary_t *bin = BIN(b);
    if (i < 0 || i >= bin->nfunctions) return NX_ERR;
    macho_function_t *f = &bin->functions[i];
    memset(out, 0, sizeof(*out));
    out->start = f->start;
    out->end = f->end;
    strncpy(out->name, f->name, sizeof(out->name) - 1);
    out->insn_count = f->insn_count;
    out->block_count = f->block_count;
    out->frame_size = f->stack_frame_size;
    out->arg_count = f->arg_count;
    out->has_loops = f->has_loops;
    out->has_calls = f->has_calls;
    out->has_indirect = f->has_indirect_calls;
    return NX_OK;
}

static int macho_cfg(nx_binary_t *b) { return macho_build_cfg(BIN(b)); }
static int macho_augment(nx_binary_t *b, uint32_t want) {
    (void)want;
    return macho_analyze(BIN(b), NULL);
}

/* ------------------------------------------------------------------ */
/* analysis passes (engine が直列/並列を決める)                        */
/* ------------------------------------------------------------------ */

#define MACHO_BIN(e)  BIN((e)->bin)
#define DEPS(e)       ((depflow_graph_t *)((e)->art[NX_SLOT_DEPS]))
#define EXEC(e)       ((exec_order_graph_t *)((e)->art[NX_SLOT_EXEC]))

/* RAW: 関数/CFG/呼び出しグラフを用意 (何も読まない) */
static int p_prepare(nx_engine_t *e) {
    macho_binary_t *bin = MACHO_BIN(e);
    macho_detect_functions(bin);
    macho_build_cfg(bin);
    macho_build_callgraph(bin);
    macho_detect_loops(bin);
    macho_detect_switch_tables(bin);
    return NX_OK;
}

static int p_depflow(nx_engine_t *e) {
    depflow_graph_t *g = (depflow_graph_t *)calloc(1, sizeof(*g));
    if (!g) return NX_ENOMEM;
    depflow_graph_init(g);
    if (depflow_build_dependency_graph(MACHO_BIN(e), g) != 0) {
        depflow_graph_free(g);
        free(g);
        return NX_ERR;
    }
    e->art[NX_SLOT_DEPS] = g;
    return NX_OK;
}

static int p_vars(nx_engine_t *e) {
    var_classifier_t *vc = (var_classifier_t *)calloc(1, sizeof(*vc));
    if (!vc) return NX_ENOMEM;
    var_classifier_init(vc);
    if (var_classify_all(vc, MACHO_BIN(e), DEPS(e)) != 0) {
        var_classifier_free(vc);
        free(vc);
        return NX_ERR;
    }
    e->art[NX_SLOT_VARS] = vc;
    return NX_OK;
}

static int p_exec(nx_engine_t *e) {
    exec_order_graph_t *g = (exec_order_graph_t *)calloc(1, sizeof(*g));
    if (!g) return NX_ENOMEM;
    exec_order_graph_init(g);
    if (exec_order_build_graph(MACHO_BIN(e), DEPS(e), g) != 0) {
        exec_order_graph_free(g);
        free(g);
        return NX_ERR;
    }
    e->art[NX_SLOT_EXEC] = g;
    return NX_OK;
}

static int p_parallel(nx_engine_t *e) {
    parallel_analysis_t *pa = (parallel_analysis_t *)calloc(1, sizeof(*pa));
    parallel_opts_t opts;
    if (!pa) return NX_ENOMEM;
    memset(&opts, 0, sizeof(opts));
    opts.detect_beam_search = 1;
    opts.detect_thread_creation = 1;
    opts.detect_dispatch_patterns = 1;
    opts.detect_synchronization = 1;
    opts.detect_data_parallelism = 1;
    opts.detect_pipeline_parallelism = 1;
    parallel_analysis_init(pa);
    if (parallel_analyze(MACHO_BIN(e), DEPS(e), EXEC(e), &opts) != 0) {
        parallel_analysis_free(pa);
        free(pa);
        return NX_ERR;
    }
    e->art[NX_SLOT_PARALLEL] = pa;
    return NX_OK;
}

static int p_state(nx_engine_t *e) {
    state_graph_t *sg = (state_graph_t *)calloc(1, sizeof(*sg));
    state_opts_t opts;
    if (!sg) return NX_ENOMEM;
    memset(&opts, 0, sizeof(opts));
    opts.enable_checkpointing = 1;
    opts.enable_variable_tracking = 1;
    opts.enable_change_tracking = 1;
    opts.enable_dependency_analysis = 1;
    opts.enable_regression_analysis = 1;
    state_graph_init(sg);
    if (state_analyze(MACHO_BIN(e), DEPS(e), EXEC(e), &opts) != 0) {
        state_graph_free(sg);
        free(sg);
        return NX_ERR;
    }
    e->art[NX_SLOT_STATE] = sg;
    return NX_OK;
}

static const nx_pass_t g_macho_passes[] = {
    { "macho.prepare",  0,                          NX_ART_RAW,      10, p_prepare  },
    { "depflow",        NX_ART_RAW,                 NX_ART_DEPS,     30, p_depflow  },
    { "variables",      NX_ART_DEPS,                NX_ART_VARS,     20, p_vars     },
    { "execution",      NX_ART_DEPS,                NX_ART_EXEC,     25, p_exec     },
    { "parallel",       NX_ART_DEPS | NX_ART_EXEC,  NX_ART_PARALLEL, 40, p_parallel },
    { "state",          NX_ART_DEPS | NX_ART_EXEC,  NX_ART_STATE,    40, p_state    },
};

static const nx_pass_t *macho_passes(int *n_out) {
    *n_out = (int)(sizeof(g_macho_passes) / sizeof(g_macho_passes[0]));
    return g_macho_passes;
}

/* ------------------------------------------------------------------ */
/* provider descriptor                                                 */
/* ------------------------------------------------------------------ */

static const nx_provider_t g_macho_provider = {
    "macho",
    NX_CAP_LOAD | NX_CAP_MACHO | NX_CAP_FUNCS | NX_CAP_CFG |
    NX_CAP_DISASM | NX_CAP_STATIC,
    macho_detect,
    macho_open,
    macho_close,
    macho_arch,
    macho_entry,
    macho_nsections,
    macho_section,
    macho_nfuncs,
    macho_func,
    NULL,               /* disasm: prepare が bin->insns を埋める */
    macho_cfg,
    macho_augment,
    macho_passes
};

const nx_provider_t *nx_provider_macho(void) {
    return &g_macho_provider;
}
