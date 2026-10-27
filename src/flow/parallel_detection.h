#ifndef PARALLEL_DETECTION_H
#define PARALLEL_DETECTION_H

#include <string.h>
#include "../core/macho_static.h"
#include "../analysis/depflow.h"
#include "execution_order.h"

// ============================================================================
// Parallel Execution Detection Module
// 
// 並列実行検出モジュール
// 静的解析に基づいて並列実行パターンを検出
// ============================================================================

#define PARALLEL_MAX_GROUPS 1024
#define PARALLEL_MAX_NODES_PER_GROUP 256
#define PARALLEL_MAX_DISPATCH_POINTS 512

// Parallel execution types
#define PARALLEL_TYPE_UNKNOWN 0
#define PARALLEL_TYPE_BEAM_SEARCH 1
#define PARALLEL_TYPE_THREAD_POOL 2
#define PARALLEL_TYPE_TASK_PARALLEL 3
#define PARALLEL_TYPE_DATA_PARALLEL 4
#define PARALLEL_TYPE_PIPELINE 5
#define PARALLEL_TYPE_MAP_REDUCE 6

// Parallel dispatch patterns
#define PARALLEL_DISPATCH_DIRECT 0
#define PARALLEL_DISPATCH_INDIRECT 1
#define PARALLEL_DISPATCH_DYNAMIC 2
#define PARALLEL_DISPATCH_WORK_QUEUE 3

// Parallel synchronization types
#define PARALLEL_SYNC_NONE 0
#define PARALLEL_SYNC_BARRIER 1
#define PARALLEL_SYNC_LOCK 2
#define PARALLEL_SYNC_SEMAPHORE 3
#define PARALLEL_SYNC_ATOMIC 4
#define PARALLEL_SYNC_FUTURE 5

// Parallel node information
typedef struct parallel_node_t {
    uint64_t id;
    uint64_t address;
    char name[128];
    int type;  // Function, block, instruction
    int is_dispatcher;
    int is_worker;
    int is_synchronization;
    uint64_t group_id;
    int role;  // Role in parallel execution
    
} parallel_node_t;

// Parallel group information
typedef struct parallel_group_t {
    uint64_t group_id;
    char name[128];
    int type;  // Beam search, thread pool, etc.
    uint64_t dispatch_addr;
    uint64_t join_addr;
    uint64_t nodes[PARALLEL_MAX_NODES_PER_GROUP];
    int node_count;
    uint64_t dispatcher_node;
    uint64_t worker_nodes[PARALLEL_MAX_NODES_PER_GROUP];
    int worker_count;
    uint64_t sync_points[32];
    int sync_count;
    int dispatch_type;
    int sync_type;
    int is_dynamic;
    uint64_t beam_width;
    uint64_t max_parallelism;
    char dispatch_function[128];
    char join_function[128];
    
} parallel_group_t;

// Parallel dispatch point
typedef struct parallel_dispatch_t {
    uint64_t addr;
    char func_name[128];
    uint64_t group_id;
    int type;
    uint64_t timestamp;
    uint64_t arguments[16];
    int arg_count;
    int is_conditional;
    char condition_var[128];
    
} parallel_dispatch_t;

// Parallel execution context
typedef struct parallel_context_t {
    uint64_t context_id;
    char name[128];
    uint64_t start_addr;
    uint64_t end_addr;
    int depth;
    uint64_t parent_context;
    
} parallel_context_t;

// Beam search specific information
typedef struct beam_search_info_t {
    uint64_t dispatch_addr;
    uint64_t evaluate_addr;
    uint64_t expand_addr;
    uint64_t select_addr;
    uint64_t beam_width;
    uint64_t max_depth;
    char dispatch_function[128];
    char state_type[128];
    char node_type[128];
    char move_type[128];
    uint64_t state_size;
    uint64_t node_size;
    int is_minimax;
    int is_expectimax;
    int is_mcts;
    int is_parallel;
    
    // Argument tracking
    uint64_t state_arg_addr;
    uint64_t moves_arg_addr;
    uint64_t depth_arg_addr;
    uint64_t result_arg_addr;
    
    // Return value tracking
    uint64_t best_move_addr;
    uint64_t best_score_addr;
    
} beam_search_info_t;

// Parallel analysis results
typedef struct parallel_analysis_t {
    parallel_group_t *groups;
    int group_count;
    int group_capacity;
    
    parallel_dispatch_t *dispatch_points;
    int dispatch_count;
    int dispatch_capacity;
    
    parallel_context_t *contexts;
    int context_count;
    int context_capacity;
    
    beam_search_info_t *beam_searches;
    int beam_search_count;
    int beam_search_capacity;
    
    // Statistics
    uint64_t total_parallel_nodes;
    uint64_t total_parallel_groups;
    uint64_t total_dispatch_points;
    uint64_t max_parallelism;
    
} parallel_analysis_t;

// Parallel detection options
typedef struct parallel_opts_t {
    int detect_beam_search;
    int detect_thread_creation;
    int detect_dispatch_patterns;
    int detect_synchronization;
    int detect_data_parallelism;
    int detect_pipeline_parallelism;
    int max_depth;
    int min_group_size;
    
    // Beam search specific
    uint64_t beam_dispatch_addr;
    char beam_dispatch_func[128];
    
} parallel_opts_t;

// ============================================================================
// Function Prototypes
// ============================================================================

