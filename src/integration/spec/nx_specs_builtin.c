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

/* ---- Mach-O (.o / 実行ファイル / dylib / fat) -------------------- */
/* 先頭 4 バイトの magic で 32/64bit・エンディアンが決まる。
   .o も実行ファイルも同じヘッダなので、filetype だけが違う。 */
static const char *macho_filetype_names[] = {
    "?0", "object", "execute", "fvmlib", "core", "preload",
    "dylib", "dylinker", "bundle", "dylib_stub", "dsym", "kext_bundle"
};

static const nx_field_t macho_fields[] = {
    { "magic",     NX_F_ABS, NX_D_U32LE, 0,  4, 0, 0, 1, NULL, 0 },
    { "cputype",   NX_F_ABS, NX_D_U32LE, 4,  4, 0, 0, 1, NULL, 0 },
    { "cpusubtype",NX_F_ABS, NX_D_U32LE, 8,  4, 0, 0, 1, NULL, 0 },
    { "filetype",  NX_F_ABS, NX_D_U32LE, 12, 4, 0, 0, 1, macho_filetype_names, 12 },
    { "ncmds",     NX_F_ABS, NX_D_U32LE, 16, 4, 0, 0, 1, NULL, 0 },
    { "sizeofcmds",NX_F_ABS, NX_D_U32LE, 20, 4, 0, 0, 1, NULL, 0 },
};

static uint32_t macho_le32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
           ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

/* ヘッダ自身の整合性 (ncmds*8 が sizeofcmds に収まる) まで確認する。
   これで「たまたま同じ 4 バイトを持つ」データを弾ける。 */
static int macho_thin_check(const uint8_t *d, size_t n) {
    if (n < 28) return 0;
    uint32_t magic = macho_le32(d);
    uint32_t hdr = (magic == 0xfeedfacf) ? 32 : 28;   /* 64bit は reserved 付き */
    if (n < hdr) return 0;
    uint32_t ncmds = macho_le32(d + 16);
    uint32_t sz = macho_le32(d + 20);
    if (ncmds > 65536) return 0;
    if ((size_t)hdr + sz > n) return 0;
    if (ncmds && sz < (uint64_t)ncmds * 8) return 0;  /* load command 最小 8byte */
    return 1;
}

static int macho64_check(const uint8_t *d, size_t n) {
    return n >= 4 && macho_le32(d) == 0xfeedfacf && macho_thin_check(d, n);
}
static int macho32_check(const uint8_t *d, size_t n) {
    return n >= 4 && macho_le32(d) == 0xfeedface && macho_thin_check(d, n);
}

static const uint8_t macho_magic64[4] = { 0xcf, 0xfa, 0xed, 0xfe };
static const uint8_t macho_magic32[4] = { 0xce, 0xfa, 0xed, 0xfe };

const nx_spec_t nx_spec_macho64 = {
    "macho64", "macho", "Mach-O 64-bit (magic feedfacf; .o=filetype object)",
    macho_magic64, 4, 0, macho64_check,
    macho_fields, (int)(sizeof(macho_fields) / sizeof(macho_fields[0]))
};
const nx_spec_t nx_spec_macho32 = {
    "macho32", "macho", "Mach-O 32-bit (magic feedface)",
    macho_magic32, 4, 0, macho32_check,
    macho_fields, (int)(sizeof(macho_fields) / sizeof(macho_fields[0]))
};

static const nx_field_t macho_fat_fields[] = {
    { "magic",    NX_F_ABS, NX_D_U32BE, 0, 4, 0, 0, 1, NULL, 0 },
    { "nfat_arch",NX_F_ABS, NX_D_U32BE, 4, 4, 0, 0, 1, NULL, 0 },
};
static const uint8_t macho_magic_fat[4] = { 0xca, 0xfe, 0xba, 0xbe };
/* fat: 8 + nfat_arch*20 (32bit) が実際のサイズと整合するか */
static int macho_fat_check(const uint8_t *d, size_t n) {
    if (n < 8) return 0;
    uint32_t nf = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                  ((uint32_t)d[6] << 8) | (uint32_t)d[7];
    if (nf == 0 || nf > 64) return 0;
    return (size_t)8 + (size_t)nf * 20 <= n;
}
const nx_spec_t nx_spec_macho_fat = {
    "macho_fat", "macho", "Mach-O universal binary (cafebabe + nfat_arch)",
    macho_magic_fat, 4, 0, macho_fat_check,
    macho_fat_fields, (int)(sizeof(macho_fat_fields) / sizeof(macho_fat_fields[0]))
};

/* ---- 永続化された状態 (JSON) ------------------------------------ */
/* Zustand persist などが書く "{\"state\":{...}}"。magic が無いので
   「キー名の存在」を check にして弱い spec の誤検出を避ける。 */
static int mem_contains(const uint8_t *d, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (!m || n < m) return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(d + i, needle, m) == 0) return 1;
    return 0;
}

static int json_state_check(const uint8_t *d, size_t n) {
    size_t i = 0;
    while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) i++;
    if (i >= n || d[i] != '{') return 0;
    /* "state" と "tabs" が両方ある = タブ状態の永続 JSON */
    return mem_contains(d, n, "\"state\"") && mem_contains(d, n, "\"tabs\"");
}

static const nx_field_t json_state_fields[] = {
    { "prefix", NX_F_ABS, NX_D_ASCII, 0, 8, 0, 0, 1, NULL, 0 },
};
const nx_spec_t nx_spec_json_state = {
    "json_state", "persisted-state", "persisted workspace state ({\"state\":{\"tabs\":...}})",
    NULL, 0, 0, json_state_check,
    json_state_fields, (int)(sizeof(json_state_fields) / sizeof(json_state_fields[0]))
};

