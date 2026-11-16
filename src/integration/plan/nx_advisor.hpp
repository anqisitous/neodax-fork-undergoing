#ifndef NX_ADVISOR_HPP
#define NX_ADVISOR_HPP

/*
 * nx_advisor — 状態から「タブ修復の手順」を neodax 自身が導出する。
 *
 * これまでの repair / clean は「決まった段」を実行するだけで、
 * なぜその段なのかを状態から説明していなかった。advisor は
 *   観測 → 問題の種類 (orphan / identity / duplicate / empty)
 *        → 安全な順の手順 (STEP) → 根拠 (ITEM) → 信頼度
 * を出し、--apply では既存の clean (唯一の永続手続き) に委ねる。
 * 個別のタブ名やアプリ名には依存しない。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

int nx_advisor_derive(const char *state_path, FILE *out);
int nx_advisor_run(const char *state_path, int apply, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_ADVISOR_HPP */
