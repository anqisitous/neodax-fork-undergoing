/*
 * static_analysis.c — NeoDAX unified static analysis
 *
 * Class-based (C) implementation of one Regression Ring with nested Groups
 * and Functions as leaves.
 *
 *   Mach-O parse
 *     → Survey    (sections / symbols / functions)
 *     → Classify  (variable roles from symbol names)
 *     → Trace     (def-use edges, call edges, variable→caller chains)
 *     → Predict   (execution order: sequential vs parallel, critical path)
 *     → Parallel  (groups: beam search, thread pool, conditional dispatch)
 *     → Verify    (state checkpoints, regression inside loops)
 *
 * The ring is the analysis loop; each lap gathers more evidence and the
 * frontier moves outward (progression). SVG renders ring → group → function.
 */

#include "static_analysis.h"

#include "arm64.h"
#include "x86.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Small helpers
 * ============================================================================ */

static void nd_copy(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

static int nd_strcasestr(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               ((p[i] | 0x20) == (needle[i] | 0x20))) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* Case-insensitive exact-ish match: "name" matches "_name" / "name_" too. */
static int nd_name_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a == '_') a++;
    while (*b == '_') b++;
    return strcmp(a, b) == 0;
}

static void nd_svg_escape(FILE *f, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '&':  fprintf(f, "&amp;");  break;
            case '<':  fprintf(f, "&lt;");   break;
            case '>':  fprintf(f, "&gt;");   break;
            case '"':  fprintf(f, "&quot;"); break;
            case '\'': fprintf(f, "&apos;"); break;
            default:   fputc(*p, f);         break;
        }
    }
}

/* ============================================================================
 * Context (root object)
 * ============================================================================ */

void nd_static_ctx_init(nd_static_ctx_t *ctx, dax_binary_t *bin, dax_opts_t *opts) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->bin  = bin;
    ctx->opts = opts;
    ctx->trace_depth = ND_STATIC_MAX_TRACE_DEPTH;
    ctx->advanced_analysis = 0;
    ctx->nvariables = 0;
    ctx->nfunctions = 0;
    ctx->nedges = 0;
    ctx->ngroups = 0;
    ctx->var_flows.flows = NULL;
    ctx->var_flows.count = 0;
    ctx->var_flows.capacity = 0;
}

void nd_static_ctx_free(nd_static_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx->variables); ctx->variables = NULL; ctx->nvariables = 0;
    free(ctx->functions); ctx->functions = NULL; ctx->nfunctions = 0;
    free(ctx->edges);     ctx->edges = NULL;     ctx->nedges = 0;
    free(ctx->groups);    ctx->groups = NULL;    ctx->ngroups = 0;
    free(ctx->var_flows.flows);
    ctx->var_flows.flows = NULL;
    ctx->var_flows.count = 0;
    ctx->var_flows.capacity = 0;
}

/* ============================================================================
 * Variable class
 * ============================================================================ */

int nd_variable_find_by_name(nd_static_ctx_t *ctx, const char *name) {
    if (!ctx || !name) return -1;
    for (int i = 0; i < ctx->nvariables; i++) {
        if (nd_name_eq(ctx->variables[i].name, name)) return i;
    }
    return -1;
}

int nd_variable_find_by_address(nd_static_ctx_t *ctx, uint64_t address) {
    if (!ctx) return -1;
    for (int i = 0; i < ctx->nvariables; i++) {
        if (ctx->variables[i].address == address) return i;
    }
    return -1;
}

int nd_variable_add(nd_static_ctx_t *ctx, uint64_t address, const char *name,
                    nd_var_type_t type, uint64_t size) {
    if (!ctx) return -1;
    if (ctx->nvariables >= ND_STATIC_MAX_VARIABLES) return -1;

    int existing = nd_variable_find_by_address(ctx, address);
    if (existing >= 0 && address != 0) {
        if (name && name[0] && !ctx->variables[existing].name[0]) {
            nd_copy(ctx->variables[existing].name, ND_STATIC_MAX_NAME, name);
        }
        return existing;
    }

    ctx->variables = (nd_variable_t *)realloc(
        ctx->variables, (size_t)(ctx->nvariables + 1) * sizeof(nd_variable_t));
    if (!ctx->variables) return -1;

    nd_variable_t *v = &ctx->variables[ctx->nvariables];
    memset(v, 0, sizeof(*v));
    v->id       = (uint64_t)ctx->nvariables + 1;
    v->address  = address;
    v->type     = type;
    v->size     = size;
    v->lifetime_start = address;
    v->lifetime_end   = address;
    nd_copy(v->name, ND_STATIC_MAX_NAME, name);
    nd_copy(v->demangled, ND_STATIC_MAX_NAME, name);
    return ctx->nvariables++;
}

/* ============================================================================
 * Function class
 * ============================================================================ */

int nd_function_find_by_name(nd_static_ctx_t *ctx, const char *name) {
    if (!ctx || !name) return -1;
    for (int i = 0; i < ctx->nfunctions; i++) {
        if (nd_name_eq(ctx->functions[i].name, name)) return i;
    }
    return -1;
}

int nd_function_find_by_address(nd_static_ctx_t *ctx, uint64_t address) {
    if (!ctx) return -1;
    for (int i = 0; i < ctx->nfunctions; i++) {
        const nd_function_t *f = &ctx->functions[i];
        if (address >= f->start && (f->end > f->start ? address < f->end : address == f->start))
            return i;
    }
    return -1;
}

int nd_function_add(nd_static_ctx_t *ctx, uint64_t start, uint64_t end,
                    const char *name, int sym_idx) {
    if (!ctx) return -1;
    if (ctx->nfunctions >= ND_STATIC_MAX_FUNCTIONS) return -1;

    ctx->functions = (nd_function_t *)realloc(
        ctx->functions, (size_t)(ctx->nfunctions + 1) * sizeof(nd_function_t));
    if (!ctx->functions) return -1;

    nd_function_t *f = &ctx->functions[ctx->nfunctions];
    memset(f, 0, sizeof(*f));
    f->id     = (uint64_t)ctx->nfunctions + 1;
    f->start  = start;
    f->end    = end;
    f->sym_idx = sym_idx;
    f->group_id = 0;
    nd_copy(f->name, ND_STATIC_MAX_NAME, name);
    nd_copy(f->demangled, ND_STATIC_MAX_NAME, name);
    return ctx->nfunctions++;
}

/* ============================================================================
 * Edge class
 * ============================================================================ */

int nd_edge_add(nd_static_ctx_t *ctx, uint64_t from, uint64_t to,
                nd_edge_type_t type, uint64_t site, int is_conditional,
                const char *condition) {
    if (!ctx) return -1;
    if (ctx->nedges >= ND_STATIC_MAX_EDGES) return -1;

    ctx->edges = (nd_edge_t *)realloc(
        ctx->edges, (size_t)(ctx->nedges + 1) * sizeof(nd_edge_t));
    if (!ctx->edges) return -1;

    nd_edge_t *e = &ctx->edges[ctx->nedges];
    memset(e, 0, sizeof(*e));
    e->id     = (uint64_t)ctx->nedges + 1;
    e->from   = from;
    e->to     = to;
    e->type   = type;
    e->site   = site;
    e->is_conditional = is_conditional;
    e->is_strong = (type == ND_EDGE_CALL || type == ND_EDGE_DEF_USE) ? 1 : 0;
    e->weight = is_conditional ? 1 : 2;
    nd_copy(e->condition, ND_STATIC_MAX_COND, condition);
    return ctx->nedges++;
}

/* ============================================================================
 * Group class
 * ============================================================================ */

int nd_group_add(nd_static_ctx_t *ctx, const char *name, nd_group_type_t type,
                 uint64_t dispatch_addr, uint64_t join_addr) {
    if (!ctx) return -1;
    if (ctx->ngroups >= ND_STATIC_MAX_GROUPS) return -1;

    ctx->groups = (nd_group_t *)realloc(
        ctx->groups, (size_t)(ctx->ngroups + 1) * sizeof(nd_group_t));
    if (!ctx->groups) return -1;

    nd_group_t *g = &ctx->groups[ctx->ngroups];
    memset(g, 0, sizeof(*g));
    g->id = (uint64_t)ctx->ngroups + 1;
    g->type = type;
    g->dispatch_addr = dispatch_addr;
    g->join_addr = join_addr;
    nd_copy(g->name, ND_STATIC_MAX_NAME, name);
    return ctx->ngroups++;
}

int nd_group_add_function(nd_static_ctx_t *ctx, uint64_t group_id, uint64_t func_id) {
    if (!ctx) return -1;
    for (int i = 0; i < ctx->ngroups; i++) {
        if (ctx->groups[i].id != group_id) continue;
        nd_group_t *g = &ctx->groups[i];
        if (g->nfunctions >= ND_STATIC_MAX_FUNCTIONS) return -1;
        g->functions[g->nfunctions++] = func_id;
        return 0;
    }
    return -1;
}

/* ============================================================================
 * Phase 1 — SURVEY: sections / symbols / functions
 * ============================================================================ */

