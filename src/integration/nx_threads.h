#ifndef NX_THREADS_H
#define NX_THREADS_H

/*
 * nx_threads — チャット (thread) 台帳を neodax 側で扱う。
 *
 * なぜ要るか:
 *   タブ (leveldb) には threadId しか無く、表示名や重複は
 *   app の SQLite (statev2.sqlite) にしか無い。目視や外部 CLI で
 *   判断すると再現できないので、ここで C 側から読む。
 *
 * 「消したのに戻る」の本体:
 *   サイドバーは orchestration_events (追記ログ) から projection を作る。
 *   projection の行だけ消しても、次の再構築で thread.created から戻る。
 *   よって削除は 2 つを 1 つのトランザクションで行う:
 *     1) thread.deleted イベントを追記 (seq / stream_version は実 max+1)
 *     2) projection の deleted_at / archived_at を更新
 *   これはアプリの delete と同じ操作で、再起動も reload も要らない。
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 既定 DB (~/.deepai-code/userdata/statev2.sqlite)。0=成功 */
int nx_threads_db_path(char *out, size_t cap);

/* 一覧 (include_deleted=0 なら生存のみ) */
int nx_threads_list(const char *db, FILE *out, int include_deleted);

/* 重複 (同一タイトル・作成時刻が within_sec 以内) を検出。
   apply=1 なら 1 つだけ残す。残す基準は updated_at ではない
   (複製間の差は同期の遅れだけで、意味がない)。
     - protect_csv に含まれる id (現在タブが参照している会話) があればそれ
     - なければ最初に作られたもの (原本)
   後から同期で増えた複製を落とす。 */
int nx_threads_dups(const char *db, FILE *out, int within_sec, int apply,
                    const char *protect_csv);

/* 1 件を恒久削除 (イベント追記 + projection 更新)。 */
int nx_threads_delete(const char *db, const char *id, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_THREADS_H */
