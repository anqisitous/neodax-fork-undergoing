#ifndef NX_PE_DISASM_H
#define NX_PE_DISASM_H

/*
 * nx_pe_disasm — PE の実行セクションを命令デコードし、関数を抽出する。
 *
 * nx_pe (ヘッダ/セクション解析) と nx_decode (x86/arm64 デコーダ) をつなぎ、
 *   .text -> 線形デコード -> 関数境界 (entry / call 先 / ret 後) -> 一覧
 * を出力する。Mach-O 側と同じ「同等のパイプライン」のコード解析段。
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* path の実行セクションをデコード。limit 命令まで (0 で既定 4000)。 */
int nx_pe_disasm(const char *path, int limit, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_PE_DISASM_H */
