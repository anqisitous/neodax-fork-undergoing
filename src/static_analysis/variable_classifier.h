#ifndef VARIABLE_CLASSIFIER_H
#define VARIABLE_CLASSIFIER_H

#include "macho_static.h"
#include "depflow.h"

// ============================================================================
// Variable Classification System
// 
// 変数分類システム
// 将棋AIの変数をその役割と性質に基づいて分類
// ============================================================================

#define VAR_CLASS_MAX_NAME_LENGTH 128
#define VAR_CLASS_MAX_CATEGORIES 32
#define VAR_CLASS_MAX_ASPECTS 16

// Variable categories (変数カテゴリ)
typedef enum {
    VAR_CAT_UNKNOWN = 0,
    VAR_CAT_BOARD,           // 盤面状態
    VAR_CAT_MOVE,            // 着手
    VAR_CAT_MOVE_TREE,       // 着手の木（探索木）
    VAR_CAT_EVALUATION,      // 評価値
    VAR_CAT_DEPTH,           // 探索深さ
    VAR_CAT_NODE,            // 探索ノード
    VAR_CAT_SEARCH_TREE,     // 探索木全体
    VAR_CAT_CONSTANT,        // 定数
    VAR_CAT_DATABASE,        // データベース（逐次アクセス）
    VAR_CAT_STATE,           // 一般的な状態
    VAR_CAT_MESSAGE,         // メッセージ
    VAR_CAT_PARALLEL,        // 並列実行関連
    VAR_CAT_TEMPORARY,       // 一時変数
    VAR_CAT_GLOBAL_CONFIG,   // グローバル設定
    VAR_CAT_HASH,            // ハッシュ値
    VAR_CAT_BITBOARD,        // ビットボード
    VAR_CAT_PIECE,           // 駒
    VAR_CAT_SCORE,           // スコア
    VAR_CAT_TIME,            // 時間
    VAR_CAT_MEMORY,          // メモリ管理
    VAR_CAT_IO,              // 入出力
    VAR_CAT_UTILITY,         // ユーティリティ
    VAR_CAT_MAX
} var_category_t;

// Variable aspects (変数の側面/性質)
typedef enum {
    VAR_ASPECT_UNKNOWN = 0,
    VAR_ASPECT_STATIC,       // 静的（不変）
    VAR_ASPECT_DYNAMIC,      // 動的（変化する）
    VAR_ASPECT_PERSISTENT,   // 持続的（セッションを通じて維持）
    VAR_ASPECT_TEMPORARY,    // 一時的（短期間で破棄）
    VAR_ASPECT_SHARED,       // 共有（複数の関数/スレッドでアクセス）
    VAR_ASPECT_PRIVATE,      // 専用（単一の関数/スレッドでのみアクセス）
    VAR_ASPECT_READONLY,     // 読み取り専用
    VAR_ASPECT_WRITABLE,     // 書き込み可能
    VAR_ASPECT_IMMUTABLE,    // 不変（初期化後は変更されない）
    VAR_ASPECT_MUTABLE,      // 可変（頻繁に変更される）
    VAR_ASPECT_SEQUENTIAL,   // 逐次アクセス
    VAR_ASPECT_RANDOM,       // ランダムアクセス
    VAR_ASPECT_INDEXED,      // インデックスによるアクセス
    VAR_ASPECT_HIERARCHICAL, // 階層的構造
    VAR_ASPECT_LINEAR,       // 線形構造
    VAR_ASPECT_TREE,         // 木構造
    VAR_ASPECT_GRAPH,        // グラフ構造
    VAR_ASPECT_HASHED,       // ハッシュ構造
    VAR_ASPECT_COMPRESSED,   // 圧縮されたデータ
    VAR_ASPECT_ENCRYPTED,    // 暗号化されたデータ
    VAR_ASPECT_MAX
} var_aspect_t;

// Variable access patterns (アクセスパターン)
typedef enum {
    VAR_ACCESS_UNKNOWN = 0,
    VAR_ACCESS_SINGLE,       // 単一アクセス
    VAR_ACCESS_MULTIPLE,     // 複数アクセス
    VAR_ACCESS_SEQUENTIAL,   // 逐次アクセス
    VAR_ACCESS_RANDOM,       // ランダムアクセス
    VAR_ACCESS_ITERATIVE,    // 反復アクセス
    VAR_ACCESS_RECURSIVE,    // 再帰的アクセス
    VAR_ACCESS_PARALLEL,     // 並列アクセス
    VAR_ACCESS_CONDITIONAL,  // 条件付きアクセス
    VAR_ACCESS_FREQUENT,     // 頻繁なアクセス
    VAR_ACCESS_RARE,         // 稀なアクセス
    VAR_ACCESS_MAX
} var_access_pattern_t;

