#ifndef NX_RECOG_H
#define NX_RECOG_H

/*
 * nx_recog — 与えられたバイト列が「何の構造か」を判定し、説明する汎用認識器。
 *
 * nx_spec の記述だけを根拠に動き、特定プロトコルをハードコードしない。
 *   - 登録済み spec を総当たりし、magic/check/妥当性でスコア付ける
 *   - 最有力 spec のフィールドをデコードして列挙
 *   - WebSocket のように「フレーム列」を成す構造は、連続フレームとして分解する
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const struct nx_spec *spec;
    int    score;         /* 0..100 */
    char   reason[128];
} nx_recog_hit_t;

/* バイト列を判定。上位 hit を hits に最大 max 件まで返す。戻り値は件数。 */
int nx_recog_identify(const uint8_t *data, size_t size,
                      nx_recog_hit_t *hits, int max);

/* 判定 + フィールドデコードを可読出力。 */
int nx_recog_explain(const uint8_t *data, size_t size, FILE *out);

/* WebSocket フレーム列を解析して各フレームを説明する (WS spec を利用)。
   戻り値は解析できたフレーム数。 */
int nx_recog_ws_frames(const uint8_t *data, size_t size, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_RECOG_H */
