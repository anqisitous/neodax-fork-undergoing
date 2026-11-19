// ===== NX TEMPLATE =====
// id: tab-restore.audit
// kind: state+code
// target: deepai-code://app
// learn: graph=tab-restore-v1
//
// タブ復元の全経路を、タブ専用にせずストア非依存で表す。
// fix / delete / clean とは別のアプローチ: ここは読むだけで、
// 観測 → 分類 → 計画 までを 1 つのノードグラフにする。
// 書き込みは既存の fix / delete / clean に任せる。
//
// node: audit.observe  全 localStorage キーと seq を列挙       run: keys
// node: audit.tabs     表示中のタブと owner を読む            run: tabs
// node: audit.decode   全ストアを型付きで読む                 run: dump
// node: audit.ledger   生存 thread 台帳を読む                 run: threads
// node: audit.detect   owner の無い参照 / 死んだ threadRef を数える run: advise
// node: audit.check    観測 TSV を Checker に渡す             run: splice
// node: audit.layout   アプリのタブ表示 (置き換えない)        layout: tabs
//
// link: audit.observe converges audit.tabs
// link: audit.tabs converges audit.decode
// link: audit.decode converges audit.ledger
// link: audit.ledger converges audit.detect
// link: audit.detect converges audit.check
// link: audit.check renders audit.layout
//
// データノード (下): 全 15 キー、全ストア、死んだ threadRef。
// タブ以外の残滓も同じ detect で拾う。したがって、表示タブに限らず
// 「参照先が消えているデータ」を全ストアで監査できる。

/*
 * この .cpp はビルド対象ではない。neodax (tpl-cpp / trace / resolve) が
 * 構成を読み、run: を既存コマンドへ解決する。分析用であり、アプリの
 * レイアウトを置き換えない。
 *
 * fix との違い:
 *   fix      = 状態を書く (修復)。
 *   tab-restore.audit = 書かない。コードとデータを同じグラフに載せ、
 *              detect が「どのストアのどの参照が死んでいるか」を出す。
 *              書き込みが要る場合だけ、既存の fix / delete / clean を呼ぶ。
 *
 * 重要: surface の保持元 (byThreadKey) を消さずに表示側だけを消しても、
 * WorkspaceTabs の effect が surfaces から include() するので戻る。
 * これはタブ専用の話ではなく「参照先を消したのに参照元を消していない」
 * という一般形。detect は全ストアを同じ規則で見る。
 */

#include <cstdio>

/* audit.observe: キーと seq を列挙する。状態は書かない。 */
static int tpl_audit_observe(const char *state) { (void)state; return 0; }

/* audit.tabs: 表示タブと owner を読む。 */
static int tpl_audit_tabs(const char *state) { (void)state; return 0; }

/* audit.decode: 各ストアを読み、参照を列挙する。 */
static int tpl_audit_decode(const char *state) { (void)state; return 0; }

/* audit.ledger: 生存 thread を引く。 */
static int tpl_audit_ledger(const char *db) { (void)db; return 0; }

/* audit.detect: 死んだ参照を数える。 */
static int tpl_audit_detect(const char *state) { (void)state; return 0; }

