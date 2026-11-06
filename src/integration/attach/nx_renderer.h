#ifndef NX_RENDERER_H
#define NX_RENDERER_H

/*
 * nx_renderer — inspector 経由で「レンダラ (webContents) の JS」を実行する汎用層。
 *
 * Electron の main プロセスにアタッチしたうえで、
 *   process.mainModule.require('electron').BrowserWindow.getAllWindows()[...]
 * を選び、その webContents.executeJavaScript を呼ぶ式を組み立てる。
 *
 * アプリ固有の処理 (何を書き換えるか) は payload として JS 文字列で渡す。
 */

#include <stdio.h>
#include <stddef.h>
#include "nx_inspect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nx_inspect_opts_t inspect;
    int  window_index;   /* BrowserWindow.getAllWindows() の添字。0 で先頭 */
} nx_renderer_opts_t;

/* payload_js を指定ウィンドウのレンダラで実行する。結果は out に出力。
   成功で 0。 */
int nx_renderer_eval_js(const nx_renderer_opts_t *opts, const char *payload_js,
                        FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_RENDERER_H */
