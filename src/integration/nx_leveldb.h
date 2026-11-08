#ifndef NX_LEVELDB_H
#define NX_LEVELDB_H

/*
 * nx_leveldb — 依存ゼロの LevelDB リーダ (Chromium/Electron の Local Storage 用)。
 *
 * 対応:
 *   - .ldb (SSTable): footer -> index block -> data blocks
 *   - .log (Write-Ahead Log): 32KB ブロックのレコードを再構成
 *   - Snappy 展開 (leveldb の block compression)
 *
 * これにより、Electron アプリが Local Storage に書いた値を
 * 圧縮状態のままでも読み出せる。
 */

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nx_kv_fn)(const char *userkey, size_t klen,
                         const unsigned char *val, size_t vlen,
                         unsigned long long seq, void *ud);

/* 1 ファイル (.ldb / .log) を走査。見つけた KV を seq 付きで cb に渡す。 */
int nx_leveldb_scan_file(const char *path, nx_kv_fn cb, void *ud);

/* LevelDB ディレクトリへ 1 件書き戻す。
   log ファイルを新規作成する (アプリ終了済みでロックが無いこと)。
   key は user key 全体 (内部の 8byte tag は含めない)。
   seq は既存最大値より大きくする必要がある (呼び出し側で決める)。 */
int nx_leveldb_write_kv(const char *dir, const char *key, size_t klen,
                        const unsigned char *val, size_t vlen,
                        unsigned long long seq, const char *log_name, FILE *out);

/* 既存の WAL (.log) の末尾に 1 件追記する。
   app が起動中でも、現在使われている WAL に追記すれば次回読込で反映される。
   log_path を省略すると、dir 内で最大番号の *.log を自動選択。 */
int nx_leveldb_append_kv(const char *dir, const char *key, size_t klen,
                         const unsigned char *val, size_t vlen,
                         unsigned long long seq, const char *log_path, FILE *out);

/* key を削除 (kTypeDeletion) として WAL に追記する。
   空値の書き込みではなく、LevelDB 本来の削除レコードを書く。 */
int nx_leveldb_append_delete(const char *dir, const char *key, size_t klen,
                             unsigned long long seq, const char *log_path,
                             FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_LEVELDB_H */