/* audit.check: 観測を TSV として Checker に渡す。 */
static int tpl_audit_check(const char *state) { (void)state; return 0; }
// node: store.t3code_diff_panel_state_v1 "t3code:diff-panel-state:v1"
// node: dead.t3code_diff_panel_state_v1.1 "28b4c9f1-1b76-47f0-8572-a5148135faa6:336748f1-2242-408f-9119-2e63b69501c6 DEAD thread=336748f1-2242-408f-9119-2e63b69501c6 byThreadKey"
// link: store.t3code_diff_panel_state_v1 owns dead.t3code_diff_panel_state_v1.1
// node: dead.t3code_diff_panel_state_v1.2 "28b4c9f1-1b76-47f0-8572-a5148135faa6:8112363b-95fa-4d84-8a28-d0679bd2bd6b DEAD thread=8112363b-95fa-4d84-8a28-d0679bd2bd6b byThreadKey"
// link: store.t3code_diff_panel_state_v1 owns dead.t3code_diff_panel_state_v1.2
// node: dead.t3code_diff_panel_state_v1.3 "d3d0c88e-4bd4-46ef-ad24-1fac103a9fbe:0a3ad361-5c9e-4ba8-b516-f65d90e2a565 DEAD thread=0a3ad361-5c9e-4ba8-b516-f65d90e2a565 byThreadKey"
// link: store.t3code_diff_panel_state_v1 owns dead.t3code_diff_panel_state_v1.3
// node: dead.t3code_diff_panel_state_v1.4 "d3d0c88e-4bd4-46ef-ad24-1fac103a9fbe:0a3ad361-5c9e-4ba8-b516-f65d90e2a565 DEAD thread=0a3ad361-5c9e-4ba8-b516-f65d90e2a565 branchBaseRefByThreadKey"
// link: store.t3code_diff_panel_state_v1 owns dead.t3code_diff_panel_state_v1.4
// node: store.t3code_composer_drafts_v1 "t3code:composer-drafts:v1"
// node: dead.t3code_composer_drafts_v1.5 "28b4c9f1-1b76-47f0-8572-a5148135faa6:2561e026-d1fd-4f92-a455-c1438f796628 DEAD thread=2561e026-d1fd-4f92-a455-c1438f796628 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.5
// node: dead.t3code_composer_drafts_v1.6 "28b4c9f1-1b76-47f0-8572-a5148135faa6:88d6eb15-f02f-4cc3-a771-42c97b5f6430 DEAD thread=88d6eb15-f02f-4cc3-a771-42c97b5f6430 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.6
// node: dead.t3code_composer_drafts_v1.7 "28b4c9f1-1b76-47f0-8572-a5148135faa6:6cf6b459-31da-4b78-a498-361b589aceab DEAD thread=6cf6b459-31da-4b78-a498-361b589aceab draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.7
// node: dead.t3code_composer_drafts_v1.8 "28b4c9f1-1b76-47f0-8572-a5148135faa6:771b9c8e-63c0-41a7-8f44-625eeeaa6673 DEAD thread=771b9c8e-63c0-41a7-8f44-625eeeaa6673 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.8
// node: dead.t3code_composer_drafts_v1.9 "28b4c9f1-1b76-47f0-8572-a5148135faa6:77d5856c-4fc6-4eac-8c0b-3e369384bb4f DEAD thread=77d5856c-4fc6-4eac-8c0b-3e369384bb4f draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.9
// node: dead.t3code_composer_drafts_v1.10 "bdfba710-f83f-45ec-8b90-47d63c8f4176 DEAD thread=bdfba710-f83f-45ec-8b90-47d63c8f4176 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.10
// node: dead.t3code_composer_drafts_v1.11 "a14f71ab-409a-4115-b811-26100195a5dc DEAD thread=a14f71ab-409a-4115-b811-26100195a5dc draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.11
// node: dead.t3code_composer_drafts_v1.12 "832a587e-6b61-475b-8cf5-0dba74a77b94 DEAD thread=832a587e-6b61-475b-8cf5-0dba74a77b94 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.12
// node: dead.t3code_composer_drafts_v1.13 "74319268-2228-4924-bf7d-c79f740700b9 DEAD thread=74319268-2228-4924-bf7d-c79f740700b9 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.13
// node: dead.t3code_composer_drafts_v1.14 "d3d0c88e-4bd4-46ef-ad24-1fac103a9fbe:5fa8c8c4-4f6a-481b-b32a-8fb17d6a8769 DEAD thread=5fa8c8c4-4f6a-481b-b32a-8fb17d6a8769 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.14
// node: dead.t3code_composer_drafts_v1.15 "0fbc08fa-a2bb-481a-87c1-16925dcff77f DEAD thread=0fbc08fa-a2bb-481a-87c1-16925dcff77f draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.15
// node: dead.t3code_composer_drafts_v1.16 "d3d0c88e-4bd4-46ef-ad24-1fac103a9fbe:a66c70a5-472c-4e71-9dfc-0b5806a9bd3a DEAD thread=a66c70a5-472c-4e71-9dfc-0b5806a9bd3a draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.16
// node: dead.t3code_composer_drafts_v1.17 "14fde373-5b3f-4963-833d-bd9e5579bf94 DEAD thread=14fde373-5b3f-4963-833d-bd9e5579bf94 draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.17
// node: dead.t3code_composer_drafts_v1.18 "fb3ebfa4-3da9-427c-b974-7d41f1484ecc DEAD thread=fb3ebfa4-3da9-427c-b974-7d41f1484ecc draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.18
// node: dead.t3code_composer_drafts_v1.19 "db6e7093-bcc9-46ac-848b-4208067336bb DEAD thread=db6e7093-bcc9-46ac-848b-4208067336bb draftsByThreadKey"
// link: store.t3code_composer_drafts_v1 owns dead.t3code_composer_drafts_v1.19
// node: store.t3code_last_editor "t3code:last-editor"
// node: store.t3code_browser_favicons_v1 "t3code:browser-favicons:v1"
// node: dead.t3code_browser_favicons_v1.20 "28b4c9f1-1b76-47f0-8572-a5148135faa6:06501a70-1fa6-456e-908d-0e5c76eda6cf https://www.youtube.com:443 DEAD thread=06501a70-1fa6-456e-908d-0e5c76eda6cf byKey"
// link: store.t3code_browser_favicons_v1 owns dead.t3code_browser_favicons_v1.20
// node: store.t3code_closed_views_v2 "t3code:closed-views:v2"
// node: dead.t3code_closed_views_v2.21 "9e585da3-e31d-4591-a854-d50f05e33dc3 DEAD thread=122c195c-9f45-4ddc-b884-7b552cfad331 chat"
// link: store.t3code_closed_views_v2 owns dead.t3code_closed_views_v2.21
// node: dead.t3code_closed_views_v2.22 "6c3b71a0-1498-4916-955c-ccef64b3809f DEAD thread=6f5029b6-e3a8-40a0-bd76-62508e288b77 new-tab"
// link: store.t3code_closed_views_v2 owns dead.t3code_closed_views_v2.22
// node: dead.t3code_closed_views_v2.23 "647d6217-e101-49ea-8690-6d26d903c32e DEAD thread=8198717a-afc9-4702-9075-590373ccf385 chat"
// link: store.t3code_closed_views_v2 owns dead.t3code_closed_views_v2.23
// node: store.t3_backgroundActivity_clientId "t3.backgroundActivity.clientId"
// node: store.t3code_provider_update_dismissals_v1 "t3code:provider-update-dismissals:v1"
// node: store.deepai_code_workspace_tabs_v1 "deepai-code:workspace-tabs:v1"
// node: store.t3code_renderMarkdown "t3code.renderMarkdown"
// node: store.t3code_diffFileTreeOpen "t3code.diffFileTreeOpen"
// node: store.t3code_terminal_state_v1 "t3code:terminal-state:v1"
// node: store.t3code_ui_state_v1 "t3code:ui-state:v1"
// node: dead.t3code_ui_state_v1.24 "d3d0c88e-4bd4-46ef-ad24-1fac103a9fbe:79608a47-186e-423a-8925-4c27b2e22ee6 DEAD thread=79608a47-186e-423a-8925-4c27b2e22ee6 threadChangedFilesExpandedById"
// link: store.t3code_ui_state_v1 owns dead.t3code_ui_state_v1.24
// node: store.t3code_right_panel_state_v2 "t3code:right-panel-state:v2"
// node: store.deepai_code_workspace_tabs_v1_nx_backup "deepai-code:workspace-tabs:v1.nx-backup"
// node: store.t3code_fileExplorerOpen "t3code.fileExplorerOpen"
// ===== /NX TEMPLATE =====
