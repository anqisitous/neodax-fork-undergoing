#include "nx_deepai.h"
#include "nx_json.h"
#include "nx_leveldb.h"
#include "nx_state.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* ================================================================== */
/* 小さなユーティリティ                                                */
/* ================================================================== */

static char *x_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void *mem_find(const void *hay, size_t haylen,
                      const void *needle, size_t nlen) {
    if (!hay || !needle || nlen == 0 || haylen < nlen) return NULL;
    const unsigned char *h = (const unsigned char *)hay;
    const unsigned char *e = h + (haylen - nlen) + 1;
    for (; h < e; h++)
        if (h[0] == ((const unsigned char *)needle)[0] &&
            memcmp(h, needle, nlen) == 0)
            return (void *)h;
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
    buf[rd] = '\0';
    if (out_size) *out_size = rd;
    return buf;
}

static unsigned le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* 伸長可能バッファ */
typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} sbuf_t;

static int sb_reserve(sbuf_t *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    char *np = (char *)realloc(b->p, cap);
    if (!np) return -1;
    b->p = np;
    b->cap = cap;
    return 0;
}

static void sb_add(sbuf_t *b, const char *s, size_t n) {
    if (!s || sb_reserve(b, n) != 0) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_adds(sbuf_t *b, const char *s) { if (s) sb_add(b, s, strlen(s)); }
static void sb_addc(sbuf_t *b, char c) { sb_add(b, &c, 1); }

/* ================================================================== */
/* asar                                                               */
/* ================================================================== */

static void asar_walk(const nx_json_t *files, const char *prefix,
                      nx_asar_t *a, int depth) {
    if (!files || files->type != NX_JSON_OBJ || depth > 64) return;
    for (size_t i = 0; i < files->n; i++) {
        const char *key = files->keys[i];
        const nx_json_t *child = files->items[i];
        if (!key || !child) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", prefix, key);

        const nx_json_t *sub = nx_json_get(child, "files");
        if (sub && sub->type == NX_JSON_OBJ) {
            asar_walk(sub, path, a, depth + 1);
            continue;
        }
        const nx_json_t *size = nx_json_get(child, "size");
        if (!size) continue;
        nx_asar_entry_t e;
        memset(&e, 0, sizeof(e));
        e.path = x_strdup(path);
        e.size = (size_t)nx_json_number(size, 0);
        // asar は offset を「文字列」で格納することがある ("82206")。
        // nx_json_number は数値型しか見ないので、文字列なら自前で変換する。
        {
            const nx_json_t *ov = nx_json_get(child, "offset");
            if (ov && ov->type == NX_JSON_STR)
                e.offset = (size_t)strtoull(nx_json_str(ov), NULL, 10);
            else
                e.offset = (size_t)nx_json_number(ov, 0);
        }
        nx_asar_entry_t *ne = (nx_asar_entry_t *)realloc(
            a->entries, (a->n_entries + 1) * sizeof(*ne));
        if (!ne) { free(e.path); return; }
        a->entries = ne;
        a->entries[a->n_entries++] = e;
    }
}

nx_asar_t *nx_asar_open(const char *path) {
    size_t size = 0;
    unsigned char *data = read_entire(path, &size);
    if (!data || size < 16) { free(data); return NULL; }

    if (le32(data) != 4) { free(data); return NULL; }
    size_t header_size = le32(data + 4);
    if (header_size == 0 || 8 + header_size > size) { free(data); return NULL; }

    const unsigned char *hb = data + 8;
    size_t str_len = le32(hb + 4);
    if (str_len == 0 || 8 + str_len > header_size) { free(data); return NULL; }

    const char *json = (const char *)(hb + 8);
    nx_json_t *root = nx_json_parse(json, str_len);
    if (!root) { free(data); return NULL; }

    const nx_json_t *files = nx_json_get(root, "files");
    if (!files || files->type != NX_JSON_OBJ) {
        nx_json_free(root);
        free(data);
        return NULL;
    }

    nx_asar_t *a = (nx_asar_t *)calloc(1, sizeof(*a));
    if (!a) { nx_json_free(root); free(data); return NULL; }
    a->path = x_strdup(path);
    a->data = data;
    a->size = size;
    a->data_base = 8 + header_size;

    asar_walk(files, "", a, 0);
    nx_json_free(root);
    return a;
}

void nx_asar_free(nx_asar_t *a) {
    if (!a) return;
    for (size_t i = 0; i < a->n_entries; i++) free(a->entries[i].path);
    free(a->entries);
    free(a->path);
    free(a->data);
    free(a);
}

const nx_asar_entry_t *nx_asar_find(const nx_asar_t *a, const char *substr) {
    if (!a || !substr) return NULL;
    for (size_t i = 0; i < a->n_entries; i++) {
        if (strstr(a->entries[i].path, substr)) return &a->entries[i];
    }
    return NULL;
}

char *nx_asar_read_text(const nx_asar_t *a, const nx_asar_entry_t *e) {
    if (!a || !e) return NULL;
    size_t off = a->data_base + e->offset;
    if (off > a->size || off + e->size > a->size) return NULL;
    char *buf = (char *)malloc(e->size + 1);
    if (!buf) return NULL;
    memcpy(buf, a->data + off, e->size);
    buf[e->size] = '\0';
    return buf;
}

/* ================================================================== */
/* workspace-tabs 状態モデル                                           */
/* ================================================================== */

typedef struct {
    char env[160];
    char thread[160];
} wx_ref_t;

typedef struct {
    wx_ref_t refs[512];
    int      n;
} wx_liveset_t;

static int wx_live_has(const wx_liveset_t *s, const char *env, const char *th) {
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->refs[i].env, env ? env : "") == 0 &&
            strcmp(s->refs[i].thread, th ? th : "") == 0)
            return 1;
    }
    return 0;
}

