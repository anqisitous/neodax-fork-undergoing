#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/macho_static.h"
#include "analysis/depflow.h"
#include "flow/execution_order.h"
#include "flow/parallel_detection.h"
#include "tracking/state_tracking.h"

// ============================================================================
// NeoDAX Static Mach-O Analysis Framework - Main Entry Point
// 
// 静的解析フレームワークのメインエントリポイント
// Mach-Oファイルの静的解析を統合的に実行
// ============================================================================

#define VERSION "1.0.0"

// Global options
static macho_opts_t g_opts;

// ============================================================================
// Usage and Help
// ============================================================================

static void print_usage(const char *progname) {
    printf("\n");
    printf("NeoDAX Static Mach-O Analysis Framework v%s\n", VERSION);
    printf("Static analysis framework for Mach-O binaries with focus on execution flow\n");
    printf("and dependency tracking for AI applications like Shogi engines.\n\n");
    
    printf("Usage: %s [options] <binary>\n", progname);
    printf("\n");
    
    printf("Core Analysis:\n");
    printf("  -a, --all             Run all analysis passes\n");
    printf("  -m, --macho           Parse Mach-O file structure\n");
    printf("  -f, --functions       Detect and analyze functions\n");
    printf("  -s, --symbols         Parse and analyze symbols\n");
    printf("  -x, --xrefs           Analyze cross-references\n");
    printf("\n");
    
    printf("Dependency Flow Analysis:\n");
    printf("  -d, --depflow         Enable dependency flow analysis\n");
    printf("  --depflow-func <name> Target function for dependency analysis\n");
    printf("  --depflow-var <name>  Target variable for tracking\n");
    printf("  --depflow-depth <n>   Maximum depth for dependency tracking\n");
    printf("  --depflow-dot <file>  Export dependency graph to DOT file\n");
    printf("\n");
    
    printf("Execution Order Analysis:\n");
    printf("  -e, --execution-order Enable execution order prediction\n");
    printf("  -p, --parallel        Detect parallel execution patterns\n");
    printf("  --beam-search         Analyze beam search patterns\n");
    printf("\n");
    
    printf("State Tracking:\n");
    printf("  -t, --state-tracking  Enable state change tracking\n");
    printf("  -r, --regression      Enable regression analysis\n");
    printf("\n");
    
    printf("Output:\n");
    printf("  -o, --output <dir>    Output directory for analysis results\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  -h, --help            Show this help message\n");
    printf("  --version             Show version information\n");
    printf("\n");
    
    printf("Shogi AI Specific:\n");
    printf("  --shogi               Enable Shogi AI specific analysis\n");
    printf("  --track               復元コードを静的に逆trackする\n");
    printf("  --track-rings         環 (call-graph SCC) を解析する\n");
    printf("  --track-prune         線を 必要/邪魔/どうでもいい に分類する\n");
    printf("  --track-files         このモジュールが読むファイルを洗い出す\n");
    printf("  --rings               neodax 本来の環を報告する\n");
    printf("  --classify            分類器と役割をまとめて出す\n");
    printf("  --find-string S       文字列を参照する箇所と関数を一覧 (静的のみ)\n");
    printf("  --guard S             起動を止める検知を静的に無効化 (--apply で実書換)\n");
    printf("  --find-symbol S      定義シンボルを名前の部分一致で一覧 (静的のみ)\n");
    printf("  --deps               リンクしているライブラリを一覧 (otool -L 相当)\n");
    printf("  --deps-tree          依存の連鎖を再帰的に辿る (@rpath 展開込み)\n");
    printf("  --disasm ADDR [--count N]  指定アドレス周辺を逆アセンブル (静的のみ)\n");
    printf("  --apply                guard の変更を実バイナリに適用\n");
    printf("  --pick N               guard で選ぶ候補の番号\n");
    printf("  --taken                guard で分岐を取る側 (b) に固定\n");
    printf("  --workflow <file>     C++ クラス設計を書き出す\n");
    printf("  --track-filter S      逆track の探索語 (部分一致)\n");
    printf("  --track-entry ADDR    分析の起点アドレス\n");
    printf("  --launch-cond         起動条件を並べて、起動するかどうかを判定する\n");
    printf("  --launch-cond-drop    条件を 1 つずつ落として、どこで起動しなくなるかを見る\n");
    printf("  --launch-fix          壊れた状態を退避から戻して、起動できるようにする\n");
    printf("  --learn-show          学んだ解析の仕方を表示する\n");
    printf("  --learn-clear         学んだことを消す\n");
    printf("  --launch-make         起動可能な状態ファイルを、無ければ作成する\n");
    printf("  --track-board         Track board state changes\n");
    printf("  --track-moves        Track move generation and history\n");
    printf("  --track-search       Track search tree state\n");
    printf("  --track-eval         Track evaluation state\n");
    printf("\n");
    
    printf("Examples:\n");
    printf("  %s -a binary               # Run all analysis\n", progname);
    printf("  %s -d -e binary            # Dependency + execution order\n", progname);
    printf("  %s -p --beam-search binary  # Parallel + beam search analysis\n", progname);
    printf("  %s --shogi binary           # Shogi AI specific analysis\n", progname);
    printf("\n");
}