/* ---- Chromium LevelDB の localStorage レコード ------------------- */
/* キーは [_<origin>\x00\x01<localStorage key>] という共通の封筒に入る。
   つまり「どのアプリの、どの状態キーか」は封筒の後ろ側だけで決まり、
   パース手順は全キーで同一。 */
static int ls_record_check(const uint8_t *d, size_t n) {
    if (n < 4 || d[0] != '_') return 0;
    size_t lim = n < 512 ? n : 512;
    for (size_t i = 1; i + 1 < lim; i++) {
        if (d[i] == 0x00 && d[i + 1] == 0x01) {
            /* 封筒の後ろにキー本体がある */
            return i + 2 < n;
        }
    }
    return 0;
}

static const nx_field_t ls_record_fields[] = {
    { "origin", NX_F_ABS, NX_D_ASCII, 1, 24, 0, 0, 1, NULL, 0 },
};
const nx_spec_t nx_spec_ls_record = {
    "leveldb_ls_record", "persisted-state", "Chromium localStorage record (_origin\\0\\1key)",
    NULL, 0, 0, ls_record_check,
    ls_record_fields, (int)(sizeof(ls_record_fields) / sizeof(ls_record_fields[0]))
};

void nx_specs_builtin_register(void) {
    nx_spec_register(&nx_spec_ws_frame);
    nx_spec_register(&nx_spec_lenprefixed_be);
    nx_spec_register(&nx_spec_http_status);
    nx_spec_register(&nx_spec_pe_dos_header);
    nx_spec_register(&nx_spec_pe_nt_signature);
    nx_spec_register(&nx_spec_pe_file_header);
    nx_spec_register(&nx_spec_pe_section_header);
    nx_spec_register(&nx_spec_macho64);
    nx_spec_register(&nx_spec_macho32);
    nx_spec_register(&nx_spec_macho_fat);
    nx_spec_register(&nx_spec_json_state);
    nx_spec_register(&nx_spec_ls_record);
}

/* ---- PE (Windows .exe/.dll) 構造 -------------------------------- */
static const nx_field_t pe_dos_fields[] = {
    { "e_magic",  NX_F_ABS, NX_D_U16LE, 0,  2, 0, 0, 1, NULL, 0 },
    { "e_lfanew", NX_F_ABS, NX_D_U32LE, 0x3c, 4, 0, 0, 1, NULL, 0 },
};
static const uint8_t pe_magic_mz[2] = { 'M', 'Z' };

/* 本物の PE かを厳密検証: MZ で、e_lfanew が範囲内で PE\0\0 を指す。 */
static int pe_dos_check(const uint8_t *d, size_t n) {
    if (n < 0x40) return 0;
    if (d[0] != 'M' || d[1] != 'Z') return 0;
    uint32_t e = (uint32_t)d[0x3c] | ((uint32_t)d[0x3d] << 8) |
                 ((uint32_t)d[0x3e] << 16) | ((uint32_t)d[0x3f] << 24);
    if ((size_t)e + 24 > n) return 0;
    if (memcmp(d + e, "PE\0\0", 4) != 0) return 0;
    /* OptionalHeader magic の妥当性も確認 (PE32/PE32+) */
    uint16_t om = (uint16_t)(d[e + 24] | (d[e + 25] << 8));
    if (om != 0x010B && om != 0x020B) return 0;
    return 1;
}

const nx_spec_t nx_spec_pe_dos_header = {
    "pe_dos_header", "pe", "PE DOS header (MZ + e_lfanew)",
    pe_magic_mz, 2, 0, pe_dos_check,
    pe_dos_fields, (int)(sizeof(pe_dos_fields) / sizeof(pe_dos_fields[0]))
};

static const nx_field_t pe_file_fields[] = {
    { "machine",          NX_F_ABS, NX_D_U16LE, 0, 2, 0, 0, 1, NULL, 0 },
    { "number_of_sections", NX_F_ABS, NX_D_U16LE, 2, 2, 0, 0, 1, NULL, 0 },
    { "time_date_stamp",  NX_F_ABS, NX_D_U32LE, 4, 4, 0, 0, 1, NULL, 0 },
};
static const uint8_t pe_magic_nt[4] = { 'P', 'E', 0, 0 };
const nx_spec_t nx_spec_pe_nt_signature = {
    "pe_nt_signature", "pe", "PE NT signature (PE\\0\\0)",
    pe_magic_nt, 4, 0, NULL, NULL, 0
};
const nx_spec_t nx_spec_pe_file_header = {
    "pe_file_header", "pe", "PE COFF file header",
    NULL, 0, 0, NULL,
    pe_file_fields, (int)(sizeof(pe_file_fields) / sizeof(pe_file_fields[0]))
};

static const nx_field_t pe_sec_fields[] = {
    { "name",            NX_F_ABS, NX_D_ASCII, 0,  8, 0, 0, 1, NULL, 0 },
    { "virtual_size",    NX_F_ABS, NX_D_U32LE, 8,  4, 0, 0, 1, NULL, 0 },
    { "virtual_address", NX_F_ABS, NX_D_U32LE, 12, 4, 0, 0, 1, NULL, 0 },
    { "size_of_raw_data",NX_F_ABS, NX_D_U32LE, 16, 4, 0, 0, 1, NULL, 0 },
    { "pointer_to_raw",  NX_F_ABS, NX_D_U32LE, 20, 4, 0, 0, 1, NULL, 0 },
};
const nx_spec_t nx_spec_pe_section_header = {
    "pe_section_header", "pe", "PE section table entry (40 bytes)",
    NULL, 0, 0, NULL,
    pe_sec_fields, (int)(sizeof(pe_sec_fields) / sizeof(pe_sec_fields[0]))
};
