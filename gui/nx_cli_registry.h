#ifndef NX_CLI_REGISTRY_H
#define NX_CLI_REGISTRY_H

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct nx_cli_ctx nx_cli_ctx_t;

typedef struct {
    const char *name;
    const char *title;
    const char *group;
    const char *takes;
    const char *desc;
    const char *alias_of;
    int (*fn)(int argc, char **argv);
} nx_cli_cmd_t;

extern const nx_cli_cmd_t nx_cli_cmds[];

const nx_cli_cmd_t *nx_cli_find(const char *name);
const nx_cli_cmd_t *nx_cli_resolve(const nx_cli_cmd_t *cmd);
int nx_cli_takes(const nx_cli_cmd_t *cmd, const char *opt);

typedef struct nx_cli_ctx {
    int argc;
    char **argv;
    const nx_cli_cmd_t *cmd;
} nx_cli_ctx;

const char *nx_cli_val(const nx_cli_ctx *c, const char *name, const char *dflt);
int nx_cli_flag(const nx_cli_ctx *c, const char *name);
int nx_cli_check_opts(const nx_cli_ctx *c);
void nx_cli_usage(void *out_file, const char *group);

#if defined(__cplusplus)
}
#endif

#endif
