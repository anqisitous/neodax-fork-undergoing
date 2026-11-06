#include "nx_spec.h"

#include <string.h>
#include <stdlib.h>

#define NX_MAX_SPECS 128
static const nx_spec_t *g_specs[NX_MAX_SPECS];
static int              g_nspecs = 0;
static int              g_registered = 0;

void nx_spec_register(const nx_spec_t *s) {
    if (!s) return;
    for (int i = 0; i < g_nspecs; i++) {
        if (g_specs[i] == s) return;
        if (g_specs[i]->name && s->name && strcmp(g_specs[i]->name, s->name) == 0) return;
    }
    if (g_nspecs < NX_MAX_SPECS) g_specs[g_nspecs++] = s;
}

const nx_spec_t *nx_spec_by_name(const char *name) {
    for (int i = 0; i < g_nspecs; i++)
        if (g_specs[i]->name && strcmp(g_specs[i]->name, name) == 0) return g_specs[i];
    return NULL;
}

const nx_spec_t *nx_spec_at(int i) {
    return (i >= 0 && i < g_nspecs) ? g_specs[i] : NULL;
}

int nx_spec_count(void) { return g_nspecs; }

uint64_t nx_spec_read_int(const uint8_t *data, size_t size, const nx_field_t *f) {
    if (!data || !f) return 0;
    size_t off = (size_t)f->offset;
    uint64_t v = 0;
    switch (f->dec) {
        case NX_D_U8:
            if (off + 1 > size) return 0;
            v = data[off];
            break;
        case NX_D_U16BE:
            if (off + 2 > size) return 0;
            v = ((uint64_t)data[off] << 8) | data[off + 1];
            break;
        case NX_D_U16LE:
            if (off + 2 > size) return 0;
            v = data[off] | ((uint64_t)data[off + 1] << 8);
            break;
        case NX_D_U32BE:
            if (off + 4 > size) return 0;
            v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | data[off + i];
            break;
        case NX_D_U32LE:
            if (off + 4 > size) return 0;
            v = 0; for (int i = 0; i < 4; i++) v |= (uint64_t)data[off + i] << (8 * i);
            break;
        case NX_D_U64BE:
            if (off + 8 > size) return 0;
            v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | data[off + i];
            break;
        case NX_D_U64LE:
            if (off + 8 > size) return 0;
            v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)data[off + i] << (8 * i);
            break;
        case NX_D_BOOL:
            if (off >= size) return 0;
            v = data[off] ? 1 : 0;
            break;
        case NX_D_ENUM:
        case NX_D_ASCII:
            if (off >= size) return 0;
            v = data[off];
            break;
        default:
            v = 0;
    }
    if (f->kind == NX_F_BITS) v = (v >> f->bit_shift) & f->bit_mask;
    return v;
}

const char *nx_spec_enum_name(const nx_field_t *f, uint64_t v) {
    if (!f || !f->enum_names) return NULL;
    if ((int)v < f->enum_count && f->enum_names[v]) return f->enum_names[v];
    return NULL;
}