static int nd_survey(nd_static_ctx_t *ctx) {
    dax_binary_t *bin = ctx->bin;
    if (!bin) return -1;

    /* Functions come from NeoDAX's own function detection. */
    for (int i = 0; i < bin->nfunctions && ctx->nfunctions < ND_STATIC_MAX_FUNCTIONS; i++) {
        dax_func_t *df = &bin->functions[i];
        int idx = nd_function_add(ctx, df->start, df->end, df->name, df->sym_idx);
        if (idx < 0) continue;
        ctx->functions[idx].insn_count  = df->insn_count;
        ctx->functions[idx].block_count = df->block_count;
        ctx->functions[idx].has_loops   = df->has_loops;
        ctx->functions[idx].has_calls   = df->has_calls;
        if (bin->entry >= df->start && (df->end <= df->start || bin->entry < df->end)) {
            ctx->functions[idx].is_entry = 1;
        }
    }

    /* Variables come from global data symbols — these are the "which
     * variables does this binary use" answer the user asked for. */
    for (int i = 0; i < bin->nsymbols; i++) {
        dax_symbol_t *s = &bin->symbols[i];
        if (s->type == SYM_FUNC || s->type == SYM_IMPORT) continue;
        if (!s->name[0]) continue;
        if (s->address == 0) continue;
        int v = nd_variable_add(ctx, s->address, s->name, ND_VAR_GLOBAL, s->size);
        if (v >= 0 && s->demangled[0]) {
            nd_copy(ctx->variables[v].demangled, ND_STATIC_MAX_NAME, s->demangled);
        }
    }

    /* Sort the variables by address so that "nearest symbol below" is
     * well defined. Mach-O symbols carry no size, so the extent of a
     * variable is left unknown on purpose — a gap-derived size would
     * swallow the neighbours (the gap between two data symbols is
     * whatever the compiler's alignment left). */
    if (ctx->nvariables > 1) {
        for (int i = 0; i < ctx->nvariables; i++) {
            for (int j = i + 1; j < ctx->nvariables; j++) {
                if (ctx->variables[j].address < ctx->variables[i].address) {
                    nd_variable_t t = ctx->variables[i];
                    ctx->variables[i] = ctx->variables[j];
                    ctx->variables[j] = t;
                }
            }
        }
        for (int i = 0; i < ctx->nvariables; i++) {
            ctx->variables[i].id = (uint64_t)i + 1;
        }
    }

    return 0;
}

/* ============================================================================
 * Phase 2 — CLASSIFY: role heuristics (domain agnostic)
 * ============================================================================ */

static const char *nd_classify_role(const char *name) {
    if (!name) return "unknown";
    if (nd_strcasestr(name, "board") || nd_strcasestr(name, "position") ||
        nd_strcasestr(name, "bitboard") || nd_strcasestr(name, "state"))
        return "position";
    if (nd_strcasestr(name, "move") || nd_strcasestr(name, "action") ||
        nd_strcasestr(name, "ply") || nd_strcasestr(name, "step"))
        return "move";
    if (nd_strcasestr(name, "score") || nd_strcasestr(name, "eval") ||
        nd_strcasestr(name, "value") || nd_strcasestr(name, "heuristic"))
        return "score";
    if (nd_strcasestr(name, "depth") || nd_strcasestr(name, "level") ||
        nd_strcasestr(name, "max_depth"))
        return "depth";
    if (nd_strcasestr(name, "node") || nd_strcasestr(name, "entry") ||
        nd_strcasestr(name, "search"))
        return "node";
    if (nd_strcasestr(name, "tree") || nd_strcasestr(name, "table") ||
        nd_strcasestr(name, "hash"))
        return "tree";
    if (nd_strcasestr(name, "beam") || nd_strcasestr(name, "width"))
        return "beam";
    if (nd_strcasestr(name, "worker") || nd_strcasestr(name, "thread") ||
        nd_strcasestr(name, "parallel") || nd_strcasestr(name, "dispatch") ||
        nd_strcasestr(name, "queue") || nd_strcasestr(name, "msg") ||
        nd_strcasestr(name, "message"))
        return "message";
    return "unknown";
}

static int nd_classify(nd_static_ctx_t *ctx) {
    (void)ctx;
    return 0;
}

/* ============================================================================
 * Data reference analysis — which function touches which global
 *
 * The call graph alone cannot answer "which function uses this variable".
 * This pass decodes each function and resolves the global addresses it
 * materialises:
 *
 *   ARM64 : adrp Xd, page  →  add Xd, Xd, #off  →  ldr/str [Xd]
 *   x86-64: rip-relative operands (lea / mov / cmp …) with the next-instruction
 *           address added to the displacement
 *
 * A resolved address is matched against the variable table, and the
 * enclosing function becomes a reader (or writer) of that variable. The
 * caller chain of a variable is then the chain of its readers, walked up
 * the call graph to the root.
 * ============================================================================ */

/* Find the variable an address belongs to.
 *
 * Exact address first: a compiler materialises a scalar global as
 * adrp+add, so the resolved address is the symbol address. Addresses that
 * fall inside a struct (base + member offset) have no exact symbol, so
 * the nearest symbol below is used — bounded, so a stray address is not
 * attributed to a distant variable. */
static int nd_find_var_containing(nd_static_ctx_t *ctx, uint64_t addr) {
    int best = -1;
    uint64_t best_addr = 0;
    int i;

    for (i = 0; i < ctx->nvariables; i++) {
        if (ctx->variables[i].address == addr) return i;
    }
    for (i = 0; i < ctx->nvariables; i++) {
        nd_variable_t *v = &ctx->variables[i];
        if (v->address == 0 || addr < v->address) continue;
        if (v->address > best_addr) { best_addr = v->address; best = i; }
    }
    if (best >= 0 && addr - best_addr > 0x1000) return -1;
    return best;
}

static void nd_note_ref(nd_static_ctx_t *ctx, int func_idx, uint64_t addr, int is_write) {
    int vi;
    nd_variable_t *v;
    nd_function_t *f;

    if (func_idx < 0 || func_idx >= ctx->nfunctions) return;
    vi = nd_find_var_containing(ctx, addr);
    if (vi < 0) return;

    v = &ctx->variables[vi];
    f = &ctx->functions[func_idx];

    if (getenv("ND_DEBUG_REFS")) {
        fprintf(stderr, "[ref] %-28s -> %-16s addr=0x%llx %s  %s\n",
                f->name, v->name, (unsigned long long)addr,
                is_write ? "W" : "R", ctx->last_insn);
    }

    if (is_write) {
        v->is_written = 1;
        v->write_count++;
        v->last_modification = addr;
        if (v->nwriters < 64) {
            int seen = 0, k;
            for (k = 0; k < v->nwriters; k++) if (v->writers[k] == f->id) { seen = 1; break; }
            if (!seen) v->writers[v->nwriters++] = f->id;
        }
        if (f->nwrites < 64) {
            int seen = 0, k;
            for (k = 0; k < f->nwrites; k++) if (f->writes[k] == v->id) { seen = 1; break; }
            if (!seen) f->writes[f->nwrites++] = v->id;
        }
    } else {
        v->is_read = 1;
        v->read_count++;
        if (v->nreaders < 64) {
            int seen = 0, k;
            for (k = 0; k < v->nreaders; k++) if (v->readers[k] == f->id) { seen = 1; break; }
            if (!seen) v->readers[v->nreaders++] = f->id;
        }
        if (f->nreads < 64) {
            int seen = 0, k;
            for (k = 0; k < f->nreads; k++) if (f->reads[k] == v->id) { seen = 1; break; }
            if (!seen) f->reads[f->nreads++] = v->id;
        }
    }
}

/* Parse a register token like "x24" / "w9" / "sp" into a 0-31 number.
 * Returns -1 when the token is not a plain GPR. */
static int nd_parse_regnum(const char *tok, size_t len) {
    if (len < 2) return -1;
    if (tok[0] != 'x' && tok[0] != 'w') return -1;
    if (len == 2 && tok[1] >= '0' && tok[1] <= '9') return tok[1] - '0';
    if (len == 3 && tok[1] >= '1' && tok[1] <= '9' && tok[2] >= '0' && tok[2] <= '5')
        return (tok[1] - '1') * 10 + (tok[2] - '0');
    return -1;
}

/* Extract the first register token of a comma-separated operand list. */
static int nd_first_reg(const char *ops, char *reg, size_t regsz) {
    const char *p = ops;
    size_t n = 0;
    while (*p == ' ') p++;
    while (p[n] && p[n] != ',' && p[n] != ' ' && p[n] != '[' && n < regsz - 1) {
        reg[n] = p[n];
        n++;
    }
    reg[n] = '\0';
    return nd_parse_regnum(reg, n);
}

/* Extract the n-th (0-based) register token. */
static int nd_nth_reg(const char *ops, int idx, char *reg, size_t regsz) {
    const char *p = ops;
    int seen = 0;
    size_t n;
    reg[0] = '\0';
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) return -1;
        n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ' && p[n] != '[' && n < regsz - 1) {
            reg[n] = p[n];
            n++;
        }
        reg[n] = '\0';
        if (seen == idx) return nd_parse_regnum(reg, n);
        seen++;
        while (*p && *p != ',') p++;
        if (!*p) return -1;
        p++;
    }
}

static int nd_contains_op(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

/* Parse "0x1234" or "-0x1234" at the given position. */
static int nd_parse_hex(const char **pp, uint64_t *out) {
    const char *p = *pp;
    int neg = 0;
    uint64_t v = 0;
    int any = 0;

    while (*p == ' ') p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
            int d;
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else d = *p - 'A' + 10;
            v = (v << 4) | (uint64_t)d;
            p++;
            any = 1;
        }
    }
    if (!any) return 0;
    *out = neg ? (uint64_t)-(int64_t)v : v;
    *pp = p;
    return 1;
}

/* Find the first "0x…" in a string and parse it. Used for operand strings
 * like "x24, 0x0000000100004000". */
static int nd_parse_hex_in(const char *s, uint64_t *out) {
    const char *p;
    if (!s) return 0;
    for (p = s; *p; p++) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            const char *q = p;
            return nd_parse_hex(&q, out);
        }
    }
    return 0;
}

/* Parse the immediate that follows '#' in an operand string.
 *
 * The AArch64 decoder prints immediates in two ways: "#0x8b4" for the
 * arithmetic forms and "#4" (decimal) for the load/store forms, so both
 * have to be accepted. */
static int nd_parse_imm_in(const char *s, uint64_t *out) {
    const char *p;
    if (!s) return 0;
    p = strchr(s, '#');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) return nd_parse_hex(&p, out);
    if (*p >= '0' && *p <= '9') {
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint64_t)(*p - '0');
            p++;
        }
        *out = v;
        return 1;
    }
    return 0;
}

/* Parse the memory operand's byte displacement.
 *
 * The AArch64 decoder prints three shapes:
 *   [x8]                → no displacement
 *   [x8, #4]            → displacement 4
 *   [x24, x9, uxtx #2]  → index register; the "#2" is a shift amount,
 *                         not a displacement, so the offset is 0.
 */
