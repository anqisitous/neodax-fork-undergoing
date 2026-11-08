#ifndef NX_APPFIX_FLOW_HPP
#define NX_APPFIX_FLOW_HPP

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/* AE の fork を 1 コマンドで作る。
   複製 -> ShouldCheckLicense を無効化 -> 差分検証 -> 再署名 -> 検証。
   元のアプリ (src_app) には一切触らない。
   成功で 0。既に dst_app があり force=0 なら 1 (置き換えない)。 */
int nx_appfix_make(const char *src_app, const char *dst_app, int force, FILE *out);

#ifdef __cplusplus
}
#endif

#endif
