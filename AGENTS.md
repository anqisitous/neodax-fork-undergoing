# AGENTS.md — neodax で作業するエージェントへの規約

## 絶対禁止

- 対象アプリ（DeepAI Code 等）を **reload / 再起動しない**。
  `location.reload()`、`app.relaunch()`、`open -a` による再起動、プロセス kill を
  「状態を反映させるため」に使わない。反映は状態そのものを書き換えて行う。
- 消えてほしい状態を **JS で parse / 走査しない**。
  `performance.getEntriesByType('resource')` の列挙、`await import()` による
  モジュール探索、JSON を JS で組み立てる処理は禁止。解析と判定は C 側
  (`nx_deepai` / `nx_state` / `nx_leveldb` / `nx_json`) で行う。
- ビルド済みバイナリを差し替えずに ad-hoc な JS を注入して済ませない。
  変更は `gui/neodax-ui.mm` と `src/integration/**` に入れ、`bin/neodax-ui` を
  ビルドし直して、そのバイナリのサブコマンドで実行する。
- 既存の動作しているコードを、頼まれていないのに書き換えない
  （`append_store_resolve` のような共通部品は特に）。

## 状態削除の正しい手順

1. 事実確認はネイティブのみ:
   `./bin/neodax-ui tabs --state "<Local Storage/leveldb>"`
2. 修復内容の作成: `./bin/neodax-ui fix --state <leveldb>`（native, `nx_deepai_fix_json`）
3. 永続層への反映: `./bin/neodax-ui apply --state <leveldb> --live`
   （`nx_leveldb_append_kv`。LevelDB の seq を上げて書く）
4. 稼働中ストアへの反映が必要な場合も、`nx_payload` が生成する JS（C 側の
   ビルド産物）と `nx_appfix_*` の経路のみを使う。

## 禁止の理由

- reload はユーザーの作業セッションを破壊する。永続層を直せば再起動時に正しく
  復元されるため、reload は不要。
- localStorage だけ消してメモリに残すと、次の persist 書き込みで復活する
  （これが「消しても復活する」の原因）。メモリ側の状態も同時に更新するか、
  書かせない経路にする。