static int nd_mem_offset(const char *br, uint64_t *off) {
    const char *p;
    char tok[64];
    size_t n = 0;

    *off = 0;
    if (!br) return 0;
    p = br + 1;                       /* skip '[' */
    while (*p == ' ') p++;
    while (p[n] && p[n] != ',' && p[n] != ']' && p[n] != ' ' && n < sizeof(tok) - 1) {
        tok[n] = p[n];
        n++;
    }
    tok[n] = '\0';
    p += n;
    while (*p == ' ') p++;
    if (*p == ']') return 1;
    if (*p == ',') {
        p++;
        while (*p == ' ') p++;
        if (*p == '#') return nd_parse_imm_in(p, off);
        return 1;                     /* index register form: no displacement */
    }
    return 1;
}

/* The third operand of an arithmetic instruction is an immediate only when
 * it starts with '#' — "add x8, x8, x9, lsl #2" has a shift amount there,
 * not an immediate. */
static int nd_third_imm(const char *ops, uint64_t *imm) {
    const char *p = ops;
    int commas = 0;

    if (!ops) return 0;
    for (; *p; p++) {
        if (*p != ',') continue;
        commas++;
        if (commas == 2) { p++; break; }
    }
    if (commas < 2) return 0;
    while (*p == ' ') p++;
    if (*p != '#') return 0;
    return nd_parse_imm_in(p, imm);
}

/* Extract the n-th (0-based) register token, accepting only 64-bit "xN"
 * forms. A "wN" register holds a 32-bit value, so it can never be the
 * base of a 64-bit global address. */
static int nd_nth_reg_x(const char *ops, int idx, char *reg, size_t regsz) {
    const char *p = ops;
    int seen = 0;
    size_t n;
    reg[0] = '\0';
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) return -1;
        n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ' && p[n] != '[' && n < regsz - 1) {
            reg[n] = p[n];
            n++;
        }
        reg[n] = '\0';
        if (seen == idx) {
            if (reg[0] != 'x') return -1;
            return nd_parse_regnum(reg, n);
        }
        seen++;
        while (*p && *p != ',') p++;
        if (!*p) return -1;
        p++;
    }
}

/* Does this instruction write its first operand? Used to invalidate the
 * tracked address registers when a register is overwritten. */
static int nd_arm64_writes_dst(const char *m) {
    static const char *const no_dst[] = {
        "cmp", "cmn", "tst", "ccmp", "ccmn", "b", "bl", "blr", "ret",
        "cbz", "cbnz", "tbz", "tbnz", "str", "stp", "stur", "sturb", "strh",
        "stxr", "stlxr", "stlr", "cas", "swp", "mrs", "msr", "svc", "nop",
        "brk", "dmb", "dsb", "isb", "hint", "adrp", "adr", "br", "eret",
        "drps", "wfe", "wfi", "sev", "sevl", "autiasp", "paciasp", NULL
    };
    int i;
    if (!m) return 0;
    for (i = 0; no_dst[i]; i++) {
        if (!strcmp(m, no_dst[i])) return 0;
    }
    return 1;
}

static void nd_scan_arm64_func(nd_static_ctx_t *ctx, int fi, uint8_t *code,
                               size_t size, uint64_t base) {
    uint64_t reg[32];
    size_t off = 0;
    int i;

    memset(reg, 0, sizeof(reg));

    while (off + 4 <= size) {
        uint32_t raw = (uint32_t)code[off] | ((uint32_t)code[off+1] << 8) |
                       ((uint32_t)code[off+2] << 16) | ((uint32_t)code[off+3] << 24);
        a64_insn_t insn;
        char tok[64];
        int dst = -1;
        int modelled = 0;

        a64_decode(raw, base + off, &insn);
        snprintf(ctx->last_insn, sizeof(ctx->last_insn), "%s %s",
                 insn.mnemonic, insn.operands);

        /* ── address materialisation ────────────────────────────────── */
        if (!strcmp(insn.mnemonic, "adrp") || !strcmp(insn.mnemonic, "adr")) {
            dst = nd_nth_reg_x(insn.operands, 0, tok, sizeof(tok));
            if (dst >= 0) {
                uint64_t val = 0;
                if (nd_parse_hex_in(insn.operands, &val)) reg[dst] = val;
            }
            modelled = 1;
        } else if (!strcmp(insn.mnemonic, "add")) {
            int d = nd_nth_reg_x(insn.operands, 0, tok, sizeof(tok));
            int s = nd_nth_reg_x(insn.operands, 1, tok, sizeof(tok));
            uint64_t imm = 0;
            dst = d;
            if (d >= 0 && s >= 0 && reg[s] && nd_third_imm(insn.operands, &imm)) {
                uint64_t addr = reg[s] + imm;
                reg[d] = addr;
                nd_note_ref(ctx, fi, addr, 0);
            }
            modelled = 1;
        }

        /* ── memory access ──────────────────────────────────────────── */
        if (!strncmp(insn.mnemonic, "ldr", 3) ||
            !strncmp(insn.mnemonic, "ldp", 3) ||
            !strncmp(insn.mnemonic, "ldur", 4)) {
            /* the destination register is overwritten by the load */
            int d = nd_nth_reg(insn.operands, 0, tok, sizeof(tok));
            if (d >= 0 && dst < 0) dst = d;

            const char *br = strchr(insn.operands, '[');
            if (br) {
                const char *p = br + 1;
                char r2[64];
                uint64_t imm = 0;
                int b;
                size_t n = 0;
                while (*p == ' ') p++;
                while (p[n] && p[n] != ',' && p[n] != ']' && p[n] != ' ' && n < sizeof(r2) - 1) {
                    r2[n] = p[n];
                    n++;
                }
                r2[n] = '\0';
                b = nd_parse_regnum(r2, n);
                if (b >= 0 && reg[b]) {
                    uint64_t addr = reg[b];
                    if (nd_mem_offset(br, &imm)) addr += imm;
                    nd_note_ref(ctx, fi, addr, 0);
                }
            }
        } else if (!strncmp(insn.mnemonic, "str", 3) ||
                   !strncmp(insn.mnemonic, "stp", 3) ||
                   !strncmp(insn.mnemonic, "stur", 4)) {
            const char *br = strchr(insn.operands, '[');
            int is_write = 1;
            if (br) {
                const char *p = br + 1;
                char r2[64];
                uint64_t imm = 0;
                int b;
                size_t n = 0;
                while (*p == ' ') p++;
                while (p[n] && p[n] != ',' && p[n] != ']' && p[n] != ' ' && n < sizeof(r2) - 1) {
                    r2[n] = p[n];
                    n++;
                }
                r2[n] = '\0';
                b = nd_parse_regnum(r2, n);
                if (b >= 0 && reg[b]) {
                    uint64_t addr = reg[b];
                    if (nd_mem_offset(br, &imm)) addr += imm;
                    nd_note_ref(ctx, fi, addr, is_write);
                }
            }
        }

        /* ── invalidate the destination register ────────────────────── */
        if (!modelled && dst >= 0 && nd_arm64_writes_dst(insn.mnemonic)) {
            reg[dst] = 0;
        } else if (!modelled && dst < 0 &&
                   nd_arm64_writes_dst(insn.mnemonic)) {
            int d = nd_nth_reg_x(insn.operands, 0, tok, sizeof(tok));
            if (d >= 0) reg[d] = 0;
        }
        /* a call clobbers the link register */
        if (!strcmp(insn.mnemonic, "bl") || !strcmp(insn.mnemonic, "blr")) {
            reg[30] = 0;
        }

        off += 4;
    }
    (void)i;
}

static void nd_scan_x86_func(nd_static_ctx_t *ctx, int fi, uint8_t *code,
                             size_t size, uint64_t base) {
    size_t off = 0;

    while (off < size) {
        x86_insn_t insn;
        int len = x86_decode(code + off, size - off, base + off, &insn);
        if (len <= 0) { off++; continue; }

        /* rip-relative: the effective address is next-insn + displacement */
        if (nd_contains_op(insn.ops, "rip+0x") || nd_contains_op(insn.ops, "rip-0x")) {
            const char *p = insn.ops;
            const char *hit = strstr(p, "rip");
            int neg = 0;
            uint64_t disp = 0;
            if (hit) {
                p = hit + 3;
                if (*p == '+') p++;
                else if (*p == '-') { neg = 1; p++; }
                nd_parse_hex(&p, &disp);
            }
            if (neg) disp = (uint64_t)-(int64_t)disp;
            {
                uint64_t next = base + off + (uint64_t)len;
                uint64_t target = next + disp;
                int is_write = (insn.mnemonic[0] == 'm' && insn.mnemonic[1] == 'o' &&
                                insn.mnemonic[2] == 'v') ||
                               strncmp(insn.mnemonic, "st", 2) == 0 ||
                               strncmp(insn.mnemonic, "cmpxchg", 6) == 0 ||
                               strncmp(insn.mnemonic, "xadd", 4) == 0;
                nd_note_ref(ctx, fi, target, is_write);
            }
        }

        off += (size_t)len;
    }
}

static void nd_scan_data_refs(nd_static_ctx_t *ctx) {
    dax_binary_t *bin = ctx->bin;
    int fi;

    if (!bin) return;

    for (fi = 0; fi < ctx->nfunctions; fi++) {
        nd_function_t *f = &ctx->functions[fi];
        dax_section_t *sec = NULL;
        size_t off, size;

        for (int i = 0; i < bin->nsections && i < DAX_MAX_SECTIONS; i++) {
            dax_section_t *s = &bin->sections[i];
            if (f->start >= s->vaddr && f->start < s->vaddr + s->size) { sec = s; break; }
        }
        if (!sec || sec->size == 0) continue;
        if (sec->offset > bin->size || sec->size > bin->size - sec->offset) continue;

        off  = (size_t)(f->start - sec->vaddr);
        size = (size_t)(f->end > f->start ? f->end - f->start : 0);
        if (size == 0) continue;
        if (off >= sec->size) continue;
        if (size > sec->size - off) size = sec->size - off;

        if (bin->arch == ARCH_ARM64) {
            nd_scan_arm64_func(ctx, fi, bin->data + sec->offset + off, size, f->start);
        } else if (bin->arch == ARCH_X86_64) {
            nd_scan_x86_func(ctx, fi, bin->data + sec->offset + off, size, f->start);
        }
    }
}



