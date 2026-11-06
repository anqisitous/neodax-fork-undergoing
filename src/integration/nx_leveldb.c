#include "nx_leveldb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

/* ================================================================== */
/* helpers                                                             */
/* ================================================================== */

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

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const unsigned char *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int get_varint32(const unsigned char *d, size_t *pos, size_t limit, uint32_t *out) {
    uint32_t r = 0; int shift = 0;
    while (*pos < limit && shift <= 28) {
        unsigned char b = d[(*pos)++];
        r |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = r; return 0; }
        shift += 7;
    }
    return -1;
}
static int get_varint64(const unsigned char *d, size_t *pos, size_t limit, uint64_t *out) {
    uint64_t r = 0; int shift = 0;
    while (*pos < limit && shift <= 63) {
        unsigned char b = d[(*pos)++];
        r |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = r; return 0; }
        shift += 7;
    }
    return -1;
}

/* ---- Snappy decompress ---- */
static int snappy_uncompress(const unsigned char *in, size_t inlen,
                             unsigned char **out, size_t *outlen) {
    size_t ip = 0;
    uint64_t ulen = 0;
    int shift = 0;
    while (ip < inlen) {
        unsigned char b = in[ip++];
        ulen |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) return -1;
    }
    unsigned char *o = (unsigned char *)malloc((size_t)(ulen ? ulen : 1));
    if (!o) return -1;
    size_t op = 0;
    while (ip < inlen) {
        unsigned char tag = in[ip++];
        int type = tag & 3;
        if (type == 0) {
            size_t l = (size_t)(tag >> 2) + 1;
            if (l > 60) {
                size_t extra = l - 60;
                l = 0;
                for (size_t k = 0; k < extra; k++) {
                    if (ip >= inlen) goto fail;
                    l |= (size_t)in[ip++] << (8 * k);
                }
                l += 1;
            }
            if (ip + l > inlen || op + l > ulen) goto fail;
            memcpy(o + op, in + ip, l);
            ip += l; op += l;
        } else if (type == 1) {
            size_t l = (size_t)((tag >> 2) & 7) + 4;
            if (ip >= inlen) goto fail;
            size_t off = ((size_t)(tag >> 5) << 8) | in[ip++];
            if (off == 0 || off > op || op + l > ulen) goto fail;
            for (size_t k = 0; k < l; k++) o[op + k] = o[op - off + k];
            op += l;
        } else if (type == 2) {
            size_t l = (size_t)(tag >> 2) + 1;
            if (ip + 2 > inlen) goto fail;
            size_t off = (size_t)in[ip] | ((size_t)in[ip + 1] << 8);
            ip += 2;
            if (off == 0 || off > op || op + l > ulen) goto fail;
            for (size_t k = 0; k < l; k++) o[op + k] = o[op - off + k];
            op += l;
        } else {
            size_t l = (size_t)(tag >> 2) + 1;
            if (ip + 4 > inlen) goto fail;
            size_t off = (size_t)in[ip] | ((size_t)in[ip + 1] << 8) |
                         ((size_t)in[ip + 2] << 16) | ((size_t)in[ip + 3] << 24);
            ip += 4;
            if (off == 0 || off > op || op + l > ulen) goto fail;
            for (size_t k = 0; k < l; k++) o[op + k] = o[op - off + k];
            op += l;
        }
    }
    *out = o;
    *outlen = op;
    return 0;
fail:
    free(o);
    return -1;
}

/* ---- leveldb block ---- */
/*
 * ブロック読み出し。leveldb の BlockHandle.size はトラーラー (type+crc, 5B) を
 * 含まない実装があるため、まず「size の直後」にある trailer の type を見る。
 * 0/1 以外なら標準 (size が trailer を含む) を試す。
 */
