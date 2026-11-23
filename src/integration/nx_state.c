#include "nx_state.h"
#include "nx_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* 1 ファイルを raw 走査する。KV ブロックの展開は行わない。 */
static void scan_file(const char *path, cand_t *c) {
    size_t size = 0;
    unsigned char *buf = read_entire(path, &size);
    if (!buf) return;
    if (size > 0 && size < (16u * 1024 * 1024)) {
        struct stat stt;
        unsigned long long mt = 0;
        if (stat(path, &stt) == 0) mt = (unsigned long long)stt.st_mtime;
        scan_json_objs(buf, size, path, mt, c);
    }
    free(buf);
}

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

char *nx_state_find_tabs_json(const char *path, const char *key_hint,
                              char **out_origin) {
    (void)key_hint;
    if (out_origin) *out_origin = NULL;
    struct stat stt;
    if (!path || stat(path, &stt) != 0) return NULL;

    cand_t c;
    memset(&c, 0, sizeof(c));

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
            scan_file(fp, &c);
        }
        closedir(d);
    } else {
        scan_file(path, &c);
    }

    if (out_origin) *out_origin = c.origin;
    else free(c.origin);
    return c.best;
}

/* ------------------------------------------------------------------ */
/* KV ストアのキー / seq / 生存                                        */
/*                                                                    */
/* LevelDB リーダと共に削除した。呼び側は 0 / NULL を「未対応」として   */
/* 扱う (推測で seq を進めたり、存在しないキーへ書いたりしない)。       */
/* ------------------------------------------------------------------ */

int nx_state_list_keys(const char *dir, const char *key_hint,
                       char ***out_keys, size_t **out_lens, int max) {
    (void)dir; (void)key_hint; (void)max;
    if (out_keys) *out_keys = NULL;
    if (out_lens) *out_lens = NULL;
    return 0;
}

unsigned long long nx_state_max_seq_bytes(const char *dir, const char *key,
                                          size_t klen) {
    (void)dir; (void)key; (void)klen;
    return 0;
}

unsigned long long nx_state_max_seq(const char *dir, const char *key_hint) {
    (void)dir; (void)key_hint;
    return 0;
}

char *nx_state_find_key(const char *dir, const char *key_hint,
                        size_t *out_key_len) {
    (void)dir; (void)key_hint;
    if (out_key_len) *out_key_len = 0;
    return NULL;
}

int nx_state_key_is_live(const char *dir, const char *key_hint) {
    (void)dir; (void)key_hint;
    return 0;
}
