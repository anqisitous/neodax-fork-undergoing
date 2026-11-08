#ifndef NX_UPDATE_H
#define NX_UPDATE_H

/*
 * nx_update — 署名済みアプリの更新チャネルを neodax 自身で読む。
 *
 * /Applications 配下の .app は codesign で封印されている。中のファイルを
 * 直接書き換えると封印が壊れるため、変更はアプリの更新機構が使う
 * release feed (app-update.yml -> latest-mac.yml) を通すのが正しい。
 *
 * ここでは feed の取得と版の比較までを行う。書き換えはしない。
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *provider;   /* "generic" など */
    char *url;        /* feed の基底 URL */
    char *channel;    /* "latest" など */
} nx_update_feed_t;

/* app-update.yml を読む。成功で 0。 */
int  nx_update_read_feed(const char *app_path, nx_update_feed_t *out, FILE *log);
void nx_update_feed_free(nx_update_feed_t *f);

/* feed を取得して版を比べる。apply は受け取るが封印済み bundle は書き換えない。
   戻り値: 0 = 取得成功, 2 = チャネル不明/未到達。 */
int  nx_update_check(const char *app_path, int apply, FILE *out);

/* feed から arm64 の配布物を取り、sha512 を照合する。
   dest に保存するだけ。アプリの差し替えはしない。 */
int  nx_update_fetch(const char *app_path, const char *dest, int apply, FILE *out);

#ifdef __cplusplus
}
#endif

#endif
