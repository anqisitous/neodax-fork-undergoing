#ifndef MACHO_STATIC_H
#define MACHO_STATIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Mach-O wire format (magic, CPU/file types, load commands, section/symbol
 * flags, and all on-disk structs) lives in include/macho.h. This header
 * composes those definitions instead of duplicating them.
 */
#include "macho.h"

// ============================================================================
// Mach-O Static Analysis Framework
//
// 静的解析のためのMach-Oファイル構造体と関数定義
// 将棋AIなどのバイナリ解析に特化した静的フロー解析を提供
// ============================================================================

#define MAX_MACHO_SECTIONS 256
#define MAX_MACHO_SYMBOLS 16384
#define MAX_MACHO_FUNCTIONS 8192
#define MAX_MACHO_XREFS 32768
#define MAX_MACHO_BLOCKS 65536
#define MAX_CALL_DEPTH 32
#define MAX_PARALLEL_PATHS 1024

// ============================================================================
// Static Analysis Data Structures
// ============================================================================

typedef enum {
    MACHO_ARCH_UNKNOWN = 0,
    MACHO_ARCH_X86_64,
    MACHO_ARCH_ARM64,
    MACHO_ARCH_RISCV64,
    MACHO_ARCH_X86_32
} macho_arch_t;

typedef enum {
    MACHO_SEC_TYPE_UNKNOWN = 0,
    MACHO_SEC_TYPE_CODE,
    MACHO_SEC_TYPE_DATA,
    MACHO_SEC_TYPE_RODATA,
    MACHO_SEC_TYPE_BSS,
    MACHO_SEC_TYPE_PLT,
    MACHO_SEC_TYPE_GOT,
    MACHO_SEC_TYPE_DYNAMIC,
    MACHO_SEC_TYPE_DEBUG,
    MACHO_SEC_TYPE_OTHER
} macho_sec_type_t;

typedef enum {
    MACHO_SYMBOL_TYPE_UNKNOWN = 0,
    MACHO_SYMBOL_TYPE_FUNC,
    MACHO_SYMBOL_TYPE_OBJECT,
    MACHO_SYMBOL_TYPE_IMPORT,
    MACHO_SYMBOL_TYPE_EXPORT,
    MACHO_SYMBOL_TYPE_WEAK,
    MACHO_SYMBOL_TYPE_LOCAL
} macho_symbol_type_t;

typedef enum {
    MACHO_XREF_TYPE_CALL = 0,
    MACHO_XREF_TYPE_BRANCH,
    MACHO_XREF_TYPE_DATA,
    MACHO_XREF_TYPE_JUMP_TABLE
} macho_xref_type_t;

typedef enum {
    MACHO_BLOCK_TYPE_REGULAR = 0,
    MACHO_BLOCK_TYPE_ENTRY,
    MACHO_BLOCK_TYPE_EXIT,
    MACHO_BLOCK_TYPE_CONDITIONAL,
    MACHO_BLOCK_TYPE_LOOP_HEADER,
    MACHO_BLOCK_TYPE_LOOP_BODY,
    MACHO_BLOCK_TYPE_LOOP_EXIT,
    MACHO_BLOCK_TYPE_SWITCH,
    MACHO_BLOCK_TYPE_POLYMORPHIC
} macho_block_type_t;

typedef enum {
    MACHO_EDGE_TYPE_FALL = 0,
    MACHO_EDGE_TYPE_JUMP,
    MACHO_EDGE_TYPE_COND_TRUE,
    MACHO_EDGE_TYPE_COND_FALSE,
    MACHO_EDGE_TYPE_CALL,
    MACHO_EDGE_TYPE_RET,
    MACHO_EDGE_TYPE_INDIRECT
} macho_edge_type_t;

// Section information
typedef struct {
    char segname[16];
    char name[64];
    uint64_t offset;
    uint64_t vaddr;
    uint64_t size;
    uint32_t flags;
    macho_sec_type_t type;
    uint32_t insn_count;
    uint64_t file_offset;
    int executable;
    int writable;
    int readable;
} macho_section_t;

// Symbol information
typedef struct {
    uint64_t address;
    char name[512];
    char demangled[512];
    macho_symbol_type_t type;
    uint64_t size;
    int is_entry;
    int is_global;
    int is_weak;
    int is_undefined;
    uint32_t section_idx;
} macho_symbol_t;