// Variable data types (データ型)
typedef enum {
    VAR_TYPE_UNKNOWN = 0,
    VAR_TYPE_SCALAR,        // スカラー（単一値）
    VAR_TYPE_ARRAY,         // 配列
    VAR_TYPE_STRUCT,        // 構造体
    VAR_TYPE_UNION,         // 共用体
    VAR_TYPE_POINTER,       // ポインタ
    VAR_TYPE_FUNCTION_PTR,  // 関数ポインタ
    VAR_TYPE_BITFIELD,      // ビットフィールド
    VAR_TYPE_STRING,        // 文字列
    VAR_TYPE_LIST,          // リスト
    VAR_TYPE_TREE,          // 木
    VAR_TYPE_GRAPH,         // グラフ
    VAR_TYPE_HASH_TABLE,    // ハッシュテーブル
    VAR_TYPE_STACK,         // スタック
    VAR_TYPE_QUEUE,         // キュー
    VAR_TYPE_HEAP,          // ヒープ
    VAR_TYPE_MATRIX,        // 行列
    VAR_TYPE_VECTOR,        // ベクター
    VAR_TYPE_MAX
} var_data_type_t;

// Variable classification result
typedef struct var_classification_t {
    uint64_t var_id;                     // Variable ID
    uint64_t address;                   // Memory address
    char name[VAR_CLASS_MAX_NAME_LENGTH]; // Variable name
    char demangled[VAR_CLASS_MAX_NAME_LENGTH]; // Demangled name
    
    // Classification results
    var_category_t primary_category;    // 主カテゴリ
    var_category_t secondary_categories[VAR_CLASS_MAX_CATEGORIES]; // 副カテゴリ
    int num_secondary_categories;
    
    var_aspect_t aspects[VAR_CLASS_MAX_ASPECTS]; // 側面/性質
    int num_aspects;
    
    var_access_pattern_t access_pattern; // アクセスパターン
    var_data_type_t data_type;           // データ型
    
    // Confidence scores (0-100)
    int category_confidence;
    int aspect_confidence;
    int access_confidence;
    int type_confidence;
    
    // Additional information
    uint64_t size;                      // Size in bytes
    int is_global;                     // Is global variable
    int is_static;                     // Is static variable
    int is_constant;                    // Is constant
    int is_volatile;                    // Is volatile
    
    // Shogi AI specific
    int is_shogi_specific;              // Is Shogi AI specific variable
    int shogi_role;                     // Role in Shogi AI (0=unknown, 1=board, 2=move, etc.)
    
    // Analysis metadata
    uint64_t definition_site;           // Where defined
    uint64_t first_use_site;            // First use site
    uint64_t last_use_site;             // Last use site
    int use_count;                      // Number of uses
    int write_count;                    // Number of writes
    
} var_classification_t;

// Classification context
typedef struct var_classifier_t {
    macho_binary_t *bin;
    depflow_graph_t *dep_graph;
    
    var_classification_t *classifications;
    int num_classifications;
    int capacity;
    
    // Statistics
    int categories[VAR_CAT_MAX];
    int aspects[VAR_ASPECT_MAX];
    int access_patterns[VAR_ACCESS_MAX];
    int data_types[VAR_TYPE_MAX];
    
} var_classifier_t;

// Classification rules
typedef struct var_classification_rule_t {
    const char *pattern;               // Name pattern to match
    var_category_t category;          // Category to assign
    int priority;                     // Rule priority (higher = more specific)
    int min_length;                   // Minimum name length
    int max_length;                   // Maximum name length
    
} var_classification_rule_t;

// ============================================================================
// Function Prototypes
// ============================================================================

// Initialization and cleanup
void var_classifier_init(var_classifier_t *classifier);
void var_classifier_free(var_classifier_t *classifier);

// Main classification function
int var_classify_all(var_classifier_t *classifier, macho_binary_t *bin, depflow_graph_t *dep_graph);

// Classification by different criteria
int var_classify_by_name(var_classifier_t *classifier, const char *name, var_classification_t *result);
int var_classify_by_address(var_classifier_t *classifier, uint64_t address, var_classification_t *result);
int var_classify_by_usage(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);

// Category-specific classification
int var_classify_board(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);
int var_classify_move(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);
int var_classify_move_tree(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);
int var_classify_evaluation(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);
int var_classify_depth(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);
int var_classify_node(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);
int var_classify_search_tree(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result);

