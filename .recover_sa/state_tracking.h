#ifndef STATE_TRACKING_H
#define STATE_TRACKING_H

#include <string.h>
#include "macho_static.h"
#include "depflow.h"
#include "execution_order.h"

// ============================================================================
// State Tracking and Regression Analysis Module
// 
// 状態追跡・回帰分析モジュール
// 静的解析に基づいて状態変化を追跡し、回帰分析を実施
// ============================================================================

#define STATE_MAX_CHECKPOINTS 1024
#define STATE_MAX_VARIABLES 4096
#define STATE_MAX_CHANGES 16384
#define STATE_MAX_DEPENDENCIES 32768
#define STATE_MAX_PATHS 1024

// State types
#define STATE_TYPE_UNKNOWN 0
#define STATE_TYPE_GLOBAL 1
#define STATE_TYPE_LOCAL 2
#define STATE_TYPE_ARGUMENT 3
#define STATE_TYPE_RETURN 4
#define STATE_TYPE_HEAP 5
#define STATE_TYPE_STACK 6
#define STATE_TYPE_REGISTER 7

// State change types
#define STATE_CHANGE_READ 0
#define STATE_CHANGE_WRITE 1
#define STATE_CHANGE_MODIFY 2
#define STATE_CHANGE_CREATE 3
#define STATE_CHANGE_DELETE 4
#define STATE_CHANGE_MOVE 5
#define STATE_CHANGE_COPY 6

// State value types for shogi AI
#define STATE_VALUE_TYPE_UNKNOWN 0
#define STATE_VALUE_TYPE_BOARD 1
#define STATE_VALUE_TYPE_MOVE 2
#define STATE_VALUE_TYPE_SCORE 3
#define STATE_VALUE_TYPE_DEPTH 4
#define STATE_VALUE_TYPE_NODE 5
#define STATE_VALUE_TYPE_SEARCH_TREE 6
#define STATE_VALUE_TYPE_PLY 7
#define STATE_VALUE_TYPE_HASH 8
#define STATE_VALUE_TYPE_EVAL 9

// State checkpoint
typedef struct state_checkpoint_t {
    uint64_t id;
    uint64_t address;
    uint64_t timestamp;
    char name[128];
    int is_entry;
    int is_exit;
    int is_branch;
    int is_loop_entry;
    int is_loop_exit;
    int is_function_call;
    uint64_t function_addr;
    uint64_t basic_block_addr;
    uint64_t value_hash;
    
} state_checkpoint_t;

// State variable
typedef struct state_variable_t {
    uint64_t id;
    uint64_t address;
    char name[128];
    int type;  // GLOBAL, LOCAL, ARGUMENT, etc.
    int value_type;  // For shogi AI: BOARD, MOVE, SCORE, etc.
    uint64_t size;
    uint64_t definition_site;
    uint64_t last_modification;
    int is_modified;
    int is_read;
    int is_written;
    int read_count;
    int write_count;
    uint64_t lifetime_start;
    uint64_t lifetime_end;
    
} state_variable_t;

// State change record
typedef struct state_change_t {
    uint64_t id;
    uint64_t checkpoint_id;
    uint64_t variable_id;
    uint64_t address;
    int change_type;  // READ, WRITE, MODIFY, etc.
    uint64_t timestamp;
    uint64_t old_value_hash;
    uint64_t new_value_hash;
    uint64_t instruction_addr;
    int is_conditional;
    uint64_t condition_addr;
    
} state_change_t;

// State dependency
typedef struct state_dependency_t {
    uint64_t id;
    uint64_t from_checkpoint;
    uint64_t to_checkpoint;
    uint64_t variable_id;
    int dependency_type;
    uint64_t weight;
    int is_strong;
    
} state_dependency_t;

// State path (sequence of state changes)
typedef struct state_path_t {
    uint64_t path_id;
    uint64_t checkpoints[STATE_MAX_CHECKPOINTS];
    int checkpoint_count;
    uint64_t changes[STATE_MAX_CHANGES];
    int change_count;
    uint64_t start_timestamp;
    uint64_t end_timestamp;
    uint64_t total_weight;
    int is_feasible;
    
} state_path_t;

// Regression analysis result
typedef struct regression_result_t {
    uint64_t id;
    uint64_t start_checkpoint;
    uint64_t end_checkpoint;
    uint64_t variable_id;
    int regression_type;
    uint64_t start_value_hash;
    uint64_t end_value_hash;
    uint64_t delta;
    int is_significant;
    char description[256];
    
} regression_result_t;

// State tracking context
typedef struct state_context_t {
    uint64_t id;
    char name[128];
    uint64_t start_addr;
    uint64_t end_addr;
    uint64_t start_timestamp;
    uint64_t end_timestamp;
    int depth;
    uint64_t parent_context;
    
} state_context_t;

// State graph (for visualization)
typedef struct state_graph_t {
    state_checkpoint_t *checkpoints;
    int checkpoint_count;
    int checkpoint_capacity;
    
    state_variable_t *variables;
    int variable_count;
    int variable_capacity;
    
    state_change_t *changes;
    int change_count;
    int change_capacity;
    
    state_dependency_t *dependencies;
    int dependency_count;
    int dependency_capacity;
    
    state_path_t *paths;
    int path_count;
    int path_capacity;
    
    regression_result_t *regressions;
    int regression_count;
    int regression_capacity;
    
    state_context_t *contexts;
    int context_count;
    int context_capacity;
    
    // Statistics
    uint64_t total_checkpoints;
    uint64_t total_changes;
    uint64_t total_dependencies;
    uint64_t max_path_length;
    uint64_t total_regressions;
    
} state_graph_t;