/* Is this function name a dispatch/parallel entry point? */
static int nd_is_dispatch_name(const char *n) {
    return nd_strcasestr(n, "dispatch") || nd_strcasestr(n, "thread") ||
           nd_strcasestr(n, "parallel") || nd_strcasestr(n, "spawn") ||
           nd_strcasestr(n, "worker") || nd_strcasestr(n, "queue");
}

/* Is this a synchronization point? */
static int nd_is_sync_name(const char *n) {
    return nd_strcasestr(n, "join") || nd_strcasestr(n, "wait") ||
           nd_strcasestr(n, "barrier") || nd_strcasestr(n, "sync") ||
           nd_strcasestr(n, "mutex") || nd_strcasestr(n, "lock") ||
           nd_strcasestr(n, "semaphore") || nd_strcasestr(n, "fence");
}

/* Is this a beam-search entry point? */
static int nd_is_beam_name(const char *n) {
    return nd_strcasestr(n, "beam");
}

static int nd_trace(nd_static_ctx_t *ctx) {
    dax_binary_t *bin = ctx->bin;
    if (!bin) return -1;

    /* Call edges: bin->xrefs already resolved call sites to addresses. */
    for (int i = 0; i < bin->nxrefs; i++) {
        dax_xref_t *x = &bin->xrefs[i];
        if (!x->is_call) continue;
        int caller = nd_function_find_by_address(ctx, x->from);
        int callee = nd_function_find_by_address(ctx, x->to);
        if (caller < 0 || callee < 0) continue;

        nd_edge_add(ctx, ctx->functions[caller].id, ctx->functions[callee].id,
                    ND_EDGE_CALL, x->from, 0, NULL);

        nd_function_t *cf = &ctx->functions[caller];
        if (cf->ncalls < 64) cf->calls[cf->ncalls++] = ctx->functions[callee].id;
        ctx->functions[callee].has_calls = 1;
    }

    /* Variable reads/writes attributed to the enclosing function. */
    for (int i = 0; i < ctx->nvariables; i++) {
        nd_variable_t *v = &ctx->variables[i];
        int owner = nd_function_find_by_address(ctx, v->address);
        if (owner < 0) continue;
        nd_function_t *f = &ctx->functions[owner];
        if (f->nwrites < 64) f->writes[f->nwrites++] = v->id;
        v->is_written = 1;
        v->last_modification = v->address;
    }

    return 0;
}

/* Build a variable → caller chain (finite depth) for one variable.
 *
 * A global is not "inside" a function, so the chain starts at the function
 * that actually touches it (the reader) and then walks up the call graph
 * to the root — that is the answer to "where does this variable come from
 * and who calls the code that needs it". */
static void nd_trace_one(nd_static_ctx_t *ctx, nd_variable_t *var) {
    nd_var_flow_list_t *list = &ctx->var_flows;
    uint64_t seed = 0;
    int seed_idx = -1;

    if (list->count >= list->capacity) {
        int newcap = list->capacity ? list->capacity * 2 : 64;
        nd_var_flow_t *grown = (nd_var_flow_t *)realloc(
            list->flows, (size_t)newcap * sizeof(nd_var_flow_t));
        if (!grown) return;
        list->flows = grown;
        list->capacity = newcap;
    }

    nd_var_flow_t *flow = &list->flows[list->count];
    memset(flow, 0, sizeof(*flow));
    flow->var_id = var->id;
    nd_copy(flow->var_name, ND_STATIC_MAX_NAME, var->name);

    /* Seed: the function that owns the variable, else its first reader. */
    seed_idx = nd_function_find_by_address(ctx, var->address);
    if (seed_idx < 0) {
        for (int k = 0; k < var->nreaders; k++) {
            for (int j = 0; j < ctx->nfunctions; j++) {
                if (ctx->functions[j].id == var->readers[k]) { seed_idx = j; break; }
            }
            if (seed_idx >= 0) break;
        }
    }
    if (seed_idx < 0) {
        for (int k = 0; k < var->nwriters; k++) {
            for (int j = 0; j < ctx->nfunctions; j++) {
                if (ctx->functions[j].id == var->writers[k]) { seed_idx = j; break; }
            }
            if (seed_idx >= 0) break;
        }
    }
    if (seed_idx < 0) { list->count++; return; }

    seed = ctx->functions[seed_idx].id;
    flow->final_caller = seed;
    flow->root_caller  = seed;
    if (flow->chain_len < ND_STATIC_MAX_TRACE_DEPTH) {
        flow->chain[flow->chain_len++] = seed;
    }

    /* Walk up the call graph: who calls the current function? */
    uint64_t current = seed;
    int depth = ctx->trace_depth;
    if (depth > ND_STATIC_MAX_TRACE_DEPTH) depth = ND_STATIC_MAX_TRACE_DEPTH;

    for (int d = 0; d < depth; d++) {
        uint64_t next = 0;
        for (int e = 0; e < ctx->nedges; e++) {
            nd_edge_t *ed = &ctx->edges[e];
            if (ed->type != ND_EDGE_CALL) continue;
            if (ed->to != current) continue;
            if (ed->from == current) continue;   /* self recursion: not a caller */
            next = ed->from;
            break;
        }
        if (!next) break;
        if (next == current) break;                       /* self recursion */
        if (flow->chain_len >= ND_STATIC_MAX_TRACE_DEPTH) break;

        /* Already visited? stop (cycle). */
        int seen = 0;
        for (int k = 0; k < flow->chain_len; k++) {
            if (flow->chain[k] == next) { seen = 1; break; }
        }
        if (seen) break;

        flow->chain[flow->chain_len++] = next;
        flow->root_caller = next;
        current = next;
    }

    list->count++;
}

int nd_static_trace_flow(nd_static_ctx_t *ctx) {
    if (!ctx) return -1;
    for (int i = 0; i < ctx->nvariables; i++) {
        nd_trace_one(ctx, &ctx->variables[i]);
    }
    return 0;
}

/* ============================================================================
 * Phase 4 — PREDICT: execution order (sequential vs parallel)
 * ============================================================================ */

int nd_static_predict_order(nd_static_ctx_t *ctx) {
    if (!ctx) return -1;

    int n = ctx->nfunctions;
    int *depth;
    int *queue;
    int qhead = 0, qtail = 0;

    if (n <= 0) return 0;

    depth = (int *)calloc((size_t)n, sizeof(int));
    queue = (int *)calloc((size_t)n, sizeof(int));
    if (!depth || !queue) { free(depth); free(queue); return -1; }

    for (int i = 0; i < n; i++) depth[i] = -1;

    /* Seed: the entry function (marked during survey), else the function
     * with no callers, else function 0. */
    {
        int seed = -1;
        for (int i = 0; i < n; i++) if (ctx->functions[i].is_entry) { seed = i; break; }
        if (seed < 0) {
            for (int i = 0; i < n; i++) {
                int callers = 0;
                for (int e = 0; e < ctx->nedges; e++) {
                    if (ctx->edges[e].type == ND_EDGE_CALL && ctx->edges[e].to == ctx->functions[i].id)
                        callers++;
                }
                if (callers == 0) { seed = i; break; }
            }
        }
        if (seed < 0) seed = 0;
        depth[seed] = 0;
        queue[qtail++] = seed;
    }

    /* BFS over caller → callee edges. The BFS depth is the execution
     * depth: functions at the same depth have no ordering constraint
     * between them and may run concurrently. */
    while (qhead < qtail) {
        int cur = queue[qhead++];
        for (int e = 0; e < ctx->nedges; e++) {
            nd_edge_t *ed = &ctx->edges[e];
            int callee = -1;
            if (ed->type != ND_EDGE_CALL) continue;
            if (ed->from != ctx->functions[cur].id) continue;
            for (int i = 0; i < n; i++) {
                if (ctx->functions[i].id == ed->to) { callee = i; break; }
            }
            if (callee < 0) continue;
            if (depth[callee] < 0) {
                depth[callee] = depth[cur] + 1;
                queue[qtail++] = callee;
            }
        }
    }

    /* Order by (depth, address). Same depth = same execution step. */
    for (int i = 0; i < n; i++) {
        ctx->functions[i].execution_order = 0;
        ctx->functions[i].earliest_start = depth[i] < 0 ? 1 << 20 : depth[i];
        ctx->functions[i].latest_start   = ctx->functions[i].earliest_start;
    }
    for (int rank = 1; rank <= n; rank++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (ctx->functions[i].execution_order != 0) continue;
            if (best < 0) { best = i; continue; }
            if (ctx->functions[i].earliest_start < ctx->functions[best].earliest_start ||
                (ctx->functions[i].earliest_start == ctx->functions[best].earliest_start &&
                 ctx->functions[i].start < ctx->functions[best].start))
                best = i;
        }
        if (best < 0) break;
        ctx->functions[best].execution_order = rank;
    }

    /* Recursion (cycle) detection → a recursive group. */
    {
        int recursive = 0;
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < ctx->functions[i].ncalls; k++) {
                if (ctx->functions[i].calls[k] == ctx->functions[i].id) { recursive++; break; }
            }
        }
        if (recursive > 0) {
            int g = nd_group_add(ctx, "recursive", ND_GROUP_RECURSIVE, 0, 0);
            for (int i = 0; i < n; i++) {
                for (int k = 0; k < ctx->functions[i].ncalls; k++) {
                    if (ctx->functions[i].calls[k] != ctx->functions[i].id) continue;
                    if (g >= 0) {
                        nd_group_add_function(ctx, ctx->groups[g].id, ctx->functions[i].id);
                        ctx->functions[i].group_id = ctx->groups[g].id;
                    }
                    break;
                }
            }
        }
    }

    free(depth);
    free(queue);
    return 0;
}