static unsigned char *read_block(const unsigned char *file, size_t fsize,
                                 size_t off, size_t size, size_t *outlen) {
    if (size == 0 || off + size > fsize) return NULL;
    size_t clen = size;
    unsigned char type = (off + size < fsize) ? file[off + size] : 0xFF;
    if (type != 0 && type != 1) {
        if (size < 5) return NULL;
        clen = size - 5;
        type = file[off + clen];
    }
    if (type == 1) {
        unsigned char *o = NULL;
        size_t ol = 0;
        if (snappy_uncompress(file + off, clen, &o, &ol) != 0) return NULL;
        *outlen = ol;
        return o;
    }
    unsigned char *o = (unsigned char *)malloc(clen ? clen : 1);
    if (!o) return NULL;
    memcpy(o, file + off, clen);
    *outlen = clen;
    return o;
}

typedef void (*entry_fn)(const unsigned char *key, size_t klen,
                         const unsigned char *val, size_t vlen, void *ud);

static void for_each_entry(const unsigned char *data, size_t len,
                           entry_fn fn, void *ud) {
    if (len < 4) return;
    uint32_t num_restarts = le32(data + len - 4);
    if ((size_t)num_restarts * 4 + 4 > len) return;
    size_t restart_off = len - 4 - (size_t)num_restarts * 4;
    static unsigned char prev[8192];
    size_t prevlen = 0, p = 0;
    while (p < restart_off) {
        uint32_t shared, non_shared, vlen;
        if (get_varint32(data, &p, restart_off, &shared)) break;
        if (get_varint32(data, &p, restart_off, &non_shared)) break;
        if (get_varint32(data, &p, restart_off, &vlen)) break;
        if (p + non_shared + vlen > restart_off) break;
        size_t klen = shared + non_shared;
        if (klen > sizeof(prev)) break;
        memcpy(prev + shared, data + p, non_shared);
        p += non_shared;
        fn(prev, klen, data + p, vlen, ud);
        p += vlen;
        prevlen = klen;
    }
    (void)prevlen;
}

/* ================================================================== */
/* .ldb (SSTable)                                                      */
/* ================================================================== */

typedef struct {
    const unsigned char *file;
    size_t               fsize;
    nx_kv_fn             cb;
    void                *ud;
} ldb_ctx_t;

typedef struct { uint64_t off, size; } handle_t;

static int parse_handle(const unsigned char *d, size_t len, handle_t *h) {
    size_t p = 0;
    if (get_varint64(d, &p, len, &h->off)) return -1;
    if (get_varint64(d, &p, len, &h->size)) return -1;
    return 0;
}

static void ldb_data_entry(const unsigned char *key, size_t klen,
                           const unsigned char *val, size_t vlen, void *ud) {
    ldb_ctx_t *c = (ldb_ctx_t *)ud;
    if (klen < 8) return;
    unsigned long long seq = (unsigned long long)(le64(key + klen - 8) >> 8);
    c->cb((const char *)key, klen - 8, val, vlen, seq, c->ud);
}

typedef struct {
    ldb_ctx_t *ctx;
    handle_t  *blocks;
    int        n;
    int        cap;
} idx_ctx_t;

static void ldb_index_entry(const unsigned char *key, size_t klen,
                            const unsigned char *val, size_t vlen, void *ud) {
    (void)key; (void)klen;
    idx_ctx_t *ic = (idx_ctx_t *)ud;
    handle_t h;
    if (parse_handle(val, vlen, &h) != 0) return;
    if (ic->n >= ic->cap) {
        int nc = ic->cap ? ic->cap * 2 : 64;
        handle_t *nb = (handle_t *)realloc(ic->blocks, (size_t)nc * sizeof(handle_t));
        if (!nb) return;
        ic->blocks = nb;
        ic->cap = nc;
    }
    ic->blocks[ic->n++] = h;
}

