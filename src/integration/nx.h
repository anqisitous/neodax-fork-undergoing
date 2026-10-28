#ifndef NX_H
#define NX_H

/*
 * nx — NeoDAX autonomous integration core.
 *
 * 目的:
 *   - src/ (旧 neodax エンジン: ELF/PE/RAW, x86/arm64/riscv, symexec/emulate/decompile)
 *     と src_new/ (新静的解析フレームワーク: Mach-O, depflow/var/exec/parallel/state)
 *     を、単一の ABI の下で相互運用させる。
 *
 * 設計原則 (自律性):
 *   1. 実行ファイル形式・要求機能から「どのバックエンド(provider)を使うか」を
 *      プログラムが実行時に決定する (capability negotiation + fallback)。
 *   2. 解析パスは read/write する artifact を宣言し、スケジューラが
 *      「並列に走らせてよいパス」と「直列に繋ぐパス」を依存関係から自動決定する。
 *   3. provider は vtable。旧 dax と新 macho を同じ操作で扱う。
 *
 * このヘッダは dax.h / macho_static.h を include しない。
 * 両者は 1 つの翻訳単位で共存できない (depflow_graph_t 等の前方宣言 typedef 衝突)
 * ため、ABI 境界をここに置き、provider 実装 (.c) 側だけがそれぞれを include する。
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    NX_OK        =  0,
    NX_ERR       = -1,
    NX_ENOTSUP   = -2,
    NX_ENOMEM    = -3,
    NX_EFORMAT   = -4,
    NX_ENOPROV   = -5
} nx_status_t;

/* ------------------------------------------------------------------ */
/* capabilities                                                        */
/* ------------------------------------------------------------------ */
enum {
    NX_CAP_LOAD       = 1u << 0,   /* open() 可能                        */
    NX_CAP_MACHO      = 1u << 1,
    NX_CAP_ELF        = 1u << 2,
    NX_CAP_PE         = 1u << 3,
    NX_CAP_RAW        = 1u << 4,
    NX_CAP_FUNCS      = 1u << 5,   /* 関数境界検出                        */
    NX_CAP_CFG        = 1u << 6,   /* 基本ブロック/CFG                    */
    NX_CAP_DISASM     = 1u << 7,
    NX_CAP_SYMEXEC    = 1u << 8,
    NX_CAP_EMULATE    = 1u << 9,
    NX_CAP_DECOMPILE  = 1u << 10,
    NX_CAP_DSA        = 1u << 11,
    NX_CAP_AIRE       = 1u << 12,
    NX_CAP_STATIC     = 1u << 13    /* 新静的解析フレームワーク             */
};

typedef enum {
    NX_ARCH_UNKNOWN = 0,
    NX_ARCH_X86_64,
    NX_ARCH_ARM64,
    NX_ARCH_RISCV64,
    NX_ARCH_X86_32
} nx_arch_t;

/* ------------------------------------------------------------------ */
/* POD projections (ABI 安定 / provider 非依存)                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char     name[64];
    char     seg[16];
    uint64_t vaddr;
    uint64_t size;
    uint64_t file_off;
    uint32_t flags;
    int      exec;
    int      writable;
    int      readable;
} nx_section_t;

typedef struct {
    uint64_t start;
    uint64_t end;
    char     name[512];
    uint32_t insn_count;
    uint32_t block_count;
    uint64_t frame_size;
    uint64_t arg_count;
    int      has_loops;
    int      has_calls;
    int      has_indirect;
} nx_func_t;

typedef struct {
    uint64_t address;
    uint8_t  length;
    char     mnemonic[32];
    char     operands[256];
    uint8_t  is_call;
    uint8_t  is_branch;
    uint8_t  is_cond;
    uint8_t  is_indirect;
    uint8_t  is_ret;
    uint8_t  has_target;   /* 直接分岐/呼び出しのターゲット解決済み */
    uint64_t target;
} nx_insn_t;

/* ------------------------------------------------------------------ */
/* artifact namespaces (pass read/write sets)                          */
/* ------------------------------------------------------------------ */
enum {
    NX_ART_RAW      = 1u << 0,
    NX_ART_DEPS     = 1u << 1,
    NX_ART_VARS     = 1u << 2,
    NX_ART_EXEC     = 1u << 3,
    NX_ART_PARALLEL = 1u << 4,
    NX_ART_STATE    = 1u << 5
};

/* artifact の格納スロット (ビットマスクとは別物) */
enum {
    NX_SLOT_RAW = 0,
    NX_SLOT_DEPS,
    NX_SLOT_VARS,
    NX_SLOT_EXEC,
    NX_SLOT_PARALLEL,
    NX_SLOT_STATE,
    NX_ART_SLOTS
};