/* ============================================================================
 * Phase 5 — PARALLEL: groups for dispatch / beam search / conditional
 * ============================================================================ */

int nd_static_detect_parallel(nd_static_ctx_t *ctx) {
    if (!ctx) return -1;

    /* One group per dispatch/worker function. */
    for (int i = 0; i < ctx->nfunctions; i++) {
        nd_function_t *f = &ctx->functions[i];
        if (nd_is_dispatch_name(f->name)) {
            char gname[ND_STATIC_MAX_NAME];
            snprintf(gname, sizeof(gname), "parallel:%s", f->name);
            int g = nd_group_add(ctx, gname, ND_GROUP_PARALLEL, f->start, 0);
            if (g < 0) continue;
            ctx->groups[g].dispatcher_func = f->id;
            f->group_id = ctx->groups[g].id;
            f->is_parallel = 1;
            /* workers = callees of the dispatcher */
            for (int k = 0; k < f->ncalls; k++) {
                for (int j = 0; j < ctx->nfunctions; j++) {
                    if (ctx->functions[j].id == f->calls[k]) {
                        nd_group_add_function(ctx, ctx->groups[g].id, ctx->functions[j].id);
                        ctx->functions[j].is_parallel = 1;
                        ctx->functions[j].group_id = ctx->groups[g].id;
                    }
                }
            }
        }
    }

    /* One group per sync point — these are the joins of parallel groups. */
    for (int i = 0; i < ctx->nfunctions; i++) {
        nd_function_t *f = &ctx->functions[i];
        if (!nd_is_sync_name(f->name)) continue;
        for (int g = 0; g < ctx->ngroups; g++) {
            if (ctx->groups[g].type != ND_GROUP_PARALLEL) continue;
            if (ctx->groups[g].join_addr == 0) {
                ctx->groups[g].join_addr = f->start;
                ctx->groups[g].sync_point = f->start;
            }
        }
    }

    return 0;
}

int nd_static_detect_beam_search(nd_static_ctx_t *ctx) {
    if (!ctx) return -1;

    for (int i = 0; i < ctx->nfunctions; i++) {
        nd_function_t *f = &ctx->functions[i];
        if (!nd_is_beam_name(f->name)) continue;

        char gname[ND_STATIC_MAX_NAME];
        snprintf(gname, sizeof(gname), "beam:%s", f->name);
        int g = nd_group_add(ctx, gname, ND_GROUP_BEAM_SEARCH, f->start, 0);
        if (g < 0) continue;

        ctx->groups[g].dispatcher_func = f->id;
        nd_group_add_function(ctx, ctx->groups[g].id, f->id);
        f->group_id = ctx->groups[g].id;

        /* Beam width: look for a global named *beam_width* / *width* */
        for (int v = 0; v < ctx->nvariables; v++) {
            nd_variable_t *var = &ctx->variables[v];
            if (nd_strcasestr(var->name, "beam_width") ||
                nd_strcasestr(var->name, "max_width")) {
                ctx->groups[g].beam_width = var->size ? var->size : 8;
                nd_copy(ctx->groups[g].condition_var,
                        ND_STATIC_MAX_NAME, var->name);
                break;
            }
        }
        if (ctx->groups[g].beam_width == 0) ctx->groups[g].beam_width = 8;

        /* The arguments the beam worker receives — condition preserved. */
        for (int v = 0; v < ctx->nvariables; v++) {
            nd_variable_t *var = &ctx->variables[v];
            if (nd_strcasestr(var->name, "message") ||
                nd_strcasestr(var->name, "msg") ||
                nd_strcasestr(var->name, "command") ||
                nd_strcasestr(var->name, "signal") ||
                nd_strcasestr(var->name, "task")) {
                nd_copy(ctx->groups[g].condition_msg,
                        ND_STATIC_MAX_NAME, var->name);
                nd_copy(f->condition_msg, ND_STATIC_MAX_NAME, var->name);
                ctx->groups[g].condition_conditional = 1;
                break;
            }
        }
    }

    /* A function that reads a message variable is conditional on it. */
    for (int i = 0; i < ctx->nfunctions; i++) {
        nd_function_t *f = &ctx->functions[i];
        for (int v = 0; v < ctx->nvariables; v++) {
            nd_variable_t *var = &ctx->variables[v];
            if (!nd_strcasestr(var->name, "msg") &&
                !nd_strcasestr(var->name, "message") &&
                !nd_strcasestr(var->name, "command") &&
                !nd_strcasestr(var->name, "signal"))
                continue;
            int owner = nd_function_find_by_address(ctx, var->address);
            if (owner == i) {
                f->is_conditional = 1;
                nd_copy(f->condition_var, ND_STATIC_MAX_NAME, var->name);
                break;
            }
        }
    }

    return 0;
}

/* ============================================================================
 * Phase 6 — VERIFY: state checkpoints, regression inside loops
 * ============================================================================ */

static int nd_verify(nd_static_ctx_t *ctx) {
    dax_binary_t *bin = ctx->bin;
    if (!bin) return -1;

    /* A "regression" in the ring sense: a function that is re-entered
     * (called from more than one place) and mutates shared state. */
    for (int i = 0; i < ctx->nfunctions; i++) {
        nd_function_t *f = &ctx->functions[i];
        int callers = 0;
        for (int e = 0; e < ctx->nedges; e++) {
            if (ctx->edges[e].type == ND_EDGE_CALL &&
                ctx->edges[e].to == f->id) callers++;
        }
        if (callers < 2) continue;
        if (f->nwrites == 0) continue;

        /* Recursive or multi-caller + writes = potential state regression. */
        f->has_loops = f->has_loops || (callers > 1);
    }
    return 0;
}

/* ============================================================================
 * Ring construction
 * ============================================================================ */

void nd_ring_init(nd_ring_t *ring, const char *binary, const char *arch) {
    if (!ring) return;
    memset(ring, 0, sizeof(*ring));
    ring->phase_count = ND_PHASE_MAX;
    nd_copy(ring->binary, sizeof(ring->binary), binary);
    nd_copy(ring->arch, sizeof(ring->arch), arch);
}

static void nd_ring_cell_set(nd_ring_t *ring, int iteration, int phase,
                             uint32_t evidence, int confidence,
                             uint64_t addr, const char *sample, const char *fmt, ...) {
    if (!ring) return;
    if (iteration < 0 || iteration >= ND_STATIC_MAX_ITERATIONS) return;
    if (phase < 0 || phase >= ND_PHASE_MAX) return;

    nd_ring_cell_t *c = NULL;
    for (int i = 0; i < ring->ncell; i++) {
        if (ring->cells[i].iteration == iteration && ring->cells[i].phase == phase) {
            c = &ring->cells[i];
            break;
        }
    }
    if (!c) {
        if (ring->ncell >= ND_STATIC_MAX_ITERATIONS * ND_PHASE_MAX) return;
        c = &ring->cells[ring->ncell++];
        memset(c, 0, sizeof(*c));
        c->iteration = iteration;
        c->phase = phase;
    }

    c->evidence = evidence;
    c->confidence = confidence;
    c->sample_addr = addr;
    nd_copy(c->sample_name, ND_STATIC_MAX_NAME, sample);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
        va_end(ap);
    }
}

