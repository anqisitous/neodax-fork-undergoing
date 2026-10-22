#ifndef EXECUTION_ORDER_H
#define EXECUTION_ORDER_H

#include "../core/macho_static.h"
#include "../analysis/depflow.h"

// ============================================================================
// Execution Order Prediction Module
// 
// 実行順序予測モジュール
// 静的解析に基づいて関数・変数の実行順序を予測
// ============================================================================

#define EXEC_ORDER_MAX_NODES 65536
#define EXEC_ORDER_MAX_EDGES 262144
#define EXEC_ORDER_MAX_PATHS 1024
#define EXEC_ORDER_MAX_DEPTH 64

// Execution order types
#define EXEC_ORDER_TYPE_SEQUENTIAL 0
#define EXEC_ORDER_TYPE_PARALLEL 1
#define EXEC_ORDER_TYPE_CONDITIONAL 2
#define EXEC_ORDER_TYPE_LOOP 3
#define EXEC_ORDER_TYPE_RECURSIVE 4

// Ordering constraints
#define EXEC_CONSTRAINT_BEFORE 0
#define EXEC_CONSTRAINT_AFTER 1
#define EXEC_CONSTRAINT_CONCURRENT 2
#define EXEC_CONSTRAINT_MUTEX 3

// Execution context
typedef struct exec_context_t {
    uint64_t id;
    char name[128];
    uint64_t start_addr;
    uint64_t end_addr;
    int type;  // SEQUENTIAL, PARALLEL, CONDITIONAL, etc.
    int depth;
    uint64_t parent_context;
    uint64_t timestamp;
    
} exec_context_t;

// Execution node (function, basic block, or instruction)
typedef struct exec_node_t {
    uint64_t id;
    uint64_t address;
    char name[128];
    int type;  // 0: function, 1: block, 2: instruction
    int func_idx;  // Index in functions array
    int block_idx; // Index in blocks array
    uint64_t size;
    
    // Execution properties
    uint64_t earliest_start;
    uint64_t latest_start;
    uint64_t earliest_finish;
    uint64_t latest_finish;
    int is_entry;
    int is_exit;
    int is_parallel;
    int is_conditional;
    
    // Dependencies
    uint64_t dependencies[64];
    int ndependencies;
    uint64_t dependents[64];
    int ndependents;
    
    // Execution order
    int execution_order;
    int critical_path_order;
    
    // Context
    uint64_t context_id;
    
} exec_node_t;

// Execution edge
typedef struct exec_edge_t {
    uint64_t from;
    uint64_t to;
    int type;  // Control flow, data flow, etc.
    int is_conditional;
    int is_parallel;
    uint64_t weight;
    uint64_t delay;
    
} exec_edge_t;

// Execution constraint
typedef struct exec_constraint_t {
    uint64_t node_a;
    uint64_t node_b;
    int type;  // BEFORE, AFTER, CONCURRENT, MUTEX
    int is_hard;  // Hard constraint (must be satisfied)
    int is_soft;  // Soft constraint (preferred but not required)
    uint64_t priority;
    
} exec_constraint_t;

// Execution path
typedef struct exec_path_t {
    uint64_t path_id;
    uint64_t nodes[EXEC_ORDER_MAX_DEPTH];
    int node_count;
    uint64_t total_weight;
    uint64_t total_delay;
    int is_feasible;
    int is_critical;
    
} exec_path_t;

// Parallel execution group
typedef struct exec_parallel_group_t {
    uint64_t group_id;
    uint64_t nodes[256];
    int node_count;
    uint64_t dispatch_addr;
    uint64_t join_addr;
    int is_beam_search;
    int is_dynamic;
    uint64_t thread_count;
    char dispatch_type[64];
    
} exec_parallel_group_t;

// Execution order graph
typedef struct exec_order_graph_t {
    exec_node_t *nodes;
    int node_count;
    int node_capacity;
    
    exec_edge_t *edges;
    int edge_count;
    int edge_capacity;
    
    exec_constraint_t *constraints;
    int constraint_count;
    int constraint_capacity;
    
    exec_path_t *paths;
    int path_count;
    int path_capacity;
    
    exec_parallel_group_t *parallel_groups;
    int parallel_group_count;
    int parallel_group_capacity;
    
    exec_context_t *contexts;
    int context_count;
    int context_capacity;
    
    // Analysis results
    uint64_t entry_node;
    uint64_t exit_node;
    uint64_t critical_path_length;
    uint64_t total_execution_time;
    uint64_t parallel_execution_time;
    
} exec_order_graph_t;

