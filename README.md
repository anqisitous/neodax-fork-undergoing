# neodax fork — tab / state repair と app fix

This fork keeps the upstream analyzer, but the reason it exists is narrower:
**repair the state that applications leave behind** — tabs that cannot be
closed, chats that come back after deletion, a live store that disagrees with
what is on disk — and **rewrite a broken application into a work copy**.

Upstream documentation (analyzer, formats, disassembler) is unchanged and lives
in [`README_UPSTREAM.md`](README_UPSTREAM.md), [`README_JA.md`](README_JA.md)
and the other top-level `*.md` files. This page covers only the fork side.

---

## ビルド

```sh
git clone https://github.com/anqisitous/neodax-fork-undergoing.git
cd neodax-fork-undergoing
bash gui/build.sh              # bin/neodax-ui (GUI + CLI)
```

以降の `./bin/neodax-ui` は、clone した `neodax-fork-undergoing` ディレクトリで
実行してください。上流アナライザ本体は従来どおり `make`（`setup.sh` 経由）で
ビルドできます。詳細は [BUILDING.md](BUILDING.md) を参照してください。

---

## 0. 最初に読む: コマンドの分類

`bin/neodax-ui` のコマンドは、役割が違う層に分かれています。混ぜると
「何をしてよいコマンドか」「何が内部で動くか」が分からなくなるため、
この README でも層ごとに分けます。中心は A/B/C の 3 分類で、D は共通です。

| 層 | 主な利用者 | 目的 | 代表コマンド | 状態を変えるか |
|---|---|---|---|---|
| **A. エージェント / 状態修復** | AGENTS.md の作業者 | タブ・チャット・永続層の観測と修復 | `tabs`, `keys`, `advise`, `plan`, `repair`, `clean`, `apply`, `delete` | 観測系は変えない。修復系は変える |
| **B. app fix / アプリ本体の書き換え** | アプリを直す作業者 | 動かないアプリを解析し、作業コピーを作る | `case`, `best`, `cut`, `search`, `recipe`, `recipe-run`, `app-analyze --fix` | 原版は変えない。`APP.neodax-work` を変える |
| **C. appfix / 稼働中アプリへの再適用** | A/B の内部 | 起動中の renderer に状態を再適用する | `apply --attach`, `delete`, `clean`, `heal`, `stop`, `baseline` | 稼働中ストアを変える（reload しない） |
| **D. 解析・共通** | 全員 | 表示・解析・検索・自動化 | `windows`, `capture`, `display`, `seek`, `asar`, `spec`, `pe`, `serve` | 原則として変えない |

### 呼び出し関係（要点）

```
A. 状態修復
  tabs / keys / advise / check / plan / case      観測だけ
        │
        ├─ repair ────────────────┐
        ├─ clean ─────────────────┤ 永続層 + 稼働中ストア
        ├─ heal ──────────────────┤ (appfix を使う)
        ├─ apply / delete ────────┘
        └─ advise-proc ── normalize → dedupe → persist → appfix

B. app fix
  case (解析)
        │
        ├─ best ──────── nx_cut(symbol) ──┐
        ├─ cut ───────── nx_cut(symbol) ──┤
        ├─ search ────── recipe を生成 ───┤
        └─ recipe-run ── clone → patch → verify → resign
                                          │
                                          └─ APP.neodax-work
  app-analyze --fix ── nx_cut("ShouldCheckLicense")

C. appfix
  nx_appfix_apply / remove / read / stop_restore / baseline_restore
        └─ nx_payload (C が JS を生成) → nx_renderer → inspector :9229
```

**重要:** `appfix` は CLI サブコマンドではありません。C の内部 API
(`nx_appfix_*`) です。CLI からは A の修復コマンド越しにだけ使います。

---

## 1. A. エージェントが呼ぶコマンド（状態修復）

### 1.0 最短手順

```sh
# 観測（変更しない）
./bin/neodax-ui tabs
./bin/neodax-ui advise
./bin/neodax-ui check
./bin/neodax-ui plan

# 修復（観測→計画→実行→再観測）
./bin/neodax-ui repair

# 検証
./bin/neodax-ui tabs
./bin/neodax-ui plan
```

`--state` は省略可（1.4 参照）。`VERDICT=HEALTHY` と
`stuck surface tabs: 0` を確認します。

### 1.1 観測（変更しない）