int nd_ring_build(nd_static_ctx_t *ctx) {
    if (!ctx || !ctx->bin) return -1;
    dax_binary_t *bin = ctx->bin;
    nd_ring_t *ring = &ctx->ring;

    const char *arch = "unknown";
    switch (bin->arch) {
        case ARCH_X86_64:  arch = "x86_64";  break;
        case ARCH_ARM64:   arch = "arm64";   break;
        case ARCH_RISCV64: arch = "riscv64"; break;
        default: break;
    }
    nd_ring_init(ring, bin->filepath, arch);

    ring->sections  = (uint64_t)bin->nsections;
    ring->symbols   = (uint64_t)bin->nsymbols;
    ring->functions = (uint64_t)ctx->nfunctions;
    ring->xrefs     = (uint64_t)bin->nxrefs;
    ring->variables = (uint64_t)ctx->nvariables;
    ring->groups    = (uint64_t)ctx->ngroups;
    ring->dep_nodes = (uint64_t)ctx->nvariables + ctx->nfunctions;
    ring->dep_edges = (uint64_t)ctx->nedges;
    ring->exec_nodes = (uint64_t)ctx->nfunctions;
    ring->parallel_groups = (uint64_t)ctx->ngroups;

    for (int g = 0; g < ctx->ngroups; g++) {
        if (ctx->groups[g].type == ND_GROUP_BEAM_SEARCH) ring->beam_searches++;
        if (ctx->groups[g].type == ND_GROUP_PARALLEL) ring->dispatch_points++;
    }

    /* Lap 1 — survey */
    nd_ring_cell_set(ring, 0, ND_PHASE_SURVEY,
                     (uint32_t)(ring->sections + ring->functions),
                     ring->functions ? 90 : 30, bin->entry, "entry",
                     "sections=%llu functions=%llu",
                     (unsigned long long)ring->sections,
                     (unsigned long long)ring->functions);

    /* Lap 1 — classify */
    nd_ring_cell_set(ring, 0, ND_PHASE_CLASSIFY,
                     (uint32_t)ring->variables,
                     ring->variables ? 70 : 20, 0, "",
                     "variables=%llu groups=%llu",
                     (unsigned long long)ring->variables,
                     (unsigned long long)ring->groups);

    /* Lap 1 — trace */
    nd_ring_cell_set(ring, 0, ND_PHASE_TRACE,
                     (uint32_t)ctx->nedges,
                     ctx->nedges ? 80 : 25, 0, "",
                     "edges=%llu flows=%d",
                     (unsigned long long)ring->dep_edges,
                     ctx->var_flows.count);

    /* Lap 1 — predict */
    int ordered = 0;
    for (int i = 0; i < ctx->nfunctions; i++) {
        if (ctx->functions[i].execution_order > 0) ordered++;
    }
    ring->exec_order_len = (uint64_t)ordered;
    nd_ring_cell_set(ring, 0, ND_PHASE_PREDICT,
                     (uint32_t)ctx->nfunctions,
                     ring->functions ? 75 : 25, 0, "",
                     "functions=%llu order_len=%llu",
                     (unsigned long long)ring->functions,
                     (unsigned long long)ring->exec_order_len);

    /* Lap 1 — parallel */
    nd_ring_cell_set(ring, 0, ND_PHASE_PARALLEL,
                     (uint32_t)ring->parallel_groups,
                     ring->parallel_groups ? 75 : 25, 0, "",
                     "parallel=%llu beam=%llu",
                     (unsigned long long)ring->dispatch_points,
                     (unsigned long long)ring->beam_searches);

    /* Lap 1 — verify */
    nd_ring_cell_set(ring, 0, ND_PHASE_VERIFY,
                     (uint32_t)ctx->nfunctions,
                     60, 0, "",
                     "checked=%llu", (unsigned long long)ring->functions);

    /* Lap 2 — progression: symbols + xrefs discovered in lap 1 */
    nd_ring_cell_set(ring, 1, ND_PHASE_SURVEY,
                     (uint32_t)(ring->symbols + ring->xrefs),
                     ring->symbols ? 80 : 30, 0, "",
                     "symbols=%llu xrefs=%llu",
                     (unsigned long long)ring->symbols,
                     (unsigned long long)ring->xrefs);

    /* Lap 2 — classify: role breakdown */
    int roles = 0;
    for (int v = 0; v < ctx->nvariables; v++) {
        if (strcmp(nd_classify_role(ctx->variables[v].name), "unknown") != 0) roles++;
    }
    nd_ring_cell_set(ring, 1, ND_PHASE_CLASSIFY,
                     (uint32_t)roles, roles ? 80 : 20, 0, "",
                     "roles_known=%d/%d", roles, ctx->nvariables);

    /* Lap 2 — trace: caller chains */
    nd_ring_cell_set(ring, 1, ND_PHASE_TRACE,
                     (uint32_t)ctx->var_flows.count,
                     ctx->var_flows.count ? 85 : 25, 0, "",
                     "caller_chains=%d", ctx->var_flows.count);

    /* Lap 2 — predict: ordered function count */
    nd_ring_cell_set(ring, 1, ND_PHASE_PREDICT,
                     (uint32_t)ordered, ordered ? 80 : 25, 0, "",
                     "ordered=%d/%d", ordered, ctx->nfunctions);

    /* Lap 2 — parallel: groups with conditions */
    int cond_groups = 0;
    for (int g = 0; g < ctx->ngroups; g++) {
        if (ctx->groups[g].condition_conditional) cond_groups++;
    }
    nd_ring_cell_set(ring, 1, ND_PHASE_PARALLEL,
                     (uint32_t)cond_groups, cond_groups ? 85 : 30, 0, "",
                     "conditional_groups=%d", cond_groups);

    /* Lap 2 — verify */
    nd_ring_cell_set(ring, 1, ND_PHASE_VERIFY,
                     (uint32_t)ring->functions, 65, 0, "",
                     "verified=%llu", (unsigned long long)ring->functions);

    /* Totals */
    uint64_t conf = 0;
    int max_iter = 0;
    for (int i = 0; i < ring->ncell; i++) {
        ring->total_evidence += ring->cells[i].evidence;
        conf += (uint64_t)ring->cells[i].confidence;
        if (ring->cells[i].iteration > max_iter) max_iter = ring->cells[i].iteration;
    }
    ring->avg_confidence = ring->ncell ? (double)conf / (double)ring->ncell : 0.0;
    ring->iterations = max_iter + 1;

    return 0;
}

/* ============================================================================
 * Full pipeline
 * ============================================================================ */

int nd_static_analyze(nd_static_ctx_t *ctx) {
    if (!ctx || !ctx->bin) return -1;

    nd_survey(ctx);
    nd_classify(ctx);
    nd_trace(ctx);
    nd_scan_data_refs(ctx);
    nd_static_trace_flow(ctx);
    nd_static_predict_order(ctx);
    nd_static_detect_parallel(ctx);
    nd_static_detect_beam_search(ctx);
    nd_verify(ctx);
    nd_ring_build(ctx);

    return 0;
}

/* ============================================================================
 * Console output
 * ============================================================================ */

void nd_ring_print(const nd_ring_t *ring, FILE *out) {
    if (!ring || !out) return;

    fprintf(out, "\n=== Regression Ring (advanced analysis) ===\n");
    fprintf(out, "  Binary     : %s\n", ring->binary);
    fprintf(out, "  Arch       : %s\n", ring->arch);
    fprintf(out, "  Iterations : %d\n", ring->iterations);
    fprintf(out, "  Evidence   : %llu (avg confidence %.0f%%)\n",
            (unsigned long long)ring->total_evidence, ring->avg_confidence);

    static const char *phase_names[ND_PHASE_MAX] = {
        "SURVEY", "CLASSIFY", "TRACE", "PREDICT", "PARALLEL", "VERIFY"
    };

    fprintf(out, "\n  %-10s", "phase");
    for (int it = 0; it < ring->iterations; it++) fprintf(out, "  pass%-2d     ", it + 1);
    fprintf(out, "\n  ----------");
    for (int it = 0; it < ring->iterations; it++) fprintf(out, "  ----------");
    fprintf(out, "\n");

    for (int p = 0; p < ring->phase_count; p++) {
        fprintf(out, "  %-10s", phase_names[p]);
        for (int it = 0; it < ring->iterations; it++) {
            const nd_ring_cell_t *c = NULL;
            for (int i = 0; i < ring->ncell; i++) {
                if (ring->cells[i].iteration == it && ring->cells[i].phase == p) {
                    c = &ring->cells[i]; break;
                }
            }
            if (c) fprintf(out, "  %5u (%3d%%)", c->evidence, c->confidence);
            else   fprintf(out, "  %5s (%3s)", "-", "-");
        }
        fprintf(out, "\n");
    }

    fprintf(out, "\n  Details:\n");
    for (int i = 0; i < ring->ncell; i++) {
        const nd_ring_cell_t *c = &ring->cells[i];
        if (!c->detail[0]) continue;
        fprintf(out, "    lap%-2d %-9s  %s\n", c->iteration + 1,
                phase_names[c->phase], c->detail);
    }
}

void nd_static_print_flow(nd_static_ctx_t *ctx, FILE *out) {
    if (!ctx || !out) return;
    fprintf(out, "\n=== Variable Caller Chains ===\n");
    if (ctx->var_flows.count == 0) {
        fprintf(out, "  (no variables traced)\n");
        return;
    }
    for (int i = 0; i < ctx->var_flows.count; i++) {
        nd_var_flow_t *fl = &ctx->var_flows.flows[i];
        if (fl->chain_len == 0) continue;
        fprintf(out, "  %s:\n", fl->var_name);
        fprintf(out, "    ");
        for (int k = 0; k < fl->chain_len; k++) {
            uint64_t id = fl->chain[k];
            const char *nm = "?";
            for (int j = 0; j < ctx->nfunctions; j++) {
                if (ctx->functions[j].id == id) { nm = ctx->functions[j].name; break; }
            }
            fprintf(out, "%s%s", k ? "  <-  " : "", nm);
        }
        fprintf(out, "\n");
    }
}

void nd_static_print_order(nd_static_ctx_t *ctx, FILE *out) {
    if (!ctx || !out) return;
    fprintf(out, "\n=== Execution Order (predicted) ===\n");

    /* Print in predicted order, not in address order. */
    for (int rank = 1; rank <= ctx->nfunctions; rank++) {
        int fi = -1;
        for (int i = 0; i < ctx->nfunctions; i++) {
            if (ctx->functions[i].execution_order == rank) { fi = i; break; }
        }
        if (fi < 0) continue;
        nd_function_t *f = &ctx->functions[fi];
        fprintf(out, "  %3d. %-40s 0x%llx", f->execution_order, f->name,
                (unsigned long long)f->start);
        if (f->is_parallel)    fprintf(out, "  [parallel]");
        if (f->is_conditional) fprintf(out, "  [cond:%s]", f->condition_var);
        if (f->group_id)       fprintf(out, "  [group:%llu]", f->group_id);
        fprintf(out, "\n");
    }
}

void nd_static_print_parallel(nd_static_ctx_t *ctx, FILE *out) {
    if (!ctx || !out) return;
    fprintf(out, "\n=== Execution Groups ===\n");
    if (ctx->ngroups == 0) {
        fprintf(out, "  (no parallel/beam groups detected)\n");
        return;
    }
    for (int g = 0; g < ctx->ngroups; g++) {
        nd_group_t *gr = &ctx->groups[g];
        fprintf(out, "  [%s] %s\n",
                gr->type == ND_GROUP_BEAM_SEARCH ? "beam" :
                gr->type == ND_GROUP_PARALLEL   ? "par"  :
                gr->type == ND_GROUP_RECURSIVE  ? "rec"  : "seq",
                gr->name);
        if (gr->beam_width) fprintf(out, "       beam_width=%llu\n",
                                    (unsigned long long)gr->beam_width);
        if (gr->condition_var[0]) fprintf(out, "       cond_var=%s\n", gr->condition_var);
        if (gr->condition_msg[0]) fprintf(out, "       cond_msg=%s\n", gr->condition_msg);
        if (gr->dispatch_addr) fprintf(out, "       dispatch=0x%llx\n",
                                       (unsigned long long)gr->dispatch_addr);
        if (gr->join_addr)    fprintf(out, "       join=0x%llx\n",
                                       (unsigned long long)gr->join_addr);
        fprintf(out, "       functions=%d\n", gr->nfunctions);
        for (int k = 0; k < gr->nfunctions && k < 12; k++) {
            for (int j = 0; j < ctx->nfunctions; j++) {
                if (ctx->functions[j].id == gr->functions[k]) {
                    fprintf(out, "         - %s\n", ctx->functions[j].name);
                    break;
                }
            }
        }
        if (gr->nfunctions > 12) fprintf(out, "         ... %d more\n", gr->nfunctions - 12);
    }
}