/* ------------------------------------------------------------------ */
/* provider vtable                                                     */
/* ------------------------------------------------------------------ */
typedef struct nx_binary   nx_binary_t;
typedef struct nx_provider nx_provider_t;

struct nx_provider {
    const char *name;
    uint32_t    caps;

    /* 1 ならこの provider が担当する */
    int  (*detect)(const uint8_t *data, size_t size);

    nx_binary_t *(*open)(const char *path, const uint8_t *data, size_t size);
    void         (*close)(nx_binary_t *b);

    /* 読み出し */
    nx_arch_t (*arch)(nx_binary_t *b);
    uint64_t  (*entry)(nx_binary_t *b);
    int       (*n_sections)(nx_binary_t *b);
    int       (*section)(nx_binary_t *b, int i, nx_section_t *out);
    int       (*n_funcs)(nx_binary_t *b);
    int       (*func)(nx_binary_t *b, int i, nx_func_t *out);

    /* 重量級操作 (任意; 未対応なら NULL) */
    int (*disasm)(nx_binary_t *b, int func_idx, nx_insn_t *out, int max, int *n_out);
    int (*cfg)(nx_binary_t *b);
    int (*augment)(nx_binary_t *b, uint32_t want);   /* symexec / dsa / aire 等 */

    /* この provider が提供する解析パス (engine がスケジュールする) */
    const struct nx_pass *(*passes)(int *n_out);
};

struct nx_binary {
    const nx_provider_t *ops;
    void                *impl;
};

/* ------------------------------------------------------------------ */
/* 命令デコードの多態 (arch ごとの実装を実行時に選ぶ)                  */
/*
 * 不変条件: 同じ操作に実装が 1 つしか無ければ実質 merge と同じ。
 * 2 つ以上あれば「多態」として登録し、nx_select_decoder が
 * arch に応じて実行時に決定する。
 */
typedef struct nx_decoder {
    const char *name;
    nx_arch_t   arch;
    int (*decode)(uint64_t addr, const uint8_t *bytes, size_t len, nx_insn_t *out);
} nx_decoder_t;

void                nx_register_decoder(const nx_decoder_t *d);
const nx_decoder_t *nx_select_decoder(nx_arch_t arch);
int                 nx_decode(nx_arch_t arch, uint64_t addr,
                              const uint8_t *bytes, size_t len, nx_insn_t *out);
void                nx_decode_register_all(void);

/* ------------------------------------------------------------------ */
/* pass + engine                                                       */
/* ------------------------------------------------------------------ */
typedef struct nx_engine nx_engine_t;

typedef struct nx_pass {
    const char *name;
    uint32_t    reads;
    uint32_t    writes;
    int         cost;                 /* 大きいほど重い。並列度の判断に使う */
    int       (*run)(nx_engine_t *e);
} nx_pass_t;

struct nx_engine {
    nx_binary_t *bin;
    uint32_t     caps;
    uint32_t     produced;            /* 既に生成済み artifact マスク       */
    void        *art[NX_ART_SLOTS];
    int          nthreads;
    int          verbose;
    FILE        *out;
};

/* スケジュール結果 */
typedef struct {
    const nx_pass_t *pass;
    int              wave;         /* 実行ウェーブ番号 (小さいほど先) */
    int              parallel;     /* このウェーブ内で並列実行可        */
} nx_pass_plan_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* デコーダ実装は nx_decode.c (arm64/x86_64) が提供 */

/* provider レジストリ */
void                nx_register_provider(const nx_provider_t *p);
const nx_provider_t *nx_provider_macho(void);
const nx_provider_t *nx_provider_dax(void);

/* 形式検出 + capability 交渉 + fallback で provider を選ぶ (自律) */
const nx_provider_t *nx_select_provider(const uint8_t *data, size_t size,
                                        uint32_t require, uint32_t *out_caps);
/* ファイルを開く (provider を自動選択) */
nx_binary_t         *nx_open(const char *path, uint32_t require);
void                 nx_close(nx_binary_t *b);

const char          *nx_arch_str(nx_arch_t a);
const char          *nx_provider_name(const nx_binary_t *b);

/* engine */
nx_engine_t *nx_engine_new(nx_binary_t *b, int nthreads, FILE *out);
void         nx_engine_free(nx_engine_t *e);

/* 依存関係から直列/並列の実行計画を立てる (自律スケジューリング) */
int nx_plan(const nx_pass_t *passes, int npasses, uint32_t have,
            nx_pass_plan_t *plan, int plan_max);
/* 計画に従って実行。並列ウェーブは pthread で、直列ウェーブは逐次で。 */
int nx_engine_run(nx_engine_t *e, const nx_pass_t *passes, int npasses);

#ifdef __cplusplus
}
#endif

#endif /* NX_H */