| コマンド | 何をする | 主な出力 |
|---|---|---|
| `tabs [--state PATH]` | 永続層のタブ一覧を TSV で出す | `STATE_SOURCE`, タブ行, owner の有無 |
| `keys [--state PATH]` | KV のキー・seq・生存を C 側で列挙 | `MAX_SEQ`, `KEY`, `LIVE`, `RESOLVED_KEY`, `RESOLVED_IS_BACKUP` |
| `tab-keys [--state PATH]` | タブ 1 件が持つキーを列挙 | 作業先（パス / ディレクトリ）の確認用 |
| `advise [--state PATH] [--id ID]` | 壊れているかだけ判定 | orphan / live surface の判定 |
| `check [--state PATH]` | 閉じられない surface タブを検出 | `stuck surface tabs: N` |
| `plan [--state PATH]` | 事実 → 問題 → 計画を出す | `VERDICT`, `confidence`, 段順 |
| `case --path PATH [--kind app\|state\|dir] [--case tpl[N]]` | 案件ごとの自発解析 | `=== case: tpl[N] ===`, findings, plan |
| `advise-proc [--state PATH]` | 状態から修復手順を導出 | `PROCEDURE`, `STEP`, `ITEM`, `CONFIDENCE` |
| `simulate [--state PATH] [--asar ASAR]` | アプリのロード結果を再現 | シミュレーション結果 |
| `replay [--state PATH] [-o FILE.png]` | キャプチャ無しで画面を再現 | ASCII レイアウト / PNG |
| `layout [--app NAME]` | 復元有無の判定用レイアウト署名 | `LAYOUT_BEGIN` / `WIN` / `LAYOUT_END` |
| `threads [--db PATH] [--list] [--deleted] [--delete ID] [--within N] [--apply]` | チャット台帳の一覧・重複検出・削除 | 既定 DB は `~/.deepai-code/userdata/statev2.sqlite` |
| `probe [--app NAME]` | renderer の origin / scripts / localStorage キーを観測 | JSON |
| `asar --file APP.asar [--grep S] [--find TEXT] [--context N] [--limit N]` | asar のエントリ列挙・中身検索 | `ENTRIES`, `HITS`, 該当行 |
| `seek PATTERN [ROOT] [--ignore-case] [--regex] [--word] [--binary] [--files] [--include G] [--context N] [--limit N] [--time MS]` | ABD 安全化済みの再帰検索 | 一致行 |
| `tpl-cpp [--dir DIR] [--svg FILE] [--record FILE] [--ng]` | テンプレート .cpp の構成・annulus・学習記録 | 構造 / SVG / 記録 |
| `app [--path APP]` | 直前に起動した app と保存フォルダを表示 | `pid`, `app`, `exe`, `dir`, `baseline` |
| `apps` | 保存済み app フォルダの一覧 | `~/.neodax/apps` の一覧 |
| `templates` | 保存済みテンプレートの一覧 | `~/.neodax/templates.tsv` |
| `cut-status` | 途中状態（チェックポイント）の一覧 | `~/.neodax/cut` |
| `strategy-memory` | 失敗履歴と間違い指数 | `~/.neodax/strategy-memory.tsv` |
| `strategy-memory --record-app A --record-symbol S --record-branch B --record-ok 0\|1` | 失敗履歴を記録 | 記録結果 |

`strategy-memory` の `--record-*` は通常は `search` / `cut` が内部で使う
診断用です。手で使うのは履歴の確認だけです。

現在の組み込み案件は `tpl[0]`（AE のライセンス）です。`case` は対象に合う
案件を登録順に試しますが、`--case tpl[0]` で明示することもできます。

### 1.2 修復（永続層 / 稼働中ストアを変える）

| コマンド | 何をする | 書き込み先 |
|---|---|---|
| `repair [--state PATH]` | 観測→計画→段順に実行→再観測を最大 4 ラウンド | 永続層（WAL 追記）+ 稼働中ストア |
| `clean [--state PATH] [--dry-run] [--no-live] [--app A] [--store S] [--key K]` | 残滓を種類・安全度から判定して削除 | 永続層（WAL）。`live-sync` 段が選ばれた時だけ稼働中ストアも同期。`--no-live` はその段だけ止める（1.6） |
| `heal [--state PATH] [--app A] [--store S] [--key K] [--dry-run]` | 検出→永続層へ書き戻し→稼働中ストア再水和 | 永続層（WAL）+ 稼働中ストア |
| `apply [--state PATH] [--id ID] [--key K] [--live]` | stale surface（`--id` 指定時はそのタブ）を除去 | 永続層。`--live` は起動中 LevelDB の WAL に追記、無指定はオフライン書き込み。`--attach` は別モード（1.6） |
| `delete --state PATH --id ID [--live] [--force] [--no-store-sync] [--app A] [--store S] [--key K]` | 指定タブを削除（先に advise で根拠確認。拒否時は `--force`） | 永続層 + `.nx-backup` の tombstone + 稼働中ストア。`--no-store-sync` は稼働中ストア同期だけ止める（1.6） |
| `close [--state PATH] --id ID` | 指定タブを除去した `<path>.fixed.json` を書く（ストアには書かない） | ファイルのみ |
| `fix [--state PATH]` | 修復 JSON を `<path>.fixed.json` に書くだけ | ファイルのみ（ストアには書かない） |
| `advise-proc --apply` | 導出した手順を実行（normalize → dedupe → persist → appfix） | 永続層 + 稼働中ストア |
| `threads --within N --apply` | 重複チャットを恒久削除 | SQLite（event + projection） |
| `stop [--app A] [--store S] [--key K]` | 起動時の状態復元を止める | 稼働中アプリ（appfix / JS 差し込み） |
| `baseline [--app A] [--store S] [--key K]` | 復元先を空の妥当な状態に置換 | 稼働中アプリ（appfix） |
| `attach [--state PATH] [--app A] [--store S] [--key K]` | 稼働中 renderer を再水和 | 稼働中アプリ（appfix） |