static void wx_live_add(wx_liveset_t *s, const nx_json_t *tr) {
    if (!tr || s->n >= (int)(sizeof(s->refs) / sizeof(s->refs[0]))) return;
    const char *env = nx_json_str(nx_json_get(tr, "environmentId"));
    const char *th  = nx_json_str(nx_json_get(tr, "threadId"));
    if (!env || !th) return;
    if (wx_live_has(s, env, th)) return;
    snprintf(s->refs[s->n].env, sizeof(s->refs[s->n].env), "%s", env);
    snprintf(s->refs[s->n].thread, sizeof(s->refs[s->n].thread), "%s", th);
    s->n++;
}

static int str_in(char **arr, int n, const char *s) {
    if (!s) return 0;
    for (int i = 0; i < n; i++)
        if (arr[i] && strcmp(arr[i], s) == 0) return 1;
    return 0;
}

static const nx_json_t *wx_state_node(const nx_json_t *root) {
    const nx_json_t *st = nx_json_get(root, "state");
    if (st && nx_json_get(st, "tabs")) return st;
    if (nx_json_get(root, "tabs")) return root;
    return NULL;
}

/* removed[] に含まれる id のタブだけを取り除いて JSON を再構築する。
   base (state の他フィールド / version / 他タブ) はそのまま保持する。 */
static int rebuild_state(const nx_json_t *root, const nx_json_t *state,
                         const nx_json_t *tabs, char **removed, int nrem,
                         char **out) {
    sbuf_t b; memset(&b, 0, sizeof(b));
    const char *raw;
    sb_adds(&b, "{\"state\":{\"tabs\":[");
    int first = 1;
    const nx_json_t *first_kept_id = NULL;
    for (size_t i = 0; i < tabs->n; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        const char *id = nx_json_str(nx_json_get(tab, "id"));
        if (str_in(removed, nrem, id)) continue;
        if (!first) sb_addc(&b, ',');
        first = 0;
        if (!first_kept_id) first_kept_id = nx_json_get(tab, "id");
        size_t n = nx_json_raw(tab, &raw);
        sb_add(&b, raw, n);
    }
    sb_adds(&b, "],\"activeId\":");
    const nx_json_t *active = nx_json_get(state, "activeId");
    const char *active_id = nx_json_str(active);
    if (str_in(removed, nrem, active_id)) {
        if (first_kept_id) {
            size_t n = nx_json_raw(first_kept_id, &raw);
            sb_add(&b, raw, n);
        } else {
            sb_adds(&b, "null");
        }
    } else if (active) {
        size_t n = nx_json_raw(active, &raw);
        sb_add(&b, raw, n);
    } else {
        sb_adds(&b, "null");
    }

    sb_adds(&b, ",\"recentIds\":[");
    const nx_json_t *recent = nx_json_get(state, "recentIds");
    if (recent && recent->type == NX_JSON_ARR) {
        int rf = 1;
        for (size_t i = 0; i < recent->n; i++) {
            const nx_json_t *r = nx_json_at(recent, i);
            const char *rid = nx_json_str(r);
            if (str_in(removed, nrem, rid)) continue;
            if (!rf) sb_addc(&b, ',');
            rf = 0;
            size_t n = nx_json_raw(r, &raw);
            sb_add(&b, raw, n);
        }
    }
    sb_adds(&b, "]}");
    const nx_json_t *ver = nx_json_get(root, "version");
    if (ver) {
        sb_adds(&b, ",\"version\":");
        size_t n = nx_json_raw(ver, &raw);
        sb_add(&b, raw, n);
    }
    sb_addc(&b, '}');
    if (out) *out = b.p; else free(b.p);
    return 0;
}

/* 指定した id のタブだけを除去する。 */
int nx_deepai_remove_tab_json(const char *json, size_t len, const char *tab_id,
                              char **out, int *n_removed, FILE *log) {
    if (out) *out = NULL;
    if (n_removed) *n_removed = 0;
    if (!json || !tab_id) return -1;
    nx_json_t *root = nx_json_parse(json, len);
    if (!root) return -1;
    const nx_json_t *state = wx_state_node(root);
    const nx_json_t *tabs = state ? nx_json_get(state, "tabs") : NULL;
    if (!tabs || tabs->type != NX_JSON_ARR) { nx_json_free(root); return -1; }

    char **removed = (char **)calloc(tabs->n + 1, sizeof(char *));
    int nrem = 0;
    if (!removed) { nx_json_free(root); return -1; }
    for (size_t i = 0; i < tabs->n; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        const char *id = nx_json_str(nx_json_get(tab, "id"));
        if (id && strcmp(id, tab_id) == 0) {
            removed[nrem++] = x_strdup(id);
            if (log) {
                const nx_json_t *tg = nx_json_get(tab, "target");
                fprintf(log, "    - remove selected tab: %s\n",
                        nx_json_str(nx_json_get(tg, "surfaceId")) ?
                        nx_json_str(nx_json_get(tg, "surfaceId")) : tab_id);
            }
        }
    }
    if (n_removed) *n_removed = nrem;

    int rc = rebuild_state(root, state, tabs, removed, nrem, out);
    for (int i = 0; i < nrem; i++) free(removed[i]);
    free(removed);
    nx_json_free(root);
    return rc;
}

/* 所有スレッドを失った surface タブ (閉じられないタブ) の id を集める。
   log が非 NULL なら各件を出力する。戻り値は件数。 */
