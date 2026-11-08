#include "macho_static.h"
#include "arm64.h"            /* 旧 neodax エンジンの ARM64 デコーダ (src/arm64_decode.c) */
#include "integration/nx.h"     /* nx_decode — arch ごとのデコーダを実行時に選ぶ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "dax.h"   /* canonical parser API: dax_parse_macho(), dax_binary_t */

// ============================================================================
// Mach-O Static Analysis — analysis layer only.
// Wire parsing is composed from src/macho.c (dax_parse_macho).
// ============================================================================

// ============================================================================
// Initialization and Cleanup
// ============================================================================

void macho_init(macho_binary_t *bin) {
    if (!bin) return;
    
    memset(bin, 0, sizeof(macho_binary_t));
    bin->arch = MACHO_ARCH_UNKNOWN;
    bin->filetype = 0;
    bin->cputype = 0;
    bin->cpusubtype = 0;
    bin->flags = 0;
    bin->entry = 0;
    bin->base = 0;
    bin->image_size = 0;
    bin->nsections = 0;
    bin->nsymbols = 0;
    bin->symbols_allocated = 0;
    bin->nfunctions = 0;
    bin->functions_allocated = 0;
    bin->nxrefs = 0;
    bin->xrefs_allocated = 0;
    bin->nblocks = 0;
    bin->blocks_allocated = 0;
    bin->ninsns = 0;
    bin->insns_allocated = 0;
    bin->ndependencies = 0;
    bin->dependencies_allocated = 0;
    bin->ncall_chains = 0;
    bin->call_chains_allocated = 0;
    bin->nexecution_paths = 0;
    bin->execution_paths_allocated = 0;
    bin->nparallel_info = 0;
    bin->parallel_info_allocated = 0;
    bin->nvariables = 0;
    bin->variables_allocated = 0;
    bin->nstate_checkpoints = 0;
    bin->state_checkpoints_allocated = 0;
    bin->is_analyzed = 0;
    bin->is_pie = 0;
    bin->is_stripped = 0;
    bin->has_debug = 0;
    bin->total_insns = 0;
    bin->code_size = 0;
    bin->data_size = 0;
}

void macho_free(macho_binary_t *bin) {
    if (!bin) return;
    
    if (bin->data) {
        free(bin->data);
        bin->data = NULL;
    }
    
    if (bin->symbols) {
        free(bin->symbols);
        bin->symbols = NULL;
    }
    
    if (bin->functions) {
        free(bin->functions);
        bin->functions = NULL;
    }
    
    if (bin->xrefs) {
        free(bin->xrefs);
        bin->xrefs = NULL;
    }
    
    if (bin->blocks) {
        free(bin->blocks);
        bin->blocks = NULL;
    }
    
    if (bin->insns) {
        free(bin->insns);
        bin->insns = NULL;
    }
    
    if (bin->dependencies) {
        free(bin->dependencies);
        bin->dependencies = NULL;
    }
    
    if (bin->call_chains) {
        free(bin->call_chains);
        bin->call_chains = NULL;
    }
    
    if (bin->execution_paths) {
        free(bin->execution_paths);
        bin->execution_paths = NULL;
    }
    
    if (bin->parallel_info) {
        free(bin->parallel_info);
        bin->parallel_info = NULL;
    }
    
    if (bin->variables) {
        free(bin->variables);
        bin->variables = NULL;
    }
    
    if (bin->state_checkpoints) {
        free(bin->state_checkpoints);
        bin->state_checkpoints = NULL;
    }
    
    bin->size = 0;
    bin->nsections = 0;
    bin->nsymbols = 0;
    bin->symbols_allocated = 0;
    bin->nfunctions = 0;
    bin->functions_allocated = 0;
}

int macho_is_valid(macho_binary_t *bin) {
    if (!bin) return 0;
    if (!bin->data) return 0;
    if (bin->size == 0) return 0;
    if (bin->arch == MACHO_ARCH_UNKNOWN) return 0;
    return 1;
}

// ============================================================================
// Parsing — composition over the canonical engine parser (src/macho.c)
//
// Mach-O wire parsing exists in exactly one place: dax_parse_macho() fills a
// dax_binary_t. This file projects that result into the analysis view and does
// not re-implement the format itself.
// ============================================================================

static macho_arch_t project_arch(dax_arch_t a) {
    switch (a) {
        case ARCH_ARM64:   return MACHO_ARCH_ARM64;
        case ARCH_X86_64:  return MACHO_ARCH_X86_64;
        case ARCH_RISCV64: return MACHO_ARCH_RISCV64;
        case ARCH_X86:     return MACHO_ARCH_X86_32;
        default:           return MACHO_ARCH_UNKNOWN;
    }
}