`delete` の安全弁:

- 先に `advise` と同じ判定を行い、**壊れていない live surface / chat は拒否**します。
- それでも消すときだけ `--force` を付けます。
- `--no-store-sync` は「永続層だけ消して、稼働中ストアは触らない」場合だけ使います。
  次の persist で復活しうるため、通常は付けません（1.6 参照）。
- `.nx-backup` が読み書きの解決先になっている場合は、本体だけでなく退避側も
  tombstone で消します。

### 1.3 検証（変更後に必ず再実行する）

```sh
./bin/neodax-ui tabs --state "$STATE"
./bin/neodax-ui plan --state "$STATE"
```

確認する文字列:

- `VERDICT=HEALTHY`
- `stuck surface tabs: 0`

`NEEDS_FIX` のままなら、`repair` / `clean` / `delete` のどれを選ぶかを
`advise` と `plan` の根拠から決めます。根拠なしに削除しないでください。

### 1.4 `--state` の省略

状態系コマンドは `--state` を省略できます。次の既知の場所を順に探し、
`CURRENT` がある LevelDB を使います（見つからなければ明示が必要）。

```
~/Library/Application Support/deepai-code-v2/Local Storage/leveldb
~/Library/Application Support/DeepAI Code/Local Storage/leveldb
~/Library/Application Support/deepai-code/Local Storage/leveldb
```

省略時は stderr に `[cmd] --state 未指定: ... を使用` と出ます。

### 1.5 チャット台帳（`threads`）

`threads` はタブ状態とは別の **SQLite** を見ます。既定は
`~/.deepai-code/userdata/statev2.sqlite` です。

| オプション | 意味 |
|---|---|
| `--db PATH` | 対象 SQLite を明示 |
| `--list` | チャット一覧を出す |
| `--deleted` | 削除済みも含める（`--list` と併用） |
| `--delete ID` | 指定チャットを削除 |
| `--within N` | 直近 N 秒以内の重複を検出（既定 600） |
| `--apply` | 検出した重複を実際に削除 |

削除は projection テーブルだけでなく追記ログにも削除イベントを書きます。
タブが参照している会話は保護されます。

### 1.6 live 系オプションの読み分け

`--live` / `--no-live` / `--no-store-sync` / `--attach` は名前が似ていますが、
指している対象が違います。同じ対象ごとにまとめると次の 3 つです。

#### (1) 永続層（LevelDB）を指す `--live`

`apply --live` と `delete --live` の `--live` は、**永続層への書き込み方法**です。

- `--live` あり: 起動中の LevelDB の WAL に `seq = 実測最大 + 1` で追記する。
- `--live` なし: オフライン書き込み（`nx_leveldb_write_kv`）で書く。
- どちらも**稼働中 renderer のメモリ側ストアは触らない**。メモリ側の話は
  `--attach` / `--no-store-sync` / `clean --no-live` です。
- `clean` / `heal` / `repair` / `advise-proc --apply` は、内部で常に WAL 追記
  を使います（`--live` はありません）。

#### (2) 稼働中ストア（renderer の localStorage / メモリ）を指す
`--no-live` / `--no-store-sync` / `--attach`

- `clean --no-live`: `clean` が選ぶ段のうち **`live-sync` 段だけ**を実行しない。
  `persist`（永続層への書き込み）、`repair-seq`（seq の修復）、
  `purge-backup`（退避キーの tombstone）はそのまま実行する。
  `clean` は `risk.live_sensitive >= 0.5` のときだけ `live-sync` を選ぶので、
  `--no-live` が効くのはその条件を満たした時だけです。
  出力は `persisted=yes live=untouched(--no-live)` になります。
- `delete --no-store-sync`: `delete` の **`nx_appfix_apply` による稼働中ストア
  同期だけ**を止める。永続層への書き込みと `.nx-backup` の tombstone は実行する。
- `apply --attach`: 永続層に書かず、**稼働中ストアの再水和だけ**を行う。
  `--state` があればその状態から修復 JSON を作って再水和し、無ければ現在の
  localStorage で再水和する。`--live` はこのモードでは使われない。
- `heal` / `repair` / `advise-proc --apply`: 稼働中ストア同期を常に行う
  （`--no-live` のような停止オプションはない）。
- `stop` / `baseline`: 稼働中アプリを触るが、タブ JSON の同期ではなく
  「起動時復元を止める」「復元先を空にする」操作です。

#### (3) 全部止める `--dry-run`

`clean --dry-run` と `heal --dry-run` は、**永続層も稼働中ストアも変更しない**。
`clean --dry-run` は選ばれる段を表示して終了し、`heal --dry-run` は検出だけして
終了します。`clean --no-live` は dry-run ではないので、永続層には書きます。

#### `--no-live` を使った後に何が残るか

`clean --no-live` / `delete --no-store-sync` の後も、稼働中アプリのメモリには
古いタブ状態が残ります。そのアプリが次に persist すると、古い状態が
LevelDB に戻り、修復が復活することがあります。したがって:

- アプリが起動中で、そのまま使い続けるなら、原則として `--no-live` /
  `--no-store-sync` は使わない（`heal` / `clean` / `delete` の既定同期を使う）。
- アプリが起動していない、または「永続層だけ直して、次回起動時に正しく
  復元されることを確認したい」場合だけ `--no-live` / `--no-store-sync` を使う。
- 使った後は `tabs` と `plan` を再実行し、`VERDICT=HEALTHY` /
  `stuck surface tabs: 0` を確認する。稼働中ストアが未同期であることは
  `persisted=yes live=untouched(--no-live)` などの出力で確認する。
- reload / 再起動で反映しない（6 章）。

---

## 2. B. app fix に役立つコマンド（アプリ本体の書き換え）

### 2.1 一覧

| コマンド | 役割 | 原版 | 作業コピー |
|---|---|---|---|
| `case --path APP [--kind app\|state\|dir] [--case tpl[N]]` | 案件の観測と計画（変更しない） | 変えない | 作らない |
| `best --path APP [--kind app\|state\|dir] [--apply]` | 問題を 1 個に絞り、確度順に修復 | 変えない | `--apply` で `APP.neodax-work` |
| `cut --path APP --symbol NAME [--apply]` | 解析→テンプレ保存→clone/patch/検証 | 変えない | `--apply` で `APP.neodax-work` |
| `cut --step --path APP --symbol NAME` | 上の 1 段だけ進める | 変えない | 途中状態を保持 |
| `cut --abort --path APP --symbol NAME` | 途中状態を捨てる | 変えない | チェックポイント削除 |
| `cut-status` | 途中状態の一覧 | 変えない | — |
| `search --path APP --symbol NAME [--apply] [--probe-ms N]` | 呼び出し元・先頭命令を観測し、テンプレを選ぶ | 変えない | `--apply` で `APP.neodax-work` |
| `recipe --path APP [-o FILE] [--target S]` | 書き換え手順書を作る（アプリは変更しない） | 変えない | 作らない |
| `recipe-show --file R` | 保存した手順書を表示 | 変えない | — |
| `recipe-run --file R [--apply] [--upto N]` | 手順を 1 段ずつ検証して実行 | 変えない | `--apply` で `APP.neodax-work` |
| `templates` | 保存済みテンプレートの一覧 | 変えない | — |
| `strategy-memory` | 失敗履歴の確認 | 変えない | — |
| `app [--path APP]` | 直前に起動した app の保存先を表示 | 変えない | — |
| `app-analyze [--path APP] [--fix]` | 分析して app ごとのフォルダに保存。前回と違う時だけ `--fix` で修正 | 変えない | 修正時は `APP.neodax-work` |
| `apps` | 保存済み app フォルダの一覧 | 変えない | — |
| `kill [--match SUBSTR]` | SUBSTR を含むプロセスを SIGKILL（既定 `.neodax-work`） | — | 作業コピーの残存を落とす |

### 2.2 app fix コマンドが内部で呼ぶもの

| 入口 | 内部で呼ぶもの | 何をする |
|---|---|---|
| `best` | `nx_bestfix` → `nx_cut` | finding を 1 個選び、`nx_cut` で作業コピーまで進める |
| `cut` | `nx_recipe_build` → `nx_recipe_run` | 手順書を作り、段階実行する |
| `search` | `nx_strategy_observe` → `nx_strategy_plan` → `nx_recipe_run` | 呼び出し元・先頭命令を観測し、テンプレを選んで適用 |
| `recipe` | `nx_recipe_build` | 手順書だけを作る（アプリは変えない） |
| `recipe-run` | `nx_recipe_run` | clone → patch → verify → resign を段ごとに実行 |
| `app-analyze --fix` | `nx_cut("ShouldCheckLicense")` | 前回と違う時だけ既知の入口を切る |
| `kill` | `pkill -KILL -f` | `.neodax-work` の残存プロセスを落とす |

### 2.3 作業コピーの名前

`cut` / `best` / `search --apply` / `recipe-run --apply` /
`app-analyze --fix` は、原版の隣に次の名前で作業コピーを作ります。

```
/Users/you/.../AE Fork.app
/Users/you/.../AE Fork.app.neodax-work
```

- 原版は変更しません。
- `recipe-run` の clone 段は既存の `APP.neodax-work` を削除してから
  APFS クローン (`cp -c -R`) で作り直します。
- 失敗時は `onfail rollback` により作業コピーを破棄します。
- 作業コピーは ad-hoc 署名 (`codesign --force --deep --sign -`) されます。

### 2.4 `recipe` の 7 段

`recipe` が作る手順書は、次の段を 1 枚に綴ります。

| 段 | 内容 | 検証 |
|---|---|---|
| 1 | `clone` | `APP.neodax-work` を作る |
| 2 | `verify-clone` | クローン後のライブラリ SHA-256 を照合 |
| 3 | `patch` | シンボル先頭を `mov w0,#0; ret`（8 バイト）に置換 |
| 4 | `verify-range` | 差分が指定範囲内だけか |
| 5 | `observe-stub` | 書き込んだ stub を読み直す |
| 6 | `resign` | ad-hoc 再署名 |
| 7 | `verify-sign` | `codesign --verify --strict` |