static int collect_stuck_surfaces(const nx_json_t *tabs, char ***out_ids, FILE *log) {
    if (out_ids) *out_ids = NULL;
    if (!tabs || tabs->type != NX_JSON_ARR) return 0;

    /* 1) 生存スレッド (chat タブ) を収集 */
    wx_liveset_t lives; memset(&lives, 0, sizeof(lives));
    for (size_t i = 0; i < tabs->n; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        const nx_json_t *target = nx_json_get(tab, "target");
        const char *kind = nx_json_str(nx_json_get(target, "kind"));
        if (kind && strcmp(kind, "chat") == 0)
            wx_live_add(&lives, nx_json_get(target, "threadRef"));
    }

    /* 2) surface かつ所有スレッドが生存していないものを集める */
    char **ids = (char **)calloc(tabs->n + 1, sizeof(char *));
    int n = 0;
    if (!ids) return 0;
    for (size_t i = 0; i < tabs->n; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        const nx_json_t *target = nx_json_get(tab, "target");
        const char *kind = nx_json_str(nx_json_get(target, "kind"));
        if (!kind || strcmp(kind, "surface") != 0) continue;
        const nx_json_t *tr = nx_json_get(target, "threadRef");
        const char *env = nx_json_str(nx_json_get(tr, "environmentId"));
        const char *th  = nx_json_str(nx_json_get(tr, "threadId"));
        int live = (lives.n > 0) && wx_live_has(&lives, env, th);
        if (live) continue;
        const char *id = nx_json_str(nx_json_get(tab, "id"));
        ids[n++] = x_strdup(id ? id : "");
        if (log) {
            const char *sid = nx_json_str(nx_json_get(target, "surfaceId"));
            fprintf(log, "    - stuck surface tab: %s @ %s/%s\n",
                    sid ? sid : "?", env ? env : "?", th ? th : "?");
        }
    }
    if (out_ids) *out_ids = ids;
    else { for (int i = 0; i < n; i++) free(ids[i]); free(ids); }
    return n;
}

/* 閉じられない surface タブを検出して出力する (変更はしない)。 */
int nx_deepai_detect_json(const char *json, size_t len, FILE *out) {
    if (!out) out = stdout;
    if (!json) return -1;
    nx_json_t *root = nx_json_parse(json, len);
    if (!root) return -1;
    const nx_json_t *state = wx_state_node(root);
    const nx_json_t *tabs = state ? nx_json_get(state, "tabs") : NULL;
    if (!tabs || tabs->type != NX_JSON_ARR) { nx_json_free(root); return -1; }
    char **ids = NULL;
    int n = collect_stuck_surfaces(tabs, &ids, out);
    for (int i = 0; i < n; i++) free(ids[i]);
    free(ids);
    nx_json_free(root);
    return n;
}

/* 所有スレッドを失った surface タブを除去した JSON を生成。 */
int nx_deepai_fix_json(const char *json, size_t len,
                       char **out, int *n_removed, FILE *log) {
    if (out) *out = NULL;
    if (n_removed) *n_removed = 0;
    if (!json) return -1;

    nx_json_t *root = nx_json_parse(json, len);
    if (!root) return -1;
    const nx_json_t *state = wx_state_node(root);
    if (!state) { nx_json_free(root); return -1; }

    const nx_json_t *tabs = nx_json_get(state, "tabs");
    if (!tabs || tabs->type != NX_JSON_ARR) { nx_json_free(root); return -1; }

    char **removed = NULL;
    int nrem = collect_stuck_surfaces(tabs, &removed, log);
    if (n_removed) *n_removed = nrem;

    /* 再構築 (base/component は保持し、tabs 配列のみ編集) */
    int rc = rebuild_state(root, state, tabs, removed, nrem, out);

    for (int i = 0; i < nrem; i++) free(removed[i]);
    free(removed);
    nx_json_free(root);
    return rc;
}

/* ================================================================== */
/* leveldb からの状態抽出                                              */
/* ================================================================== */

/* バイト列中から {\"state\":...} を探して、JSON として妥当なものを返す。
   複数あれば最後のものを採用 (後の書き込みほど新しい)。 */
static char *find_state_json(const unsigned char *b, size_t n) {
    char *best = NULL;
    for (size_t i = 0; i + 10 < n; i++) {
        if (b[i] != '{' || memcmp(b + i, "{\"state\":", 9) != 0) continue;
        int depth = 0, in_str = 0, esc = 0, ok = 0;
        size_t k = i;
        for (; k < n; k++) {
            char c = (char)b[k];
            if (in_str) {
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') in_str = 0;
                continue;
            }
            if (c == '"') { in_str = 1; continue; }
            if (c == '{') depth++;
            else if (c == '}') { depth--; if (depth == 0) { ok = 1; k++; break; } }
        }
        if (!ok) continue;
        size_t len = k - i;
        char *cand = (char *)malloc(len + 1);
        if (!cand) break;
        memcpy(cand, b + i, len);
        cand[len] = '\0';
        nx_json_t *t = nx_json_parse(cand, len);
        if (t) {
            const nx_json_t *st = wx_state_node(t);
            if (st && nx_json_get(st, "tabs")) {
                free(best);
                best = cand;
                cand = NULL;
            }
            nx_json_free(t);
        }
        free(cand);
        i = k - 1;
    }
    return best;
}

static char *extract_state_blob(const unsigned char *b, size_t n) {
    return find_state_json(b, n);
}

/* ---- leveldb (.ldb/.log) 経由の読み出し ---- */
typedef struct {
    unsigned long long best_seq;
    char              *best;
} ldb_collect_t;

