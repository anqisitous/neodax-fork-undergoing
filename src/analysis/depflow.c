#include "depflow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ============================================================================
// Data Dependency Flow Analysis Implementation
// 
// 依存性追跡システムの実装
// 静的解析によるデータフロー追跡
// ============================================================================

// ============================================================================
// Graph Construction Functions
// ============================================================================

static unsigned depflow_addr_hash(uint64_t a) {
    a ^= a >> 33; a *= 0xff51afd7ed558ccdULL; a ^= a >> 33;
    a *= 0xc4ceb9fe1a85ec53ULL; a ^= a >> 33;
    return (unsigned)(a & (DEPFLOW_ADDR_BUCKETS - 1));
}

void depflow_graph_init(depflow_graph_t *graph) {
    if (!graph) return;
    
    memset(graph, 0, sizeof(depflow_graph_t));
    graph->node_capacity = DEPFLOW_MAX_NODES;
    graph->edge_capacity = DEPFLOW_MAX_EDGES;
    graph->nodes = (depflow_node_t *)calloc(DEPFLOW_MAX_NODES, sizeof(depflow_node_t));
    graph->edges = (depflow_edge_t *)calloc(DEPFLOW_MAX_EDGES, sizeof(depflow_edge_t));
    graph->func_deps = (depflow_func_deps_t *)calloc(DEPFLOW_MAX_NODES, sizeof(depflow_func_deps_t));
    graph->var_usages = (depflow_var_usage_t *)calloc(DEPFLOW_MAX_EDGES, sizeof(depflow_var_usage_t));
    graph->paths = (depflow_path_t *)calloc(DEPFLOW_MAX_PATHS, sizeof(depflow_path_t));
    
    for (int i = 0; i < DEPFLOW_ADDR_BUCKETS; i++) {
        graph->addr_bucket[i] = -1;
    }

    graph->summaries = NULL;   /* allocated lazily by depflow_build_summaries */
    graph->edge_ht_cap = 1 << 17;
    graph->edge_ht = (int *)calloc((size_t)graph->edge_ht_cap, sizeof(int));

    graph->node_count = 0;
    graph->edge_count = 0;
    graph->func_deps_count = 0;
    graph->var_usage_count = 0;
    graph->path_count = 0;
    graph->summary_count = 0;
    graph->total_dependencies = 0;
    graph->max_path_length = 0;
    graph->avg_path_length = 0;
}

void depflow_graph_free(depflow_graph_t *graph) {
    if (!graph) return;
    
    if (graph->nodes) {
        free(graph->nodes);
        graph->nodes = NULL;
    }
    if (graph->edges) {
        free(graph->edges);
        graph->edges = NULL;
    }
    if (graph->func_deps) {
        free(graph->func_deps);
        graph->func_deps = NULL;
    }
    if (graph->var_usages) {
        free(graph->var_usages);
        graph->var_usages = NULL;
    }
    if (graph->paths) {
        free(graph->paths);
        graph->paths = NULL;
    }
    if (graph->summaries) {
        free(graph->summaries);
        graph->summaries = NULL;
    }
    if (graph->first_call) {
        free(graph->first_call);
        graph->first_call = NULL;
    }
    if (graph->edge_ht) {
        free(graph->edge_ht);
        graph->edge_ht = NULL;
        graph->edge_ht_cap = 0;
    }
    
    graph->node_count = 0;
    graph->edge_count = 0;
    graph->func_deps_count = 0;
    graph->var_usage_count = 0;
    graph->path_count = 0;
    graph->summary_count = 0;
    graph->first_call_count = 0;
}

int depflow_graph_add_node(depflow_graph_t *graph, depflow_node_t *node) {
    if (!graph || !node) return -1;
    
    if (graph->node_count >= graph->node_capacity) {
        return -1; // No more space
    }
    
    // Generate unique ID
    node->id = graph->node_count + 1;
    
    // Store node
    memcpy(&graph->nodes[graph->node_count], node, sizeof(depflow_node_t));
    graph->node_count++;
    
    // Update the address hash chain (full 64-bit VAs).
    {
        int idx = graph->node_count - 1;
        graph->nodes[idx].addr_next = -1;
        if (node->address != 0) {
            unsigned h = depflow_addr_hash(node->address);
            graph->nodes[idx].addr_next = graph->addr_bucket[h];
            graph->addr_bucket[h] = idx;
        }
    }
    
    return (int)node->id;
}

int depflow_graph_add_edge(depflow_graph_t *graph, uint64_t from, uint64_t to, int type) {
    return depflow_graph_add_edge_full(graph, from, to, type, 0, 0);
}

int depflow_graph_add_edge_full(depflow_graph_t *graph, uint64_t from, uint64_t to, 
                                  int type, uint64_t site, int is_conditional) {
    if (!graph) return -1;
    
    if (graph->edge_count >= graph->edge_capacity) {
        return -1; // No more space
    }
    
    depflow_edge_t edge;
    edge.from = from;
    edge.to = to;
    edge.type = type;
    edge.site = site;
    edge.is_conditional = is_conditional;
    edge.is_strong = 1;
    edge.weight = 1;
    edge.origin = DEPFLOW_ORIGIN_UNKNOWN;
    edge.confidence = 0;
    
    memcpy(&graph->edges[graph->edge_count], &edge, sizeof(depflow_edge_t));
    graph->edge_count++;
    graph->total_dependencies++;
    
    return graph->edge_count - 1;
}

int depflow_graph_add_edge_origin(depflow_graph_t *graph, uint64_t from, uint64_t to,
                                  int type, uint64_t site, int is_conditional,
                                  int origin, int confidence) {
    /* Address sharing: one edge per (from,to,type). Collisions join origin. */
    int mask = graph->edge_ht_cap - 1;
    int slot = -1;
    if (graph->edge_ht && mask > 0) {
        uint64_t h = (from * 2654435761u) ^ (to * 2246822519u) ^ ((uint64_t)type * 3266489917u);
        int base = (int)(h & (uint64_t)mask);
        for (int probe = 0; probe < 16; probe++) {
            int s = (base + probe) & mask;
            int e = graph->edge_ht[s];
            if (e == 0) { slot = s; break; }
            depflow_edge_t *ed = &graph->edges[e - 1];
            if (ed->from == from && ed->to == to && ed->type == type) {
                if (ed->origin == DEPFLOW_ORIGIN_UNKNOWN && origin != DEPFLOW_ORIGIN_UNKNOWN)
                    ed->origin = origin;
                else if (ed->origin != origin && origin != DEPFLOW_ORIGIN_UNKNOWN)
                    ed->origin = DEPFLOW_ORIGIN_UNKNOWN;
                if (confidence > ed->confidence) ed->confidence = confidence;
                return e - 1;
            }
        }
    }

    int idx = depflow_graph_add_edge_full(graph, from, to, type, site, is_conditional);
    if (idx >= 0) {
        graph->edges[idx].origin = origin;
        graph->edges[idx].confidence = confidence;
        if (slot >= 0) graph->edge_ht[slot] = idx + 1;
    }
    return idx;
}

const char *depflow_origin_str(int origin) {
    switch (origin) {
        case DEPFLOW_ORIGIN_CONST:      return "const";
        case DEPFLOW_ORIGIN_PARAM:      return "param";
        case DEPFLOW_ORIGIN_GLOBAL:     return "global";
        case DEPFLOW_ORIGIN_EXTCALL:    return "extcall";
        case DEPFLOW_ORIGIN_CALLEE_RET: return "callee-ret";
        case DEPFLOW_ORIGIN_RECURSION:  return "recursion";
        case DEPFLOW_ORIGIN_INDIRECT:   return "indirect";
        default:                        return "unknown";
    }
}

const char *depflow_cond_str(int kind) {
    switch (kind) {
        case DEPFLOW_COND_EQ_CONST: return "==const";
        case DEPFLOW_COND_NE_CONST: return "!=const";
        case DEPFLOW_COND_MSG_TYPE: return "msg-type";
        case DEPFLOW_COND_FLAG_BIT: return "flag-bit";
        case DEPFLOW_COND_BOUNDS:   return "bounds";
        case DEPFLOW_COND_PTR_NULL: return "ptr-null";
        case DEPFLOW_COND_LOOP:     return "loop";
        case DEPFLOW_COND_EXTERNAL: return "external";
        default:                    return "none";
    }
}

// ============================================================================
// Node Creation Functions
// ============================================================================

uint64_t depflow_node_create(depflow_graph_t *graph, int type, uint64_t address, 
                              const char *name, uint64_t size) {
    if (!graph) return 0;

    /* Address sharing: same address => same node (state is shared). */
    if (address != 0) {
        uint64_t ex = depflow_node_id_by_address(graph, address);
        if (ex) return ex;
    }
    
    depflow_node_t node;
    memset(&node, 0, sizeof(depflow_node_t));
    
    node.type = type;
    node.address = address;
    if (name) {
        strncpy(node.name, name, sizeof(node.name) - 1);
        node.name[sizeof(node.name) - 1] = '\0';
    }
    node.size = size;
    node.category = DEPFLOW_VAR_CATEGORY_UNKNOWN;
    node.definition_site = address;
    node.visited = 0;
    node.timestamp = 0;
    
    if (depflow_graph_add_node(graph, &node) < 0) {
        return 0;
    }
    
    return node.id;
}

uint64_t depflow_node_create_variable(depflow_graph_t *graph, uint64_t address,
                                         const char *name, uint64_t size, int category) {
    uint64_t node_id = depflow_node_create(graph, DEPFLOW_NODE_VARIABLE, address, name, size);
    if (node_id > 0) {
        graph->nodes[graph->node_count - 1].category = category;
    }
    return node_id;
}

uint64_t depflow_node_create_register(depflow_graph_t *graph, uint64_t address,
                                        int reg_num, int reg_class) {
    char reg_name[32];
    snprintf(reg_name, sizeof(reg_name), "reg_%d_class_%d", reg_num, reg_class);
    uint64_t node_id = depflow_node_create(graph, DEPFLOW_NODE_REGISTER, address, reg_name, 8);
    if (node_id > 0) {
        graph->nodes[graph->node_count - 1].reg_num = reg_num;
        graph->nodes[graph->node_count - 1].reg_class = reg_class;
    }
    return node_id;
}

uint64_t depflow_node_create_function(depflow_graph_t *graph, int func_idx,
                                      uint64_t address, const char *name) {
    uint64_t node_id = depflow_node_create(graph, DEPFLOW_NODE_FUNCTION,
                                           address, name, 0);
    if (node_id > 0) {
        graph->nodes[graph->node_count - 1].func_idx = func_idx;
    }
    return node_id;
}

// ============================================================================
// Main Dependency Analysis
// ============================================================================

int depflow_analyze(macho_binary_t *bin, macho_opts_t *opts) {
    if (!bin || !macho_is_valid(bin)) return -1;
    
    depflow_context_t ctx;
    memset(&ctx, 0, sizeof(depflow_context_t));
    
    ctx.bin = bin;
    ctx.opts = opts;
    
    // Initialize graph
    depflow_graph_init(&ctx.graph);
    
    // Build dependency graph
    if (depflow_build_dependency_graph(bin, &ctx.graph) != 0) {
        fprintf(stderr, "depflow: Failed to build dependency graph\n");
        depflow_graph_free(&ctx.graph);
        return -1;
    }
    
    // Analyze function dependencies
    if (depflow_analyze_function_dependencies(bin, &ctx.graph) != 0) {
        fprintf(stderr, "depflow: Failed to analyze function dependencies\n");
    }
    
    // Track variable usage
    if (opts && opts->depflow_target_var[0] != '\0') {
        if (depflow_track_variable_usage(bin, &ctx.graph, opts->depflow_target_var) != 0) {
            fprintf(stderr, "depflow: Failed to track variable usage\n");
        }
    }
    
    // Predict execution order
    if (depflow_predict_execution_order(bin, &ctx.graph, ctx.execution_order, 
                                         DEPFLOW_MAX_NODES) != 0) {
        fprintf(stderr, "depflow: Failed to predict execution order\n");
    }
    
    // Detect parallel execution
    if (depflow_detect_parallel_execution(bin, &ctx.graph) != 0) {
        fprintf(stderr, "depflow: Failed to detect parallel execution\n");
    }
    
    // Beam search specific analysis
    if (opts && opts->beam_search_analysis) {
        if (depflow_analyze_beam_search(&ctx.graph, bin) != 0) {
            fprintf(stderr, "depflow: Failed to analyze beam search patterns\n");
        }
    }
    
    // Output results
    if (opts && opts->depflow_dot[0] != '\0') {
        depflow_export_dot(&ctx.graph, opts->depflow_dot);
    }
    
    // Print analysis
    if (opts && opts->verbose) {
        depflow_print_dependency_graph(&ctx.graph, opts, stdout);
    }
    
    // Cleanup
    depflow_graph_free(&ctx.graph);
    
    return 0;
}

