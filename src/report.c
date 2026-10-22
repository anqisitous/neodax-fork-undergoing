/*
 * report.c — NeoDAX banner / section reporting and shared fault register.
 *
 * These helpers were part of the original CLI entry point. During the static
 * analysis framework integration the CLI was restructured, so the shared
 * reporting surface moved here: it is linked into every build (native CLI and
 * the JS addon) and is used by the interactive shell and the .daxc converter.
 */

#include <stdio.h>
#include <string.h>

#include "dax.h"

/* ── Fault register ────────────────────────────────────────────────────────
 * Set by any module on a structural (but recoverable) error. The
 * DAX_RUN_PASS macro in dax_guard.h clears it before each pass and prints a
 * recovery notice when a pass trips it. */
volatile int g_dax_fault = 0;
char         g_dax_fault_msg[256] = {0};

/* ── Enum → string ──────────────────────────────────────────────────────── */

const char *dax_arch_str(dax_arch_t a) {
    switch (a) {
        case ARCH_X86_64:  return "x86_64";
        case ARCH_ARM64:   return "ARM64";
        case ARCH_RISCV64: return "RISCV64";
        default:           return "unknown";
    }
}

const char *dax_fmt_str(dax_fmt_t f) {
    switch (f) {
        case FMT_ELF32: return "ELF32";
        case FMT_ELF64: return "ELF64";
        case FMT_PE32:  return "PE32";
        case FMT_PE64:  return "PE64";
        case FMT_MACHO: return "Mach-O";
        case FMT_RAW:   return "RAW";
        default:        return "unknown";
    }
}

const char *dax_os_str(dax_os_t o) {
    switch (o) {
        case DAX_PLAT_LINUX:   return "Linux";
        case DAX_PLAT_ANDROID: return "Android";
        case DAX_PLAT_BSD:     return "BSD";
        case DAX_PLAT_UNIX:    return "UNIX";
        case DAX_PLAT_WINDOWS: return "Windows";
        default:               return "unknown";
    }
}

static const char *sec_type_str(dax_sec_type_t t) {
    switch (t) {
        case SEC_TYPE_CODE:   return "CODE";
        case SEC_TYPE_DATA:   return "DATA";
        case SEC_TYPE_RODATA: return "RODATA";
        case SEC_TYPE_BSS:    return "BSS";
        case SEC_TYPE_PLT:    return "PLT";
        case SEC_TYPE_GOT:    return "GOT";
        case SEC_TYPE_DYNAMIC:return "DYNAMIC";
        case SEC_TYPE_DEBUG:  return "DEBUG";
        default:              return "OTHER";
    }
}

/* ── Banner ─────────────────────────────────────────────────────────────── */

void dax_print_banner(dax_binary_t *bin, dax_opts_t *opts) {
    if (!bin) return;

    int c = opts ? opts->color : 0;

    printf("\n");
    printf("  %sFile%s          : %s\n", COL_LABEL, COL_RESET, bin->filepath);
    printf("  %sFormat%s        : %s\n", COL_LABEL, COL_RESET, dax_fmt_str(bin->fmt));
    printf("  %sArch%s          : %s\n", COL_LABEL, COL_RESET, dax_arch_str(bin->arch));
    printf("  %sOS/ABI%s        : %s\n", COL_LABEL, COL_RESET, dax_os_str(bin->os));
    printf("  %sEntry%s         : 0x%016llx\n", COL_LABEL, COL_RESET,
           (unsigned long long)bin->entry);
    printf("  %sImage Size%s    : %llu bytes\n", COL_LABEL, COL_RESET,
           (unsigned long long)bin->image_size);
    printf("  %sCode Size%s     : %llu bytes\n", COL_LABEL, COL_RESET,
           (unsigned long long)bin->code_size);
    printf("  %sData Size%s     : %llu bytes\n", COL_LABEL, COL_RESET,
           (unsigned long long)bin->data_size);
    printf("  %sSymbols%s       : %d\n", COL_LABEL, COL_RESET, bin->nsymbols);
    printf("  %sFunctions%s     : %d\n", COL_LABEL, COL_RESET, bin->nfunctions);
    printf("  %sCFG Blocks%s    : %d\n", COL_LABEL, COL_RESET, bin->nblocks);
    printf("  %sXrefs%s         : %d\n", COL_LABEL, COL_RESET, bin->nxrefs);

    if (bin->sha256[0]) {
        printf("  %sSHA-256%s       : %.16s...\n", COL_LABEL, COL_RESET, bin->sha256);
    }
    if (bin->build_id[0]) {
        printf("  %sBuild-ID%s      : %s\n", COL_LABEL, COL_RESET, bin->build_id);
    }

    printf("  %sPIE%s           : %s\n", COL_LABEL, COL_RESET, bin->is_pie ? "yes" : "no");
    printf("  %sStripped%s      : %s\n", COL_LABEL, COL_RESET, bin->is_stripped ? "yes" : "no");
    printf("  %sDebug Info%s    : %s\n", COL_LABEL, COL_RESET, bin->has_debug ? "yes" : "no");

    if (c) printf("%s", COL_RESET);
    printf("\n");
}

/* ── Section table ──────────────────────────────────────────────────────── */

void dax_print_sections(dax_binary_t *bin, dax_opts_t *opts) {
    if (!bin) return;

    int c = opts ? opts->color : 0;

    printf("\n");
    printf("  %s%-20s %-18s %-10s %s%s\n", COL_LABEL, "Section", "VAddr", "Size", "Type",
           COL_RESET);
    printf("  %s-------------------- ------------------ ---------- ----%s\n",
           COL_LABEL, COL_RESET);

    for (int i = 0; i < bin->nsections; i++) {
        dax_section_t *s = &bin->sections[i];
        printf("  %-20s 0x%016llx %-10llu %s\n",
               s->name,
               (unsigned long long)s->vaddr,
               (unsigned long long)s->size,
               sec_type_str(dax_sec_classify(s->name, s->flags)));
    }

    printf("\n  %d section%s\n", bin->nsections, bin->nsections == 1 ? "" : "s");
    if (c) printf("%s", COL_RESET);
}