static void ldb_collect_cb(const char *key, size_t klen,
                           const unsigned char *val, size_t vlen,
                           unsigned long long seq, void *ud) {
    ldb_collect_t *c = (ldb_collect_t *)ud;
    if (!key || !val || vlen == 0) return;
    if (!mem_find(key, klen, "workspace-tabs:v1", 17)) return;
    char *j = find_state_json(val, vlen);
    if (!j) return;
    if (seq >= c->best_seq) {
        free(c->best);
        c->best = j;
        c->best_seq = seq;
    } else {
        free(j);
    }
}

static int has_suffix(const char *s, const char *suf);

static char *extract_state_leveldb(const char *path, int is_dir) {
    ldb_collect_t c;
    memset(&c, 0, sizeof(c));
    if (!is_dir) {
        nx_leveldb_scan_file(path, ldb_collect_cb, &c);
        return c.best;
    }
    DIR *d = opendir(path);
    if (!d) return NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_suffix(ent->d_name, ".log") && !has_suffix(ent->d_name, ".ldb"))
            continue;
        char fp[4096];
        snprintf(fp, sizeof(fp), "%s/%s", path, ent->d_name);
        nx_leveldb_scan_file(fp, ldb_collect_cb, &c);
    }
    closedir(d);
    return c.best;
}

static int has_suffix(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

/* ディレクトリ内の leveldb から最新らしき状態を得る */
static char *extract_state_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return NULL;
    char *best = NULL;
    time_t best_mtime = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_suffix(ent->d_name, ".log") && !has_suffix(ent->d_name, ".ldb"))
            continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat stt;
        if (stat(path, &stt) != 0) continue;
        size_t sz = 0;
        unsigned char *blob = read_entire(path, &sz);
        if (!blob) continue;
        char *cand = extract_state_blob(blob, sz);
        free(blob);
        if (cand && stt.st_mtime >= best_mtime) {
            free(best);
            best = cand;
            best_mtime = stt.st_mtime;
        } else {
            free(cand);
        }
    }
    closedir(d);
    return best;
}

char *nx_deepai_load_state(const char *path) {
    /* 汎用ロケータ: 形式 (LevelDB/JSON/任意バイナリ) を問わずタブ状 JSON を探す */
    char *found = nx_state_find_tabs_json(path, "workspace-tabs", NULL);
    if (found) return found;
    /* fallback: 旧来の個別抽出 */
    struct stat stt;
    if (!path || stat(path, &stt) != 0) return NULL;
    if (S_ISDIR(stt.st_mode)) {
        char *s = extract_state_leveldb(path, 1);
        return s ? s : extract_state_dir(path);
    }
    const char *dot = strrchr(path, '.');
    if (dot && (strcmp(dot, ".ldb") == 0 || strcmp(dot, ".log") == 0)) {
        char *s = extract_state_leveldb(path, 0);
        if (s) return s;
    }
    size_t sz = 0;
    unsigned char *blob = read_entire(path, &sz);
    if (!blob) return NULL;
    char *state = NULL;
    nx_json_t *t = nx_json_parse((const char *)blob, sz);
    if (t && wx_state_node(t)) {
        state = (char *)blob;
        blob = NULL;
    }
    if (t) nx_json_free(t);
    if (!state) state = extract_state_blob(blob, sz);
    free(blob);
    return state;
}

int nx_deepai_fix_path(const char *path, FILE *out) {
    if (!out) out = stdout;
    char *state = nx_deepai_load_state(path);
    if (!state) {
        fprintf(out, "  [fix] no workspace-tabs state found in: %s\n", path);
        return -1;
    }

    fprintf(out, "\n  [workspace-tabs] source: %s\n", path);
    /* 現在のタブ一覧を表示 */
    nx_json_t *root = nx_json_parse(state, strlen(state));
    const nx_json_t *st = root ? wx_state_node(root) : NULL;
    const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : NULL;
    if (tabs) {
        fprintf(out, "  displayed tabs (%zu):\n", tabs->n);
        for (size_t i = 0; i < tabs->n; i++) {
            const nx_json_t *tab = nx_json_at(tabs, i);
            const nx_json_t *tg = nx_json_get(tab, "target");
            const char *kind = nx_json_str(nx_json_get(tg, "kind"));
            const char *sid = nx_json_str(nx_json_get(tg, "surfaceId"));
            const nx_json_t *tr = nx_json_get(tg, "threadRef");
            const char *env = nx_json_str(nx_json_get(tr, "environmentId"));
            const char *th = nx_json_str(nx_json_get(tr, "threadId"));
            fprintf(out, "    [%zu] kind=%-8s %s%s%s%s%s\n", i,
                    kind ? kind : "?",
                    sid ? sid : "",
                    env ? " @ " : "", env ? env : "",
                    th ? "/" : "", th ? th : "");
        }
    }
    if (root) nx_json_free(root);

    char *fixed = NULL;
    int nrem = 0;
    if (nx_deepai_fix_json(state, strlen(state), &fixed, &nrem, out) != 0) {
        fprintf(out, "  [fix] failed to rebuild state\n");
        free(state);
        return -1;
    }
    fprintf(out, "  removed stale surface tabs: %d\n", nrem);

    char fixed_path[4200];
    snprintf(fixed_path, sizeof(fixed_path), "%s.fixed.json", path);
    FILE *fp = fopen(fixed_path, "wb");
    if (fp) {
        if (fixed) fwrite(fixed, 1, strlen(fixed), fp);
        fclose(fp);
        fprintf(out, "  fixed state written: %s\n", fixed_path);
        fprintf(out, "  apply: quit DeepAI Code, then replace the leveldb value "
                     "for key 'deepai-code:workspace-tabs:v1' with this JSON "
                     "(or run: localStorage.setItem('deepai-code:workspace-tabs:v1', <json>) "
                     "in DevTools and reload).\n");
    }

    free(fixed);
    free(state);
    return 0;
}

