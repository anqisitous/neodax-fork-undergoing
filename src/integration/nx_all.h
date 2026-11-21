/* ================================================================
 * neodax のクラス分類 (集成ヘッダ)
 *
 * 目的:
 *   「どの実装をリンクするか」をビルドファイルに書かない。
 *   コードの中で「どの分類を使うか」を include で宣言する。
 *
 * 使い方:
 *      #include "integration/nx_all.h"   // 全分類 (CLI/GUI の入口)
 *      #include "integration/nx_plan.h"  // 計画だけを使う
 *      #include "integration/nx_clean.h" // 残滓の観測と除去だけ
 *
 * 分類と対応する中身:
 *   nx_state.h   状態の観測・キー解決 (形式非依存のロケータ)
 *   nx_plan.h    観測→問題→計画→検証の骨格 (plan/, template/)
 *   nx_recipe.h  手順書の保存と段ごとの検証 (recipe/)
 *   nx_search.h  介入戦略の探索 (search/)
 *   nx_clean.h   残滓の分類と除去 (clean/)
 *   nx_app.h     app 単位の分析と最善 (app/)
 *   nx_spec.h    構造認識・生成 (spec/)
 *   nx_attach.h  稼働プロセスへの反映 (attach/)
 *
 * ビルドは「分類ごとの静的ライブラリ」を作り、リンク時に必要なものだけ
 * 引き抜かれる (アーカイブ単位)。どの分類を使うかは「入口がどれを
 * include したか」で決まるので、リストを 2 箇所に持たない。
 * ================================================================ */

#ifndef NX_ALL_H
#define NX_ALL_H

/* --- 基盤 (状態の観測・JSON・LevelDB) --- */
#include "nx_json.h"
#include "nx_leveldb.h"
#include "nx_state.h"
#include "nx_deepai.h"
#include "nx_sim.h"

/* --- 判断 (観測 → 問題 → 計画 → 検証) --- */
#include "plan/nx_plan.hpp"
#include "plan/nx_case.hpp"
#include "plan/nx_template.hpp"

/* --- 手順書 (保存 → 段ごとの検証 → 実行) --- */
#include "plan/nx_recipe.hpp"

/* --- 探索 (介入戦略の一般探索) --- */
#include "plan/nx_strategy.hpp"

/* --- 残滓 (観測 → 分類 → 除去) --- */
#include "plan/nx_residual.hpp"
#include "plan/nx_clean.hpp"

/* --- 永続層の事実と判断 (状態の見方を 1 箇所に固定する) --- */
#include "plan/nx_store_facts.hpp"

/* --- コマンドをノードから呼ぶ橋 (実装を 2 つ持たない) --- */
#include "plan/nx_node.hpp"

/* --- board 探索 (2 人の player が finish を競う) --- */
#include "plan/nx_arena.hpp"

/* --- 分析をプロジェクトファイルに保存する --- */
#include "plan/nx_project.h"

/* --- テンプレート = C++ ファイル (構成はヘッダとして cpp 内に逆順で) --- */
#include "plan/nx_tplcpp.hpp"

/* --- UI 挙動の静的抽出 (asar/JS -> action/command/state/persist) --- */
#include "plan/nx_ui.hpp"

/* --- チャット (thread) 台帳: 重複検出と恒久削除 --- */
#include "nx_threads.h"

/* --- 状態から修復手順を導出する助言 --- */
#include "plan/nx_advisor.hpp"

/* --- app 単位 (分析の保存・1 個に絞って最善) --- */
#include "plan/nx_appstore.hpp"
#include "plan/nx_bestfix.hpp"

/* --- 稼働プロセスへの反映 (reload しない) --- */
#include "attach/nx_appfix.h"
#include "attach/nx_inspect.h"
#include "attach/nx_renderer.h"
#include "attach/nx_payload.h"

/* --- 構造認識・生成 --- */
#include "spec/nx_spec.h"
#include "spec/nx_specs_builtin.h"
#include "spec/nx_recog.h"
#include "spec/nx_gen.h"
#include "spec/nx_pe.h"
#include "spec/nx_pe_disasm.h"
#include "spec/nx_pipeline.h"

#endif /* NX_ALL_H */
