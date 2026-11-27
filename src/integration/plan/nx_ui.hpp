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

/* ノードの作成・編集。ui.tsv / ui.node を直接書き換えるので、
   直後に nx_ui_flow(..., "cpp") を呼べば反映済みの cpp が出る。
   dir が空なら app の保存フォルダ (default_dir) を使う。 */
int nx_ui_node_new(const char *dir, const char *app, const char *kind,
                   const char *key, const char *label, FILE *out);
int nx_ui_node_set(const char *dir, const char *app, const char *kind,
                   const char *key, const char *label, FILE *out);
int nx_ui_node_link(const char *dir, const char *app, const char *from,
                    const char *kind, const char *to, FILE *out);
int nx_ui_node_rm(const char *dir, const char *app, const char *key,
                  int cascade, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_UI_HPP */