int nx_deepai_remove_tab_path(const char *path, const char *tab_id, FILE *out) {
    if (!out) out = stdout;
    char *state = nx_deepai_load_state(path);
    if (!state) {
        fprintf(out, "  [close-tab] no workspace-tabs state found in: %s\n", path);
        return -1;
    }
    char *fixed = NULL;
    int nrem = 0;
    if (nx_deepai_remove_tab_json(state, strlen(state), tab_id, &fixed, &nrem, out) != 0) {
        fprintf(out, "  [close-tab] failed to rebuild state\n");
        free(state);
        return -1;
    }
    fprintf(out, "  [close-tab] removed=%d (base components preserved)\n", nrem);
    char fixed_path[4200];
    snprintf(fixed_path, sizeof(fixed_path), "%s.fixed.json", path);
    FILE *fp = fopen(fixed_path, "wb");
    if (fp) {
        if (fixed) fwrite(fixed, 1, strlen(fixed), fp);
        fclose(fp);
        fprintf(out, "  fixed state written: %s\n", fixed_path);
    }
    free(fixed);
    free(state);
    return nrem > 0 ? 0 : 1;
}

int nx_deepai_dump_state(const char *json, FILE *out) {
    if (!out) out = stdout;
    if (!json) return -1;
    nx_json_t *root = nx_json_parse(json, strlen(json));
    if (!root) { fprintf(out, "  (invalid json)\n"); return -1; }
    const nx_json_t *st = wx_state_node(root);
    const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : NULL;
    const char *active = st ? nx_json_str(nx_json_get(st, "activeId")) : NULL;
    if (!tabs) { fprintf(out, "  (no tabs)\n"); nx_json_free(root); return -1; }

    fprintf(out, "IDX\tACTIVE\tKIND\tSURFACE\tOWNER_ENV\tOWNER_THREAD\tID\n");
    for (size_t i = 0; i < tabs->n; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        const nx_json_t *tg = nx_json_get(tab, "target");
        const char *id = nx_json_str(nx_json_get(tab, "id"));
        const char *kind = nx_json_str(nx_json_get(tg, "kind"));
        const char *sid = nx_json_str(nx_json_get(tg, "surfaceId"));
        const nx_json_t *tr = nx_json_get(tg, "threadRef");
        const char *env = nx_json_str(nx_json_get(tr, "environmentId"));
        const char *th = nx_json_str(nx_json_get(tr, "threadId"));
        fprintf(out, "%zu\t%s\t%s\t%s\t%s\t%s\t%s\n", i,
                (active && id && strcmp(active, id) == 0) ? "yes" : "no",
                kind ? kind : "", sid ? sid : "",
                env ? env : "", th ? th : "", id ? id : "");
    }
    nx_json_free(root);
    return 0;
}

/* ================================================================== */
/* asar 実行時挙動の解析                                               */
/* ================================================================== */

