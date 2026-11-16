# neodax fork — tab / state repair

[NeoDAX](README_UPSTREAM.md)（マルチアーキテクチャ逆解析フレームワーク）のフォークです。
上流のアナライザ（ディスアセンブラ / CFG / デコンパイラ / エミュレータ）はそのまま保持し、
このフォーク独自の目的は **アプリが残した状態の修復** に置いています:

- 閉じられないタブ
- 削除したはずのチャットが復活する
- 稼働中のストアがディスク上の永続層と食い違う

これらを **観測 → 分類 → 手順の導出 → 実行 → 再観測で検証** の流れで扱います。
アプリの reload / 再起動は行いません。反映は状態そのものの書き換えで行います。

---

## 何ができるか

バイナリは `bin/neodax-ui` 1 つです（GUI なしで起動すると CLI、引数なしだと GUI）。
永続層 (LevelDB / SQLite) と稼働中プロセス (renderer のストア) の両方を扱えます。

対象は特定のアプリ名やタブ名に依存しません。「参照できない実体」という形で判定するため、
種類が増えても収集側を足すだけで済みます。

エージェントから使う場合の規約は [AGENTS.md](AGENTS.md) にあります（要点: 状態の読み書きは
`neodax-ui` のサブコマンドだけを使い、grep / python によるテキスト走査や reload で代用しない）。

## 使い方

### ビルド

```sh
git clone https://github.com/anqisitous/neodax-fork-undergoing.git
bash gui/build.sh              # bin/neodax-ui (GUI + CLI)
```

コマンドはclone したディレクトリから
```sh
cd neodax-fork-undergoing
```
でneodax-fork-undergoingフォルダに移動して実行してください。

ビルド定義は `src/Makefile` が唯一の場所です。分類ごとの静的ライブラリを作り、
`gui/build.sh` はそれをリンクするだけです。上流アナライザ本体は従来どおり
`make`（`setup.sh` 経由）でビルドできます。詳細は [BUILDING.md](BUILDING.md)。

### 観測（変更しない）

```sh
./bin/neodax-ui windows                 # 画面上のウィンドウ一覧 (owner/name/id)
./bin/neodax-ui capture --app NAME -o out.png   # 対象アプリの画面を PNG にキャプチャ
./bin/neodax-ui display [--app NAME]    # 最も情報量の多いフレームを可読テキスト化
./bin/neodax-ui tabs                    # タブ一覧 (owner の有無つき, TSV)
./bin/neodax-ui advise                  # 壊れているかだけ判定
./bin/neodax-ui check                   # 状態の点検
./bin/neodax-ui plan                    # 事実 → 問題 → 計画
./bin/neodax-ui probe                   # renderer の origin / scripts / localStorage キー
./bin/neodax-ui keys / tab-keys         # ストアのキー一覧
./bin/neodax-ui apps / app / app-analyze  # 対象アプリの検出と分析
```

`--state` は省略できます（既知の場所を自動で探します）。

### 修復

```sh
./bin/neodax-ui repair                  # 観測 → 計画 → 実行 → 再観測を健全まで
./bin/neodax-ui advise-proc --apply     # 状態から修復手順を導出してそのまま実行
./bin/neodax-ui fix                     # 閉じられない surface タブを一括除去
./bin/neodax-ui close --id ID           # 選択したタブだけを除去 (base は保持)
./bin/neodax-ui delete --id '<tab id>' --live   # タブを 1 つ消す (根拠を先に確認)
./bin/neodax-ui clean --dry-run         # 残滓の確認 (--dry-run で変更なし)
./bin/neodax-ui clean                   # 永続層 + 全ウィンドウの live store
./bin/neodax-ui threads --within 86400           # 重複チャットの検出のみ
./bin/neodax-ui threads --within 86400 --apply   # 重複チャットを恒久削除
./bin/neodax-ui apply --live            # 永続層へ書き込む (LevelDB の seq を上げて書く)
./bin/neodax-ui heal                    # 不整合の個別修復 (--dry-run 可)
```

### 調査・その他

