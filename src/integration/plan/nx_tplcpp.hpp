#ifndef NX_TPLCPP_HPP
#define NX_TPLCPP_HPP

/*
 * nx_tplcpp — 「テンプレート = C++ ファイル」の登録と学習。
 *
 * なぜ C++ ファイルなのか:
 *   修復の 1 手は、結局どこかの C++ の関数を呼ぶ。設定ファイル (TSV/JSON) に
 *   手順だけを書くと、実装と手順が離れて「書いたのに動かない」が起きる。
 *   そこで 1 テンプレート = 1 .cpp とし、その中に
 *     - 構成 (どのノードが何に依存するか) をコメントのヘッダとして
 *     - 実際の処理を C++ の関数として
 *   同居させる。構成は「最後の段 → 最初の段」の逆順で書く (逆構成)。
 *
 * ヘッダ書式 (テンプレート .cpp の先頭付近):
 *
 *   // ===== NX TEMPLATE =====
 *   // id: workspace-tabs.clean
 *   // kind: state
 *   // target: deepai-code:workspace-tabs:v1
 *   // node: detect   owner の無い surface を数える        (run: clean --dry-run)
 *   // node: prune    孤児 surface を JSON から除く         (run: persist)
 *   // node: persist  seq+1 で永続層へ書き戻す             (run: persist)
 *   // node: live     稼働中 store を同じ値にする          (run: live)
 *   // link: live requires persist
 *   // link: persist requires prune
 *   // link: prune requires detect
 *   // learn: graph=clean-v1
 *   // ===== /NX TEMPLATE =====
 *
 *   (以下に C++ の実装。ノード名と同じ関数を置いてよい)
 *
 * なぜ「逆構成」か:
 *   実行順は detect → prune → persist → live。学習で枝刈りするときは
 *   「最後に何をしたいか」から逆に辿る方が、失敗した段の手前だけを
 *   差し替えられる。だからヘッダは reverse で書く。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[160];
    char kind[64];
    char target[240];
    char label[200];
    char run[200];       /* 実行に使う既存コマンド名 (空なら表示のみ) */
    char file[512];
    int  line;
} nx_tpl_node_t;

typedef struct {
    int  from;           /* node index */
    int  to;
    char kind[24];       /* requires / next */
    int  pruned;         /* 学習で枝刈り済み */
} nx_tpl_edge_t;

typedef struct {
    nx_tpl_node_t *nodes;  size_t n_nodes;
    nx_tpl_edge_t *edges;  size_t n_edges;
    int  *order;           /* 実行順 (node index) */
    size_t n_order;
    int  *ring;            /* 同じ ring は同じ annulus (SCC) */
    int   n_rings;
    char  graph[128];
} nx_tpl_graph_t;

/* dir 配下の *.cpp を読んで構成を集める。0=成功。 */
int nx_tplcpp_scan(const char *dir, nx_tpl_graph_t *out);
void nx_tplcpp_free(nx_tpl_graph_t *g);

/* 実行順・依存・学習状態を表示する。 */
int nx_tplcpp_dump(const nx_tpl_graph_t *g, FILE *out);

/* annulus (SCC リング) + ノード + エッジを SVG に書く。 */
int nx_tplcpp_svg(const nx_tpl_graph_t *g, const char *path, FILE *out);

/* 学習: グラフ単位で分岐の成否を記録する (nx_strategy の有界記憶を使う)。 */
int nx_tplcpp_record(const nx_tpl_graph_t *g, const char *branch, int ok, FILE *out);

/* 既定のテンプレート置き場 (~/.neodax/templates)。 */
const char *nx_tplcpp_default_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_TPLCPP_HPP */
