# neodax fork — tab / state repair

このフォークは上流のアナライザを保持しつつ、**アプリが残した状態の修復**を目的としています。
閉じられないタブ、削除後に復活するチャット、ディスクと食い違う稼働中ストア、そうした残滓を
観測し、分類し、修復し、検証します。

上流のドキュメント（アナライザ、フォーマット、ディスアセンブラ）は変更していません。
[`README_UPSTREAM.md`](README_UPSTREAM.md) と他のトップレベル `*.md` を参照してください。
このページは修復側のみを扱います。

---

## 概要

- 永続層 (LevelDB / SQLite) と稼働中プロセス (renderer のストア) を**観測**します
- 残滓を**分類**し、修復手順を**導出して実行**し、**再観測で検証**します
- アプリの reload / 再起動は行いません。反映は状態そのものの書き換えで行います

対象は特定のアプリ名やタブ名に依存しません。「参照できない実体」という形で判定するため、
種類が増えても収集側を足すだけで済みます。

## 使い方

### ビルド

```sh
bash gui/build.sh          # bin/neodax-ui (GUI + CLI)
```

### 観測（変更しない）

```sh
./bin/neodax-ui tabs                     # タブ一覧（owner の有無つき）
./bin/neodax-ui advise                   # 壊れているかだけ判定
./bin/neodax-ui plan                     # 事実 → 問題 → 計画
./bin/neodax-ui advise-proc              # 状態から修復手順を導出
./bin/neodax-ui threads --list           # チャット台帳（重複の検出も）
./bin/neodax-ui threads --within 86400   # 重複チャットの検出のみ
```

### 修復

```sh
./bin/neodax-ui advise-proc --apply      # 導出した手順をそのまま実行 → 再導出
./bin/neodax-ui clean --dry-run          # 残滓の確認
./bin/neodax-ui clean                    # 永続層 + 全ウィンドウの live store
./bin/neodax-ui threads --within 86400 --apply   # 重複チャットを恒久削除
./bin/neodax-ui delete --id '<tab id>' --live    # タブを 1 つ消す（根拠を先に確認）
```

### 調査

```sh
./bin/neodax-ui seek "PATTERN" DIR [--regex|--ignore-case|--files|--context N]
./bin/neodax-ui asar --file app.asar --find "PATTERN" --context 200
./bin/neodax-ui probe                    # renderer の origin / scripts / localStorage キー
```

## 設計方針

1. **判定は C 側で行う**
   状態はバイナリで、行指向の走査では正しく読めません。解析・判定・修復は
   `nx_state` / `nx_deepai` / `nx_json` / `nx_leveldb` に閉じます。

2. **1 つの永続手続きに収束させる**
   「A を直してから B」と枝分かれさせません。個々の不整合は独立に観測し、
   永続層を書く段は 1 つだけにします。途中が失敗しても他へ伝染しません。

3. **消すときは実体で一致を取る**
   タブの `id` はアプリが表示に合わせて書き換えます（`file:…` → `diff` など）。
   id 文字列ではなく `threadRef + 種別` を同一性として扱います。

4. **消したものは復元経路ごと塞ぐ**
   チャットは投影テーブルだけでなく追記ログにも削除イベントを書きます。
   タブは永続層と**全ウィンドウ**のメモリ側ストアを同じ値にします
   （1 枚だけ直すと他の窓が古い値を persist して戻るため）。

5. **最後の 1 つは消さない**
   重複除去で残すのは「タブが参照しているもの → 原本（最古）」です。
   `updated_at` は同期の遅れしか表さないため、判定に使いません。

6. **アプリを再起動しない**
   reload はユーザーの作業を壊すため、反映は状態そのものを書き換えて行います。

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