static int contains(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

int nx_deepai_analyze_asar(const char *asar_path, const char *tabs_json, FILE *out) {
    if (!out) out = stdout;
    nx_asar_t *a = nx_asar_open(asar_path);
    if (!a) {
        fprintf(out, "  [asar] not an asar archive: %s\n", asar_path);
        return -1;
    }

    fprintf(out, "\n  [asar] %s\n", asar_path);
    fprintf(out, "    entries: %zu  data-base: 0x%zx\n", a->n_entries, a->data_base);

    const nx_asar_entry_t *tabs_e = nx_asar_find(a, "WorkspaceTabs-");
    const nx_asar_entry_t *store_e = nx_asar_find(a, "WorkspaceTabStore-");
    fprintf(out, "    WorkspaceTabs      : %s\n", tabs_e ? tabs_e->path : "(not found)");
    fprintf(out, "    WorkspaceTabStore  : %s\n", store_e ? store_e->path : "(not found)");

    char *tabs_js = tabs_e ? nx_asar_read_text(a, tabs_e) : NULL;
    char *store_js = store_e ? nx_asar_read_text(a, store_e) : NULL;

    fprintf(out, "\n    [detected close-path behavior]\n");
    fprintf(out, "      surface tab X -> pe(tabId)\n");
    if (contains(tabs_js, "onCloseSurface") && contains(tabs_js, "resourceRequest"))
        fprintf(out, "      -> de(tab,'close') fires onCloseSurface via resourceRequest  [present]\n");
    if (contains(tabs_js, "Open the owning chat first"))
        fprintf(out, "      -> guard: owner thread != current thread => warning, ABORT  [present]\n");
    if (contains(tabs_js, "byThreadKey"))
        fprintf(out, "      -> surfaceTabs aggregates ALL threads (byThreadKey)        [present]\n");
    if (contains(tabs_js, "This tab is no longer available"))
        fprintf(out, "      -> fallback 'tab no longer available' close                 [present]\n");
    if (contains(store_js, "workspace-tabs"))
        fprintf(out, "      -> state persisted at key 'deepai-code:workspace-tabs:v1'   [present]\n");

    free(tabs_js);
    free(store_js);

    if (tabs_json) {
        nx_json_t *root = nx_json_parse(tabs_json, strlen(tabs_json));
        const nx_json_t *st = root ? wx_state_node(root) : NULL;
        const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : NULL;
        if (!tabs) {
            fprintf(out, "    [state] tabs_json did not contain a tabs array\n");
        } else {
            /* 現在スレッド = activeId が chat の threadRef */
            wx_liveset_t lives; memset(&lives, 0, sizeof(lives));
            const char *active_id = nx_json_str(nx_json_get(st, "activeId"));
            for (size_t i = 0; i < tabs->n; i++) {
                const nx_json_t *tab = nx_json_at(tabs, i);
                const nx_json_t *target = nx_json_get(tab, "target");
                const char *kind = nx_json_str(nx_json_get(target, "kind"));
                if (kind && strcmp(kind, "chat") == 0)
                    wx_live_add(&lives, nx_json_get(target, "threadRef"));
            }
            fprintf(out, "\n    [runtime state simulation] activeId=%s\n",
                    active_id ? active_id : "?");
            for (size_t i = 0; i < tabs->n; i++) {
                const nx_json_t *tab = nx_json_at(tabs, i);
                const nx_json_t *target = nx_json_get(tab, "target");
                const char *kind = nx_json_str(nx_json_get(target, "kind"));
                const char *sid = nx_json_str(nx_json_get(target, "surfaceId"));
                const nx_json_t *tr = nx_json_get(target, "threadRef");
                const char *env = nx_json_str(nx_json_get(tr, "environmentId"));
                const char *th  = nx_json_str(nx_json_get(tr, "threadId"));
                if (!kind) continue;
                if (strcmp(kind, "surface") == 0) {
                    int closable = (lives.n > 0) && wx_live_has(&lives, env, th);
                    fprintf(out, "      surface %-28s owner=%s/%s -> %s\n",
                            sid ? sid : "?", env ? env : "?", th ? th : "",
                            closable ? "closable" : "STUCK (close aborts; open owning chat first)");
                }
            }
        }
        if (root) nx_json_free(root);
    }

    nx_asar_free(a);
    return 0;
}

/* ================================================================== */
/* C/C++/ObjC++ テキストの実挙動解析                                   */
/* ================================================================== */

static int line_has_word(const char *line, const char *w) {
    return strstr(line, w) != NULL;
}

int nx_deepai_analyze_source(const char *src_path, FILE *out) {
    if (!out) out = stdout;
    size_t sz = 0;
    unsigned char *buf = read_entire(src_path, &sz);
    if (!buf) {
        fprintf(out, "  [src] cannot read: %s\n", src_path);
        return -1;
    }

    size_t n_interfaces = 0, n_impls = 0, n_objc_methods = 0;
    size_t n_cfuncs = 0, n_bridge = 0, n_nswindow = 0, n_nstable = 0;
    size_t n_lines = 1;
    char first_interfaces[8][128];
    size_t n_if_names = 0;
    char *p = (char *)buf;
    size_t line_no = 1;

    while (*p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        size_t cp = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, cp);
        line[cp] = '\0';

        if (strncmp(line, "@interface", 10) == 0) {
            n_interfaces++;
            if (n_if_names < 8) {
                const char *q = line + 10;
                while (*q == ' ') q++;
                size_t k = 0;
                while (q[k] && q[k] != ' ' && q[k] != '(' && q[k] != '\n' &&
                       k < sizeof(first_interfaces[0]) - 1) {
                    first_interfaces[n_if_names][k] = q[k];
                    k++;
                }
                first_interfaces[n_if_names][k] = '\0';
                n_if_names++;
            }
        } else if (strncmp(line, "@implementation", 15) == 0) {
            n_impls++;
        } else if ((line[0] == '-' || line[0] == '+') && line[1] == ' ' &&
                   strchr(line, ']') && line[2] == '(') {
            n_objc_methods++;
        } else if (strstr(line, "neodax_bridge_")) {
            n_bridge++;
        }
        if (line_has_word(line, "NSWindow")) n_nswindow++;
        if (line_has_word(line, "NSTableView")) n_nstable++;

        /* C/C++ 関数定義ヒューリスティック */
        if (line[0] != ' ' && line[0] != '\t' && line[0] != '/' &&
            line[0] != '#' && line[0] != '*' && line[0] != '}' &&
            strchr(line, '(') && strchr(line, ')') && strchr(line, '{') &&
            !strstr(line, "if ") && !strstr(line, "for ") &&
            !strstr(line, "while ") && !strstr(line, "switch ") &&
            !strstr(line, "NSWindow") && !strstr(line, "@")) {
            n_cfuncs++;
        }

        if (nl) { p = nl + 1; line_no++; n_lines++; } else break;
    }

    fprintf(out, "\n  [source behavior] %s\n", src_path);
    fprintf(out, "    lines              : %zu\n", n_lines);
    fprintf(out, "    @interface         : %zu", n_interfaces);
    for (size_t i = 0; i < n_if_names; i++)
        fprintf(out, " %s", first_interfaces[i]);
    fprintf(out, "\n");
    fprintf(out, "    @implementation    : %zu\n", n_impls);
    fprintf(out, "    ObjC methods       : %zu\n", n_objc_methods);
    fprintf(out, "    C/C++ functions    : %zu\n", n_cfuncs);
    fprintf(out, "    NSWindow refs      : %zu\n", n_nswindow);
    fprintf(out, "    NSTableView refs   : %zu\n", n_nstable);
    fprintf(out, "    neodax_bridge_*    : %zu\n", n_bridge);

    fprintf(out, "\n    [actual execution model]\n");
    if (n_nswindow)
        fprintf(out, "      1. AppKit: NSWindow を生成 (titled/closable/resizable)\n");
    if (n_nstable)
        fprintf(out, "      2. NSTableView を dataSource/delegate として登録\n");
    if (n_bridge)
        fprintf(out, "      3. バイナリ選択時: neodax_bridge_* を呼び、解析結果をテーブルへ反映\n");
    if (n_objc_methods)
        fprintf(out, "      4. ObjC メソッド %zu 個が UI アクションを処理\n", n_objc_methods);
    if (!n_nswindow && !n_nstable && !n_bridge)
        fprintf(out, "      (UI/ブリッジ呼び出しは検出されず)\n");

    (void)line_no;
    free(buf);
    return 0;
}