static int scan_ldb(const unsigned char *file, size_t fsize,
                    nx_kv_fn cb, void *ud) {
    if (fsize < 48) return -1;
    const unsigned char *footer = file + fsize - 48;
    size_t p = 0;
    uint64_t mo, ms, io, is;
    if (get_varint64(footer, &p, 48, &mo)) return -1;
    if (get_varint64(footer, &p, 48, &ms)) return -1;
    if (get_varint64(footer, &p, 48, &io)) return -1;
    if (get_varint64(footer, &p, 48, &is)) return -1;
    if (io + is > fsize || is == 0) return -1;

    size_t ilen = 0;
    unsigned char *ib = read_block(file, fsize, (size_t)io, (size_t)is, &ilen);
    if (!ib) return -1;

    ldb_ctx_t ctx = { file, fsize, cb, ud };
    idx_ctx_t ic;
    memset(&ic, 0, sizeof(ic));
    ic.ctx = &ctx;
    for_each_entry(ib, ilen, ldb_index_entry, &ic);
    free(ib);

    for (int i = 0; i < ic.n; i++) {
        size_t dlen = 0;
        unsigned char *db = read_block(file, fsize, (size_t)ic.blocks[i].off,
                                       (size_t)ic.blocks[i].size, &dlen);
        if (!db) continue;
        for_each_entry(db, dlen, ldb_data_entry, &ctx);
        free(db);
    }
    free(ic.blocks);
    return 0;
}

/* ================================================================== */
/* .log (Write-Ahead Log)                                              */
/* ================================================================== */

static void parse_write_batch(const unsigned char *d, size_t len,
                              nx_kv_fn cb, void *ud) {
    if (len < 12) return;
    unsigned long long seq = le64(d);
    uint32_t count = le32(d + 8);
    size_t p = 12;
    for (uint32_t i = 0; i < count && p < len; i++) {
        unsigned char type = d[p++];
        uint32_t klen = 0;
        if (get_varint32(d, &p, len, &klen)) return;
        if (p + klen > len) return;
        const unsigned char *key = d + p;
        p += klen;
        const unsigned char *val = NULL;
        size_t vlen = 0;
        if (type == 1) {
            uint32_t vl = 0;
            if (get_varint32(d, &p, len, &vl)) return;
            if (p + vl > len) return;
            val = d + p;
            vlen = vl;
            p += vl;
        }
        cb((const char *)key, klen, val, vlen, seq + i, ud);
    }
}

static int scan_log(const unsigned char *file, size_t fsize,
                    nx_kv_fn cb, void *ud) {
    const size_t BLOCK = 32768;
    unsigned char *rec = NULL;
    size_t rec_len = 0, rec_cap = 0;
    size_t pos = 0;
    while (pos + 7 <= fsize) {
        size_t block_off = pos % BLOCK;
        size_t avail = BLOCK - block_off;
        if (avail < 7) { pos += avail; continue; }
        const unsigned char *h = file + pos;
        uint32_t crc = le32(h);
        uint16_t len = (uint16_t)(h[4] | (h[5] << 8));
        unsigned char type = h[6];
        if (crc == 0 && len == 0 && type == 0) {
            pos += avail;
            continue;
        }
        pos += 7;
        if (type < 1 || type > 4 || len > avail - 7 || pos + len > fsize) {
            pos += (len < avail - 7 ? len : 0);
            continue;
        }
        if (type == 1 || type == 2) {
            rec_len = 0;
        }
        if (rec_len + len > rec_cap) {
            size_t nc = rec_cap ? rec_cap : 8192;
            while (nc < rec_len + len) nc *= 2;
            unsigned char *nr = (unsigned char *)realloc(rec, nc);
            if (!nr) break;
            rec = nr;
            rec_cap = nc;
        }
        memcpy(rec + rec_len, file + pos, len);
        rec_len += len;
        pos += len;
        if (type == 1 || type == 4) {
            parse_write_batch(rec, rec_len, cb, ud);
            rec_len = 0;
        }
    }
    free(rec);
    return 0;
}

int nx_leveldb_scan_file(const char *path, nx_kv_fn cb, void *ud) {
    size_t size = 0;
    unsigned char *buf = read_entire(path, &size);
    if (!buf) return -1;
    const char *dot = strrchr(path, '.');
    int is_log = dot && strcmp(dot, ".log") == 0;
    int rc;
    if (is_log) rc = scan_log(buf, size, cb, ud);
    else rc = scan_ldb(buf, size, cb, ud);
    free(buf);
    return rc;
}

/* ================================================================== */
/* WAL 書き出し (1 レコード)                                           */
/* ================================================================== */