static macho_sec_type_t project_sec(dax_sec_type_t t) {
    switch (t) {
        case SEC_TYPE_CODE:    return MACHO_SEC_TYPE_CODE;
        case SEC_TYPE_DATA:    return MACHO_SEC_TYPE_DATA;
        case SEC_TYPE_RODATA:  return MACHO_SEC_TYPE_RODATA;
        case SEC_TYPE_BSS:     return MACHO_SEC_TYPE_BSS;
        case SEC_TYPE_PLT:     return MACHO_SEC_TYPE_PLT;
        case SEC_TYPE_GOT:     return MACHO_SEC_TYPE_GOT;
        case SEC_TYPE_DYNAMIC: return MACHO_SEC_TYPE_DYNAMIC;
        case SEC_TYPE_DEBUG:   return MACHO_SEC_TYPE_DEBUG;
        case SEC_TYPE_OTHER:   return MACHO_SEC_TYPE_OTHER;
        default:               return MACHO_SEC_TYPE_UNKNOWN;
    }
}

static macho_symbol_type_t project_sym(dax_sym_type_t t) {
    switch (t) {
        case SYM_FUNC:   return MACHO_SYMBOL_TYPE_FUNC;
        case SYM_OBJECT: return MACHO_SYMBOL_TYPE_OBJECT;
        case SYM_IMPORT: return MACHO_SYMBOL_TYPE_IMPORT;
        case SYM_EXPORT: return MACHO_SYMBOL_TYPE_EXPORT;
        case SYM_WEAK:   return MACHO_SYMBOL_TYPE_WEAK;
        case SYM_LOCAL:  return MACHO_SYMBOL_TYPE_LOCAL;
        default:         return MACHO_SYMBOL_TYPE_UNKNOWN;
    }
}

/* dax_binary_t -> macho_binary_t. data/size stay borrowed by the parser. */
static int project_binary(const dax_binary_t *src, macho_binary_t *dst) {
    int i;

    dst->data        = src->data;   /* borrowed; macho_free releases it */
    dst->size        = src->size;
    dst->arch        = project_arch(src->arch);
    dst->filetype    = src->filetype;
    dst->cputype     = src->cputype;
    dst->cpusubtype  = src->cpusubtype;
    dst->flags       = src->flags;
    dst->entry       = src->entry;
    dst->base        = src->base;
    /* dax_binary_t stores image_size as a relative size; the analysis layer
       (nx_addr_in_image / function bounds) expects the absolute end address. */
    dst->image_size  = src->image_size ? (src->base + src->image_size) : 0;
    dst->is_pie      = src->is_pie;
    dst->is_stripped = src->is_stripped;
    dst->code_size   = src->code_size;
    dst->data_size   = src->data_size;
    strncpy(dst->filepath, src->filepath, sizeof(dst->filepath) - 1);

    dst->nsections = 0;
    for (i = 0; i < src->nsections && dst->nsections < MAX_MACHO_SECTIONS; i++) {
        const dax_section_t *s = &src->sections[i];
        macho_section_t *d = &dst->sections[dst->nsections++];
        memset(d, 0, sizeof(*d));
        strncpy(d->segname, s->segname, sizeof(d->segname) - 1);
        strncpy(d->name, s->name, sizeof(d->name) - 1);
        d->offset      = s->offset;
        d->vaddr       = s->vaddr;
        d->size        = s->size;
        d->flags       = s->flags;
        d->type        = project_sec(s->type);
        d->insn_count  = s->insn_count;
        d->file_offset = s->file_offset;
        d->executable  = s->executable;
        d->writable    = s->writable;
        d->readable    = s->readable;
    }

    dst->nsymbols = 0;
    if (src->symbols && src->nsymbols > 0) {
        int n = src->nsymbols < MAX_MACHO_SYMBOLS ? src->nsymbols : MAX_MACHO_SYMBOLS;
        dst->symbols = (macho_symbol_t *)calloc((size_t)n, sizeof(macho_symbol_t));
        if (!dst->symbols) return -1;
        dst->symbols_allocated = n;
        for (i = 0; i < n; i++) {
            const dax_symbol_t *s = &src->symbols[i];
            macho_symbol_t *d = &dst->symbols[dst->nsymbols++];
            d->address = s->address;
            strncpy(d->name, s->name, sizeof(d->name) - 1);
            strncpy(d->demangled, s->name, sizeof(d->demangled) - 1);
            d->type         = project_sym(s->type);
            d->is_entry     = s->is_entry;
            d->is_global    = s->is_global;
            d->is_weak      = s->is_weak;
            d->is_undefined = s->is_undefined;
            d->section_idx  = s->section_idx;
        }
    }
    return 0;
}