static macho_symbol_t *find_symbol(macho_binary_t *bin, uint64_t addr) {
    if (!bin || !bin->symbols) return NULL;
    for (int i = 0; i < bin->nsymbols; i++)
        if (bin->symbols[i].address == addr) return &bin->symbols[i];
    return NULL;
}

static void depflow_cond_set(depflow_condition_t *c, depflow_cond_kind_t kind,
                             uint64_t site, uint64_t value,
                             const char *tag, int confidence) {
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->site = site;
    c->value = value;
    c->target_func = -1;
    c->confidence = confidence;
    c->state = 1;   /* default: then / branch taken */
    if (tag) {
        strncpy(c->tag, tag, sizeof(c->tag) - 1);
        c->tag[sizeof(c->tag) - 1] = '\0';
    }
}

static int depflow_origin_join(int a, int b) {
    if (a == b) return a;
    if (a == DEPFLOW_ORIGIN_UNKNOWN) return b;
    if (b == DEPFLOW_ORIGIN_UNKNOWN) return a;
    return DEPFLOW_ORIGIN_UNKNOWN;   /* conflicting evidence -> widen */
}

/* O(1) via the graph's address hash (function nodes carry func_idx). */
static int depflow_find_func_idx(macho_binary_t *bin, depflow_graph_t *graph, uint64_t addr) {
    if (graph) {
        uint64_t id = depflow_node_id_by_address(graph, addr);
        if (id > 0 && (int)id <= graph->node_count) {
            depflow_node_t *n = &graph->nodes[id - 1];
            if (n->type == DEPFLOW_NODE_FUNCTION &&
                n->func_idx >= 0 && n->func_idx < bin->nfunctions)
                return n->func_idx;
        }
        return -1;
    }
    for (int i = 0; i < bin->nfunctions; i++)   /* fallback: no graph */
        if (bin->functions[i].start == addr) return i;
    return -1;
}

static int depflow_primary_ret_origin(const depflow_func_summary_t *s) {
    int best = DEPFLOW_ORIGIN_UNKNOWN, bestc = -1;
    for (int i = 0; i < s->nouts; i++) {
        if (s->outs[i].kind != DEPFLOW_VOUT_RET) continue;
        if (s->outs[i].origin != DEPFLOW_ORIGIN_UNKNOWN &&
            s->outs[i].confidence > bestc) {
            best = s->outs[i].origin;
            bestc = s->outs[i].confidence;
        }
    }
    return best;
}

/* ── instruction-level helpers (ARM64 ABI, mac-only) ─────────────── */

/* Parse a leading ABI arg register (x0..x30 / w0..w30) from an operand
 * token; returns 0..7 for arg regs, -1 otherwise. */
static int depflow_reg_of_token(const char *tok) {
    if (!tok) return -1;
    while (*tok == ' ' || *tok == '[' || *tok == ']' || *tok == '#' ||
           *tok == '!' || *tok == '{' || *tok == '}') tok++;
    if ((tok[0] != 'x' && tok[0] != 'w') || tok[1] < '0' || tok[1] > '9') return -1;
    int n = tok[1] - '0';
    if (tok[2] >= '0' && tok[2] <= '9') n = n * 10 + (tok[2] - '0');
    return (n <= 7) ? n : -1;
}

/* How many leading operands are destinations (writes)? Stores / compares /
 * branches / returns have 0; ldp/ldpsw/ldnp have 2. NOTE: this must not
 * blanket-exclude "b*" — bic/bfi/bfxil/bsl/bif/bics write their first operand. */
static int depflow_def_count(const char *m) {
    if (!m || !m[0]) return 0;
    if (!strncmp(m, "st", 2)) return 0;                 /* str/stp/stur/st1... */
    if (!strcmp(m, "cmp") || !strcmp(m, "cmn") ||
        !strcmp(m, "tst") || !strcmp(m, "ccmp") || !strcmp(m, "ccmn")) return 0;
    if (!strcmp(m, "b") || (m[0] == 'b' && m[1] == '.') ||
        !strcmp(m, "bl") || !strcmp(m, "br") || !strcmp(m, "blr") ||
        !strncmp(m, "braa", 4) || !strncmp(m, "brab", 4) ||
        !strncmp(m, "blraa", 5) || !strncmp(m, "blrab", 5) ||
        !strncmp(m, "brabz", 5) || !strncmp(m, "blrabz", 6)) return 0;
    if (!strcmp(m, "ret") || !strcmp(m, "eret") || !strcmp(m, "drps")) return 0;
    if (!strncmp(m, "cbz", 3) || !strncmp(m, "cbnz", 4) ||
        !strncmp(m, "tbz", 3) || !strncmp(m, "tbnz", 4)) return 0;
    if (!strcmp(m, "ldp") || !strncmp(m, "ldpsw", 5) || !strcmp(m, "ldnp")) return 2;
    return 1;
}

static uint64_t depflow_imm_of(const char *ops) {
    const char *p = strchr(ops, '#');
    if (!p) return 0;
    return strtoull(p + 1, NULL, 0);
}

/* ── dominator guards ───────────────────────────────────────────────
 * For each state-changing store, keep only the *necessary* conditional
 * branches: those whose block dominates the store. Both outcomes are
 * considered autonomously — the branch that leads toward the store fixes
 * the polarity (then / now). No path-depth cap. */

static int depflow_insn_lower_bound(macho_binary_t *bin, uint64_t addr) {
    int lo = 0, hi = bin->ninsns;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (bin->insns[mid].address < addr) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static int depflow_insn_index_at(macho_binary_t *bin, uint64_t addr) {
    int i = depflow_insn_lower_bound(bin, addr);
    if (i < bin->ninsns && bin->insns[i].address == addr) return i;
    return -1;
}

/* Classify a conditional branch into EQ/NE/FLAG/BOUNDS and return its
 * compare immediate. `*out_reg` is the compared ABI arg reg (x0-x7) or -1. */
static uint64_t depflow_branch_kind(macho_binary_t *bin, uint64_t branch_addr,
                                    int *out_reg, depflow_cond_kind_t *out_kind) {
    int bi = depflow_insn_index_at(bin, branch_addr);
    *out_reg = -1;
    *out_kind = DEPFLOW_COND_EXTERNAL;
    if (bi < 0) return 0;

    const char *m = bin->insns[bi].mnemonic;
    const char *ops = bin->insns[bi].operands;

    if (!strcmp(m, "cbz") || !strcmp(m, "cbnz")) {
        *out_reg = depflow_reg_of_token(ops);
        *out_kind = !strcmp(m, "cbz") ? DEPFLOW_COND_EQ_CONST
                                       : DEPFLOW_COND_NE_CONST;
        return 0;
    }
    if (!strcmp(m, "tbz") || !strcmp(m, "tbnz")) {
        *out_reg = depflow_reg_of_token(ops);
        *out_kind = DEPFLOW_COND_FLAG_BIT;
        return depflow_imm_of(ops);
    }
    if (m[0] == 'b' && m[1] == '.') {
        for (int k = bi - 1; k >= 0; k--) {
            macho_insn_t *c = &bin->insns[k];
            if (!strcmp(c->mnemonic, "cmp") || !strcmp(c->mnemonic, "cmn") ||
                !strncmp(c->mnemonic, "subs", 4) || !strcmp(c->mnemonic, "tst") ||
                !strncmp(c->mnemonic, "ands", 4)) {
                *out_reg = depflow_reg_of_token(c->operands);
                const char *cond = m + 2;
                if (!strncmp(cond, "eq", 2))      *out_kind = DEPFLOW_COND_EQ_CONST;
                else if (!strncmp(cond, "ne", 2)) *out_kind = DEPFLOW_COND_NE_CONST;
                else if (!strncmp(cond, "lt", 2) || !strncmp(cond, "gt", 2) ||
                         !strncmp(cond, "le", 2) || !strncmp(cond, "ge", 2) ||
                         !strncmp(cond, "hi", 2) || !strncmp(cond, "lo", 2) ||
                         !strncmp(cond, "hs", 2) || !strncmp(cond, "ls", 2))
                    *out_kind = DEPFLOW_COND_BOUNDS;
                else *out_kind = DEPFLOW_COND_EQ_CONST;
                return depflow_imm_of(c->operands);
            }
            if (c->is_branch) break;   /* don't cross another branch */
        }
    }
    return 0;
}

#define DEPFLOW_DOM_MAX_BLOCKS 1024

/* blk[] is the function's block list in ascending address order. */
static int depflow_local_block_of(macho_binary_t *bin, const int *blk, int n, uint64_t addr) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        macho_block_t *b = &bin->blocks[blk[mid]];
        if (addr < b->start) hi = mid - 1;
        else if (addr >= b->end) lo = mid + 1;
        else return mid;
    }
    return -1;
}

static void depflow_attach_guard(depflow_vout_t *o, const depflow_condition_t *g) {
    if (!o || o->nconds >= DEPFLOW_MAX_CONDS) return;
    o->conds[o->nconds++] = *g;
}

static void depflow_dominator_guards(macho_binary_t *bin, depflow_graph_t *graph,
                                     depflow_func_summary_t *s,
                                     const uint64_t *stores, int nstores) {
    int n = 0;
    int blk[DEPFLOW_DOM_MAX_BLOCKS];
    for (int b = 0; b < bin->nblocks && n < DEPFLOW_DOM_MAX_BLOCKS; b++)
        if (bin->blocks[b].func_idx == s->func_idx) blk[n++] = b;
    if (n <= 1) return;

    /* direct global->local block map (removes all linear scans) */
    int nb = bin->nblocks > 0 ? bin->nblocks : 1;
    int *g2l = (int *)malloc(sizeof(int) * (size_t)nb);
    if (!g2l) return;
    for (int b = 0; b < bin->nblocks; b++) g2l[b] = -1;
    for (int i = 0; i < n; i++) g2l[blk[i]] = i;
#define LI(x) ((x) >= 0 && (x) < bin->nblocks ? g2l[(x)] : -1)

    int W = (n + 63) / 64;
    uint64_t *dom = (uint64_t *)calloc((size_t)n * W, 8);
    if (!dom) { free(g2l); return; }

    for (int i = 0; i < n; i++) {
        for (int w = 0; w < W; w++) dom[i * W + w] = (i == 0) ? 0ULL : ~0ULL;
        dom[i * W + (i / 64)] |= (1ULL << (i % 64));
    }

    int changed = 1, it = 0;
    while (changed && it++ < 256) {
        changed = 0;
        for (int i = 1; i < n; i++) {
            uint64_t tmp[16];
            for (int w = 0; w < W; w++) tmp[w] = ~0ULL;
            macho_block_t *gb = &bin->blocks[blk[i]];
            int npred = 0;
            for (int p = 0; p < gb->npred && p < 16; p++) {
                int li = LI(gb->pred[p]);
                if (li < 0) continue;
                for (int w = 0; w < W; w++) tmp[w] &= dom[li * W + w];
                npred++;
            }
            if (npred == 0) for (int w = 0; w < W; w++) tmp[w] = 0ULL;
            tmp[i / 64] |= (1ULL << (i % 64));
            for (int w = 0; w < W; w++) {
                if (dom[i * W + w] != tmp[w]) { dom[i * W + w] = tmp[w]; changed = 1; }
            }
        }
    }

    for (int si = 0; si < nstores; si++) {
        uint64_t addr = stores[si];
        int lb = depflow_local_block_of(bin, blk, n, addr);
        if (lb < 0) continue;

        depflow_vout_t *out = NULL;
        for (int o = 0; o < s->nouts; o++)
            if (s->outs[o].source == addr) { out = &s->outs[o]; break; }
        if (!out) continue;
        out->nconds = 0;   /* replace provisional guards with dominator ones */

        for (int d = 0; d < n && out->nconds < DEPFLOW_MAX_CONDS; d++) {
            if (!(dom[lb * W + (d / 64)] & (1ULL << (d % 64)))) continue;
            macho_block_t *db = &bin->blocks[blk[d]];
            if (db->nsucc < 2) continue;
            for (int k = 0; k < db->nsucc && k < 16; k++) {
                int ls = LI(db->succ[k]);
                if (ls < 0) continue;
                if (!(dom[lb * W + (ls / 64)] & (1ULL << (ls % 64)))) continue;
                if (db->edge_type[k] != MACHO_EDGE_TYPE_COND_TRUE &&
                    db->edge_type[k] != MACHO_EDGE_TYPE_COND_FALSE) continue;

                depflow_cond_kind_t ck;
                int creg = -1;
                uint64_t cimm = depflow_branch_kind(bin, db->end - 4, &creg, &ck);
                int handler = -1;   /* MSG_TYPE -> handler function index */

                /* Dispatch heuristic: arg-reg compare (message tag) whose taken
                 * side reaches a call. BFS over blocks using the precomputed
                 * first-call table; bounded by visited blocks, not path depth. */
                if ((ck == DEPFLOW_COND_EQ_CONST || ck == DEPFLOW_COND_NE_CONST) &&
                    creg >= 0) {
                    unsigned char vis[DEPFLOW_DOM_MAX_BLOCKS];
                    int stack[DEPFLOW_DOM_MAX_BLOCKS], sp = 0;
                    memset(vis, 0, sizeof(vis));
                    vis[ls] = 1; stack[sp++] = ls;
                    while (sp > 0) {
                        int cur = stack[--sp];
                        depflow_first_call_t *fc = graph->first_call
                            ? &graph->first_call[blk[cur]] : NULL;
                        if (fc && fc->valid) {
                            ck = DEPFLOW_COND_MSG_TYPE;
                            if (fc->has_target)
                                handler = depflow_find_func_idx(bin, graph, fc->target);
                            break;
                        }
                        macho_block_t *cb = &bin->blocks[blk[cur]];
                        for (int s2 = 0; s2 < cb->nsucc && s2 < 16; s2++) {
                            int l2 = LI(cb->succ[s2]);
                            if (l2 >= 0 && !vis[l2]) { vis[l2] = 1; stack[sp++] = l2; }
                        }
                    }
                    if (ck != DEPFLOW_COND_MSG_TYPE && cimm < 256)
                        ck = DEPFLOW_COND_MSG_TYPE;
                }

                char tag[24];
                if (ck == DEPFLOW_COND_MSG_TYPE && handler >= 0)
                    snprintf(tag, sizeof(tag), "x%d==0x%llx->f%d", creg,
                             (unsigned long long)cimm, handler);
                else
                    snprintf(tag, sizeof(tag), "x%d==0x%llx", creg,
                             (unsigned long long)cimm);
                depflow_condition_t g;
                depflow_cond_set(&g, ck, db->end - 4, cimm, tag, 65);
                g.state = (db->edge_type[k] == MACHO_EDGE_TYPE_COND_TRUE) ? 1 : 0;
                g.target_func = handler;
                depflow_attach_guard(out, &g);
                break;   /* one side of this branch reaches the store */
            }
        }
    }
    free(dom);
    free(g2l);
#undef LI
}

