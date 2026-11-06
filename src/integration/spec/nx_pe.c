#include "nx_pe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 読み出しヘルパ (リトルエンディアン) ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static char *xdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static const char *machine_name(uint16_t m) {
    switch (m) {
        case 0x014c: return "i386";
        case 0x8664: return "x86-64";
        case 0x01c0: return "ARM";
        case 0x01c4: return "ARMv7";
        case 0xaa64: return "ARM64";
        case 0x0200: return "IA64";
        default:     return "unknown";
    }
}
static const char *subsystem_name(uint16_t s) {
    switch (s) {
        case 1:  return "NATIVE";
        case 2:  return "WINDOWS_GUI";
        case 3:  return "WINDOWS_CUI";
        case 9:  return "WINDOWS_CE_GUI";
        case 10: return "EFI_APPLICATION";
        case 14: return "XBOX";
        default: return "other";
    }
}

static const uint8_t *rva_to_ptr(const nx_pe_image_t *img, const uint8_t *data,
                                 size_t size, uint32_t rva) {
    for (int i = 0; i < img->n_sections; i++) {
        const nx_pe_section_t *s = &img->sections[i];
        uint32_t sz = s->virtual_size ? s->virtual_size : s->raw_size;
        if (rva >= s->virtual_address && rva < s->virtual_address + sz) {
            size_t off = s->raw_ptr + (rva - s->virtual_address);
            if (off < size) return data + off;
        }
    }
    return NULL;
}

nx_pe_image_t *nx_pe_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 64) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(fp); return NULL; }
    if (fread(data, 1, (size_t)sz, fp) != (size_t)sz) { free(data); fclose(fp); return NULL; }
    fclose(fp);
    size_t size = (size_t)sz;

    if (data[0] != 'M' || data[1] != 'Z') { free(data); return NULL; }
    uint32_t e_lfanew = rd32(data + 0x3c);
    if (e_lfanew + 24 > size) { free(data); return NULL; }
    const uint8_t *nt = data + e_lfanew;
    if (memcmp(nt, "PE\0\0", 4) != 0) { free(data); return NULL; }

    const uint8_t *fh = nt + 4;                 /* IMAGE_FILE_HEADER */
    uint16_t machine = rd16(fh);
    uint16_t nsec = rd16(fh + 2);
    uint32_t tds = rd32(fh + 4);
    uint16_t opt_size = rd16(fh + 16);
    const uint8_t *opt = fh + 20;

    nx_pe_image_t *img = (nx_pe_image_t *)calloc(1, sizeof(*img));
    if (!img) { free(data); return NULL; }
    img->machine = machine;
    img->machine_name = machine_name(machine);
    img->num_sections = nsec;
    img->timestamp = tds;

    uint16_t omagic = rd16(opt);
    img->is64 = (omagic == 0x020B);
    if (omagic != 0x010B && omagic != 0x020B) { free(img); free(data); return NULL; }

    img->entry_point = rd32(opt + 16);
    if (img->is64) img->image_base = rd64(opt + 24);
    else           img->image_base = rd32(opt + 28);
    uint16_t subsys_off = img->is64 ? 68 : 68;  /* Subsystem offset in optional header */
    img->subsystem = rd16(opt + subsys_off);
    img->subsystem_name = subsystem_name(img->subsystem);
    img->size_of_image = rd32(opt + 56);
    img->size_of_headers = rd32(opt + 60);

    /* DataDirectory: 32bit は opt+96, 64bit は opt+112 から 16 エントリ */
    size_t dd_off = img->is64 ? 112 : 96;
    for (int i = 0; i < 16; i++) {
        img->dd_rva[i] = rd32(opt + dd_off + i * 8);
        img->dd_size[i] = rd32(opt + dd_off + i * 8 + 4);
    }

    /* セクション表: opt の後ろ opt_size バイト */
    const uint8_t *secs = opt + opt_size;
    if (secs + (size_t)nsec * 40 > data + size) {
        img->n_sections = 0;
    } else {
        img->sections = (nx_pe_section_t *)calloc(nsec ? nsec : 1, sizeof(nx_pe_section_t));
        img->n_sections = nsec;
        for (int i = 0; i < nsec; i++) {
            const uint8_t *s = secs + (size_t)i * 40;
            nx_pe_section_t *o = &img->sections[i];
            memcpy(o->name, s, 8); o->name[8] = 0;
            o->virtual_size = rd32(s + 8);
            o->virtual_address = rd32(s + 12);
            o->raw_size = rd32(s + 16);
            o->raw_ptr = rd32(s + 20);
            o->characteristics = rd32(s + 36);
        }
    }
    free(data);
    return img;
}

