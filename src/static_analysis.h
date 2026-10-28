#ifndef NEODAX_STATIC_ANALYSIS_H
#define NEODAX_STATIC_ANALYSIS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include "dax.h"

/* ============================================================================
 * NeoDAX Unified Static Analysis
 *
 * Class-based design (C). One Regression Ring with nested Groups, each
 * containing Functions as leaves. Follows Bend language principles:
 *   - Regression Ring (環): analysis loops over phases, evidence accumulates
 *   - Progression (前進): each iteration pushes the frontier outward
 *   - Deterministic parallel: order and conditions are statically preserved
 *
 * Flow: Mach-O parse → Function detection → Variable/dependency tracing
 *       → Execution order prediction → Parallel/beam detection → State tracking
 *       → Regression ring build → SVG output
 * ============================================================================ */

/* ---------- Limits ---------- */
#define ND_STATIC_MAX_RINGS       1
#define ND_STATIC_MAX_ITERATIONS  4
#define ND_STATIC_MAX_GROUPS      64
#define ND_STATIC_MAX_FUNCTIONS   4096
#define ND_STATIC_MAX_VARIABLES   8192
#define ND_STATIC_MAX_EDGES       65536
#define ND_STATIC_MAX_TRACE_DEPTH 32
#define ND_STATIC_MAX_NAME        128
#define ND_STATIC_MAX_COND        128

/* ---------- Types ---------- */

typedef enum {
    ND_PHASE_SURVEY   = 0,   /* binary structure: sections, symbols, functions */
    ND_PHASE_CLASSIFY,       /* variable role classification */
    ND_PHASE_TRACE,          /* dependency tracing: def-use, call edges */
    ND_PHASE_PREDICT,        /* execution order: topological / critical path */
    ND_PHASE_PARALLEL,       /* parallel / beam search groups */
    ND_PHASE_VERIFY,         /* state checkpoints, regression */
    ND_PHASE_MAX
} nd_phase_t;

typedef enum {
    ND_GROUP_SEQUENTIAL = 0, /* functions called in order */
    ND_GROUP_PARALLEL,       /* functions dispatched concurrently */
    ND_GROUP_CONDITIONAL,    /* functions gated on a condition/message */
    ND_GROUP_BEAM_SEARCH,    /* beam search dispatch group */
    ND_GROUP_RECURSIVE       /* recursive function group */
} nd_group_type_t;

typedef enum {
    ND_EDGE_CALL      = 0,   /* function → function call */
    ND_EDGE_DEF_USE,           /* variable definition → use */
    ND_EDGE_USE_DEF,           /* use → definition */
    ND_EDGE_CONTROL,           /* control dependency (branch) */
    ND_EDGE_DATA,              /* data dependency */
    ND_EDGE_MESSAGE            /* message-passing / dispatch condition */
} nd_edge_type_t;

typedef enum {
    ND_VAR_GLOBAL   = 0,
    ND_VAR_LOCAL    = 1,
    ND_VAR_ARGUMENT = 2,
    ND_VAR_RETURN   = 3,
    ND_VAR_REGISTER = 4,
    ND_VAR_STACK    = 5
} nd_var_type_t;

/* ---------- Variable ---------- */

typedef struct {
    uint64_t id;
    uint64_t address;
    char     name[ND_STATIC_MAX_NAME];
    char     demangled[ND_STATIC_MAX_NAME];
    nd_var_type_t type;
    uint64_t size;
    uint64_t definition_site;   /* address where defined */
    uint64_t last_modification;
    int      is_modified;
    int      is_read;
    int      is_written;
    int      read_count;
    int      write_count;
    uint64_t lifetime_start;
    uint64_t lifetime_end;
    /* Which functions read/write this variable */
    uint64_t readers[64];
    int      nreaders;
    uint64_t writers[64];
    int      nwriters;
} nd_variable_t;

/* ---------- Function (leaf in group) ---------- */

