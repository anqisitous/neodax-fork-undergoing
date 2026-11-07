#ifndef NX_DEEPAI_H
#define NX_DEEPAI_H

/*
 * nx_deepai — Electron/DeepAI Code 向けの「実行時の振る舞い」解析と
 *             スタックした workspace タブの修復機能。
 *
 * 背景 (今回の事象):
 *   DeepAI Code の workspace タブ帯には kind:"surface" のタブ (Diff / ファイル
 *   プレビュー) が localStorage に永続化される。surface タブを閉じる処理は
 *   「現在のスレッド (currentThreadRef) が、そのタブの所有スレッド
 *   (threadRef) と一致すること」を要求する。別スレッド/別環境に属する
 *   タブは解決できず、× を押しても閉じられない (stale タブ)。
 *
 *   本モジュールは
 *     1. app.asar を解析し、タブ閉じ処理の実挙動 (状態機械) を明示する。
 *     2. 実際の workspace-tabs 状態を評価し、どのタブが閉じられないかを
 *        判定して、所有スレッドを失った surface タブを除去した修復 JSON を
 *        生成する。
 *     3. C/C++/ObjC++ のソーステキストからも実挙動 (window/table/bridge 呼び出し)
 *        を抽出する。
 */

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- asar ---- */
typedef struct {
    char  *path;
    size_t size;
    size_t offset;      /* data_base からの相対 */
} nx_asar_entry_t;

typedef struct {
    char            *path;
    unsigned char   *data;
    size_t           size;
    nx_asar_entry_t *entries;
    size_t           n_entries;
    size_t           data_base;   /* 実データ開始オフセット */
} nx_asar_t;

nx_asar_t *nx_asar_open(const char *path);
void       nx_asar_free(nx_asar_t *a);
const nx_asar_entry_t *nx_asar_find(const nx_asar_t *a, const char *substr);
/* エントリ本体を NUL 終端付きで複製 (呼び出し側 free) */
char      *nx_asar_read_text(const nx_asar_t *a, const nx_asar_entry_t *e);

/* ---- 解析 (実挙動の可視化) ---- */
/* asar を解析し、タブ閉じ状態機械 + 現在の tabs_json 状態を評価して出力。
   tabs_json は NULL 可 (その場合は asar 単体の挙動のみ)。 */
int nx_deepai_analyze_asar(const char *asar_path, const char *tabs_json, FILE *out);

/* C/C++/ObjC++ テキストの実挙動を抽出 */
int nx_deepai_analyze_source(const char *src_path, FILE *out);

/* ---- 修復 ---- */
/* workspace-tabs の JSON を評価し、所有スレッドを失った surface タブを除いた
   修復 JSON 文字列を *out に返す (free は呼び出し側)。除去件数を *n_removed。 */
int nx_deepai_fix_json(const char *json, size_t len,
                       char **out, int *n_removed, FILE *log);

/* 閉じられない (所有スレッドが生存していない) surface タブを検出して out に
   出力する。状態は変更しない。戻り値は検出件数 (エラー時 -1)。 */
int nx_deepai_detect_json(const char *json, size_t len, FILE *out);

/* ファイル or leveldb ディレクトリから状態を抽出して修復。
   fixed JSON を <path>.fixed.json に書き、ログを out に出す。 */
int nx_deepai_fix_path(const char *path, FILE *out);

/* ファイル or leveldb ディレクトリから workspace-tabs の JSON を得る。
   成功時は malloc 済み文字列 (呼び出し側 free)。 */
char *nx_deepai_load_state(const char *path);

/* 指定した id のタブだけを除去する (base の component/他タブは保持)。
   activeId / recentIds の参照だけ整合を取る。 */
int nx_deepai_remove_tab_json(const char *json, size_t len, const char *tab_id,
                              char **out, int *n_removed, FILE *log);

/* ファイル or leveldb から状態を読み、tab_id を除去して
   <path>.fixed.json に書き出す。 */
int nx_deepai_remove_tab_path(const char *path, const char *tab_id, FILE *out);

/* タブ一覧を機械可読 (TSV) で出力する。エージェント/CLI 用。 */
int nx_deepai_dump_state(const char *json, FILE *out);

/* 実キャプチャを撮らずに、状態から「画面の再現レイアウト」を組み立てる。
   行を line 配列として返し、GUI (CGContext) と CLI (テキスト) が共有する。 */
typedef struct { int x, y, w, h, kind; char label[96]; } nx_ui_node_t;
typedef struct { int x1, y1, x2, y2, a, b; } nx_ui_edge_t;
typedef struct {
    int width, height;
    char title[160];
    nx_ui_node_t *nodes;  int n_nodes;
    nx_ui_edge_t *edges;  int n_edges;
    int tab_index[64];    int n_tabs;      /* nodes のうち tab である添字 */
    int active_tab;                       /* tab_index 内の位置 (なければ -1) */
    int stuck_tab[64];    int n_stuck;     /* nodes のうち閉じられない tab */
} nx_ui_layout_t;

/* kind: 0=input,1=button,2=tab_active,3=tab_other,4=tab_stuck,5=table,6=text */
int  nx_deepai_build_layout(const char *json, nx_ui_layout_t *out);
void nx_deepai_free_layout(nx_ui_layout_t *l);

/* レイアウトを可読ダイアグラム (CLI) として出力。 */
int nx_deepai_render_ascii(const nx_ui_layout_t *l, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_DEEPAI_H */