void nx_pe_free(nx_pe_image_t *img) {
    if (!img) return;
    free(img->sections);
    free(img);
}

void nx_pe_print(const nx_pe_image_t *img, FILE *out) {
    if (!img || !out) return;
    fprintf(out, "  Format: PE (%s)\n", img->is64 ? "PE32+" : "PE32");
    fprintf(out, "  Machine: 0x%04x (%s)\n", img->machine, img->machine_name);
    fprintf(out, "  Entry: 0x%x   ImageBase: 0x%llx\n", img->entry_point,
            (unsigned long long)img->image_base);
    fprintf(out, "  Subsystem: %s   SizeOfImage: 0x%x\n",
            img->subsystem_name, img->size_of_image);
    fprintf(out, "  Sections: %d\n", img->n_sections);
    fprintf(out, "    %-9s %10s %10s %10s %10s  flags\n", "name", "vsize", "vaddr", "rawsz", "rawptr");
    for (int i = 0; i < img->n_sections; i++) {
        const nx_pe_section_t *s = &img->sections[i];
        fprintf(out, "    %-9s %10u %10x %10u %10x  0x%08x%s%s%s\n",
                s->name, s->virtual_size, s->virtual_address, s->raw_size, s->raw_ptr,
                s->characteristics,
                (s->characteristics & 0x20000000) ? " X" : "",
                (s->characteristics & 0x80000000) ? " W" : "",
                (s->characteristics & 0x40000000) ? " R" : "");
    }
    fprintf(out, "  DataDirectories:\n");
    static const char *ddn[16] = {
        "Export", "Import", "Resource", "Exception", "Certificate", "Reloc",
        "Debug", "Arch", "GlobalPtr", "TLS", "LoadConfig", "BoundImport",
        "IAT", "DelayImport", "CLR", "Reserved"
    };
    for (int i = 0; i < 16; i++) {
        if (!img->dd_rva[i]) continue;
        fprintf(out, "    %-12s rva=0x%-8x size=%u\n", ddn[i], img->dd_rva[i], img->dd_size[i]);
    }
}

/* ---- Import / Export ---- */

nx_pe_import_t *nx_pe_read_imports(const char *path, int *n_out) {
    if (n_out) *n_out = 0;
    nx_pe_image_t *img = nx_pe_open(path);
    if (!img) return NULL;
    if (img->dd_rva[1] == 0) { nx_pe_free(img); return NULL; }

    /* need raw data again */
    FILE *fp = fopen(path, "rb");
    if (!fp) { nx_pe_free(img); return NULL; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(fp); nx_pe_free(img); return NULL; }
    if (fread(data, 1, (size_t)sz, fp) != (size_t)sz) { free(data); fclose(fp); nx_pe_free(img); return NULL; }
    fclose(fp);

    nx_pe_import_t *arr = NULL;
    int n = 0, cap = 0;
    const uint8_t *desc = rva_to_ptr(img, data, (size_t)sz, img->dd_rva[1]);
    while (desc) {
        uint32_t orig_first = rd32(desc);
        uint32_t name_rva = rd32(desc + 12);
        uint32_t first_thunk = rd32(desc + 16);
        if (orig_first == 0 && name_rva == 0 && first_thunk == 0) break;

        const char *dll = NULL;
        if (name_rva) {
            const uint8_t *dn = rva_to_ptr(img, data, (size_t)sz, name_rva);
            if (dn) dll = (const char *)dn;
        }
        if (n >= cap) { cap = cap ? cap * 2 : 16; arr = (nx_pe_import_t *)realloc(arr, cap * sizeof(*arr)); }
        nx_pe_import_t *imp = &arr[n];
        memset(imp, 0, sizeof(*imp));
        snprintf(imp->dll, sizeof(imp->dll), "%s", dll ? dll : "?");
        imp->funcs = NULL; imp->ords = NULL; imp->n_funcs = 0;

        uint32_t thunk_rva = orig_first ? orig_first : first_thunk;
        const uint8_t *th = rva_to_ptr(img, data, (size_t)sz, thunk_rva);
        int fc = 0, fcap = 0;
        while (th) {
            uint64_t t = img->is64 ? rd64(th) : rd32(th);
            if (t == 0) break;
            const char *fname = NULL;
            uint16_t ord = 0;
            int by_ord = img->is64 ? (t & 0x8000000000000000ULL) : (t & 0x80000000u);
            if (by_ord) {
                ord = (uint16_t)(t & 0xffff);
            } else {
                uint32_t hint_rva = (uint32_t)t;
                const uint8_t *hn = rva_to_ptr(img, data, (size_t)sz, hint_rva + 2);
                if (hn) fname = (const char *)hn;
            }
            if (fc >= fcap) { fcap = fcap ? fcap * 2 : 8;
                imp->funcs = (char **)realloc(imp->funcs, fcap * sizeof(char *));
                imp->ords = (uint16_t *)realloc(imp->ords, fcap * sizeof(uint16_t)); }
            imp->funcs[fc] = fname ? xdup(fname) : NULL;
            imp->ords[fc] = ord;
            fc++;
            th += img->is64 ? 8 : 4;
        }
        imp->n_funcs = fc;
        n++;
        desc += 20; /* sizeof IMAGE_IMPORT_DESCRIPTOR */
    }
    free(data);
    nx_pe_free(img);
    if (n_out) *n_out = n;
    return arr;
}

