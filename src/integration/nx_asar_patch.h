#ifndef NX_ASAR_PATCH_H
#define NX_ASAR_PATCH_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* needle を含むエントリの絶対 offset/size を出す。成功で 0。 */
int nx_asar_locate(const char *asar, const char *needle, FILE *out);

/* エントリ内の find を repl に差し替える。同長であること。
   変えた箇所数を返す (0 なら 1 を返す)。成功で 0。 */
int nx_asar_patch_bytes(const char *asar, const char *needle_entry,
                        const char *find, const char *repl, FILE *out);

#ifdef __cplusplus
}
#endif

#endif