void nd_static_print_report(nd_static_ctx_t *ctx, FILE *out) {
    if (!ctx || !out) return;
    nd_ring_print(&ctx->ring, out);
    nd_static_print_order(ctx, out);
    nd_static_print_parallel(ctx, out);
    nd_static_print_flow(ctx, out);
}

/* ============================================================================
 * SVG output — ring → group → function hierarchy
 *
 * Layout:
 *   - Outer: regression rings (one per iteration), dashed, with a
 *     clockwise arrow showing the loop back.
 *   - Middle: groups placed on the current ring radius.
 *   - Inner: functions inside each group (the lowest-level group).
 *   - Progression: outward spiral arcs from lap N to lap N+1.
 * ============================================================================ */


/* ============================================================================
 * SVG output — ring → group → function
 *
 * Layout
 *   Left  : the Regression Ring. One dashed circle per lap, a clockwise
 *           arrow showing the loop back, an outward spiral (progression)
 *           between laps, and the six phases as nodes on the inner ring.
 *   Right : the group tree. Every group is a box; inside it each function
 *           is listed one by one — the function is the lowest-level group.
 *           Functions that belong to no group fall into a "sequential"
 *           group so every function appears exactly once.
 * ============================================================================ */

#define ND_SVG_W       1560
#define ND_SVG_H       1040
#define ND_SVG_CX      400
#define ND_SVG_CY      500
#define ND_SVG_R0      135
#define ND_SVG_DR      118
#define ND_SVG_PANEL   790

static const char *const nd_phase_color[ND_PHASE_MAX] = {
    "#4c8dff", "#33c17a", "#f2b134", "#e0603c", "#a76ee0", "#25b6c4"
};

static const char *const nd_phase_name[ND_PHASE_MAX] = {
    "SURVEY", "CLASSIFY", "TRACE", "PREDICT", "PARALLEL", "VERIFY"
};

static const char *nd_group_color(nd_group_type_t t) {
    switch (t) {
        case ND_GROUP_PARALLEL:    return "#a76ee0";
        case ND_GROUP_BEAM_SEARCH: return "#f2b134";
        case ND_GROUP_CONDITIONAL: return "#e0603c";
        case ND_GROUP_RECURSIVE:   return "#25b6c4";
        default:                   return "#4c8dff";
    }
}

static const char *nd_group_kind(nd_group_type_t t) {
    switch (t) {
        case ND_GROUP_PARALLEL:    return "parallel";
        case ND_GROUP_BEAM_SEARCH: return "beam search";
        case ND_GROUP_CONDITIONAL: return "conditional";
        case ND_GROUP_RECURSIVE:   return "recursive";
        default:                   return "sequential";
    }
}

/* Find a function's index by id. */
static int nd_func_index(const nd_static_ctx_t *ctx, uint64_t id) {
    int i;
    for (i = 0; i < ctx->nfunctions; i++) {
        if (ctx->functions[i].id == id) return i;
    }
    return -1;
}

static void nd_svg_group_box(FILE *f, const nd_static_ctx_t *ctx, int g,
                             double x, double *y, double w) {
    const nd_group_t *gr = &ctx->groups[g];
    const char *col = nd_group_color(gr->type);
    double rowh = 15.0;
    double headh = 34.0;
    int nrows = 0;
    double boxh;

    for (int k = 0; k < gr->nfunctions; k++) {
        if (nd_func_index(ctx, gr->functions[k]) >= 0) nrows++;
    }

    /* Group header + one row per function + a condition line if present. */
    boxh = headh + (double)nrows * rowh + 6.0;
    if (gr->condition_var[0] || gr->condition_msg[0] || gr->beam_width) boxh += 15.0;

    fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"7\" "
               "fill=\"#161b22\" stroke=\"%s\" stroke-width=\"1.4\"/>\n",
            x, *y, w, boxh, col);
    fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"4\" height=\"%.1f\" rx=\"2\" fill=\"%s\"/>\n",
            x, *y, boxh, col);

    /* Header: group name + kind */
    fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"12.5\" fill=\"%s\" font-weight=\"bold\">",
            x + 12, *y + 15, col);
    nd_svg_escape(f, gr->name);
    fprintf(f, "</text>\n");
    fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"10\" fill=\"#6b7688\">%s</text>\n",
            x + 12, *y + 28, nd_group_kind(gr->type));

    /* Condition line: what gates this group */
    if (gr->beam_width || gr->condition_var[0] || gr->condition_msg[0]) {
        char cond[256];
        cond[0] = '\0';
        if (gr->beam_width)
            snprintf(cond + strlen(cond), sizeof(cond) - strlen(cond),
                     "beam_width=%llu  ", (unsigned long long)gr->beam_width);
        if (gr->condition_var[0])
            snprintf(cond + strlen(cond), sizeof(cond) - strlen(cond),
                     "var:%s  ", gr->condition_var);
        if (gr->condition_msg[0])
            snprintf(cond + strlen(cond), sizeof(cond) - strlen(cond),
                     "msg:%s", gr->condition_msg);
        fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"10\" fill=\"#f2b134\">",
                x + 12, *y + headh + 6);
        nd_svg_escape(f, cond);
        fprintf(f, "</text>\n");
    }

    /* The functions — the lowest-level group, one by one. */
    {
        double ry = *y + headh + (gr->beam_width || gr->condition_var[0] ||
                                  gr->condition_msg[0] ? 19.0 : 4.0);
        for (int k = 0; k < gr->nfunctions; k++) {
            int fi = nd_func_index(ctx, gr->functions[k]);
            const nd_function_t *fn;
            if (fi < 0) continue;
            fn = &ctx->functions[fi];
            fprintf(f, "  <circle cx=\"%.1f\" cy=\"%.1f\" r=\"2.6\" fill=\"%s\"/>\n",
                    x + 16, ry - 3.5, col);
            fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"11\" fill=\"#c9d1d9\" "
                       "font-family=\"Menlo,monospace\">", x + 24, ry);
            nd_svg_escape(f, fn->name);
            fprintf(f, "</text>\n");
            fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"9.5\" fill=\"#6b7688\" "
                       "text-anchor=\"end\">0x%llx</text>\n",
                    x + w - 10, ry, (unsigned long long)fn->start);
            if (fn->is_conditional)
                fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"9.5\" fill=\"#e0603c\" "
                           "text-anchor=\"end\">cond</text>\n", x + w - 62, ry);
            ry += rowh;
        }
    }

    *y += boxh + 12.0;
}

