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
    printf("  --track-annuli         環 (call-graph SCC) を解析する\n");
    printf("  --track-prune         線を 必要/邪魔/どうでもいい に分類する\n");
    printf("  --track-files         このモジュールが読むファイルを洗い出す\n");
    printf("  --annuli               neodax 本来の環を報告する\n");
    printf("  --classify            分類器と役割をまとめて出す\n");
    printf("  --find-string S       文字列を参照する箇所と関数を一覧 (静的のみ)\n");
    printf("  --guard S             起動を止める検知を静的に無効化 (--apply で実書換)\n");
    printf("  --find-symbol S      定義シンボルを名前の部分一致で一覧 (静的のみ)\n");
    printf("  --deps               リンクしているライブラリを一覧 (otool -L 相当)\n");
    printf("  --deps-tree          依存の連鎖を再帰的に辿る (@rpath 展開込み)\n");
    printf("  --scan-strings S     生の文字列を走査する (strings 相当)\n");
    printf("  --heal-state         SSO フラグを良い状態に揃える (Mach-O は触らない, --apply)\n");
    printf("  --ngl-reset          NGL の状態を退避する (Group Container / Scripts / ~/Library/NGL)\n");
    printf("  --predict-login      NGL プロファイルと SSO からログインダイアログを静的に予測\n");
    printf("  --advise-ae [--save F.svg]  AE 起動/ログインの IT アドバイザ (非 HTML 表示)\n");
    printf("  --ae-login-purge     AE のログイン状態とアクセスログを退避する (--apply)\n");
    printf("  --term S             実行パスに S を含むプロセスを解放 (--apply で SIGTERM)\n");
    printf("  --classes [S]        C++ vtable/typeinfo からクラスと仮想メソッドを復元\n");
    printf("  --imports [S]        取り込む外部シンボルと GOT スロットを一覧\n");
    printf("  --seek PATTERN [PATH] neodax の検索 (再帰・C++ クラス設計)\n");
    printf("  --seek-ignore-case   大小を無視 / --seek-regex 正規表現 / --seek-word 単語境界\n");
    printf("  --seek-include G     対象を絞る (例 '*.txt,*.plist')\n");
    printf("  --seek-context N     一致行の前後 N 行を出す\n");
    printf("  --seek-limit N       一致行の上限 (0=無制限) / --seek-time MS 走査時間の上限\n");
    printf("  --seek-files         一致したファイル名のみ / --seek-binary バイナリも走査\n");
    printf("  --lc                 Mach-O load commands を全部出す\n");
    printf("  --callers ADDR       指定アドレスを呼ぶ箇所 (BL) を列挙\n");
    printf("  --motion ADDR        指定関数から仮想運動 (--count steps)\n");
    printf("  --class-graph [S]    関数グラフからクラスグラフを導出 (--save で SVG)\n");
    printf("  --display-graph S    実画面/保存モデルのノード木を cpp/svg に多態出力 (--save F)\n");
    printf("  --virtual-display S  neodax の仮想ディスプレイ (保存済み DVNode/PID -> cpp/svg)\n");
    printf("  --display-focus S    仮想ディスプレイを role/title の部分一致で絞る\n");
    printf("  --class-svg ADDR OUT class + 呼び出しを SVG に (部分構造)\n");
    printf("  --prune-svg OUT      重要+状態依存だけを SVG に (irrelevant は捨てる)\n");
    printf("  --patch-ret ADDR     関数先頭を ret に潰す (実バイナリ書換)\n");
    printf("  --patch-bytes ADDR HEX  任意バイトを vaddr に書く\n");
    printf("  --resign PATH        ad-hoc 再署名 + 検証\n");
    printf("  --stasis S           対象が停滞かどうかを測る (--count ms)\n");
    printf("  --probe APP          起動して停滞を測定 (--count ms) を一つで\n");
    printf("  --launch APP         バンドルを直接 detached 起動 (open なし)\n");
    printf("  --activate APP       起動してウィンドウブロックを観測 (--count ms)\n");
    printf("  --ax PID|S           Accessibility でウィンドウの role/title を同定\n");
    printf("  --ax-press PID|S D   Accessibility で desc=D の要素を押す\n");
    printf("  --wire HOST:PORT     汎用バイト列プローブ (--send-text/--send-hex, --count N, --save F)\n");
    printf("  --watch S            S を含む実行パスのプロセスが現れたら終了 (--count grace_ms)\n");
    printf("  --finder-move SRC DST Finder に移動を依頼 (App Management の対象外)\n");
    printf("  --move SRC DST       rename で移動 (コピーなし; App Management 要)\n");
    printf("  --move-auth SRC DST  管理者パスワード一回で root 移动 (Finder なし)\n");
    printf("  --priv-run TOOL ARGS 管理者パスワード一回で TOOL を root 実行\n");
    printf("  --fork <app>         アプリのコピーを作る (--to <dst> --fork-move/--fork-patch/--fork-launch)\n");
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
    g_opts.seek_limit = 1000;
    g_opts.seek_time_ms = 15000;
    
    // --priv-run は以降の全引数をツールとその引数として扱い、ここで処理する。
    // (パスワード許可ダイアログの開始手続きを neodax が持つ)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--priv-run") == 0) {
            extern int nx_priv_exec(const char *, char *const [], FILE *);
            if (i + 1 >= argc) {
                fprintf(stderr, "--priv-run needs a tool\n");
                return 2;
            }
            return nx_priv_exec(argv[i + 1], &argv[i + 2], stdout);
        }
    }

    // fork 系は位置引数 (ソース) の後ろに置かれても効くよう、先に全部拾う。
    // 本体ループは値の読み飛ばしだけを行う。
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fork") == 0) {
            g_opts.track_mode = 15;
        } else if (strcmp(argv[i], "--fork-launch") == 0) {
            g_opts.fork_launch = 1;
        } else if (strcmp(argv[i], "--fork-move") == 0) {
            g_opts.fork_move = 1;
        } else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            snprintf(g_opts.fork_dest, sizeof(g_opts.fork_dest), "%s", argv[i + 1]);
        } else if (strcmp(argv[i], "--fork-patch") == 0 && i + 1 < argc) {
            const char *v = argv[i + 1];
            size_t l = strlen(g_opts.track_filter);
            if (l && l + 2 < sizeof(g_opts.track_filter))
                snprintf(g_opts.track_filter + l, sizeof(g_opts.track_filter) - l, "%c%s", 0x1f, v);
            else
                snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", v);
        }
    }

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
        } else if (strcmp(argv[optind], "--track-annuli") == 0) {
            g_opts.track_mode = 2;                 // 環 (SCC) 解析
        } else if (strcmp(argv[optind], "--track-prune") == 0) {
            g_opts.track_mode = 3;                 // 線の分類と分析断ち切り
        } else if (strcmp(argv[optind], "--track-files") == 0) {
            g_opts.track_mode = 4;                 // このモジュールが読むファイル
        } else if (strcmp(argv[optind], "--annuli") == 0) {
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
        } else if (strcmp(argv[optind], "--scan-strings") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 13;                // 生の文字列走査 (strip されても見える)
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--term") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 16;                // 止まったプロセスを解放 (neodax 内部)
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--wire") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 18;                // 汎用バイト列プローブ
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--send-text") == 0 && optind + 1 < argc) {
            snprintf(g_opts.wire_send, sizeof(g_opts.wire_send), "%s", argv[++optind]);
            g_opts.wire_hex = 0;
        } else if (strcmp(argv[optind], "--send-hex") == 0 && optind + 1 < argc) {
            snprintf(g_opts.wire_send, sizeof(g_opts.wire_send), "%s", argv[++optind]);
            g_opts.wire_hex = 1;
        } else if (strcmp(argv[optind], "--save") == 0 && optind + 1 < argc) {
            snprintf(g_opts.wire_save, sizeof(g_opts.wire_save), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--move-auth") == 0 && optind + 2 < argc) {
            g_opts.track_mode = 22;                // 管理者パスワード一回 → root で移動
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
            snprintf(g_opts.fork_dest, sizeof(g_opts.fork_dest), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--move") == 0 && optind + 2 < argc) {
            g_opts.track_mode = 21;                // rename で移動 (コピーなし)
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
            snprintf(g_opts.fork_dest, sizeof(g_opts.fork_dest), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--finder-move") == 0 && optind + 2 < argc) {
            g_opts.track_mode = 20;                // Finder に move を依頼
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
            snprintf(g_opts.fork_dest, sizeof(g_opts.fork_dest), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--watch") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 19;                // 現れたら猶予後に終了 (サインイン窓対策)
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--patch-bytes") == 0 && optind + 2 < argc) {
            g_opts.track_mode = 32;                // 任意バイトを vaddr に書く
            g_opts.track_entry = strtoull(argv[++optind], NULL, 0);
            snprintf(g_opts.workflow_out, sizeof(g_opts.workflow_out), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--patch-ret") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 24;                // 関数先頭を ret に
            g_opts.track_entry = strtoull(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "--resign") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 25;                // ad-hoc 再署名 + 検証
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--ax-press") == 0 && optind + 2 < argc) {
            g_opts.track_mode = 31;                // AX でボタンを押す
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
            snprintf(g_opts.workflow_out, sizeof(g_opts.workflow_out), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--ax") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 30;                // Accessibility でウィンドウ同定
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--activate") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 29;                // 起動 + ウィンドウブロック観測
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--launch") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 28;                // 直接 detached 起動
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--probe") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 27;                // 起動 + 停滞測定を一つに
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--stasis") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 26;                // 生きているが進んでいない状態を測る
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--prune-svg") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 37;                // 重要だけの SVG
            snprintf(g_opts.workflow_out, sizeof(g_opts.workflow_out), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--class-svg") == 0 && optind + 2 < argc) {
            g_opts.track_mode = 38;                // class + annulus を SVG に
            g_opts.track_entry = strtoull(argv[++optind], NULL, 0);
            snprintf(g_opts.workflow_out, sizeof(g_opts.workflow_out), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--display-focus") == 0 && optind + 1 < argc) {
            snprintf(g_opts.display_focus, sizeof(g_opts.display_focus), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--virtual-display") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 36;                // neodax の仮想ディスプレイ (app 非依存)
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--display-graph") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 36;                // 実画面のノード木 -> cpp/svg
            snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--class-graph") == 0) {
            g_opts.track_mode = 35;                // 関数グラフ -> クラスグラフ導出
            if (optind + 1 < argc && argv[optind + 1][0] != '-')
                snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
            else
                g_opts.track_filter[0] = '\0';
        } else if (strcmp(argv[optind], "--motion") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 34;                // 仮想運動 (制御流れ)
            g_opts.track_entry = strtoull(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "--seek") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 39;                // neodax の検索 ()
            snprintf(g_opts.seek_pattern, sizeof(g_opts.seek_pattern), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--seek-ignore-case") == 0) {
            g_opts.seek_flags |= 1;
        } else if (strcmp(argv[optind], "--seek-regex") == 0) {
            g_opts.seek_flags |= 2;
        } else if (strcmp(argv[optind], "--seek-binary") == 0) {
            g_opts.seek_flags |= 4;
        } else if (strcmp(argv[optind], "--seek-files") == 0) {
            g_opts.seek_flags |= 8;
        } else if (strcmp(argv[optind], "--seek-word") == 0) {
            g_opts.seek_flags |= 16;
        } else if (strcmp(argv[optind], "--seek-include") == 0 && optind + 1 < argc) {
            snprintf(g_opts.seek_include, sizeof(g_opts.seek_include), "%s", argv[++optind]);
        } else if (strcmp(argv[optind], "--seek-context") == 0 && optind + 1 < argc) {
            g_opts.seek_context = atoi(argv[++optind]);
        } else if (strcmp(argv[optind], "--seek-limit") == 0 && optind + 1 < argc) {
            g_opts.seek_limit = atoi(argv[++optind]);
        } else if (strcmp(argv[optind], "--seek-time") == 0 && optind + 1 < argc) {
            g_opts.seek_time_ms = atoi(argv[++optind]);
        } else if (strcmp(argv[optind], "--lc") == 0) {
            g_opts.track_mode = 33;                // Mach-O load commands
        } else if (strcmp(argv[optind], "--callers") == 0 && optind + 1 < argc) {
            g_opts.track_mode = 38;                // 呼び出し元を列挙
            g_opts.track_entry = strtoull(argv[++optind], NULL, 0);
        } else if (strcmp(argv[optind], "--imports") == 0) {
            g_opts.track_mode = 23;                // import シンボル一覧
            if (optind + 1 < argc && argv[optind + 1][0] != '-')
                snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
            else
                g_opts.track_filter[0] = '\0';
        } else if (strcmp(argv[optind], "--classes") == 0) {
            g_opts.track_mode = 17;                // C++ vtable/typeinfo からクラス復元
            if (optind + 1 < argc && argv[optind + 1][0] != '-')
                snprintf(g_opts.track_filter, sizeof(g_opts.track_filter), "%s", argv[++optind]);
            else
                g_opts.track_filter[0] = '\0';
        } else if (strcmp(argv[optind], "--ae-login-purge") == 0) {
            g_opts.track_mode = 43;                // AE のログイン状態/ログを退避
        } else if (strcmp(argv[optind], "--purge") == 0) {
            g_opts.track_taken = 1;                // --ae-login-purge のバックアップも完全削除
        } else if (strcmp(argv[optind], "--advise-ae") == 0) {
            g_opts.track_mode = 44;                // AE 起動/ログインの IT アドバイザ
        } else if (strcmp(argv[optind], "--predict-login") == 0) {
            g_opts.track_mode = 42;                // ログインダイアログの静的予測
        } else if (strcmp(argv[optind], "--ngl-reset") == 0) {
            g_opts.track_mode = 40;                // NGL の状態を退避 (--apply)
        } else if (strcmp(argv[optind], "--heal-state") == 0) {
            g_opts.track_mode = 14;                // SSO フラグを良い状態に揃える
        } else if (strcmp(argv[optind], "--fork") == 0 ||
                   strcmp(argv[optind], "--fork-launch") == 0 ||
                   strcmp(argv[optind], "--fork-move") == 0) {
            /* 事前走査で処理済み */
        } else if ((strcmp(argv[optind], "--to") == 0 ||
                    strcmp(argv[optind], "--fork-patch") == 0) && optind + 1 < argc) {
            optind++;   /* 事前走査で処理済み。値を読み飛ばす */
        } else if (strcmp(argv[optind], "--count") == 0 && optind + 1 < argc) {
            g_opts.track_pick = atoi(argv[++optind]);
        } else if (strcmp(argv[optind], "--apply") == 0) {
            g_opts.track_apply = 1;
        } else if (strcmp(argv[optind], "--pick") == 0 && optind + 1 < argc) {
            g_opts.track_pick = atoi(argv[++optind]);
        } else if (strcmp(argv[optind], "--taken") == 0) {
            g_opts.track_taken = 1;
        } else if (strcmp(argv[optind], "--workflow") == 0 && optind + 1 < argc) {
            snprintf(g_opts.workflow_out, sizeof(g_opts.workflow_out), "%s", argv[++optind]);                 // neodax 本来の環 (depflow_build_annuli)
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

    // --heal-state は入力がバイナリではない (状態ディレクトリのみ)。
    if (g_opts.track_mode == 14) {
        extern int nx_state_heal(const char *, int, FILE *);
        return nx_state_heal(NULL, g_opts.track_apply, stdout);
    }
    // --advise-ae は Signal を分類して Verdict/Advice を出し、SVG に描く。
    if (g_opts.track_mode == 44) {
        extern int nx_advise_ae(const char *, FILE *);
        return nx_advise_ae(g_opts.wire_save[0] ? g_opts.wire_save : NULL, stdout);
    }
    // --ae-login-purge は AE のログイン状態とアクセスログを退避する。
    if (g_opts.track_mode == 43) {
        extern int nx_ae_login_purge(int, int, FILE *);
        return nx_ae_login_purge(g_opts.track_apply, g_opts.track_taken, stdout);
    }
    // --predict-login は NGL の状態だけを見て、ログインダイアログを静的に予測する。
    if (g_opts.track_mode == 42) {
        extern int nx_predict_login(FILE *);
        return nx_predict_login(stdout);
    }
    // --ngl-reset は NGL の状態をアクティブな場所から退避する。
    if (g_opts.track_mode == 40) {
        extern int nx_ngl_reset(int, FILE *);
        return nx_ngl_reset(g_opts.track_apply, stdout);
    }
    // --term はプロセス名で止まった相手を解放する (入力はバイナリではない)。
    if (g_opts.track_mode == 16) {
        extern int nx_release_proc(const char *, int, FILE *);
        return nx_release_proc(g_opts.track_filter, g_opts.track_apply, stdout);
    }
    // --ax-press は Accessibility で要素を押す。
    if (g_opts.track_mode == 31) {
        extern int nx_ax_press(const char *, const char *, FILE *);
        return nx_ax_press(g_opts.track_filter, g_opts.workflow_out, stdout);
    }
    // --ax は Accessibility でウィンドウを同定する。
    if (g_opts.track_mode == 30) {
        extern int nx_ax_windows(const char *, FILE *);
        return nx_ax_windows(g_opts.track_filter, stdout);
    }
    // --activate は起動し、ウィンドウブロックを pid で観測する (キャプチャなし)。
    if (g_opts.track_mode == 29) {
        extern int nx_activate(const char *, int, FILE *);
        return nx_activate(g_opts.track_filter,
                           g_opts.track_pick > 0 ? g_opts.track_pick : 10000, stdout);
    }
    // --launch はバンドルの実行ファイルを直接起動する。
    if (g_opts.track_mode == 28) {
        extern int nx_launch(const char *, FILE *);
        return nx_launch(g_opts.track_filter, stdout);
    }
    // --probe は起動して停滞を測るまでを一つで行う。
    if (g_opts.track_mode == 27) {
        extern int nx_probe(const char *, int, FILE *);
        return nx_probe(g_opts.track_filter,
                        g_opts.track_pick > 0 ? g_opts.track_pick : 8000, stdout);
    }
    if (g_opts.track_mode == 36) {
        extern int nx_display_graph(const char *, const char *, FILE *);
        extern void nx_display_set_focus(const char *);
        if (g_opts.display_focus[0]) nx_display_set_focus(g_opts.display_focus);
        return nx_display_graph(g_opts.track_filter,
                                g_opts.wire_save[0] ? g_opts.wire_save : NULL, stdout);
    }
    // --stasis は対象の停滞を測る (入力はバイナリではない)。
    if (g_opts.track_mode == 26) {
        extern int nx_stasis_watch(const char *, int, FILE *);
        return nx_stasis_watch(g_opts.track_filter,
                               g_opts.track_pick > 0 ? g_opts.track_pick : 15000, stdout);
    }
    // --resign は ad-hoc 再署名 (入力はバイナリではない)。
    if (g_opts.track_mode == 25) {
        extern int nx_resign(const char *, FILE *);
        return nx_resign(g_opts.track_filter, stdout);
    }
    // --move-auth は Authorization Services で root として移動する。
    if (g_opts.track_mode == 22) {
        extern int nx_move_auth(const char *, const char *, FILE *);
        return nx_move_auth(g_opts.track_filter, g_opts.fork_dest, stdout);
    }
    // --move は rename だけで移動する (コピーなし、Finder なし)。
    if (g_opts.track_mode == 21) {
        extern int nx_move(const char *, const char *, FILE *);
        return nx_move(g_opts.track_filter, g_opts.fork_dest, stdout);
    }
    // --finder-move は Finder に移動を依頼する (App Management の対象外)。
    if (g_opts.track_mode == 20) {
        extern int nx_finder_move(const char *, const char *, FILE *);
        return nx_finder_move(g_opts.track_filter, g_opts.fork_dest, stdout);
    }
    // --watch は対象が現れたら猶予後に終える (窓はプロセス内なので終了で消す)。
    if (g_opts.track_mode == 19) {
        extern int nx_watch_proc(const char *, int, int, FILE *);
        return nx_watch_proc(g_opts.track_filter, g_opts.track_pick, 120000, stdout);
    }
    // --wire は汎用のバイト列プローブ (ws/http はこの上でのプリセット)。
    if (g_opts.track_mode == 18) {
        extern int nx_wire(const char *, const char *, const char *, int, int, const char *, FILE *);
        return nx_wire(g_opts.track_filter,
                       g_opts.wire_hex ? NULL : g_opts.wire_send,
                       g_opts.wire_hex ? g_opts.wire_send : NULL,
                       g_opts.track_pick > 0 ? g_opts.track_pick : 512,
                       2000, g_opts.wire_save, stdout);
    }

    // --seek は入力がバイナリでなくてもよい (ディレクトリ・テキストを走査する)。
    if (g_opts.track_mode == 39) {
        extern int nx_seek(const char *, const char *, const char *, int, int, int, int, FILE *);
        const char *root = (optind < argc) ? argv[optind] : ".";
        return nx_seek(g_opts.seek_pattern, root,
                       g_opts.seek_include[0] ? g_opts.seek_include : NULL,
                       g_opts.seek_flags, g_opts.seek_context,
                       g_opts.seek_limit, g_opts.seek_time_ms, stdout);
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
        g_opts.track_mode == 10 || g_opts.track_mode == 11 || g_opts.track_mode == 12 ||
        g_opts.track_mode == 13 || g_opts.track_mode == 15 ||
        g_opts.track_mode == 17 || g_opts.track_mode == 23 ||
        g_opts.track_mode == 24 || g_opts.track_mode == 32 ||
        g_opts.track_mode == 33 || g_opts.track_mode == 34 || g_opts.track_mode == 38 ||
        g_opts.track_mode == 35 || g_opts.track_mode == 36 ||
        g_opts.track_mode == 37 || g_opts.track_mode == 38) {
        extern int nx_find_string(const char *, const char *, FILE *);
        extern int nx_guard_run(const char *, const char *, int, int, int, FILE *);
        extern int nx_list_symbols(const char *, const char *, FILE *);
        extern int nx_disasm(const char *, uint64_t, int, FILE *);
        extern int nx_list_deps(const char *, int, FILE *);
        extern int nx_scan_strings(const char *, const char *, FILE *);
        extern int nx_fork_run(const char *, const char *, const char *, int, int, FILE *);
        extern int nx_cpp_classes(const char *, const char *, FILE *);
        extern int nx_list_imports(const char *, const char *, FILE *);
        extern int nx_patch_ret(const char *, uint64_t, FILE *);
        extern int nx_patch_bytes(const char *, uint64_t, const char *, FILE *);
        extern int nx_callers(const char *, uint64_t, FILE *);
        if (g_opts.track_mode == 38)
            return nx_callers(filename, g_opts.track_entry, stdout);
        extern int nx_load_commands(const char *, FILE *);
        extern int nx_motion(const char *, uint64_t, int, FILE *);
        extern int nx_class_graph(const char *, const char *, const char *, FILE *);
        extern int nx_class_svg(const char *, uint64_t, const char *, int, FILE *);
        extern int nx_prune_svg(const char *, const char *, FILE *);
        if (g_opts.track_mode == 37)
            return nx_prune_svg(filename, g_opts.workflow_out, stdout);
        if (g_opts.track_mode == 38)
            return nx_class_svg(filename, g_opts.track_entry, g_opts.workflow_out,
                                g_opts.track_pick > 0 ? g_opts.track_pick : 600, stdout);
        if (g_opts.track_mode == 35)
            return nx_class_graph(filename, g_opts.track_filter,
                                  g_opts.wire_save[0] ? g_opts.wire_save : NULL, stdout);
        if (g_opts.track_mode == 34)
            return nx_motion(filename, g_opts.track_entry,
                             g_opts.track_pick > 0 ? g_opts.track_pick : 400, stdout);
        if (g_opts.track_mode == 33)
            return nx_load_commands(filename, stdout);
        if (g_opts.track_mode == 32)
            return nx_patch_bytes(filename, g_opts.track_entry, g_opts.workflow_out, stdout);
        if (g_opts.track_mode == 24)
            return nx_patch_ret(filename, g_opts.track_entry, stdout);
        if (g_opts.track_mode == 23)
            return nx_list_imports(filename, g_opts.track_filter, stdout);
        if (g_opts.track_mode == 17)
            return nx_cpp_classes(filename, g_opts.track_filter, stdout);
        if (g_opts.track_mode == 15)
            return nx_fork_run(filename, g_opts.fork_dest, g_opts.track_filter,
                               g_opts.fork_launch, g_opts.fork_move, stdout);
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
        if (g_opts.track_mode == 13)
            return nx_scan_strings(filename, g_opts.track_filter, stdout);
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
    // 環は neodax 本来の depflow_build_annuli の結果をそのまま読む。
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
            extern int nx_annuli_report(macho_binary_t *, depflow_graph_t *, FILE *);
            extern int nx_classify_report(macho_binary_t *, depflow_graph_t *, const char *, FILE *);
            macho_analyze(&bin, &g_opts);
            extern int nx_narrow_functions(macho_binary_t *);
            nx_narrow_functions(&bin);

            depflow_graph_t dg;
            depflow_graph_init(&dg);
            depflow_build_dependency_graph(&bin, &dg);
            if (g_opts.track_mode == 5)
                nx_annuli_report(&bin, &dg, stdout);
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
