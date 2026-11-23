/* chained_style.h — 「}{連結スタイル」整形器の部分関数 (低コード用)
 *
 * どの関数も「バイト列を入れて、バイト列を出す」だけ。組み合わせるとモードが増える。
 *
 *   nx_style_span     範囲と基準桁を指定して整形 (他の全部がここに落ちる)
 *   nx_style_span_auto  基準桁を自分で決める
 *   nx_style_all      ファイル全体 (//[ //] も無視して中まで整形)
 *   nx_style_outside  //[ //] の中はそのまま、外だけ整形
 *   nx_style_markers  //[ //] の中だけ整形 (従来モード)
 *   nx_style_run      名前から選ぶ (CLI はこれ 1 行)
 *   nx_style_same     2 つの出力が同じか
 */
#ifndef NX_CHAINED_STYLE_H
#define NX_CHAINED_STYLE_H

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct {
    char *p;
    size_t n, cap;
} NxStyleBuf;

void nx_style_span(const char *s, int a, int b, int indent, NxStyleBuf *out);
void nx_style_span_auto(const char *s, int a, int b, NxStyleBuf *out);
int  nx_style_indent_of(const char *s, int a, int b);
int  nx_style_skip_bom(const char *s, int n);

void nx_style_all(const char *s, int n, NxStyleBuf *out);
void nx_style_outside(const char *s, int n, NxStyleBuf *out);
void nx_style_markers(const char *s, int n, NxStyleBuf *out);

int  nx_style_same(const NxStyleBuf *a, const NxStyleBuf *b);
int  nx_style_run(const char *mode, const char *s, int n, NxStyleBuf *out);
void nx_style_free(NxStyleBuf *b);

/* 整形の対象にする拡張子か (C 系のみ)。*/
int  nx_style_is_source(const char *path);

/* ファイル 1 つを整形して結果を返す (0=成功)。out は呼び側が free する。 */
int  nx_style_file(const char *path, const char *mode, NxStyleBuf *out);
/* 整形結果を書き戻す (同じ inode に書く)。 0=成功 */
int  nx_style_save(const char *path, const NxStyleBuf *b);

#if defined(__cplusplus)
}
#endif

#endif