/* Walk decoded instructions of one function: infer ABI args (read before
 * write) and the branch guards under which state changes. Appends to the
 * summary; no path-depth cap. */
static void depflow_scan_insns(macho_binary_t *bin, depflow_graph_t *graph,
                               macho_function_t *f,
                               depflow_func_summary_t *s) {
    if (!bin->insns || bin->ninsns <= 0 || !f || !s) return;

    uint64_t first_use[8], first_def[8];
    for (int i = 0; i < 8; i++) { first_use[i] = UINT64_MAX; first_def[i] = UINT64_MAX; }

    depflow_condition_t guards[DEPFLOW_MAX_CONDS];
    int nguards = 0;
    char pend_reg = -1;
    uint64_t pend_imm = 0;
    uint64_t stores[16];
    int nstores = 0;
    uint64_t calls[16];
    int ncalls = 0;

    for (int k = 0; k < bin->ninsns; k++) {
        macho_insn_t *in = &bin->insns[k];
        if (in->address < f->start || in->address >= f->end) continue;
        const char *m = in->mnemonic;
        const char *ops = in->operands;

        /* guards from compare + conditional branch */
        if (!strcmp(m, "cmp") || !strcmp(m, "cmn")) {
            pend_reg = (char)depflow_reg_of_token(ops);
            pend_imm = depflow_imm_of(ops);
        } else if (!strcmp(m, "cbz") || !strcmp(m, "cbnz") ||
                   !strcmp(m, "tbz") || !strcmp(m, "tbnz")) {
            if (nguards < DEPFLOW_MAX_CONDS) {
                depflow_condition_t *g = &guards[nguards];
                int is_t = (m[0] == 't');
                depflow_cond_set(g, is_t ? DEPFLOW_COND_FLAG_BIT : DEPFLOW_COND_EQ_CONST,
                                 in->address, is_t ? depflow_imm_of(ops) : 0,
                                 m, 40);
                nguards++;
            }
            pend_reg = -1;
        } else if (m[0] == 'b' && m[1] == '.') {
            if (nguards < DEPFLOW_MAX_CONDS) {
                depflow_condition_t *g = &guards[nguards];
                depflow_cond_kind_t k2 = strstr(m, "ne") ? DEPFLOW_COND_NE_CONST
                                                          : DEPFLOW_COND_EQ_CONST;
                depflow_cond_set(g, k2, in->address, pend_imm,
                                 pend_reg >= 0 ? "cmp+b.cond" : m, 45);
                nguards++;
            }
            pend_reg = -1;
        }

        /* memory stores = state changes; calls = ring edges (guards below) */
        if (!strncmp(m, "st", 2) && nstores < 16) stores[nstores++] = in->address;
        if (in->is_call && ncalls < 16) calls[ncalls++] = in->address;

        /* ABI arg regs: first operand is a def unless it's a store/compare */
        int ndef = depflow_def_count(m);
        int idx = 0;
        char buf[256];
        strncpy(buf, ops, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","), idx++) {
            int r = depflow_reg_of_token(tok);
            if (r < 0) continue;
            if (idx < ndef) {
                if (in->address < first_def[r]) first_def[r] = in->address;
            } else {
                if (in->address < first_use[r]) first_use[r] = in->address;
            }
        }
    }

    /* state-changing stores, with dominator guards (then/now) */
    for (int si = 0; si < nstores && s->nouts < DEPFLOW_MAX_VOUTS; si++) {
        depflow_vout_t *o = &s->outs[s->nouts++];
        memset(o, 0, sizeof(*o));
        o->kind = DEPFLOW_VOUT_GLOBAL;
        o->source = stores[si];
        o->address = stores[si];
        o->origin = DEPFLOW_ORIGIN_GLOBAL;
        o->confidence = 35;
        o->changes_state = 1;
    }
    if (nstores > 0 && bin->nblocks > 0)
        depflow_dominator_guards(bin, graph, s, stores, nstores);
    /* then/now guard on conditional calls (ring edges) */
    if (ncalls > 0 && bin->nblocks > 0)
        depflow_dominator_guards(bin, graph, s, calls, ncalls);

    /* virtual args from ABI registers read before written */
    for (int r = 0; r < 8 && s->nargs < DEPFLOW_MAX_VARGS; r++) {
        if (first_use[r] == UINT64_MAX) continue;
        if (first_def[r] != UINT64_MAX && first_def[r] <= first_use[r]) continue;

        depflow_varg_t *a = &s->args[s->nargs];
        memset(a, 0, sizeof(*a));
        a->kind = DEPFLOW_VARG_REG;
        a->reg  = r;
        a->index = s->nargs;
        a->origin = DEPFLOW_ORIGIN_PARAM;
        a->confidence = 70;
        a->read_before_write = 1;
        for (int g = 0; g < nguards && a->nconds < DEPFLOW_MAX_CONDS; g++)
            a->conds[a->nconds++] = guards[g];
        s->nargs++;
    }

    /* attach the guards to state-changing outputs */
    for (int o = 0; o < s->nouts; o++) {
        if (!s->outs[o].changes_state) continue;
        for (int g = 0; g < nguards && s->outs[o].nconds < DEPFLOW_MAX_CONDS; g++)
            s->outs[o].conds[s->outs[o].nconds++] = guards[g];
    }
}

/* ── block-level reaching definitions ──────────────────────────────
 * Values are identified by their def-site address (no SSA renaming), so a
 * variable can be shared across branches (union of reaching defs at merges =
 * phi as a set of edges) without merging distinct states. Bounded: at most
 * RD_MAX_DEFS reaching defs are kept per register. */

#define RD_MAX_DEFS 4
#define RD_MAX_MEM  8

typedef struct { uint64_t addr; int origin; int certain; } rd_def_t;
typedef struct { rd_def_t d[RD_MAX_DEFS]; int n; } rd_set_t;
typedef struct { uint64_t loc; uint64_t def; int origin; } rd_mem_t;
typedef struct { rd_mem_t d[RD_MAX_MEM]; int n; } rd_mset_t;
typedef struct {
    rd_set_t  in[8], out[8];
    rd_mset_t min_, mout;          /* memory/global reaching defs */
    uint64_t  adr_in[31], adr_out[31];  /* adrp/adr base values (0=unknown) */
} rd_blk_t;

static void rd_add(rd_set_t *s, uint64_t addr, int origin, int certain) {
    for (int i = 0; i < s->n; i++) {
        if (s->d[i].addr == addr) {
            if (s->d[i].origin != origin) s->d[i].origin = DEPFLOW_ORIGIN_UNKNOWN;
            s->d[i].certain &= certain;
            return;
        }
    }
    if (s->n < RD_MAX_DEFS) {
        s->d[s->n].addr = addr; s->d[s->n].origin = origin;
        s->d[s->n].certain = certain; s->n++;
    }
}

static void rd_union(rd_set_t *dst, const rd_set_t *src) {
    for (int i = 0; i < src->n; i++)
        rd_add(dst, src->d[i].addr, src->d[i].origin, src->d[i].certain);
}

static void rd_mem_add(rd_mset_t *m, uint64_t loc, uint64_t def, int origin) {
    for (int i = 0; i < m->n; i++) {
        if (m->d[i].loc == loc) { m->d[i].def = def; m->d[i].origin = origin; return; }
    }
    if (m->n < RD_MAX_MEM) { m->d[m->n].loc = loc; m->d[m->n].def = def;
                             m->d[m->n].origin = origin; m->n++; }
}

static void rd_mem_union(rd_mset_t *dst, const rd_mset_t *src) {
    for (int i = 0; i < src->n; i++) rd_mem_add(dst, src->d[i].loc, src->d[i].def, src->d[i].origin);
}

static const rd_mem_t *rd_mem_find(const rd_mset_t *m, uint64_t loc) {
    for (int i = 0; i < m->n; i++) if (m->d[i].loc == loc) return &m->d[i];
    return NULL;
}

/* full 0..30 register number (for base/address regs) */
static int depflow_reg_any(const char *tok) {
    if (!tok) return -1;
    while (*tok == ' ' || *tok == '[' || *tok == ']' || *tok == '#' ||
           *tok == '!' || *tok == '{' || *tok == '}') tok++;
    if ((tok[0] != 'x' && tok[0] != 'w') || tok[1] < '0' || tok[1] > '9') return -1;
    int n = tok[1] - '0';
    if (tok[2] >= '0' && tok[2] <= '9') n = n * 10 + (tok[2] - '0');
    return (n <= 30) ? n : -1;
}

static int depflow_is_mem(const macho_insn_t *in) {
    return !strncmp(in->mnemonic, "ld", 2) || !strncmp(in->mnemonic, "st", 2);
}

/* Resolve a load/store to a global address via a tracked adrp/adr base.
 * Returns 0 (honest skip) when the base is unknown or aliasing is unclear. */
static int depflow_mem_access(const macho_insn_t *in, const uint64_t adr[31],
                              uint64_t *loc, int *is_store) {
    if (!depflow_is_mem(in)) return 0;
    const char *p = strchr(in->operands, '[');
    if (!p) return 0;
    int base = depflow_reg_any(p + 1);
    if (base < 0 || adr[base] == 0) return 0;
    uint64_t off = 0;
    const char *h = strchr(p, '#');
    if (h) off = strtoull(h + 1, NULL, 0);
    *loc = adr[base] + off;
    *is_store = !strncmp(in->mnemonic, "st", 2);
    return 1;
}

/* Track adrp/adr page bases and carry them through add/sub-immediate. Any
 * other write to a tracked register invalidates it, so a stale base can never
 * be used to compute a wrong global address. */
static void depflow_adr_update(const macho_insn_t *in, const int *defr, int ndefr,
                               uint64_t adr[31]) {
    const char *m = in->mnemonic;

    if (!strcmp(m, "adrp") || !strcmp(m, "adr")) {
        int d = depflow_reg_any(in->operands);
        const char *x = strstr(in->operands, "0x");
        if (d >= 0 && x) adr[d] = strtoull(x, NULL, 0);
        return;
    }

    if ((!strcmp(m, "add") || !strcmp(m, "sub")) && ndefr >= 1 && defr[0] >= 0) {
        int dst = defr[0], src = -1;
        uint64_t imm = 0;
        char buf[256];
        strncpy(buf, in->operands, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        int idx = 0;
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","), idx++) {
            if (idx == 1) src = depflow_reg_any(tok);
            else if (idx == 2) { const char *h = strchr(tok, '#'); if (h) imm = strtoull(h + 1, NULL, 0); }
        }
        adr[dst] = (src >= 0 && adr[src] != 0)
                   ? (m[0] == 's' ? adr[src] - imm : adr[src] + imm)
                   : 0;
        return;
    }

    for (int d = 0; d < ndefr; d++)
        if (defr[d] >= 0) adr[defr[d]] = 0;
}

/* Do we fully model this instruction's register read/write set? If not, the
 * value analysis must not claim certainty across it (soundness). */