// Cross-reference information
typedef struct {
    uint64_t from;
    uint64_t to;
    macho_xref_type_t type;
    int is_call;
    int is_conditional;
    int is_indirect;
    uint64_t resolved_target;
} macho_xref_t;

// Function information
typedef struct {
    uint64_t start;
    uint64_t end;
    char name[512];
    int sym_idx;
    uint32_t insn_count;
    uint32_t block_count;
    uint8_t has_loops;
    uint8_t has_calls;
    uint8_t has_branches;
    uint8_t has_indirect_calls;
    uint64_t stack_frame_size;
    uint64_t local_vars_size;
    uint64_t arg_count;
    uint64_t return_type;
} macho_function_t;

// Basic block information
typedef struct {
    uint64_t start;
    uint64_t end;
    int func_idx;
    int id;
    macho_block_type_t block_type;
    int succ[16];
    int nsucc;
    int pred[16];
    int npred;
    macho_edge_type_t edge_type[16];
    int is_entry;
    int is_exit;
    int is_visited;
    uint64_t execution_count;
} macho_block_t;

// Instruction information
typedef struct {
    uint64_t address;
    uint8_t bytes[15];
    uint8_t length;
    char mnemonic[32];
    char operands[256];
    uint8_t is_call;
    uint8_t is_branch;
    uint8_t is_conditional;
    uint8_t is_indirect;
    uint8_t is_return;
    uint8_t is_nop;
    uint8_t affects_flags;
    uint8_t reads_memory;
    uint8_t writes_memory;
    uint8_t has_target;
    uint64_t target;      /* resolved direct branch/call target */
} macho_insn_t;

// Data dependency information
typedef struct {
    uint64_t address;
    int type;  // 0: register, 1: memory, 2: stack
    char name[64];
    uint64_t size;
    int is_input;
    int is_output;
    int is_modified;
} macho_dependency_t;

// Function call chain
typedef struct {
    uint64_t call_site;
    uint64_t caller;
    uint64_t callee;
    int depth;
    uint64_t timestamp;
    int is_recursive;
    int is_parallel;
    uint64_t parallel_group_id;
} macho_call_chain_t;

// Execution path
typedef struct {
    uint64_t path_id;
    uint64_t start_addr;
    uint64_t end_addr;
    uint64_t entry_point;
    int block_count;
    uint64_t blocks[MAX_CALL_DEPTH];
    int is_feasible;
    uint64_t probability;
    uint64_t execution_order;
} macho_execution_path_t;

// Parallel execution information
typedef struct {
    uint64_t dispatch_addr;
    uint64_t function_addr;
    uint64_t thread_count;
    uint64_t group_id;
    char dispatch_type[32];  // "beam_search", "parallel_for", etc.
    uint64_t argument_base;
    uint64_t argument_size;
    int is_dynamic;
    uint64_t synchronization_point;
} macho_parallel_info_t;

// Variable tracking information
typedef struct {
    uint64_t address;
    char name[128];
    uint64_t size;
    int type;  // 0: local, 1: global, 2: argument, 3: return
    uint64_t definition_site;
    uint64_t use_sites[64];
    int use_count;
    int is_modified;
    int is_read;
    int is_written;
    uint64_t lifetime_start;
    uint64_t lifetime_end;
} macho_variable_t;

// State tracking for regression analysis
typedef struct {
    uint64_t address;
    uint64_t value_hash;
    uint64_t timestamp;
    int is_checkpoint;
    char description[256];
} macho_state_checkpoint_t;

