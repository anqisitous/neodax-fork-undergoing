#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dax.h"
#include "macho.h"

/*
 * Canonical Mach-O parser (32-bit i386 + 64-bit x86_64/arm64).
 *
 * One code path parameterized by `is64`, wincomp-style: field widths and
 * record strides are compressed into small helpers, and the on-disk struct
 * layouts are locked by the size checks in include/macho.h. Consumers (the
 * static-analysis layer) compose this via dax_parse_macho(); nothing else
 * parses Mach-O.
 */

static uint32_t bswap32(uint32_t v){
    return ((v&0xFF)<<24)|((v&0xFF00)<<8)|((v&0xFF0000)>>8)|(v>>24);
}
static uint64_t bswap64(uint64_t v){
    return ((uint64_t)bswap32((uint32_t)(v&0xFFFFFFFF))<<32)|
            (uint64_t)bswap32((uint32_t)(v>>32));
}

/* Packed/un-aligned-safe scalar reads. */
static uint32_t rd32(const uint8_t *p, int swap){
    uint32_t v; memcpy(&v, p, 4); return swap ? bswap32(v) : v;
}
static uint64_t rd64(const uint8_t *p, int swap){
    uint64_t v; memcpy(&v, p, 8); return swap ? bswap64(v) : v;
}
static uint64_t rd_word(const uint8_t *p, int swap, int is64){
    return is64 ? rd64(p, swap) : (uint64_t)rd32(p, swap);
}

static int fat_find_slice(const uint8_t *data, size_t sz,
                           uint32_t want_cpu,
                           uint32_t *out_off, uint32_t *out_size)
{
    if (sz < sizeof(macho_fat_header_t)) return -1;
    const macho_fat_header_t *fh = (const macho_fat_header_t *)data;
    uint32_t narch = bswap32(fh->nfat_arch);
    if (narch > 64) return -1;

    size_t base = sizeof(macho_fat_header_t);
    for (uint32_t i = 0; i < narch; i++) {
        if (base + sizeof(macho_fat_arch_t) > sz) break;
        const macho_fat_arch_t *fa = (const macho_fat_arch_t *)(data + base);
        uint32_t cpu  = bswap32(fa->cputype);
        uint32_t off  = bswap32(fa->offset);
        uint32_t fsz  = bswap32(fa->size);
        if (cpu == want_cpu && off + fsz <= sz) {
            *out_off  = off;
            *out_size = fsz;
            return 0;
        }
        base += sizeof(macho_fat_arch_t);
    }

    base = sizeof(macho_fat_header_t);
    for (uint32_t i = 0; i < narch; i++) {
        if (base + sizeof(macho_fat_arch_t) > sz) break;
        const macho_fat_arch_t *fa = (const macho_fat_arch_t *)(data + base);
        uint32_t off = bswap32(fa->offset);
        uint32_t fsz = bswap32(fa->size);
        if (off + fsz <= sz) { *out_off = off; *out_size = fsz; return 0; }
        base += sizeof(macho_fat_arch_t);
    }
    return -1;
}