static int depflow_insn_modeled(const char *m) {
    static const char *ok[] = {
        "nop","ret","eret","drps","mov","movz","movk","movn","mvn",
        "add","adds","sub","subs","neg","and","ands","orr","orn","eor",
        "bic","bics","lsl","lsr","asr","ror","mul","madd","msub","mneg",
        "cmp","cmn","tst","ccmp","ccmn","adr","adrp",
        "ldr","ldrb","ldrh","ldrsb","ldrsh","ldrsw","ldur","ldurb","ldurh",
        "ldp","ldpsw","ldnp","str","strb","strh","stur","sturb","sturh",
        "stp","stnp","csel","csinc","csinv","csneg","cset","cinc",
        "sxtb","sxth","sxtw","uxtb","uxth","ubfm","sbfm","bfm","extr",
        "bfi","bfxil","ubfx","sbfx","rev","rev16","rev32","clz","cls",
        "mrs","msr"
    };
    if (!m || !m[0]) return 0;
    if (m[0] == 'b' && (m[1] == '.' || !strcmp(m, "b") || !strcmp(m, "bl") ||
                        !strcmp(m, "br") || !strcmp(m, "blr")
                        || !strncmp(m, "braa", 4) || !strncmp(m, "brab", 4)
                        || !strncmp(m, "blraa", 5) || !strncmp(m, "blrab", 5)
                        || !strncmp(m, "brabz", 5))) return 1;
    if (!strncmp(m, "cbz", 3) || !strncmp(m, "cbnz", 4) ||
        !strncmp(m, "tbz", 3) || !strncmp(m, "tbnz", 4)) return 1;
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (!strcmp(m, ok[i])) return 1;
    return 0;
}

/* Apply one instruction. emit!=0 writes the def-use edges (register + memory).
 * Uses read the pre-state; defs/clobbers/stores update it afterwards. */
static void depflow_apply_insn(macho_binary_t *bin, depflow_graph_t *graph,
                               macho_insn_t *in, rd_set_t cur[8],
                               uint64_t adr[31], rd_mset_t *mem, int emit) {
    const char *m = in->mnemonic;

    /* Soundness: an unmodeled instruction may define anything, so we never
     * keep claiming certainty across it, and drop tracked address bases. */
    if (!depflow_insn_modeled(m)) {
        for (int r = 0; r < 8; r++)
            for (int k = 0; k < cur[r].n; k++) cur[r].d[k].certain = 0;
        for (int r = 0; r < 31; r++) adr[r] = 0;
        return;
    }

    int ndef = depflow_def_count(m);
    int defr[2] = { -1, -1 }, ndefr = 0;              /* x0..x7 value defs */
    int defr_any[2] = { -1, -1 }, ndefr_any = 0;      /* x0..x30 for adr */
    int uses[8], nuse = 0;

    char buf[256];
    strncpy(buf, in->operands, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    int idx = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","), idx++) {
        int R = depflow_reg_any(tok);
        if (R < 0) continue;
        if (idx < ndef) {
            if (ndefr_any < 2) defr_any[ndefr_any++] = R;
            if (R <= 7 && ndefr < 2) defr[ndefr++] = R;
        } else if (R <= 7 && nuse < 8) {
            uses[nuse++] = R;
        }
    }

    int org = DEPFLOW_ORIGIN_UNKNOWN;
    if (in->is_call) {
        macho_symbol_t *cs = in->has_target ? find_symbol(bin, in->target) : NULL;
        org = (!cs || cs->is_undefined) ? DEPFLOW_ORIGIN_EXTCALL
                                        : DEPFLOW_ORIGIN_CALLEE_RET;
    } else if (!strncmp(m, "ldr", 3) || !strncmp(m, "ldur", 4) || !strncmp(m, "ldp", 3)) {
        org = DEPFLOW_ORIGIN_GLOBAL;
    } else {
        for (int u = 0; u < nuse; u++) {
            rd_set_t *s = &cur[uses[u]];
            if (s->n == 0) { org = DEPFLOW_ORIGIN_PARAM; break; }
            for (int k = 0; k < s->n; k++)
                if (s->d[k].origin == DEPFLOW_ORIGIN_PARAM) { org = DEPFLOW_ORIGIN_PARAM; break; }
            if (org == DEPFLOW_ORIGIN_PARAM) break;
        }
    }

    /* memory: load reads the reaching store, store updates the set */
    uint64_t loc = 0;
    int is_store = 0;
    int has_mem = depflow_mem_access(in, adr, &loc, &is_store);

    if (emit) {
        uint64_t un = depflow_node_id_by_address(graph, in->address);
        if (un == 0) un = depflow_node_create_register(graph, in->address, 0, 0);
        for (int u = 0; u < nuse && un; u++) {
            rd_set_t *s = &cur[uses[u]];
            for (int k = 0; k < s->n; k++) {
                uint64_t dn = depflow_node_id_by_address(graph, s->d[k].addr);
                if (!dn) continue;
                int conf = s->d[k].certain ? 100
                         : (s->d[k].origin == DEPFLOW_ORIGIN_UNKNOWN ? 25 : 50);
                depflow_graph_add_edge_origin(graph, dn, un, DEPFLOW_EDGE_DEF_USE,
                                              in->address, 0, s->d[k].origin, conf);
            }
        }
        if (has_mem && !is_store && un) {
            const rd_mem_t *md = rd_mem_find(mem, loc);
            if (md) {
                uint64_t dn = depflow_node_id_by_address(graph, md->def);
                if (dn)
                    depflow_graph_add_edge_origin(graph, dn, un, DEPFLOW_EDGE_DATA,
                                                  in->address, 0, DEPFLOW_ORIGIN_GLOBAL, 50);
            }
        }
    }

    if (has_mem && is_store)
        rd_mem_add(mem, loc, in->address, DEPFLOW_ORIGIN_GLOBAL);

    depflow_adr_update(in, defr_any, ndefr_any, adr);

    if (in->is_call) {
        for (int r = 0; r < 8; r++) cur[r].n = 0;
        rd_add(&cur[0], in->address, org, 1);                            /* x0 = ret */
        for (int r = 1; r < 8; r++) rd_add(&cur[r], in->address, DEPFLOW_ORIGIN_UNKNOWN, 1);
    } else {
        for (int d = 0; d < ndefr; d++) {
            if (defr[d] < 0) continue;
            cur[defr[d]].n = 0;
            rd_add(&cur[defr[d]], in->address, org, 1);
        }
    }
}

static void depflow_build_value_edges(macho_binary_t *bin, depflow_graph_t *graph,
                                      depflow_func_summary_t *s) {
    if (!bin->insns || bin->ninsns <= 0 || !graph) return;

    int blk[DEPFLOW_DOM_MAX_BLOCKS], n = 0;
    for (int b = 0; b < bin->nblocks && n < DEPFLOW_DOM_MAX_BLOCKS; b++)
        if (bin->blocks[b].func_idx == s->func_idx) blk[n++] = b;
    if (n <= 0) return;

    int nb = bin->nblocks > 0 ? bin->nblocks : 1;
    int *g2l = (int *)malloc(sizeof(int) * (size_t)nb);
    rd_blk_t *rd = (rd_blk_t *)calloc((size_t)n, sizeof(rd_blk_t));
    if (!g2l || !rd) { free(g2l); free(rd); return; }
    for (int b = 0; b < bin->nblocks; b++) g2l[b] = -1;
    for (int i = 0; i < n; i++) g2l[blk[i]] = i;
#define RDLI(x) ((x) >= 0 && (x) < bin->nblocks ? g2l[(x)] : -1)

    int changed = 1, it = 0;
    while (changed && it++ < 256) {
        changed = 0;
        for (int i = 0; i < n; i++) {
            macho_block_t *gb = &bin->blocks[blk[i]];
            rd_set_t  nin[8], nout[8];
            rd_mset_t nmin, nmout;
            uint64_t  nadr[31];
            memset(nin, 0, sizeof(nin));
            memset(&nmin, 0, sizeof(nmin));
            for (int r = 0; r < 31; r++) nadr[r] = 0;

            if (i != 0) {
                for (int p = 0; p < gb->npred && p < 16; p++) {
                    int li = RDLI(gb->pred[p]);
                    if (li < 0) continue;
                    for (int r = 0; r < 8; r++) rd_union(&nin[r], &rd[li].out[r]);
                    rd_mem_union(&nmin, &rd[li].mout);
                    for (int r = 0; r < 31; r++) {   /* adr merges only if equal */
                        uint64_t pv = rd[li].adr_out[r];
                        if (nadr[r] == 0) nadr[r] = pv;
                        else if (pv != nadr[r] && pv != 0) nadr[r] = 0;
                    }
                }
            }
            uint64_t nadr_in[31];
            memcpy(nadr_in, nadr, sizeof(nadr_in));
            for (int r = 0; r < 8; r++) nout[r] = nin[r];
            nmout = nmin;
            for (int k = depflow_insn_lower_bound(bin, gb->start);
                 k < bin->ninsns && bin->insns[k].address < gb->end; k++)
                depflow_apply_insn(bin, graph, &bin->insns[k], nout, nadr, &nmout, 0);

            if (memcmp(rd[i].in, nin, sizeof(nin)) != 0 ||
                memcmp(rd[i].out, nout, sizeof(nout)) != 0 ||
                memcmp(&rd[i].min_, &nmin, sizeof(nmin)) != 0 ||
                memcmp(&rd[i].mout, &nmout, sizeof(nmout)) != 0 ||
                memcmp(rd[i].adr_in, nadr_in, sizeof(nadr_in)) != 0 ||
                memcmp(rd[i].adr_out, nadr, sizeof(nadr)) != 0) {
                memcpy(rd[i].in, nin, sizeof(nin));
                memcpy(rd[i].out, nout, sizeof(nout));
                rd[i].min_ = nmin;
                rd[i].mout = nmout;
                memcpy(rd[i].adr_in, nadr_in, sizeof(nadr_in));
                memcpy(rd[i].adr_out, nadr, sizeof(nadr));
                changed = 1;
            }
        }
    }

    /* emit edges from the converged in-states */
    for (int i = 0; i < n; i++) {
        macho_block_t *gb = &bin->blocks[blk[i]];
        rd_set_t  cur[8];
        rd_mset_t mem = rd[i].min_;
        uint64_t  adr[31];
        memcpy(cur, rd[i].in, sizeof(cur));
        memcpy(adr, rd[i].adr_in, sizeof(adr));
        for (int k = depflow_insn_lower_bound(bin, gb->start);
             k < bin->ninsns && bin->insns[k].address < gb->end; k++)
            depflow_apply_insn(bin, graph, &bin->insns[k], cur, adr, &mem, 1);
    }

    free(g2l);
    free(rd);
#undef RDLI
}

/* Virtual per-function summary: virtual outputs (ret / global writes /
 * side effects) and virtual arguments (incoming direct calls), each carrying
 * origin + confidence, and the guard under which passing the value changes
 * state. Format-neutral: call graph + xrefs only, no NR. */
