#include "nx_pipeline.h"
#include "nx_pe.h"
#include "../nx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    uint64_t addr;
    uint8_t  len;
    char     mnem[24];
    char     ops[96];
    int      is_call;
    int      is_ret;
} pdi_t;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static nx_arch_t arch_of(uint16_t m) {
    switch (m) {
        case 0x8664: case 0x014c: return NX_ARCH_X86_64;
        case 0xaa64:             return NX_ARCH_ARM64;
        default:                 return NX_ARCH_UNKNOWN;
    }
}

static uint8_t *read_file(const char *path, size_t *sz) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(fp); return NULL; }
    *sz = fread(b, 1, (size_t)n, fp);
    fclose(fp);
    return b;
}

/* 実行セクションを決め、コード先頭と長さ、base を返す */
static int locate_code(const nx_pe_image_t *img, const uint8_t *data, size_t fsz,
                       const uint8_t **code, size_t *clen, uint64_t *base) {
    int pick = -1;
    for (int i = 0; i < img->n_sections; i++)
        if (strcmp(img->sections[i].name, ".text") == 0) { pick = i; break; }
    if (pick < 0)
        for (int i = 0; i < img->n_sections; i++)
            if (img->sections[i].characteristics & 0x20000000) { pick = i; break; }
    if (pick < 0) return -1;
    const nx_pe_section_t *s = &img->sections[pick];
    if (s->raw_ptr + s->raw_size > fsz) return -1;
    *code = data + s->raw_ptr;
    *clen = s->raw_size;
    *base = img->image_base + s->virtual_address;
    return 0;
}

/* デコードして配列に詰める (並行版で共有) */
static int decode_all(nx_arch_t arch, const uint8_t *code, size_t clen,
                      uint64_t base, int limit, pdi_t **out) {
    pdi_t *arr = (pdi_t *)calloc((size_t)limit, sizeof(pdi_t));
    if (!arr) return -1;
    int n = 0;
    size_t off = 0;
    while (off < clen && n < limit) {
        nx_insn_t ni;
        memset(&ni, 0, sizeof(ni));
        if (nx_decode(arch, base + off, code + off, clen - off, &ni) != NX_OK || ni.length == 0) {
            off += 1;
            continue;
        }
        arr[n].addr = base + off;
        arr[n].len = ni.length;
        snprintf(arr[n].mnem, sizeof(arr[n].mnem), "%s", ni.mnemonic);
        snprintf(arr[n].ops, sizeof(arr[n].ops), "%s", ni.operands);
        arr[n].is_call = ni.is_call;
        arr[n].is_ret = ni.is_ret;
        n++;
        off += ni.length;
    }
    *out = arr;
    return n;
}

/* C++ エンコード (配列 -> C++ ソース) */
static void encode_one(FILE *o, const pdi_t *d) {
    fprintf(o, "{0x%llxULL,%u,\"%s\",\"%s\",%d,%d},\n",
            (unsigned long long)d->addr, d->len, d->mnem, d->ops, d->is_call, d->is_ret);
}

/* ---- simultaneous: producer が decode、consumer が encode ---- */

typedef struct {
    const uint8_t *code;
    size_t clen;
    uint64_t base;
    nx_arch_t arch;
    int limit;

    pdi_t *arr;
    int    n;
    int    cap;
    int    done;

    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    FILE *sink;   /* encode 出力先 (ores) — 実測では /dev/null 相当のバッファ */
    char *encbuf;
    size_t encbuf_len;
    size_t encbuf_cap;
} sim_ctx_t;

static void encbuf_add(sim_ctx_t *c, const char *s, size_t n) {
    if (c->encbuf_len + n + 1 > c->encbuf_cap) {
        size_t nc = c->encbuf_cap ? c->encbuf_cap * 2 : 65536;
        while (nc < c->encbuf_len + n + 1) nc *= 2;
        c->encbuf = (char *)realloc(c->encbuf, nc);
        c->encbuf_cap = nc;
    }
    if (c->encbuf) { memcpy(c->encbuf + c->encbuf_len, s, n); c->encbuf_len += n; c->encbuf[c->encbuf_len] = 0; }
}

static void *producer(void *arg) {
    sim_ctx_t *c = (sim_ctx_t *)arg;
    pthread_mutex_lock(&c->mtx);
    size_t off = 0;
    while (off < c->clen && c->n < c->limit) {
        nx_insn_t ni;
        memset(&ni, 0, sizeof(ni));
        if (nx_decode(c->arch, c->base + off, c->code + off, c->clen - off, &ni) != NX_OK || ni.length == 0) {
            off += 1;
            continue;
        }
        pdi_t d;
        memset(&d, 0, sizeof(d));
        d.addr = c->base + off;
        d.len = ni.length;
        snprintf(d.mnem, sizeof(d.mnem), "%s", ni.mnemonic);
        snprintf(d.ops, sizeof(d.ops), "%s", ni.operands);
        d.is_call = ni.is_call;
        d.is_ret = ni.is_ret;
        c->arr[c->n++] = d;
        pthread_cond_signal(&c->cond);
        off += ni.length;
    }
    c->done = 1;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->mtx);
    return NULL;
}

static void *consumer(void *arg) {
    sim_ctx_t *c = (sim_ctx_t *)arg;
    int idx = 0;
    char line[256];
    pthread_mutex_lock(&c->mtx);
    for (;;) {
        while (idx >= c->n && !c->done) pthread_cond_wait(&c->cond, &c->mtx);
        while (idx < c->n) {
            int len = snprintf(line, sizeof(line), "{0x%llxULL,%u,\"%s\",\"%s\",%d,%d},\n",
                               (unsigned long long)c->arr[idx].addr, c->arr[idx].len,
                               c->arr[idx].mnem, c->arr[idx].ops,
                               c->arr[idx].is_call, c->arr[idx].is_ret);
            encbuf_add(c, line, (size_t)len);
            idx++;
        }
        if (c->done && idx >= c->n) break;
    }
    pthread_mutex_unlock(&c->mtx);
    return NULL;
}