`recipe` はまず `--target` なしなら `NglControllerC1` / `NglControllerC2` /
`DoSetup` / `ShouldCheckLicense` の順で解決できるものを選びます。
`--target S` でシンボルを直接指定できます。`complete=0` の手順は
`recipe-run` が実行を拒否します。

```sh
# 手順書を作る（アプリは変えない）
./bin/neodax-ui recipe --path "/path/App.app" --target ShouldCheckLicense -o /tmp/app.recipe

# 手順書を確認
./bin/neodax-ui recipe-show --file /tmp/app.recipe

# 検証だけ（dry-run）
./bin/neodax-ui recipe-run --file /tmp/app.recipe

# 作業コピー APP.neodax-work を作る
./bin/neodax-ui recipe-run --file /tmp/app.recipe --apply
```

### 2.5 `cut` の段階実行

`cut` は `recipe` を内部で作り、`~/.neodax/cut/<app>__<symbol>.state` に
段を保存します。

```sh
# 解析だけ（原版も作業コピーも変えない）
./bin/neodax-ui cut --path "/path/App.app" --symbol ShouldCheckLicense

# 1 段ずつ
./bin/neodax-ui cut --step --path "/path/App.app" --symbol ShouldCheckLicense

# 最後まで
./bin/neodax-ui cut --apply --path "/path/App.app" --symbol ShouldCheckLicense

# 途中状態を捨てる
./bin/neodax-ui cut --abort --path "/path/App.app" --symbol ShouldCheckLicense
```

### 2.6 現在の app fix の対象

状態修復（A）は特定のアプリ名に依存しません。一方、**アプリ本体の書き換え
（B）は現在 AE 向け**です。`cut` / `recipe` / `best` は
`Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib` と、
その中の既知シンボルを前提にしています。別アプリに広げる場合は、
シンボル解決と手順書の対象を追加します。

---

## 3. C. appfix が使う経路（稼働中アプリへの再適用）

`appfix` は CLI サブコマンドではなく、`src/integration/attach/` の
C API です。**reload / 再起動をしません。** 稼働中の renderer に対して
localStorage を書き、ストアを再水和します。

### 3.1 CLI から appfix に入るコマンド

| CLI | 内部で呼ぶもの | 効果 |
|---|---|---|
| `apply --attach [--app A] [--store S] [--key K] [--id ID]` | `nx_appfix_apply` | 修復 JSON を稼働中ストアへ再適用 |
| `delete ...` | `nx_appfix_apply`（`--no-store-sync` 以外） | 削除後の JSON を稼働中ストアへ同期 |
| `clean ...` | `nx_appfix_apply`（`live-sync` 段が選ばれ、かつ `--no-live` でない時） | 残滓除去後の JSON を同期 |
| `heal ...` | `nx_appfix_apply` | 検出→永続層→稼働中ストア再水和 |
| `repair ...` | `nx_appfix_apply`（live-sync 段） | 修復後の JSON を同期 |
| `advise-proc --apply` | `nx_appfix_apply` | 正規化・重複除去後の JSON を同期 |
| `attach ...` | `nx_appfix_apply` | 再水和のみ（value が無ければ現状で再水和） |
| `stop ...` | `nx_appfix_stop_restore` | 起動時復元を止める JS を差し込む |
| `baseline ...` | `nx_appfix_baseline_restore` | 復元先を空の妥当な状態に置換 |

### 3.2 内部 API

| 関数 | 役割 |
|---|---|
| `nx_appfix_apply(profile, value, len, out)` | 修復済み JSON を localStorage に書き、ストアを再水和 |
| `nx_appfix_remove(profile, out)` | キーを削除して再水和（定義のみ。現在 CLI からは未使用） |
| `nx_appfix_read(profile, out)` | キーの現在値を読む（定義のみ。現在 CLI からは未使用） |
| `nx_appfix_stop_restore(profile, out)` | 起動時復元を止める |
| `nx_appfix_baseline_restore(profile, out)` | 復元先を空の妥当な状態に置換 |
| `nx_appfix_make(src, dst, force, out)` | AE 用の clone + patch + resign の実験用。**現在 CLI からは呼ばれていない** |

`nx_appfix_apply` は次の順で動きます。

1. `nx_inspect` でプロセスを解決（macOS は libproc）
2. `nx_payload` が C 側で JS を生成
3. `nx_renderer` が inspector（既定 `127.0.0.1:9229`）へ接続
4. renderer の localStorage を書き、ストアを再水和
5. 既定では**全ウィンドウ**を同期（1 枚だけ直すと他の窓が古い値を persist するため）

### 3.3 appfix が起動する外部ユーティリティ

これらはユーザーが打つコマンドではなく、C / C++ の内部処理が呼びます。

