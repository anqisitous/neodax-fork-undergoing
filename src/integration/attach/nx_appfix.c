#include "nx_appfix.h"
#include "nx_renderer.h"
#include "nx_payload.h"

#include <stdlib.h>
#include <string.h>

const nx_appfix_profile_t *nx_appfix_default_profile(void) {
    static const nx_appfix_profile_t p = {
        "DeepAI Code", 9229, 0,
        "WorkspaceTabStore", "n,default",
        "deepai-code:workspace-tabs:v1"
    };
    return &p;
}

static nx_appfix_profile_t merged(const nx_appfix_profile_t *in) {
    nx_appfix_profile_t p = *nx_appfix_default_profile();
    if (in) {
        if (in->app_name) p.app_name = in->app_name;
        if (in->port) p.port = in->port;
        p.window_index = in->window_index;
        if (in->store_substr) p.store_substr = in->store_substr;
        if (in->store_exports) p.store_exports = in->store_exports;
        if (in->key) p.key = in->key;
    }
    return p;
}

static int run_payload(const nx_appfix_profile_t *p, char *js, FILE *out) {
    if (!js) { fprintf(out, "  [appfix] failed to build payload\n"); return -1; }
    nx_renderer_opts_t ro;
    memset(&ro, 0, sizeof(ro));
    ro.inspect.app_name = p->app_name;
    ro.inspect.port = p->port;
    ro.window_index = p->window_index;
    int rc = nx_renderer_eval_js(&ro, js, out);
    free(js);
    return rc;
}

int nx_appfix_apply(const nx_appfix_profile_t *in,
                    const char *value, size_t value_len, FILE *out) {
    nx_appfix_profile_t p = merged(in);
    char *js = nx_payload_build_rehydrate(p.store_substr, p.key,
                                          value, value_len, p.store_exports);
    return run_payload(&p, js, out);
}

int nx_appfix_remove(const nx_appfix_profile_t *in, FILE *out) {
    nx_appfix_profile_t p = merged(in);
    char *js = nx_payload_build_remove_key(p.store_substr, p.key, p.store_exports);
    return run_payload(&p, js, out);
}

int nx_appfix_read(const nx_appfix_profile_t *in, FILE *out) {
    nx_appfix_profile_t p = merged(in);
    char *js = nx_payload_build_read_key(p.key);
    return run_payload(&p, js, out);
}