static void print_version() {
    printf("NeoDAX Static Mach-O Analysis Framework v%s\n", VERSION);
    printf("Build: %s %s\n", __DATE__, __TIME__);
}

// ============================================================================
// File Loading
// ============================================================================

static int load_file(const char *filename, uint8_t **data, size_t *size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Failed to open file '%s'\n", filename);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    *data = (uint8_t *)malloc(*size);
    if (!*data) {
        fprintf(stderr, "Error: Failed to allocate memory for file '%s'\n", filename);
        fclose(f);
        return -1;
    }
    
    if (fread(*data, 1, *size, f) != *size) {
        fprintf(stderr, "Error: Failed to read file '%s'\n", filename);
        free(*data);
        fclose(f);
        return -1;
    }
    
    fclose(f);
    return 0;
}

// ============================================================================
// Analysis Pipeline
// ============================================================================

static int run_analysis(macho_binary_t *bin, const char *filename) {
    if (!bin || !filename) return -1;
    
    printf("Analyzing: %s\n", filename);
    printf("Architecture: %s\n", 
          bin->arch == MACHO_ARCH_X86_64 ? "x86_64" :
          bin->arch == MACHO_ARCH_X86_32 ? "i386" :
          bin->arch == MACHO_ARCH_ARM64 ? "ARM64" : "Unknown");
    printf("File Type: 0x%08X\n", bin->filetype);
    printf("Entry Point: 0x%llx\n", (unsigned long long)bin->entry);
    printf("\n");
    
    // Initialize analysis modules
    depflow_graph_t dep_graph;
    exec_order_graph_t exec_graph;
    parallel_analysis_t parallel_analysis;
    state_graph_t state_graph;
    
    depflow_graph_init(&dep_graph);
    exec_order_graph_init(&exec_graph);
    parallel_analysis_init(&parallel_analysis);
    state_graph_init(&state_graph);
    
    // Run Mach-O analysis
    printf("=== Mach-O Structure Analysis ===\n");
    if (macho_analyze(bin, &g_opts) != 0) {
        fprintf(stderr, "Error: Mach-O analysis failed\n");
        return -1;
    }
    
    printf("Sections: %d\n", bin->nsections);
    printf("Symbols: %d\n", bin->nsymbols);
    printf("Functions: %d\n", bin->nfunctions);
    printf("\n");
    
    // Run dependency flow analysis
    if (g_opts.depflow) {
        printf("=== Dependency Flow Analysis ===\n");
        if (depflow_analyze(bin, &g_opts) != 0) {
            fprintf(stderr, "Warning: Dependency flow analysis failed\n");
        }
    }
    
    // Run execution order analysis
    if (g_opts.execution_order || g_opts.parallel_detection) {
        printf("=== Execution Order Analysis ===\n");
        
        exec_order_opts_t exec_opts;
        memset(&exec_opts, 0, sizeof(exec_order_opts_t));
        exec_opts.enable_parallel_detection = g_opts.parallel_detection;
        exec_opts.enable_beam_search_analysis = g_opts.parallel_detection;
        exec_opts.enable_conditional_analysis = 1;
        exec_opts.enable_loop_analysis = 1;
        exec_opts.enable_recursion_analysis = 1;
        exec_opts.enable_state_tracking = g_opts.depflow;
        
        if (exec_order_analyze(bin, &dep_graph, &exec_opts) != 0) {
            fprintf(stderr, "Warning: Execution order analysis failed\n");
        }
    }
    
    // Run parallel detection
    if (g_opts.parallel_detection) {
        printf("=== Parallel Execution Detection ===\n");
        
        parallel_opts_t parallel_opts;
        memset(&parallel_opts, 0, sizeof(parallel_opts_t));
        parallel_opts.detect_beam_search = 1;
        parallel_opts.detect_dispatch_patterns = 1;
        parallel_opts.detect_synchronization = 1;
        parallel_opts.detect_thread_creation = 1;
        parallel_opts.detect_data_parallelism = 1;
        parallel_opts.detect_pipeline_parallelism = 1;
        
        if (parallel_analyze(bin, &dep_graph, &exec_graph, &parallel_opts) != 0) {
            fprintf(stderr, "Warning: Parallel detection failed\n");
        }
    }
    
    // Run state tracking
    if (g_opts.depflow || g_opts.state_tracking) {
        printf("=== State Tracking Analysis ===\n");
        
        state_opts_t state_opts;
        memset(&state_opts, 0, sizeof(state_opts_t));
        state_opts.enable_checkpointing = 1;
        state_opts.enable_variable_tracking = 1;
        state_opts.enable_change_tracking = 1;
        state_opts.enable_dependency_analysis = 1;
        state_opts.enable_regression_analysis = g_opts.state_tracking;
        state_opts.enable_path_analysis = 1;
        state_opts.enable_shogi_analysis = g_opts.depflow;
        
        if (state_analyze(bin, &dep_graph, &exec_graph, &state_opts) != 0) {
            fprintf(stderr, "Warning: State tracking failed\n");
        }
    }
    
    // Print analysis results
    printf("\n=== Analysis Results ===\n");
    
    // Print function information
    printf("Functions:\n");
    for (int i = 0; i < bin->nfunctions && i < 20; i++) {
        macho_function_t *func = &bin->functions[i];
        printf("  %s: 0x%llx-0x%llx\n", func->name,
               (unsigned long long)func->start, (unsigned long long)func->end);
    }
    if (bin->nfunctions > 20) {
        printf("  ... and %d more functions\n", bin->nfunctions - 20);
    }
    printf("\n");
    
    // Print section information
    printf("Sections:\n");
    for (int i = 0; i < bin->nsections && i < 10; i++) {
        macho_section_t *sec = &bin->sections[i];
        printf("  %s: 0x%llx-0x%llx (size: %llu, vaddr: 0x%llx)\n", sec->name,
               (unsigned long long)sec->offset, (unsigned long long)(sec->offset + sec->size),
               (unsigned long long)sec->size, (unsigned long long)sec->vaddr);
    }
    if (bin->nsections > 10) {
        printf("  ... and %d more sections\n", bin->nsections - 10);
    }
    printf("\n");
    
    // Print symbols
    printf("Symbols:\n");
    for (int i = 0; i < bin->nsymbols && i < 20; i++) {
        macho_symbol_t *sym = &bin->symbols[i];
        printf("  %s: 0x%llx (type: %d)\n", sym->demangled[0] ? sym->demangled : sym->name,
               (unsigned long long)sym->address, sym->type);
    }
    if (bin->nsymbols > 20) {
        printf("  ... and %d more symbols\n", bin->nsymbols - 20);
    }
    printf("\n");
    
    // Cleanup
    depflow_graph_free(&dep_graph);
    exec_order_graph_free(&exec_graph);
    parallel_analysis_free(&parallel_analysis);
    state_graph_free(&state_graph);
    
    return 0;
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Parse command line options
    memset(&g_opts, 0, sizeof(macho_opts_t));
    g_opts.color = 1;
    g_opts.verbose = 0;
    g_opts.depflow = 0;
    g_opts.execution_order = 0;
    g_opts.parallel_detection = 0;
    g_opts.state_tracking = 0;
    g_opts.depflow_depth = 10;
    g_opts.output_dir[0] = '\0';
    g_opts.depflow_target_func[0] = '\0';
    g_opts.depflow_target_var[0] = '\0';
    g_opts.depflow_dot[0] = '\0';
    
    int optind = 1;
    while (optind < argc) {
        if (strcmp(argv[optind], "-h") == 0 || strcmp(argv[optind], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[optind], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[optind], "-a") == 0 || strcmp(argv[optind], "--all") == 0) {
            g_opts.depflow = 1;
            g_opts.execution_order = 1;
            g_opts.parallel_detection = 1;
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "-v") == 0 || strcmp(argv[optind], "--verbose") == 0) {
            g_opts.verbose = 1;
        } else if (strcmp(argv[optind], "-d") == 0 || strcmp(argv[optind], "--depflow") == 0) {
            g_opts.depflow = 1;
        } else if (strcmp(argv[optind], "-e") == 0 || strcmp(argv[optind], "--execution-order") == 0) {
            g_opts.execution_order = 1;
        } else if (strcmp(argv[optind], "-p") == 0 || strcmp(argv[optind], "--parallel") == 0) {
            g_opts.parallel_detection = 1;
        } else if (strcmp(argv[optind], "-t") == 0 || strcmp(argv[optind], "--state-tracking") == 0) {
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "-r") == 0 || strcmp(argv[optind], "--regression") == 0) {
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "-m") == 0 || strcmp(argv[optind], "--macho") == 0) {
            // Mach-O parsing is always done
        } else if (strcmp(argv[optind], "-f") == 0 || strcmp(argv[optind], "--functions") == 0) {
            // Function detection is always done
        } else if (strcmp(argv[optind], "-s") == 0 || strcmp(argv[optind], "--symbols") == 0) {
            // Symbol parsing is always done
        } else if (strcmp(argv[optind], "-x") == 0 || strcmp(argv[optind], "--xrefs") == 0) {
            // Cross-reference analysis is always done
        } else if (strcmp(argv[optind], "--beam-search") == 0) {
            g_opts.parallel_detection = 1;
        } else if (strcmp(argv[optind], "--shogi") == 0) {
            g_opts.depflow = 1;
            g_opts.parallel_detection = 1;
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "--track") == 0) {
            g_opts.track_mode = 1;                 // 復元コードの逆track
        } else if (strcmp(argv[optind], "--track-rings") == 0) {
            g_opts.track_mode = 2;                 // 環 (SCC) 解析
        } else if (strcmp(argv[optind], "--track-prune") == 0) {
            g_opts.track_mode = 3;                 // 線の分類と分析断ち切り
        } else if (strcmp(argv[optind], "--track-files") == 0) {
            g_opts.track_mode = 4;                 // このモジュールが読むファイル
        } else if (strcmp(argv[optind], "--rings") == 0) {
            g_opts.track_mode = 5;
        } else if (strcmp(argv[optind], "--classify") == 0) {
            g_opts.track_mode = 6;
        } else if (strcmp(argv[optind], "--find-string") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 7;                 // 文字列参照の一覧 (静的のみ)
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--guard") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 8;                 // 起動を止める検知の静的無効化
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--find-symbol") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 9;                 // 定義シンボルの一覧
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--disasm") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 10;                // 指定アドレス周辺の逆アセンブル
            g_opts.track_entry = strtoull(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "--deps") == 0) {
            g_opts.track_mode = 11;                // リンク依存の一覧 (otool -L 相当)
        } else if (strcmp(argv[optind], "--deps-tree") == 0) {
            g_opts.track_mode = 12;                // 依存の連鎖を再帰的に辿る
        } else if (strcmp(argv[optind], "--count") == 0 && optind + 1 < argc) {
            g_opts.track_pick = atoi(argv[++optind]);
        } else if (strcmp(argv[optind], "--apply") == 0) {
            g_opts.track_apply = 1;
        } else if (strcmp(argv[optind], "--pick") == 0 && optind + 1 < argc) {
            g_opts.track_pick = atoi(argv[++optind]);
        } else if (strcmp(argv[optind], "--taken") == 0) {
            g_opts.track_taken = 1;
        } else if (strcmp(argv[optind], "--workflow") == 0 && optind + 1 < argc) {
            snprintf(g_opts.workflow_out, sizeof(g_opts.workflow_out), "%s", argv[++optind]);                 // neodax 本来の環 (depflow_build_rings)
        } else if (strcmp(argv[optind], "--track-filter") == 0 && optind + 1 < argc) {
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--track-entry") == 0 && optind + 1 < argc) {
            g_opts.track_entry = strtoull(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "--track-symbols") == 0) {
            g_opts.track_symbols_only = 1;
        } else if (strcmp(argv[optind], "--launch-cond") == 0) {
            g_opts.launch_cond = 1;
        } else if (strcmp(argv[optind], "--launch-cond-drop") == 0) {
            g_opts.launch_cond = 2;
        } else if (strcmp(argv[optind], "--launch-fix") == 0) {
            g_opts.launch_cond = 3;                // 壊れた状態を退避から戻す
        } else if (strcmp(argv[optind], "--learn-show") == 0) {
            g_opts.launch_cond = 4;                // 学んだ解析の仕方を表示する
        } else if (strcmp(argv[optind], "--learn-clear") == 0) {
            g_opts.launch_cond = 5;                // 学んだことを消す
        } else if (strcmp(argv[optind], "--launch-make") == 0) {
            g_opts.launch_cond = 6;                // 起動可能な状態ファイルを作成する
        } else if (strcmp(argv[optind], "--track-board") == 0) {
            g_opts.depflow = 1;
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "--track-moves") == 0) {
            g_opts.depflow = 1;
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "--track-search") == 0) {
            g_opts.depflow = 1;
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "--track-eval") == 0) {
            g_opts.depflow = 1;
            g_opts.state_tracking = 1;
        } else if (strcmp(argv[optind], "--depflow-func") == 0) {
            if (optind + 1 < argc) {
                strncpy(g_opts.depflow_target_func, argv[optind + 1], sizeof(g_opts.depflow_target_func) - 1);
                optind++;
            }
        } else if (strcmp(argv[optind], "--depflow-var") == 0) {
            if (optind + 1 < argc) {
                strncpy(g_opts.depflow_target_var, argv[optind + 1], sizeof(g_opts.depflow_target_var) - 1);
                optind++;
            }
        } else if (strcmp(argv[optind], "--depflow-depth") == 0) {
            if (optind + 1 < argc) {
                g_opts.depflow_depth = atoi(argv[optind + 1]);
                optind++;
            }
        } else if (strcmp(argv[optind], "--depflow-dot") == 0) {
            if (optind + 1 < argc) {
                strncpy(g_opts.depflow_dot, argv[optind + 1], sizeof(g_opts.depflow_dot) - 1);
                optind++;
            }
        } else if (strcmp(argv[optind], "-o") == 0 || strcmp(argv[optind], "--output") == 0) {
            if (optind + 1 < argc) {
                strncpy(g_opts.output_dir, argv[optind + 1], sizeof(g_opts.output_dir) - 1);
                optind++;
            }
        } else if (argv[optind][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[optind]);
            print_usage(argv[0]);
            return 1;
        } else {
            // This should be the binary file
            break;
        }
        optind++;
    }
    
    // --launch-cond は入力がファイルではない (.app や修復のみ) ので、
    // ファイル指定の検査より前に処理する。
    if (g_opts.launch_cond) {
        extern int nx_cond_run(const char *app, int drop_each, FILE *out);
        extern int nx_cond_fix(FILE *out);
        extern int nx_learn_run(int mode, FILE *out);
        extern int nx_make_run(const char *app, FILE *out);
        if (g_opts.launch_cond == 6)
            return nx_make_run(optind < argc ? argv[optind] : "", stdout);
        if (g_opts.launch_cond == 3) return nx_cond_fix(stdout);
        if (g_opts.launch_cond == 4) return nx_learn_run(1, stdout);
        if (g_opts.launch_cond == 5) return nx_learn_run(2, stdout);
        if (optind >= argc) {
            fprintf(stderr, "Error: No application specified\n");
            return 1;
        }
        return nx_cond_run(argv[optind], g_opts.launch_cond == 2, stdout);
    }

    // Check if we have a binary file
    if (optind >= argc) {
        fprintf(stderr, "Error: No binary file specified\n");
        print_usage(argv[0]);
        return 1;
    }
    
    const char *filename = argv[optind];

    // --find-string / --guard は生のパスを静的に読む (動的解析なし)。
    if (g_opts.track_mode == 7 || g_opts.track_mode == 8 || g_opts.track_mode == 9 ||
        g_opts.track_mode == 10 || g_opts.track_mode == 11 || g_opts.track_mode == 12) {
        extern int nx_find_string(const char *, const char *, FILE *);
        extern int nx_guard_run(const char *, const char *, int, int, int, FILE *);
        extern int nx_list_symbols(const char *, const char *, FILE *);
        extern int nx_disasm(const char *, uint64_t, int, FILE *);
        extern int nx_list_deps(const char *, int, FILE *);
        if (g_opts.track_mode == 7)
            return nx_find_string(filename, g_opts.track_filter, stdout);
        if (g_opts.track_mode == 9)
            return nx_list_symbols(filename, g_opts.track_filter, stdout);
        if (g_opts.track_mode == 10)
            return nx_disasm(filename, g_opts.track_entry, g_opts.track_pick, stdout);
        if (g_opts.track_mode == 11)
            return nx_list_deps(filename, 0, stdout);
        if (g_opts.track_mode == 12)
            return nx_list_deps(filename, 1, stdout);
        return nx_guard_run(filename, g_opts.track_filter,
                            g_opts.track_apply, g_opts.track_pick,
                            g_opts.track_taken, stdout);
    }

    // Load the binary file
    uint8_t *data = NULL;
    size_t size = 0;
    if (load_file(filename, &data, &size) != 0) {
        return 1;
    }

    // --track は独立したモード。生の call 命令は使わない。
    // 環は neodax 本来の depflow_build_rings の結果をそのまま読む。
    if (g_opts.track_mode) {
        macho_binary_t bin;
        macho_init(&bin);
        bin.data = data;
        bin.size = size;
        if (macho_parse(&bin, data, size) != 0) {
            fprintf(stderr, "Error: Failed to parse Mach-O file\n");
            macho_free(&bin);
            return 1;
        }

        // 環の報告は depflow の戻り値依存辺から作る (効率的・本来の意味)。
        if (g_opts.track_mode == 5 || g_opts.track_mode == 6) {
            extern int nx_rings_report(macho_binary_t *, depflow_graph_t *, FILE *);
            extern int nx_classify_report(macho_binary_t *, depflow_graph_t *, const char *, FILE *);
            macho_analyze(&bin, &g_opts);
            extern int nx_narrow_functions(macho_binary_t *);
            nx_narrow_functions(&bin);

            depflow_graph_t dg;
            depflow_graph_init(&dg);
            depflow_build_dependency_graph(&bin, &dg);
            if (g_opts.track_mode == 5)
                nx_rings_report(&bin, &dg, stdout);
            else
                nx_classify_report(&bin, &dg, g_opts.workflow_out, stdout);
            depflow_graph_free(&dg);
            macho_free(&bin);
            return 0;
        }

        // 逆track 系は生のバイトから追う (こちらは命令単位が必要)。
        extern int nx_track_run(const char *path, int mode, uint64_t entry,
                                const char *filter, int symbols_only, FILE *out);
        int cmode = (g_opts.track_mode == 1) ? 0 : (g_opts.track_mode - 1);
        int rc = nx_track_run(filename, cmode, g_opts.track_entry,
                              g_opts.track_filter, g_opts.track_symbols_only, stdout);
        macho_free(&bin);
        return rc;
    }
    
    // Initialize binary structure
    macho_binary_t bin;
    macho_init(&bin);

    /* macho_binary_t borrows the raw bytes; macho_free() is the single owner.
       Without this, macho_is_valid() rejects every input and analysis aborts. */
    bin.data = data;
    bin.size = size;

    // Parse the Mach-O file
    if (macho_parse(&bin, data, size) != 0) {
        fprintf(stderr, "Error: Failed to parse Mach-O file\n");
        macho_free(&bin);   /* frees bin.data == data */
        return 1;
    }
    
    // Run analysis
    if (run_analysis(&bin, filename) != 0) {
        fprintf(stderr, "Error: Analysis failed\n");
        macho_free(&bin);
        return 1;
    }
    
    // Cleanup
    macho_free(&bin);
    
    return 0;
}