// Initialization and cleanup
void parallel_analysis_init(parallel_analysis_t *analysis);
void parallel_analysis_free(parallel_analysis_t *analysis);

// Main analysis function
int parallel_analyze(macho_binary_t *bin, depflow_graph_t *dep_graph, 
                    exec_order_graph_t *exec_graph, parallel_opts_t *opts);

// Parallel group detection
int parallel_detect_groups(macho_binary_t *bin, depflow_graph_t *dep_graph, 
                           exec_order_graph_t *exec_graph, parallel_analysis_t *analysis);
int parallel_identify_group_type(parallel_group_t *group, macho_binary_t *bin, 
                                depflow_graph_t *dep_graph);

// Beam search detection
int parallel_detect_beam_search(macho_binary_t *bin, depflow_graph_t *dep_graph,
                                exec_order_graph_t *exec_graph, parallel_analysis_t *analysis);
int parallel_analyze_beam_search_patterns(macho_binary_t *bin, parallel_analysis_t *analysis);
int parallel_track_beam_search_arguments(macho_binary_t *bin, parallel_analysis_t *analysis);
int parallel_identify_beam_search_functions(macho_binary_t *bin, parallel_analysis_t *analysis);

// Dispatch pattern detection
int parallel_detect_dispatch_patterns(macho_binary_t *bin, depflow_graph_t *dep_graph,
                                      exec_order_graph_t *exec_graph, parallel_analysis_t *analysis);
int parallel_analyze_dispatch_calls(macho_binary_t *bin, parallel_analysis_t *analysis);
int parallel_track_dispatch_arguments(macho_binary_t *bin, parallel_analysis_t *analysis);

// Synchronization detection
int parallel_detect_synchronization(macho_binary_t *bin, parallel_analysis_t *analysis);
int parallel_analyze_sync_patterns(macho_binary_t *bin, parallel_analysis_t *analysis);

// Thread creation detection
int parallel_detect_thread_creation(macho_binary_t *bin, parallel_analysis_t *analysis);
int parallel_analyze_thread_functions(macho_binary_t *bin, parallel_analysis_t *analysis);

// Data parallelism detection
int parallel_detect_data_parallelism(macho_binary_t *bin, depflow_graph_t *dep_graph,
                                    exec_order_graph_t *exec_graph, parallel_analysis_t *analysis);
int parallel_analyze_loop_parallelism(macho_binary_t *bin, parallel_analysis_t *analysis);

// Pipeline parallelism detection
int parallel_detect_pipeline_parallelism(macho_binary_t *bin, depflow_graph_t *dep_graph,
                                         exec_order_graph_t *exec_graph, parallel_analysis_t *analysis);
int parallel_analyze_stage_patterns(macho_binary_t *bin, parallel_analysis_t *analysis);

// Utility functions
int parallel_is_parallel_function(macho_binary_t *bin, int func_idx);
int parallel_is_dispatch_function(macho_binary_t *bin, int func_idx);
int parallel_is_worker_function(macho_binary_t *bin, int func_idx);
int parallel_is_sync_function(macho_binary_t *bin, int func_idx);

// Shogi AI specific analysis
int parallel_analyze_shogi_parallelism(macho_binary_t *bin, parallel_analysis_t *analysis);
int parallel_detect_shogi_search_parallelism(macho_binary_t *bin, parallel_analysis_t *analysis);
int parallel_analyze_move_generation_parallelism(macho_binary_t *bin, parallel_analysis_t *analysis);

// Visualization and output
void parallel_print_analysis(parallel_analysis_t *analysis, macho_opts_t *opts, FILE *out);
void parallel_print_groups(parallel_analysis_t *analysis, macho_opts_t *opts, FILE *out);
void parallel_print_beam_search(parallel_analysis_t *analysis, macho_opts_t *opts, FILE *out);
void parallel_print_dispatch_points(parallel_analysis_t *analysis, macho_opts_t *opts, FILE *out);

// DOT export
void parallel_export_dot(parallel_analysis_t *analysis, const char *filename);
void parallel_export_beam_search_dot(parallel_analysis_t *analysis, const char *filename);

// ============================================================================
// Inline helper functions
// ============================================================================

static inline int is_beam_search_function(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "beam_search", "beam_search", "BeamSearch",
        "expand_nodes", "expand_nodes", "ExpandNodes",
        "evaluate_nodes", "evaluate", "Evaluate",
        "select_best", "select", "Select",
        "parallel_search", "parallel", "Parallel",
        "dispatch_async", "dispatch", "Dispatch",
        "concurrent", "Concurrent",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int is_dispatch_function(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "dispatch", "Dispatch",
        "submit", "Submit",
        "invoke", "Invoke",
        "execute", "Execute",
        "run", "Run",
        "apply", "Apply",
        "map", "Map",
        "for_each", "ForEach",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int is_worker_function(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "worker", "Worker",
        "thread", "Thread",
        "task", "Task",
        "process", "Process",
        "handle", "Handle",
        "callback", "Callback",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int is_sync_function(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "wait", "Wait",
        "join", "Join",
        "sync", "Sync",
        "barrier", "Barrier",
        "lock", "Lock",
        "unlock", "Unlock",
        "mutex", "Mutex",
        "semaphore", "Semaphore",
        "atomic", "Atomic",
        "fence", "Fence",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

#endif // PARALLEL_DETECTION_H
