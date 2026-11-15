#ifndef NX_STRATEGY_HPP
#define NX_STRATEGY_HPP

/*
 * nx_strategy — 介入戦略の一般探索。「観測 → テンプレ」で進める。
 *
 * アプリ固有の名前は入れない。対象シンボルについて事実 (Facts) を集め、
 * 当てはまる介入テンプレ (Template) をリスクの低い順に並べ、先頭から試す。
 *
 *   Facts     : 呼び出し元の数 / 入口の命令 / 呼び出し元の位置 など
 *   Template  : 前提 (applies) と 手順 (build) を持つ
 *
 * 実例から得た一般則:
 *   関数入口を丸ごと ret にする (ノードを潰す) と、呼び出し元が依存する
 *   副作用・初期化まで消えて落ちる。
 *   呼び出し (エッジ) を NOP にする方が安全なことが多い。
 *   データ (フラグ/定数) を変えるのが最も安全。
 *
 * 従ってテンプレは「介入する面」の広さで並べる:
 *   data < edge(one) < edge(all) < entry_ret
 *
 * 検証は nx_recipe に委譲する (clone / 範囲契約 / 再観測 / 再署名 / rollback)。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/* 観測した事実。ここだけがアプリ依存の入り口。 */
typedef struct {
    char symbol[128];
    unsigned long long vaddr;      /* 対象関数の仮想アドレス */
    unsigned long long off;        /* 対象関数のファイルオフセット */
    int n_callers;                 /* BL で呼ぶ箇所の数 */
    unsigned long long caller_off[64];
    unsigned long long caller_va[64];
    unsigned long first_word;      /* 入口の命令語 (分岐判定用) */
    int first_is_cond_branch;      /* 入口が条件分岐か */
} nx_facts_t;

/* テンプレ ID。 */
typedef enum {
    NX_TPL_DATA = 0,      /* データを書き換える (最も安全) */
    NX_TPL_EDGE_ONE,      /* 特定の呼び出し 1 つを NOP */
    NX_TPL_EDGE_ALL,      /* 呼び出し (少数) をまとめて NOP */
    NX_TPL_ENTRY_RET,     /* 関数入口を ret (最も危険) */
} nx_tpl_t;

/* 1 つの推力候補 (テンプレを Facts に当てはめた結果)。 */
typedef struct {
    nx_tpl_t tpl;
    char name[32];
    double risk;              /* 0..1。低いほど安全 */
    int applicable;
    char why[200];
} nx_choice_t;

/* Facts を集める。戻り値 0=成功。 */
int nx_strategy_observe(const char *app, const char *symbol, nx_facts_t *f, FILE *out);

/* Facts からテンプレを当てはめ、リスク昇順で choices を返す。 */
int nx_strategy_plan(const nx_facts_t *f, nx_choice_t *choices, int max);

/* 失敗の記憶 (有界)。
 *
 * 容量:
 *   作業用テンプレ (active) は最大 10 個
 *   全体 (archive) は最大 200 個
 * 間違い指数は「分岐を辿った確率」で近似し、雑でよい。
 * 指数の再計算は「間違ったタイミング」かつ容量を超えた時だけ、
 * 200 個全体に対して行う (毎回はやらない)。
 */
#define NX_ACTIVE_MAX 10
#define NX_TOTAL_MAX  200

/* 1 回の試行を記録する。branch はその試行を選んだ分岐パス。
 * ok=0 が「間違い」。戻り値: 0=記録した, 1=容量超過で枝刈りした。 */
int nx_strategy_record(const char *app, const char *symbol,
                       const char *branch, int ok);

/* branch が枝刈り済みか。理由を out に書く (NULL 可)。
 * 戻り値 1 = pruned, 0 = 有効。 */
int nx_strategy_is_pruned(const char *app, const char *symbol,
                          const char *branch, char *why, size_t why_cap);

/* 保存されている成否履歴と、現在の間違い指数を表示する。 */
int nx_strategy_stats_show(FILE *out);

/* 観測 → テンプレ適用。
 * apply=0: 観測と順序の表示のみ (変更しない)
 * apply=1: 先頭の適用可能テンプレを「作業コピー」に適用し、検証まで行う。
 *          原版は変更しない。失敗時 rollback。
 * probe_ms>0: 適用後に作業コピーを起動し、probe_ms 以上生存したら成功とする。 */
int nx_strategy_search(const char *app, const char *symbol, int apply,
                       int probe_ms, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_STRATEGY_HPP */
