#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dax.h"
#include "macho.h"

static uint32_t bswap32(uint32_t v){
    return ((v&0xFF)<<24)|((v&0xFF00)<<8)|((v&0xFF0000)>>8)|(v>>24);
}
static uint64_t bswap64(uint64_t v){
    return ((uint64_t)bswap32((uint32_t)(v&0xFFFFFFFF))<<32)|
            (uint64_t)bswap32((uint32_t)(v>>32));
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

    
    uint32_t fat_magic = *(const uint32_t *)data;
    if (fat_magic == MACHO_FAT_MAGIC || fat_magic == MACHO_FAT_MAGIC_LE) {
        
        uint32_t off = 0, slice_sz = 0;
        
        if (fat_find_slice(data, sz, MACHO_CPU_ARM64,  &off, &slice_sz) != 0)
            fat_find_slice(data, sz, MACHO_CPU_X86_64, &off, &slice_sz);
        if (off == 0 || off + slice_sz > sz) {
            return -1;
        }
        
        data = bin->data + off;
        sz   = slice_sz;
    }

    if (sz < sizeof(macho_header64_t)) {
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

    
    const macho_header64_t *hdr = (const macho_header64_t *)data;
    uint32_t cputype  = LE32(hdr->cputype);
    uint32_t filetype = LE32(hdr->filetype);
    uint32_t ncmds    = LE32(hdr->ncmds);
    uint32_t hdr_size = is64 ? sizeof(macho_header64_t) : sizeof(macho_header32_t);

    
    switch (cputype) {
        case MACHO_CPU_ARM64:
        case MACHO_CPU_ARM64_32: bin->arch = ARCH_ARM64;  break;
        case MACHO_CPU_X86_64:   bin->arch = ARCH_X86_64; break;
        default:                 bin->arch = ARCH_UNKNOWN; break;
    }

    bin->fmt  = FMT_MACHO;
    bin->os   = DAX_PLAT_BSD;

    
    bin->is_pie = (filetype == MACHO_MH_DYLIB) ? 1 : 0;

    
    size_t   cmd_off     = hdr_size;
    uint64_t text_vmbase = 0;
    uint64_t entry_off   = 0;
    int      got_entry   = 0;
    uint64_t total_code  = 0;
    uint64_t total_data  = 0;
    uint64_t image_end   = 0;

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

        
        if (cmd == MACHO_LC_SEGMENT_64 && is64) {
            const macho_segment64_t *seg = (const macho_segment64_t *)(data + cmd_off);
            uint64_t vm  = LE64(seg->vmaddr);
            uint64_t vsz = LE64(seg->vmsize);
            uint32_t ns  = LE32(seg->nsects);
            uint32_t ip  = LE32(seg->initprot);

            if (strncmp(seg->segname, "__TEXT", 6) == 0)
                text_vmbase = vm;

            if (vm + vsz > image_end) image_end = vm + vsz;

            
            size_t sec_off = cmd_off + sizeof(macho_segment64_t);
            for (uint32_t si = 0; si < ns && si < (uint32_t)DAX_MAX_SECTIONS; si++) {
                if (sec_off + sizeof(macho_section64_t) > sz) break;
                const macho_section64_t *sec =
                    (const macho_section64_t *)(data + sec_off);

                if (bin->nsections >= DAX_MAX_SECTIONS) break;

                dax_section_t *ds = &bin->sections[bin->nsections++];

                
                char sname[64];
                char seg_s[17] = {0};
                char sec_s[17] = {0};
                strncpy(seg_s, sec->segname,  16);
                strncpy(sec_s, sec->sectname, 16);
                snprintf(sname, sizeof(sname), "%s", sec_s);
                
                if (sname[0]=='_' && sname[1]=='_') {
                    snprintf(sname, sizeof(sname), ".%s", sec_s+2);
                }
                strncpy(ds->name, sname, sizeof(ds->name)-1);

                ds->vaddr  = LE64(sec->addr);
                ds->size   = LE64(sec->size);
                ds->offset = (uint64_t)LE32(sec->offset);
                ds->flags  = LE32(sec->flags);

                /* record symbol-stub sections for the post-pass */
                if ((ds->flags & 0xFF) == MACHO_S_SYMBOL_STUBS &&
                    nstubsecs < 16 && LE32(sec->reserved2) > 0) {
                    stubsecs[nstubsecs].addr  = LE64(sec->addr);
                    stubsecs[nstubsecs].size  = LE64(sec->size);
                    stubsecs[nstubsecs].r1    = LE32(sec->reserved1);
                    stubsecs[nstubsecs].r2    = LE32(sec->reserved2);
                    nstubsecs++;
                }

                
                uint32_t stype = ds->flags & 0xFF;
                int exec = (ip & 4) != 0;  
                if (exec || stype == 0 ) {
                    if (strncmp(seg->segname, "__TEXT", 6) == 0 && exec)
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
                /* Zerofill sections (__bss, __common) carry no file bytes —
                 * their offset legitimately points past EOF. */
                if (stype == MACHO_S_ZEROFILL)
                    ds->type = SEC_TYPE_BSS;

                
                if (ds->type == SEC_TYPE_CODE)  total_code += ds->size;
                else                             total_data += ds->size;

                sec_off += sizeof(macho_section64_t);
            }
        }

        
        else if (cmd == MACHO_LC_MAIN) {
            const macho_entry_cmd_t *ec = (const macho_entry_cmd_t *)(data + cmd_off);
            entry_off  = LE64(ec->entryoff);
            got_entry  = 1;
        }

        
        else if (cmd == MACHO_LC_SYMTAB && is64) {
            const macho_symtab_cmd_t *st = (const macho_symtab_cmd_t *)(data + cmd_off);
            uint32_t symoff  = LE32(st->symoff);
            uint32_t nsyms   = LE32(st->nsyms);
            uint32_t stroff  = LE32(st->stroff);
            uint32_t strsz   = LE32(st->strsize);

            size_t slice_base = (size_t)(data - bin->data);
            symoff += (uint32_t)slice_base;
            stroff += (uint32_t)slice_base;

            g_symoff = symoff; g_nsyms = nsyms;
            g_stroff = stroff; g_strsz = strsz;
            g_have_symtab = 1;
        }

        /* LC_DYSYMTAB — indirect symbol table, used to name __stubs entries */
        else if (cmd == MACHO_LC_DYSYMTAB && is64) {
            const macho_dysymtab_cmd_t *dy =
                (const macho_dysymtab_cmd_t *)(data + cmd_off);
            size_t slice_base = (size_t)(data - bin->data);
            uint32_t ioff = LE32(dy->indirectoff) + (uint32_t)slice_base;
            uint32_t nind  = LE32(dy->nindirectsyms);
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
    else if (text_vmbase > 0)
        bin->entry = text_vmbase;

    /* ── Symbol table ──────────────────────────────────────────────────
     * Parsed after the load-command loop so that the section table is
     * complete: Mach-O's n_sect is a 1-based index into the flat section
     * list, and only a complete list tells code symbols from data ones.
     * (n_type N_SECT covers both functions and global objects — the
     * section they live in is the only discriminator.) */
    if (g_have_symtab && g_nsyms > 0 && g_nsyms < DAX_MAX_SYMBOLS &&
        g_symoff <= bin->size &&
        (uint64_t)g_nsyms * sizeof(macho_nlist64_t) <= (uint64_t)bin->size - g_symoff &&
        g_stroff <= bin->size && g_strsz <= bin->size - g_stroff) {

        bin->is_stripped = 0;
        uint32_t count = 0;
        bin->symbols = (dax_symbol_t *)calloc(g_nsyms + 1, sizeof(dax_symbol_t));
        if (bin->symbols) {
            for (uint32_t si = 0; si < g_nsyms && count < DAX_MAX_SYMBOLS; si++) {
                const macho_nlist64_t *nl = (const macho_nlist64_t *)
                    (bin->data + g_symoff + si * sizeof(macho_nlist64_t));
                uint32_t strx  = LE32(nl->n_strx);
                uint8_t  ntype = nl->n_type;
                uint8_t  nsect = nl->n_sect;
                uint64_t nval  = LE64(nl->n_value);

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

                /* Code section → function, anything else → data object. */
                if ((ntype & 0x0E) == 0x0E && nsect >= 1 &&
                    (int)nsect <= bin->nsections) {
                    dax_section_t *sec = &bin->sections[nsect - 1];
                    sym->type = (sec->type == SEC_TYPE_CODE) ? SYM_FUNC : SYM_OBJECT;
                } else {
                    sym->type = SYM_OBJECT;
                }
            }
            bin->nsymbols = (int)count;
        } else {
            bin->is_stripped = 1;
        }
    } else if (g_have_symtab) {
        bin->is_stripped = 1;
    }

    /* ── Stub symbol resolution ───────────────────────────────────────
     * For every __stubs entry (S_SYMBOL_STUBS), look up the matching
     * indirect symbol table entry (index = section reserved1 + stub no.),
     * map it to a symtab entry and register the imported name at the stub
     * address as SYM_IMPORT. This makes pthread_create / dispatch_async /
     * pthread_join etc. resolvable by name on real Mach-O binaries, which
     * the depflow spawn/join detection and the call graph rely on. */
    if (g_have_dysym && g_have_symtab && nstubsecs > 0) {
        int ssi;
        for (ssi = 0; ssi < nstubsecs; ssi++) {
            uint32_t stub_sz = stubsecs[ssi].r2;
            uint32_t nstub = stub_sz ? (uint32_t)(stubsecs[ssi].size / stub_sz) : 0;
            uint32_t k;
            if (stubsecs[ssi].r1 >= g_nindirect) continue;
            for (k = 0; k < nstub; k++) {
                uint32_t ind_idx = stubsecs[ssi].r1 + k;
                uint32_t raw_isym, isym;
                const macho_nlist64_t *nl;
                uint32_t strx;
                const char *name;
                dax_symbol_t *ns;
                dax_symbol_t *grown;

                if (ind_idx >= g_nindirect) break;
                if (bin->nsymbols >= DAX_MAX_SYMBOLS) break;
                raw_isym = LE32(*(const uint32_t *)(bin->data + g_indirectoff + (size_t)ind_idx * 4));
                if (raw_isym & 0x80000000u) continue; /* INDIRECT_SYMBOL_LOCAL */
                if (raw_isym & 0x40000000u) continue; /* INDIRECT_SYMBOL_ABS   */
                isym = raw_isym & 0x00FFFFFFu;
                if (isym >= g_nsyms) continue;

                nl = (const macho_nlist64_t *)(bin->data + g_symoff +
                                               (size_t)isym * sizeof(macho_nlist64_t));
                strx = LE32(nl->n_strx);
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


    
    bin->base       = text_vmbase;
    if (image_end > 0) {
        /* Store relative size (image_end - base), consistent with ELF parser */
        bin->image_size = (text_vmbase > 0 && image_end > text_vmbase)
                          ? (image_end - text_vmbase)
                          : image_end;
    } else {
        bin->image_size = (uint64_t)sz;
    }
    bin->code_size  = total_code;
    bin->data_size  = total_data;

    if (!bin->symbols) bin->is_stripped = 1;

    
    return 0;

#undef LE32
#undef LE64
}