typedef struct {
    uint64_t id;
    uint64_t start;
    uint64_t end;
    char     name[ND_STATIC_MAX_NAME];
    char     demangled[ND_STATIC_MAX_NAME];
    int      sym_idx;
    uint32_t insn_count;
    uint32_t block_count;
    int      has_loops;
    int      has_calls;
    int      has_branches;
    int      has_indirect_calls;
    /* Execution order properties */
    int      execution_order;       /* topological order index */
    int      earliest_start;        /* ASAP */
    int      latest_start;          /* ALAP */
    int      is_entry;
    int      is_exit;
    int      is_parallel;           /* runs in parallel group */
    int      is_conditional;        /* gated on condition */
    /* Which variables this function reads/writes */
    uint64_t reads[64];
    int      nreads;
    uint64_t writes[64];
    int      nwrites;
    uint64_t calls[64];             /* callee function IDs */
    int      ncalls;
    /* Parallel group membership */
    uint64_t group_id;
    char     condition_var[ND_STATIC_MAX_NAME]; /* variable that gates this */
    char     condition_msg[ND_STATIC_MAX_NAME]; /* message that triggers this */
} nd_function_t;

/* ---------- Edge ---------- */

typedef struct {
    uint64_t id;
    uint64_t from;      /* source node id (function or variable) */
    uint64_t to;        /* target node id */
    nd_edge_type_t type;
    uint64_t site;      /* instruction address where edge occurs */
    int      is_conditional;
    int      is_strong;
    uint64_t weight;
    char     condition[ND_STATIC_MAX_COND]; /* condition text (e.g. "msg == X") */
} nd_edge_t;

/* ---------- Group (contains functions) ---------- */

typedef struct {
    uint64_t id;
    char     name[ND_STATIC_MAX_NAME];
    nd_group_type_t type;
    uint64_t dispatch_addr;   /* where group is dispatched from */
    uint64_t join_addr;       /* where group rejoins */
    uint64_t functions[ND_STATIC_MAX_FUNCTIONS];
    int      nfunctions;
    uint64_t dispatcher_func; /* function that dispatches this group */
    uint64_t sync_point;      /* synchronization address */
    int      is_dynamic;
    uint64_t beam_width;      /* for beam search groups */
    uint64_t max_parallelism;
    /* Condition: what message/var triggers this group */
    char     condition_var[ND_STATIC_MAX_NAME];
    char     condition_msg[ND_STATIC_MAX_NAME];
    int      condition_conditional; /* 1 if group only runs on condition */
} nd_group_t;

/* ---------- Ring (one ring, multiple iterations) ---------- */

typedef struct {
    uint64_t id;
    int      iteration;           /* 0 = first lap */
    int      phase;               /* ND_PHASE_* */
    uint32_t evidence;            /* evidence count at this node */
    int      confidence;          /* 0-100 */
    uint64_t sample_addr;
    char     sample_name[ND_STATIC_MAX_NAME];
    char     detail[256];
    double   x, y;                /* SVG layout */
    double   r;
} nd_ring_cell_t;

typedef struct {
    char     binary[512];
    char     arch[32];
    int      iterations;
    int      phase_count;
    int      ncell;
    nd_ring_cell_t cells[ND_STATIC_MAX_ITERATIONS * ND_PHASE_MAX];

    /* Aggregates */
    uint64_t sections, symbols, functions, xrefs;
    uint64_t variables, groups;
    uint64_t dep_nodes, dep_edges;
    uint64_t exec_nodes, exec_edges, exec_order_len;
    uint64_t parallel_groups, beam_searches, dispatch_points;
    uint64_t checkpoints, changes, regressions;

    uint64_t total_evidence;
    double   avg_confidence;
} nd_ring_t;

/* ---------- Flow Tracer: variable → caller chain ---------- */

typedef struct {
    uint64_t var_id;
    char     var_name[ND_STATIC_MAX_NAME];
    uint64_t chain[ND_STATIC_MAX_TRACE_DEPTH]; /* function IDs */
    int      chain_len;
    int      is_conditional;
    char     condition[ND_STATIC_MAX_COND];
    uint64_t final_caller;     /* direct caller at depth 0 */
    uint64_t root_caller;      /* topmost caller */
} nd_var_flow_t;

