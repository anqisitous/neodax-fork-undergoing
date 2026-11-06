#ifndef NX_SIM_H
#define NX_SIM_H

/*
 * nx_sim — アプリの「ロード結果」と「タブ操作の実行結果」を再現する汎用シミュレータ。
 *
 * asar 専用ではない。任意のアプリについて
 *   永続化されたタブ状態 (JSON)  +  振る舞いプロファイル (profile)
 * を入力に、
 *   1. アプリ起動時の migrate / validate  (不正タブ・重複・上限の除去)
 *   2. タブ帯の集約 (どの owner の surface を表示するか)
 *   3. タブを閉じたときの実行パス (precondition -> close / abort)
 * を順に再現し、結果 (閉じられる / スタックする) を出力する。
 *
 * profile はアプリごとに差し替え可能。DeepAI Code (Electron) の asar は
 * profile を「抽出する」任意の供給源として扱う (無くても組み込み既定で動く)。
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;

    /* target オブジェクト内のキー名 */
    const char *kind_key;        /* 例 "kind"                          */
    const char *surface_key;     /* 例 "surfaceId"                     */
    const char *owner_key;       /* 例 "threadRef"                     */
    const char *owner_env_key;   /* 例 "environmentId"                 */
    const char *owner_thread_key;/* 例 "threadId"                      */

    /* 値の分類 (カンマ区切り) */
    const char *chat_kinds;      /* owner を持つ "会話" kind            */
    const char *surface_kinds;   /* owner を持つ "面" kind              */

    /* 閉じる precondition:
       0 = owner が現在の会話と一致すること (close は owner に依存)
       1 = 常に閉じられる                                       */
    int  close_requires_owner;
} nx_sim_profile_t;

/* 組み込みプロファイル */
const nx_sim_profile_t *nx_sim_profile_default(void);   /* generic-tabs   */
const nx_sim_profile_t *nx_sim_profile_deepai(void);    /* electron 系    */

/* json: タブ状態 ({"state":{"tabs":[...]}} または {"tabs":[...]})。
   asar_path は profile 抽出用 (NULL 可)。
   返回: 0 成功。 */
int nx_sim_run(const char *state_json, const nx_sim_profile_t *profile,
               const char *asar_path, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_SIM_H */
