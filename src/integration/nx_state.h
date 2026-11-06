#ifndef NX_STATE_H
#define NX_STATE_H

/*
 * nx_state — アプリの永続状態を「形式に依存せず」見つける汎用ロケータ。
 *
 * LevelDB 専用ではない。次のような任意のストアを対象にする:
 *   - JSON ファイル (タブ配列を直接含む)
 *   - LevelDB   (.ldb / .log; Snappy 展開は nx_leveldb が担当)
 *   - 任意バイナリ (JSON 断片の raw scan)
 *   - SQLite / plist などは同じ raw scan でタブ状 JSON を拾える
 *
 * 「タブ状 JSON」の判定は形式ではなく内容で行う:
 *   - {"tabs":[...]}  または  {"state":{"tabs":[...]}}
 * これにより、どのアプリのどのストアでも、タブ状態を見つけられる。
 *
 * 形式ごとの抽出器は nx_state_adapter として登録でき、後から追加できる。
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
   key_hint を含む user key (例 "_deepai-code://app\x00\x01deepai-code:workspace-tabs:v1")
   のうち、最も新しい seq のものを返す (malloc 済み, 呼び出し側 free)。
   out_key_len にバイト長 (NUL を含みうるので長さ必須) を返す。 */
char *nx_state_find_key(const char *dir, const char *key_hint,
                        size_t *out_key_len);

/* key_hint を含む全 user key を列挙する (診断用, 重複除去, 最大 max 件)。
   各要素は malloc 済み。戻り値は件数。 */
int nx_state_list_keys(const char *dir, const char *key_hint,
                       char ***out_keys, size_t **out_lens, int max);

#ifdef __cplusplus
}
#endif

#endif /* NX_STATE_H */
