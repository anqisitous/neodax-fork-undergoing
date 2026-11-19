#ifndef NX_RECIPE_HPP
#define NX_RECIPE_HPP

/*
 * nx_recipe — 「書き換え方の手順」を先に保存し、後から段ごとに検証して実行する。
 *
 * なぜ必要か:
 *   アプリを最初から直接書き換えると、間違った前提のまま一気に壊す。
 *   そうではなく、まず手順書 (recipe) を作って保存し、手順が最終形に
 *   なってから 1 段ずつ「うまくいくか」を確かめる。
 *
 * 3 つの安全原則をステップ種別として持つ:
 *   P4: 不可侵クローン (clone) / 原子的署名 (resign, verify-sign) / 失敗時 rollback
 *   P2: 各段の後に再観測 (observe) — 書いたつもりを成功と呼ばない
 *   P3: アドレス解決 (resolve) と差分範囲契約 (patch の lo/hi, verify-range)
 *
 * 設計:
 *   recipe はテキスト。1 行 1 ステップ。順序と引数をそのまま保存する。
 *   build は「保存だけ」でアプリを変更しない。
 *   run は既定 dry-run。--apply のときだけ実行し、各段で検証する。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/* 手順書を作って out_path に保存する。アプリは変更しない。
 * app_path: 対象 .app。out_path: 保存先 (NULL なら表示のみ)。
 * target_hint: 優先して切りたい関数名の部分一致 (NULL なら既定の順)。
 * 戻り値: 0=保存/表示成功, 1=未解決ステップあり (手順は未完成), 2=エラー */
int nx_recipe_build(const char *app_path, const char *target_hint,
                    const char *out_path, FILE *out);

/* 手順書を表示する (実行しない)。 */
int nx_recipe_show(const char *recipe_path, FILE *out);

/* バイナリ内のシンボル位置 (vaddr とファイルオフセット)。 */
typedef struct { unsigned long long vaddr, off; } nx_site_t;

/* name を部分一致で解決する。見つからなければ 1 を返す。 */
int nx_recipe_locate_symbol(const char *lib, const char *name, nx_site_t *out);

/* target_vaddr を呼ぶ BL 命令の位置を列挙する。out[i].off は BL のファイルオフセット、
   out[i].vaddr は BL 命令の仮想アドレス。戻り値は件数。 */
int nx_recipe_find_callers(const char *lib, unsigned long long target_vaddr,
                           nx_site_t *out, int max);

/* あるアドレスの命令語 (4 バイト) を読む。 */
int nx_recipe_read_word(const char *lib, unsigned long long off, unsigned long *word);

/* 手順書を実行する。
 * apply=0: dry-run (各段の内容と前提だけ検証し、変更しない)
 * apply=1: 実行。各段の後に検証し、失敗したら rollback して停止。
 * upto: 最初から何段目まで実行するか (0 以下なら全部)。
 * 戻り値: 0=成功, 1=検証失敗 (rollback 済), 2=エラー/未完成 */
int nx_recipe_run(const char *recipe_path, int apply, int upto, FILE *out);

/* 一発のアプリ修復: recipe を作り、そのまま run する。
 * apply=0 は dry-run (アプリを変更しない)。apply=1 で作業コピー
 * (<app>.neodax-work) を作る。戻り値は nx_recipe_run と同じ。 */
int nx_fix_app(const char *app_path, const char *target_hint, int apply, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_RECIPE_HPP */