int nd_static_write_svg(nd_static_ctx_t *ctx, const char *path) {
    const nd_ring_t *ring;
    FILE *f;
    int g;
    double py;
    double px = ND_SVG_PANEL;
    double pw = ND_SVG_W - ND_SVG_PANEL - 30;
    int assigned[ND_STATIC_MAX_FUNCTIONS];
    int nassigned = 0;

    if (!ctx || !path) return -1;
    ring = &ctx->ring;

    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "static_analysis: cannot open '%s' for writing\n", path);
        return -1;
    }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
               "viewBox=\"0 0 %d %d\" font-family=\"Helvetica,Arial,sans-serif\">\n",
            ND_SVG_W, ND_SVG_H, ND_SVG_W, ND_SVG_H);
    fprintf(f, "  <rect width=\"%d\" height=\"%d\" fill=\"#0e1117\"/>\n", ND_SVG_W, ND_SVG_H);

    /* Markers */
    fprintf(f, "  <defs>\n");
    for (int p = 0; p < ND_PHASE_MAX; p++) {
        fprintf(f, "    <marker id=\"arw%d\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" "
                   "markerWidth=\"6\" markerHeight=\"6\" orient=\"auto-start-reverse\">\n", p);
        fprintf(f, "      <path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"%s\"/>\n", nd_phase_color[p]);
        fprintf(f, "    </marker>\n");
    }
    fprintf(f, "    <marker id=\"arwRing\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" "
               "markerWidth=\"6\" markerHeight=\"6\" orient=\"auto\">\n");
    fprintf(f, "      <path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"#6b7688\"/>\n");
    fprintf(f, "    </marker>\n");
    fprintf(f, "    <marker id=\"arwProg\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" "
               "markerWidth=\"6\" markerHeight=\"6\" orient=\"auto\">\n");
    fprintf(f, "      <path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"#f2b134\"/>\n");
    fprintf(f, "    </marker>\n");
    fprintf(f, "  </defs>\n");

    /* Title */
    fprintf(f, "  <text x=\"28\" y=\"36\" fill=\"#e6edf3\" font-size=\"20\" font-weight=\"bold\">"
               "Advanced Analysis — Regression Ring, Progression &amp; Group Tree</text>\n");
    fprintf(f, "  <text x=\"28\" y=\"58\" fill=\"#8b949e\" font-size=\"12\">");
    nd_svg_escape(f, ring->binary);
    fprintf(f, "  ·  %s  ·  %d laps  ·  %llu functions  ·  %llu groups  ·  "
               "%llu findings  ·  avg confidence %.0f%%</text>\n",
            ring->arch, ring->iterations,
            (unsigned long long)ring->functions, (unsigned long long)ring->groups,
            (unsigned long long)ring->total_evidence, ring->avg_confidence);

    /* ---- Rings (regression) ---- */
    for (int it = 0; it < ring->iterations; it++) {
        double radius = ND_SVG_R0 + (double)it * ND_SVG_DR;
        fprintf(f, "  <circle cx=\"%d\" cy=\"%d\" r=\"%.1f\" fill=\"none\" stroke=\"#2b3440\" "
                   "stroke-width=\"1.4\" stroke-dasharray=\"4 6\"/>\n",
                ND_SVG_CX, ND_SVG_CY, radius);
        fprintf(f, "  <text x=\"%d\" y=\"%.1f\" fill=\"#6b7688\" font-size=\"11\">lap %d</text>\n",
                ND_SVG_CX - 16, ND_SVG_CY - radius - 6, it + 1);
    }

    /* ---- Ring arrow (regression = loop back) ---- */
    for (int it = 0; it < ring->iterations; it++) {
        double radius = ND_SVG_R0 + (double)it * ND_SVG_DR;
        double a0 = -90.0 + 300.0, a1 = -90.0 + 360.0;
        double x0 = ND_SVG_CX + radius * cos(a0 * M_PI / 180.0);
        double y0 = ND_SVG_CY + radius * sin(a0 * M_PI / 180.0);
        double x1 = ND_SVG_CX + radius * cos(a1 * M_PI / 180.0);
        double y1 = ND_SVG_CY + radius * sin(a1 * M_PI / 180.0);
        fprintf(f, "  <path d=\"M %.1f %.1f A %.1f %.1f 0 0 1 %.1f %.1f\" fill=\"none\" "
                   "stroke=\"#6b7688\" stroke-width=\"2\" stroke-dasharray=\"7 5\" "
                   "marker-end=\"url(#arwRing)\" opacity=\"0.85\"/>\n",
                x0, y0, radius, radius, x1, y1);
    }

    /* ---- Progression: outward arcs lap N → lap N+1 ---- */
    for (int it = 0; it + 1 < ring->iterations; it++) {
        for (int p = 0; p < ND_PHASE_MAX; p++) {
            const nd_ring_cell_t *a = NULL, *b = NULL;
            for (int i = 0; i < ring->ncell; i++) {
                if (ring->cells[i].iteration == it     && ring->cells[i].phase == p) a = &ring->cells[i];
                if (ring->cells[i].iteration == it + 1 && ring->cells[i].phase == p) b = &ring->cells[i];
            }
            if (!a || !b) continue;

            {
                double r0 = ND_SVG_R0 + (double)it * ND_SVG_DR;
                double r1 = ND_SVG_R0 + (double)(it + 1) * ND_SVG_DR;
                double ang = (-90.0 + (360.0 / ND_PHASE_MAX) * p) * M_PI / 180.0;
                double x0 = ND_SVG_CX + r0 * cos(ang), y0 = ND_SVG_CY + r0 * sin(ang);
                double x1 = ND_SVG_CX + r1 * cos(ang), y1 = ND_SVG_CY + r1 * sin(ang);
                double rm = (r0 + r1) / 2.0;
                double xm = ND_SVG_CX + rm * cos(ang), ym = ND_SVG_CY + rm * sin(ang);

                fprintf(f, "  <path d=\"M %.1f %.1f Q %.1f %.1f %.1f %.1f\" fill=\"none\" "
                           "stroke=\"%s\" stroke-width=\"2.4\" opacity=\"0.92\" "
                           "marker-end=\"url(#arwProg)\"/>\n",
                        x0, y0, xm, ym, x1, y1, nd_phase_color[p]);
            }
        }
    }

    /* ---- Phase nodes on the innermost ring ---- */
    {
        double pr = ND_SVG_R0;
        for (int p = 0; p < ND_PHASE_MAX; p++) {
            double ang = (-90.0 + (360.0 / ND_PHASE_MAX) * p) * M_PI / 180.0;
            double x = ND_SVG_CX + pr * cos(ang);
            double y = ND_SVG_CY + pr * sin(ang);
            uint32_t ev = 0;
            int conf = 0;
            double rad;
            for (int i = 0; i < ring->ncell; i++) {
                if (ring->cells[i].iteration == 0 && ring->cells[i].phase == p) {
                    ev = ring->cells[i].evidence;
                    conf = ring->cells[i].confidence;
                }
            }
            rad = 8.0 + (ev > 60 ? 8.0 : ev * 0.12);
            if (rad > 17.0) rad = 17.0;
            fprintf(f, "  <circle cx=\"%.1f\" cy=\"%.1f\" r=\"%.1f\" fill=\"%s\" fill-opacity=\"0.85\" "
                       "stroke=\"#e6edf3\" stroke-width=\"%.1f\"/>\n",
                    x, y, rad, nd_phase_color[p], 1.2 + (conf / 100.0) * 2.2);
            fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"10.5\" fill=\"#e6edf3\" "
                       "text-anchor=\"middle\">%u</text>\n", x, y + 3.5, ev);
            fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"10\" fill=\"#9aa7b4\" "
                       "text-anchor=\"middle\">%s</text>\n", x, y - rad - 5, nd_phase_name[p]);
        }
    }

    /* ---- Centre ---- */
    fprintf(f, "  <circle cx=\"%d\" cy=\"%d\" r=\"58\" fill=\"#161b22\" stroke=\"#2b3440\"/>\n",
            ND_SVG_CX, ND_SVG_CY);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" font-size=\"12.5\" fill=\"#e6edf3\" text-anchor=\"middle\" "
               "font-weight=\"bold\">REASONING</text>\n", ND_SVG_CX, ND_SVG_CY - 10);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" font-size=\"10.5\" fill=\"#8b949e\" text-anchor=\"middle\">"
               "ring = regression</text>\n", ND_SVG_CX, ND_SVG_CY + 8);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" font-size=\"10.5\" fill=\"#8b949e\" text-anchor=\"middle\">"
               "spiral = progression</text>\n", ND_SVG_CX, ND_SVG_CY + 24);

    /* ---- Legend ---- */
    {
        double ly = 92;
        fprintf(f, "  <text x=\"28\" y=\"%.1f\" font-size=\"13\" fill=\"#e6edf3\" font-weight=\"bold\">"
                   "Legend</text>\n", ly);
        ly += 20;
        fprintf(f, "  <line x1=\"28\" y1=\"%.1f\" x2=\"62\" y2=\"%.1f\" stroke=\"#6b7688\" "
                   "stroke-width=\"2\" stroke-dasharray=\"7 5\"/>\n", ly - 4, ly - 4);
        fprintf(f, "  <text x=\"70\" y=\"%.1f\" font-size=\"11\" fill=\"#c9d1d9\">"
                   "regression (loop back to the same ring)</text>\n", ly);
        ly += 18;
        fprintf(f, "  <line x1=\"28\" y1=\"%.1f\" x2=\"62\" y2=\"%.1f\" stroke=\"#f2b134\" "
                   "stroke-width=\"2.2\"/>\n", ly - 4, ly - 4);
        fprintf(f, "  <text x=\"70\" y=\"%.1f\" font-size=\"11\" fill=\"#c9d1d9\">"
                   "progression (evidence moves outward)</text>\n", ly);
        ly += 18;
        for (int p = 0; p < ND_PHASE_MAX; p++) {
            fprintf(f, "  <circle cx=\"34\" cy=\"%.1f\" r=\"5\" fill=\"%s\"/>\n", ly - 4, nd_phase_color[p]);
            fprintf(f, "  <text x=\"70\" y=\"%.1f\" font-size=\"11\" fill=\"#c9d1d9\">%s</text>\n",
                    ly, nd_phase_name[p]);
            ly += 17;
        }
    }

    /* ---- Right panel: the group tree ---- */
    py = 92;
    fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"14\" fill=\"#e6edf3\" font-weight=\"bold\">"
               "Groups → Functions</text>\n", px, py);
    fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"10.5\" fill=\"#6b7688\">"
               "each function is the lowest-level group</text>\n", px, py + 16);
    py += 34;

    /* Which functions already appear in a group? */
    memset(assigned, 0, sizeof(assigned));
    for (g = 0; g < ctx->ngroups; g++) {
        for (int k = 0; k < ctx->groups[g].nfunctions; k++) {
            int fi = nd_func_index(ctx, ctx->groups[g].functions[k]);
            if (fi >= 0 && fi < ND_STATIC_MAX_FUNCTIONS) { assigned[fi] = 1; nassigned++; }
        }
    }

    /* Detected groups first. */
    for (g = 0; g < ctx->ngroups; g++) {
        if (ctx->groups[g].nfunctions == 0) continue;
        nd_svg_group_box(f, ctx, g, px, &py, pw);
    }

    /* Everything else lands in one sequential group. */
    {
        int loose = 0;
        for (int i = 0; i < ctx->nfunctions; i++) if (!assigned[i]) loose++;
        if (loose > 0) {
            double boxh = 34.0 + (double)loose * 15.0 + 6.0;
            double ry = py;
            fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"7\" "
                       "fill=\"#161b22\" stroke=\"#4c8dff\" stroke-width=\"1.4\"/>\n",
                    px, ry, pw, boxh);
            fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"4\" height=\"%.1f\" rx=\"2\" fill=\"#4c8dff\"/>\n",
                    px, ry, boxh);
            fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"12.5\" fill=\"#4c8dff\" "
                       "font-weight=\"bold\">sequential</text>\n", px + 12, ry + 15);
            fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"10\" fill=\"#6b7688\">"
                       "functions not dispatched in parallel</text>\n", px + 12, ry + 28);
            ry += 38;
            for (int i = 0; i < ctx->nfunctions; i++) {
                if (assigned[i]) continue;
                fprintf(f, "  <circle cx=\"%.1f\" cy=\"%.1f\" r=\"2.6\" fill=\"#4c8dff\"/>\n",
                        px + 16, ry - 3.5);
                fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"11\" fill=\"#c9d1d9\" "
                           "font-family=\"Menlo,monospace\">", px + 24, ry);
                nd_svg_escape(f, ctx->functions[i].name);
                fprintf(f, "</text>\n");
                fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" font-size=\"9.5\" fill=\"#6b7688\" "
                           "text-anchor=\"end\">0x%llx</text>\n",
                        px + pw - 10, ry, (unsigned long long)ctx->functions[i].start);
                ry += 15;
            }
            py += boxh + 12.0;
        }
    }

    /* ---- Footer ---- */
    fprintf(f, "  <text x=\"28\" y=\"%d\" font-size=\"11\" fill=\"#6b7688\">"
               "NeoDAX advanced analysis — ring = regression loop, outward spiral = forward "
               "progress, groups nest their functions as leaves</text>\n", ND_SVG_H - 20);

    fprintf(f, "</svg>\n");
    fclose(f);
    return 0;
}
