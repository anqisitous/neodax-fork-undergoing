#ifndef NX_PAYLOAD_H
#define NX_PAYLOAD_H

/*
 * nx_payload — レンダラで実行する JS ペイロードのビルダ (アプリ非依存の部品)。
 *
 * ここに置くのは「JS 文字列を作る」純関数だけ。実行は nx_renderer が行う。
 * これにより attach パッケージは
 *   nx_inspect (プロセス/inspector)
 *   nx_renderer (webContents 実行)
 *   nx_payload (JS 生成)
 *   nx_appfix (対象アプリの具体策)
 * に閉じて分割される。
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* base64 エンコード (JS へバイナリ安全に渡す用)。呼び出し側 free。 */
char *nx_b64_encode(const char *data, size_t len);

/* Zustand persist ストアを再水和する JS を生成する。
   - store_substr: ストアのアセット名の一部 (performance entries から探す)
   - key / value: value が非 NULL なら先に localStorage[key]=value を書き、rehydrate
   - export_names: ストアの export 名の候補 (カンマ区切り、例 "n,default")。NULL なら "default"
   戻り値は malloc 済み JS。呼び出し側 free。 */
char *nx_payload_build_rehydrate(const char *store_substr,
                                 const char *key, const char *value, size_t value_len,
                                 const char *export_names);

/* localStorage[key] を削除してから rehydrate する JS。 */
char *nx_payload_build_remove_key(const char *store_substr, const char *key,
                                  const char *export_names);

/* 任意の localStorage キーを読む JS。 */
char *nx_payload_build_read_key(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* NX_PAYLOAD_H */