/* ================================================================== */
/* 画面の再現レイアウト (キャプチャ無し)                               */
/* ================================================================== */

typedef struct {
    int  chat;
    int  is_active;
    char env[160];
    char th[160];
} c_owner_t;

static void add_node(nx_ui_layout_t *l, int x, int y, int w, int h, int kind,
                     const char *label) {
    nx_ui_node_t *nn = (nx_ui_node_t *)realloc(l->nodes,
                                               (l->n_nodes + 1) * sizeof(*nn));
    if (!nn) return;
    l->nodes = nn;
    nx_ui_node_t *n = &l->nodes[l->n_nodes++];
    n->x = x; n->y = y; n->w = w; n->h = h; n->kind = kind;
    snprintf(n->label, sizeof(n->label), "%s", label ? label : "");
}

static void add_edge(nx_ui_layout_t *l, int x1, int y1, int x2, int y2, int a, int b) {
    nx_ui_edge_t *ne = (nx_ui_edge_t *)realloc(l->edges,
                                               (l->n_edges + 1) * sizeof(*ne));
    if (!ne) return;
    l->edges = ne;
    nx_ui_edge_t *e = &l->edges[l->n_edges++];
    e->x1 = x1; e->y1 = y1; e->x2 = x2; e->y2 = y2; e->a = a; e->b = b;
}

int nx_deepai_build_layout(const char *json, nx_ui_layout_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->active_tab = -1;
    out->width = 1200;
    out->height = 780;
    if (!json) return -1;

    nx_json_t *root = nx_json_parse(json, strlen(json));
    if (!root) return -1;
    const nx_json_t *st = wx_state_node(root);
    if (st) {
        const nx_json_t *tabs = nx_json_get(st, "tabs");

        /* タブの所有スレッド集合と active を求める */
        const char *active = nx_json_str(nx_json_get(st, "activeId"));
        c_owner_t owners[64];
        int now = 0;
        if (tabs) {
            for (size_t i = 0; i < tabs->n && now < 64; i++) {
                const nx_json_t *tab = nx_json_at(tabs, i);
                const nx_json_t *tg = nx_json_get(tab, "target");
                const char *kind = nx_json_str(nx_json_get(tg, "kind"));
                const nx_json_t *tr = nx_json_get(tg, "threadRef");
                const char *env = nx_json_str(nx_json_get(tr, "environmentId"));
                const char *th = nx_json_str(nx_json_get(tr, "threadId"));
                owners[now].chat = (kind && strcmp(kind, "chat") == 0);
                snprintf(owners[now].env, sizeof(owners[now].env), "%s", env ? env : "");
                snprintf(owners[now].th, sizeof(owners[now].th), "%s", th ? th : "");
                owners[now].is_active =
                    (active && nx_json_str(nx_json_get(tab, "id")) &&
                     strcmp(active, nx_json_str(nx_json_get(tab, "id"))) == 0);
                now++;
            }
        }
        int active_chat = -1;
        for (int i = 0; i < now; i++)
            if (owners[i].is_active && owners[i].chat) active_chat = i;

        /* タイトルバー */
        snprintf(out->title, sizeof(out->title), "app-state replay [profile: deepai-workspace-tabs]");

        /* タブ帯 */
        int tx = 12;
        const int ty = 690, tw = 92, th = 30;
        if (tabs) {
            for (size_t i = 0; i < tabs->n; i++) {
                const nx_json_t *tab = nx_json_at(tabs, i);
                const nx_json_t *tg = nx_json_get(tab, "target");
                const char *kind = nx_json_str(nx_json_get(tg, "kind"));
                const char *sid = nx_json_str(nx_json_get(tg, "surfaceId"));
                const char *id = nx_json_str(nx_json_get(tab, "id"));
                int is_active = (active && id && strcmp(active, id) == 0);
                int stuck = 0;
                if (kind && strcmp(kind, "surface") == 0) {
                    /* 所有 chat が active でなければ STUCK */
                    int found = 0;
                    for (int k = 0; k < now; k++) {
                        if (!owners[k].chat) continue;
                        const nx_json_t *ttab = nx_json_at(tabs, k);
                        const nx_json_t *ttg = nx_json_get(ttab, "target");
                        const nx_json_t *ttr = nx_json_get(ttg, "threadRef");
                        const char *e2 = nx_json_str(nx_json_get(ttr, "environmentId"));
                        const char *t2 = nx_json_str(nx_json_get(ttr, "threadId"));
                        const nx_json_t *otr = nx_json_get(tg, "threadRef");
                        const char *oe = nx_json_str(nx_json_get(otr, "environmentId"));
                        const char *ot = nx_json_str(nx_json_get(otr, "threadId"));
                        if (e2 && t2 && oe && ot && strcmp(e2, oe) == 0 && strcmp(t2, ot) == 0)
                            found = 1;
                    }
                    if (!found) stuck = 1;
                }
                char lbl[96];
                if (kind && strcmp(kind, "surface") == 0)
                    snprintf(lbl, sizeof(lbl), "%s", sid ? sid : "surface");
                else if (kind && strcmp(kind, "chat") == 0)
                    snprintf(lbl, sizeof(lbl), "chat");
                else if (kind && strcmp(kind, "draft") == 0)
                    snprintf(lbl, sizeof(lbl), "New chat");
                else
                    snprintf(lbl, sizeof(lbl), "%s", kind ? kind : "?");
                int kind_code = stuck ? 4 : (is_active ? 2 : 3);
                add_node(out, tx, ty, tw, th, kind_code, lbl);
                out->tab_index[out->n_tabs++] = out->n_nodes - 1;
                if (is_active) out->active_tab = out->n_tabs - 1;
                if (stuck) out->stuck_tab[out->n_stuck++] = out->n_nodes - 1;
                tx += tw + 6;
            }
        }
        (void)active_chat;

        /* メイン領域 */
        add_node(out, 12, 400, 760, 278, 6, "content");
        add_node(out, 12, 60, 760, 330, 5, "conversation");
        add_node(out, 786, 60, 402, 618, 5, "inspector");
        /* content をタブに関連づける (選択中タブ→content) */
        if (out->active_tab >= 0) {
            nx_ui_node_t *t = &out->nodes[out->tab_index[out->active_tab]];
            add_edge(out, t->x + t->w / 2, ty, 12 + 380, 400, out->tab_index[out->active_tab], out->n_nodes - 3);
        }
        /* STUCK タブから警告ノードへのエッジ */
        for (int i = 0; i < out->n_stuck; i++) {
            nx_ui_node_t *t = &out->nodes[out->stuck_tab[i]];
            add_edge(out, t->x + t->w / 2, ty, 786 + 201, 60, out->stuck_tab[i], -1);
        }
    }
    nx_json_free(root);
    return 0;
}

