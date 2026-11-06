#ifndef NX_INSPECT_H
#define NX_INSPECT_H

/*
 * nx_inspect — 稼働中のプロセスへ「非正規経路」でアタッチして JS を実行する
 *              汎用ランタイムドライバ。
 *
 * 用途を限定しない。Node/Electron を内蔵する任意のプロセスに対して:
 *   1. プロセス名から PID を解決 (macOS: libproc)
 *   2. SIGUSR1 を送り Node/V8 inspector を起動させる
 *   3. inspector の HTTP エンドポイントから WebSocket URL を取得
 *   4. WebSocket で接続し Runtime.evaluate で式を評価する
 *
 * 「何を実行するか」は呼び出し側が expression として与える (payload 分離)。
 * アプリ固有の知識はここに持たない。
 */

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int         port;          /* inspector ポート。0 なら 9229 */
    const char *app_name;      /* プロセス名の一部。NULL なら "Electron" */
    int         wait_ms;       /* inspector 起動待ちの上限 (ms)。0 なら 4000 */
} nx_inspect_opts_t;

/* プロセス名の一部から PID を返す。見つからなければ -1。 */
long nx_inspect_find_pid(const char *app_name);

/* PID へ SIGUSR1 を送り、inspector が port で応答するまで待つ。
   成功で 0、失敗で負値。 */
int nx_inspect_start(long pid, int port, int wait_ms, FILE *out);

/* 指定ポートの inspector に対して 1 つの式を評価し、結果 JSON を out に出す。
   expression は self-contained な JS。成功で 0。 */
int nx_inspect_eval(int port, const char *expression, FILE *out);

/* 上記をまとめて実行する簡易エントリ。
   成功で 0。result_out に結果 (malloc) を返すこともできる。 */
int nx_inspect_run(const nx_inspect_opts_t *opts, const char *expression,
                   char **result_out, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_INSPECT_H */
