#ifndef NX_SPECS_BUILTIN_H
#define NX_SPECS_BUILTIN_H

/* 組み込みの「構造記述」群。WebSocket フレームはその 1 つにすぎない。 */

#include "nx_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WebSocket フレーム (RFC6455) を nx_spec_t として記述したもの。 */
extern const nx_spec_t nx_spec_ws_frame;

/* 汎用の「長さ前置き」バイナリ (4byte BE 長 + payload) の記述例。 */
extern const nx_spec_t nx_spec_lenprefixed_be;

/* HTTP レスポンスヘッダ行の記述例。 */
extern const nx_spec_t nx_spec_http_status;

/* PE 構造 */
extern const nx_spec_t nx_spec_pe_dos_header;
extern const nx_spec_t nx_spec_pe_nt_signature;
extern const nx_spec_t nx_spec_pe_file_header;
extern const nx_spec_t nx_spec_pe_section_header;

/* 登録 */
void nx_specs_builtin_register(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_SPECS_BUILTIN_H */