nx_pe_export_t *nx_pe_read_exports(const char *path, int *n_out) {
    if (n_out) *n_out = 0;
    nx_pe_image_t *img = nx_pe_open(path);
    if (!img) return NULL;
    if (img->dd_rva[0] == 0) { nx_pe_free(img); return NULL; }

    FILE *fp = fopen(path, "rb");
    if (!fp) { nx_pe_free(img); return NULL; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(fp); nx_pe_free(img); return NULL; }
    if (fread(data, 1, (size_t)sz, fp) != (size_t)sz) { free(data); fclose(fp); nx_pe_free(img); return NULL; }
    fclose(fp);

    nx_pe_export_t *ex = (nx_pe_export_t *)calloc(1, sizeof(*ex));
    const uint8_t *dir = rva_to_ptr(img, data, (size_t)sz, img->dd_rva[0]);
    if (dir) {
        uint32_t nnames = rd32(dir + 24);
        uint32_t names_rva = rd32(dir + 32);
        const uint8_t *names = rva_to_ptr(img, data, (size_t)sz, names_rva);
        ex->names = (char **)calloc(nnames ? nnames : 1, sizeof(char *));
        ex->rvas = (uint32_t *)calloc(nnames ? nnames : 1, sizeof(uint32_t));
        ex->n = 0;
        for (uint32_t i = 0; i < nnames && names; i++) {
            uint32_t nr = rd32(names + i * 4);
            const uint8_t *nm = rva_to_ptr(img, data, (size_t)sz, nr);
            if (nm) { ex->names[ex->n] = xdup((const char *)nm); ex->rvas[ex->n] = nr; ex->n++; }
        }
    }
    free(data);
    nx_pe_free(img);
    if (n_out) *n_out = ex->n;
    return ex;
}

int nx_pe_is_pe(const uint8_t *data, size_t size) {
    if (!data || size < 0x40) return 0;
    if (data[0] != 'M' || data[1] != 'Z') return 0;
    uint32_t e = rd32(data + 0x3c);
    if ((size_t)e + 4 > size) return 0;
    return memcmp(data + e, "PE\0\0", 4) == 0;
}

int nx_pe_analyze(const char *path, FILE *out) {
    if (!out) out = stdout;
    nx_pe_image_t *img = nx_pe_open(path);
    if (!img) { fprintf(out, "  [pe] not a valid PE: %s\n", path); return -1; }

    fprintf(out, "\n  === PE analysis: %s ===\n", path);
    nx_pe_print(img, out);

    int ni = 0;
    nx_pe_import_t *imps = nx_pe_read_imports(path, &ni);
    fprintf(out, "\n  Imports (%d DLLs):\n", ni);
    for (int i = 0; i < ni; i++) {
        fprintf(out, "    %s (%d funcs)\n", imps[i].dll, imps[i].n_funcs);
        for (int k = 0; k < imps[i].n_funcs && k < 8; k++) {
            if (imps[i].funcs[k]) fprintf(out, "        %s\n", imps[i].funcs[k]);
            else fprintf(out, "        #%u\n", imps[i].ords[k]);
        }
        if (imps[i].n_funcs > 8) fprintf(out, "        ... +%d more\n", imps[i].n_funcs - 8);
    }
    for (int i = 0; i < ni; i++) {
        for (int k = 0; k < imps[i].n_funcs; k++) free(imps[i].funcs ? imps[i].funcs[k] : NULL);
        free(imps[i].funcs); free(imps[i].ords);
    }
    free(imps);

    int ne = 0;
    nx_pe_export_t *exps = nx_pe_read_exports(path, &ne);
    fprintf(out, "\n  Exports (%d):\n", ne);
    if (exps) {
        for (int i = 0; i < ne && i < 20; i++) fprintf(out, "    %s\n", exps[i].names[i]);
        if (ne > 20) fprintf(out, "    ... +%d more\n", ne - 20);
        for (int i = 0; i < ne; i++) free(exps[i].names[i]);
        free(exps->names); free(exps->rvas); free(exps);
    }
    nx_pe_free(img);
    return 0;
}