// Execution order analysis options
typedef struct exec_order_opts_t {
    int enable_parallel_detection;
    int enable_conditional_analysis;
    int enable_loop_analysis;
    int enable_recursion_analysis;
    int enable_beam_search_analysis;
    int enable_state_tracking;
    int max_depth;
    int max_paths;
    
    // Beam search specific
    int beam_width;
    int beam_depth;
    uint64_t beam_dispatch_addr;
    
} exec_order_opts_t;

// ============================================================================
// Function Prototypes
// ============================================================================

// Graph construction
void exec_order_graph_init(exec_order_graph_t *graph);
void exec_order_graph_free(exec_order_graph_t *graph);
int exec_order_graph_add_node(exec_order_graph_t *graph, exec_node_t *node);
int exec_order_graph_add_edge(exec_order_graph_t *graph, uint64_t from, uint64_t to, int type);
int exec_order_graph_add_constraint(exec_order_graph_t *graph, uint64_t a, uint64_t b, int type);

// Node creation
uint64_t exec_order_node_create_function(exec_order_graph_t *graph, int func_idx);
uint64_t exec_order_node_create_block(exec_order_graph_t *graph, int block_idx);
uint64_t exec_order_node_create_instruction(exec_order_graph_t *graph, uint64_t addr);

// Main analysis functions
int exec_order_analyze(macho_binary_t *bin, depflow_graph_t *dep_graph, exec_order_opts_t *opts);
int exec_order_build_graph(macho_binary_t *bin, depflow_graph_t *dep_graph, exec_order_graph_t *exec_graph);
int exec_order_analyze_dependencies(exec_order_graph_t *exec_graph, depflow_graph_t *dep_graph);

// Order prediction algorithms
int exec_order_topological_sort(exec_order_graph_t *graph, uint64_t *order, int max_order);
int exec_order_critical_path_analysis(exec_order_graph_t *graph);
int exec_order_asap_analysis(exec_order_graph_t *graph);  // As Soon As Possible
int exec_order_alap_analysis(exec_order_graph_t *graph);  // As Late As Possible

// Parallel execution detection
int exec_order_detect_parallel(exec_order_graph_t *graph);
int exec_order_analyze_parallel_patterns(exec_order_graph_t *graph);
int exec_order_identify_parallel_groups(exec_order_graph_t *graph);

// Beam search specific analysis
int exec_order_analyze_beam_search(exec_order_graph_t *graph, macho_binary_t *bin);
int exec_order_track_beam_dispatch(exec_order_graph_t *graph, uint64_t dispatch_addr);
int exec_order_analyze_beam_parallelism(exec_order_graph_t *graph);

// Conditional execution analysis
int exec_order_analyze_conditional(exec_order_graph_t *graph);
int exec_order_analyze_branches(exec_order_graph_t *graph);

// Loop analysis
int exec_order_analyze_loops(exec_order_graph_t *graph);
int exec_order_analyze_loop_dependencies(exec_order_graph_t *graph);

// Recursion analysis
int exec_order_analyze_recursion(exec_order_graph_t *graph);
int exec_order_detect_recursive_patterns(exec_order_graph_t *graph);

// State tracking
int exec_order_track_state_changes(exec_order_graph_t *graph, macho_binary_t *bin);
int exec_order_analyze_state_dependencies(exec_order_graph_t *graph);

// Visualization and output
void exec_order_print_graph(exec_order_graph_t *graph, macho_opts_t *opts, FILE *out);
void exec_order_print_execution_sequence(exec_order_graph_t *graph, macho_opts_t *opts, FILE *out);
void exec_order_print_parallel_analysis(exec_order_graph_t *graph, macho_opts_t *opts, FILE *out);
void exec_order_print_critical_path(exec_order_graph_t *graph, macho_opts_t *opts, FILE *out);

// DOT export
void exec_order_export_dot(exec_order_graph_t *graph, const char *filename);
void exec_order_export_parallel_dot(exec_order_graph_t *graph, const char *filename);

// Utility functions
uint64_t exec_order_node_id_by_address(exec_order_graph_t *graph, uint64_t address);
uint64_t exec_order_node_id_by_name(exec_order_graph_t *graph, const char *name);
int exec_order_is_reachable(exec_order_graph_t *graph, uint64_t from, uint64_t to);
int exec_order_get_predecessors(exec_order_graph_t *graph, uint64_t node_id, uint64_t *preds, int max_preds);
int exec_order_get_successors(exec_order_graph_t *graph, uint64_t node_id, uint64_t *succs, int max_succs);

// Execution order prediction for shogi AI
int exec_order_analyze_shogi_search(exec_order_graph_t *graph, macho_binary_t *bin);
int exec_order_predict_move_generation_order(exec_order_graph_t *graph, macho_binary_t *bin);
int exec_order_analyze_evaluation_order(exec_order_graph_t *graph, macho_binary_t *bin);

#endif // EXECUTION_ORDER_H
