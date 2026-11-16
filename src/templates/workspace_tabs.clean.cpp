// ===== NX TEMPLATE =====
// id: workspace-tabs.clean
// kind: state
// target: deepai-code:workspace-tabs:v1
// learn: graph=clean-v1
//
// 原則: 「A を直してから B」と繋がない。個々の scratch (残滓・不整合・
// 古い store) はそれぞれ独立に観測し、すべてを 1 つの永続手続きへ収束
// させる。手順が枝分かれしないので、途中の段が失敗しても他へ伝染しない。
//
// 逆構成 (収束先から書く):
//   persist が唯一の永続手続き。detect / prune / live はそこへ収束する。
//   永続層を書くのは persist だけ。他は観測か、メモリ側の一致だけ。
//
// node: persist 唯一の永続手続き (leveldb に seq+1 で書く)  run: persist
// node: live    稼働中 store を同じ値にする              run: apply
// node: prune   孤児 surface を JSON から除く            run: fix
// node: detect  owner の無い surface を数える            run: clean
// node: tabs    app のタブ表示 (置き換えない)            layout: tabs
//
// link: detect converges persist
// link: prune converges persist
// link: live converges persist
// link: persist renders tabs
// ===== /NX TEMPLATE =====

/*
 * この .cpp はビルド対象ではない。neodax (tpl-cpp) が構成を読み、
 * 各ノードの run: を既存コマンドへ解決して 1 本の永続手続きとして実行する。
 *
 * ノードは互いに依存しない (requires を持たない)。どの scratch から
 * 始めても、同じ persist に合流する。だから「1 つ直したら別が壊れた」
 * が起きない。annulus はその収束の深さ (中心 = persist)。
 */

#include <cstdio>

/* detect: 状態から「参照できない実体」を数える。表示は読むだけ。 */
static int tpl_detect(const char *state) { (void)state; return 0; }

/* prune: JSON から孤児 surface を除く (まだ書かない)。 */
static int tpl_prune(const char *state) { (void)state; return 0; }

/* live: 稼働中 store を永続層と同じ値にする (app は再起動しない)。 */
static int tpl_live(const char *state) { (void)state; return 0; }

/* persist: 唯一の永続手続き。ここだけが leveldb を書く。 */
static int tpl_persist(const char *state) { (void)state; return 0; }