void nx_deepai_free_layout(nx_ui_layout_t *l) {
    if (!l) return;
    free(l->nodes);
    free(l->edges);
    memset(l, 0, sizeof(*l));
}

static const char *kind_name(int k) {
    switch (k) {
        case 0: return "input";
        case 1: return "button";
        case 2: return "tab(active)";
        case 3: return "tab";
        case 4: return "tab(STUCK)";
        case 5: return "table";
        default: return "text";
    }
}

int nx_deepai_render_ascii(const nx_ui_layout_t *l, FILE *out) {
    if (!out || !l) return -1;
    /* グリッド作成 */
    enum { W = 96, H = 34 };
    char grid[H][W + 1];
    for (int y = 0; y < H; y++) {
        memset(grid[y], ' ', W);
        grid[y][W] = '\0';
    }
    /* スケール */
    double sx = (double)(W - 2) / l->width;
    double sy = (double)(H - 2) / l->height;
    for (int i = 0; i < l->n_nodes; i++) {
        const nx_ui_node_t *n = &l->nodes[i];
        int x0 = (int)(n->x * sx) + 1;
        int y0 = (int)((l->height - (n->y + n->h)) * sy) + 1;
        int w = (int)(n->w * sx);
        int h = (int)(n->h * sy);
        if (w < 2) w = 2;
        if (h < 1) h = 1;
        char ch = ' ';
        char corner = '+';
        switch (n->kind) {
            case 0: ch = '.'; break;
            case 1: ch = '#'; break;
            case 2: ch = '='; break;
            case 3: ch = '-'; break;
            case 4: ch = '!'; break;
            case 5: ch = ':'; break;
            default: ch = '\''; break;
        }
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
                int gx = x0 + xx, gy = y0 + yy;
                if (gx < 0 || gx >= W || gy < 0 || gy >= H) continue;
                int border = (xx == 0 || yy == 0 || xx == w - 1 || yy == h - 1);
                grid[gy][gx] = border ? corner : ch;
            }
        /* ラベル */
        int lx = x0 + 1, ly = y0 + h / 2;
        if (ly >= 0 && ly < H) {
            for (int k = 0; n->label[k] && lx + k < W - 1; k++)
                grid[ly][lx + k] = n->label[k];
        }
    }
    fprintf(out, "\n");
    fprintf(out, "  %s\n", l->title);
    fprintf(out, "  +");
    for (int i = 0; i < W; i++) putc('-', out);
    fprintf(out, "+\n");
    for (int y = 0; y < H; y++) {
        fprintf(out, "  |");
        fputs(grid[y], out);
        fprintf(out, "|\n");
    }
    fprintf(out, "  +");
    for (int i = 0; i < W; i++) putc('-', out);
    fprintf(out, "+\n");

    fprintf(out, "\n  legend: input=.  button=#  tab(active)==  tab=-  tab(STUCK)=!  table=:\n");
    fprintf(out, "\n  widgets (%d), edges (%d)\n", l->n_nodes, l->n_edges);
    for (int i = 0; i < l->n_nodes; i++) {
        const nx_ui_node_t *n = &l->nodes[i];
        fprintf(out, "    [%2d] %-12s @(%4d,%4d %3dx%3d) %s\n",
                i, kind_name(n->kind), n->x, n->y, n->w, n->h, n->label);
    }
    for (int i = 0; i < l->n_edges; i++) {
        const nx_ui_edge_t *e = &l->edges[i];
        fprintf(out, "    edge (%d,%d)-(%d,%d)%s\n", e->x1, e->y1, e->x2, e->y2,
                e->b < 0 ? "  [stuck-warning]" : "");
    }
    if (l->n_stuck)
        fprintf(out, "\n  %d stuck tab(s): they cannot be closed with the visible UI.\n",
                l->n_stuck);
    return 0;
}