// State tracking options
typedef struct state_opts_t {
    int enable_checkpointing;
    int enable_variable_tracking;
    int enable_change_tracking;
    int enable_dependency_analysis;
    int enable_regression_analysis;
    int enable_path_analysis;
    int max_checkpoints;
    int max_changes;
    int max_depth;
    
    // Shogi AI specific
    int enable_shogi_analysis;
    int track_board_state;
    int track_move_history;
    int track_search_tree;
    int track_evaluation;
    
} state_opts_t;

// ============================================================================
// Function Prototypes
// ============================================================================

// Initialization and cleanup
void state_graph_init(state_graph_t *graph);
void state_graph_free(state_graph_t *graph);

// Main analysis function
int state_analyze(macho_binary_t *bin, depflow_graph_t *dep_graph, 
                 exec_order_graph_t *exec_graph, state_opts_t *opts);

// Checkpoint management
int state_create_checkpoints(macho_binary_t *bin, state_graph_t *graph);
int state_add_checkpoint(state_graph_t *graph, uint64_t address, const char *name, 
                         int is_entry, int is_exit, int is_branch, uint64_t timestamp);
int state_identify_key_checkpoints(macho_binary_t *bin, state_graph_t *graph);

// Variable tracking
int state_track_variables(macho_binary_t *bin, state_graph_t *graph);
int state_add_variable(state_graph_t *graph, uint64_t address, const char *name, 
                       int type, int value_type, uint64_t size);
int state_identify_shogi_variables(macho_binary_t *bin, state_graph_t *graph);

// Change tracking
int state_track_changes(macho_binary_t *bin, state_graph_t *graph);
int state_add_change(state_graph_t *graph, uint64_t checkpoint_id, uint64_t variable_id,
                     uint64_t address, int change_type, uint64_t timestamp,
                     uint64_t old_hash, uint64_t new_hash, uint64_t instr_addr);

// Dependency analysis
int state_analyze_dependencies(state_graph_t *graph);
int state_add_dependency(state_graph_t *graph, uint64_t from_checkpoint, uint64_t to_checkpoint,
                         uint64_t variable_id, int type, uint64_t weight);

// Path analysis
int state_analyze_paths(state_graph_t *graph);
int state_find_paths(state_graph_t *graph, uint64_t start_checkpoint, uint64_t end_checkpoint);

// Regression analysis
int state_analyze_regression(state_graph_t *graph);
int state_detect_regression(state_graph_t *graph, uint64_t start_checkpoint, uint64_t end_checkpoint);
int state_identify_significant_regressions(state_graph_t *graph);

// Shogi AI specific analysis
int state_analyze_shogi_state(macho_binary_t *bin, state_graph_t *graph);
int state_track_board_state(macho_binary_t *bin, state_graph_t *graph);
int state_track_move_history(macho_binary_t *bin, state_graph_t *graph);
int state_track_search_tree_state(macho_binary_t *bin, state_graph_t *graph);
int state_track_evaluation_state(macho_binary_t *bin, state_graph_t *graph);

// Context management
int state_create_contexts(macho_binary_t *bin, state_graph_t *graph);
int state_add_context(state_graph_t *graph, uint64_t start_addr, uint64_t end_addr,
                      const char *name, uint64_t start_ts, uint64_t end_ts);

// Visualization and output
void state_print_analysis(state_graph_t *graph, macho_opts_t *opts, FILE *out);
void state_print_checkpoints(state_graph_t *graph, macho_opts_t *opts, FILE *out);
void state_print_variables(state_graph_t *graph, macho_opts_t *opts, FILE *out);
void state_print_changes(state_graph_t *graph, macho_opts_t *opts, FILE *out);
void state_print_dependencies(state_graph_t *graph, macho_opts_t *opts, FILE *out);
void state_print_regressions(state_graph_t *graph, macho_opts_t *opts, FILE *out);

// DOT export
void state_export_dot(state_graph_t *graph, const char *filename);
void state_export_checkpoint_dot(state_graph_t *graph, const char *filename);
void state_export_regression_dot(state_graph_t *graph, const char *filename);

// Utility functions
uint64_t state_checkpoint_id_by_address(state_graph_t *graph, uint64_t address);
uint64_t state_variable_id_by_name(state_graph_t *graph, const char *name);
int state_is_variable_modified(state_graph_t *graph, uint64_t variable_id);
int state_get_variable_changes(state_graph_t *graph, uint64_t variable_id,
                               state_change_t *changes, int max_changes);

// ============================================================================
// Inline helper functions
// ============================================================================

static inline int is_shogi_state_variable(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "board", "Board", "ban", "Ban",
        "state", "State",
        "position", "Position",
        "game", "Game",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int is_move_variable(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "move", "Move", "te", "Te",
        "action", "Action",
        "step", "Step",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int is_score_variable(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "score", "Score", "eval", "Eval",
        "value", "Value",
        "grade", "Grade",
        "point", "Point",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int is_depth_variable(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "depth", "Depth", "ply", "Ply",
        "level", "Level",
        "step", "Step",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int is_node_variable(const char *name) {
    if (!name) return 0;
    const char *patterns[] = {
        "node", "Node",
        "search", "Search",
        "tree", "Tree",
        "position", "Position",
        NULL
    };
    
    for (int i = 0; patterns[i]; i++) {
        if (strstr(name, patterns[i])) {
            return 1;
        }
    }
    return 0;
}

#endif // STATE_TRACKING_H