// Aspect analysis
int var_analyze_aspects(var_classifier_t *classifier, var_classification_t *classification);
int var_analyze_access_patterns(var_classifier_t *classifier, var_classification_t *classification);
int var_analyze_data_types(var_classifier_t *classifier, var_classification_t *classification);

// Shogi AI specific classification
int var_classify_shogi_variables(var_classifier_t *classifier);
int var_identify_shogi_roles(var_classifier_t *classifier);

// Parallel execution classification
int var_classify_parallel_variables(var_classifier_t *classifier);
int var_identify_shared_variables(var_classifier_t *classifier);

// Utility functions
const char *var_category_to_string(var_category_t category);
const char *var_aspect_to_string(var_aspect_t aspect);
const char *var_access_pattern_to_string(var_access_pattern_t pattern);
const char *var_data_type_to_string(var_data_type_t type);

// Output functions
void var_classifier_print(var_classifier_t *classifier, macho_opts_t *opts, FILE *out);
void var_classifier_print_summary(var_classifier_t *classifier, macho_opts_t *opts, FILE *out);
void var_classifier_print_detailed(var_classifier_t *classifier, macho_opts_t *opts, FILE *out);

// DOT export
void var_classifier_export_dot(var_classifier_t *classifier, const char *filename);
void var_classifier_export_category_dot(var_classifier_t *classifier, const char *filename);

// ============================================================================
// Classification Rule Definitions
// ============================================================================

