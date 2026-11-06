#include "nx_state.h"
#include "nx_json.h"
#include "nx_leveldb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static char *xdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void *memfind(const void *hay, size_t hlen, const void *needle, size_t nlen) {
    if (!hay || !needle || nlen == 0 || hlen < nlen) return NULL;
    const unsigned char *h = (const unsigned char *)hay;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (h[i] == ((const unsigned char *)needle)[0] &&
            memcmp(h + i, needle, nlen) == 0)
            return (void *)(h + i);
    return NULL;
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static unsigned char *read_entire(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = 0;
    if (out_size) *out_size = rd;
    return buf;
}

/* ------------------------------------------------------------------ */
/* タブ状 JSON の判定 (形式非依存)                                     */
/* ------------------------------------------------------------------ */

static int tab_like(const char *txt, size_t len) {
    nx_json_t *root = nx_json_parse(txt, len);
    if (!root) return 0;
    const nx_json_t *st = nx_json_get(root, "state");
    const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : nx_json_get(root, "tabs");
    int ok = tabs && tabs->type == NX_JSON_ARR;
    nx_json_free(root);
    return ok;
}

typedef struct {
    char              *best;
    unsigned long long best_seq;
    char              *origin;
} cand_t;

/* バッファ内の { ... } を総当たりで調べ、タブ状 JSON を拾う。 */
static void scan_json_objs(const unsigned char *b, size_t n, const char *origin,
                           unsigned long long seq, cand_t *c) {
    for (size_t i = 0; i + 8 < n; i++) {
        if (b[i] != '{') continue;
        size_t j = i + 1;
        while (j < n && (b[j] == ' ' || b[j] == '\n' || b[j] == '\r' || b[j] == '\t'))
            j++;
        if (j >= n || b[j] != '"') continue;

        int depth = 0, in_str = 0, esc = 0, ok = 0;
        size_t k = i;
        for (; k < n; k++) {
            char ch = (char)b[k];
            if (in_str) {
                if (esc) esc = 0;
                else if (ch == '\\') esc = 1;
                else if (ch == '"') in_str = 0;
                continue;
            }
            if (ch == '"') { in_str = 1; continue; }
            if (ch == '{') depth++;
            else if (ch == '}') { depth--; if (depth == 0) { ok = 1; k++; break; } }
        }
        if (!ok) continue;
        size_t len = k - i;
        if (!memfind(b + i, len, "\"tabs\"", 6)) { i = k - 1; continue; }
        char *cand = (char *)malloc(len + 1);
        if (!cand) return;
        memcpy(cand, b + i, len);
        cand[len] = '\0';
        if (tab_like(cand, len) && seq >= c->best_seq) {
            free(c->best);
            free(c->origin);
            c->best = cand;
            c->best_seq = seq;
            c->origin = xdup(origin);
        } else {
            free(cand);
        }
        i = k - 1;
    }
}

/* ------------------------------------------------------------------ */
/* LevelDB / KV ストア                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *hint;
    const char *origin;
    cand_t     *c;
    /* key 記録用 */
    unsigned long long best_seq;
    char              *best_key;
    size_t             best_keylen;
    int                want_key;
} kv_ud_t;

static void kv_cb(const char *key, size_t klen, const unsigned char *val, size_t vlen,
                  unsigned long long seq, void *ud) {
    kv_ud_t *u = (kv_ud_t *)ud;
    if (u->want_key && key && klen) {
        if (!u->hint || memfind(key, klen, u->hint, strlen(u->hint))) {
            if (seq >= u->best_seq) {
                char *kc = (char *)malloc(klen + 1);
                if (kc) {
                    memcpy(kc, key, klen);
                    kc[klen] = '\0';
                    free(u->best_key);
                    u->best_key = kc;
                    u->best_keylen = klen;
                    u->best_seq = seq;
                }
            }
        }
    }
    if (!val || vlen == 0) return;
    if (u->hint && !memfind(key, klen, u->hint, strlen(u->hint))) return;
    scan_json_objs(val, vlen, u->origin, seq, u->c);
}

static int looks_like_leveldb(const unsigned char *buf, size_t size) {
    if (size < 48) return 0;
    uint64_t magic = (uint64_t)buf[size - 8] | ((uint64_t)buf[size - 7] << 8) |
                     ((uint64_t)buf[size - 6] << 16) | ((uint64_t)buf[size - 5] << 24) |
                     ((uint64_t)buf[size - 4] << 32) | ((uint64_t)buf[size - 3] << 40) |
                     ((uint64_t)buf[size - 2] << 48) | ((uint64_t)buf[size - 1] << 56);
    return magic == 0xdb4775248b80fb57ULL;
}

static void scan_file(const char *path, const char *hint, cand_t *raw, cand_t *ldb) {
    size_t size = 0;
    unsigned char *buf = read_entire(path, &size);
    if (!buf) return;
    if (size > 0 && size < (16u * 1024 * 1024)) {
        int is_ldb = ends_with(path, ".ldb") || ends_with(path, ".log") ||
                     looks_like_leveldb(buf, size);
        if (is_ldb) {
            kv_ud_t u = { hint, path, ldb };
            nx_leveldb_scan_file(path, kv_cb, &u);
        }
        /* 非圧縮ストア (JSON / SQLite / plist / 生ログ) は raw scan で拾う */
        struct stat stt;
        unsigned long long mt = 0;
        if (stat(path, &stt) == 0) mt = (unsigned long long)stt.st_mtime;
        scan_json_objs(buf, size, path, mt, raw);
    }
    free(buf);
}

