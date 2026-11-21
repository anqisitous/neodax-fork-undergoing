#ifndef NX_UI_HPP
#define NX_UI_HPP

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UI 挙動の静的抽出。app.asar / ディレクトリを読み、操作→コマンド→状態→
   永続キーの関係を ui.tsv / ui.node / ui.cpp / ui.hex に保存する。
   アプリは起動しない。 */
int nx_ui_scan(const char *app_path, const char *out_dir, int depth,
               const char *include, FILE *out);

/* 抽出済みの ui.tsv を読み、操作のアルゴリズムを出す。
   action が無ければ match でランキングし、それも無ければ inventory。 */
int nx_ui_flow(const char *dir, const char *app, const char *action,
               const char *match, const char *format, int depth, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_UI_HPP */
