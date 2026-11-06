#include "nx_gen.h"
#include "nx_specs_builtin.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static char *camel(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 2);
    if (!out) return NULL;
    size_t o = 0;
    int up = 1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '_' || s[i] == '-' || s[i] == ' ') { up = 1; continue; }
        out[o++] = up ? (char)toupper((unsigned char)s[i]) : s[i];
        up = 0;
    }
    out[o] = 0;
    return out;
}

static const char *base_type(const nx_field_t *f) {
    switch (f->dec) {
        case NX_D_U8:    return "uint8_t";
        case NX_D_U16BE:
        case NX_D_U16LE: return "uint16_t";
        case NX_D_U32BE:
        case NX_D_U32LE: return "uint32_t";
        case NX_D_U64BE:
        case NX_D_U64LE: return "uint64_t";
        case NX_D_BOOL:  return "bool";
        case NX_D_ASCII: return "std::string";
        default:         return "uint64_t";
    }
}

static int is_le(const nx_field_t *f) {
    return f->dec == NX_D_U16LE || f->dec == NX_D_U32LE || f->dec == NX_D_U64LE;
}

/* 構造内で使う最大バイト数 (out バッファ確保用) */
static int struct_size(const nx_spec_t *s) {
    int max = 0;
    for (int i = 0; i < s->n_fields; i++) {
        const nx_field_t *f = &s->fields[i];
        int end = f->offset + (f->width > 0 ? f->width : 1);
        if (end > max) max = end;
    }
    return max < 1 ? 1 : max;
}

