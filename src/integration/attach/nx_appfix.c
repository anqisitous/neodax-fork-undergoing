#include "nx_appfix.h"
#include "nx_renderer.h"
#include "nx_payload.h"
#include "nx_inspect.h"
#include "../nx_deepai.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const nx_appfix_profile_t *nx_appfix_default_profile(void) {
    static const nx_appfix_profile_t p = {
        "DeepAI Code", 9229, 0,
        "WorkspaceTabs", "n,default",
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
    /* 既定は全ウィンドウ同期 (復活防止)。プロファイルが >=0 ならその窓のみ。 */
    ro.window_index = p->window_index ? p->window_index : -1;
    int rc = nx_renderer_eval_js(&ro, js, out);
    free(js);
    return rc;
}

/* ストア定義を含む JS チャンク名を asar から求める。
   レンダラはエントリ JS 1 本しか resource に出さないことがあるので、
   URL 列挙だけではストアのチャンクに届かない。asar を C 側で読み、
   キー文字列を含む .js エントリの basename をヒントとして渡す。 */
static void derive_module_hint(const nx_appfix_profile_t *p, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!p || !p->app_name || !p->key) return;

    long pid = nx_inspect_find_pid(p->app_name);
    if (pid <= 0) return;
    char exe[4096];
    if (nx_os_executable_path(pid, exe, sizeof(exe)) != 0) return;

    /* .../Contents/MacOS/<exe> -> .../Contents/Resources/app.asar */
    char *m = strstr(exe, "/Contents/MacOS/");
    if (!m) return;
    char asar[4400];
    snprintf(asar, sizeof(asar), "%.*s/Contents/Resources/app.asar",
             (int)(m - exe), exe);

    nx_asar_t *a = nx_asar_open(asar);
    if (!a) return;
    for (size_t i = 0; i < a->n_entries; i++) {
        const char *path = a->entries[i].path;
        if (!path) continue;
        if (!strstr(path, "/assets/")) continue;
        size_t n = strlen(path);
        if (n < 3 || strcmp(path + n - 3, ".js") != 0) continue;
        if (a->entries[i].size > 4u * 1024 * 1024) continue;
        char *txt = nx_asar_read_text(a, &a->entries[i]);
        if (!txt) continue;
        if (strstr(txt, p->key)) {
            const char *base = strrchr(path, '/');
            snprintf(out, cap, "%s", base ? base + 1 : path);
        }
        free(txt);
        if (out[0]) break;
    }
    nx_asar_free(a);
}

int nx_appfix_apply(const nx_appfix_profile_t *in,
                    const char *value, size_t value_len, FILE *out) {
    nx_appfix_profile_t p = merged(in);
    char hint[256];
    derive_module_hint(&p, hint, sizeof(hint));
    nx_payload_set_module_hint(hint);
    if (hint[0]) fprintf(out, "  [appfix] store module: %s\n", hint);
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

int nx_appfix_stop_restore(const nx_appfix_profile_t *in, FILE *out) {
    nx_appfix_profile_t p = merged(in);
    char *js = nx_payload_build_stop_restore(p.store_substr, p.store_exports, p.key);
    return run_payload(&p, js, out);
}

int nx_appfix_baseline_restore(const nx_appfix_profile_t *in, FILE *out) {
    nx_appfix_profile_t p = merged(in);
    /* backup=0: 退避を作らない。作ると次回の復元で戻る。 */
    char *js = nx_payload_build_baseline_restore(p.store_substr, p.store_exports, p.key, 0);
    return run_payload(&p, js, out);
}
