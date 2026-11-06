#include "nx_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* C 文字列を JSON 文字列リテラルに (nx_inspect と同等の最小実装) */
static char *jes(const char *s) {
    if (!s) return NULL;
    size_t cap = strlen(s) * 2 + 8, o = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[o++] = '"';
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
    return out;
}

int nx_renderer_eval_js(const nx_renderer_opts_t *opts, const char *payload_js,
                        FILE *out) {
    if (!out) out = stdout;
    if (!payload_js) return -1;

    int widx = opts ? opts->window_index : 0;
    if (widx < 0) widx = 0;

    /* レンダラ側で実行する JS のソースを JSON 文字列化 */
    char *jsjson = jes(payload_js);
    if (!jsjson) return -1;

    /* main プロセス側の式:
       Electron を取得し、対象ウィンドウの webContents に executeJavaScript する。
       結果はグローバル __NX_RESULT に格納し、後で読めるようにする。 */
    size_t cap = strlen(jsjson) + 1024;
    char *expr = (char *)malloc(cap);
    if (!expr) { free(jsjson); return -1; }
    snprintf(expr, cap,
        "(function(){try{"
        "const e=process.mainModule.require('electron');"
        "const ws=e.BrowserWindow.getAllWindows();"
        "const w=ws[%d];"
        "if(!w)return JSON.stringify({err:'no window at index %d',count:ws.length});"
        "globalThis.__NX_RESULT=null;"
        "w.webContents.executeJavaScript(%s).then(v=>{globalThis.__NX_RESULT=v})"
        ".catch(err=>{globalThis.__NX_RESULT=JSON.stringify({err:'executeJavaScript failed: '+err.message})});"
        "return 'sent'"
        "}catch(err){return JSON.stringify({err:'main: '+err.message})}})()",
        widx, widx, jsjson);
    free(jsjson);
    if (!expr) return -1;

    /* 実行 (結果はレンダラへ非同期。ここでは送信完了のみ確認) */
    nx_inspect_opts_t io;
    if (opts) io = opts->inspect; else { memset(&io, 0, sizeof(io)); }
    if (!io.app_name) io.app_name = "Electron";

    long pid = nx_inspect_find_pid(io.app_name);
    if (pid <= 0) { fprintf(out, "  [renderer] process not found: %s\n", io.app_name); free(expr); return -1; }
    fprintf(out, "  [renderer] target pid=%ld (%s)\n", pid, io.app_name);
    if (nx_inspect_start(pid, io.port, io.wait_ms, out) != 0) {
        fprintf(out, "  [renderer] inspector did not start\n");
        free(expr);
        return -1;
    }
    fprintf(out, "  [renderer] inspector up on :%d\n", io.port ? io.port : 9229);

    if (nx_inspect_eval(io.port, expr, out) != 0) { free(expr); return -1; }
    free(expr);

    /* 非同期結果 __NX_RESULT を読む。値が入るまで少し待って再試行。 */
    const char *poll = "globalThis.__NX_RESULT";
    for (int i = 0; i < 30; i++) {
        nx_inspect_eval(io.port, poll, out);
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
