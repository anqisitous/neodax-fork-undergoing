#ifndef DEPFLOW_H
#define DEPFLOW_H

#include "../core/macho_static.h"

// ============================================================================
// Data Dependency Flow Analysis (依存性追跡システム)
// 
// 静的解析によるデータ依存性追跡
// 変数の定義と使用の関係を追跡し、実行順序を予測
// ============================================================================

#define DEPFLOW_MAX_NODES 65536
#define DEPFLOW_MAX_EDGES 262144
#define DEPFLOW_MAX_PATHS 1024
#define DEPFLOW_MAX_DEPTH 32

// Dependency node types
#define DEPFLOW_NODE_REGISTER 0
#define DEPFLOW_NODE_MEMORY 1
#define DEPFLOW_NODE_VARIABLE 2
#define DEPFLOW_NODE_FUNCTION 3
#define DEPFLOW_NODE_CONSTANT 4
#define DEPFLOW_NODE_STACK 5

// Dependency edge types
#define DEPFLOW_EDGE_DEF_USE 0    // Definition to use
#define DEPFLOW_EDGE_USE_DEF 1    // Use to definition
#define DEPFLOW_EDGE_CONTROL 2   // Control dependency
#define DEPFLOW_EDGE_DATA 3      // Data dependency
#define DEPFLOW_EDGE_ANTI 4       // Anti-dependency
#define DEPFLOW_EDGE_OUTPUT 5    // Output dependency

// Variable categories for shogi AI
#define DEPFLOW_VAR_CATEGORY_UNKNOWN 0
#define DEPFLOW_VAR_CATEGORY_BOARD 1      // 将棋盤
#define DEPFLOW_VAR_CATEGORY_MOVE 2       // 手
#define DEPFLOW_VAR_CATEGORY_SCORE 3      // 評価値
#define DEPFLOW_VAR_CATEGORY_DEPTH 4      // 探索深さ
#define DEPFLOW_VAR_CATEGORY_NODE 5       // 探索ノード
#define DEPFLOW_VAR_CATEGORY_SEARCH_TREE 6 // 探索木
#define DEPFLOW_VAR_CATEGORY_PARALLEL 7   // 並列実行関連
#define DEPFLOW_VAR_CATEGORY_MESSAGE 8    // メッセージ
#define DEPFLOW_VAR_CATEGORY_STATE 9      // 状態

// Dependency graph node
typedef struct depflow_node_t {
    uint64_t id;                        // Unique node ID
    int type;                          // Node type (register, memory, variable, etc.)
    uint64_t address;                  // Memory address or instruction address
    char name[128];                    // Variable/function name
    uint64_t size;                     // Size in bytes
    int category;                      // Variable category for shogi AI
    uint64_t definition_site;          // Address where defined
    int is_input;                      // Is this an input node?
    int is_output;                     // Is this an output node?
    int is_modified;                   // Is this node modified?
    int visited;                       // Visited flag for traversal
    uint64_t timestamp;                 // Timestamp for ordering
    
    // For function nodes
    int func_idx;                      // Index in functions array
    
    // For register nodes
    int reg_num;                       // Register number
    int reg_class;                     // Register class (GPR, FPR, etc.)
    
    // For stack nodes
    int64_t stack_offset;              // Stack offset
    
} depflow_node_t;

// Dependency graph edge
typedef struct depflow_edge_t {
    uint64_t from;                     // Source node ID
    uint64_t to;                       // Destination node ID
    int type;                          // Edge type
    uint64_t site;                     // Instruction address where dependency occurs
    int is_conditional;                // Is this a conditional dependency?
    int is_strong;                     // Is this a strong dependency?
    uint64_t weight;                   // Edge weight for importance
    
} depflow_edge_t;

// Dependency path
typedef struct depflow_path_t {
    uint64_t path_id;
    uint64_t start_node;
    uint64_t end_node;
    uint64_t nodes[DEPFLOW_MAX_DEPTH];
    int node_count;
    uint64_t edges[DEPFLOW_MAX_DEPTH];
    int edge_count;
    uint64_t total_weight;
    int is_feasible;
    
} depflow_path_t;

// Variable usage information
typedef struct depflow_var_usage_t {
    uint64_t var_id;                   // Variable ID
    uint64_t instruction_addr;          // Instruction address
    int usage_type;                   // 0: read, 1: write, 2: read/write
    int is_conditional;                // Is usage conditional?
    uint64_t condition_addr;           // Address of condition
    uint64_t timestamp;                // Execution timestamp
    
} depflow_var_usage_t;

// Function dependency information
typedef struct depflow_func_deps_t {
    int func_idx;                      // Function index
    uint64_t inputs[256];              // Input variable/parameter IDs
    int ninputs;
    uint64_t outputs[256];             // Output variable IDs
    int noutputs;
    uint64_t reads[256];               // Variables read
    int nreads;
    uint64_t writes[256];              // Variables written
    int nwrites;
    uint64_t calls[256];               // Functions called
    int ncalls;
    int has_side_effects;              // Has side effects?
    
} depflow_func_deps_t;

// Dependency graph structure
typedef struct depflow_graph_t {
    depflow_node_t *nodes;
    int node_count;
    int node_capacity;
    
    depflow_edge_t *edges;
    int edge_count;
    int edge_capacity;
    
    // Index maps for quick lookup
    uint64_t addr_to_node[DEPFLOW_MAX_NODES];  // Address to node ID mapping
    int node_count_at_addr;
    
    // Function dependencies
    depflow_func_deps_t *func_deps;
    int func_deps_count;
    
    // Variable usage tracking
    depflow_var_usage_t *var_usages;
    int var_usage_count;
    int var_usage_capacity;
    
    // Dependency paths
    depflow_path_t *paths;
    int path_count;
    int path_capacity;
    
    // Analysis statistics
    uint64_t total_dependencies;
    uint64_t max_path_length;
    uint64_t avg_path_length;
    
} depflow_graph_t;