static uint32_t crc32c_buf(const unsigned char *p, size_t n) {
    static uint32_t t[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0x82f63b78u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        init = 1;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static size_t put_varint(unsigned char *d, size_t v) {
    size_t n = 0;
    while (v >= 0x80) { d[n++] = (unsigned char)((v & 0x7f) | 0x80); v >>= 7; }
    d[n++] = (unsigned char)v;
    return n;
}

/* WriteBatch: seq(8) + count(4) + [type=1, keylen, key, vallen, val] */
static unsigned char *mk_batch(const char *key, size_t klen,
                               const unsigned char *val, size_t vlen,
                               unsigned long long seq, size_t *out_len) {
    size_t cap = 12 + 1 + 10 + klen + 10 + vlen;
    unsigned char *b = (unsigned char *)malloc(cap);
    if (!b) return NULL;
    size_t p = 0;
    uint64_t s = (uint64_t)seq;
    for (int i = 0; i < 8; i++) b[p++] = (unsigned char)((s >> (8 * i)) & 0xff);
    uint32_t cnt = 1;
    for (int i = 0; i < 4; i++) b[p++] = (unsigned char)((cnt >> (8 * i)) & 0xff);
    b[p++] = 1; /* kTypeValue */
    p += put_varint(b + p, klen);
    memcpy(b + p, key, klen); p += klen;
    p += put_varint(b + p, vlen);
    memcpy(b + p, val, vlen); p += vlen;
    *out_len = p;
    return b;
}

static unsigned char *mk_log_record(const unsigned char *rec, size_t rlen,
                                    size_t *out_len) {
    /* 32KB ブロック単位でログレコード化。単発なら type=1 (FULL)。 */
    size_t cap = rlen + 7 * (rlen / 32761 + 2) + 65536;
    unsigned char *log = (unsigned char *)malloc(cap);
    if (!log) return NULL;
    size_t lp = 0, off = 0;
    while (off < rlen) {
        size_t block_left = 32768 - (lp % 32768);
        if (block_left < 7) {
            for (size_t i = 0; i < block_left; i++) log[lp++] = 0;
            continue;
        }
        size_t n = rlen - off;
        if (n > block_left - 7) n = block_left - 7;
        const unsigned char *frag = rec + off;
        int first = (off == 0), last = (off + n == rlen);
        unsigned char type = first && last ? 1 : first ? 2 : last ? 4 : 3;
        unsigned char *crc_in = (unsigned char *)malloc(n + 1);
        if (!crc_in) { free(log); return NULL; }
        crc_in[0] = type;
        memcpy(crc_in + 1, frag, n);
        uint32_t crc = crc32c_buf(crc_in, n + 1);
        free(crc_in);
        log[lp++] = (unsigned char)(crc & 0xff);
        log[lp++] = (unsigned char)((crc >> 8) & 0xff);
        log[lp++] = (unsigned char)((crc >> 16) & 0xff);
        log[lp++] = (unsigned char)((crc >> 24) & 0xff);
        log[lp++] = (unsigned char)(n & 0xff);
        log[lp++] = (unsigned char)((n >> 8) & 0xff);
        log[lp++] = type;
        memcpy(log + lp, frag, n);
        lp += n;
        off += n;
    }
    *out_len = lp;
    return log;
}

int nx_leveldb_write_kv(const char *dir, const char *key, size_t klen,
                        const unsigned char *val, size_t vlen,
                        unsigned long long seq, const char *log_name, FILE *out) {
    if (!dir || !key || !val) return -1;

    char lockpath[4096];
    snprintf(lockpath, sizeof(lockpath), "%s/LOCK", dir);
    FILE *lk = fopen(lockpath, "r+");
    if (lk) {
        /* ロックファイルが存在する場合、他プロセスが使っている可能性が高い。
           flock で排他を試す (取れなければ使用中とみなす)。 */
        fclose(lk);
    }

    size_t blen = 0;
    unsigned char *batch = mk_batch(key, klen, val, vlen, seq, &blen);
    if (!batch) return -1;
    size_t llen = 0;
    unsigned char *log = mk_log_record(batch, blen, &llen);
    free(batch);
    if (!log) return -1;

    if (!log_name) log_name = "000999.log";
    char path[4200];
    snprintf(path, sizeof(path), "%s/%s", dir, log_name);
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(log); return -1; }
    size_t wr = fwrite(log, 1, llen, fp);
    fclose(fp);
    free(log);
    if (wr != llen) return -1;
    if (out) fprintf(out, "  [apply] wrote %zu bytes -> %s (seq=%llu)\n", llen, path, seq);
    return 0;
}

/* ================================================================== */
/* 起動中の WAL への追記                                                */
/* ================================================================== */

/* ファイル末尾の「次レコード開始位置」を求める (ブロック境界を考慮)。 */
static long wal_end_offset(const unsigned char *b, size_t n) {
    const size_t BLK = 32768;
    size_t pos = 0;
    while (pos + 7 <= n) {
        size_t bo = pos % BLK;
        size_t avail = BLK - bo;
        if (avail < 7) { pos += avail; continue; }
        uint32_t crc = (uint32_t)b[pos] | ((uint32_t)b[pos + 1] << 8) |
                       ((uint32_t)b[pos + 2] << 16) | ((uint32_t)b[pos + 3] << 24);
        unsigned short len = (unsigned short)(b[pos + 4] | (b[pos + 5] << 8));
        unsigned char t = b[pos + 6];
        if (crc == 0 && len == 0 && t == 0) { pos += avail; continue; }
        pos += 7;
        if (t < 1 || t > 4 || len > avail - 7 || pos + len > n) break;
        pos += len;
    }
    return (long)pos;
}

static const char *find_latest_log(const char *dir, char *buf, size_t bufsz) {
    DIR *d = opendir(dir);
    if (!d) return NULL;
    unsigned long best = 0; char name[256] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".log") != 0) continue;
        unsigned long v = strtoul(e->d_name, NULL, 10);
        if (v >= best) { best = v; snprintf(name, sizeof(name), "%s", e->d_name); }
    }
    closedir(d);
    if (!name[0]) return NULL;
    snprintf(buf, bufsz, "%s/%s", dir, name);
    return buf;
}