int dax_parse_macho(dax_binary_t *bin)
{
    const uint8_t *data = bin->data;
    size_t         sz   = bin->size;
    int            swap = 0;

    if (!data || sz < 4) return -1;

    uint32_t fat_magic = *(const uint32_t *)data;
    if (fat_magic == MACHO_FAT_MAGIC || fat_magic == MACHO_FAT_MAGIC_LE) {
        uint32_t off = 0, slice_sz = 0;
        if (fat_find_slice(data, sz, MACHO_CPU_ARM64,  &off, &slice_sz) != 0 &&
            fat_find_slice(data, sz, MACHO_CPU_X86_64, &off, &slice_sz) != 0 &&
            fat_find_slice(data, sz, MACHO_CPU_X86,    &off, &slice_sz) != 0)
            return -1;
        if (off == 0 || off + slice_sz > sz) return -1;
        data = bin->data + off;
        sz   = slice_sz;
    }

    if (sz < sizeof(macho_header32_t)) {
        fprintf(stderr, "neodax: Mach-O too small\n");
        return -1;
    }

    uint32_t magic = *(const uint32_t *)data;
    int is64 = 0;

    if      (magic == MACHO_MAGIC_64_LE) { is64 = 1; swap = 0; }
    else if (magic == MACHO_MAGIC_64)    { is64 = 1; swap = 1; }
    else if (magic == MACHO_MAGIC_32_LE) { is64 = 0; swap = 0; }
    else if (magic == MACHO_MAGIC_32)    { is64 = 0; swap = 1; }
    else {
        fprintf(stderr, "neodax: unrecognised Mach-O magic 0x%08X\n", magic);
        return -1;
    }

#define LE32(v) (swap ? bswap32(v) : (v))
#define LE64(v) (swap ? bswap64(v) : (v))

    const uint8_t *hdr     = data;
    uint32_t cputype       = rd32(hdr + 4,  swap);
    uint32_t cpusubtype    = rd32(hdr + 8,  swap);
    uint32_t filetype      = rd32(hdr + 12, swap);
    uint32_t ncmds         = rd32(hdr + 16, swap);
    uint32_t flags         = rd32(hdr + 24, swap);
    size_t   hdr_size      = is64 ? sizeof(macho_header64_t)
                                  : sizeof(macho_header32_t);
    size_t   seg_hdr       = is64 ? sizeof(macho_segment64_t)
                                  : sizeof(macho_segment32_t);
    size_t   sec_hdr       = is64 ? sizeof(macho_section64_t)
                                  : sizeof(macho_section32_t);
    size_t   nlist_sz      = is64 ? sizeof(macho_nlist64_t)
                                  : sizeof(macho_nlist32_t);

    switch (cputype) {
        case MACHO_CPU_ARM64:
        case MACHO_CPU_ARM64_32: bin->arch = ARCH_ARM64;  break;
        case MACHO_CPU_X86_64:   bin->arch = ARCH_X86_64; break;
        case MACHO_CPU_X86:      bin->arch = ARCH_X86;    break;
        default:                 bin->arch = ARCH_UNKNOWN; break;
    }

    bin->fmt        = FMT_MACHO;
    bin->os         = DAX_PLAT_BSD;
    bin->filetype   = filetype;
    bin->cputype    = cputype;
    bin->cpusubtype = cpusubtype;
    bin->flags      = flags;

    /* PIE is MH_EXECUTE + MH_PIE, not "is a dylib". */
    bin->is_pie = (filetype == MACHO_MH_EXECUTE) && (flags & MACHO_MH_PIE);

    size_t   cmd_off        = hdr_size;
    uint64_t text_vmbase    = 0;
    uint64_t text_sect_addr = 0;
    uint64_t entry_off      = 0;
    int      got_entry      = 0;
    uint64_t total_code     = 0;
    uint64_t total_data     = 0;
    uint64_t image_end      = 0;

    /* symtab/dysymtab for indirect (stub) symbol resolution */
    uint32_t g_symoff = 0, g_nsyms = 0, g_stroff = 0, g_strsz = 0;
    int      g_have_symtab = 0;
    uint32_t g_indirectoff = 0, g_nindirect = 0;
    int      g_have_dysym = 0;

    /* S_SYMBOL_STUBS sections (__stubs): resolved after the load-command
     * loop via the indirect symbol table so pthread_create / dispatch_async
     * etc. get real names at their stub addresses. */
    struct { uint64_t addr; uint64_t size; uint32_t r1; uint32_t r2; } stubsecs[16];
    int nstubsecs = 0;

    for (uint32_t ci = 0; ci < ncmds; ci++) {
        if (cmd_off + sizeof(macho_load_cmd_t) > sz) break;
        const macho_load_cmd_t *lc = (const macho_load_cmd_t *)(data + cmd_off);
        uint32_t cmd     = LE32(lc->cmd);
        uint32_t cmdsize = LE32(lc->cmdsize);
        if (cmdsize < 8 || cmd_off + cmdsize > sz) break;

        const uint8_t *cmd_base = data + cmd_off;

        /* ── Segment (32 or 64): sections live here ─────────────────── */
        if ((cmd == MACHO_LC_SEGMENT_64 && is64) ||
            (cmd == MACHO_LC_SEGMENT    && !is64)) {
            const uint8_t *segp    = cmd_base;
            const char    *segname = (const char *)(segp + 8);
            uint64_t vm  = rd_word(segp + 24, swap, is64);
            uint64_t vsz = is64 ? rd64(segp + 32, swap) : rd32(segp + 28, swap);
            uint32_t ip  = is64 ? rd32(segp + 60, swap) : rd32(segp + 44, swap);
            uint32_t ns  = is64 ? rd32(segp + 64, swap) : rd32(segp + 48, swap);

            if (strncmp(segname, "__TEXT", 6) == 0)
                text_vmbase = vm;
            if (vm + vsz > image_end) image_end = vm + vsz;

            size_t sec_off = cmd_off + seg_hdr;
            for (uint32_t si = 0; si < ns && si < (uint32_t)DAX_MAX_SECTIONS; si++) {
                if (sec_off + sec_hdr > sz) break;
                if (bin->nsections >= DAX_MAX_SECTIONS) break;

                const uint8_t *sp = data + sec_off;
                char sec_s[17] = {0};
                strncpy(sec_s, (const char *)sp, 16);

                dax_section_t *ds = &bin->sections[bin->nsections++];
                char sname[64];
                if (sec_s[0] == '_' && sec_s[1] == '_')
                    snprintf(sname, sizeof(sname), ".%s", sec_s + 2);
                else
                    snprintf(sname, sizeof(sname), "%s", sec_s);
                strncpy(ds->name, sname, sizeof(ds->name) - 1);

                ds->vaddr      = rd_word(sp + 32, swap, is64);
                ds->size       = is64 ? rd64(sp + 40, swap) : rd32(sp + 36, swap);
                ds->offset     = is64 ? rd32(sp + 48, swap) : rd32(sp + 40, swap);
                ds->flags      = is64 ? rd32(sp + 64, swap) : rd32(sp + 56, swap);
                ds->executable = (ip & 4) != 0;
                ds->writable   = (ip & 2) != 0;
                ds->readable   = (ip & 1) != 0;
                ds->file_offset = ds->offset;

                if (strncmp(sec_s, "__text", 6) == 0 && text_sect_addr == 0)
                    text_sect_addr = ds->vaddr;

                /* record symbol-stub sections for the post-pass */
                if ((ds->flags & 0xFF) == MACHO_S_SYMBOL_STUBS && nstubsecs < 16) {
                    uint32_t r1 = is64 ? rd32(sp + 68, swap) : rd32(sp + 60, swap);
                    uint32_t r2 = is64 ? rd32(sp + 72, swap) : rd32(sp + 64, swap);
                    if (r2 > 0) {
                        stubsecs[nstubsecs].addr = ds->vaddr;
                        stubsecs[nstubsecs].size = ds->size;
                        stubsecs[nstubsecs].r1   = r1;
                        stubsecs[nstubsecs].r2   = r2;
                        nstubsecs++;
                    }
                }

                uint32_t stype = ds->flags & 0xFF;
                int exec = (ip & 4) != 0;
                if (exec || stype == 0) {
                    if (strncmp(segname, "__TEXT", 6) == 0 && exec)
                        ds->type = SEC_TYPE_CODE;
                    else if (strstr(ds->name, "data") || strstr(ds->name, "bss"))
                        ds->type = SEC_TYPE_DATA;
                    else
                        ds->type = SEC_TYPE_OTHER;
                } else {
                    ds->type = SEC_TYPE_DATA;
                }
                if (strstr(ds->name, "rodata") || strstr(ds->name, "cstring") ||
                    strstr(ds->name, "const"))
                    ds->type = SEC_TYPE_RODATA;
                if (strstr(ds->name, "got") || strstr(ds->name, "la_symbol_ptr"))
                    ds->type = SEC_TYPE_GOT;
                if (strstr(ds->name, "stubs") || strstr(ds->name, "plt"))
                    ds->type = SEC_TYPE_PLT;
                if (stype == MACHO_S_ZEROFILL)
                    ds->type = SEC_TYPE_BSS;

                if (ds->type == SEC_TYPE_CODE) total_code += ds->size;
                else                           total_data += ds->size;

                sec_off += sec_hdr;
            }
        }

        /* ── Entry: LC_MAIN (both widths) ───────────────────────────── */
        else if (cmd == MACHO_LC_MAIN) {
            if (cmdsize >= sizeof(macho_entry_cmd_t)) {
                entry_off = rd64(cmd_base + 8, swap);
                got_entry = 1;
            }
        }

        /* ── Entry: LC_UNIXTHREAD (i386 eip / x86_64 rip / arm64 pc) ── */
        else if (cmd == MACHO_LC_UNIXTHREAD) {
            size_t state = cmd_off + 16;
            if (!is64) {
                if (bin->arch == ARCH_X86 && cmdsize >= 16 + (10 + 1) * 4)
                    bin->entry = rd32(data + state + 10 * 4, swap);   /* eip */
            } else if (bin->arch == ARCH_X86_64 && cmdsize >= 16 + (16 + 1) * 8) {
                bin->entry = rd64(data + state + 16 * 8, swap);        /* rip */
            } else if (bin->arch == ARCH_ARM64 && cmdsize >= 16 + (32 + 1) * 8) {
                bin->entry = rd64(data + state + 32 * 8, swap);        /* pc  */
            }
        }

        /* ── Symbols: LC_SYMTAB (both widths) ───────────────────────── */
        else if (cmd == MACHO_LC_SYMTAB) {
            uint32_t symoff = rd32(cmd_base + 8,  swap);
            uint32_t nsyms  = rd32(cmd_base + 12, swap);
            uint32_t stroff = rd32(cmd_base + 16, swap);
            uint32_t strsz  = rd32(cmd_base + 20, swap);
            size_t   slice_base = (size_t)(data - bin->data);
            g_symoff = symoff + (uint32_t)slice_base;
            g_nsyms  = nsyms;
            g_stroff = stroff + (uint32_t)slice_base;
            g_strsz  = strsz;
            g_have_symtab = 1;
        }

        /* ── LC_DYSYMTAB — indirect symbol table for __stubs ────────── */
        else if (cmd == MACHO_LC_DYSYMTAB) {
            size_t   slice_base = (size_t)(data - bin->data);
            uint32_t ioff = rd32(cmd_base + 56, swap) + (uint32_t)slice_base;
            uint32_t nind = rd32(cmd_base + 60, swap);
            if (nind > 0 &&
                (uint64_t)ioff + (uint64_t)nind * 4u <= bin->size) {
                g_indirectoff = ioff;
                g_nindirect   = nind;
                g_have_dysym  = 1;
            }
        }

        cmd_off += cmdsize;
    }

    if (got_entry && text_vmbase > 0)
        bin->entry = text_vmbase + entry_off;
    else if (bin->entry == 0)   /* LC_UNIXTHREAD may already have set it */
        bin->entry = text_sect_addr ? text_sect_addr : text_vmbase;

    /* ── Symbol table ──────────────────────────────────────────────────
     * Parsed after the load-command loop so the section table is complete:
     * Mach-O's n_sect is a 1-based index into the flat section list. */
    if (g_have_symtab && g_nsyms > 0 && g_nsyms < DAX_MAX_SYMBOLS &&
        g_symoff <= bin->size &&
        (uint64_t)g_nsyms * nlist_sz <= (uint64_t)bin->size - g_symoff &&
        g_stroff <= bin->size && g_strsz <= bin->size - g_stroff) {

        bin->is_stripped = 0;
        uint32_t count = 0;
        bin->symbols = (dax_symbol_t *)calloc(g_nsyms + 1, sizeof(dax_symbol_t));
        if (bin->symbols) {
            for (uint32_t si = 0; si < g_nsyms && count < DAX_MAX_SYMBOLS; si++) {
                const uint8_t *rec = bin->data + g_symoff + (size_t)si * nlist_sz;
                uint32_t strx  = rd32(rec, swap);
                uint8_t  ntype = rec[4];
                uint8_t  nsect = rec[5];
                uint64_t nval  = rd_word(rec + 8, swap, is64);

                if ((ntype & 0xE0) != 0) continue;   /* STAB debug symbol */
                if (nval == 0)           continue;
                if (strx >= g_strsz)     continue;

                const char *name = (const char *)(bin->data + g_stroff + strx);
                if (name[0] == '_') name++;
                if (!name[0]) continue;

                dax_symbol_t *sym = &bin->symbols[count++];
                memset(sym, 0, sizeof(*sym));
                strncpy(sym->name, name, sizeof(sym->name) - 1);
                sym->name[sizeof(sym->name) - 1] = '\0';
                sym->address = nval;

                if ((ntype & 0x0E) == 0x0E && nsect >= 1 &&
                    (int)nsect <= bin->nsections &&
                    bin->sections[nsect - 1].type == SEC_TYPE_CODE)
                    sym->type = SYM_FUNC;
                else
                    sym->type = SYM_OBJECT;
            }
            bin->nsymbols = (int)count;
        } else {
            bin->is_stripped = 1;
        }
    } else if (g_have_symtab) {
        bin->is_stripped = 1;
    }

    /* ── Stub symbol resolution ───────────────────────────────────────
     * For every __stubs entry (S_SYMBOL_STUBS), map the indirect symbol
     * table entry to a symtab name and register SYM_IMPORT at the stub
     * address. Makes pthread_create / dispatch_async / pthread_join etc.
     * resolvable on real Mach-O binaries. */
    if (g_have_dysym && g_have_symtab && nstubsecs > 0) {
        for (int ssi = 0; ssi < nstubsecs; ssi++) {
            uint32_t stub_sz = stubsecs[ssi].r2;
            uint32_t nstub = stub_sz ? (uint32_t)(stubsecs[ssi].size / stub_sz) : 0;
            if (stubsecs[ssi].r1 >= g_nindirect) continue;
            for (uint32_t k = 0; k < nstub; k++) {
                uint32_t ind_idx = stubsecs[ssi].r1 + k;
                uint32_t raw_isym, isym, strx;
                const uint8_t *rec;
                const char *name;
                dax_symbol_t *grown, *ns;

                if (ind_idx >= g_nindirect) break;
                if (bin->nsymbols >= DAX_MAX_SYMBOLS) break;
                raw_isym = rd32(bin->data + g_indirectoff + (size_t)ind_idx * 4, swap);
                if (raw_isym & 0x80000000u) continue;  /* INDIRECT_SYMBOL_LOCAL */
                if (raw_isym & 0x40000000u) continue;  /* INDIRECT_SYMBOL_ABS   */
                isym = raw_isym & 0x00FFFFFFu;
                if (isym >= g_nsyms) continue;

                rec  = bin->data + g_symoff + (size_t)isym * nlist_sz;
                strx = rd32(rec, swap);
                if (strx >= g_strsz) continue;
                name = (const char *)(bin->data + g_stroff + strx);
                if (name[0] == '_') name++;
                if (!name[0]) continue;

                grown = (dax_symbol_t *)realloc(bin->symbols,
                        (size_t)(bin->nsymbols + 1) * sizeof(dax_symbol_t));
                if (!grown) break;
                bin->symbols = grown;
                ns = &bin->symbols[bin->nsymbols++];
                memset(ns, 0, sizeof(*ns));
                strncpy(ns->name, name, sizeof(ns->name) - 1);
                ns->address = stubsecs[ssi].addr + (uint64_t)k * stub_sz;
                ns->type    = SYM_IMPORT;
            }
        }
        if (bin->nsymbols > 0) bin->is_stripped = 0;
    }

    bin->base = text_vmbase;
    if (image_end > 0) {
        /* Store relative size (image_end - base), consistent with ELF parser */
        bin->image_size = (text_vmbase > 0 && image_end > text_vmbase)
                          ? (image_end - text_vmbase)
                          : image_end;
    } else {
        bin->image_size = (uint64_t)sz;
    }
    bin->code_size = total_code;
    bin->data_size = total_data;

    if (!bin->symbols) bin->is_stripped = 1;

    return 0;

#undef LE32
#undef LE64
}