| ユーティリティ | 呼ぶ場所 | 用途 |
|---|---|---|
| `cp -c -R` | `nx_recipe_run` / `nx_appfix_make` | APFS クローンで作業コピーを作る |
| `rm -rf` | `nx_recipe_run` / `nx_appfix_make` | 既存の作業コピーを破棄（rollback 含む） |
| `codesign --force --deep --sign -` | `nx_recipe_run` / `nx_appfix_make` | ad-hoc 再署名 |
| `codesign --verify --strict` | `nx_recipe_run` / `nx_appfix_make` | 署名検証 |
| `pkill -KILL -f` | `kill`, `nx_strategy_search` | `.neodax-work` の残存プロセスを落とす |
| `screencapture` | `capture_best` の最終 fallback | CoreGraphics で取れない時の画面取得 |

---

## 4. D. 解析・共通コマンド

| コマンド | 何をする |
|---|---|
| `windows` | 画面上のウィンドウ一覧（`WINID`, `PID`, `AREA`, `OWNER`, `NAME`） |
| `capture --app NAME -o FILE.png`（`--out` も可） | 対象アプリの画面を PNG にキャプチャ |
| `display [--app NAME] [-o FILE] [--state PATH]` | 複数バックエンドを試し、最も情報量の多いフレームを選んで出力（既定 `/tmp/neodax-display.png`） |
| `spec --hex "0x81 0x05 ..."` / `spec --file PATH [--ws]` | 構造認識（`--ws` で WebSocket フレームも） |
| `gen --spec NAME\|all [--class NAME] [-o FILE]` | spec から C++ クラスを生成 |
| `pe --file FILE.exe [--disasm] [--limit N]` | Windows PE の解析 / 逆アセンブル |
| `pipeline --file FILE.exe [--strategy auto\|seq\|sim] [--limit N] [-o out.cpp]` | decode + encode 戦略の選択 |
| `serve` | 標準入力からパイプ区切りでコマンドを読む常駐モード |
| `--gui` / `-g` / 引数なし | GUI を起動 |
| `--selftest` | GUI と同じ実行経路を CLI から点検（`repair` を含むため状態を変えうる） |

---

## 5. GUI の操作方法

### 5.1 起動

```sh
./bin/neodax-ui            # 引数なしで GUI
./bin/neodax-ui --gui      # 同じ
./bin/neodax-ui -g         # 同じ
```

ビルドは `bash gui/build.sh` です。`bin/neodax-ui` は GUI と CLI の
両方を持ち、GUI は CLI の `nx_cli_dispatch` をそのまま呼びます
（別実装を持たない）。

### 5.2 メインウィンドウ（`NeoDAX`）

構成:

- **左: コマンド一覧** — グループ見出し付き（観測 / 修復 / AE手順 / 画面 / 解析）。
  行を選ぶと、そのコマンドの argv テンプレートが下の引数欄に入ります。
- **引数欄** — `|` 区切りの argv を編集できます。パス・`--id`・`--app` など、
  値を入れるコマンドはここで実際の値に書き換えてから実行します。
- **実行ボタン** — 押すか **Enter** で実行。引数欄が空なら一覧のテンプレートを使います。
- **右: 出力** — `$ neodax-ui <argv>` と C 側の出力、`[exit N]` を表示。
- **状態表示** — 自動解決した LevelDB のパス、または「見つからない」。

左の一覧はショートカットであり、全コマンドを網羅した一覧ではありません。
一覧に無いコマンド（`close`, `delete`, `strategy-memory`, `serve` など）も、
引数欄に直接 `close --state ... --id ...` のように入力すれば実行できます。
`serve` は GUI ではなく別プロセスで使います。

### 5.3 app 分析ウィンドウ

起動時に別フレームで開きます。タイトルは
`app 分析 (最近起動した app から選ぶ)` です。

- 上: 最近起動した app の一覧（新しい順、同じ app は最新 1 件）
- ボタン:
  - **更新** — 一覧を再取得
  - **分析** — `app-analyze --path "<選択>"`
  - **分析して修正** — `app-analyze --path "<選択>" --fix`
  - **1個特定して最善** — `best --path "<選択>" --apply`
- 下: コマンドの出力

`分析して修正` は前回の分析（`after.txt`）と違う時だけ修正します。
既に `ShouldCheckLicense` が `mov w0,#0` になっている作業コピーがあれば
`already` と表示し、再 clone しません。

### 5.4 AE エミュレータウィンドウ

メインとは別フレームで開きます。タイトルは
`AE エミュレータ (neodax とは別フレーム)` です。

- 左: ウィジェットを描いた操作ビュー
- 右: annulus / line / point のノード表示
- 下: レポート（選択結果）とログ（実行したコマンド）

ボタンは 2 つだけです。

| ボタン | 動作 |
|---|---|
| **範囲選択 / 整形 / 操作** | モードを 0→1→2→0 と切り替える |
| **関連表示** | 選択ノードに関連するウィジェットを強調 / 解除 |

モード:

| モード | 操作 |
|---|---|
| **操作**（0） | ウィジェットをクリックすると、そのウィジェットに割り当てた neodax コマンドを実行し、ログに `$ neodax-ui ...` と出力を出す |
| **範囲選択**（1） | ドラッグした矩形と交差するウィジェットを、サイズ・座標・被覆率つきで解析（neodax へは渡さない） |
| **整形**（2） | ウィジェットをドラッグで移動し、離すと 10px にスナップ |

ウィジェットとコマンドの対応:

| ウィジェット | 実行するコマンド |
|---|---|
| `menu` | `templates` |
| `tool` | `cut-status` |
| `timeline` | `strategy-memory` |
| `composition` | `templates` |
| `panel` | `cut-status` |
| `effects` | `strategy-memory` |

ノードをクリックすると選択ノードが変わり、`関連表示` がオンのときは
関連ウィジェットがハイライトされます。

### 5.5 `serve`（GUI 無しの常駐モード）

```sh
./bin/neodax-ui serve
```

起動時に `READY neodax-ui serve` を出し、標準入力から 1 行 1 コマンドを
読みます。区切りはパイプ（`|`）です（パスに空白があってもよい）。

```text
windows                         ウィンドウ一覧
capture|APP|OUT.png             キャプチャ
display|APP|STATE               表示（APP に - で省略）
tabs|PATH                       タブ一覧
sim|PATH[|ASAR]                 シミュレーション
close|PATH|ID                   タブ除去
fix|PATH                        修復 JSON
delete|PATH|ID                  削除（live 同期あり）
replay|PATH[|OUT.png]           画面再現
apply|PATH[|KEY]                オフライン書き戻し
spec|HEX                        構造認識
spec|file:PATH[|--ws]           ファイルから構造認識
attach|APP[|STORE]              再水和
quit                            終了（BYE）
```

各コマンドの出力の最後に `END` が出ます。

### 5.6 GUI 経路の点検

```sh
./bin/neodax-ui --selftest
```

`tabs`, `advise`, `check`, `plan`, `repair` を GUI と同じ
`nx_gui_exec` 経路で実行し、終了コードと先頭 3 行を出します。

**注意:** `--selftest` は `repair` を含むため、状態を変更する可能性があります。
観測だけしたいときは `tabs` / `advise` / `check` / `plan` を個別に実行してください。

---

## 6. 状態削除の正しい順序

1. **事実確認（ネイティブのみ）**
   ```sh
   ./bin/neodax-ui tabs --state "$STATE"
   ./bin/neodax-ui keys --state "$STATE"
   ```
2. **壊れているか判定**
   ```sh
   ./bin/neodax-ui advise --state "$STATE"
   ./bin/neodax-ui check --state "$STATE"
   ./bin/neodax-ui plan --state "$STATE"
   ```
3. **修復内容の作成**
   ```sh
   ./bin/neodax-ui fix --state "$STATE"
   ```
   または、状態から手順を導出:
   ```sh
   ./bin/neodax-ui advise-proc --state "$STATE"
   ```
4. **永続層への反映**
   ```sh
   ./bin/neodax-ui apply --state "$STATE" --live
   ```
   特定タブを消すなら、根拠を確認したうえで:
   ```sh
   ./bin/neodax-ui delete --state "$STATE" --id "$ID" --live
   ```
5. **稼働中ストアへの反映が必要な場合**
   `heal` / `repair` / `advise-proc --apply` は appfix 経由で稼働中ストアも
   同じ値にします。`clean` は `live-sync` 段が選ばれた時だけ、`delete` は
   `--no-store-sync` 以外の時に同期します。`apply --attach` は同期だけを
   行います。**reload はしません。**
   `clean --no-live` / `delete --no-store-sync` は「永続層だけ」のモードで、
   稼働中ストアは触りません（1.6 参照）。
6. **再観測で検証**
   ```sh
   ./bin/neodax-ui tabs --state "$STATE"
   ./bin/neodax-ui plan --state "$STATE"
   ```
   `VERDICT=HEALTHY` と `stuck surface tabs: 0` を確認します。

やってはいけないこと:

- 原版の状態を消してメモリに残す（次の persist で復活する）
- 稼働中アプリを reload / 再起動して反映する（ユーザーの作業を壊す）
- `advise` の根拠なしに `delete --force` する
- LevelDB を行指向の `grep` / `sed` で読む（バイナリで誤読する）

---

## 7. 出力マーカー

| マーカー | 出るコマンド | 意味 |
|---|---|---|
| `STATE_SOURCE` | `tabs`, `keys`, `tab-keys` | 実際に読んだ状態の場所 |
| `MAX_SEQ` | `keys` | 観測した最大 seq |
| `RESOLVED_KEY` / `RESOLVED_IS_BACKUP` | `keys` | 読み書きに使われるキーと、退避側へのすり替わり |
| `VERDICT=HEALTHY` / `NEEDS_FIX` / `UNKNOWN` | `plan`, `case`, `repair` | 判定 |
| `confidence=` | `plan`, `case` | 根拠の強さ |
| `stuck surface tabs: N` | `check`, `heal` | 閉じられない surface の件数 |
| `PROCEDURE` / `STEP` / `ITEM` / `CONFIDENCE` | `advise-proc` | 導出した手順と根拠 |
| `LAYOUT_BEGIN` / `WIN` / `LAYOUT_END` | `layout` | 復元有無の比較用署名 |
| `NXRECIPE` | `recipe*` | 手順書フォーマット |
| `complete=0` | `recipe` | 対象シンボル未解決。実行不可 |
| `diff` / `diffs=` / `PASS` / `FAIL` | `recipe-run`, `cut` | 差分範囲・stub・署名の検証 |
| `[delete] REFUSED` | `delete` | 壊れていないため拒否（`--force` で解除） |
| `persisted=yes/no` / `live=synced/untouched(--no-live)` | `clean` | 永続層に書けたか / 稼働中ストアを同期したか |

