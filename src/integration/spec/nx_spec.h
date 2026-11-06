#ifndef NX_SPEC_H
#define NX_SPEC_H

/*
 * nx_spec — 「構造の記述」を宣言的に扱う汎用モデル。
 *
 * 目的:
 *   WebSocket に限らず、任意のバイナリ/テキスト構造を
 *   「フィールドの並び」として記述し、解析・判定・説明に使う。
 *   エンジン側はこの記述だけを見て動き、特定プロトコルを知らない。
 *
 * 構成:
 *   nx_field_t   : 1 フィールド (名前・位置・幅・デコーダ種別)
 *   nx_spec_t    : 1 構造 (名前・判定条件・フィールド列)
 *   エンジンは nx_spec_t を走査して「与えられたバイト列が何か」を判定する。
 *
 * WebSocket のフレームは「このモデルに登録された 1 つの spec」にすぎない。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* フィールドの置き方 */
typedef enum {
    NX_F_ABS = 0,   /* offset バイト目から width バイト (リトル/ビッグは dec で) */
    NX_F_BITS,      /* offset はバイト、bit_shift/bit_mask で一部を抽出 */
    NX_F_LENEX,     /* 直前フィールドを長さとして可変長データを消費 */
    NX_F_REPEAT     /* 直前の要素を repeat 回 */
} nx_field_kind_t;

/* 値のデコーダ */
typedef enum {
    NX_D_U8 = 0, NX_D_U16BE, NX_D_U16LE, NX_D_U32BE, NX_D_U32LE,
    NX_D_U64BE, NX_D_U64LE, NX_D_BOOL, NX_D_ENUM, NX_D_ASCII
} nx_decode_t;

typedef struct {
    const char     *name;
    nx_field_kind_t kind;
    nx_decode_t     dec;
    int             offset;     /* NX_F_ABS / NX_F_BITS で使用 */
    int             width;      /* バイト幅 */
    int             bit_shift;
    uint64_t        bit_mask;
    int             repeat;
    const char     *const *enum_names;  /* NX_D_ENUM 用 (NULL 終端) */
    int             enum_count;
} nx_field_t;

/* 判定: magic が一致するか、または check が非 0 を返す */
typedef int (*nx_spec_check_fn)(const uint8_t *data, size_t size);

typedef struct nx_spec {
    const char      *name;
    const char      *family;      /* 上位分類 (例 "websocket", "length-prefixed") */
    const char      *summary;     /* 一言説明 */
    const uint8_t   *magic;       /* NULL 可 */
    size_t           magic_len;
    int              magic_offset;
    nx_spec_check_fn check;       /* NULL 可。magic と AND */
    const nx_field_t *fields;
    int              n_fields;
} nx_spec_t;

/* 登録/取得 (エンジンは名前を知らずに全走査できる) */
void                 nx_spec_register(const nx_spec_t *s);
const nx_spec_t     *nx_spec_by_name(const char *name);
const nx_spec_t     *nx_spec_at(int i);
int                  nx_spec_count(void);
void                 nx_spec_register_all(void);   /* 組み込み spec を登録 */

/* デコードに使う共通ヘルパ */
uint64_t nx_spec_read_int(const uint8_t *data, size_t size, const nx_field_t *f);
const char *nx_spec_enum_name(const nx_field_t *f, uint64_t v);

#ifdef __cplusplus
}
#endif

#endif /* NX_SPEC_H */
