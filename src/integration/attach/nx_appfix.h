#ifndef NX_APPFIX_H
#define NX_APPFIX_H

/*
 * nx_appfix — attach パッケージの入口。
 *
 *   nx_inspect   プロセス解決 + SIGUSR1 + inspector
 *   nx_renderer  webContents で JS 実行
 *   nx_payload   実行する JS の生成
 *   nx_appfix    上記をつなぐ「アプリに状態を再適用する」高レベル API
 *
 * アプリ固有の知識は nx_appfix_profile_t に集約し、既定値は DeepAI Code。
 * 別アプリは profile を差し替えるだけで使える (汎用)。
 */

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *app_name;      /* プロセス名の一部 (既定 "DeepAI Code") */
    int         port;          /* inspector ポート (既定 9229) */
    int         window_index;  /* BrowserWindow 添字 (既定 0) */
    const char *store_substr;  /* ストアのアセット名の一部 (既定 "WorkspaceTabStore") */
    const char *store_exports; /* ストアの export 名候補 (既定 "n,default") */
    const char *key;           /* localStorage キー (既定 "deepai-code:workspace-tabs:v1") */
} nx_appfix_profile_t;

const nx_appfix_profile_t *nx_appfix_default_profile(void);

/* 修復済み JSON (value) をアプリの localStorage に書き、ストアを再水和する。
   value が NULL なら現状の localStorage で再水和のみ。
   アプリを終了・リロードしない。成功で 0。 */
int nx_appfix_apply(const nx_appfix_profile_t *profile,
                    const char *value, size_t value_len, FILE *out);

/* key を削除して再水和する。 */
int nx_appfix_remove(const nx_appfix_profile_t *profile, FILE *out);

/* key の現在値を読む (out に出力)。 */
int nx_appfix_read(const nx_appfix_profile_t *profile, FILE *out);

/* 起動時の状態復元を止める。profile が指すストアと localStorage キーから
   復元用の永続状態を消す。次回起動は既定状態になる (再起動なしで反映される)。 */
int nx_appfix_stop_restore(const nx_appfix_profile_t *profile, FILE *out);

/* 復元先を「空の妥当な状態」に置き換える。
   ブロックではなく置換なので、アプリの復元経路はそのまま正常に走る。
   元の値は <key>.nx-backup に退避され、後で戻せる。 */
int nx_appfix_baseline_restore(const nx_appfix_profile_t *profile, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_APPFIX_H */