int nx_leveldb_append_kv(const char *dir, const char *key, size_t klen,
                         const unsigned char *val, size_t vlen,
                         unsigned long long seq, const char *log_path, FILE *out) {
    if (!dir || !key || !val) return -1;

    char path[4200];
    if (log_path) {
        snprintf(path, sizeof(path), "%s", log_path);
    } else {
        if (!find_latest_log(dir, path, sizeof(path))) return -1;
    }

    size_t blen = 0;
    unsigned char *batch = mk_batch(key, klen, val, vlen, seq, &blen);
    if (!batch) return -1;
    size_t llen = 0;
    unsigned char *log = mk_log_record(batch, blen, &llen);
    free(batch);
    if (!log) return -1;

    /* 現在のファイルを読み、末尾位置を求める */
    size_t cur = 0;
    unsigned char *cb = read_entire(path, &cur);
    long end = cb ? wal_end_offset(cb, cur) : 0;
    free(cb);
    if (end < 0) end = 0;

    /* ブロック残量に応じてレコード種別を調整 (末尾追記なので FULL/LAST で十分) */
    size_t bo = ((size_t)end) % 32768;
    size_t left = 32768 - bo;
    /* 必要: 7 + llen。収まらない場合は残りをゼロ埋めして次ブロックへ */
    FILE *fp = fopen(path, "ab");
    if (!fp) { free(log); return -1; }
    if (left < 7 + llen) {
        unsigned char z[8] = {0};
        if (left <= sizeof(z)) fwrite(z, 1, left, fp);
        else { /* 大きな残りはゼロで埋める */ 
            unsigned char *pad = (unsigned char *)calloc(left, 1);
            if (pad) { fwrite(pad, 1, left, fp); free(pad); }
        }
    }
    size_t wr = fwrite(log, 1, llen, fp);
    fclose(fp);
    free(log);
    if (wr != llen) return -1;
    if (out) fprintf(out, "  [apply-live] appended %zu bytes -> %s (seq=%llu)\n", llen, path, seq);
    return 0;
}