int depflow_build_summaries(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;

    int nfunc = bin->nfunctions > 0 ? bin->nfunctions : 1;
    if (!graph->summaries) {
        graph->summaries = (depflow_func_summary_t *)calloc(
            (size_t)nfunc, sizeof(depflow_func_summary_t));
        if (!graph->summaries) return -1;
    }

    graph->summary_count = bin->nfunctions;

    /* ---- outputs ---- */
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *f = &bin->functions[i];
        depflow_func_summary_t *s = &graph->summaries[i];
        memset(s, 0, sizeof(*s));
        s->func_idx = i;
        s->address  = f->start;
        s->dirty    = 1;
        s->confidence = 10;

        uint64_t func_node = depflow_node_id_by_address(graph, f->start);

        for (int x = 0; x < bin->nxrefs; x++) {
            macho_xref_t *xr = &bin->xrefs[x];
            if (xr->from < f->start || xr->from >= f->end) continue;

            if (xr->is_call) {
                uint64_t callee_node = xr->is_indirect
                    ? 0 : depflow_node_id_by_address(graph, xr->to);
                if (func_node && callee_node)
                    depflow_graph_add_edge_origin(graph, func_node, callee_node,
                        DEPFLOW_EDGE_CONTROL, xr->from, xr->is_conditional,
                        DEPFLOW_ORIGIN_PARAM, 50);

                if (s->nouts < DEPFLOW_MAX_VOUTS) {
                    depflow_vout_t *o = &s->outs[s->nouts++];
                    o->kind   = xr->is_indirect ? DEPFLOW_VOUT_SIDEEFFECT
                                                : DEPFLOW_VOUT_RET;
                    o->address = xr->to;
                    o->source  = xr->from;
                    if (xr->is_indirect) {
                        s->nindirect++;
                        o->origin = DEPFLOW_ORIGIN_INDIRECT; o->confidence = 30;
                        o->changes_state = 1;
                        depflow_cond_set(&o->conds[o->nconds++], DEPFLOW_COND_EXTERNAL,
                                         xr->from, 0, "indirect", 20);
                    } else {
                        macho_symbol_t *cs = find_symbol(bin, xr->to);
                        if (!cs || cs->is_undefined) {
                            s->n_extcalls++;
                            o->origin = DEPFLOW_ORIGIN_EXTCALL; o->confidence = 55;
                        } else {
                            s->ncallees++;
                            o->origin = DEPFLOW_ORIGIN_CALLEE_RET; o->confidence = 45;
                        }
                        if (xr->is_conditional && o->nconds < DEPFLOW_MAX_CONDS)
                            depflow_cond_set(&o->conds[o->nconds++], DEPFLOW_COND_EXTERNAL,
                                             xr->from, 0, "guarded call", 30);
                    }
                }
            } else if (xr->type == MACHO_XREF_TYPE_DATA) {
                if (s->nouts < DEPFLOW_MAX_VOUTS) {
                    depflow_vout_t *o = &s->outs[s->nouts++];
                    o->kind   = DEPFLOW_VOUT_GLOBAL;
                    o->address = xr->to;
                    o->source  = xr->from;
                    o->origin  = DEPFLOW_ORIGIN_GLOBAL;
                    o->confidence = 50;
                    o->changes_state = 1;
                    if (xr->is_conditional && o->nconds < DEPFLOW_MAX_CONDS)
                        depflow_cond_set(&o->conds[o->nconds++], DEPFLOW_COND_EXTERNAL,
                                         xr->from, xr->to, "conditional store", 30);
                }
            }
        }

        if (s->nindirect > 0)       s->confidence = 30;
        else if (s->n_extcalls > 0) s->confidence = 55;
        else if (s->ncallees > 0)   s->confidence = 45;
        else if (s->nouts > 0)      s->confidence = 40;
    }

    /* ---- virtual arguments (incoming direct calls) ---- */
    for (int i = 0; i < bin->nfunctions; i++) {
        depflow_func_summary_t *s = &graph->summaries[i];
        macho_function_t *f = &bin->functions[i];
        int changes_state = 0;
        for (int o = 0; o < s->nouts; o++)
            if (s->outs[o].changes_state) { changes_state = 1; break; }

        for (int x = 0; x < bin->nxrefs && s->nargs < DEPFLOW_MAX_VARGS; x++) {
            macho_xref_t *xr = &bin->xrefs[x];
            if (!xr->is_call || xr->is_indirect) continue;
            if (xr->to != f->start) continue;   /* direct entry call */

            depflow_varg_t *a = &s->args[s->nargs];
            memset(a, 0, sizeof(*a));
            a->kind = DEPFLOW_VARG_REG;
            a->reg  = -1;                       /* ABI reg unknown w/o insns */
            a->index = s->nargs;
            a->source = xr->from;
            a->origin = DEPFLOW_ORIGIN_PARAM;
            a->confidence = 50;
            a->read_before_write = 1;

            /* Passing this value can change state only under these guards. */
            if (xr->is_conditional && a->nconds < DEPFLOW_MAX_CONDS)
                depflow_cond_set(&a->conds[a->nconds++], DEPFLOW_COND_EXTERNAL,
                                 xr->from, 0, "guarded call", 30);
            if (changes_state && a->nconds < DEPFLOW_MAX_CONDS)
                depflow_cond_set(&a->conds[a->nconds++], DEPFLOW_COND_EXTERNAL,
                                 f->start, 0, "writes state", 25);
            s->nargs++;
        }

        /* instruction-level refinement (ABI args + branch guards) */
        depflow_scan_insns(bin, graph, f, s);
        depflow_build_value_edges(bin, graph, s);
    }
    return 0;
}

/* Demand-driven fixpoint over call edges: a function's return origin is the
 * join of its callees' return origins. Terminates by monotonicity (origin
 * only widens toward UNKNOWN, confidence only drops) — no path depth cap.
 * Recursion is marked RECURSION instead of being cut off. */
int depflow_summaries_fixpoint(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph || !graph->summaries) return -1;
    int n = graph->summary_count;
    if (n <= 0) return 0;

    int cap = n + 1;
    int *q = (int *)malloc(sizeof(int) * (size_t)cap);
    unsigned char *inq = (unsigned char *)calloc((size_t)n, 1);
    if (!q || !inq) { free(q); free(inq); return -1; }

    int qh = 0, qt = 0, qlen = 0;
    /* Seed in ring order: callee rings (higher Kosaraju id) before callers, so
     * each ring is solved on inputs that are already stable. The monotone
     * worklist then converges to the same unique fixpoint. */
    if (graph->ring_count > 0) {
        for (int r = graph->ring_count - 1; r >= 0; r--)
            for (int i = 0; i < n; i++)
                if (graph->summaries[i].ring_id == r) {
                    q[qt] = i; qt = (qt + 1) % cap; qlen++;
                    inq[i] = 1; graph->summaries[i].dirty = 1;
                }
        for (int i = 0; i < n; i++)
            if (graph->summaries[i].ring_id < 0) {
                q[qt] = i; qt = (qt + 1) % cap; qlen++;
                inq[i] = 1; graph->summaries[i].dirty = 1;
            }
    } else {
        for (int i = 0; i < n; i++) {
            q[qt] = i; qt = (qt + 1) % cap; qlen++;
            inq[i] = 1; graph->summaries[i].dirty = 1;
        }
    }

    unsigned long guard = (unsigned long)n * 64ul + 4096ul;  /* safety only */
    while (qlen > 0 && guard-- > 0) {
        int i = q[qh]; qh = (qh + 1) % cap; qlen--;
        inq[i] = 0;
        depflow_func_summary_t *s = &graph->summaries[i];
        if (!s->dirty) continue;
        s->dirty = 0;

        for (int k = 0; k < s->nouts; k++) {
            depflow_vout_t *o = &s->outs[k];
            if (o->origin != DEPFLOW_ORIGIN_CALLEE_RET) continue;
            int ci = depflow_find_func_idx(bin, graph, o->address);
            if (ci < 0 || ci >= n) continue;

            if (ci == i) {                       /* self recursion */
                if (o->origin != DEPFLOW_ORIGIN_RECURSION) {
                    o->origin = DEPFLOW_ORIGIN_RECURSION;
                    o->confidence = 25;
                    s->is_recursive = 1;
                    s->dirty = 1;
                }
                continue;
            }

            depflow_func_summary_t *c = &graph->summaries[ci];
            int corigin = depflow_primary_ret_origin(c);
            if (corigin == DEPFLOW_ORIGIN_UNKNOWN) continue;

            int joined  = depflow_origin_join(o->origin, corigin);
            int newconf = c->confidence - 5;
            if (newconf < 10) newconf = 10;
            if (newconf > o->confidence) newconf = o->confidence;  /* monotone */

            if (joined != o->origin || newconf < o->confidence) {
                o->origin = joined;
                o->confidence = newconf;
                s->dirty = 1;
                if (!inq[i]) { q[qt] = i; qt = (qt + 1) % cap; qlen++; inq[i] = 1; }
            }
        }
    }

    free(q);
    free(inq);
    return 0;
}

/* Precompute, once per graph, the first call in every basic block. Removes
 * the per-function rescan of calls during dominator-guard construction. */
int depflow_cache_first_calls(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;
    if (graph->first_call) return 0;   /* idempotent */

    int nb = bin->nblocks > 0 ? bin->nblocks : 1;
    graph->first_call = (depflow_first_call_t *)calloc(
        (size_t)nb, sizeof(depflow_first_call_t));
    if (!graph->first_call) return -1;
    graph->first_call_count = bin->nblocks;

    if (!bin->insns || bin->ninsns <= 0) return 0;

    for (int b = 0; b < bin->nblocks; b++) {
        macho_block_t *bl = &bin->blocks[b];
        for (int k = depflow_insn_lower_bound(bin, bl->start);
             k < bin->ninsns && bin->insns[k].address < bl->end; k++) {
            macho_insn_t *in = &bin->insns[k];
            if (!(in->is_call || in->is_indirect)) continue;
            graph->first_call[b].valid = 1;
            graph->first_call[b].addr = in->address;
            graph->first_call[b].has_target = in->has_target;
            graph->first_call[b].target = in->has_target ? in->target : 0;
            break;
        }
    }
    return 0;
}

/* Strongly-connected components of the call graph ("rings"). Functions in a
 * ring are mutually recursive, so their summaries are solved together; once
 * the ring member set is fixed the interprocedural analysis is determined.
 * Kosaraju, iterative (no recursion-depth risk). */
int depflow_build_rings(macho_binary_t *bin, depflow_graph_t *graph) {
    int nf = bin->nfunctions;
    if (nf <= 0 || !graph->summaries) return -1;
    for (int i = 0; i < nf; i++) graph->summaries[i].ring_id = -1;

    int ne = 0;
    for (int i = 0; i < nf; i++) {
        depflow_func_summary_t *s = &graph->summaries[i];
        for (int o = 0; o < s->nouts; o++)
            if (s->outs[o].kind == DEPFLOW_VOUT_RET &&
                s->outs[o].origin == DEPFLOW_ORIGIN_CALLEE_RET &&
                depflow_find_func_idx(bin, graph, s->outs[o].address) >= 0) ne++;
    }
    graph->ring_count = 0;
    if (ne == 0) return 0;

    int *ecu = malloc(sizeof(int) * (size_t)ne);
    int *ecv = malloc(sizeof(int) * (size_t)ne);
    if (!ecu || !ecv) { free(ecu); free(ecv); return -1; }
    int k = 0;
    for (int i = 0; i < nf; i++) {
        depflow_func_summary_t *s = &graph->summaries[i];
        for (int o = 0; o < s->nouts; o++) {
            if (s->outs[o].kind != DEPFLOW_VOUT_RET ||
                s->outs[o].origin != DEPFLOW_ORIGIN_CALLEE_RET) continue;
            int j = depflow_find_func_idx(bin, graph, s->outs[o].address);
            if (j < 0) continue;
            ecu[k] = i; ecv[k] = j; k++;
        }
    }
    ne = k;

    int *fc = calloc((size_t)nf + 1, sizeof(int));
    int *rc = calloc((size_t)nf + 1, sizeof(int));
    if (!fc || !rc) { free(ecu); free(ecv); free(fc); free(rc); return -1; }
    for (int e = 0; e < ne; e++) { fc[ecu[e] + 1]++; rc[ecv[e] + 1]++; }
    for (int i = 0; i < nf; i++) { fc[i + 1] += fc[i]; rc[i + 1] += rc[i]; }
    int *fwd = malloc(sizeof(int) * (size_t)ne);
    int *rev = malloc(sizeof(int) * (size_t)ne);
    int *fp = malloc(sizeof(int) * (size_t)nf);
    int *rp = malloc(sizeof(int) * (size_t)nf);
    int *order = malloc(sizeof(int) * (size_t)nf);
    int *stack = malloc(sizeof(int) * (size_t)nf);
    int *it = malloc(sizeof(int) * (size_t)nf);
    char *vis = calloc((size_t)nf, 1);
    if (!fwd || !rev || !fp || !rp || !order || !stack || !it || !vis) {
        free(ecu); free(ecv); free(fc); free(rc); free(fwd); free(rev);
        free(fp); free(rp); free(order); free(stack); free(it); free(vis);
        return -1;
    }
    memcpy(fp, fc, sizeof(int) * (size_t)nf);
    memcpy(rp, rc, sizeof(int) * (size_t)nf);
    for (int e = 0; e < ne; e++) { fwd[fp[ecu[e]]++] = ecv[e]; rev[rp[ecv[e]]++] = ecu[e]; }

    /* pass 1: finish order (iterative DFS on forward graph) */
    int no = 0;
    for (int s0 = 0; s0 < nf; s0++) {
        if (vis[s0]) continue;
        int sp = 0; stack[sp] = s0; it[sp] = fc[s0]; vis[s0] = 1; sp++;
        while (sp > 0) {
            int v = stack[sp - 1];
            if (it[sp - 1] < fc[v + 1]) {
                int w = fwd[it[sp - 1]++];
                if (!vis[w]) { vis[w] = 1; stack[sp] = w; it[sp] = fc[w]; sp++; }
            } else { order[no++] = v; sp--; }
        }
    }

    /* pass 2: reverse graph in reverse finish order */
    memset(vis, 0, (size_t)nf);
    for (int idx = no - 1; idx >= 0; idx--) {
        int s0 = order[idx];
        if (vis[s0]) continue;
        int ring = graph->ring_count, sp = 0, size = 0;
        stack[sp++] = s0; vis[s0] = 1;
        while (sp > 0) {
            int v = stack[--sp];
            graph->summaries[v].ring_id = ring;
            size++;
            for (int e = rc[v]; e < rc[v + 1]; e++) {
                int w = rev[e];
                if (!vis[w]) { vis[w] = 1; stack[sp++] = w; }
            }
        }
        if (size > 1)
            for (int v = 0; v < nf; v++)
                if (graph->summaries[v].ring_id == ring) graph->summaries[v].is_recursive = 1;
        graph->ring_count++;
    }

    free(ecu); free(ecv); free(fc); free(rc); free(fwd); free(rev);
    free(fp); free(rp); free(order); free(stack); free(it); free(vis);
    return 0;
}

