#include "nx.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ================================================================== */
/* provider registry                                                  */
/* ================================================================== */

#define NX_MAX_PROVIDERS 8

static const nx_provider_t *g_providers[NX_MAX_PROVIDERS];
static int                  g_nproviders = 0;
static pthread_mutex_t      g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

void nx_register_provider(const nx_provider_t *p) {
    if (!p) return;
    pthread_mutex_lock(&g_reg_lock);
    /* 同じ名前は一度だけ */
    for (int i = 0; i < g_nproviders; i++) {
        if (g_providers[i] == p ||
            (g_providers[i]->name && p->name &&
             strcmp(g_providers[i]->name, p->name) == 0)) {
            pthread_mutex_unlock(&g_reg_lock);
            return;
        }
    }
    if (g_nproviders < NX_MAX_PROVIDERS)
        g_providers[g_nproviders++] = p;
    pthread_mutex_unlock(&g_reg_lock);
}

static void nx_reg_all_impl(void);
static pthread_once_t g_reg_once = PTHREAD_ONCE_INIT;
static void nx_reg_all_impl(void) {
    nx_register_provider(nx_provider_macho());
    /* 旧 neodax は NX_WITH_LEGACY でリンクされたときだけ実体を持つ。
       常に登録するが、未ビルドなら open() が NULL を返す。 */
    nx_register_provider(nx_provider_dax());
}
static void nx_registry_init(void) {
    pthread_once(&g_reg_once, nx_reg_all_impl);
}

/* ================================================================== */
/* decoder registry now lives in nx_decode.c (linked by both binaries)  */
/* ================================================================== */

/* ================================================================== */
/* detection + capability negotiation (自律ディスパッチ)               */
/* ================================================================== */

const nx_provider_t *nx_select_provider(const uint8_t *data, size_t size,
                                        uint32_t require, uint32_t *out_caps) {
    nx_registry_init();

    const nx_provider_t *best = NULL;
    int best_score = -1;

    pthread_mutex_lock(&g_reg_lock);
    for (int i = 0; i < g_nproviders; i++) {
        const nx_provider_t *p = g_providers[i];
        if (!p || !(p->caps & NX_CAP_LOAD) || !p->detect || !p->open)
            continue;
        if (!p->detect(data, size))
            continue;

        /* 必須機能を満たすか */
        int missing = (int)(require & ~p->caps);
        /* 必須機能を満たす provider を優先。満たさない場合も
           最も多く満たす provider を fallback として覚えておく。 */
        int have = 0;
        uint32_t r = require & p->caps;
        while (r) { have += (int)(r & 1u); r >>= 1; }

        int score = (missing == 0) ? (1000 + have) : have;
        if (score > best_score) {
            best_score = score;
            best = p;
        }
    }
    pthread_mutex_unlock(&g_reg_lock);

    if (out_caps && best) *out_caps = best->caps;
    return best;
}

const char *nx_arch_str(nx_arch_t a) {
    switch (a) {
        case NX_ARCH_X86_64:  return "x86_64";
        case NX_ARCH_ARM64:   return "arm64";
        case NX_ARCH_RISCV64: return "riscv64";
        default:              return "unknown";
    }
}

const char *nx_provider_name(const nx_binary_t *b) {
    return (b && b->ops && b->ops->name) ? b->ops->name : "(none)";
}

nx_binary_t *nx_open(const char *path, uint32_t require) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);

    const nx_provider_t *p = nx_select_provider(buf, (size_t)sz, require, NULL);
    if (!p) { free(buf); return NULL; }

    nx_binary_t *b = p->open(path, buf, (size_t)sz);
    if (!b) { free(buf); return NULL; }

    /* provider が ops を設定し忘れても呼び出し側で補う。 */
    if (!b->ops) b->ops = p;

    /* 形式判定用のバッファは provider が所有権を取るかコピーする契約。
       ここでは provider が data を保持しない実装でも安全なように
       provider->close 側で free する前提とする。 */
    return b;
}

void nx_close(nx_binary_t *b) {
    if (!b) return;
    if (b->ops && b->ops->close) b->ops->close(b);
}

/* ================================================================== */
/* scheduler: 直列/並列を依存関係から自律決定                          */
/* ================================================================== */