// Dependency analysis context
typedef struct depflow_context_t {
    macho_binary_t *bin;
    macho_opts_t *opts;
    depflow_graph_t graph;
    
    // Target for analysis
    char target_func[512];
    char target_var[256];
    int target_depth;
    
    // Analysis results
    uint64_t execution_order[DEPFLOW_MAX_NODES];
    int execution_order_count;
    
    // Parallel execution info
    uint64_t parallel_dispatch_points[256];
    int nparallel_dispatch;
    
    // Beam search specific
    uint64_t beam_search_nodes[256];
    int nbeam_search_nodes;
    uint64_t beam_search_arguments[256];
    int nbeam_search_args;
    
} depflow_context_t;

// ============================================================================
// Function Prototypes
// ============================================================================

// Graph construction
void depflow_graph_init(depflow_graph_t *graph);
void depflow_graph_free(depflow_graph_t *graph);
int depflow_graph_add_node(depflow_graph_t *graph, depflow_node_t *node);
int depflow_graph_add_edge(depflow_graph_t *graph, uint64_t from, uint64_t to, int type);
int depflow_graph_add_edge_full(depflow_graph_t *graph, uint64_t from, uint64_t to, 
                                  int type, uint64_t site, int is_conditional);

// Node management
uint64_t depflow_node_create(depflow_graph_t *graph, int type, uint64_t address, 
                              const char *name, uint64_t size);
uint64_t depflow_node_create_variable(depflow_graph_t *graph, uint64_t address,
                                         const char *name, uint64_t size, int category);
uint64_t depflow_node_create_register(depflow_graph_t *graph, uint64_t address,
                                        int reg_num, int reg_class);
uint64_t depflow_node_create_function(depflow_graph_t *graph, int func_idx);

// Dependency analysis
int depflow_analyze(macho_binary_t *bin, macho_opts_t *opts);
int depflow_build_dependency_graph(macho_binary_t *bin, depflow_graph_t *graph);
int depflow_track_variable_usage(macho_binary_t *bin, depflow_graph_t *graph, 
                                   const char *var_name);
int depflow_analyze_function_dependencies(macho_binary_t *bin, depflow_graph_t *graph);

// Path finding
int depflow_find_all_paths(depflow_graph_t *graph, uint64_t start, uint64_t end,
                             depflow_path_t *paths, int max_paths);
int depflow_find_shortest_path(depflow_graph_t *graph, uint64_t start, uint64_t end,
                                uint64_t *path, int *path_length);
int depflow_find_longest_path(depflow_graph_t *graph, uint64_t start, uint64_t end,
                               uint64_t *path, int *path_length);

// Execution order prediction
int depflow_predict_execution_order(macho_binary_t *bin, depflow_graph_t *graph,
                                     uint64_t *order, int max_order);
int depflow_analyze_data_flow_order(macho_binary_t *bin, depflow_graph_t *graph);

// Parallel execution detection
int depflow_detect_parallel_execution(macho_binary_t *bin, depflow_graph_t *graph);
int depflow_analyze_parallel_patterns(depflow_graph_t *graph);

// Beam search specific analysis
int depflow_analyze_beam_search(depflow_graph_t *graph, macho_binary_t *bin);
int depflow_track_beam_search_arguments(depflow_graph_t *graph, macho_binary_t *bin);

// State tracking for regression analysis
int depflow_track_state_dependencies(depflow_graph_t *graph, macho_binary_t *bin);
int depflow_analyze_state_changes(depflow_graph_t *graph, macho_binary_t *bin);

// Visualization and output
void depflow_print_dependency_graph(depflow_graph_t *graph, macho_opts_t *opts, FILE *out);
void depflow_print_variable_usage(depflow_graph_t *graph, macho_opts_t *opts, FILE *out);
void depflow_print_execution_order(depflow_graph_t *graph, macho_opts_t *opts, FILE *out);
void depflow_print_parallel_analysis(depflow_graph_t *graph, macho_opts_t *opts, FILE *out);

// DOT export
void depflow_export_dot(depflow_graph_t *graph, const char *filename);
void depflow_export_subgraph_dot(depflow_graph_t *graph, const char *filename,
                                  uint64_t start_node, int depth);

// Utility functions
uint64_t depflow_node_id_by_address(depflow_graph_t *graph, uint64_t address);
uint64_t depflow_node_id_by_name(depflow_graph_t *graph, const char *name);
int depflow_is_reachable(depflow_graph_t *graph, uint64_t from, uint64_t to);
int depflow_get_dependencies(depflow_graph_t *graph, uint64_t node_id,
                              uint64_t *deps, int max_deps);
int depflow_get_dependents(depflow_graph_t *graph, uint64_t node_id,
                            uint64_t *dependents, int max_dependents);

// Shogi AI specific analysis
int depflow_analyze_shogi_variables(depflow_graph_t *graph, macho_binary_t *bin);
int depflow_track_search_tree(depflow_graph_t *graph, macho_binary_t *bin);
int depflow_analyze_move_generation(depflow_graph_t *graph, macho_binary_t *bin);

#endif // DEPFLOW_H