// Main binary structure
typedef struct {
    // File information
    uint8_t *data;
    size_t size;
    char filepath[1024];
    char sha256[65];

    // Architecture and format
    macho_arch_t arch;
    uint32_t filetype;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t flags;

    // Entry point
    uint64_t entry;
    uint64_t base;
    uint64_t image_size;

    // Sections
    macho_section_t sections[MAX_MACHO_SECTIONS];
    int nsections;

    // Symbols
    macho_symbol_t *symbols;
    int nsymbols;
    int symbols_allocated;

    // Functions
    macho_function_t *functions;
    int nfunctions;
    int functions_allocated;

    // Cross-references
    macho_xref_t *xrefs;
    int nxrefs;
    int xrefs_allocated;

    // Basic blocks
    macho_block_t *blocks;
    int nblocks;
    int blocks_allocated;

    // Instructions
    macho_insn_t *insns;
    int ninsns;
    int insns_allocated;

    // Dependencies
    macho_dependency_t *dependencies;
    int ndependencies;
    int dependencies_allocated;

    // Call chains
    macho_call_chain_t *call_chains;
    int ncall_chains;
    int call_chains_allocated;

    // Execution paths
    macho_execution_path_t *execution_paths;
    int nexecution_paths;
    int execution_paths_allocated;

    // Parallel execution info
    macho_parallel_info_t *parallel_info;
    int nparallel_info;
    int parallel_info_allocated;

    // Variables
    macho_variable_t *variables;
    int nvariables;
    int variables_allocated;

    // State checkpoints
    macho_state_checkpoint_t *state_checkpoints;
    int nstate_checkpoints;
    int state_checkpoints_allocated;

    // Analysis flags
    int is_analyzed;
    int is_pie;
    int is_stripped;
    int has_debug;

    // Statistics
    uint64_t total_insns;
    uint64_t code_size;
    uint64_t data_size;

} macho_binary_t;

// Analysis options
typedef struct {
    int show_bytes;
    int show_addr;
    int color;
    int verbose;
    int symbols;
    int xrefs;
    int groups;
    int strings;
    int funcs;
    int demangle;
    int all_sections;
    int cfg;
    int loops;
    int callgraph;
    int switches;
    int interactive;
    int unicode;
    int symexec;
    int ssa;
    int decompile;
    int emulate;
    int entropy;
    int rda;
    int ivf;

    // New analysis options for static flow
    int depflow;                    // Data dependency flow analysis
    char depflow_target_func[512]; // Target function for dependency analysis
    char depflow_target_var[256];  // Target variable for tracking
    int depflow_depth;              // Maximum depth for dependency tracking
    char depflow_dot[1024];         // Output file for DOT format

    int execution_order;           // Analyze execution order
    int parallel_detection;        // Detect parallel execution patterns
    int state_tracking;            // Track state changes
    int regression_analysis;       // Analyze regression in state
    int beam_search_analysis;      // Special analysis for beam search patterns

    char output_dir[1024];         // Output directory for analysis results
    char analysis_report[1024];    // Report file path

} macho_opts_t;

// ============================================================================
// Function Prototypes
// ============================================================================

// Mach-O parsing (composed from the canonical parser in src/macho.c)
int macho_parse(macho_binary_t *bin, const uint8_t *data, size_t size);

// Decode executable sections into bin->insns (idempotent; via nx_decode)
int macho_fill_insns(macho_binary_t *bin);

// Static analysis
int macho_analyze(macho_binary_t *bin, macho_opts_t *opts);
int macho_detect_functions(macho_binary_t *bin);
int macho_build_cfg(macho_binary_t *bin);
int macho_build_callgraph(macho_binary_t *bin);
int macho_detect_loops(macho_binary_t *bin);
int macho_detect_switch_tables(macho_binary_t *bin);

// Data dependency analysis
int macho_analyze_dependencies(macho_binary_t *bin, macho_opts_t *opts);
int macho_track_variable_usage(macho_binary_t *bin, const char *var_name);
int macho_build_dependency_graph(macho_binary_t *bin);

// Execution flow analysis
int macho_analyze_execution_order(macho_binary_t *bin);
int macho_detect_parallel_execution(macho_binary_t *bin);
int macho_analyze_beam_search_patterns(macho_binary_t *bin);
int macho_predict_execution_sequence(macho_binary_t *bin);

// State tracking
int macho_track_state_changes(macho_binary_t *bin);
int macho_analyze_regression(macho_binary_t *bin);
int macho_build_state_checkpoints(macho_binary_t *bin);

// Utility functions
void macho_free(macho_binary_t *bin);
void macho_init(macho_binary_t *bin);
int macho_is_valid(macho_binary_t *bin);

// Output functions
void macho_print_analysis_report(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_dependency_graph(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_execution_flow(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_parallel_analysis(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_state_tracking(macho_binary_t *bin, macho_opts_t *opts, FILE *out);

// DOT output for visualization
void macho_export_cfg_dot(macho_binary_t *bin, const char *filename);
void macho_export_callgraph_dot(macho_binary_t *bin, const char *filename);
void macho_export_dependency_dot(macho_binary_t *bin, const char *filename);

#endif // MACHO_STATIC_H
