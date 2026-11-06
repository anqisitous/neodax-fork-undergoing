#include "nx_json.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
} jctx_t;

static nx_json_t *j_new(nx_json_type_t t) {
    nx_json_t *v = (nx_json_t *)calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

static void j_skip(jctx_t *c) {
    while (c->pos < c->len) {
        char ch = c->src[c->pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            c->pos++;
        } else {
            break;
        }
    }
}

static nx_json_t *j_value(jctx_t *c);

static nx_json_t *j_string(jctx_t *c) {
    if (c->pos >= c->len || c->src[c->pos] != '"') return NULL;
    size_t begin = c->pos;
    c->pos++; /* skip " */
    size_t cap = 32, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    while (c->pos < c->len) {
        char ch = c->src[c->pos++];
        if (ch == '"') {
            buf[n] = '\0';
            nx_json_t *v = j_new(NX_JSON_STR);
            if (!v) { free(buf); return NULL; }
            v->src = c->src;
            v->raw_begin = begin;
            v->raw_end = c->pos;
            v->str = buf;
            v->str_len = n;
            return v;
        }
        if (ch == '\\') {
            if (c->pos >= c->len) break;
            char e = c->src[c->pos++];
            switch (e) {
                case '"':  ch = '"';  break;
                case '\\': ch = '\\'; break;
                case '/':  ch = '/';  break;
                case 'b':  ch = '\b'; break;
                case 'f':  ch = '\f'; break;
                case 'n':  ch = '\n'; break;
                case 'r':  ch = '\r'; break;
                case 't':  ch = '\t'; break;
                case 'u': {
                    /* \uXXXX -> UTF-8 (BMP のみ) */
                    if (c->pos + 4 > c->len) { c->pos = c->len; break; }
                    char hex[5];
                    memcpy(hex, c->src + c->pos, 4);
                    hex[4] = '\0';
                    c->pos += 4;
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    if (cp < 0x80) {
                        ch = (char)cp;
                    } else if (cp < 0x800) {
                        if (n + 2 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
                        buf[n++] = (char)(0xC0 | (cp >> 6));
                        ch = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (n + 3 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
                        buf[n++] = (char)(0xE0 | (cp >> 12));
                        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        ch = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: ch = e; break;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[n++] = ch;
    }
    free(buf);
    return NULL;
}

static nx_json_t *j_number(jctx_t *c) {
    size_t begin = c->pos;
    if (c->pos < c->len && (c->src[c->pos] == '-' || c->src[c->pos] == '+'))
        c->pos++;
    while (c->pos < c->len) {
        char ch = c->src[c->pos];
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' ||
            ch == '+' || ch == '-') {
            c->pos++;
        } else {
            break;
        }
    }
    if (c->pos == begin) return NULL;
    char tmp[64];
    size_t n = c->pos - begin;
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, c->src + begin, n);
    tmp[n] = '\0';
    nx_json_t *v = j_new(NX_JSON_NUM);
    if (!v) return NULL;
    v->src = c->src;
    v->raw_begin = begin;
    v->raw_end = c->pos;
    v->num = strtod(tmp, NULL);
    return v;
}

static int j_push(nx_json_t *v, char *key, size_t key_len, nx_json_t *item) {
    size_t n = v->n;
    nx_json_t **ni = (nx_json_t **)realloc(v->items, (n + 1) * sizeof(*ni));
    if (!ni) return -1;
    v->items = ni;
    v->items[n] = item;
    if (v->type == NX_JSON_OBJ) {
        char **nk = (char **)realloc(v->keys, (n + 1) * sizeof(*nk));
        size_t *nl = (size_t *)realloc(v->key_lens, (n + 1) * sizeof(*nl));
        if (!nk || !nl) return -1;
        v->keys = nk;
        v->key_lens = nl;
        v->keys[n] = key;
        v->key_lens[n] = key_len;
    }
    v->n = n + 1;
    return 0;
}

static nx_json_t *j_array(jctx_t *c) {
    size_t begin = c->pos;
    c->pos++; /* [ */
    nx_json_t *v = j_new(NX_JSON_ARR);
    if (!v) return NULL;
    j_skip(c);
    if (c->pos < c->len && c->src[c->pos] == ']') {
        c->pos++;
        v->src = c->src; v->raw_begin = begin; v->raw_end = c->pos;
        return v;
    }
    for (;;) {
        j_skip(c);
        nx_json_t *item = j_value(c);
        if (!item) { nx_json_free(v); return NULL; }
        if (j_push(v, NULL, 0, item) != 0) { nx_json_free(item); nx_json_free(v); return NULL; }
        j_skip(c);
        if (c->pos >= c->len) { nx_json_free(v); return NULL; }
        if (c->src[c->pos] == ',') { c->pos++; continue; }
        if (c->src[c->pos] == ']') { c->pos++; break; }
        nx_json_free(v);
        return NULL;
    }
    v->src = c->src; v->raw_begin = begin; v->raw_end = c->pos;
    return v;
}

static nx_json_t *j_object(jctx_t *c) {
    size_t begin = c->pos;
    c->pos++; /* { */
    nx_json_t *v = j_new(NX_JSON_OBJ);
    if (!v) return NULL;
    j_skip(c);
    if (c->pos < c->len && c->src[c->pos] == '}') {
        c->pos++;
        v->src = c->src; v->raw_begin = begin; v->raw_end = c->pos;
        return v;
    }
    for (;;) {
        j_skip(c);
        nx_json_t *key = j_string(c);
        if (!key) { nx_json_free(v); return NULL; }
        j_skip(c);
        if (c->pos >= c->len || c->src[c->pos] != ':') { nx_json_free(key); nx_json_free(v); return NULL; }
        c->pos++;
        j_skip(c);
        nx_json_t *val = j_value(c);
        if (!val) { nx_json_free(key); nx_json_free(v); return NULL; }
        char *kbuf = key->str;
        size_t klen = key->str_len;
        key->str = NULL; /* ownership moves to v */
        nx_json_free(key);
        if (j_push(v, kbuf, klen, val) != 0) { free(kbuf); nx_json_free(val); nx_json_free(v); return NULL; }
        j_skip(c);
        if (c->pos >= c->len) { nx_json_free(v); return NULL; }
        if (c->src[c->pos] == ',') { c->pos++; continue; }
        if (c->src[c->pos] == '}') { c->pos++; break; }
        nx_json_free(v);
        return NULL;
    }
    v->src = c->src; v->raw_begin = begin; v->raw_end = c->pos;
    return v;
}

static nx_json_t *j_literal(jctx_t *c) {
    size_t begin = c->pos;
    if (c->pos + 4 <= c->len && strncmp(c->src + c->pos, "true", 4) == 0) {
        c->pos += 4;
        nx_json_t *v = j_new(NX_JSON_BOOL);
        if (v) { v->src = c->src; v->raw_begin = begin; v->raw_end = c->pos; v->boolean = 1; }
        return v;
    }
    if (c->pos + 5 <= c->len && strncmp(c->src + c->pos, "false", 5) == 0) {
        c->pos += 5;
        nx_json_t *v = j_new(NX_JSON_BOOL);
        if (v) { v->src = c->src; v->raw_begin = begin; v->raw_end = c->pos; v->boolean = 0; }
        return v;
    }
    if (c->pos + 4 <= c->len && strncmp(c->src + c->pos, "null", 4) == 0) {
        c->pos += 4;
        nx_json_t *v = j_new(NX_JSON_NULL);
        if (v) { v->src = c->src; v->raw_begin = begin; v->raw_end = c->pos; }
        return v;
    }
    return NULL;
}

static nx_json_t *j_value(jctx_t *c) {
    j_skip(c);
    if (c->pos >= c->len) return NULL;
    char ch = c->src[c->pos];
    if (ch == '"') return j_string(c);
    if (ch == '[')  return j_array(c);
    if (ch == '{')  return j_object(c);
    if (ch == 't' || ch == 'f' || ch == 'n') return j_literal(c);
    if (ch == '-' || (ch >= '0' && ch <= '9')) return j_number(c);
    return NULL;
}

nx_json_t *nx_json_parse(const char *src, size_t len) {
    if (!src || len == 0) return NULL;
    jctx_t c = { src, len, 0 };
    nx_json_t *v = j_value(&c);
    return v;
}

void nx_json_free(nx_json_t *v) {
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) {
        if (v->items) nx_json_free(v->items[i]);
        if (v->keys && v->keys[i]) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->key_lens);
    free(v->str);
    free(v);
}

const nx_json_t *nx_json_get(const nx_json_t *obj, const char *key) {
    if (!obj || obj->type != NX_JSON_OBJ || !key) return NULL;
    size_t klen = strlen(key);
    for (size_t i = 0; i < obj->n; i++) {
        if (obj->key_lens[i] == klen &&
            obj->keys[i] && strncmp(obj->keys[i], key, klen) == 0)
            return obj->items[i];
    }
    return NULL;
}

const nx_json_t *nx_json_at(const nx_json_t *arr, size_t i) {
    if (!arr || (arr->type != NX_JSON_ARR && arr->type != NX_JSON_OBJ)) return NULL;
    return i < arr->n ? arr->items[i] : NULL;
}

const char *nx_json_str(const nx_json_t *v) {
    return (v && v->type == NX_JSON_STR) ? v->str : NULL;
}

double nx_json_number(const nx_json_t *v, double dflt) {
    return (v && v->type == NX_JSON_NUM) ? v->num : dflt;
}

int nx_json_is(const nx_json_t *v, nx_json_type_t t) {
    return v && v->type == t;
}

size_t nx_json_raw(const nx_json_t *v, const char **out) {
    if (!v || !v->src) { if (out) *out = NULL; return 0; }
    if (out) *out = v->src + v->raw_begin;
    return v->raw_end - v->raw_begin;
}