/* ---------- naming / structure recovery ---------- */

static void depflow_cpp_name(const char *in, char *out, size_t n) {
    if (!out || n == 0) return;
    if (!in || !in[0]) { snprintf(out, n, "sub"); return; }
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') out[j++] = (char)c;
        else out[j++] = '_';
    }
    out[j] = '\0';
    if (out[0] >= '0' && out[0] <= '9') { memmove(out + 1, out, j + 1); out[0] = '_'; }
}

static void depflow_cpp_prefix(const char *name, char *out, size_t n) {
    size_t i = 0;
    while (name[i] && name[i] != '_' && i + 1 < n) { out[i] = name[i]; i++; }
    out[i] = '\0';
}

/* Role-based naming: classify a function from the APIs it calls and its name,
 * so the recovered class reflects behaviour, not just a symbol prefix. */
static int depflow_calls_ext(macho_binary_t *bin, depflow_graph_t *graph,
                             int i, const char *api) {
    if (!graph->summaries || i >= graph->summary_count) return 0;
    depflow_func_summary_t *s = &graph->summaries[i];
    for (int o = 0; o < s->nouts; o++) {
        if (s->outs[o].origin != DEPFLOW_ORIGIN_EXTCALL) continue;
        macho_symbol_t *sym = find_symbol(bin, s->outs[o].address);
        if (sym && strstr(sym->name, api)) return 1;
    }
    return 0;
}

int depflow_function_role(macho_binary_t *bin, depflow_graph_t *graph, int i,
                             char *out, size_t n) {
    const char *nm = bin->functions[i].name;
#define NXHAS(s) (nm && strstr(nm, (s)))
    if (depflow_calls_ext(bin, graph, i, "pthread_create") ||
        depflow_calls_ext(bin, graph, i, "dispatch_async") ||
        depflow_calls_ext(bin, graph, i, "dispatch_apply") ||
        depflow_calls_ext(bin, graph, i, "dispatch_group") ||
        depflow_calls_ext(bin, graph, i, "parallel_for") ||
        NXHAS("dispatch") || NXHAS("spawn")) { snprintf(out, n, "Dispatcher"); return 1; }
    if (depflow_calls_ext(bin, graph, i, "pthread_join") ||
        depflow_calls_ext(bin, graph, i, "dispatch_group_wait") ||
        NXHAS("join") || NXHAS("barrier") || NXHAS("sync")) { snprintf(out, n, "Sync"); return 1; }
    if (NXHAS("worker") || NXHAS("Worker")) { snprintf(out, n, "Worker"); return 1; }
    if (NXHAS("beam") || NXHAS("Beam")) { snprintf(out, n, "BeamSearch"); return 1; }
    if (NXHAS("board") || NXHAS("_ban")) { snprintf(out, n, "Board"); return 1; }
    if (NXHAS("eval") || NXHAS("score")) { snprintf(out, n, "Evaluator"); return 1; }
    if (NXHAS("move") || NXHAS("_te_")) { snprintf(out, n, "MoveGen"); return 1; }
    if (NXHAS("search") || NXHAS("node")) { snprintf(out, n, "Search"); return 1; }
    if (NXHAS("decode") || NXHAS("disasm")) { snprintf(out, n, "Decoder"); return 1; }
    if (NXHAS("parse") || NXHAS("loader")) { snprintf(out, n, "Loader"); return 1; }
#undef NXHAS

    /* Reuse the variable-role categorisation (board / move / eval / search):
     * a function touching a categorised global inherits that role even when
     * its own name is opaque. */
    if (graph->summaries && i < graph->summary_count) {
        depflow_func_summary_t *sm = &graph->summaries[i];
        for (int o = 0; o < sm->nouts; o++) {
            if (sm->outs[o].kind != DEPFLOW_VOUT_GLOBAL) continue;
            uint64_t nid = depflow_node_id_by_address(graph, sm->outs[o].address);
            if (!nid || (int)nid > graph->node_count) continue;
            int cat = graph->nodes[nid - 1].category;
            if (cat == DEPFLOW_VAR_CATEGORY_BOARD)       { snprintf(out, n, "Board");    return 1; }
            if (cat == DEPFLOW_VAR_CATEGORY_MOVE)        { snprintf(out, n, "MoveGen");  return 1; }
            if (cat == DEPFLOW_VAR_CATEGORY_SCORE)       { snprintf(out, n, "Evaluator"); return 1; }
            if (cat == DEPFLOW_VAR_CATEGORY_SEARCH_TREE ||
                cat == DEPFLOW_VAR_CATEGORY_NODE)        { snprintf(out, n, "Search");   return 1; }
        }
    }
    return 0;
}

void depflow_export_workflow(macho_binary_t *bin, depflow_graph_t *graph,
                             const char *filename) {
    if (!bin || !graph || !filename) return;
    int nf = bin->nfunctions;
    if (nf <= 0) return;
    FILE *f = fopen(filename, "w");
    if (!f) return;

    /* ---- recover a good name + class for every function ---- */
    char (*cls)[64] = (char (*)[64])calloc((size_t)nf, 64);
    char (*mth)[64] = (char (*)[64])calloc((size_t)nf, 64);
    int  *ringsz = (int *)calloc((size_t)(graph->ring_count > 0 ? graph->ring_count : 1), sizeof(int));
    if (!cls || !mth || !ringsz) { free(cls); free(mth); free(ringsz); fclose(f); return; }

    for (int i = 0; i < nf; i++) depflow_cpp_name(bin->functions[i].name, mth[i], 64);
    for (int i = 0; i < nf; i++) {
        int r = (graph->summaries && i < graph->summary_count) ? graph->summaries[i].ring_id : -1;
        if (r >= 0 && r < graph->ring_count) ringsz[r]++;
    }

    for (int i = 0; i < nf; i++) {
        int r = (graph->summaries && i < graph->summary_count) ? graph->summaries[i].ring_id : -1;
        char p[64];
        depflow_cpp_prefix(mth[i], p, 64);
        int cnt = 0;
        for (int j = 0; j < nf; j++) {
            char pj[64];
            depflow_cpp_prefix(mth[j], pj, 64);
            if (p[0] && !strcmp(p, pj)) cnt++;
        }
        char role[64];
        if (r >= 0 && r < graph->ring_count && ringsz[r] > 1) {
            snprintf(cls[i], 64, "ring_%d", r);          /* mutually recursive */
        } else if (depflow_function_role(bin, graph, i, role, sizeof(role))) {
            snprintf(cls[i], 64, "%s", role);            /* behaviour-based */
        } else if (p[0] && cnt >= 2) {
            snprintf(cls[i], 64, "%s", p);               /* shared domain prefix */
            const char *rest = strchr(mth[i], '_');
            if (rest && rest[1]) memmove(mth[i], rest + 1, strlen(rest) + 1);
        } else {
            snprintf(cls[i], 64, "misc");
        }
    }

    fprintf(f, "// neodax workflow (auto-generated)\n");
    fprintf(f, "// source: %s\n// functions: %d  rings: %d\n\n",
            bin->filepath, nf, graph->ring_count);
    fprintf(f, "#include <cstdint>\n#include <string>\n"
               "#include <functional>\n#include <initializer_list>\n\n");
    fprintf(f, "namespace nxd_workflow {\n\n");
    fprintf(f, "struct Value { uint64_t v = 0; int origin = 0; };\n");
    fprintf(f, "inline bool guard(const char*, int) { return true; }\n");
    fprintf(f, "inline Value external(const char*) { return {}; }\n");
    fprintf(f, "inline Value indirect() { return {}; }\n");
    fprintf(f, "inline void store_global(uint64_t, const Value&) {}\n");
    fprintf(f, "inline void parallel(std::initializer_list<std::function<void()>>) {}\n\n");

    /* ---- class declarations, grouped by recovered class ---- */
    for (int i = 0; i < nf; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) if (!strcmp(cls[i], cls[j])) { seen = 1; break; }
        if (seen) continue;
        fprintf(f, "class %s {\n public:\n", cls[i]);
        for (int j = i; j < nf; j++) {
            if (strcmp(cls[i], cls[j])) continue;
            int na = (graph->summaries && j < graph->summary_count) ? graph->summaries[j].nargs : 0;
            if (na < 1) na = 1;
            if (na > DEPFLOW_MAX_VARGS) na = DEPFLOW_MAX_VARGS;
            fprintf(f, "  static Value %s(", mth[j]);
            for (int a = 0; a < na; a++) fprintf(f, "%suint64_t a%d", a ? ", " : "", a);
            fprintf(f, ");\n");
        }
        fprintf(f, "};\n\n");
    }

    /* ---- definitions ---- */
    for (int i = 0; i < nf; i++) {
        depflow_func_summary_t *s = (graph->summaries && i < graph->summary_count)
                                    ? &graph->summaries[i] : NULL;
        int na = s ? s->nargs : 0;
        if (na < 1) na = 1;
        if (na > DEPFLOW_MAX_VARGS) na = DEPFLOW_MAX_VARGS;

        fprintf(f, "// [0x%llx] ring=%d%s  '%s'\n",
                (unsigned long long)bin->functions[i].start,
                s ? s->ring_id : -1,
                (s && s->is_recursive) ? " recursive" : "", bin->functions[i].name);
        fprintf(f, "Value %s::%s(", cls[i], mth[i]);
        for (int a = 0; a < na; a++) fprintf(f, "%suint64_t a%d", a ? ", " : "", a);
        fprintf(f, ") {\n");
        for (int a = 0; a < na; a++) fprintf(f, "  (void)a%d;\n", a);
        fprintf(f, "  Value r{0, %d};\n", s ? depflow_primary_ret_origin(s) : 0);

        if (s) {
            for (int o = 0; o < s->nouts; o++) {
                depflow_vout_t *out = &s->outs[o];
                char gbuf[160] = "";
                if (out->nconds > 0)
                    snprintf(gbuf, sizeof(gbuf), "if (guard(\"%s\", %d)) ",
                             out->conds[0].tag, out->conds[0].state);

                if (out->kind == DEPFLOW_VOUT_GLOBAL) {
                    fprintf(f, "  // [0x%llx] state (%s)\n",
                            (unsigned long long)out->source, depflow_origin_str(out->origin));
                    fprintf(f, "  %sstore_global(0x%llx, r);\n",
                            gbuf, (unsigned long long)out->address);
                } else if (out->kind == DEPFLOW_VOUT_SIDEEFFECT ||
                           out->origin == DEPFLOW_ORIGIN_INDIRECT) {
                    fprintf(f, "  // [0x%llx] indirect call\n", (unsigned long long)out->source);
                    fprintf(f, "  %sr = indirect();\n", gbuf);
                } else if (out->origin == DEPFLOW_ORIGIN_EXTCALL) {
                    macho_symbol_t *sym = find_symbol(bin, out->address);
                    fprintf(f, "  // [0x%llx] external '%s'\n",
                            (unsigned long long)out->source, sym ? sym->name : "?");
                    fprintf(f, "  %sr = external(\"%s\");\n", gbuf, sym ? sym->name : "?");
                } else if (out->origin == DEPFLOW_ORIGIN_CALLEE_RET) {
                    int j = depflow_find_func_idx(bin, graph, out->address);
                    if (j >= 0) {
                        int ca = (graph->summaries && j < graph->summary_count)
                                 ? graph->summaries[j].nargs : 0;
                        if (ca < 1) ca = 1;
                        if (ca > DEPFLOW_MAX_VARGS) ca = DEPFLOW_MAX_VARGS;
                        fprintf(f, "  // [0x%llx] -> %s::%s\n",
                                (unsigned long long)out->source, cls[j], mth[j]);
                        fprintf(f, "  %sr = %s::%s(", gbuf, cls[j], mth[j]);
                        for (int a = 0; a < ca; a++) {
                            if (a > 0) fprintf(f, ", ");
                            if (a < na) fprintf(f, "a%d", a);
                            else        fprintf(f, "uint64_t(0)");
                        }
                        fprintf(f, ");\n");
                    }
                }
            }
        }
        fprintf(f, "  return r;\n}\n\n");
    }

    fprintf(f, "} // namespace nxd_workflow\n");
    fclose(f);
    free(cls); free(mth); free(ringsz);
}