---

## 8. 保存場所

| パス | 内容 |
|---|---|
| `~/.neodax/templates.tsv` | テンプレート（kind / target / symbol / vaddr / action / status） |
| `~/.neodax/cut/` | `cut` のチェックポイント（`<app>__<symbol>.state`） |
| `~/.neodax/recipes/` | `cut` が内部で作る手順書 |
| `~/.neodax/strategy-memory.tsv` | `search` の失敗履歴（作業 >10 / 全体 >200 で枝刈り） |
| `~/.neodax/apps/<name>_<hash>/` | `app-analyze` の保存先（`app.path`, `analysis.txt`, `after.txt`, `history.tsv`） |
| `~/.deepai-code/userdata/statev2.sqlite` | `threads` の既定 DB |
| `APP.neodax-work` | `cut` / `best` / `search --apply` / `recipe-run --apply` の作業コピー |

---

## 9. 設計方針（概略）

1. **判定は C 側で行う。**
   状態はバイナリで、行指向の走査では正しく読めない。解析・判定・修復は
   `nx_state` / `nx_deepai` / `nx_json` / `nx_leveldb` に閉じる。

2. **1 つの永続手続きに収束させる。**
   「A を直してから B」と枝分かれさせない。個々の不整合は独立に観測し、
   永続層を書く段は 1 つだけにする。途中が失敗しても他へ伝染しない。

3. **消すときは実体で一致を取る。**
   タブの `id` はアプリが表示に合わせて書き換える（`file:…` → `diff` など）。
   id 文字列ではなく `threadRef + 種別` を同一性として扱う。

4. **消したものは復元経路ごと塞ぐ。**
   チャットは投影テーブルだけでなく追記ログにも削除イベントを書く。
   タブは永続層と、**全ウィンドウ**のメモリ側ストアを同じ値にする
   （1 枚だけ直すと他の窓が古い値を persist して戻る）。

5. **最後の 1 つは消さない。**
   重複除去で残すのは「タブが参照しているもの → 原本（最古）」。
   `updated_at` は同期の遅れしか表さないので判定に使わない。

6. **アプリを再起動しない。**
   反映は状態そのものを書き換えて行う。reload はユーザーの作業を壊す。
   稼働中ストアへの反映は、`nx_payload` が生成する JS（C 側のビルド産物）と
   `nx_appfix_*` の経路のみを使う。

7. **原版と作業コピーを分ける。**
   app fix は `APP.neodax-work` だけを変える。失敗時は作業コピーを捨て、
   原版には触らない。

---

## 10. 構成

```
gui/neodax-ui.mm             CLI（サブコマンドの配線のみ）+ GUI
src/integration/             修復側の本体（OS 非依存）
  nx_deepai.c                タブ状態の判定・正規化・重複除去
  nx_threads.c               チャット台帳（event + projection の同時更新）
  nx_leveldb.c / nx_state.c  永続層の読み書き
  nx_payload.c               renderer へ渡す JS（C が生成）
  nx_track.cpp               トラッキング系
  attach/                    inspector / ストア解決 / 再水和（appfix）
  plan/                      residual / clean / advisor / cpp テンプレート / recipe
src/os/{mac,win}/            OS 固有の操作だけ
src/templates/*.cpp          テンプレート（構成は cpp 内に逆順ヘッダで）
src/*.c                      上流アナライザ（disasm / cfg / decomp / emulate ...）
```

ビルド定義は `src/Makefile` が唯一の場所です。分類ごとの静的ライブラリを作り、
`gui/build.sh` はそれをリンクするだけです。

---

## 11. ドキュメント

| ファイル | 内容 |
|---|---|
| [README_UPSTREAM.md](README_UPSTREAM.md) | 上流 NeoDAX の機能・CLI・npm 利用方法 |
| [AGENTS.md](AGENTS.md) | エージェント向けの作業規約 |
| [BUILDING.md](BUILDING.md) | 上流のビルド手順（全プラットフォーム） |
| [CLI_REFERENCE.md](CLI_REFERENCE.md) | 上流 `neodax` CLI のリファレンス |
| [ARCHITECTURE.md](ARCHITECTURE.md) / [ALGORITHMS.md](ALGORITHMS.md) | 上流の内部設計とアルゴリズム |
| [src/README.md](src/README.md) / [src/README_JA.md](src/README_JA.md) | src 配下の説明 |
