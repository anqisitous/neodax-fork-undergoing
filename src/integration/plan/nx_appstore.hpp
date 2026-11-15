#ifndef NX_APPSTORE_HPP
#define NX_APPSTORE_HPP

/*
 * nx_appstore — 直前に起動した app を分析し、app ごとに保存する。
 *
 * 方針:
 *   - 直前に起動した GUI app (最近の起動時刻) を 1 つ選ぶ。
 *   - app ごとに 1 フォルダ: ~/.neodax/apps/<name>_<hash>/
 *       依存フォルダは作らない。全部そのフォルダに平坦に置く。
 *       app.path       … 対象 app のフルパス (同梱)
 *       analysis.txt   … 今回の分析
 *       after.txt      … 前回の「修正後」の分析 (比較の基準)
 *       history.tsv    … 実行履歴 (timestamp / changed / fixed)
 *   - 今回の分析が after.txt と同じなら何もしない。
 *     違うときだけ修正 (既知の入口 cut) を行い、after.txt を更新する。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/* 直前に起動した app と、その保存フォルダを表示する (変更しない)。 */
int nx_app_show(FILE *out);

/* 分析して保存し、前回の修正後データと違えば修正する。
 * fix=0 なら分析と比較だけ。 */
int nx_app_analyze(int fix, FILE *out);

/* 指定 app を分析する (GUI の選択リスト用)。 */
int nx_app_analyze_path(const char *app, int fix, FILE *out);
int nx_app_show_path(const char *app, FILE *out);

/* 最近起動した app の一覧 (新しい順)。*/ 
int nx_app_recent_count(void);
const char *nx_app_recent_path(int i);
const char *nx_app_recent_exe(int i);
void nx_app_recent_refresh(void);

/* 保存済みの app フォルダを一覧する。 */
int nx_app_list(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_APPSTORE_HPP */