// Board variable patterns
static const var_classification_rule_t board_rules[] = {
    {"board", VAR_CAT_BOARD, 10, 3, 32},
    {"ban", VAR_CAT_BOARD, 10, 3, 32},
    {"position", VAR_CAT_BOARD, 9, 3, 32},
    {"state", VAR_CAT_BOARD, 8, 3, 32},
    {"game", VAR_CAT_BOARD, 7, 3, 32},
    {"pos", VAR_CAT_BOARD, 6, 3, 32},
    {"current_board", VAR_CAT_BOARD, 10, 3, 32},
    {"current_position", VAR_CAT_BOARD, 10, 3, 32},
    {"initial_board", VAR_CAT_BOARD, 10, 3, 32},
    {"initial_position", VAR_CAT_BOARD, 10, 3, 32},
    {"bitboard", VAR_CAT_BITBOARD, 10, 3, 32},
    {"bit_board", VAR_CAT_BITBOARD, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Move variable patterns
static const var_classification_rule_t move_rules[] = {
    {"move", VAR_CAT_MOVE, 10, 3, 32},
    {"te", VAR_CAT_MOVE, 10, 2, 32},
    {"action", VAR_CAT_MOVE, 9, 3, 32},
    {"step", VAR_CAT_MOVE, 8, 3, 32},
    {"play", VAR_CAT_MOVE, 7, 3, 32},
    {"best_move", VAR_CAT_MOVE, 10, 3, 32},
    {"current_move", VAR_CAT_MOVE, 10, 3, 32},
    {"next_move", VAR_CAT_MOVE, 10, 3, 32},
    {"prev_move", VAR_CAT_MOVE, 10, 3, 32},
    {"last_move", VAR_CAT_MOVE, 10, 3, 32},
    {"legal_move", VAR_CAT_MOVE, 10, 3, 32},
    {"legal_moves", VAR_CAT_MOVE, 10, 3, 32},
    {"move_list", VAR_CAT_MOVE, 10, 3, 32},
    {"move_history", VAR_CAT_MOVE, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Move tree variable patterns
static const var_classification_rule_t move_tree_rules[] = {
    {"tree", VAR_CAT_MOVE_TREE, 10, 3, 32},
    {"search_tree", VAR_CAT_MOVE_TREE, 10, 3, 32},
    {"game_tree", VAR_CAT_MOVE_TREE, 10, 3, 32},
    {"node_tree", VAR_CAT_MOVE_TREE, 10, 3, 32},
    {"position_tree", VAR_CAT_MOVE_TREE, 10, 3, 32},
    {"root", VAR_CAT_MOVE_TREE, 9, 3, 32},
    {"search_root", VAR_CAT_MOVE_TREE, 10, 3, 32},
    {"game_root", VAR_CAT_MOVE_TREE, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Evaluation variable patterns
static const var_classification_rule_t evaluation_rules[] = {
    {"score", VAR_CAT_EVALUATION, 10, 3, 32},
    {"eval", VAR_CAT_EVALUATION, 10, 3, 32},
    {"evaluate", VAR_CAT_EVALUATION, 10, 3, 32},
    {"value", VAR_CAT_EVALUATION, 9, 3, 32},
    {"grade", VAR_CAT_EVALUATION, 8, 3, 32},
    {"point", VAR_CAT_EVALUATION, 7, 3, 32},
    {"best_score", VAR_CAT_EVALUATION, 10, 3, 32},
    {"current_score", VAR_CAT_EVALUATION, 10, 3, 32},
    {"total_score", VAR_CAT_EVALUATION, 10, 3, 32},
    {"max_score", VAR_CAT_EVALUATION, 10, 3, 32},
    {"min_score", VAR_CAT_EVALUATION, 10, 3, 32},
    {"alpha", VAR_CAT_EVALUATION, 7, 3, 32},
    {"beta", VAR_CAT_EVALUATION, 7, 3, 32},
    {"heuristic", VAR_CAT_EVALUATION, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Depth variable patterns
static const var_classification_rule_t depth_rules[] = {
    {"depth", VAR_CAT_DEPTH, 10, 3, 32},
    {"ply", VAR_CAT_DEPTH, 10, 3, 32},
    {"level", VAR_CAT_DEPTH, 9, 3, 32},
    {"step", VAR_CAT_DEPTH, 8, 3, 32},
    {"current_depth", VAR_CAT_DEPTH, 10, 3, 32},
    {"max_depth", VAR_CAT_DEPTH, 10, 3, 32},
    {"min_depth", VAR_CAT_DEPTH, 10, 3, 32},
    {"search_depth", VAR_CAT_DEPTH, 10, 3, 32},
    {"initial_depth", VAR_CAT_DEPTH, 10, 3, 32},
    {"remaining_depth", VAR_CAT_DEPTH, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Node variable patterns
static const var_classification_rule_t node_rules[] = {
    {"node", VAR_CAT_NODE, 10, 3, 32},
    {"current_node", VAR_CAT_NODE, 10, 3, 32},
    {"next_node", VAR_CAT_NODE, 10, 3, 32},
    {"prev_node", VAR_CAT_NODE, 10, 3, 32},
    {"child_node", VAR_CAT_NODE, 10, 3, 32},
    {"parent_node", VAR_CAT_NODE, 10, 3, 32},
    {"root_node", VAR_CAT_NODE, 10, 3, 32},
    {"leaf_node", VAR_CAT_NODE, 10, 3, 32},
    {"search_node", VAR_CAT_NODE, 10, 3, 32},
    {"game_node", VAR_CAT_NODE, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Search tree variable patterns
static const var_classification_rule_t search_tree_rules[] = {
    {"search_tree", VAR_CAT_SEARCH_TREE, 10, 3, 32},
    {"game_tree", VAR_CAT_SEARCH_TREE, 10, 3, 32},
    {"tree", VAR_CAT_SEARCH_TREE, 9, 3, 32},
    {"transposition_table", VAR_CAT_SEARCH_TREE, 10, 3, 32},
    {"tt", VAR_CAT_SEARCH_TREE, 10, 2, 32},
    {"hash_table", VAR_CAT_SEARCH_TREE, 10, 3, 32},
    {"visited", VAR_CAT_SEARCH_TREE, 10, 3, 32},
    {"explored", VAR_CAT_SEARCH_TREE, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Constant variable patterns
static const var_classification_rule_t constant_rules[] = {
    {"MAX_", VAR_CAT_CONSTANT, 10, 4, 32},
    {"MIN_", VAR_CAT_CONSTANT, 10, 4, 32},
    {"DEFAULT_", VAR_CAT_CONSTANT, 10, 8, 32},
    {"INITIAL_", VAR_CAT_CONSTANT, 10, 8, 32},
    {"FINAL_", VAR_CAT_CONSTANT, 10, 6, 32},
    {"SIZE_", VAR_CAT_CONSTANT, 10, 5, 32},
    {"COUNT_", VAR_CAT_CONSTANT, 10, 6, 32},
    {"LIMIT_", VAR_CAT_CONSTANT, 10, 6, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Database variable patterns
static const var_classification_rule_t database_rules[] = {
    {"db", VAR_CAT_DATABASE, 10, 2, 32},
    {"database", VAR_CAT_DATABASE, 10, 3, 32},
    {"table", VAR_CAT_DATABASE, 9, 3, 32},
    {"list", VAR_CAT_DATABASE, 8, 3, 32},
    {"array", VAR_CAT_DATABASE, 8, 3, 32},
    {"collection", VAR_CAT_DATABASE, 10, 3, 32},
    {"storage", VAR_CAT_DATABASE, 9, 3, 32},
    {"cache", VAR_CAT_DATABASE, 8, 3, 32},
    {"buffer", VAR_CAT_DATABASE, 8, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Parallel variable patterns
static const var_classification_rule_t parallel_rules[] = {
    {"parallel", VAR_CAT_PARALLEL, 10, 3, 32},
    {"thread", VAR_CAT_PARALLEL, 9, 3, 32},
    {"mutex", VAR_CAT_PARALLEL, 8, 3, 32},
    {"lock", VAR_CAT_PARALLEL, 7, 3, 32},
    {"semaphore", VAR_CAT_PARALLEL, 10, 3, 32},
    {"barrier", VAR_CAT_PARALLEL, 9, 3, 32},
    {"atomic", VAR_CAT_PARALLEL, 8, 3, 32},
    {"dispatch", VAR_CAT_PARALLEL, 9, 3, 32},
    {"worker", VAR_CAT_PARALLEL, 8, 3, 32},
    {"pool", VAR_CAT_PARALLEL, 7, 3, 32},
    {"task", VAR_CAT_PARALLEL, 7, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Message variable patterns
static const var_classification_rule_t message_rules[] = {
    {"msg", VAR_CAT_MESSAGE, 10, 3, 32},
    {"message", VAR_CAT_MESSAGE, 10, 3, 32},
    {"command", VAR_CAT_MESSAGE, 9, 3, 32},
    {"request", VAR_CAT_MESSAGE, 9, 3, 32},
    {"response", VAR_CAT_MESSAGE, 9, 3, 32},
    {"event", VAR_CAT_MESSAGE, 8, 3, 32},
    {"signal", VAR_CAT_MESSAGE, 8, 3, 32},
    {"queue", VAR_CAT_MESSAGE, 8, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Temporary variable patterns
static const var_classification_rule_t temporary_rules[] = {
    {"temp", VAR_CAT_TEMPORARY, 10, 4, 32},
    {"tmp", VAR_CAT_TEMPORARY, 10, 3, 32},
    {"temp_", VAR_CAT_TEMPORARY, 10, 5, 32},
    {"tmp_", VAR_CAT_TEMPORARY, 10, 4, 32},
    {"scratch", VAR_CAT_TEMPORARY, 10, 3, 32},
    {"work", VAR_CAT_TEMPORARY, 8, 3, 32},
    {"working", VAR_CAT_TEMPORARY, 10, 3, 32},
    {NULL, VAR_CAT_UNKNOWN, 0, 0, 0}
};

// Aspect classification rules
static const var_classification_rule_t static_aspect_rules[] = {
    {"const", VAR_ASPECT_STATIC, 10, 5, 32},
    {"final", VAR_ASPECT_STATIC, 10, 5, 32},
    {"immutable", VAR_ASPECT_STATIC, 10, 5, 32},
    {NULL, VAR_ASPECT_UNKNOWN, 0, 0, 0}
};

static const var_classification_rule_t shared_aspect_rules[] = {
    {"global", VAR_ASPECT_SHARED, 10, 6, 32},
    {"shared", VAR_ASPECT_SHARED, 10, 6, 32},
    {"common", VAR_ASPECT_SHARED, 10, 6, 32},
    {"public", VAR_ASPECT_SHARED, 10, 6, 32},
    {NULL, VAR_ASPECT_UNKNOWN, 0, 0, 0}
};

static const var_classification_rule_t private_aspect_rules[] = {
    {"local", VAR_ASPECT_PRIVATE, 10, 5, 32},
    {"private", VAR_ASPECT_PRIVATE, 10, 7, 32},
    {"static", VAR_ASPECT_PRIVATE, 10, 6, 32},
    {"internal", VAR_ASPECT_PRIVATE, 10, 8, 32},
    {NULL, VAR_ASPECT_UNKNOWN, 0, 0, 0}
};

static const var_classification_rule_t tree_aspect_rules[] = {
    {"tree", VAR_ASPECT_TREE, 10, 4, 32},
    {"node", VAR_ASPECT_TREE, 10, 4, 32},
    {"branch", VAR_ASPECT_TREE, 10, 6, 32},
    {"leaf", VAR_ASPECT_TREE, 10, 4, 32},
    {"root", VAR_ASPECT_TREE, 10, 4, 32},
    {NULL, VAR_ASPECT_UNKNOWN, 0, 0, 0}
};

static const var_classification_rule_t graph_aspect_rules[] = {
    {"graph", VAR_ASPECT_GRAPH, 10, 5, 32},
    {"network", VAR_ASPECT_GRAPH, 10, 7, 32},
    {"edge", VAR_ASPECT_GRAPH, 10, 4, 32},
    {"vertex", VAR_ASPECT_GRAPH, 10, 6, 32},
    {NULL, VAR_ASPECT_UNKNOWN, 0, 0, 0}
};

#endif // VARIABLE_CLASSIFIER_H
