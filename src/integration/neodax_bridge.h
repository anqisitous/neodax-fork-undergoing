#ifndef NEODAX_BRIDGE_H
#define NEODAX_BRIDGE_H

#include "static_analysis/macho_static.h"
#include "static_analysis/depflow.h"
#include "static_analysis/variable_classifier.h"
#include "static_analysis/execution_order.h"
#include "static_analysis/parallel_detection.h"
#include "static_analysis/state_tracking.h"

#include "neodax_svg.h"



/* Static analysis options */
typedef struct {
    int enable_static_analysis;
    int classify_variables;
    int detect_parallel;
    int predict_execution_order;
    int track_state;
    int beam_search_analysis;
    int num_threads;
    
    /* Output options */
    char dot_output[512];
    int show_dependency_graph;
    int show_execution_graph;
    int show_state_graph;
    int show_classification;
    
    /* Analysis depth */
    int max_depth;
    int max_iterations;
    
    /* Generic analysis options */
    int proof_based_analysis;
} static_analysis_opts_t;

/* Integrated binary structure */
typedef struct {
    macho_binary_t macho_bin;
    depflow_graph_t dep_graph;
    var_classifier_t classifier;
    exec_order_graph_t exec_graph;
    parallel_analysis_t parallel_detector;
    state_tracker_t state_tracker;
    
    /* Analysis results */
    int has_beam_search;
    int has_parallel_execution;
    int execution_order_determined;
    int variables_classified;
    
    /* Generic state fields */
    int has_proof_based_analysis;
} integrated_binary_t;

/* Function prototypes */

/* Initialization and cleanup */
int neodax_bridge_init(integrated_binary_t *ibin, const uint8_t *data, size_t size);
void neodax_bridge_free(integrated_binary_t *ibin);

/* Static analysis pipeline */
int neodax_run_static_analysis(integrated_binary_t *ibin, 
                                static_analysis_opts_t *opts,
                                FILE *output);

/* Variable classification wrapper */
int neodax_classify_variables(integrated_binary_t *ibin,
                                static_analysis_opts_t *opts,
                                FILE *output);

/* Execution order prediction wrapper */
int neodax_predict_execution_order(integrated_binary_t *ibin,
                                    static_analysis_opts_t *opts,
                                    FILE *output);

/* Parallel detection wrapper */
int neodax_detect_parallel(integrated_binary_t *ibin,
                           static_analysis_opts_t *opts,
                           FILE *output);

/* State tracking wrapper */
int neodax_track_state(integrated_binary_t *ibin,
                       static_analysis_opts_t *opts,
                       FILE *output);

/* DOT output generation */
int neodax_generate_dot(integrated_binary_t *ibin,
                       static_analysis_opts_t *opts,
                       const char *filename);

/* Integration with existing NeoDAX structures */
int neodax_integrate_with_binary(void *dax_bin, 
                                  static_analysis_opts_t *opts,
                                  FILE *output);

#endif /* NEODAX_BRIDGE_H */