typedef struct {
    const char *hint;
    char      **keys;
    size_t     *lens;
    int         n;
    int         cap;
    int         max;
} listkey_ud_t;

static void listkey_cb(const char *key, size_t klen, const unsigned char *val,
                       size_t vlen, unsigned long long seq, void *ud) {
    (void)val; (void)vlen; (void)seq;
    listkey_ud_t *u = (listkey_ud_t *)ud;
    if (!key || !klen) return;
    if (u->hint && !memfind(key, klen, u->hint, strlen(u->hint))) return;
    for (int i = 0; i < u->n; i++)
        if (u->lens[i] == klen && memcmp(u->keys[i], key, klen) == 0) return;
    if (u->n >= u->max) return;
    char *kc = (char *)malloc(klen + 1);
    if (!kc) return;
    memcpy(kc, key, klen); kc[klen] = '\0';
    u->keys[u->n] = kc; u->lens[u->n] = klen; u->n++;
}

int nx_state_list_keys(const char *dir, const char *key_hint,
                       char ***out_keys, size_t **out_lens, int max) {
    if (out_keys) *out_keys = NULL;
    if (out_lens) *out_lens = NULL;
    struct stat stt;
    if (!dir || stat(dir, &stt) != 0) return 0;
    listkey_ud_t u; memset(&u, 0, sizeof(u));
    u.hint = key_hint; u.max = max > 0 ? max : 64;
    u.keys = (char **)calloc(u.max, sizeof(char *));
    u.lens = (size_t *)calloc(u.max, sizeof(size_t));
    if (!u.keys || !u.lens) { free(u.keys); free(u.lens); return 0; }

    if (S_ISDIR(stt.st_mode)) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                if (!ends_with(e->d_name, ".ldb") && !ends_with(e->d_name, ".log"))
                    continue;
                char fp[4096];
                snprintf(fp, sizeof(fp), "%s/%s", dir, e->d_name);
                nx_leveldb_scan_file(fp, listkey_cb, &u);
            }
            closedir(d);
        }
    } else if (ends_with(dir, ".ldb") || ends_with(dir, ".log")) {
        nx_leveldb_scan_file(dir, listkey_cb, &u);
    }
    if (out_keys) *out_keys = u.keys; else { for (int i=0;i<u.n;i++) free(u.keys[i]); free(u.keys); }
    if (out_lens) *out_lens = u.lens; else free(u.lens);
    return u.n;
}

char *nx_state_find_key(const char *dir, const char *key_hint,
                        size_t *out_key_len) {
    if (out_key_len) *out_key_len = 0;
    struct stat stt;
    if (!dir || stat(dir, &stt) != 0) return NULL;
    kv_ud_t u; memset(&u, 0, sizeof(u));
    u.hint = key_hint; u.origin = dir; u.want_key = 1;
    cand_t dummy;
    dummy.best = NULL; dummy.origin = NULL; dummy.best_seq = 0;
    u.c = &dummy;

    if (S_ISDIR(stt.st_mode)) {
        DIR *d = opendir(dir);
        if (!d) return NULL;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (!ends_with(e->d_name, ".ldb") && !ends_with(e->d_name, ".log"))
                continue;
            char fp[4096];
            snprintf(fp, sizeof(fp), "%s/%s", dir, e->d_name);
            nx_leveldb_scan_file(fp, kv_cb, &u);
        }
        closedir(d);
    } else if (ends_with(dir, ".ldb") || ends_with(dir, ".log")) {
        nx_leveldb_scan_file(dir, kv_cb, &u);
    }
    free(dummy.best); free(dummy.origin);
    if (out_key_len) *out_key_len = u.best_keylen;
    return u.best_key;
}

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

char *nx_state_find_tabs_json(const char *path, const char *key_hint,
                              char **out_origin) {
    if (out_origin) *out_origin = NULL;
    struct stat stt;
    if (!path || stat(path, &stt) != 0) return NULL;

    cand_t raw, ldb;
    memset(&raw, 0, sizeof(raw));
    memset(&ldb, 0, sizeof(ldb));

    if (S_ISDIR(stt.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return NULL;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strcmp(ent->d_name, "LOCK") == 0 ||
                strcmp(ent->d_name, "CURRENT") == 0) continue;
            char fp[4096];
            snprintf(fp, sizeof(fp), "%s/%s", path, ent->d_name);
            scan_file(fp, key_hint, &raw, &ldb);
        }
        closedir(d);
    } else {
        scan_file(path, key_hint, &raw, &ldb);
    }

    /* KV ストアで見つかればそれを優先 (アプリの一次ストア)。
       無ければ任意バイナリ/JSON からの raw 発見を使う。 */
    cand_t *win = ldb.best ? &ldb : &raw;
    char *result = win->best;
    if (out_origin) *out_origin = win->origin;
    else free(win->origin);

    if (win != &ldb) free(ldb.best), free(ldb.origin);
    if (win != &raw) free(raw.best), free(raw.origin);
    return result;
}