int nx_plan(const nx_pass_t *passes, int npasses, uint32_t have,
            nx_pass_plan_t *plan, int plan_max) {
    if (!passes || npasses <= 0 || !plan || plan_max <= 0) return 0;

    char done[256];
    if (npasses > (int)sizeof(done)) npasses = (int)sizeof(done);
    memset(done, 0, sizeof(done));

    uint32_t produced = have;
    int nplan = 0;
    int wave = 0;

    while (nplan < npasses && nplan < plan_max) {
        /* このウェーブで実行可能なパスを集める */
        uint32_t wave_writes = 0;
        int progressed = 0;

        for (int i = 0; i < npasses && nplan < plan_max; i++) {
            if (done[i]) continue;
            const nx_pass_t *p = &passes[i];
            if ((p->reads & ~produced) != 0) continue;   /* 入力未生成 */
            if (p->writes == 0) {
                /* 書き込みなし = 純粋出力。同一ウェーブで許容 */
            } else if ((p->writes & wave_writes) != 0) {
                continue;                                 /* 書き込み衝突 → 次波 */
            }

            plan[nplan].pass = p;
            plan[nplan].wave = wave;
            plan[nplan].parallel = 1;   /* 後で単独波なら 0 に補正 */
            nplan++;
            done[i] = 1;
            wave_writes |= p->writes;
            progressed = 1;
        }

        if (!progressed) {
            /* 依存が満たせない (循環 or 未提供)。残りを直列で強制消化。 */
            for (int i = 0; i < npasses && nplan < plan_max; i++) {
                if (done[i]) continue;
                plan[nplan].pass = &passes[i];
                plan[nplan].wave = wave;
                plan[nplan].parallel = 0;
                nplan++;
                done[i] = 1;
                produced |= passes[i].writes;
            }
            break;
        }

        /* 各ウェーブの並列度を確定: その波に 2 つ以上あれば並列 */
        int cnt = 0;
        for (int k = nplan - 1; k >= 0 && plan[k].wave == wave; k--) cnt++;
        for (int k = nplan - 1; k >= 0 && plan[k].wave == wave; k--)
            plan[k].parallel = (cnt > 1);

        produced |= wave_writes;
        wave++;
    }

    return nplan;
}

/* ================================================================== */
/* engine                                                              */
/* ================================================================== */

nx_engine_t *nx_engine_new(nx_binary_t *b, int nthreads, FILE *out) {
    nx_engine_t *e = (nx_engine_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->bin = b;
    e->caps = b ? b->ops->caps : 0;
    e->nthreads = nthreads > 0 ? nthreads : 1;
    e->out = out ? out : stdout;
    return e;
}

void nx_engine_free(nx_engine_t *e) {
    if (!e) return;
    free(e);
}

typedef struct {
    nx_engine_t     *e;
    const nx_pass_t *pass;
    int              rc;
} nx_task_t;

static void *nx_task_main(void *arg) {
    nx_task_t *t = (nx_task_t *)arg;
    t->rc = t->pass->run ? t->pass->run(t->e) : NX_OK;
    return NULL;
}

int nx_engine_run(nx_engine_t *e, const nx_pass_t *passes, int npasses) {
    if (!e || !passes || npasses <= 0) return NX_ERR;

    nx_pass_plan_t plan[256];
    int nplan = nx_plan(passes, npasses, e->produced, plan, 256);
    if (nplan <= 0) return NX_ERR;

    int rc = NX_OK;
    for (int i = 0; i < nplan; ) {
        int wave = plan[i].wave;
        int j = i;
        while (j < nplan && plan[j].wave == wave) j++;
        int count = j - i;

        if (count > 1 && plan[i].parallel && e->nthreads > 1) {
            pthread_t *th = (pthread_t *)calloc((size_t)count, sizeof(pthread_t));
            nx_task_t *tk = (nx_task_t *)calloc((size_t)count, sizeof(nx_task_t));
            int launched = 0;
            if (th && tk) {
                for (int k = 0; k < count; k++) {
                    tk[k].e = e;
                    tk[k].pass = plan[i + k].pass;
                    tk[k].rc = NX_ERR;
                    if (pthread_create(&th[k], NULL, nx_task_main, &tk[k]) == 0)
                        launched++;
                    else
                        nx_task_main(&tk[k]);   /* 生成失敗は同期実行 */
                }
                for (int k = 0; k < launched; k++) pthread_join(th[k], NULL);
                for (int k = 0; k < count; k++) {
                    if (tk[k].rc != NX_OK) {
                        rc = tk[k].rc;
                        fprintf(stderr, "[nx] pass '%s' failed rc=%d\n",
                                plan[i + k].pass->name, tk[k].rc);
                    }
                    e->produced |= plan[i + k].pass->writes;
                }
            } else {
                rc = NX_ENOMEM;
            }
            free(th);
            free(tk);
        } else {
            for (int k = 0; k < count; k++) {
                const nx_pass_t *p = plan[i + k].pass;
                if (p->run) {
                    int r = p->run(e);
                    if (r != NX_OK) {
                        rc = r;
                        fprintf(stderr, "[nx] pass '%s' failed rc=%d\n", p->name, r);
                    }
                }
                e->produced |= p->writes;
            }
        }
        i = j;
    }
    return rc;
}
