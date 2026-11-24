# neodax editor

「必要なノードだけ」を編集する GUI。

- reviewer 版 (`vibe/code-advisor`) の `gui/build.sh` は、この editor を
  `NeoDAX.app` の実行ファイルとして組み込む。
- editor 単体だけを作るときは `bash gui/editor/build.sh`。
- tab-state-repair 側では単体ビルドのみ (メインアプリの構成は変えない)。

```sh
# reviewer 版のアプリごとビルド
bash gui/build.sh
open bin/NeoDAX.app

# editor 単体
bash gui/editor/build.sh
./bin/neodax-editor <repo-dir>
# reviewer の結果を固定ファイルから読む場合
./bin/neodax-editor <repo-dir> --map /tmp/fixmap.tsv
```

引数なしで起動した場合はフォルダ選択になる。メニューの
「フォルダを開く…」(Cmd+O) からも切り替えられる。

`<repo-dir>` に `tools/java-review/neodax-java-review` があれば、
`--map` の TSV を自動生成して読む。無い場合は `TODO` / `task1..3` /
`ここを実装` の行だけを拾う。

## 画面

- 左: 検索セル / 必要ノード一覧 / ファイル番号フィールド / 保存
- 中央: 必要ノードのセルを縦に直結したエディタ
- 右: 元ファイル (行番号つき。行単位で選択・ドラッグ)

reviewer 版では AE の virtual capture / emulator をビルドから外している
(`-DNX_REVIEWER=1`)。従来の `neodax-ui` は同じ `.app` 内に残り、
editor のメニュー「従来の NeoDAX」から起動できる。

Cocoa の丸い標準コントロールは見た目に使わない。検索・ファイル番号・
ボタン・セル枠・スクロール枠は `NxBinaryView` 系の四角い View で描く。
スクロールバーも表示しない (ホイール / 2 本指スクロールで動く)。

## 操作

| 操作 | 動作 |
|---|---|
| 左のノードをクリック | そのセルへ移動 |
| セル内 ↑ / ↓ | 隣のセルへ移動 |
| 関数/クラス名の内側で ← / → の後、↑ / ↓ | その定義ブロックのセルを上下に追加 |
| 追加セル内で Esc | そのセルを削除 |
| Ctrl+Enter | 右の元ファイルビューで現在行を開く |
| 右の行を選択して中央へドラッグ | 選択した行範囲の元コードセルを追加 |
| 右の `≡` を中央へドラッグ | ファイル全体の元コードセルを追加 |
| セルを空にする | セルを削除。ノードのセルならノード一覧からも消す |
| Cmd+S / 保存 | 変更したセルを元ファイルへ書き戻す |

## 解析の流れ

1. `neodax-java-review <root> --map` で「必要なノード」を得る
2. 各ノードの `where` / `body` を元ファイルから切り出してセルにする
3. 元ファイルの関数/クラス範囲は brace scanner で索引化する
   (セル追加のための意味情報。Java の型解決は reviewer 側)
4. 保存時はセルを行範囲へ戻し、下から順に置換する