int depflow_build_dependency_graph(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;
    depflow_cache_first_calls(bin, graph);
    
    // Create nodes for all functions (address must be set before insertion so
    // the VA hash map can find them).
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        depflow_node_create_function(graph, i, func->start, func->name);
    }
    
    // Create nodes for all symbols
    for (int i = 0; i < bin->nsymbols; i++) {
        macho_symbol_t *sym = &bin->symbols[i];
        
        // Skip undefined symbols
        if (sym->is_undefined) continue;
        
        // Determine node type based on symbol type
        int node_type = DEPFLOW_NODE_VARIABLE;
        int category = DEPFLOW_VAR_CATEGORY_UNKNOWN;
        
        switch (sym->type) {
            case MACHO_SYMBOL_TYPE_FUNC:
                node_type = DEPFLOW_NODE_FUNCTION;
                break;
            case MACHO_SYMBOL_TYPE_OBJECT:
                node_type = DEPFLOW_NODE_VARIABLE;
                // Try to categorize based on name
                if (strstr(sym->name, "board") || strstr(sym->name, "ban")) {
                    category = DEPFLOW_VAR_CATEGORY_BOARD;
                } else if (strstr(sym->name, "move") || strstr(sym->name, "te")) {
                    category = DEPFLOW_VAR_CATEGORY_MOVE;
                } else if (strstr(sym->name, "score") || strstr(sym->name, "eval")) {
                    category = DEPFLOW_VAR_CATEGORY_SCORE;
                } else if (strstr(sym->name, "depth") || strstr(sym->name, "ply")) {
                    category = DEPFLOW_VAR_CATEGORY_DEPTH;
                } else if (strstr(sym->name, "node") || strstr(sym->name, "search")) {
                    category = DEPFLOW_VAR_CATEGORY_SEARCH_TREE;
                }
                break;
            case MACHO_SYMBOL_TYPE_IMPORT:
            case MACHO_SYMBOL_TYPE_EXPORT:
                node_type = DEPFLOW_NODE_FUNCTION;
                break;
            default:
                node_type = DEPFLOW_NODE_VARIABLE;
                break;
        }
        
        uint64_t node_id = depflow_node_create(graph, node_type, sym->address, 
                                                sym->demangled[0] ? sym->demangled : sym->name, 
                                                sym->size);
        if (node_id > 0 && node_type == DEPFLOW_NODE_VARIABLE) {
            graph->nodes[graph->node_count - 1].category = category;
        }
    }
    
    // Create edges based on cross-references
    for (int i = 0; i < bin->nxrefs; i++) {
        macho_xref_t *xref = &bin->xrefs[i];
        
        // Find source and destination nodes
        uint64_t from_node = depflow_node_id_by_address(graph, xref->from);
        uint64_t to_node = depflow_node_id_by_address(graph, xref->to);
        
        if (from_node > 0 && to_node > 0) {
            int edge_type = DEPFLOW_EDGE_CONTROL;
            int origin = DEPFLOW_ORIGIN_UNKNOWN;
            int conf = 0;
            if (xref->is_call) {
                edge_type = DEPFLOW_EDGE_CONTROL;
                if (xref->is_indirect) {
                    origin = DEPFLOW_ORIGIN_INDIRECT; conf = 30;
                } else {
                    macho_symbol_t *cs = find_symbol(bin, xref->to);
                    if (!cs || cs->is_undefined) { origin = DEPFLOW_ORIGIN_EXTCALL; conf = 70; }
                    else                          { origin = DEPFLOW_ORIGIN_CALLEE_RET; conf = 60; }
                }
            } else if (xref->type == MACHO_XREF_TYPE_DATA) {
                edge_type = DEPFLOW_EDGE_DATA;
                origin = DEPFLOW_ORIGIN_GLOBAL; conf = 50;
            }
            
            depflow_graph_add_edge_origin(graph, from_node, to_node, edge_type,
                                          xref->from, xref->is_conditional,
                                          origin, conf);
        }
    }
    
    // Create dependency edges between functions and their variables
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        uint64_t func_node = depflow_node_id_by_address(graph, func->start);
        
        if (func_node == 0) continue;
        
        // Find variables used by this function
        // This would require more detailed analysis in a real implementation
        for (int j = 0; j < bin->nsymbols; j++) {
            macho_symbol_t *sym = &bin->symbols[j];
            
            if (sym->is_undefined) continue;
            
            // Check if symbol is in the same section as function
            if (func->start <= sym->address && sym->address < func->end) {
                uint64_t var_node = depflow_node_id_by_address(graph, sym->address);
                if (var_node > 0) {
                    // Function uses this variable
                    depflow_graph_add_edge_origin(graph, func_node, var_node,
                        DEPFLOW_EDGE_USE_DEF, sym->address, 0,
                        DEPFLOW_ORIGIN_GLOBAL, 40);
                }
            }
        }
    }

    depflow_build_summaries(bin, graph);
    depflow_build_rings(bin, graph);
    depflow_summaries_fixpoint(bin, graph);
    return 0;
}

int depflow_analyze_function_dependencies(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;
    
    // Analyze each function's dependencies
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        uint64_t func_node = depflow_node_id_by_address(graph, func->start);
        
        if (func_node == 0) continue;
        
        depflow_func_deps_t *deps = &graph->func_deps[graph->func_deps_count++];
        deps->func_idx = i;
        deps->ninputs = 0;
        deps->noutputs = 0;
        deps->nreads = 0;
        deps->nwrites = 0;
        deps->ncalls = 0;
        deps->has_side_effects = 0;
        
        // Find all edges from this function
        for (int j = 0; j < graph->edge_count; j++) {
            depflow_edge_t *edge = &graph->edges[j];
            
            if (edge->from == func_node) {
                // This is an outgoing dependency
                depflow_node_t *to_node = &graph->nodes[edge->to - 1];
                
                if (to_node->type == DEPFLOW_NODE_FUNCTION) {
                    if (deps->ncalls < 256) {
                        deps->calls[deps->ncalls++] = to_node->func_idx;
                    }
                } else if (to_node->type == DEPFLOW_NODE_VARIABLE) {
                    if (edge->type == DEPFLOW_EDGE_USE_DEF) {
                        if (deps->nreads < 256) {
                            deps->reads[deps->nreads++] = edge->to;
                        }
                    } else if (edge->type == DEPFLOW_EDGE_DEF_USE) {
                        if (deps->nwrites < 256) {
                            deps->writes[deps->nwrites++] = edge->to;
                        }
                    }
                }
            } else if (edge->to == func_node) {
                // This is an incoming dependency
                depflow_node_t *from_node = &graph->nodes[edge->from - 1];
                
                if (from_node->type == DEPFLOW_NODE_VARIABLE) {
                    if (edge->type == DEPFLOW_EDGE_USE_DEF) {
                        if (deps->ninputs < 256) {
                            deps->inputs[deps->ninputs++] = edge->from;
                        }
                    }
                }
            }
        }
        
        // Determine if function has side effects
        deps->has_side_effects = (deps->nwrites > 0 || deps->ncalls > 0);
    }
    
    return 0;
}

int depflow_track_variable_usage(macho_binary_t *bin, depflow_graph_t *graph, 
                                   const char *var_name) {
    if (!bin || !graph || !var_name) return -1;
    
    // Find the variable node
    uint64_t var_node = 0;
    for (int i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i].type == DEPFLOW_NODE_VARIABLE &&
            strcmp(graph->nodes[i].name, var_name) == 0) {
            var_node = graph->nodes[i].id;
            break;
        }
    }
    
    if (var_node == 0) {
        fprintf(stderr, "depflow: Variable '%s' not found\n", var_name);
        return -1;
    }
    
    // Track all usages of this variable
    for (int i = 0; i < graph->edge_count; i++) {
        depflow_edge_t *edge = &graph->edges[i];
        
        if (edge->to == var_node && edge->type == DEPFLOW_EDGE_USE_DEF) {
            // This is a use of the variable
            depflow_var_usage_t *usage = &graph->var_usages[graph->var_usage_count++];
            usage->var_id = var_node;
            usage->instruction_addr = edge->site;
            usage->usage_type = 0; // read
            usage->is_conditional = edge->is_conditional;
            usage->timestamp = 0;
        } else if (edge->from == var_node && edge->type == DEPFLOW_EDGE_DEF_USE) {
            // This is a definition of the variable
            depflow_var_usage_t *usage = &graph->var_usages[graph->var_usage_count++];
            usage->var_id = var_node;
            usage->instruction_addr = edge->site;
            usage->usage_type = 1; // write
            usage->is_conditional = edge->is_conditional;
            usage->timestamp = 0;
        }
    }
    
    return 0;
}

// ============================================================================
// Path Finding Algorithms
// ============================================================================

int depflow_find_all_paths(depflow_graph_t *graph, uint64_t start, uint64_t end,
                             depflow_path_t *paths, int max_paths) {
    if (!graph || start == 0 || end == 0) return -1;
    
    // Simple BFS-based path finding (would use DFS in real implementation)
    int path_count = 0;
    
    // Check if start and end are valid nodes
    if (start >= graph->node_count + 1 || end >= graph->node_count + 1) {
        return -1;
    }
    
    // Simple path: direct edge
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i].from == start && graph->edges[i].to == end) {
            if (path_count < max_paths) {
                paths[path_count].path_id = path_count + 1;
                paths[path_count].start_node = start;
                paths[path_count].end_node = end;
                paths[path_count].nodes[0] = start;
                paths[path_count].nodes[1] = end;
                paths[path_count].node_count = 2;
                paths[path_count].edges[0] = i + 1;
                paths[path_count].edge_count = 1;
                paths[path_count].total_weight = graph->edges[i].weight;
                paths[path_count].is_feasible = 1;
                path_count++;
            }
        }
    }
    
    return path_count;
}

int depflow_find_shortest_path(depflow_graph_t *graph, uint64_t start, uint64_t end,
                                uint64_t *path, int *path_length) {
    if (!graph || !path || !path_length || start == 0 || end == 0) return -1;
    
    // Simple implementation: find direct edge or two-hop path
    
    // Check direct edge
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i].from == start && graph->edges[i].to == end) {
            path[0] = start;
            path[1] = end;
            *path_length = 2;
            return 0;
        }
    }
    
    // Check two-hop path
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i].from == start) {
            uint64_t mid = graph->edges[i].to;
            for (int j = 0; j < graph->edge_count; j++) {
                if (graph->edges[j].from == mid && graph->edges[j].to == end) {
                    path[0] = start;
                    path[1] = mid;
                    path[2] = end;
                    *path_length = 3;
                    return 0;
                }
            }
        }
    }
    
    return -1; // No path found
}

int depflow_find_longest_path(depflow_graph_t *graph, uint64_t start, uint64_t end,
                               uint64_t *path, int *path_length) {
    // For now, same as shortest path
    return depflow_find_shortest_path(graph, start, end, path, path_length);
}

// ============================================================================
// Execution Order Prediction
// ============================================================================

int depflow_predict_execution_order(macho_binary_t *bin, depflow_graph_t *graph,
                                     uint64_t *order, int max_order) {
    if (!bin || !graph || !order) return -1;
    
    int order_count = 0;
    
    // Simple topological sort based on dependencies
    // In a real implementation, this would use Kahn's algorithm or similar
    
    // Mark all nodes as unvisited
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i].visited = 0;
    }
    
    // Start with entry point function
    uint64_t entry_node = 0;
    for (int i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i].type == DEPFLOW_NODE_FUNCTION &&
            graph->nodes[i].address == bin->entry) {
            entry_node = graph->nodes[i].id;
            break;
        }
    }
    
    if (entry_node == 0) {
        // Use first function as entry point
        for (int i = 0; i < graph->node_count; i++) {
            if (graph->nodes[i].type == DEPFLOW_NODE_FUNCTION) {
                entry_node = graph->nodes[i].id;
                break;
            }
        }
    }
    
    if (entry_node > 0) {
        order[order_count++] = entry_node;
        graph->nodes[entry_node - 1].visited = 1;
    }
    
    // Simple BFS traversal
    for (int i = 0; i < graph->node_count && order_count < max_order; i++) {
        for (int j = 0; j < graph->edge_count; j++) {
            depflow_edge_t *edge = &graph->edges[j];
            
            // Check if source is in order and destination is not visited
            int src_in_order = 0;
            for (int k = 0; k < order_count; k++) {
                if (order[k] == edge->from) {
                    src_in_order = 1;
                    break;
                }
            }
            
            if (src_in_order && !graph->nodes[edge->to - 1].visited) {
                order[order_count++] = edge->to;
                graph->nodes[edge->to - 1].visited = 1;
            }
        }
    }
    
    return order_count;
}

int depflow_analyze_data_flow_order(macho_binary_t *bin, depflow_graph_t *graph) {
    // Analyze data flow to determine execution order
    // This would use more sophisticated algorithms in a real implementation
    return 0;
}

// ============================================================================
// Parallel Execution Detection
// ============================================================================

int depflow_detect_parallel_execution(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;
    
    // Look for patterns that indicate parallel execution
    // 1. Multiple functions with same or similar inputs
    // 2. Dispatch patterns (beam search, etc.)
    // 3. Thread creation or parallel constructs
    
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        
        // Check for parallel dispatch patterns
        if (strstr(func->name, "dispatch") || strstr(func->name, "parallel") ||
            strstr(func->name, "beam") || strstr(func->name, "thread")) {
            // This might be a parallel dispatch function
            uint64_t func_node = depflow_node_id_by_address(graph, func->start);
            if (func_node > 0) {
                // Mark as potential parallel dispatch point
                // In a real implementation, we'd analyze the function
            }
        }
    }
    
    // Analyze call patterns for parallel execution
    for (int i = 0; i < graph->func_deps_count; i++) {
        depflow_func_deps_t *deps = &graph->func_deps[i];
        
        // If a function calls multiple functions with similar inputs,
        // it might be parallel execution
        if (deps->ncalls > 1) {
            // Check if called functions have similar input dependencies
            // This would require more detailed analysis
        }
    }
    
    return 0;
}