int nx_gen_cpp(const nx_spec_t *spec, const char *class_name, int inline_impl, FILE *out) {
    if (!spec || !out) return -1;
    (void)inline_impl;
    char *owned = NULL;
    if (!class_name) { owned = camel(spec->name); class_name = owned; }
    if (!class_name) { free(owned); return -1; }

    fprintf(out, "// generated from nx_spec \"%s\" (family: %s)\n", spec->name, spec->family);
    fprintf(out, "// %s\n", spec->summary);
    fprintf(out, "struct %s {\n", class_name);

    /* enum 型 (enum_names を持つフィールド。BITS/U8 でも列挙化) */
    for (int i = 0; i < spec->n_fields; i++) {
        const nx_field_t *f = &spec->fields[i];
        if (!f->enum_names) continue;
        char *en = camel(f->name);
        fprintf(out, "    enum class %s : uint8_t {\n", en ? en : "E");
        for (int k = 0; k < f->enum_count; k++)
            fprintf(out, "        %s = %d,\n", f->enum_names[k] ? f->enum_names[k] : "r", k);
        fprintf(out, "    };\n");
        free(en);
    }

    /* メンバ */
    for (int i = 0; i < spec->n_fields; i++) {
        const nx_field_t *f = &spec->fields[i];
        if (f->kind == NX_F_LENEX || f->kind == NX_F_REPEAT) {
            fprintf(out, "    std::vector<uint8_t> %s;   // %s payload\n", f->name,
                    f->kind == NX_F_LENEX ? "length-prefixed" : "repeated");
            continue;
        }
        if (f->enum_names) {
            char *en = camel(f->name);
            fprintf(out, "    %s %s = %s::%s;\n", en ? en : "uint8_t", f->name,
                    en ? en : "uint8_t",
                    f->enum_names[0] ? f->enum_names[0] : "0");
            free(en);
        } else if (f->dec == NX_D_BOOL) {
            fprintf(out, "    bool %s = false;   // bit %d of byte %d\n",
                    f->name, f->bit_shift, f->offset);
        } else if (f->dec == NX_D_ASCII) {
            fprintf(out, "    std::string %s;\n", f->name);
        } else {
            fprintf(out, "    %s %s = 0;\n", base_type(f), f->name);
        }
    }
    fprintf(out, "    std::vector<uint8_t> payload;   // trailing bytes\n");

    int ssize = struct_size(spec);

    /* parse */
    fprintf(out, "\n    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {\n");
    for (int i = 0; i < spec->n_fields; i++) {
        const nx_field_t *f = &spec->fields[i];
        if (f->kind == NX_F_LENEX) continue;
        int w = f->width > 0 ? f->width : 1;
        fprintf(out, "        if (%d + %d > size) return false;\n", f->offset, w);
        if (f->kind == NX_F_BITS) {
            if (f->enum_names) {
                char *en = camel(f->name);
                fprintf(out, "        %s = (%s)(((uint64_t)data[%d] >> %d) & 0x%llxULL);\n",
                        f->name, en ? en : "uint8_t", f->offset, f->bit_shift,
                        (unsigned long long)f->bit_mask);
                free(en);
            } else {
                fprintf(out, "        %s = (%s)(((uint64_t)data[%d] >> %d) & 0x%llxULL)%s;\n",
                        f->name, f->dec == NX_D_BOOL ? "bool" : "uint8_t",
                        f->offset, f->bit_shift, (unsigned long long)f->bit_mask,
                        f->dec == NX_D_BOOL ? " != 0" : "");
            }
        } else if (f->enum_names) {
            char *en = camel(f->name);
            fprintf(out, "        %s = (%s)data[%d];\n", f->name, en ? en : "uint8_t", f->offset);
            free(en);
        } else if (f->dec == NX_D_BOOL) {
            fprintf(out, "        %s = (data[%d] != 0);\n", f->name, f->offset);
        } else if (f->dec == NX_D_ASCII) {
            fprintf(out, "        %s.assign(reinterpret_cast<const char*>(data + %d), %d);\n",
                    f->name, f->offset, w);
        } else {
            fprintf(out, "        { uint64_t v=0,u=0; for(int i=0;i<%d;++i)v=(v<<8)|data[%d+i];"
                         "for(int i=0;i<%d;++i)u|=(uint64_t)data[%d+i]<<(8*i); %s=(%s)(%s); }\n",
                    w, f->offset, w, f->offset, f->name, base_type(f), is_le(f) ? "u" : "v");
        }
    }
    fprintf(out, "        if (size > %d) payload.assign(data + %d, data + size);\n", ssize, ssize);
    fprintf(out, "        if (consumed) *consumed = size;\n");
    fprintf(out, "        return true;\n    }\n");

    /* serialize */
    fprintf(out, "\n    std::vector<uint8_t> serialize() const {\n");
    fprintf(out, "        std::vector<uint8_t> out(%d, 0);\n", ssize);
    for (int i = 0; i < spec->n_fields; i++) {
        const nx_field_t *f = &spec->fields[i];
        if (f->kind == NX_F_LENEX || f->kind == NX_F_REPEAT) continue;
        int w = f->width > 0 ? f->width : 1;
        if (f->kind == NX_F_BITS) {
            fprintf(out, "        out[%d] |= (uint8_t)(((uint64_t)%s & 0x%llxULL) << %d);\n",
                    f->offset, f->name, (unsigned long long)f->bit_mask, f->bit_shift);
        } else if (f->enum_names || f->dec == NX_D_U8) {
            fprintf(out, "        out[%d] = (uint8_t)%s;\n", f->offset, f->name);
        } else if (f->dec == NX_D_BOOL) {
            fprintf(out, "        out[%d] = %s ? 1 : 0;\n", f->offset, f->name);
        } else if (f->dec == NX_D_ASCII) {
            fprintf(out, "        for (size_t i=0;i<%s.size() && %d+i<out.size();++i) out[%d+i]=(uint8_t)%s[i];\n",
                    f->name, f->offset, f->offset, f->name);
        } else {
            if (is_le(f))
                fprintf(out, "        { uint64_t v=(uint64_t)%s; for(int i=0;i<%d;++i) out[%d+i]=(uint8_t)((v>>(8*i))&0xff); }\n",
                        f->name, w, f->offset);
            else
                fprintf(out, "        { uint64_t v=(uint64_t)%s; for(int i=0;i<%d;++i) out[%d+i]=(uint8_t)((v>>(8*(%d-1-i)))&0xff); }\n",
                        f->name, w, f->offset, w);
        }
    }
    fprintf(out, "        out.insert(out.end(), payload.begin(), payload.end());\n");
    fprintf(out, "        return out;\n    }\n");

    fprintf(out, "\n    static const char* spec_name() { return \"%s\"; }\n", spec->name);
    fprintf(out, "};\n\n");

    free(owned);
    return 0;
}

int nx_gen_cpp_all(int inline_impl, FILE *out) {
    nx_specs_builtin_register();
    fprintf(out, "#pragma once\n");
    fprintf(out, "#include <cstdint>\n#include <vector>\n#include <string>\n\n");
    fprintf(out, "namespace nx_generated {\n\n");
    for (int i = 0; i < nx_spec_count(); i++)
        nx_gen_cpp(nx_spec_at(i), NULL, inline_impl, out);
    fprintf(out, "} // namespace nx_generated\n");
    return 0;
}
