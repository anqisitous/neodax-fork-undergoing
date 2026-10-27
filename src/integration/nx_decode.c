/*
 * nx_decode.c — 命令デコードの多態実装。
 *
 * 「同じ操作に実装が 1 つなら merge、2 つ以上なら多態」のポリシーに従い、
 * arch ごとのデコーダを nx_register_decoder で登録する。
 * 呼び出し側 (core/macho_static.c) は nx_decode(arch, ...) を呼ぶだけで、
 * どの実装を使うかはプログラム (nx_select_decoder) が実行時に決める。
 *
 * arm64  : 旧 neodax エンジンの a64_decode (src/arm64_decode.c)
 * x86_64 : 旧 neodax エンジンの x86_decode (src/x86_decode.c)
 */

#include "nx.h"

#include <stdio.h>
#include <string.h>

#include "arm64.h"
#include "x86.h"

/* ------------------------------------------------------------------ */
/* ARM64                                                               */
/* ------------------------------------------------------------------ */

static int dec_arm64(uint64_t addr, const uint8_t *bytes, size_t len, nx_insn_t *out) {
    if (len < 4) return NX_ERR;
    uint32_t raw = (uint32_t)bytes[0]
                 | ((uint32_t)bytes[1] << 8)
                 | ((uint32_t)bytes[2] << 16)
                 | ((uint32_t)bytes[3] << 24);

    a64_insn_t in;
    a64_decode(raw, addr, &in);

    memset(out, 0, sizeof(*out));
    out->address = addr;
    out->length = 4;
    strncpy(out->mnemonic, in.mnemonic, sizeof(out->mnemonic) - 1);
    strncpy(out->operands, in.operands, sizeof(out->operands) - 1);

    const char *m = in.mnemonic;
    if (!strcmp(m, "bl"))            { out->is_call = 1; out->is_branch = 1; }
    else if (!strcmp(m, "blr"))      { out->is_call = 1; out->is_branch = 1; out->is_indirect = 1; }
    else if (!strcmp(m, "br"))       { out->is_branch = 1; out->is_indirect = 1; }
    else if (!strcmp(m, "b") || !strncmp(m, "b.", 2) ||
             !strcmp(m, "cbz") || !strcmp(m, "cbnz") ||
             !strcmp(m, "tbz") || !strcmp(m, "tbnz")) { out->is_branch = 1; }
    else if (!strcmp(m, "ret"))      { out->is_ret = 1; }

    out->is_cond = (m[0] == 'b' && m[1] == '.') ||
                   !strcmp(m, "cbz") || !strcmp(m, "cbnz") ||
                   !strcmp(m, "tbz") || !strcmp(m, "tbnz");

    if (out->is_branch && !out->is_indirect) {
        const char *p = strstr(in.operands, "0x");
        if (p && sscanf(p, "0x%llx", (unsigned long long *)&out->target) == 1)
            out->has_target = 1;
    }
    return NX_OK;
}

/* ------------------------------------------------------------------ */
/* x86_64                                                              */
/* ------------------------------------------------------------------ */

static int dec_x86_64(uint64_t addr, const uint8_t *bytes, size_t len, nx_insn_t *out) {
    x86_insn_t in;
    int n = x86_decode(bytes, len, addr, &in);
    if (n <= 0) return NX_ERR;

    memset(out, 0, sizeof(*out));
    out->address = addr;
    out->length = in.length ? in.length : (uint8_t)n;
    strncpy(out->mnemonic, in.mnemonic, sizeof(out->mnemonic) - 1);
    strncpy(out->operands, in.ops, sizeof(out->operands) - 1);

    const char *m = in.mnemonic;
    if (!strcmp(m, "call") || !strcmp(m, "callq")) out->is_call = 1;
    else if (!strcmp(m, "jmp") || !strcmp(m, "jmpq")) out->is_branch = 1;
    else if (m[0] == 'j' && strcmp(m, "jmp")) { out->is_branch = 1; out->is_cond = 1; }
    else if (!strncmp(m, "ret", 3)) out->is_ret = 1;

    if (out->is_call || out->is_branch) {
        const char *p = strstr(in.ops, "0x");
        if (p && sscanf(p, "0x%llx", (unsigned long long *)&out->target) == 1)
            out->has_target = 1;
    }
    return NX_OK;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

static const nx_decoder_t g_dec_arm64 = { "arm64",  NX_ARCH_ARM64,  dec_arm64  };
static const nx_decoder_t g_dec_x86   = { "x86_64", NX_ARCH_X86_64, dec_x86_64 };

void nx_decode_register_all(void) {
    nx_register_decoder(&g_dec_arm64);
    nx_register_decoder(&g_dec_x86);
}