int macho_parse(macho_binary_t *bin, const uint8_t *data, size_t size) {
    if (!bin || !data || size == 0) return -1;

    dax_binary_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.data = (uint8_t *)data;   /* borrowed by the canonical parser */
    tmp.size = size;
    strncpy(tmp.filepath, bin->filepath, sizeof(tmp.filepath) - 1);

    if (dax_parse_macho(&tmp) != 0) {
        free(tmp.symbols);
        return -1;
    }

    int rc = project_binary(&tmp, bin);
    free(tmp.symbols);            /* projection copied what it needs */
    return rc;
}


// ============================================================================
// Basic Analysis Functions
// ============================================================================

int macho_analyze(macho_binary_t *bin, macho_opts_t *opts) {
    if (!bin || !macho_is_valid(bin)) return -1;
    
    // Detect functions
    if (macho_detect_functions(bin) != 0) {
        fprintf(stderr, "macho: Failed to detect functions\n");
        return -1;
    }
    
    // Build control flow graph
    if (macho_build_cfg(bin) != 0) {
        fprintf(stderr, "macho: Failed to build CFG\n");
        return -1;
    }
    
    // Build call graph
    if (macho_build_callgraph(bin) != 0) {
        fprintf(stderr, "macho: Failed to build call graph\n");
        return -1;
    }
    
    // Detect loops
    if (macho_detect_loops(bin) != 0) {
        fprintf(stderr, "macho: Failed to detect loops\n");
        // Non-fatal
    }
    
    // Detect switch tables
    if (macho_detect_switch_tables(bin) != 0) {
        fprintf(stderr, "macho: Failed to detect switch tables\n");
        // Non-fatal
    }
    
    // Data dependency analysis
    if (opts && opts->depflow) {
        if (macho_analyze_dependencies(bin, opts) != 0) {
            fprintf(stderr, "macho: Failed to analyze dependencies\n");
            // Non-fatal
        }
    }
    
    // Execution order analysis
    if (opts && opts->execution_order) {
        if (macho_analyze_execution_order(bin) != 0) {
            fprintf(stderr, "macho: Failed to analyze execution order\n");
            // Non-fatal
        }
    }
    
    // Parallel execution detection
    if (opts && opts->parallel_detection) {
        if (macho_detect_parallel_execution(bin) != 0) {
            fprintf(stderr, "macho: Failed to detect parallel execution\n");
            // Non-fatal
        }
    }
    
    // Beam search pattern analysis
    if (opts && opts->beam_search_analysis) {
        if (macho_analyze_beam_search_patterns(bin) != 0) {
            fprintf(stderr, "macho: Failed to analyze beam search patterns\n");
            // Non-fatal
        }
    }
    
    bin->is_analyzed = 1;
    return 0;
}

// ============================================================================
// Function Detection (Stub)
// ============================================================================

int macho_detect_functions(macho_binary_t *bin) {
    // This is a placeholder - actual implementation would use
    // symbol information and heuristic analysis

    /* 既に検出済みなら再確保しない (冪等) */
    if (bin->functions) return 0;
    
    if (bin->nsymbols > 0) {
        // Allocate function array
        bin->functions_allocated = bin->nsymbols;
        bin->functions = (macho_function_t *)calloc(bin->nsymbols, sizeof(macho_function_t));
        if (!bin->functions) return -1;
        
        for (int i = 0; i < bin->nsymbols; i++) {
            macho_symbol_t *sym = &bin->symbols[i];
            
            if (sym->type == MACHO_SYMBOL_TYPE_FUNC) {
                macho_function_t *func = &bin->functions[bin->nfunctions++];
                
                func->start = sym->address;
                func->end = sym->address + sym->size; // Size might not be accurate
                strncpy(func->name, sym->demangled[0] ? sym->demangled : sym->name, sizeof(func->name) - 1);
                func->name[sizeof(func->name) - 1] = '\0';
                func->sym_idx = i;
                func->insn_count = 0;
                func->block_count = 0;
                func->has_loops = 0;
                func->has_calls = 0;
                func->has_branches = 0;
                func->has_indirect_calls = 0;
            }
        }
    }
    
    return 0;
}

// ============================================================================
// Cross-reference / function-bound analysis
//
// 旧 neodax エンジンの ARM64 デコーダ (arm64_decode.c の a64_decode) を呼び、
// 分岐/呼び出しから xref を構築する。depflow はこの xref からのみ
// 制御フロー edges を作るため、xref が無いと edges=0 になる。
// ============================================================================

