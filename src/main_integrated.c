#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/macho_static.h"
#include "analysis/depflow.h"
#include "analysis/variable_classifier.h"
#include "flow/execution_order.h"
#include "flow/parallel_detection.h"
#include "tracking/state_tracking.h"
#include "integration/neodax_bridge.h"

/* Global options */
typedef struct {
    int color;
    int verbose;
    
    /* Static analysis options */
    int static_analysis;
    int classify_vars;
    int predict_execution;
    int detect_parallel;
    int track_state;
    int beam_search;
    int shogi;
    int parallel_complement;
    int num_threads;
    
    /* Output options */
    char dot_output[512];
    int show_dep_graph;
    int show_exec_graph;
    int show_state_graph;
    int show_classification;
    
    /* Bend options */
    int bend_analysis;
    int proof_based;
    
    /* Shogi options */
    int shogi_board;
    int shogi_moves;
    int shogi_search;
    int shogi_eval;
    
    /* Input file */
    char filepath[512];
} integrated_opts_t;

/* Color codes */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[1;31m"
#define COL_GREEN   "\033[1;32m"
#define COL_YELLOW  "\033[1;33m"
#define COL_BLUE    "\033[1;34m"
#define COL_MAGENTA "\033[1;35m"
#define COL_CYAN    "\033[1;36m"
#define COL_WHITE   "\033[1;37m"
#define COL_DGRAY   "\033[0;90m"

static void print_usage_integrated(void) {
    const char *R = COL_RESET;
    const char *Y = COL_YELLOW;
    const char *W = COL_WHITE;
    const char *G = COL_GREEN;
    const char *C = COL_CYAN;
    const char *M = COL_MAGENTA;
    const char *B = COL_BLUE;
    const char *DG = COL_DGRAY;
    const char *RD = COL_RED;
    FILE *o = stdout;

    fprintf(o, "\n");
    fprintf(o, "  %s╔════════════════════════════════════════════════════════════════════╗%s\n", B, R);
    fprintf(o, "  %s║%s  ╦  ╔═══════════════════════════════════════════════════════════════════════  %s║%s\n", B, Y, Y, R);
    fprintf(o, "  %s║%s  ║ %sNeoDAX Static Analysis Engine%s  ║  %sv%s%s  %s║%s\n", B, Y, W, R, DG, "1.0.0", R, Y, R);
    fprintf(o, "  %s║%s  ║ %sStatic Mach-O Binary Analysis with Shogi AI Support%s    %s║%s\n", B, Y, DG, R, Y, R);
    fprintf(o, "  %s║%s  ╚═══════════════════════════════════════════════════════════════════════  %s║%s\n", B, Y, Y, R);
    fprintf(o, "  %s╚══════════════════════════════════════════════════════════════════════════════╝%s\n", B, R);
    fprintf(o, "\n");

    fprintf(o, "  %sUSAGE%s\n", W, R);
    fprintf(o, "  %s  neodax_static%s %s[options]%s %s<mach-o-binary>%s\n", C, R, DG, R, G, R);
    fprintf(o, "\n");

    fprintf(o, "  %sSTATIC ANALYSIS OPTIONS%s\n", Y, R);
    fprintf(o, "  %s  --static-analysis%s     Enable full static analysis pipeline\n", G, R);
    fprintf(o, "  %s  --classify-vars%s      Classify variables by aspect (board, move, eval, etc.)\n", G, R);
    fprintf(o, "  %s  --predict-execution%s  Predict execution order of functions\n", G, R);
    fprintf(o, "  %s  --detect-parallel%s    Detect parallel execution patterns\n", G, R);
    fprintf(o, "  %s  --track-state%s        Track state changes throughout execution\n", G, R);
    fprintf(o, "\n");

    fprintf(o, "  %sSHOGI AI ANALYSIS%s\n", M, R);
    fprintf(o, "  %s  --shogi%s             Enable Shogi AI specific analysis\n", G, R);
    fprintf(o, "  %s  --beam-search%s       Analyze beam search patterns\n", G, R);
    fprintf(o, "  %s  --shogi-board%s       Track board state changes\n", G, R);
    fprintf(o, "  %s  --shogi-moves%s       Track move generation and history\n", G, R);
    fprintf(o, "  %s  --shogi-search%s      Analyze search tree construction\n", G, R);
    fprintf(o, "  %s  --shogi-eval%s        Track evaluation values\n", G, R);
    fprintf(o, "\n");

    fprintf(o, "  %sBEND INSPIRED ANALYSIS%s\n", C, R);
    fprintf(o, "  %s  --bend%s             Enable Bend language inspired analysis\n", G, R);
    fprintf(o, "  %s  --proof-based%s      Use proof-based state tracking\n", G, R);
    fprintf(o, "\n");

    fprintf(o, "  %sPARALLEL PROCESSING%s\n", B, R);
    fprintf(o, "  %s  --parallel-complement%s  Use parallel processing to complement unknown areas\n", G, R);
    fprintf(o, "  %s  --threads N%s         Number of threads for parallel analysis (default: 4)\n", G, R);
    fprintf(o, "\n");

    fprintf(o, "  %sOUTPUT OPTIONS%s\n", Y, R);
    fprintf(o, "  %s  -o, --output FILE%s    Output file for DOT graphs\n", G, R);
    fprintf(o, "  %s  --show-dep-graph%s    Show dependency graph in output\n", G, R);
    fprintf(o, "  %s  --show-exec-graph%s   Show execution graph in output\n", G, R);
    fprintf(o, "  %s  --show-state-graph%s   Show state graph in output\n", G, R);
    fprintf(o, "  %s  --show-classification%s Show variable classification in output\n", G, R);
    fprintf(o, "  %s  -n, --no-color%s       Disable color output\n", G, R);
    fprintf(o, "  %s  -v, --verbose%s        Verbose output\n", G, R);
    fprintf(o, "\n");

    fprintf(o, "  %sEXAMPLES%s\n", W, R);
    fprintf(o, "  %s  neodax_static --static-analysis --classify-vars shogi_engine\n", G, R);
    fprintf(o, "  %s  neodax_static --shogi --beam-search --parallel-complement shogi_ai\n", G, R);
    fprintf(o, "  %s  neodax_static --bend --proof-based --detect-parallel binary\n", G, R);
    fprintf(o, "  %s  neodax_static --static-analysis -o analysis.dot binary\n", G, R);
    fprintf(o, "\n");
}

static void print_version_integrated(void) {
    printf("NeoDAX Static Analysis Engine v1.0.0\n");
    printf("Static Mach-O Binary Analysis with Shogi AI Support\n");
}

int main(int argc, char **argv) {
    integrated_opts_t opts;
    integrated_binary_t ibin;
    struct timespec ts_start, ts_end;
    
    memset(&opts, 0, sizeof(opts));
    memset(&ibin, 0, sizeof(ibin));
    
    opts.color = 1;
    opts.num_threads = 4;
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage_integrated();
            return 0;
        }
        else if (strcmp(argv[i], "--version") == 0) {
            print_version_integrated();
            return 0;
        }
        else if (strcmp(argv[i], "--static-analysis") == 0) {
            opts.static_analysis = 1;
        }
        else if (strcmp(argv[i], "--classify-vars") == 0) {
            opts.classify_vars = 1;
        }
        else if (strcmp(argv[i], "--predict-execution") == 0) {
            opts.predict_execution = 1;
        }
        else if (strcmp(argv[i], "--detect-parallel") == 0) {
            opts.detect_parallel = 1;
        }
        else if (strcmp(argv[i], "--track-state") == 0) {
            opts.track_state = 1;
        }
        else if (strcmp(argv[i], "--beam-search") == 0) {
            opts.beam_search = 1;
        }
        else if (strcmp(argv[i], "--shogi") == 0) {
            opts.shogi = 1;
        }
        else if (strcmp(argv[i], "--shogi-board") == 0) {
            opts.shogi_board = 1;
        }
        else if (strcmp(argv[i], "--shogi-moves") == 0) {
            opts.shogi_moves = 1;
        }
        else if (strcmp(argv[i], "--shogi-search") == 0) {
            opts.shogi_search = 1;
        }
        else if (strcmp(argv[i], "--shogi-eval") == 0) {
            opts.shogi_eval = 1;
        }
        else if (strcmp(argv[i], "--bend") == 0) {
            opts.bend_analysis = 1;
        }
        else if (strcmp(argv[i], "--proof-based") == 0) {
            opts.proof_based = 1;
        }
        else if (strcmp(argv[i], "--parallel-complement") == 0) {
            opts.parallel_complement = 1;
        }
        else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                strncpy(opts.dot_output, argv[++i], sizeof(opts.dot_output) - 1);
            }
        }
        else if (strcmp(argv[i], "--show-dep-graph") == 0) {
            opts.show_dep_graph = 1;
        }
        else if (strcmp(argv[i], "--show-exec-graph") == 0) {
            opts.show_exec_graph = 1;
        }
        else if (strcmp(argv[i], "--show-state-graph") == 0) {
            opts.show_state_graph = 1;
        }
        else if (strcmp(argv[i], "--show-classification") == 0) {
            opts.show_classification = 1;
        }
        else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-color") == 0) {
            opts.color = 0;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opts.verbose = 1;
        }
        else if (strcmp(argv[i], "--threads") == 0) {
            if (i + 1 < argc) {
                opts.num_threads = atoi(argv[++i]);
                if (opts.num_threads <= 0) opts.num_threads = 4;
                if (opts.num_threads > 8) opts.num_threads = 8;
            }
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage_integrated();
            return 1;
        }
        else {
            strncpy(opts.filepath, argv[i], sizeof(opts.filepath) - 1);
        }
    }
    
    /* Check if file is specified */
    if (!opts.filepath[0]) {
        fprintf(stderr, "No input file specified\n");
        print_usage_integrated();
        return 1;
    }
    
    /* Load binary file */
    FILE *fp = fopen(opts.filepath, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open file: %s\n", opts.filepath);
        return 1;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        fclose(fp);
        fprintf(stderr, "Failed to allocate memory for file\n");
        return 1;
    }
    
    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        fclose(fp);
        free(file_data);
        fprintf(stderr, "Failed to read file\n");
        return 1;
    }
    fclose(fp);
    
    /* Initialize bridge */
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    
    if (neodax_bridge_init(&ibin, file_data, (size_t)file_size) != 0) {
        free(file_data);
        fprintf(stderr, "Failed to initialize bridge\n");
        return 1;
    }
    
    /* Print header */
    if (opts.color) printf("%s", COL_CYAN);
    printf("\n  Analyzing: %s\n", opts.filepath);
    if (opts.color) printf("%s", COL_RESET);
    printf("  Format: Mach-O (%s)\
", "64-bit");
    printf("  Entry: 0x%llx\n", (unsigned long long)ibin.macho_bin.entry);
    printf("  Functions: %d\n", ibin.macho_bin.nfunctions);
    printf("  Symbols: %d\n\n", ibin.macho_bin.nsymbols);
    
    /* Convert options to static_analysis_opts_t */
    static_analysis_opts_t static_opts;
    memset(&static_opts, 0, sizeof(static_opts));
    
    static_opts.enable_static_analysis = opts.static_analysis || 
                                          opts.classify_vars || 
                                          opts.predict_execution || 
                                          opts.detect_parallel || 
                                          opts.track_state;
    static_opts.classify_variables = opts.classify_vars || opts.shogi;
    static_opts.detect_parallel = opts.detect_parallel || opts.beam_search || opts.shogi;
    static_opts.predict_execution_order = opts.predict_execution || opts.shogi;
    static_opts.track_state = opts.track_state || opts.shogi;
    static_opts.beam_search_analysis = opts.beam_search || opts.shogi;
    static_opts.shogi_mode = opts.shogi || 
                           opts.shogi_board || 
                           opts.shogi_moves || 
                           opts.shogi_search || 
                           opts.shogi_eval;
    static_opts.parallel_complement = opts.parallel_complement;
    static_opts.num_threads = opts.num_threads;
    static_opts.use_bend_concepts = opts.bend_analysis;
    static_opts.proof_based_analysis = opts.proof_based;
    
    strncpy(static_opts.dot_output, opts.dot_output, sizeof(static_opts.dot_output) - 1);
    static_opts.show_dependency_graph = opts.show_dep_graph;
    static_opts.show_execution_graph = opts.show_exec_graph;
    static_opts.show_state_graph = opts.show_state_graph;
    static_opts.show_classification = opts.show_classification;
    
    /* Enable all for Shogi mode */
    if (opts.shogi) {
        static_opts.enable_static_analysis = 1;
        static_opts.classify_variables = 1;
        static_opts.detect_parallel = 1;
        static_opts.predict_execution_order = 1;
        static_opts.track_state = 1;
    }
    
    /* Run static analysis pipeline */
    if (static_opts.enable_static_analysis) {
        if (neodax_run_static_analysis(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "Static analysis failed\n");
            return 1;
        }
    }
    
    /* Run parallel complementation */
    if (static_opts.parallel_complement) {
        if (neodax_parallel_complement(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "Parallel complementation failed\n");
            return 1;
        }
    }
    
    /* Run Shogi AI specific analysis */
    if (static_opts.shogi_mode) {
        if (neodax_shogi_analysis(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "Shogi analysis failed\n");
            return 1;
        }
    }
    
    /* Run Bend-inspired analysis */
    if (static_opts.use_bend_concepts || static_opts.proof_based_analysis) {
        if (neodax_bend_analysis(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "Bend analysis failed\n");
            return 1;
        }
    }
    
    /* Output variable classification */
    if (static_opts.classify_variables || static_opts.show_classification) {
        if (neodax_classify_variables(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "Variable classification output failed\n");
            return 1;
        }
    }
    
    /* Output execution order prediction */
    if (static_opts.predict_execution_order) {
        if (neodax_predict_execution_order(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "Execution order prediction output failed\n");
            return 1;
        }
    }
    
    /* Output parallel detection */
    if (static_opts.detect_parallel) {
        if (neodax_detect_parallel(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "Parallel detection output failed\n");
            return 1;
        }
    }
    
    /* Output state tracking */
    if (static_opts.track_state) {
        if (neodax_track_state(&ibin, &static_opts, stdout) != 0) {
            free(file_data);
            neodax_bridge_free(&ibin);
            fprintf(stderr, "State tracking output failed\n");
            return 1;
        }
    }
    
    /* Generate DOT output */
    if (static_opts.dot_output[0]) {
        if (neodax_generate_dot(&ibin, &static_opts, static_opts.dot_output) != 0) {
            fprintf(stderr, "Failed to generate DOT output\n");
        } else {
            printf("DOT output written to: %s\n", static_opts.dot_output);
        }
    }
    
    /* Cleanup */
    free(file_data);
    neodax_bridge_free(&ibin);
    
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    {
        double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                         (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
        if (opts.color) printf("%s", COL_DGRAY);
        printf("\n  Analysis completed in %.3f seconds.\n", elapsed);
        if (opts.color) printf("%s", COL_RESET);
    }
    
    return 0;
}