int depflow_analyze_parallel_patterns(depflow_graph_t *graph) {
    // Analyze patterns that indicate parallel execution
    return 0;
}

// ============================================================================
// Beam Search Specific Analysis
// ============================================================================

int depflow_analyze_beam_search(depflow_graph_t *graph, macho_binary_t *bin) {
    if (!graph || !bin) return -1;
    
    // Look for beam search patterns:
    // 1. Functions that process multiple states/nodes in parallel
    // 2. Argument passing patterns
    // 3. Result collection patterns
    
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        
        // Check for beam search related names
        if (strstr(node->name, "beam") || strstr(node->name, "search") ||
            strstr(node->name, "expand") || strstr(node->name, "evaluate")) {
            // This might be part of beam search
            // In a real implementation, we'd analyze the connections
        }
    }
    
    return 0;
}

int depflow_track_beam_search_arguments(depflow_graph_t *graph, macho_binary_t *bin) {
    // Track arguments passed to beam search functions
    return 0;
}

// ============================================================================
// State Tracking for Regression Analysis
// ============================================================================

int depflow_track_state_dependencies(depflow_graph_t *graph, macho_binary_t *bin) {
    // Track state dependencies for regression analysis
    return 0;
}

int depflow_analyze_state_changes(depflow_graph_t *graph, macho_binary_t *bin) {
    // Analyze state changes and their dependencies
    return 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

uint64_t depflow_node_id_by_address(depflow_graph_t *graph, uint64_t address) {
    if (!graph || address == 0) return 0;
    unsigned h = depflow_addr_hash(address);
    for (int i = graph->addr_bucket[h]; i >= 0 && i < graph->node_count;
         i = graph->nodes[i].addr_next) {
        if (graph->nodes[i].address == address) return graph->nodes[i].id;
    }
    return 0;
}

uint64_t depflow_node_id_by_name(depflow_graph_t *graph, const char *name) {
    if (!graph || !name) return 0;
    
    for (int i = 0; i < graph->node_count; i++) {
        if (strcmp(graph->nodes[i].name, name) == 0) {
            return graph->nodes[i].id;
        }
    }
    
    return 0;
}

int depflow_is_reachable(depflow_graph_t *graph, uint64_t from, uint64_t to) {
    if (!graph || from == 0 || to == 0) return 0;
    
    // Simple BFS reachability check
    if (from >= graph->node_count + 1 || to >= graph->node_count + 1) return 0;
    
    // Mark all nodes as unvisited
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i].visited = 0;
    }
    
    // BFS from 'from' node
    uint64_t queue[DEPFLOW_MAX_NODES];
    int queue_front = 0, queue_back = 0;
    
    queue[queue_back++] = from;
    graph->nodes[from - 1].visited = 1;
    
    while (queue_front < queue_back) {
        uint64_t current = queue[queue_front++];
        
        if (current == to) {
            return 1; // Reachable
        }
        
        // Find all outgoing edges
        for (int i = 0; i < graph->edge_count; i++) {
            if (graph->edges[i].from == current && !graph->nodes[graph->edges[i].to - 1].visited) {
                queue[queue_back++] = graph->edges[i].to;
                graph->nodes[graph->edges[i].to - 1].visited = 1;
            }
        }
    }
    
    return 0; // Not reachable
}

int depflow_get_dependencies(depflow_graph_t *graph, uint64_t node_id,
                              uint64_t *deps, int max_deps) {
    if (!graph || node_id == 0 || !deps) return -1;
    
    int count = 0;
    
    for (int i = 0; i < graph->edge_count && count < max_deps; i++) {
        if (graph->edges[i].to == node_id) {
            deps[count++] = graph->edges[i].from;
        }
    }
    
    return count;
}

int depflow_get_dependents(depflow_graph_t *graph, uint64_t node_id,
                            uint64_t *dependents, int max_dependents) {
    if (!graph || node_id == 0 || !dependents) return -1;
    
    int count = 0;
    
    for (int i = 0; i < graph->edge_count && count < max_dependents; i++) {
        if (graph->edges[i].from == node_id) {
            dependents[count++] = graph->edges[i].to;
        }
    }
    
    return count;
}

// ============================================================================
// Output Functions
// ============================================================================

void depflow_print_dependency_graph(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_SECTION = c ? "\033[1;33m" : "";
    const char *C_NODE = c ? "\033[1;32m" : "";
    const char *C_EDGE = c ? "\033[1;35m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Dependency Graph ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "Nodes: %d, Edges: %d\n\n", graph->node_count, graph->edge_count);
    
    fprintf(out, "%sNodes:%s\n", C_SECTION, C_RESET);
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        fprintf(out, "  %s[%llu]%s %s (type: %d, addr: 0x%llx, size: %llu)%s\n",
               C_NODE, (unsigned long long)node->id, C_RESET,
               node->name, node->type, (unsigned long long)node->address,
               (unsigned long long)node->size, C_RESET);
        
        if (node->category != DEPFLOW_VAR_CATEGORY_UNKNOWN) {
            fprintf(out, "      Category: %d", node->category);
            switch (node->category) {
                case DEPFLOW_VAR_CATEGORY_BOARD: fprintf(out, " (Board)"); break;
                case DEPFLOW_VAR_CATEGORY_MOVE: fprintf(out, " (Move)"); break;
                case DEPFLOW_VAR_CATEGORY_SCORE: fprintf(out, " (Score)"); break;
                case DEPFLOW_VAR_CATEGORY_DEPTH: fprintf(out, " (Depth)"); break;
                case DEPFLOW_VAR_CATEGORY_NODE: fprintf(out, " (Node)"); break;
                case DEPFLOW_VAR_CATEGORY_SEARCH_TREE: fprintf(out, " (Search Tree)"); break;
                case DEPFLOW_VAR_CATEGORY_PARALLEL: fprintf(out, " (Parallel)"); break;
                case DEPFLOW_VAR_CATEGORY_MESSAGE: fprintf(out, " (Message)"); break;
                case DEPFLOW_VAR_CATEGORY_STATE: fprintf(out, " (State)"); break;
                default: fprintf(out, " (Unknown)"); break;
            }
            fprintf(out, "%s\n", C_RESET);
        }
    }
    
    fprintf(out, "\n%sEdges:%s\n", C_SECTION, C_RESET);
    for (int i = 0; i < graph->edge_count; i++) {
        depflow_edge_t *edge = &graph->edges[i];
        depflow_node_t *from = &graph->nodes[edge->from - 1];
        depflow_node_t *to = &graph->nodes[edge->to - 1];
        
        fprintf(out, "  %s[%llu -> %llu]%s type: %d, site: 0x%llx, conditional: %d%s\n",
               C_EDGE, (unsigned long long)edge->from, (unsigned long long)edge->to,
               C_RESET, edge->type, (unsigned long long)edge->site, edge->is_conditional, C_RESET);
        fprintf(out, "      %s -> %s%s\n", from->name, to->name, C_RESET);
    }
}

void depflow_print_variable_usage(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Variable Usage ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "Total usages: %d\n\n", graph->var_usage_count);
    
    for (int i = 0; i < graph->var_usage_count; i++) {
        depflow_var_usage_t *usage = &graph->var_usages[i];
        depflow_node_t *var_node = NULL;
        
        for (int j = 0; j < graph->node_count; j++) {
            if (graph->nodes[j].id == usage->var_id) {
                var_node = &graph->nodes[j];
                break;
            }
        }
        
        if (var_node) {
            const char *usage_type_str = "";
            switch (usage->usage_type) {
                case 0: usage_type_str = "Read"; break;
                case 1: usage_type_str = "Write"; break;
                case 2: usage_type_str = "Read/Write"; break;
            }
            
            fprintf(out, "  Variable: %s (0x%llx), Type: %s, Addr: 0x%llx, Conditional: %d\n",
                   var_node->name, (unsigned long long)usage->var_id,
                   usage_type_str, (unsigned long long)usage->instruction_addr,
                   usage->is_conditional);
        }
    }
}

void depflow_print_execution_order(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Execution Order ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "(Topological order based on dependencies)\n\n");
    
    // In a real implementation, we'd have a proper topological sort
    // For now, just list nodes in order
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        fprintf(out, "  %d: %s (0x%llx)\n", i + 1, node->name, (unsigned long long)node->address);
    }
}

void depflow_print_parallel_analysis(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Parallel Execution Analysis ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "(Placeholder - would show detected parallel patterns)\n\n");
}

// ============================================================================
// DOT Export Functions
// ============================================================================

void depflow_export_dot(depflow_graph_t *graph, const char *filename) {
    if (!graph || !filename) return;
    
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "depflow: Failed to open file '%s' for writing\n", filename);
        return;
    }
    
    fprintf(f, "digraph DependencyGraph {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  node [shape=box, style=filled];\n");
    fprintf(f, "  edge [color=gray40];\n\n");
    
    // Define colors for different node types
    fprintf(f, "  // Node type colors\n");
    fprintf(f, "  node [fillcolor=lightblue, color=black]; // Default\n\n");
    
    // Write nodes
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        
        const char *shape = "box";
        const char *fillcolor = "lightblue";
        const char *color = "black";
        
        switch (node->type) {
            case DEPFLOW_NODE_FUNCTION:
                shape = "ellipse";
                fillcolor = "lightgreen";
                break;
            case DEPFLOW_NODE_VARIABLE:
                shape = "box";
                fillcolor = "lightyellow";
                break;
            case DEPFLOW_NODE_REGISTER:
                shape = "diamond";
                fillcolor = "lightpink";
                break;
            case DEPFLOW_NODE_MEMORY:
                shape = "cylinder";
                fillcolor = "lightgray";
                break;
            case DEPFLOW_NODE_CONSTANT:
                shape = "plaintext";
                fillcolor = "white";
                break;
            case DEPFLOW_NODE_STACK:
                shape = "record";
                fillcolor = "lightsalmon";
                break;
        }
        
        // Escape special characters in name
        char escaped_name[256];
        int j = 0;
        for (int k = 0; node->name[k] && j < sizeof(escaped_name) - 1; k++) {
            if (node->name[k] == '"' || node->name[k] == '\\' || node->name[k] == '\n') {
                escaped_name[j++] = '\\';
            }
            escaped_name[j++] = node->name[k];
        }
        escaped_name[j] = '\0';
        
        fprintf(f, "  n%llu [label=\"%s\\n0x%llx\", shape=%s, fillcolor=%s, color=%s];\n",
               (unsigned long long)node->id, escaped_name, (unsigned long long)node->address,
               shape, fillcolor, color);
    }
    
    fprintf(f, "\n");
    
    // Write edges
    for (int i = 0; i < graph->edge_count; i++) {
        depflow_edge_t *edge = &graph->edges[i];
        
        const char *color = "gray40";
        const char *style = "solid";
        int width = 1;
        
        switch (edge->type) {
            case DEPFLOW_EDGE_DEF_USE:
                color = "blue";
                break;
            case DEPFLOW_EDGE_USE_DEF:
                color = "green";
                break;
            case DEPFLOW_EDGE_CONTROL:
                color = "red";
                width = 2;
                break;
            case DEPFLOW_EDGE_DATA:
                color = "purple";
                break;
            case DEPFLOW_EDGE_ANTI:
                color = "orange";
                style = "dashed";
                break;
            case DEPFLOW_EDGE_OUTPUT:
                color = "brown";
                break;
        }
        
        if (edge->is_conditional) {
            style = "dashed";
        }
        
        fprintf(f, "  n%llu -> n%llu [color=%s, style=%s, penwidth=%d, label=\"0x%llx\"];\n",
               (unsigned long long)edge->from, (unsigned long long)edge->to,
               color, style, width, (unsigned long long)edge->site);
    }
    
    fprintf(f, "}\n");
    
    fclose(f);
}

void depflow_export_subgraph_dot(depflow_graph_t *graph, const char *filename,
                                  uint64_t start_node, int depth) {
    // Export a subgraph starting from a specific node
    // Placeholder implementation
    depflow_export_dot(graph, filename);
}

// ============================================================================
// Shogi AI Specific Analysis
// ============================================================================

int depflow_analyze_shogi_variables(depflow_graph_t *graph, macho_binary_t *bin) {
    // Analyze variables specific to shogi AI
    return 0;
}

int depflow_track_search_tree(depflow_graph_t *graph, macho_binary_t *bin) {
    // Track search tree construction and traversal
    return 0;
}

int depflow_analyze_move_generation(depflow_graph_t *graph, macho_binary_t *bin) {
    // Analyze move generation logic
    return 0;
}