static int nx_add_xref(macho_binary_t *bin, uint64_t from, uint64_t to,
                       macho_xref_type_t type, int is_call, int is_cond, int is_indirect) {
    if (bin->nxrefs >= bin->xrefs_allocated) {
        int ncap = bin->xrefs_allocated ? bin->xrefs_allocated * 2 : 4096;
        macho_xref_t *nx = (macho_xref_t *)realloc(bin->xrefs,
                                                   (size_t)ncap * sizeof(macho_xref_t));
        if (!nx) return -1;
        bin->xrefs = nx;
        bin->xrefs_allocated = ncap;
    }
    macho_xref_t *x = &bin->xrefs[bin->nxrefs++];
    memset(x, 0, sizeof(*x));
    x->from = from;
    x->to = to;
    x->type = type;
    x->is_call = is_call;
    x->is_conditional = is_cond;
    x->is_indirect = is_indirect;
    x->resolved_target = is_indirect ? 0 : to;
    return 0;
}

static int nx_addr_in_image(macho_binary_t *bin, uint64_t a) {
    uint64_t hi = bin->image_size;
    if (hi == 0) {
        for (int s = 0; s < bin->nsections; s++) {
            uint64_t e = bin->sections[s].vaddr + bin->sections[s].size;
            if (e > hi) hi = e;
        }
    }
    return hi != 0 && a >= bin->base && a < hi;
}

/* 関数 end を「次に大きい関数/シンボル先頭」で確定する。
   depflow の関数↔変数エッジは func->end を使うため必須。 */
static void nx_compute_function_bounds(macho_binary_t *bin) {
    uint64_t limit = bin->image_size;
    if (limit == 0) {
        for (int s = 0; s < bin->nsections; s++) {
            uint64_t e = bin->sections[s].vaddr + bin->sections[s].size;
            if (e > limit) limit = e;
        }
    }
    for (int i = 0; i < bin->nfunctions; i++) {
        uint64_t start = bin->functions[i].start;
        uint64_t end = (limit > start) ? limit : start + 4;
        for (int j = 0; j < bin->nfunctions; j++) {
            uint64_t s = bin->functions[j].start;
            if (s > start && s < end) end = s;
        }
        for (int j = 0; j < bin->nsymbols; j++) {
            uint64_t s = bin->symbols[j].address;
            if (s > start && s < end) end = s;
        }
        bin->functions[i].end = end;
    }
}

static nx_arch_t macho_to_nx_arch(macho_arch_t a) {
    switch (a) {
        case MACHO_ARCH_ARM64:  return NX_ARCH_ARM64;
        case MACHO_ARCH_X86_64: return NX_ARCH_X86_64;
        case MACHO_ARCH_X86_32: return NX_ARCH_X86_32;
        default:                return NX_ARCH_UNKNOWN;
    }
}

