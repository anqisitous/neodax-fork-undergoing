/* nx_cli_registry.h — コマンドとオプションの一元化
 *
 * コマンドの定義 (名前・オプション・説明・分類) を **1 箇所** に集める。
 * 実装 (cmd_xxx) とは分離する。ここを直せば、CLI の usage も、
 * ディスパッチも、GUI のボタンも同時に変わる。
 *
 * 以前は 2 つの真実があった:
 *   - nx_cli_dispatch() の if/else 連鎖 (実際の挙動)
 *   - kNxCmds[] の表 (GUI 表示だけ)
 * 片方だけ直すと食い違う。ここでは 1 つにまとめる。
 *
 * オプションも同じく一元化する。各コマンドがどのオプションを取るかは
 * takes フィールドに列挙し、解析は nx_opt_* で行う。
 * (オプション名を各 cmd_ の中で文字列比較しない)
 */
#ifndef NX_CLI_REGISTRY_H
#define NX_CLI_REGISTRY_H

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct nx_cli_ctx nx_cli_ctx_t;

/* オプションの種類。値の有無で 2 種に分ける。 */
typedef enum {
    NX_OPT_VAL,   /* --name VALUE を取る */
    NX_OPT_BOOL   /* --name だけで真偽 */
} nx_opt_kind_t;

/* 1 つのコマンド定義。 */
typedef struct {
    const char *name;      /* CLI のコマンド名 (argv[1]) */
    const char *title;     /* GUI ボタンに出る名前 */
    const char *group;     /* GUI のセクション見出し */
    const char *takes;     /* 取るオプション。空白区切り。例: "--state --app" */
    const char *desc;      /* 説明 (usage と GUI の両方に出る) */
    /* 同じことをする別名。中身が同じなら別コマンドにしない。
       例: check は advise の「件数だけ」版なので advise に吸う。
       別名は dispatch で解決する。NULL なら別名なし。 */
    const char *alias_of;
    int (*fn)(int argc, char **argv);  /* 実装。NULL は未接続 */
} nx_cli_cmd_t;

/* 全コマンド。終端は name == NULL。 */
extern const nx_cli_cmd_t nx_cli_cmds[];

/* 名前で引く。無ければ NULL。 */
const nx_cli_cmd_t *nx_cli_find(const char *name);

/* 別名を本体に解決する。別名でなければそのまま。 */
const nx_cli_cmd_t *nx_cli_resolve(const nx_cli_cmd_t *cmd);

/* コマンドが takes にそのオプションを持つか。 */
int nx_cli_takes(const nx_cli_cmd_t *cmd, const char *opt);

/* ---- オプションの解析 (一元化したもの) ----
 * 各コマンドの実装は、生の argv を走査せず、この ctx を通して読む。
 * 宣言されていないオプションを渡されたら 0 以外を返して気付けるようにする。
 */
typedef struct nx_cli_ctx {
    int argc;
    char **argv;
    const nx_cli_cmd_t *cmd;
} nx_cli_ctx;

/* --name の値。無ければ dflt。 */
const char *nx_cli_val(const nx_cli_ctx *c, const char *name, const char *dflt);

/* --name が付いているか。 */
int nx_cli_flag(const nx_cli_ctx *c, const char *name);

/* argv に、そのコマンドが宣言していないオプションが混ざっていないか調べる。
   混ざっていれば stderr に出し、0 以外を返す。 */
int nx_cli_check_opts(const nx_cli_ctx *c);

/* usage を 1 行ずつ出す。group で絞れる (NULL なら全部)。 */
void nx_cli_usage(void *out_file, const char *group);

#if defined(__cplusplus)
}
#endif

#endif /* NX_CLI_REGISTRY_H */
