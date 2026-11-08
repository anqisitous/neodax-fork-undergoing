/*
 * nx_asar_patch — app.asar の中身を「同じ長さ」で書き換える。
 *
 * なぜ同長に限るか:
 *   asar は header (JSON) に各ファイルの offset と size を持ち、その直後から
 *   実データが連結されている。長さを変えると後続すべての offset を書き直す
 *   必要があり、header 長も変わる。同長なら 1 箇所のバイト列を差し替えるだけで
 *   他のエントリに影響しない。
 *
 * 署名について:
 *   変更後は codesign のハッシュが合わなくなる。ad-hoc 再署名
 *   (codesign --force --sign -) で検証を通せるが、TeamIdentifier は失われる。
 *   これは「配布物の同一性を捨てて、ローカルのコピーを動かす」選択であり、
 *   元の /Applications を直接書き換えるべきではない (--fork で複製してから行う)。
 */

#include "nx_asar_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* asar header を読み、(data_base, 目的エントリの絶対 offset/size) を返す。 */
typedef struct {
    size_t data_base;
    size_t offset;   /* 絶対ファイル位置 */
    size_t size;
    char   path[512];
} asar_loc_t;

static unsigned le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* 文字列中の "offset" を JSON 内で探して数値を取る (限定的な走査)。 */
static int json_number_after(const char *hay, const char *key, long long *out) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(hay, pat);
    if (!p) return -1;
    // このエントリのオブジェクト (外側の {} ) の中だけを探す。
    // 入れ子 (integrity など) があるので、深さを数えて外側の閉じを探す。
    {
        const char *open = strchr(hay, '{');
        if (!open || p < open) return -1;
        int depth = 0, in_str = 0, esc = 0;
        const char *q = open;
        for (; *q; q++) {
            char ch = *q;
            if (in_str) {
                if (esc) esc = 0;
                else if (ch == '\\') esc = 1;
                else if (ch == '"') in_str = 0;
                continue;
            }
            if (ch == '"') { in_str = 1; continue; }
            if (ch == '{') depth++;
            else if (ch == '}') { depth--; if (depth == 0) break; }
        }
        if (p > q) return -1;   // エントリの外
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        *out = strtoll(p, NULL, 10);
    } else {
        *out = strtoll(p, NULL, 10);
    }
    return 0;
}

int nx_asar_locate(const char *asar, const char *needle, FILE *out) {
    if (!asar || !needle) return -1;
    FILE *f = fopen(asar, "rb");
    if (!f) return -1;
    unsigned char hdr[16];
    if (fread(hdr, 1, 16, f) != 16) { fclose(f); return -1; }
    if (le32(hdr) != 4) { fclose(f); return -1; }
    size_t header_size = le32(hdr + 4);
    size_t json_len = le32(hdr + 12);
    char *json = (char *)malloc(json_len + 1);
    if (!json) { fclose(f); return -1; }
    if (fread(json, 1, json_len, f) != json_len) { free(json); fclose(f); return -1; }
    json[json_len] = 0;
    fclose(f);

    size_t data_base = 8 + header_size;

    /* needle を含むファイルエントリを探す。"path" の直後のブロックを見る。 */
    const char *p = json;
    int found = 0;
    asar_loc_t loc;
    memset(&loc, 0, sizeof loc);
    while ((p = strstr(p, "\"")) != NULL) {
        /* "name": { ... } の形で、name に needle が含まれるものを探す */
        const char *name_end = strchr(p + 1, '"');
        if (!name_end) break;
        size_t nlen = (size_t)(name_end - (p + 1));
        char name[512] = {0};
        if (nlen < sizeof name) {
            memcpy(name, p + 1, nlen);
            if (strstr(name, needle)) {
                /* このブロック内の offset と size */
                const char *q = name_end;
                long long off = -1, sz = -1;
                const char *ob = strchr(q, '{');
                if (ob && json_number_after(ob, "offset", &off) == 0 &&
                    json_number_after(ob, "size", &sz) == 0) {
                    loc.data_base = data_base;
                    loc.offset = data_base + (size_t)off;
                    loc.size = (size_t)sz;
                    snprintf(loc.path, sizeof loc.path, "%s", name);
                    found = 1;
                    break;
                }
            }
        }
        p = name_end;
    }
    free(json);
    if (!found) return -1;
    if (out) fprintf(out, "  [asar] %s abs=%zu size=%zu\n",
                     loc.path, loc.offset, loc.size);
    return 0;
}

int nx_asar_patch_bytes(const char *asar, const char *needle_entry,
                        const char *find, const char *repl, FILE *out) {
    if (!asar || !needle_entry || !find || !repl) return 2;
    size_t flen = strlen(find), rlen = strlen(repl);
    if (flen != rlen) {
        if (out) fprintf(out, "  [asar] find/repl must be the same length "
                              "(%zu vs %zu)\n", flen, rlen);
        return 2;
    }
    /* locate の結果を使うため内部で再走査する */
    FILE *f = fopen(asar, "r+b");
    if (!f) return 1;
    unsigned char hdr[16];
    if (fread(hdr, 1, 16, f) != 16) { fclose(f); return 1; }
    size_t header_size = le32(hdr + 4);
    size_t json_len = le32(hdr + 12);
    char *json = (char *)malloc(json_len + 1);
    if (!json) { fclose(f); return 1; }
    if (fread(json, 1, json_len, f) != json_len) { free(json); fclose(f); return 1; }
    json[json_len] = 0;
    size_t data_base = 8 + header_size;

    size_t ent_off = 0, ent_size = 0;
    int found = 0;
    char ent_path[512] = {0};
    const char *p = json;
    while ((p = strstr(p, "\"")) != NULL) {
        const char *name_end = strchr(p + 1, '"');
        if (!name_end) break;
        size_t nlen = (size_t)(name_end - (p + 1));
        char name[512] = {0};
        if (nlen < sizeof name) {
            memcpy(name, p + 1, nlen);
            if (strstr(name, needle_entry)) {
                long long off = -1, sz = -1;
                const char *ob = strchr(name_end, '{');
                if (ob && json_number_after(ob, "offset", &off) == 0 &&
                    json_number_after(ob, "size", &sz) == 0) {
                    ent_off = data_base + (size_t)off;
                    ent_size = (size_t)sz;
                    snprintf(ent_path, sizeof ent_path, "%s", name);
                    found = 1;
                    break;
                }
            }
        }
        p = name_end;
    }
    free(json);
    if (!found) { fclose(f); if (out) fprintf(out, "  [asar] entry not found\n"); return 1; }

    char *buf = (char *)malloc(ent_size);
    if (!buf) { fclose(f); return 1; }
    fseek(f, (long)ent_off, SEEK_SET);
    if (fread(buf, 1, ent_size, f) != ent_size) { free(buf); fclose(f); return 1; }

    int hits = 0;
    for (size_t i = 0; i + flen <= ent_size; i++) {
        if (memcmp(buf + i, find, flen) == 0) {
            fseek(f, (long)(ent_off + i), SEEK_SET);
            fwrite(repl, 1, rlen, f);
            hits++;
        }
    }
    free(buf);
    fclose(f);
    if (out)
        fprintf(out, "  [asar] %s: %d occurrence(s) '%s' -> '%s'\n",
                ent_path, hits, find, repl);
    return hits > 0 ? 0 : 1;
}
