# neodax fork — tab / state repair

This fork keeps the upstream analyzer, but the reason it exists is narrower:
**repair the state that applications leave behind** — tabs that cannot be
closed, chats that come back after deletion, a live store that disagrees with
what is on disk.

Upstream documentation (analyzer, formats, disassembler) is unchanged and lives
in [`README_UPSTREAM.md`](README_UPSTREAM.md) and the other top-level `*.md`
files. This page covers only the repair side.

---

## 何をするものか

永続層 (LevelDB / SQLite) と稼働中プロセス (renderer のストア) を **観測**し、
**残滓を分類**し、**手順を導出して実行**し、**再観測で検証**します。
アプリの reload / 再起動は行いません。

対象は特定のアプリ名やタブ名に依存しません。「参照できない実体」という形で
判定するので、種類が増えても収集側だけを足せば済みます。

## 使い方

ビルド:

```sh
bash gui/build.sh          # bin/neodax-ui (GUI + CLI)
```

観測（変更しない）:

```sh
./bin/neodax-ui tabs                     # タブ一覧（owner の有無つき）
./bin/neodax-ui advise                   # 壊れているかだけ判定
./bin/neodax-ui plan                     # 事実 → 問題 → 計画
./bin/neodax-ui advise-proc              # 状態から修復手順を導出
./bin/neodax-ui threads --list           # チャット台帳（重複の検出も）
./bin/neodax-ui threads --within 86400   # 重複チャットの検出のみ
```

修復:

```sh
./bin/neodax-ui advise-proc --apply      # 導出した手順をそのまま実行 → 再導出
./bin/neodax-ui clean --dry-run          # 残滓の確認
./bin/neodax-ui clean                    # 永続層 + 全ウィンドウの live store
./bin/neodax-ui threads --within 86400 --apply   # 重複チャットを恒久削除
./bin/neodax-ui delete --id '<tab id>' --live    # タブを 1 つ消す（根拠を先に確認）
```

調査:

```sh
./bin/neodax-ui seek "PATTERN" DIR [--regex|--ignore-case|--files|--context N]
./bin/neodax-ui asar --file app.asar --find "PATTERN" --context 200
./bin/neodax-ui probe                    # renderer の origin / scripts / localStorage キー
```

## 設計方針（概略）

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

## 構成

```
gui/neodax-ui.mm             CLI（サブコマンドの配線のみ）
src/integration/             分析（OS 非依存）
  nx_deepai.c                タブ状態の判定・正規化・重複除去
  nx_threads.c               チャット台帳（event + projection の同時更新）
  nx_payload.c               renderer へ渡す JS（C が生成）
  attach/                    inspector / ストア解決 / 再水和
  plan/                      residual / clean / advisor / cpp テンプレート
src/os/{mac,win}/            OS 固有の操作だけ
src/templates/*.cpp          テンプレート（構成は cpp 内に逆順ヘッダで）
```

ビルド定義は `src/Makefile` が唯一の場所です。分類ごとの静的ライブラリを作り、
`gui/build.sh` はそれをリンクするだけです。