static int macho_insn_cmp(const void *a, const void *b) {
    uint64_t x = ((const macho_insn_t *)a)->address;
    uint64_t y = ((const macho_insn_t *)b)->address;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Decode every executable section once into bin->insns. */
int macho_fill_insns(macho_binary_t *bin) {
    if (!bin || !bin->data) return -1;
    if (bin->insns) return 0;   /* idempotent */

    nx_arch_t na = macho_to_nx_arch(bin->arch);
    if (na == NX_ARCH_UNKNOWN) return 0;   /* no decoder for this arch */

    size_t cap = 0;
    for (int s = 0; s < bin->nsections; s++) {
        macho_section_t *sec = &bin->sections[s];
        if (!(sec->executable || sec->type == MACHO_SEC_TYPE_CODE)) continue;
        cap += (size_t)(sec->size / 4) + 1;
    }
    if (cap == 0) return 0;

    bin->insns = (macho_insn_t *)calloc(cap, sizeof(macho_insn_t));
    if (!bin->insns) return -1;
    bin->insns_allocated = (int)cap;
    bin->ninsns = 0;

    for (int s = 0; s < bin->nsections; s++) {
        macho_section_t *sec = &bin->sections[s];
        if (!(sec->executable || sec->type == MACHO_SEC_TYPE_CODE)) continue;

        for (uint64_t off = 0; off < sec->size && bin->ninsns < bin->insns_allocated; ) {
            size_t fpos = (size_t)sec->file_offset + (size_t)off;
            if (fpos + 4 > bin->size) break;

            size_t maxlen = (size_t)(sec->size - off);
            if (maxlen > 15) maxlen = 15;
            if (fpos + maxlen > bin->size) maxlen = bin->size - fpos;

            nx_insn_t ni;
            if (nx_decode(na, sec->vaddr + off, bin->data + fpos, maxlen, &ni) != NX_OK
                || ni.length == 0) {
                off += 4;
                continue;
            }

            macho_insn_t *mi = &bin->insns[bin->ninsns++];
            memset(mi, 0, sizeof(*mi));
            mi->address = ni.address;
            mi->length  = ni.length;
            /* 生の命令バイトを実アドレスに対応させて保持する。
               後段の再デコード (ADRP/ADD/LDR の解析) はここを読むため、
               bytes を欠かすと命令が 0 個になる。 */
            memcpy(mi->bytes, bin->data + fpos, ni.length);
            strncpy(mi->mnemonic, ni.mnemonic, sizeof(mi->mnemonic) - 1);
            strncpy(mi->operands, ni.operands, sizeof(mi->operands) - 1);
            mi->is_call       = ni.is_call;
            mi->is_branch     = ni.is_branch;
            mi->is_conditional= ni.is_cond;
            mi->is_indirect   = ni.is_indirect;
            mi->is_return     = ni.is_ret;
            mi->has_target    = ni.has_target;
            mi->target        = ni.target;
            off += ni.length;
        }
    }

    /* Sort by address so consumers can binary-search a function's range. */
    if (bin->ninsns > 1)
        qsort(bin->insns, (size_t)bin->ninsns, sizeof(macho_insn_t), macho_insn_cmp);
    return 0;
}

static int nx_build_xrefs_arm64(macho_binary_t *bin) {
    if (!bin->data) return -1;

    /* Prefer the pre-decoded instruction list (single decode, all arches). */
    if (bin->ninsns > 0) {
        for (int k = 0; k < bin->ninsns; k++) {
            macho_insn_t *in = &bin->insns[k];
            if (!(in->is_call || in->is_branch)) continue;
            if (in->is_indirect) {
                nx_add_xref(bin, in->address, 0,
                            in->is_call ? MACHO_XREF_TYPE_CALL : MACHO_XREF_TYPE_BRANCH,
                            in->is_call, in->is_conditional, 1);
            } else if (in->has_target && nx_addr_in_image(bin, in->target)) {
                nx_add_xref(bin, in->address, in->target,
                            in->is_call ? MACHO_XREF_TYPE_CALL : MACHO_XREF_TYPE_BRANCH,
                            in->is_call, in->is_conditional, 0);
            }
        }
        return 0;
    }

    for (int s = 0; s < bin->nsections; s++) {
        macho_section_t *sec = &bin->sections[s];
        if (!(sec->executable || sec->type == MACHO_SEC_TYPE_CODE)) continue;

        for (uint64_t off = 0; off + 4 <= sec->size; off += 4) {
            size_t fpos = (size_t)sec->file_offset + (size_t)off;
            if (fpos + 4 > bin->size) break;

            uint32_t raw = (uint32_t)bin->data[fpos]
                         | ((uint32_t)bin->data[fpos + 1] << 8)
                         | ((uint32_t)bin->data[fpos + 2] << 16)
                         | ((uint32_t)bin->data[fpos + 3] << 24);
            uint64_t addr = sec->vaddr + off;

            a64_insn_t insn;
            a64_decode(raw, addr, &insn);
            const char *m = insn.mnemonic;

            if (!strcmp(m, "bl") || !strcmp(m, "b") ||
                !strcmp(m, "cbz") || !strcmp(m, "cbnz") ||
                !strcmp(m, "tbz") || !strcmp(m, "tbnz") ||
                !strncmp(m, "b.", 2)) {
                uint64_t target = 0;
                const char *p = strstr(insn.operands, "0x");
                if (p && sscanf(p, "0x%llx", (unsigned long long *)&target) == 1 &&
                    nx_addr_in_image(bin, target)) {
                    int is_call = !strcmp(m, "bl");
                    int is_cond = (m[0] == 'b' && m[1] == '.') ||
                                  !strcmp(m, "cbz") || !strcmp(m, "cbnz") ||
                                  !strcmp(m, "tbz") || !strcmp(m, "tbnz");
                    nx_add_xref(bin, addr, target,
                                is_call ? MACHO_XREF_TYPE_CALL : MACHO_XREF_TYPE_BRANCH,
                                is_call, is_cond, 0);
                }
            } else if (!strcmp(m, "blr") || !strcmp(m, "br")) {
                int is_call = !strcmp(m, "blr");
                nx_add_xref(bin, addr, 0,
                            is_call ? MACHO_XREF_TYPE_CALL : MACHO_XREF_TYPE_BRANCH,
                            is_call, 0, 1);
            }
        }
    }
    return 0;
}

// ============================================================================
// Call Graph Building
// ============================================================================

int macho_build_callgraph(macho_binary_t *bin) {
    if (!bin) return -1;
    if (bin->arch == MACHO_ARCH_ARM64 && bin->nxrefs == 0)
        nx_build_xrefs_arm64(bin);
    return 0;
}

// ============================================================================
// CFG Building
// ============================================================================

static int nx_u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int nx_xref_cmp(const void *a, const void *b) {
    const macho_xref_t *x = (const macho_xref_t *)a;
    const macho_xref_t *y = (const macho_xref_t *)b;
    return (x->from > y->from) - (x->from < y->from);
}


/* xref を from 昇順に保つ。CFG 構築が単一パスで済む前提になる。 */
static void nx_sort_xrefs(macho_binary_t *bin) {
    if (bin->nxrefs > 1)
        qsort(bin->xrefs, (size_t)bin->nxrefs, sizeof(macho_xref_t), nx_xref_cmp);
}

/* from 昇順の配列から addr 以上の最初の添字。 */
static int nx_first_xref_at_or_after(const macho_binary_t *bin, uint64_t addr) {
    int lo = 0, hi = bin->nxrefs;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (bin->xrefs[mid].from < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* to 昇順の配列から addr 以上の最初の添字。 */
// 命令列から addr 以上の最初の添字を返す (命令は昇順)。
static int nx_insn_at_or_after(const macho_binary_t *bin, uint64_t addr) {
    int lo = 0, hi = bin->ninsns;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (bin->insns[mid].address < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}


/* [first, nblocks) の中で addr を含むブロックの添字。無ければ -1。 */
static int nx_block_at(const macho_binary_t *bin, int first, uint64_t addr) {
    int lo = first, hi = bin->nblocks - 1, best = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (bin->blocks[mid].start <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return -1;
    if (addr < bin->blocks[best].end) return best;
    return -1;
}

int macho_build_cfg(macho_binary_t *bin) {
    if (!bin || !bin->data) return -1;
    macho_fill_insns(bin);   /* populate bin->insns before any CFG work */
    if (bin->arch != MACHO_ARCH_ARM64) return 0;

    nx_compute_function_bounds(bin);
    if (bin->nxrefs == 0) nx_build_xrefs_arm64(bin);
    nx_sort_xrefs(bin);

    /* 関数ごとに基本ブロックを構築する。
       境界 = 関数先頭 ∪ 関数内の分岐先 ∪ 条件分岐直後のフォールスルー。 */
    // ブロック上限は「データから導ける上界」で決める。
    // ブロック境界になり得るのは
    //   ・関数の先頭        (高々 nfunctions)
    //   ・分岐先 / フォールスルー (高々 nxrefs)
    // だけなので、両者の和が上界になる。関数数に係数を掛けるような
    // 対象依存の当て推量はしない。
    size_t cap = (size_t)bin->nfunctions + (size_t)bin->nxrefs;
    if (cap < 4096) cap = 4096;
    if (cap > (size_t)INT_MAX) cap = (size_t)INT_MAX;
    if (!bin->blocks) {
        bin->blocks = (macho_block_t *)calloc((size_t)cap, sizeof(macho_block_t));
        if (!bin->blocks) return -1;
        bin->blocks_allocated = (int)cap;
    }

    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *f = &bin->functions[i];
        if (f->end <= f->start) continue;

        uint64_t bounds[512];
        int nb = 0;
        bounds[nb++] = f->start;

        /* 索引を使わない。関数の命令列を自分の範囲だけ走査する。
           xref は命令から作られているので、範囲内の命令を見れば
           「その関数が持ちうる境界」がすべて分かる。
           他の関数を参照しないので、判定はその関数だけで閉じる。 */
        int a0 = nx_insn_at_or_after(bin, f->start);
        for (int k = a0; k < bin->ninsns; k++) {
            const macho_insn_t *in = &bin->insns[k];
            if (in->address >= f->end) break;        /* 命令は昇順 */
            if (in->address < f->start) continue;

            /* 分岐先 (直接分岐のみ) */
            if (in->has_target && !in->is_indirect &&
                in->target >= f->start && in->target < f->end) {
                if (nb < 512) bounds[nb++] = in->target;
                else { bin->cfg_truncated = 1; break; }
            }
            /* 条件分岐のフォールスルー */
            if (in->is_conditional) {
                uint64_t nxt = in->address + in->length;
                if (nxt >= f->start && nxt < f->end) {
                    if (nb < 512) bounds[nb++] = nxt;
                    else { bin->cfg_truncated = 1; break; }
                }
            }
        }

        /* 重複除去 + 昇順化は qsort 1 回で済ませる (挿入ソートは O(n^2))。 */
        if (nb > 1) {
            qsort(bounds, (size_t)nb, sizeof(uint64_t), nx_u64_cmp);
            int w = 1;
            for (int a = 1; a < nb; a++)
                if (bounds[a] != bounds[w - 1]) bounds[w++] = bounds[a];
            nb = w;
        }

        int first_idx = bin->nblocks;
        for (int b = 0; b < nb && bin->nblocks < bin->blocks_allocated; b++) {
            uint64_t bstart = bounds[b];
            uint64_t bend = (b + 1 < nb) ? bounds[b + 1] : f->end;
            if (bend <= bstart) continue;
            macho_block_t *blk = &bin->blocks[bin->nblocks];
            memset(blk, 0, sizeof(*blk));
            blk->id = bin->nblocks;
            blk->start = bstart;
            blk->end = bend;
            blk->func_idx = i;
            blk->block_type = (b == 0) ? MACHO_BLOCK_TYPE_ENTRY : MACHO_BLOCK_TYPE_REGULAR;
            blk->is_entry = (b == 0);
            bin->nblocks++;
        }
        if (nb > 0 && bin->nblocks >= bin->blocks_allocated) {
            bin->cfg_truncated = 1;   // ブロック枠の枯渇
        }

        /* ブロック間 succ を張る。
           ブロックは関数内でアドレス昇順に作られるので block.start は単調。
           よって
             - blk->end-4 の xref は二分探索で 1 点に絞れ、
             - 飛び先のブロックも二分探索で求まる。
           以前はブロックごとに全 xref を走査し、飛び先ごとに全ブロックを
           線形探索していた (二重の総当たり)。 */
        for (int b = first_idx; b < bin->nblocks; b++) {
            macho_block_t *blk = &bin->blocks[b];
            uint64_t last = blk->end - 4;   /* ARM64 は固定長 */

            int x0 = nx_first_xref_at_or_after(bin, last);
            for (int x = x0; x < bin->nxrefs && bin->xrefs[x].from == last; x++) {
                const macho_xref_t *xr = &bin->xrefs[x];
                if (!xr->is_indirect && xr->to != 0) {
                    int t = nx_block_at(bin, first_idx, xr->to);
                    if (t >= 0 && blk->nsucc < 16) {
                        blk->succ[blk->nsucc] = t;
                        blk->edge_type[blk->nsucc] = xr->is_conditional
                            ? MACHO_EDGE_TYPE_COND_TRUE : MACHO_EDGE_TYPE_JUMP;
                        blk->nsucc++;
                    }
                }
                if (xr->is_conditional && last + 4 < blk->end) {
                    int t = nx_block_at(bin, first_idx, last + 4);
                    if (t >= 0 && blk->nsucc < 16) {
                        blk->succ[blk->nsucc] = t;
                        blk->edge_type[blk->nsucc] = MACHO_EDGE_TYPE_COND_FALSE;
                        blk->nsucc++;
                    }
                }
            }
            if (blk->nsucc == 0 && blk->end >= f->end) blk->is_exit = 1;
        }
        f->block_count = (uint32_t)(bin->nblocks - first_idx);
    }

    /* pred を succ から逆算 */
    for (int b = 0; b < bin->nblocks; b++) {
        macho_block_t *blk = &bin->blocks[b];
        for (int k = 0; k < blk->nsucc; k++) {
            int t = blk->succ[k];
            if (t >= 0 && t < bin->nblocks && bin->blocks[t].npred < 16)
                bin->blocks[t].pred[bin->blocks[t].npred++] = b;
        }
    }
    return 0;
}

// ============================================================================
// Loop Detection (Stub)
// ============================================================================

int macho_detect_loops(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Switch Table Detection (Stub)
// ============================================================================

int macho_detect_switch_tables(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Data Dependency Analysis (Stub)
// ============================================================================

int macho_analyze_dependencies(macho_binary_t *bin, macho_opts_t *opts) {
    // Placeholder
    return 0;
}

// ============================================================================
// Execution Order Analysis (Stub)
// ============================================================================

int macho_analyze_execution_order(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Parallel Execution Detection (Stub)
// ============================================================================

int macho_detect_parallel_execution(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Beam Search Pattern Analysis (Stub)
// ============================================================================

int macho_analyze_beam_search_patterns(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// State Tracking (Stub)
// ============================================================================

int macho_track_state_changes(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Regression Analysis (Stub)
// ============================================================================

int macho_analyze_regression(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// State Checkpoint Building (Stub)
// ============================================================================

int macho_build_state_checkpoints(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// CFG 自己検証
//
// 高速経路 (二分探索 + to 昇順索引) は、xref の並びに関する前提に依存する。
// 前提が崩れると黙って境界やエッジを取りこぼす。そこで
//   - 実際に使った高速経路の結果
//   - 総当たりで数え直した参照実装の結果
// を同じバイナリで比べる。一致すれば前提が成り立っていたと言える。
// 「速いから正しい」ではなく「両者が一致したから正しい」とする。
// ============================================================================

// 参照実装: 関数ごとに全 xref を走査して境界を集める (O(nfunc * nxref))。
static int cfg_reference_bounds(const macho_binary_t *bin, int func_idx,
                                uint64_t *bounds, int cap) {
    const macho_function_t *f = &bin->functions[func_idx];
    if (f->end <= f->start) return 0;
    int nb = 0;
    bounds[nb++] = f->start;
    for (int x = 0; x < bin->nxrefs; x++) {
        const macho_xref_t *xr = &bin->xrefs[x];
        if (!xr->is_indirect && xr->to != 0 &&
            xr->to >= f->start && xr->to < f->end) {
            int dup = 0;
            for (int b = 0; b < nb; b++) if (bounds[b] == xr->to) { dup = 1; break; }
            if (!dup && nb < cap) bounds[nb++] = xr->to;
        }
        if (xr->is_conditional) {
            uint64_t nxt = xr->from + 4;
            if (nxt >= f->start && nxt < f->end) {
                int dup = 0;
                for (int b = 0; b < nb; b++) if (bounds[b] == nxt) { dup = 1; break; }
                if (!dup && nb < cap) bounds[nb++] = nxt;
            }
        }
    }
    // 昇順化
    for (int a = 0; a < nb; a++)
        for (int b = a + 1; b < nb; b++)
            if (bounds[b] < bounds[a]) { uint64_t t = bounds[a]; bounds[a] = bounds[b]; bounds[b] = t; }
    return nb;
}

// 高速経路の境界を関数ごとに取り出す。
uint64_t nx_cfg_digest(const macho_binary_t *bin) {
    // ブロック数と (start,end) のハッシュ。順序も含める。
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < bin->nblocks; i++) {
        h ^= bin->blocks[i].start;      h *= 1099511628211ULL;
        h ^= bin->blocks[i].end;        h *= 1099511628211ULL;
        h ^= (uint64_t)bin->blocks[i].func_idx; h *= 1099511628211ULL;
        h ^= (uint64_t)bin->blocks[i].nsucc;    h *= 1099511628211ULL;
    }
    return h;
}

// 参照実装のダイジェスト。
// 高速経路と同じ項目 (start, end, func_idx, nsucc) を、
// 総当たりで求め直してハッシュする。succ も参照側で数え直すので、
// エッジの取りこぼしも検出できる。
uint64_t nx_cfg_digest_reference(const macho_binary_t *bin) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < bin->nfunctions; i++) {
        uint64_t b[512];
        int nb = cfg_reference_bounds(bin, i, b, 512);
        for (int k = 0; k < nb; k++) {
            uint64_t bstart = b[k];
            uint64_t bend = (k + 1 < nb) ? b[k + 1] : bin->functions[i].end;
            if (bend <= bstart) continue;
            // このブロックの succ を総当たりで数える
            uint64_t last = bend - 4;
            int ns = 0;
            for (int x = 0; x < bin->nxrefs; x++) {
                const macho_xref_t *xr = &bin->xrefs[x];
                if (xr->from != last) continue;
                if (!xr->is_indirect && xr->to != 0) {
                    // to がいずれかのブロックに入るか
                    for (int t = 0; t < nb; t++) {
                        uint64_t ts = b[t];
                        uint64_t te = (t + 1 < nb) ? b[t + 1] : bin->functions[i].end;
                        if (ts <= xr->to && xr->to < te) { ns++; break; }
                    }
                }
                if (xr->is_conditional && last + 4 < bend) ns++;
            }
            if (ns > 16) ns = 16;
            h ^= bstart;              h *= 1099511628211ULL;
            h ^= bend;                h *= 1099511628211ULL;
            h ^= (uint64_t)i;         h *= 1099511628211ULL;
            h ^= (uint64_t)ns;        h *= 1099511628211ULL;
        }
    }
    return h;
}

int nx_cfg_ref_block_count(const macho_binary_t *bin) {
    int total = 0;
    for (int i = 0; i < bin->nfunctions; i++) {
        uint64_t b[512];
        int nb = cfg_reference_bounds(bin, i, b, 512);
        for (int k = 0; k < nb; k++) {
            uint64_t s = b[k];
            uint64_t e = (k + 1 < nb) ? b[k + 1] : bin->functions[i].end;
            if (e > s) total++;
        }
    }
    return total;
}

int nx_cfg_ref_bounds(const macho_binary_t *bin, int idx, uint64_t *b, int cap) {
    return cfg_reference_bounds(bin, idx, b, cap);
}
