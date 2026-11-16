#ifndef NX_PROJECT_HPP
#define NX_PROJECT_HPP

/*
 * nx_project — 分析をプロジェクトファイルに保存する。
 *
 * 方針 (GUI と同じ順序):
 *   1. まず path を決める。path が決まるまで 1 バイトも書かない。
 *   2. path が決まったら、そこに分析を書く。
 *
 * 「先に path、後から書き込み」を守る理由:
 *   書くと決めてから場所を考えると、失敗したときに中途半端な
 *   ファイルが残る。先に path を確定させれば、書く前に
 *   「そこに書けるか」を確かめられる。
 *
 * 保存の形 (1 プロジェクト 1 フォルダ):
 *   <path>/
 *     project.tsv     … どの状態を見たか (source / seq / 指紋)
 *     analysis.tsv    … 分析の結果 (コマンド / 判定 / 件数)
 *     frames.tsv      … frame ごとにどれだけ動いたか
 *     history.tsv     … いつ何をしたか (追記のみ)
 *
 * 同じ分析が既にあれば書かない (重複を増やさない)。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/* path を決める。既存の状態から既定の場所を出す (まだ書かない)。
   戻り値: 0=決まった, 負値=決まらない。 */
int nx_project_default_path(const char *state_path, char *out, size_t cap);

/* path に書けるかを確かめる (まだ書かない)。
   戻り値: 0=書ける, 1=フォルダが無い (作れば書ける), 負値=書けない。 */
int nx_project_can_write(const char *path);

/* 分析を保存する。path が NULL か空なら、既定の場所を決めてから書く。
   analysis は TSV (kind\tkey\tvalue の行)。
   同じ内容が既にあれば書かない (dup=1 で戻る)。
   戻り値: 0=書いた, 1=同じなので書かなかった, 負値=エラー。 */
int nx_project_save(const char *path, const char *source,
                    const char *analysis_tsv, FILE *out);

/* frame の動きを追記する。 */
int nx_project_save_frame(const char *path, int frame,
                          unsigned long long seq_before,
                          unsigned long long seq_after,
                          int resolved, int opened, int cut,
                          int progressed, FILE *out);

/* 保存済みの分析を読む (表示用)。 */
int nx_project_show(const char *path, FILE *out);

/* 保存済みの分析を一覧する。 */
int nx_project_list(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_PROJECT_HPP */