typedef struct {
    nd_var_flow_t *flows;
    int            count;
    int            capacity;
} nd_var_flow_list_t;

/* ---------- Unified Static Analysis Context ---------- */

typedef struct {
    /* Input binary */
    dax_binary_t  *bin;
    dax_opts_t    *opts;

    /* Classes (instances) */
    nd_variable_t *variables;
    int            nvariables;
    nd_function_t  *functions;
    int            nfunctions;
    nd_edge_t      *edges;
    int            nedges;
    nd_group_t     *groups;
    int            ngroups;
    nd_ring_t      ring;

    /* Flow tracing results */
    nd_var_flow_list_t var_flows;

    /* Analysis flags */
    int  advanced_analysis;
    int  trace_depth;
    int  emit_svg;
    char svg_path[512];

    /* Last decoded instruction — used for the debug reference trace. */
    char last_insn[192];
} nd_static_ctx_t;

/* ============================================================================
 * Class Constructors / Destructors
 * ============================================================================ */

void nd_static_ctx_init(nd_static_ctx_t *ctx, dax_binary_t *bin, dax_opts_t *opts);
void nd_static_ctx_free(nd_static_ctx_t *ctx);

/* Variable class methods */
int nd_variable_add(nd_static_ctx_t *ctx, uint64_t address, const char *name,
                    nd_var_type_t type, uint64_t size);
int nd_variable_find_by_name(nd_static_ctx_t *ctx, const char *name);
int nd_variable_find_by_address(nd_static_ctx_t *ctx, uint64_t address);

/* Function class methods */
int nd_function_add(nd_static_ctx_t *ctx, uint64_t start, uint64_t end,
                    const char *name, int sym_idx);
int nd_function_find_by_name(nd_static_ctx_t *ctx, const char *name);
int nd_function_find_by_address(nd_static_ctx_t *ctx, uint64_t address);

/* Edge class methods */
int nd_edge_add(nd_static_ctx_t *ctx, uint64_t from, uint64_t to,
                nd_edge_type_t type, uint64_t site, int is_conditional,
                const char *condition);

/* Group class methods */
int nd_group_add(nd_static_ctx_t *ctx, const char *name, nd_group_type_t type,
                 uint64_t dispatch_addr, uint64_t join_addr);
int nd_group_add_function(nd_static_ctx_t *ctx, uint64_t group_id, uint64_t func_id);

/* Ring class methods */
void nd_ring_init(nd_ring_t *ring, const char *binary, const char *arch);
int nd_ring_build(nd_static_ctx_t *ctx);
void nd_ring_print(const nd_ring_t *ring, FILE *out);
/* nd_static_write_svg — ring → group → function hierarchy. */
int  nd_static_write_svg(nd_static_ctx_t *ctx, const char *path);

/* ============================================================================
 * Analysis Pipeline (one ring, multiple phases)
 * ============================================================================ */

int nd_static_analyze(nd_static_ctx_t *ctx);   /* run full pipeline */
int nd_static_trace_flow(nd_static_ctx_t *ctx); /* variable → caller chains */
int nd_static_predict_order(nd_static_ctx_t *ctx); /* execution order */
int nd_static_detect_parallel(nd_static_ctx_t *ctx); /* parallel groups */
int nd_static_detect_beam_search(nd_static_ctx_t *ctx); /* beam search groups */

/* ============================================================================
 * Output
 * ============================================================================ */

void nd_static_print_report(nd_static_ctx_t *ctx, FILE *out);
void nd_static_print_flow(nd_static_ctx_t *ctx, FILE *out);
void nd_static_print_order(nd_static_ctx_t *ctx, FILE *out);
void nd_static_print_parallel(nd_static_ctx_t *ctx, FILE *out);

#endif /* NEODAX_STATIC_ANALYSIS_H */