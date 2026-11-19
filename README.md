# neodax-fork — コマンドと、人とエージェントがやること

この README は **コマンド一覧と、人間／エージェントが何をすればよいか**だけを
書きます。上流の解析器（NeoDAX）の説明は [`README_UPSTREAM.md`](README_UPSTREAM.md)、
[`README_JA.md`](README_JA.md) とその他の `*.md` にあります。

---

## 目次

1. [ビルドと前提](#1-ビルドと前提)
2. [最初に覚える 3 つ](#2-最初に覚える-3-つ)
3. [grep の代わりに `seek` を使う](#3-grep-の代わりに-seek-を使う)
4. [人間がやること](#4-人間がやること)
5. [エージェントがやること](#5-エージェントがやること)
6. [コマンド一覧](#6-コマンド一覧)
7. [オプション](#7-オプション)
8. [出力の読み方](#8-出力の読み方)

---

## 1. ビルドと前提

```sh
git clone https://github.com/anqisitous/neodax-fork-undergoing.git
cd neodax-fork-undergoing
bash gui/build.sh              # → bin/neodax-ui（GUI + CLI）
```

以降はこのディレクトリで `./bin/neodax-ui` を実行します。
上流アナライザ本体は `make`（`setup.sh` 経由）でもビルドできます。

この README の例では、タブ状態の LevelDB ディレクトリを `$STATE` に入れます。

```sh
export STATE="$HOME/Library/Application Support/deepai-code-v2/Local Storage/leveldb"
```

`--state` は省略できます。省略すると次の 3 か所を順に試し、`CURRENT`
ファイルのある（＝本物の LevelDB の）最初のものを使います。stderr に
`  [tabs] --state 未指定: ... を使用` と出ます。

```
~/Library/Application Support/deepai-code-v2/Local Storage/leveldb
~/Library/Application Support/DeepAI Code/Local Storage/leveldb
~/Library/Application Support/deepai-code/Local Storage/leveldb
```

---

# 2. コマンド解説
##fixコマンドこれしかいらない
`./bin/neodax-ui fix --path "some.app" --apply`


--file　データを見る
```./bin/neodax-ui asar        --file app.asar --find "text"
./bin/neodax-ui spec        --file data.json
./bin/neodax-ui pe          --file app.exe
./bin/neodax-ui pipeline    --file app.exe
./bin/neodax-ui recipe-show --file ~/.neodax/recipes/fix-AE\ Fork.app.nxrecipe
./bin/neodax-ui recipe-run  --file ~/.neodax/recipes/fix-AE\ Fork.app.nxrecipe --apply
```

## 日常で使う入口

```sh
./bin/neodax-ui help        # 全コマンド
./bin/neodax-ui --selftest  # 健康診断
```

## 修復

```sh
# アプリ全体を一発で直す（作業コピー <app>.neodax-work を作る）
./bin/neodax-ui fix --path "/Applications/Some.app" --apply
./bin/neodax-ui fix --path "/Applications/Some.app" --target NglController --apply

# なんらかのアプリ例:Some.app の現在の状態を一発で直す
./bin/neodax-ui fix --app "Some.app" --apply

# 状態だけを直す（LevelDB 専用）
./bin/neodax-ui clean --state "<leveldb>" --dry-run
./bin/neodax-ui clean --state "<leveldb>" --apply

# 1 件だけ消す
./bin/neodax-ui delete --state "<leveldb>" --id "<ID>" --live --force

# 観測→計画→実行→再観測
./bin/neodax-ui repair --state "<leveldb>"

# 永続層へ反映 / 稼働中ストアへ反映
./bin/neodax-ui apply --state "<leveldb>" --live
./bin/neodax-ui apply --attach --app "DeepAI Code"

# 復元を止める / 基準状態に置き換える
./bin/neodax-ui stop --key "t3code:right-panel-state:v2"
./bin/neodax-ui baseline --key "t3code:right-panel-state:v2"
```

## 観測・判定

```sh
./bin/neodax-ui tabs      --state "<leveldb>"
./bin/neodax-ui keys      --state "<leveldb>"
./bin/neodax-ui tab-keys  --state "<leveldb>"
./bin/neodax-ui advise    --state "<leveldb>"
./bin/neodax-ui advise --id "<ID>"
./bin/neodax-ui plan      --state "<leveldb>"
./bin/neodax-ui check     --state "<leveldb>"
./bin/neodax-ui splice    --state "<leveldb>"
./bin/neodax-ui dump      --key "t3code:right-panel-state:v2"
./bin/neodax-ui threads   --list
./bin/neodax-ui probe     --app "DeepAI Code"
./bin/neodax-ui windows
./bin/neodax-ui capture   --app "DeepAI Code" -o shot.png
```

## 解析・プロジェクト

```sh
# 全ストア+コードのノード監査（読むだけ）
./bin/neodax-ui tpl-cpp --dir analysis/tab_restore.cpp
./bin/neodax-ui trace   --dir analysis/tab_restore.cpp

# プロジェクト（今は空の置き場。必要時だけ手で保存）
./bin/neodax-ui project --dir analysis/project --show
./bin/neodax-ui project --dir analysis/project --analysis "$(cat analysis/PROJECT_ANALYSIS.tsv)"

# 案件ごとの自発解析
./bin/neodax-ui case --path "/Applications/DeepAI Code.app" --kind app
./bin/neodax-ui best --path "/Applications/Some.app" --kind app
./bin/neodax-ui app-analyze --path "/Applications/Some.app"

# 構造認識・C++ decoder 生成
./bin/neodax-ui spec --file data.json
./bin/neodax-ui gen  --spec all -o decoders.cpp

# 手順（recipe）
./bin/neodax-ui recipe      --path "/Applications/Some.app" --target NglController
./bin/neodax-ui recipe-show --file ~/.neodax/recipes/fix-Some.app.nxrecipe
./bin/neodax-ui recipe-run  --file ~/.neodax/recipes/fix-Some.app.nxrecipe --apply

# 検索・asar
./bin/neodax-ui seek "pattern" src --regex
./bin/neodax-ui asar --file "/Applications/DeepAI Code.app/Contents/Resources/app.asar" --find "text"
```

## NGL（AE）

`neodax-ui` ではなく単体 CLI 側です。

```sh
./bin/neodax --ngl-reset --apply   # NGL 状態を退避
./bin/neodax --ngl-reset           # 退避内容の確認のみ
./bin/neodax --predict-login       # ログインダイアログを静的に予測
```

## 迷ったとき

```sh
./bin/neodax-ui advise --state "<leveldb>"   # 壊れているか
./bin/neodax-ui plan   --state "<leveldb>"   # 何をどの順で直すか
./bin/neodax-ui fix    --app "DeepAI Code" --apply
```

--kind　観測したデータの表示オプション
### `dir` = 構造を観測する

「この資産はどのノードがどのノードを要求しているか」を、ファイルと辺から組み立てます。

- `node:` / `link:` の並び
- `requires` / `converges` の向き
- ring（同時に入れ替え可能なまとまり）
- 孤立・隠れ・影
### `state` = 順序を観測する


- どのプロセスが生きているか
- seq がいくつか
##

### `app` = 因果を観測する

「このアプリが終了するのは、どの関数がどの関数を呼んでいるからか」を、バイナリの事実から組み立てます。

- `NglController` があるか
- `ShouldCheckLicense` がどのアドレスか
- どの BL 命令がそれを呼ぶか

だから `app` の事実は**シンボルとアドレスに必ず紐づく**。紐づかない推測は事実にしません。`nx_recipe_locate_symbol` / `nx_recipe_find_callers` がそのための観測器です。




## `--regex`

**`seek` 専用の真偽フラグ**。パターンを正規表現として扱う。
`seek --regex` の実体は `src/integration/track/search.cpp` の `TextQuery`
`src/integration/track/search.hpp` … `SeekOptions` の上限値
```
./bin/neodax-ui seek "a|b" . --regex
#                       ↑  ↑  ↑
#                  PATTERN ROOT FLAG
```

| 添字 | 値 | 意味 |
|---|---|---|
| `argv[0]` | `./bin/neodax-ui` | プログラム |
| `argv[1]` | `seek` | コマンド |
| `argv[2]` | `a|b` | PATTERN |
| `argv[3]` | `.` | ROOT |
| `argv[4]` | `--regex` | オプション |
### --seekオプション

`--context N`あり一致行の前後 N 行も出す`--limit N`あり一致行の上限（既定 1000、`0` で無制限）`--time MS`あり走査時間の上限（既定 15000）
## asarコマンド
./bin/neodax-ui asar --file "/Applications/Some.app/Contents/Resources/app.asar" \
  --find "t3code:right-panel-state:v2"


---

## 3. grep の代わりに `seek` を使う

**`grep` / `rg` / `sed` / `awk` をテキスト走査に使わないでください。
その代わりが `seek` です。** 人間にもエージェントにも同じ規約です。

```sh
./bin/neodax-ui seek PATTERN [ROOT]
```


理由は 3 つあります。

- **止まらない。** 全体バイト数・1 行の長さ・実行時間に上限があり、
  いつまでも走り続けません（既定 15000 ms / 1000 件で打ち切り）。
- **安全。** symlink を追わず、巨大ファイルは読む前に弾き、危険な正規表現は
  実行前に拒否します。`grep -r` をリポジトリの外やホーム全体に向けても
  事故になりません。
- **結果を毎回同じ形で出す。** 走査した量と打ち切り理由を末尾の 1 行に出すので、
  「見つからなかった」と「打ち切って見ていない」を区別できます。

### 使い方

```sh
./bin/neodax-ui seek "workspace-tabs" ./src
./bin/neodax-ui seek "TODO|FIXME" . --regex --ignore-case --include "*.c"
./bin/neodax-ui seek "nx_cli_dispatch" src --context 3 --limit 20
./bin/neodax-ui seek "seek" . --include "*.md" --files
```

**引数の順番に注意してください。** `PATTERN` は `seek` の直後（第 2 引数）に
置きます。`ROOT` はその後に続く**最初の `-` で始まらない引数**です。
`seek` より前にオプションを置くと、そのオプション名が `PATTERN` として
扱われます（`seek --regex "a|b" .` は `--regex` という文字列を探しに行きます）。

```sh
# 誤り: --regex がパターンとして扱われる
./bin/neodax-ui seek --regex "a|b" .
# 正しい
./bin/neodax-ui seek "a|b" . --regex
```

出力は `パス:行番号:本文` で、最後に 1 行の統計が付きます。

```
src/integration/plan/nx_plan.cpp:166:    if (have.count("wal-torn")) {
src/integration/plan/nx_plan.cpp:168:        s.id = "repair-wal";
seek: pattern="persist-fix" root=src files=244 matched=1 hits=2 bytes=2501893 elapsed=71ms
```

| 統計の語 | 意味 |
|---|---|
| `files=` | 読んだファイル数 |
| `matched=` | 1 件以上ヒットしたファイル数 |
| `hits=` | ヒット行数。`--files` 指定時はファイル数と同じ |
| `binary_skipped=` | バイナリと判定して飛ばした数（`--binary` で対象にできる） |
| `unreadable=` | 権限などで読めなかった数 |
| `long_lines=` | 長すぎて切り詰めた行数 |
| `[limit N]` / `[time Nms]` / `[bytes N]` | **打ち切った**理由。この印が出たら「見ていない範囲がある」 |

終了コードは `0` = ヒットあり、`1` = ヒットなし、`2` = パターンが不正 /
危険な正規表現（入れ子の量化など）として拒否。

### grep からの置き換え早見表

| grep / rg | neodax |
|---|---|
| `grep -r "foo" dir` | `./bin/neodax-ui seek "foo" dir` |
| `grep -rn "foo" .` | `./bin/neodax-ui seek "foo" .`（行番号は常に出る） |
| `grep -ri "foo" .` | `seek "foo" . --ignore-case` |
| `grep -rE "a\|b" .` | `seek "a\|b" . --regex` |
| `grep -rl "foo" .` | `seek "foo" . --files` |
| `grep -rw "foo" .` | `seek "foo" . --word` |
| `grep -A3 -B3 "foo" f` | `seek "foo" f --context 3` |
| `grep -r --include="*.c" foo .` | `seek "foo" . --include "*.c"` |
| `grep -ra foo dir`（バイナリ込み） | `seek "foo" dir --binary` |

※ `--context N` は**行**単位です（前後 N 行）。`asar --find` の `--context` は
**文字**単位なので混同しないでください。
※ `--limit N` は `--files` 指定時でも**ファイル数**で数えられます。
ファイル名の一覧が欲しいときは `--limit` を大きめにしてください。

### `seek` で状態を探さない

`seek` は**ソースやドキュメント、asar の中身を探す道具**です。
LevelDB のタブ状態は `seek` で読めません。

```sh
# これは「ファイルにその文字列が入っているか」を見ているだけ。状態の解釈ではない
./bin/neodax-ui seek "workspace-tabs" "$STATE" --binary
```

状態を知りたいときは `tabs` / `keys` / `advise` / `check` / `plan` を使います。
`seek` が返すのは生のバイト列のヒットであり、どのタブが壊れているかは
分かりません。**状態の判定を `seek` で代用しないでください。**

### `seek` でも足りないとき

| 探したいもの | コマンド |
|---|---|
| リポジトリ内の文字列 | `seek` |
| asar（アプリのバンドル）の中身 | `asar --file APP.asar --find TEXT` |
| バイナリの構造（ELF / PE） | `pe --file ...` / `spec` / `pipeline --file ...` |
| 実行中のアプリの localStorage キー | `probe --app NAME` |

---

## 4. 人間がやること

### 4.1 困ったときの入口は 1 つ

```sh
./bin/neodax-ui repair
```

観測 → 計画 → 実行 → 再観測を、健全になるまで最大 4 ラウンド行います。
終わったら結果を見ます。

```sh
./bin/neodax-ui splice   # ノード木 → データ/コード → バイナリ → 実行
./bin/neodax-ui tabs     # タブが消えたか
./bin/neodax-ui plan     # VERDICT=HEALTHY か
```

**`repair` が `VERDICT=HEALTHY` を出しても、それだけで直ったと判断しないでください。**
`repair` が確かめるのは「修復中に値が解決した」ことだけです。その後にアプリが
古い状態を書き戻すと戻ります。`splice` は観測した事実をデータとして渡し、
別に焼いた検査バイナリに判定させます。

### `splice` の流れ

```
observe → tree → emit → bake → run
　観測      木を組む  データ/コード  バイナリ化  実行
```

- **tree**: 抽象クラス `Node` を並べたもの。各ノードは「データ行を出す」
  （`data()`）と「判定コードを出す」（`rule()`）だけを持ちます。
  木を辿る側はノードの種類を知りません。
- **emit**: ノードがデータ (TSV) とコード (C++) を出す。コードは
  状態を含まないので、何度でも・どんな状態にも使えます。
- **bake**: コードを**バイナリ**にし、コードのハッシュをキーに
  `~/.neodax/splice/<hash>.bin` へ置く。同じコードなら焼き直しません。
- **run**: そのバイナリを実行し、データを stdin で渡す。

**バイナリは単体で動きます**（neodax を通さなくてもよい）。

```sh
~/.neodax/splice/<hash>.bin < data.tsv
```

段を分けて叩けます。

```sh
./bin/neodax-ui splice --show   # データとコードを表示（焼かない）
./bin/neodax-ui splice --bake   # バイナリを焼くところまで
./bin/neodax-ui splice --run    # キャッシュ済みバイナリで実行
```


### 4.2 迷ったら「壊す前に見る」

何も変更しないコマンドは次のとおりです。まずこれだけを実行してください。

```sh
./bin/neodax-ui tabs
./bin/neodax-ui keys
./bin/neodax-ui advise
./bin/neodax-ui check
./bin/neodax-ui plan
./bin/neodax-ui splice
```

`--dry-run` を付ければ `clean` / `heal` も変更しません。
`fix` / `close` は `.fixed.json` というファイルを書くだけで、アプリの状態は
触りません。

---

## 5. エージェントがやること

### 5.1 状態は必ず neodax のコマンドで読む

```sh
./bin/neodax-ui tabs        # タブ一覧（TSV）
./bin/neodax-ui keys        # キー・seq・生存・実際に解決されるキー
./bin/neodax-ui check       # 閉じられない surface の件数
./bin/neodax-ui plan        # 事実 → 問題 → 計画
./bin/neodax-ui splice     # ノード木を辿り、焼いたバイナリで判定させる
```

- **`python3` を使わない。`grep` / `rg` / `sed` / `awk` を使わない。**
  状態は LevelDB のバイナリで、行指向の走査では正しく読めません。間違った
  読み取りが「直したのに直っていない」を生みます。
- ソースを読むときは `read` ツール（行番号付き）を使い、**探すときは `seek`**。

### 5.2 コードやドキュメントを探すときは `seek`

```sh
./bin/neodax-ui seek "nx_cli_dispatch" src
./bin/neodax-ui seek "todo|fixme" . --regex --ignore-case --include "*.cpp"
./bin/neodax-ui seek "workspace-tabs" . --files
```

`grep` の代わりに常にこれを使ってください。理由と早見表は [3 章](#3-grep-の代わりに-seek-を使う)。
**`PATTERN` は `seek` の直後**に置きます。末尾の `[limit ...]` / `[time ...]` が
出たら**見ていない範囲がある**ので、`--limit` / `--time` を上げるか
`--include` で範囲を絞って再実行します。

---

## 6. コマンド一覧

### 6.1 状態を見る（変更しない）

| コマンド | 人間・エージェントがやること |
|---|---|
| `tabs` | タブ一覧を TSV で見る。`ID` 列は `delete` / `close` / `advise --id` にそのまま渡せる |
| `keys` | `MAX_SEQ` と `RESOLVED_KEY` / `RESOLVED_IS_BACKUP` を見る。退避コピーが解決先になっていないか確認 |
| `tab-keys` | タブがどのキーに入っているか見る。`apply --key` に何を渡すか決める |
| `advise` | 閉じられないタブがあるかを判定。`--id` で 1 件の削除可否（`removable=yes/no`）。`--count` で件数だけ |
| `check` | （`advise --count` の別名。同じ経路を 2 つ持たないため統合済み） |
| `splice` | 抽象ノードの木を辿り、**データ (TSV)** と **コード (C++)** を作り、コードを**バイナリに焼いて**ハッシュでキャッシュし、そのバイナリにデータを stdin で渡して実行する。コードは状態を焼き込まないので同じバイナリが何度でも使える。`--show`（表示のみ）/ `--bake`（焼くだけ）/ `--run`（キャッシュ実行）で段を分けられる |
| `plan` | 事実 → 問題 → 計画と `confidence` を見る。実行はしない |
| `case` | 対象ごとの自発解析（`--kind app\|state\|dir`、`--case tpl[N]`） |
| `advise-proc` | 修復手順の導出を見る（`--apply` で実行） |
| `windows` | 画面上のウィンドウ一覧 |
| `layout` | 復元されたかを判定する署名。起動前後で `diff` する |
| `probe` | 起動中アプリの origin / scripts / localStorage キー |
| `simulate` / `replay` | state からアプリのロード結果・画面を再現 |
| `app` / `apps` / `templates` / `cut-status` / `strategy-memory` | 保存済みの作業物を見る |

### 6.2 直す（状態を変える）

| コマンド | 人間・エージェントがやること |
|---|---|
| `repair` | まずこれを実行する。健全になるまで最大 4 ラウンド（オプションは `--state` のみ） |
| `clean` | 残滓の種類と危険度から段を選んで消す。`--dry-run` で計画だけ見られる |
| `heal` | （`clean` の別名。同じ経路を 2 つ持たないため統合済み） |
| `apply` | 孤児 surface を除いて永続層へ書く。`--live` で WAL 追記、`--attach` で再水和のみ |
| `delete` | 1 件を消す。既定は判定して拒否、`--force` で強制 |
| `fix` / `close` | `.fixed.json` を書くだけ。ストアは触らない |
| `attach` | 永続層に書かず稼働中ストアだけ再水和 |
| `stop` / `baseline` | 次回起動時の復元をやめる／空のセッションにする |
| `kill --match SUBSTR` | 作業コピーの残存プロセスを落とす（既定 `.neodax-work`） |

### 6.3 チャット台帳（SQLite）

| コマンド | 人間・エージェントがやること |
|---|---|
| `threads --list` | 一覧を見る（`--deleted` で削除済みも） |
| `threads` | 直近 600 秒の重複を検出するだけ |
| `threads --within N --apply` | 重複を実際に削除する |
| `threads --delete "<id>"` | 1 件だけ削除する |

タブが参照している会話は保護されます。残す基準は「タブ参照 → 原本（最古）」。

### 6.4 アプリ本体

| コマンド | 人間・エージェントがやること |
|---|---|
| `best --path APP` | 問題を 1 個に絞る。`--apply` で作業コピー作成まで |
| `app-analyze --path APP` | 分析して保存。`--fix` で前回と違うときだけ修正 |
| `recipe --path APP` | 手順書（NXRECIPE）を作る。原版は変更しない |
| `recipe-run --file R` | 検証だけ。`--apply` で実行、`--upto N` で途中停止 |
| `cut --path APP --symbol S` | 解析 → patch を 1 コマンドで。`--step` / `--abort` |
| `search --path APP --symbol S` | テンプレを選んで介入。`--probe-ms N` で生存確認 |

原版は絶対に変更されません。すべて `APP.neodax-work` に対して行います。

### 6.5 探す・解析する

| コマンド | 人間・エージェントがやること |
|---|---|
| `seek PATTERN [ROOT]` | **grep の代わり。** ソース・ドキュメントの検索 |
| `asar --file APP.asar` | asar のエントリ列挙 / `--find TEXT` で本文検索 |
| `pe --file X.exe` | PE の解析（`--disasm` で逆アセンブル） |
| `spec` / `gen` / `pipeline` | 構造認識・C++ 生成・decode+encode 戦略 |
| `tpl-cpp` | テンプレート `.cpp` の構成と学習記録 |
| `capture --app NAME -o F.png` | 画面を PNG で撮る |
| `display` | 複数バックエンドから情報量の多いフレームを選ぶ |
| `serve` | パイプ区切りの常駐モード（GUI 以外から使う） |
| `--selftest` | `tabs`/`advise`/`check`/`plan`/`repair` を GUI と同じ経路で点検（**状態を変える**） |

### 6.6 GUI

引数なし / `--gui` / `-g` で起動します。GUI は CLI の `nx_cli_dispatch` を
そのまま呼ぶので、できることに差はありません。左にコマンド一覧、中央に `|`
区切りの引数欄、右に出力。一覧に無いコマンド（`close` / `delete` /
`strategy-memory` / `serve` など）は引数欄に直接入力します。

---

## 7. オプション

**付けないとき何が起きるか**を軸に並べています。

| オプション | 付けると | 付けないと |
|---|---|---|
| `--state PATH` | その LevelDB を読む | 既知の 3 か所から自動解決 |
| `--live` | 書き込みを WAL（LevelDB の追記ジャーナル）経由にする | オフライン書き込み |
| `--attach` | 永続層に書かず稼働中ストアだけ再水和 | 永続層に書く |
| `--no-live` | `clean` の `live-sync` 段だけ省略（永続層には書く） | 永続層と稼働中ストアの両方を直す |
| `--no-store-sync` | `delete` の稼働中ストア同期だけ止める（永続層と退避の掃除は実行） | 両方直す |
| `--dry-run` | 何も変更しない（`clean` は段の表示、`heal` は検出だけ） | 実際に変更する |
| `--force` | `delete` の拒否判定を無効化（拒否理由は表示される） | 壊れていない surface と chat は拒否 |
| `--id ID` | 対象を 1 タブに限定 | `apply` は孤児 surface 全体、`advise` は全体判定 |
| `--key` | 書き込み先キーを指定 | `workspace-tabs` を含むキーを自動判別 |
| `--app` / `--store` | 同期先を絞り込む | DeepAI Code の既定プロファイル |
| `--apply` | 「検出・表示」から「実行」へ切り替え | 検出・表示・検証のみ |
| `--within N` | `threads` の重複検出幅（秒） | 600 秒 |
| `--db PATH` | `threads` の SQLite を明示 | 自動解決した場所 |
| `--upto N` | `recipe-run` を N 段で止める | 全段 |
| `--probe-ms N` | `search` 適用後に N ms 起動して生存判定 | 起動判定なし |
| `--kind` | `case` / `best` の対象種別 | `app` として扱う |
| `--target S` | `recipe` のシンボルを直接指定 | 既定候補順で解決 |
| `--case tpl[N]` | `case` で使う案件を番号で指定 | 合う案件を登録順に試す |
| `--time MS` / `--limit N` | `seek` の打ち切り条件を変える | 15000 ms / 1000 件 |

`--live` / `--attach` / `--no-live` / `--no-store-sync` は**どの層に書くか**の
選択です。どれも reload する方向には働きません。

### 7.1 オプションは「指すもの」で族に分かれている

名前が似ているから同じ、ではありません。**同じものを指すか**で分けています
(`gui/nx_cli_registry.c`)。

| 族 | オプション | 指すもの |
|---|---|---|
| state | `--state` | LevelDB のディレクトリ |
| live | `--store` `--key` `--app` | 稼働中ストアの探し方（`--key` は部分一致の手掛かり） |
| asset | `--dir` | テンプレート資産の置き場所（**状態ではない**） |
| app-path | `--path` | app のパス（`best` / `cut` など） |
| input | `--file` | 入力ファイル（asar / pe / recipe） |
| pick | `--id` `--symbol` `--kind` | 複数から 1 つ選ぶ |
| write | `--apply` `--live` `--dry-run` `--force` | 状態に書くかどうか |
| emit | `--show` `--bake` `--keep` | 実行せず表示・焼くだけ（**書くのとは別の軸**） |
| scope | `--depth` `--upto` `--step` `--limit` | どこまで進めるか |
| filter | `--include` `--within` `--context` `--match` | 絞り込み |

`--dir` と `--state` は別物です。前者は資産の場所、後者は LevelDB です。
`--show`（実行しない）と `--dry-run`（書かない）も別の軸です。

宣言していないオプションを渡すと、そのコマンドで何が使えるかを出して
`rc=2` で止まります。黙って無視しません。

### 7.2 資産の入力は種類を問わない

`tpl-cpp` / `trace` の `--dir` は、**cpp / svg / node / ディレクトリのどれでも
受けます**。種類は中で判定します（`nx_tplcpp_open`）。呼ぶ側が「これは svg だから
こっちの関数」と選ぶ必要はありません。

```sh
./bin/neodax-ui tpl-cpp --dir src/templates      # cpp 群
./bin/neodax-ui tpl-cpp --dir asset.svg          # 図から読み戻す
./bin/neodax-ui tpl-cpp --dir graph.node         # 1 行 1 ノード/リンク
```

部分資産も読めます。`id:` を書いていない資産はファイル名から id を作り、
link の端点は全体から名前で解決します。だから「ノードを 1 つだけ書いた
ファイル」を後から足せます。`persist` のような同じ名前のノードは 1 つに
寄せます。

`--svg` で書き出した図は `data-id` / `data-from` / `data-to` / `data-kind` を
持つので、**そのまま読み戻せます**（cpp → svg → cpp でノード・辺・ring が
一致することを確認済み）。

---

## 7.3 board 探索と分析の保存

`arena` は 2 人の player が board を大量に進めて、先に finish（問題が消えた
状態）へ着く手順を探します。状態は書きません。

```sh
./bin/neodax-ui arena --player-a resolve --player-b block
```

board はスナップショット（形だけ）で持ち、**同じ形＋同じ手なら結果も同じ**と
見なしてテンプレートで予想します。`predicted` が観測せずに済ませた手数、
`verified` が実際に確かめた手数です。

`project` は分析をプロジェクトファイルに保存します。**先に path を決め、
決まってから書く**順序です（`--dry-run` は path を出すだけで書きません）。

```sh
./bin/neodax-ui project --run "advise"        # 実行して保存
./bin/neodax-ui project --dry-run --run "advise"
./bin/neodax-ui project --dir /path/to/proj   # 場所を指定
./bin/neodax-ui project --list                # 保存済みの一覧
./bin/neodax-ui project --show                # 中身を見る
```

保存先は `~/.neodax/projects/<name>_<hash>/` で、`analysis.tsv`（分析）、
`frames.tsv`（frame ごとの動き）、`history.tsv`（追記）を持ちます。
同じ分析が既にあれば書きません（行の並びは無視して比べます）。

---

## 8. 出力の読み方

| マーカー | 出るコマンド | 意味 |
|---|---|---|
| `STATE_SOURCE` | `tabs` `keys` | 実際に読んだ場所 |
| `MAX_SEQ` / `COUNT` | `keys` | 最大 seq / キー数 |
| `RESOLVED_KEY` / `RESOLVED_IS_BACKUP` | `keys` | 読み書きに使われるキー。`=1` は退避コピーが解決先（要修復） |
| `VERDICT=HEALTHY` / `NEEDS_FIX` | `plan` `case` `repair` | 状態の判定 |
| `VERDICT=YES` / `NO` | `advise` | 閉じられないタブがあるか |
| `VERDICT=CLEAN` / `STILL_DIRTY` | `clean` | 残滓がなくなったか |
| `stuck surface tabs: N` | `check` `heal` | 閉じられない surface の件数 |
| `removable=yes/no` | `advise --id` | その ID を削除してよいか |
| `persisted=yes/no` / `live=synced/untouched` | `clean` | 永続層に書けたか / 稼働中ストアを同期したか |
| `VERIFY FAILED` | `apply` `delete` | 書いた値が実際に解決されなかった |
| `confidence=` | `plan` `case` | 根拠の強さ |
| `files=` `matched=` `hits=` | `seek` | 読んだ数 / ヒットしたファイル数 / ヒット行数 |
| `[limit]` `[time]` `[bytes]` | `seek` | 打ち切った理由（見ていない範囲がある） |
| `PASS` / `FAIL` / `diffs=` | `recipe-run` `cut` | 手順の各段の検証 |
| `complete=0` | `recipe` | シンボル未解決。`recipe-run` は実行を拒否 |

### 終了コード

| コマンド | `0` | `1` | `2` |
|---|---|---|---|
| `advise` | 健全 | 閉じられないタブあり / 当該 ID は削除不可 | 判定不能 |
| `seek` | ヒットあり | ヒットなし | パターンが不正 / 危険な正規表現として拒否 |
| `repair` | 健全 | 4 ラウンド使い切った | 計画エラー / 効果なし |
| `clean` | `CLEAN` | `STILL_DIRTY` | 状態を読めない |

---

## 9. 保存場所

| パス | 内容 |
|---|---|
| `~/.neodax/templates.tsv` | テンプレート |
| `~/.neodax/cut/` | `cut` のチェックポイント |
| `~/.neodax/recipes/` | `cut` が内部で作る手順書 |
| `~/.neodax/strategy-memory.tsv` | `search` の失敗履歴 |
| `~/.neodax/apps/<name>_<hash>/` | `app-analyze` の保存先 |
| `~/.deepai-code/userdata/statev2.sqlite` | `threads` の既定 DB |
| `<state>.fixed.json` | `fix` / `close` の出力 |
| `APP.neodax-work` | アプリの作業コピー |
| `~/.neodax/splice/<hash>.bin` | `splice` が焼いた検査バイナリ（コードのハッシュでキャッシュ。単体で実行できる） |

---

## 10. ドキュメント

| ファイル | 内容 |
|---|---|
| [AGENTS.md](AGENTS.md) | エージェント向けの作業規約（この README の 5 章と同内容） |
| [README_UPSTREAM.md](README_UPSTREAM.md) | 上流 NeoDAX の機能・CLI・npm |
| [BUILDING.md](BUILDING.md) | ビルド手順 |
| [CLI_REFERENCE.md](CLI_REFERENCE.md) | 上流 `neodax` CLI のリファレンス |
| [ARCHITECTURE.md](ARCHITECTURE.md) / [ALGORITHMS.md](ALGORITHMS.md) | 内部設計とアルゴリズム |
