/*
 * nx_provider_dax.c — 旧 neodax エンジン (src 直下の .c, include/dax.h) を nx ABI に適合させる。
 *
 * dax.h は新フレームワークの解析ヘッダと同一 TU で共存できないため、
 * このファイルだけが dax.h を include する。
 *
 * NX_WITH_LEGACY が定義されているときだけ実際の dax_* を呼ぶ。
 * 未定義 (既定) のときは「形式は認識するが load 不可」の provider を返し、
 * nx_select_provider が macho provider へ fallback する。
 */

#include "nx.h"

#include <stdlib.h>
#include <string.h>

#ifdef NX_WITH_LEGACY
#include "dax.h"

typedef struct {
    dax_binary_t bin;
} dax_impl_t;

static int dax_detect(const uint8_t *data, size_t size) {
    if (!data || size < 4) return 0;
    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
        return 1;                                  /* ELF */
    if (data[0] == 'M' && data[1] == 'Z')
        return 1;                                  /* PE  */
    return 1;                                      /* raw fallback */
}

static nx_binary_t *dax_open(const char *path, const uint8_t *data, size_t size) {
    (void)size;
    if (!path) { free((void *)data); return NULL; }

    dax_impl_t *impl = (dax_impl_t *)calloc(1, sizeof(*impl));
    if (!impl) { free((void *)data); return NULL; }

    /* dax_load_binary は自前でファイルを読む。nx_open が渡した buffer は不要。 */
    free((void *)data);

    if (dax_load_binary(path, &impl->bin) != 0) {
        free(impl);
        return NULL;
    }
    dax_symexec_prepass(&impl->bin);

    nx_binary_t *b = (nx_binary_t *)calloc(1, sizeof(*b));
    if (!b) { dax_free_binary(&impl->bin); free(impl); return NULL; }
    b->impl = impl;
    b->ops  = nx_provider_dax();
    return b;
}

static void dax_close(nx_binary_t *b) {
    if (!b) return;
    dax_impl_t *impl = (dax_impl_t *)b->impl;
    if (impl) { dax_free_binary(&impl->bin); free(impl); }
    free(b);
}

static nx_arch_t dax_arch(nx_binary_t *b) {
    switch (((dax_impl_t *)b->impl)->bin.arch) {
        case ARCH_X86_64:  return NX_ARCH_X86_64;
        case ARCH_ARM64:   return NX_ARCH_ARM64;
        case ARCH_RISCV64: return NX_ARCH_RISCV64;
        case ARCH_X86:     return NX_ARCH_X86_32;
        default:           return NX_ARCH_UNKNOWN;
    }
}

static uint64_t dax_entry(nx_binary_t *b) { return ((dax_impl_t *)b->impl)->bin.entry; }
static int dax_nsections(nx_binary_t *b)   { return ((dax_impl_t *)b->impl)->bin.nsections; }

static int dax_section(nx_binary_t *b, int i, nx_section_t *out) {
    dax_impl_t *im = (dax_impl_t *)b->impl;
    if (i < 0 || i >= im->bin.nsections) return NX_ERR;
    dax_section_t *s = &im->bin.sections[i];
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

static int dax_nfuncs(nx_binary_t *b) { return ((dax_impl_t *)b->impl)->bin.nfunctions; }

static int dax_func(nx_binary_t *b, int i, nx_func_t *out) {
    dax_impl_t *im = (dax_impl_t *)b->impl;
    if (i < 0 || i >= im->bin.nfunctions) return NX_ERR;
    dax_func_t *f = &im->bin.functions[i];
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

static int dax_augment(nx_binary_t *b, uint32_t want) {
    dax_impl_t *im = (dax_impl_t *)b->impl;
    if (want & NX_CAP_SYMEXEC) dax_symexec_prepass(&im->bin);
    return NX_OK;
}

/* 旧エンジンの解析はすべて dax_binary_t 自体を書き換えるため
   NX_ART_RAW に対する read/write として宣言され、スケジューラは直列化する。 */
static int p_dax_symexec(nx_engine_t *e) {
    dax_impl_t *im = (dax_impl_t *)e->bin->impl;
    dax_symexec_prepass(&im->bin);
    return NX_OK;
}

static const nx_pass_t g_dax_passes[] = {
    { "dax.symexec", NX_ART_RAW, NX_ART_RAW, 50, p_dax_symexec },
};

static const nx_pass_t *dax_passes(int *n_out) {
    *n_out = (int)(sizeof(g_dax_passes) / sizeof(g_dax_passes[0]));
    return g_dax_passes;
}

/* neodax_bridge から呼ばれる、旧エンジンへの実処理入口。
   dax.h を知るこの TU だけが旧 dax_* を呼べる。 */
int nx_dax_integrate(void *dax_bin, FILE *out) {
    if (!dax_bin) return NX_ERR;
    dax_binary_t *bin = (dax_binary_t *)dax_bin;
    dax_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.aire = 1;
    opts.poly_map = 1;

    dax_symexec_prepass(bin);
    if (out) fprintf(out, "[dax] symexec prepass done (indirect=%d smc=%d)\n",
                     bin->nresolved_indirect, bin->nsmc_patches);
    dax_aire_analyze(bin, &opts, out);
    dax_poly_map(bin, &opts, out);
    return NX_OK;
}

static const nx_provider_t g_dax_provider = {
    "dax",
    NX_CAP_LOAD | NX_CAP_ELF | NX_CAP_PE | NX_CAP_RAW | NX_CAP_FUNCS |
    NX_CAP_CFG | NX_CAP_DISASM | NX_CAP_SYMEXEC | NX_CAP_EMULATE |
    NX_CAP_DECOMPILE | NX_CAP_DSA | NX_CAP_AIRE,
    dax_detect, dax_open, dax_close,
    dax_arch, dax_entry, dax_nsections, dax_section, dax_nfuncs, dax_func,
    NULL, NULL, dax_augment, dax_passes
};

const nx_provider_t *nx_provider_dax(void) {
    return &g_dax_provider;
}

#else /* !NX_WITH_LEGACY ------------------------------------------------ */

/* 旧エンジン未リンク: 認識だけして load 不可。選択時は macho へ fallback。 */
static int dax_detect_stub(const uint8_t *data, size_t size) {
    if (!data || size < 4) return 0;
    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
        return 1;
    if (data[0] == 'M' && data[1] == 'Z')
        return 1;
    return 0;
}

static const nx_provider_t g_dax_stub = {
    "dax(disabled)",
    0,                    /* NX_CAP_LOAD なし → 選択対象にならない */
    dax_detect_stub,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

const nx_provider_t *nx_provider_dax(void) {
    return &g_dax_stub;
}

int nx_dax_integrate(void *dax_bin, FILE *out) {
    (void)dax_bin;
    if (out) fprintf(out, "[dax] legacy engine not linked (build with -DNX_WITH_LEGACY)\n");
    return NX_ENOTSUP;
}

#endif /* NX_WITH_LEGACY */
