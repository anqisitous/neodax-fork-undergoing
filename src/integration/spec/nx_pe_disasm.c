#include "nx_pe_disasm.h"
#include "nx_pe.h"
#include "../nx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t addr;
    uint8_t  len;
    char     mnem[32];
    char     ops[128];
    int      is_call;
    int      is_branch;
    int      is_ret;
    int      has_target;
    uint64_t target;
} di_t;

/* PE machine -> nx_arch */
static nx_arch_t arch_of(uint16_t machine) {
    switch (machine) {
        case 0x8664: return NX_ARCH_X86_64;
        case 0x014c: return NX_ARCH_X86_64;   /* i386: x86 デコーダで近似 */
        case 0xaa64: return NX_ARCH_ARM64;
        default:     return NX_ARCH_UNKNOWN;
    }
}

static uint8_t *read_file(const char *path, size_t *sz) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(fp); return NULL; }
    size_t r = fread(b, 1, (size_t)n, fp);
    fclose(fp);
    *sz = r;
    return b;
}

int nx_pe_disasm(const char *path, int limit, FILE *out) {
    if (!out) out = stdout;
    if (limit <= 0) limit = 4000;

    nx_pe_image_t *img = nx_pe_open(path);
    if (!img) { fprintf(out, "  [disasm] not a PE: %s\n", path); return -1; }

    nx_arch_t arch = arch_of(img->machine);
    if (arch == NX_ARCH_UNKNOWN) {
        fprintf(out, "  [disasm] unsupported machine 0x%04x\n", img->machine);
        nx_pe_free(img);
        return -1;
    }

    /* 実行可能セクションを探す (優先 .text) */
    int pick = -1;
    for (int i = 0; i < img->n_sections; i++)
        if (strcmp(img->sections[i].name, ".text") == 0) { pick = i; break; }
    if (pick < 0)
        for (int i = 0; i < img->n_sections; i++)
            if (img->sections[i].characteristics & 0x20000000) { pick = i; break; }
    if (pick < 0) {
        fprintf(out, "  [disasm] no executable section\n");
        nx_pe_free(img);
        return -1;
    }
    const nx_pe_section_t *sec = &img->sections[pick];

    size_t fsz = 0;
    uint8_t *data = read_file(path, &fsz);
    if (!data) { nx_pe_free(img); return -1; }
    if (sec->raw_ptr + sec->raw_size > fsz) {
        fprintf(out, "  [disasm] section out of file\n");
        free(data);
        nx_pe_free(img);
        return -1;
    }

    nx_decode_register_all();

    const uint8_t *code = data + sec->raw_ptr;
    size_t code_len = sec->raw_size;
    uint64_t base = img->image_base + sec->virtual_address;

    fprintf(out, "\n  === PE disassembly: %s ===\n", path);
    fprintf(out, "  arch=%s section=%s vaddr=0x%x size=%zu entry=0x%x\n",
            arch == NX_ARCH_ARM64 ? "arm64" : "x86",
            sec->name, sec->virtual_address, code_len, img->entry_point);

    di_t *ins = (di_t *)calloc((size_t)limit, sizeof(di_t));
    if (!ins) { free(data); nx_pe_free(img); return -1; }
    int n = 0;
    size_t off = 0;
    while (off < code_len && n < limit) {
        nx_insn_t ni;
        memset(&ni, 0, sizeof(ni));
        int rc = nx_decode(arch, base + off, code + off, code_len - off, &ni);
        if (rc != NX_OK || ni.length == 0) { off += 1; continue; }
        ins[n].addr = base + off;
        ins[n].len = ni.length;
        snprintf(ins[n].mnem, sizeof(ins[n].mnem), "%s", ni.mnemonic);
        snprintf(ins[n].ops, sizeof(ins[n].ops), "%s", ni.operands);
        ins[n].is_call = ni.is_call;
        ins[n].is_branch = ni.is_branch;
        ins[n].is_ret = ni.is_ret;
        ins[n].has_target = ni.has_target;
        ins[n].target = ni.target;
        n++;
        off += ni.length;
    }

    /* 関数開始: entry, 直接 call 先, ret/無条件 jmp の次 */
    unsigned char *is_func = (unsigned char *)calloc((size_t)n, 1);
    int nfunc = 0;
    for (int i = 0; i < n; i++) {
        if (ins[i].is_call && ins[i].has_target) {
            for (int k = 0; k < n; k++)
                if (ins[k].addr == ins[i].target && !is_func[k]) { is_func[k] = 1; nfunc++; break; }
        }
        if ((ins[i].is_ret || (ins[i].is_branch && !ins[i].is_call)) && i + 1 < n && !is_func[i + 1]) {
            is_func[i + 1] = 1; nfunc++;
        }
    }
    /* entry を関数先頭に */
    for (int k = 0; k < n; k++)
        if (ins[k].addr == img->image_base + img->entry_point && !is_func[k]) { is_func[k] = 1; nfunc++; }
    /* 先頭も */
    if (n > 0 && !is_func[0]) { is_func[0] = 1; nfunc++; }

    /* 出力: 関数ごと */
    fprintf(out, "  decoded %d instructions, %d functions\n\n", n, nfunc);
    int printed = 0;
    for (int i = 0; i < n && printed < 400; i++) {
        if (is_func[i]) {
            if (i) fprintf(out, "\n");
            fprintf(out, "  func 0x%llx:\n", (unsigned long long)ins[i].addr);
        }
        fprintf(out, "    %08llx  %-8s %s%s\n",
                (unsigned long long)ins[i].addr, ins[i].mnem, ins[i].ops,
                ins[i].is_call ? "   ; call" : ins[i].is_ret ? "   ; ret" : "");
        printed++;
    }

    free(is_func);
    free(ins);
    free(data);
    nx_pe_free(img);
    return 0;
}