/* 実測用のダミー出力 (実際の cpp_out とは別に、時間だけ測る) */
static double run_sequential(nx_arch_t arch, const uint8_t *code, size_t clen,
                             uint64_t base, int limit, pdi_t **out_arr, int *out_n) {
    double t0 = now_ms();
    pdi_t *arr = NULL;
    int n = decode_all(arch, code, clen, base, limit, &arr);
    /* ここで decode 完了。続けて encode */
    char line[256];
    for (int i = 0; i < n; i++) {
        (void)snprintf(line, sizeof(line), "{0x%llxULL,%u,\"%s\",\"%s\",%d,%d},\n",
                       (unsigned long long)arr[i].addr, arr[i].len, arr[i].mnem,
                       arr[i].ops, arr[i].is_call, arr[i].is_ret);
    }
    double t1 = now_ms();
    *out_arr = arr;
    *out_n = n;
    return t1 - t0;
}

static double run_simultaneous(nx_arch_t arch, const uint8_t *code, size_t clen,
                               uint64_t base, int limit, pdi_t **out_arr, int *out_n) {
    sim_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.code = code; c.clen = clen; c.base = base; c.arch = arch; c.limit = limit;
    c.cap = limit;
    c.arr = (pdi_t *)calloc((size_t)limit, sizeof(pdi_t));
    if (!c.arr) { *out_arr = NULL; *out_n = 0; return 0; }
    pthread_mutex_init(&c.mtx, NULL);
    pthread_cond_init(&c.cond, NULL);

    double t0 = now_ms();
    pthread_t tp, tc;
    pthread_create(&tp, NULL, producer, &c);
    pthread_create(&tc, NULL, consumer, &c);
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    double t1 = now_ms();

    pthread_mutex_destroy(&c.mtx);
    pthread_cond_destroy(&c.cond);
    free(c.encbuf);
    *out_arr = c.arr;
    *out_n = c.n;
    return t1 - t0;
}

int nx_pipeline_run(const char *pe_path, nx_strategy_t strat, int limit,
                    FILE *cpp_out, nx_pipeline_result_t *res, FILE *log) {
    if (!log) log = stdout;
    if (limit <= 0) limit = 4000;

    nx_pe_image_t *img = nx_pe_open(pe_path);
    if (!img) { fprintf(log, "  [pipeline] not a PE: %s\n", pe_path); return -1; }
    nx_arch_t arch = arch_of(img->machine);
    if (arch == NX_ARCH_UNKNOWN) { nx_pe_free(img); return -1; }

    size_t fsz = 0;
    uint8_t *data = read_file(pe_path, &fsz);
    const uint8_t *code; size_t clen; uint64_t base;
    if (!data || locate_code(img, data, fsz, &code, &clen, &base) != 0) {
        fprintf(log, "  [pipeline] no executable section\n");
        free(data); nx_pe_free(img); return -1;
    }

    nx_decode_register_all();

    nx_pipeline_result_t r;
    memset(&r, 0, sizeof(r));

    if (strat == NX_STRAT_SEQUENTIAL) {
        pdi_t *a = NULL; int n = 0;
        r.seq_ms = run_sequential(arch, code, clen, base, limit, &a, &n);
        free(a); r.n_insns = n;
        r.chosen = NX_STRAT_SEQUENTIAL;
    } else if (strat == NX_STRAT_SIMULTANEOUS) {
        pdi_t *a = NULL; int n = 0;
        r.sim_ms = run_simultaneous(arch, code, clen, base, limit, &a, &n);
        free(a); r.n_insns = n;
        r.chosen = NX_STRAT_SIMULTANEOUS;
    } else {
        pdi_t *a1 = NULL; int n1 = 0;
        r.seq_ms = run_sequential(arch, code, clen, base, limit, &a1, &n1);
        free(a1);
        pdi_t *a2 = NULL; int n2 = 0;
        r.sim_ms = run_simultaneous(arch, code, clen, base, limit, &a2, &n2);
        free(a2);
        r.chosen = (r.sim_ms < r.seq_ms) ? NX_STRAT_SIMULTANEOUS : NX_STRAT_SEQUENTIAL;
        r.n_insns = n1;
    }

    /* 出力: 選んだ戦略で本番実行し cpp_out に書く */
    if (cpp_out) {
        pdi_t *arr = NULL; int n = 0;
        if (r.chosen == NX_STRAT_SIMULTANEOUS)
            run_simultaneous(arch, code, clen, base, limit, &arr, &n);
        else
            run_sequential(arch, code, clen, base, limit, &arr, &n);
        fprintf(cpp_out, "// generated from PE: %s (arch=%s)\n",
                pe_path, arch == NX_ARCH_ARM64 ? "arm64" : "x86");
        fprintf(cpp_out, "// model: {addr,len,mnemonic,operands,is_call,is_ret}\n");
        fprintf(cpp_out, "struct NxInsn { uint64_t addr; uint8_t len; const char* mnem; const char* ops; bool call; bool ret; };\n");
        fprintf(cpp_out, "static const NxInsn kInsns[] = {\n");
        for (int i = 0; i < n; i++) encode_one(cpp_out, &arr[i]);
        fprintf(cpp_out, "};\nstatic const int kNInsns = %d;\n", n);
        r.n_insns = n;
        free(arr);
    }

    if (res) *res = r;
    free(data);
    nx_pe_free(img);
    return 0;
}
