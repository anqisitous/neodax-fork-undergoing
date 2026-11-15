#ifndef NX_TEMPLATE_HPP
#define NX_TEMPLATE_HPP

/*
 * nx_template — neodax が解析しながら「やったこと」をテンプレートとして残す。
 *
 * 方針:
 *   - 名前で管理しない。リストに追記するだけ。
 *   - 1 行 = 1 テンプレート (kind / target / symbol / vaddr / action / status)。
 *   - 同じ (kind, target, symbol) は状態だけ更新する (重複を増やさない)。
 *   - 保存先: ~/.neodax/templates.tsv
 *
 * これにより「あなたが手でやった」ではなく、
 * neodax のコマンドが解析・除去・記録を一貫して行う。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/* 追記 (既存なら status 更新)。applied=1 で適用済みを記録。 */
int nx_template_record(const char *kind, const char *target, const char *symbol,
                       unsigned long long vaddr, const char *action,
                       int applied, FILE *out);

/* 保存済みテンプレートを一覧する。 */
int nx_template_list(FILE *out);

/* 保存先のパス。 */
const char *nx_template_path(void);

/* 解析→テンプレート保存→除去→再観測を neodax 内で一貫実行する。
 * apply=0 なら解析と計画まで。apply=1 で最後まで。
 * 途中介入は nx_cut_step_cmd / nx_cut_status / nx_cut_abort。 */
int nx_cut(const char *app, const char *symbol, int apply, FILE *out);

/* 1 段だけ進める (clone → verify → patch → verify-range → observe → resign → verify-sign)。 */
int nx_cut_step_cmd(const char *app, const char *symbol, FILE *out);

/* 途中状態の一覧 / 中止。 */
int nx_cut_status(FILE *out);
int nx_cut_abort(const char *app, const char *symbol, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_TEMPLATE_HPP */
