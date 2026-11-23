#ifndef NX_STATE_H
#define NX_STATE_H

/*
 * nx_state — アプリの永続状態を「形式に依存せず」見つける汎用ロケータ。
 *
 * 対象は JSON ファイルと、任意バイナリ中のタブ状 JSON。
 *
 * 「タブ状 JSON」の判定は形式ではなく内容で行う:
 *   - {"tabs":[...]}  または  {"state":{"tabs":[...]}}
 * これにより、どのアプリのどのストアでも、タブ状態を見つけられる。
 *
 * KV ストア (LevelDB) のキー列挙・seq・生存判定・書き戻しは削除した。
 * 該当 API は互換のため残しているが、常に 0 / NULL を返す。
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* path: ファイルまたはディレクトリ。
   key_hint: KV ストアでキーに含まれるべき文字列 (NULL なら任意)。
   戻り値: malloc 済みのタブ状態 JSON (呼び出し側 free)。無ければ NULL。
   out_origin: 発見元 (malloc 済み, 呼び出し側 free)。NULL 可。 */
char *nx_state_find_tabs_json(const char *path, const char *key_hint,
                              char **out_origin);

/* LevelDB の書き込みに必要な「正準キー」を自動判別する。
   KV 書き戻しの削除に伴い未対応。常に NULL を返す。 */
char *nx_state_find_key(const char *dir, const char *key_hint,
                        size_t *out_key_len);

/* key_hint を含む全 user key を列挙する (診断用, 重複除去, 最大 max 件)。
   KV 列挙の削除に伴い未対応。常に 0 を返す。 */
int nx_state_list_keys(const char *dir, const char *key_hint,
                       char ***out_keys, size_t **out_lens, int max);

/* ストア内で観測された最大 seq を返す。
   KV 読み取りの削除に伴い未対応。常に 0 を返す。 */
unsigned long long nx_state_max_seq(const char *dir, const char *key_hint);

/* キーを「生バイト」で指定して、そのキーだけの最大 seq を返す。
   KV 読み取りの削除に伴い未対応。常に 0 を返す。 */
unsigned long long nx_state_max_seq_bytes(const char *dir, const char *key,
                                          size_t klen);

/* key_hint を含む key が「今も生きているか」を返す。
   KV 読み取りの削除に伴い未対応。常に 0 を返す。 */
int nx_state_key_is_live(const char *dir, const char *key_hint);

#ifdef __cplusplus
}
#endif

#endif /* NX_STATE_H */
