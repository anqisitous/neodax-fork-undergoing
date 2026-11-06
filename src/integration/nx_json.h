#ifndef NX_JSON_H
#define NX_JSON_H

/*
 * nx_json — 依存ゼロの最小 JSON パーサ。
 *
 * 用途:
 *   - Electron の app.asar ヘッダ (JSON) を解析してエントリ表を得る。
 *   - DeepAI Code の localStorage に保存された workspace-tabs 状態を解析し、
 *     閉じられない surface タブを除去した修復 JSON を生成する。
 *
 * 特徴:
 *   - 各ノードが元ソース内の raw 範囲 (raw_begin/raw_end) を保持する。
 *     これにより「一度 parse した値を再エスケープせず、元のバイト列を
 *     そのまま組み立て直す」ことができ、文字列の取りこぼしが無い。
 *   - 外部ライブラリ不使用。
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NX_JSON_NULL = 0,
    NX_JSON_BOOL,
    NX_JSON_NUM,
    NX_JSON_STR,
    NX_JSON_ARR,
    NX_JSON_OBJ
} nx_json_type_t;

typedef struct nx_json nx_json_t;

struct nx_json {
    nx_json_type_t type;

    const char *src;        /* 所有はしない。パース対象バッファを指す       */
    size_t      raw_begin;  /* src 内の先頭バイト位置                       */
    size_t      raw_end;    /* 次のバイト位置 (排他)                        */

    /* scalar */
    double      num;
    int         boolean;
    char       *str;        /* STR: unescape 済み (NUL 終端)                */
    size_t      str_len;

    /* container */
    nx_json_t **items;      /* ARR/OBJ: 値ノード                           */
    char      **keys;       /* OBJ: キー (NUL 終端)                        */
    size_t     *key_lens;
    size_t      n;
};

/* パース。失敗時は NULL。成功時は nx_json_free で解放する。 */
nx_json_t *nx_json_parse(const char *src, size_t len);
void       nx_json_free(nx_json_t *v);

/* 参照系 */
const nx_json_t *nx_json_get(const nx_json_t *obj, const char *key);
const nx_json_t *nx_json_at(const nx_json_t *arr, size_t i);
const char      *nx_json_str(const nx_json_t *v);
double           nx_json_number(const nx_json_t *v, double dflt);
int              nx_json_is(const nx_json_t *v, nx_json_type_t t);

/* raw slice (src 内のバイト列) を out/fd に。長さを返す。 */
size_t nx_json_raw(const nx_json_t *v, const char **out);

#ifdef __cplusplus
}
#endif

#endif /* NX_JSON_H */
