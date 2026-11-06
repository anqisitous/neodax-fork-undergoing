#include "nx_specs_builtin.h"

#include <string.h>

/* ---- WebSocket フレーム (RFC6455) -------------------------------- */
/* byte0: FIN(1) RSV1..3(3) opcode(4)
   byte1: MASK(1) payload_len(7)
   拡張長: 126 -> 2byte, 127 -> 8byte
   MASK ビットが立っていれば 4byte mask key                         */

static const char *ws_opcode_names[] = {
    /* 0..2 reserved */
    "reserved0", "text", "binary",                              /* 0,1,2 */
    "reserved3", "reserved4", "reserved5", "reserved6", "reserved7",
    "close", "ping", "pong",                                    /* 8,9,10 */
    "reserved11", "reserved12", "reserved13", "reserved14", "reserved15"
};

static const nx_field_t ws_fields[] = {
    /* name          kind       dec        off w  shift mask  rep enum           cnt */
    { "fin",         NX_F_BITS, NX_D_U8,   0, 1, 7, 0x1,      1, NULL, 0 },
    { "rsv1",        NX_F_BITS, NX_D_U8,   0, 1, 6, 0x1,      1, NULL, 0 },
    { "rsv2",        NX_F_BITS, NX_D_U8,   0, 1, 5, 0x1,      1, NULL, 0 },
    { "rsv3",        NX_F_BITS, NX_D_U8,   0, 1, 4, 0x1,      1, NULL, 0 },
    { "opcode",      NX_F_BITS, NX_D_U8,   0, 1, 0, 0xf,      1, ws_opcode_names, 16 },
    { "masked",      NX_F_BITS, NX_D_U8,   1, 1, 7, 0x1,      1, NULL, 0 },
    { "payload_len", NX_F_BITS, NX_D_U8,   1, 1, 0, 0x7f,     1, NULL, 0 },
};

/* WS フレームらしさ: RSV ビットが 0、opcode が既知、長さが整合する。 */
static int ws_check(const uint8_t *d, size_t n) {
    if (n < 2) return 0;
    uint8_t b0 = d[0], b1 = d[1];
    if (b0 & 0x70) return 0;                 /* RSV1-3 は通常 0 */
    int opcode = b0 & 0x0f;
    if (!(opcode == 0 || opcode == 1 || opcode == 2 ||
          (opcode >= 8 && opcode <= 10))) return 0;
    uint64_t plen = b1 & 0x7f;
    size_t hdr = 2 + (plen == 126 ? 2 : plen == 127 ? 8 : 0) + ((b1 & 0x80) ? 4 : 0);
    size_t body = plen;
    if (plen == 126) { if (n < 4) return 0; body = (d[2] << 8) | d[3]; }
    else if (plen == 127) { if (n < 10) return 0; body = 0; for (int i = 0; i < 8; i++) body = (body << 8) | d[2 + i]; }
    /* ヘッダ + body がサイズ内に収まる = 妥当 */
    return (hdr + body) <= n;
}

const nx_spec_t nx_spec_ws_frame = {
    "ws_frame", "websocket",
    "WebSocket frame (RFC6455): FIN/opcode + MASK/payload-len",
    NULL, 0, 0, ws_check,
    ws_fields, (int)(sizeof(ws_fields) / sizeof(ws_fields[0]))
};

/* ---- 汎用: 4byte BE 長 + payload --------------------------------- */
static const nx_field_t lenpre_fields[] = {
    { "length", NX_F_ABS, NX_D_U32BE, 0, 4, 0, 0, 1, NULL, 0 },
    { "body",   NX_F_LENEX, NX_D_ASCII, 4, 0, 0, 0, 1, NULL, 0 },
};

const nx_spec_t nx_spec_lenprefixed_be = {
    "lenprefixed_be", "length-prefixed",
    "big-endian 32-bit length followed by payload",
    NULL, 0, 0, NULL,
    lenpre_fields, (int)(sizeof(lenpre_fields) / sizeof(lenpre_fields[0]))
};

/* ---- HTTP ステータス行 ------------------------------------------- */
static const nx_field_t http_fields[] = {
    { "status", NX_F_ABS, NX_D_U16BE, 9, 2, 0, 0, 1, NULL, 0 },
};

static int http_check(const uint8_t *d, size_t n) {
    return n >= 12 && memcmp(d, "HTTP/", 5) == 0;
}

const nx_spec_t nx_spec_http_status = {
    "http_status_line", "http",
    "HTTP status line (HTTP/x.y NNN ...)",
    NULL, 0, 0, http_check,
    http_fields, (int)(sizeof(http_fields) / sizeof(http_fields[0]))
};

void nx_specs_builtin_register(void) {
    nx_spec_register(&nx_spec_ws_frame);
    nx_spec_register(&nx_spec_lenprefixed_be);
    nx_spec_register(&nx_spec_http_status);
}
