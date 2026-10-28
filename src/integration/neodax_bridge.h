#ifndef NEODAX_BRIDGE_H
#define NEODAX_BRIDGE_H

/*
 * neodax_bridge — 新静的解析フレームワークの公開 API と、
 * nx 自律ディスパッチ層 (nx.h) を繋ぐ統合ファサード。
 *
 * 旧版は存在しない API を呼んでいたためリンク不能だった。
 * 本版は src_new/ 系の実 API に合わせて書き直し、
 * 解析の実行順序・並列度は nx エンジンに委ねる。
 */

#include <stdio.h>
#include <stdint.h>

#include "../core/macho_static.h"
#include "../analysis/depflow.h"
#include "../analysis/variable_classifier.h"
#include "../flow/execution_order.h"
#include "../flow/parallel_detection.h"
#include "../tracking/state_tracking.h"
#include "nx.h"

/* Bend-inspired parallel execution types */
typedef enum {
    BEND_PARALLEL_NONE = 0,
    BEND_PARALLEL_BEAM_SEARCH,
    BEND_PARALLEL_TASK,
    BEND_PARALLEL_DATA,
    BEND_PARALLEL_PIPELINE,
    BEND_PARALLEL_MAP_REDUCE
} bend_parallel_type_t;

/* Shogi AI specific analysis types */
typedef enum {
    SHOGI_ANALYSIS_NONE = 0,
    SHOGI_ANALYSIS_BOARD_STATE,
    SHOGI_ANALYSIS_MOVE_GENERATION,
    SHOGI_ANALYSIS_SEARCH_TREE,
    SHOGI_ANALYSIS_EVALUATION,
    SHOGI_ANALYSIS_BEAM_SEARCH,
    SHOGI_ANALYSIS_FULL
} shogi_analysis_type_t;

/* Static analysis options */
typedef struct {
    int enable_static_analysis;
    int classify_variables;
    int detect_parallel;
    int predict_execution_order;
    int track_state;
    int beam_search_analysis;
    int shogi_mode;
    int parallel_complement;
    int num_threads;

    char dot_output[512];
    char workflow_output[512];
    int show_dependency_graph;
    int show_execution_graph;
    int show_state_graph;
    int show_classification;

    int max_depth;
    int max_iterations;

    int use_bend_concepts;
    int proof_based_analysis;

    shogi_analysis_type_t shogi_analysis;
    int track_board_state;
    int track_move_history;
    int track_search_tree;
    int track_evaluation;
} static_analysis_opts_t;

/* Integrated binary structure — artifacts は engine が所有する */
typedef struct {
    macho_binary_t macho_bin;

    nx_binary_t *bin;        /* macho_bin を borrow する nx ハンドル */
    nx_engine_t *engine;

    depflow_graph_t     *dep_graph;
    var_classifier_t    *classifier;
    exec_order_graph_t  *exec_graph;
    parallel_analysis_t *parallel;
    state_graph_t       *state;

    int has_beam_search;
    int has_parallel_execution;
    int execution_order_determined;
    int variables_classified;

    bend_parallel_type_t parallel_type;
    int has_proof_based_analysis;

    /* 自律ディスパッチの結果 */
    uint32_t    backend_caps;
    const char *backend_name;
    int         plan_waves;
} integrated_binary_t;

/* nx 側ヘルパ (nx_provider_macho.c / nx_provider_dax.c が実装) */
nx_binary_t *nx_macho_wrap(macho_binary_t *bin);
int          nx_dax_integrate(void *dax_bin, FILE *out);

/* Function prototypes */
int  neodax_bridge_init(integrated_binary_t *ibin, const uint8_t *data, size_t size);
void neodax_bridge_free(integrated_binary_t *ibin);

int  neodax_run_static_analysis(integrated_binary_t *ibin,
                                static_analysis_opts_t *opts, FILE *output);
int  neodax_emit_workflow(integrated_binary_t *ibin, const char *filename);
int  neodax_parallel_complement(integrated_binary_t *ibin,
                                static_analysis_opts_t *opts, FILE *output);
int  neodax_shogi_analysis(integrated_binary_t *ibin,
                           static_analysis_opts_t *opts, FILE *output);
int  neodax_bend_analysis(integrated_binary_t *ibin,
                          static_analysis_opts_t *opts, FILE *output);
int  neodax_classify_variables(integrated_binary_t *ibin,
                               static_analysis_opts_t *opts, FILE *output);
int  neodax_predict_execution_order(integrated_binary_t *ibin,
                                    static_analysis_opts_t *opts, FILE *output);
int  neodax_detect_parallel(integrated_binary_t *ibin,
                            static_analysis_opts_t *opts, FILE *output);
int  neodax_track_state(integrated_binary_t *ibin,
                        static_analysis_opts_t *opts, FILE *output);
int  neodax_generate_dot(integrated_binary_t *ibin,
                         static_analysis_opts_t *opts, const char *filename);

int  neodax_integrate_with_binary(void *dax_bin,
                                  static_analysis_opts_t *opts, FILE *output);

#endif /* NEODAX_BRIDGE_H */
