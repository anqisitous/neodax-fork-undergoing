#ifndef NX_BESTFIX_HPP
#define NX_BESTFIX_HPP

/*
 * nx_bestfix — 「動かない」問題を 1 個だけ特定し、その 1 個に最善を尽くす。
 *
 * 方針:
 *   - 全部を一度に直さない。まず問題を 1 つに絞る。
 *   - 絞った問題に対して、修復候補を確度順に並べ、確実なものから試す。
 *   - 1 つ成功したら止まる (総当たりしない)。
 *   - 失敗したら次の候補へ (同じ問題の中だけ)。
 *
 * 候補の確度:
 *   license.should-check-gate  ShouldCheckLicense   (確認済み・最優先)
 *   license.root-spawns        AdobeGCInvokeClient → InvokeGoCart
 *   license.exit-on-signin     DoSetup              (最後の手段)
 *   token 系はコードで直せないので選ばない (報告のみ)。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

/* app: 対象 .app。kind: "app" など。apply=0 は計画のみ、1 で実行。 */
int nx_bestfix(const char *app, const char *kind, int apply, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_BESTFIX_HPP */