```sh
./bin/neodax-ui seek "PATTERN" DIR [--regex|--ignore-case|--files|--binary|--context N]
./bin/neodax-ui asar --file app.asar --find "PATTERN" --context 200
./bin/neodax-ui simulate --state PATH [--asar ASAR]  # アプリのロード結果を再現
./bin/neodax-ui replay                                # 操作の再生
./bin/neodax-ui serve                                 # 標準入力からコマンドを読む常駐モード
./bin/neodax-ui --selftest                            # GUI と同じ経路の点検
```

サブコマンドの全量は `gui/neodax-ui.mm` の `nx_cli_dispatch` を参照してください
（`cut` / `best` / `recipe` / `pipeline` / `gen` / `pe` / `spec` / `templates` / `layout` /
`baseline` / `stop` / `strategy-memory` / `kill` などもあります）。

## 設計方針

1. **判定は C 側で行う。**
   状態は LevelDB のバイナリで、行指向の走査では正しく読めません。解析・判定・修復は
   `nx_state` / `nx_deepai` / `nx_json` / `nx_leveldb` に閉じます。
   grep / python / JS での状態走査は「直したのに直っていない」を生むのでしません。

2. **1 つの永続手続きに収束させる。**
   「A を直してから B」と枝分かれさせません。個々の不整合は独立に観測し、
   永続層を書く段は 1 つだけにします。途中が失敗しても他へ伝染しません。

3. **消すときは実体で一致を取る。**
   タブの `id` はアプリが表示に合わせて書き換えます（`file:…` → `diff` など）。
   id 文字列ではなく `threadRef + 種別` を同一性として扱います。

4. **消したものは復元経路ごと塞ぐ。**
   チャットは投影テーブルだけでなく追記ログにも削除イベントを書きます。
   タブは永続層と **全ウィンドウ** のメモリ側ストアを同じ値にします
   （1 枚だけ直すと他の窓が古い値を persist して戻るため）。

5. **最後の 1 つは消さない。**
   重複除去で残すのは「タブが参照しているもの → 原本（最古）」です。
   `updated_at` は同期の遅れしか表さないため、判定に使いません。

6. **アプリを再起動しない。**
   reload はユーザーの作業セッションを破壊します。永続層を直せば再起動時に正しく
   復元されるため、反映は状態そのものを書き換えて行います。
   稼働中ストアへの反映は `nx_payload` が生成する JS（C 側のビルド産物）と
   `nx_appfix_*` の経路のみを使います。

## 構成

```
gui/neodax-ui.mm             CLI / GUI（サブコマンドの配線と画面キャプチャ）
src/integration/             修復側の本体（OS 非依存）
  nx_deepai.c                タブ状態の判定・正規化・重複除去
  nx_threads.c               チャット台帳（event + projection の同時更新）
  nx_leveldb.c / nx_state.c  永続層の読み書き
  nx_payload.c               renderer へ渡す JS（C が生成）
  nx_track.cpp               トラッキング系（最大のモジュール）
  attach/                    inspector / ストア解決 / 再水和
  plan/                      residual / clean / advisor / cpp テンプレート
src/os/{mac,win}/            OS 固有の操作だけ
src/templates/*.cpp          テンプレート（構成は cpp 内に逆順ヘッダで）
src/*.c                      上流アナライザ（disasm / cfg / decomp / emulate ...）
```

## ドキュメント

| ファイル | 内容 |
|---|---|
| [README_UPSTREAM.md](README_UPSTREAM.md) | 上流 NeoDAX の機能・CLI・npm 利用方法 |
| [AGENTS.md](AGENTS.md) | エージェント向けの作業規約 |
| [BUILDING.md](BUILDING.md) | 上流のビルド手順（全プラットフォーム） |
| [CLI_REFERENCE.md](CLI_REFERENCE.md) | 上流 `neodax` CLI のリファレンス |
| [ARCHITECTURE.md](ARCHITECTURE.md) / [ALGORITHMS.md](ALGORITHMS.md) | 上流の内部設計とアルゴリズム |
| [src/README.md](src/README.md) / [src/README_JA.md](src/README_JA.md) | src 配下の説明 |
