# neodax-fork — タブ / 状態 repair と app fix

この fork は上流の解析器（NeoDAX）をそのまま残し、存在理由は 2 つに絞って
あります。

1. **アプリが残した状態を直す** — 閉じられないタブ、削除したのに復活するチャット、
   ディスクと食い違う稼働中ストア。
2. **壊れたアプリの作業コピーを作る** — 起動しない / ライセンスで止まるアプリを
   解析し、原版を残したまま作業コピーを書き換える。

上流のドキュメント（アナライザ、フォーマット、逆アセンブラ）は
[`README_UPSTREAM.md`](README_UPSTREAM.md)、[`README_JA.md`](README_JA.md) と
その他の `*.md` にあります。**この README は fork 側だけ**を扱います。

---

## 目次

1. [ビルド](#1-ビルド)
2. [30 秒で分かる全体像](#2-30-秒で分かる全体像)
3. [最短手順](#3-最短手順コピペ用)
4. [ユースケース索引（症状から選ぶ）](#4-ユースケース索引症状から選ぶ)
5. [ユースケース索引（別尺度で選ぶ）](#5-ユースケース索引別尺度で選ぶ)
6. [コマンド詳細](#6-コマンド詳細)
7. [オプション早見表](#7-オプション早見表)
8. [出力マーカー](#8-出力マーカー)
9. [内部の層（設計の視点）](#9-内部の層設計の視点)
10. [保存場所](#10-保存場所)
11. [ドキュメント](#11-ドキュメント)

---

## 1. ビルド

```sh
git clone https://github.com/anqisitous/neodax-fork-undergoing.git
cd neodax-fork-undergoing
bash gui/build.sh              # → bin/neodax-ui（GUI + CLI 両方）
```

以降の `./bin/neodax-ui` は、このディレクトリで実行してください。
上流アナライザ本体は従来どおり `make`（`setup.sh` 経由）でビルドできます。
詳細は [BUILDING.md](BUILDING.md) を参照してください。

この README の例は、シェルの変数 `$STATE` に「DeepAI Code がタブ状態を
保存している LevelDB ディレクトリ」を入れて実行します。
（DeepAI Code の設定フォルダの中の `Local Storage/leveldb` という名前の
フォルダです。`tabs` / `keys` などの対象がここです。）

```sh
export STATE="$HOME/Library/Application Support/deepai-code-v2/Local Storage/leveldb"
```

---

## 2. 30 秒で分かる全体像

`bin/neodax-ui` のコマンドは、**何に書き込むか**で 3 つの層に分かれます。
オプション `--live` / `--attach` / `--no-live` を選ぶ前に、この区別を
押さえてください。

| 書き込む層 | 中身 | 変更する主なコマンド |
|---|---|---|
| **① 永続層**（LevelDB のファイル） | ディスク上のタブ状態。アプリが終了しても残る | `apply` `delete` `clean` `heal` `repair` `advise-proc --apply` |
| **② 稼働中ストア**（起動中のアプリが今見ている値） | 次回 persist で ① に書き戻される | `apply --attach` `attach` `stop` `baseline` `delete`（既定） `clean`（live-sync 段） `heal` `repair` |
| **③ アプリ本体**（.app バンドルのコード） | 起動時の挙動・ライセンス判定のコード | `cut` `best` `search` `recipe-run --apply` `app-analyze --fix` |

鉄則:

- **反映は「状態そのもの」を書き換えて行う。** アプリの reload / 再起動で
  反映させることはしません。
- **① だけ直しても ② が古い値を持つと、次の persist で ① に戻り、修復が
  「復活」します。** そのため既定では ① と ② を同時に直します。
- 判定・解析は C 側で行います。LevelDB はバイナリなので、行指向の
  `grep` / `sed` / `awk` では正しく読めません。状態を見るのは
  `tabs` / `keys` / `advise` / `check` / `plan` です。

### `--state` を省略したときの自動解決

状態系コマンドでは `--state` を省略できます。省略すると、**DeepAI Code の
LevelDB が置かれうる 3 か所**（アプリのインストール形態によるプロファイル名の
違い。中身は同じ役割です）を次の順に試し、`CURRENT` ファイルのある（＝本物の
LevelDB の）最初のものを使います。stderr に
`[cmd] --state 未指定: ... を使用` と使われた場所が出ます。

```
~/Library/Application Support/deepai-code-v2/Local Storage/leveldb
~/Library/Application Support/DeepAI Code/Local Storage/leveldb
~/Library/Application Support/deepai-code/Local Storage/leveldb
```

3 か所とも見つからなければエラーになり、`--state` の明示が必要です。

---

## 3. 最短手順（コピペ用）

```sh
# 1) 事実を見る
./bin/neodax-ui tabs        --state "$STATE"
./bin/neodax-ui advise     --state "$STATE"
./bin/neodax-ui check      --state "$STATE"
./bin/neodax-ui plan       --state "$STATE"

# 2) 直す（観測 → 計画 → 実行 → 再観測 を最大 4 ラウンド）
./bin/neodax-ui repair     --state "$STATE"

# 3) 検証（必ず再実行する）
./bin/neodax-ui tabs        --state "$STATE"
./bin/neodax-ui plan       --state "$STATE"
```

`repair` 中の判定は `VERDICT=HEALTHY` / `VERDICT=NEEDS_FIX`、
`check` は `stuck surface tabs: N` で確認します。`NEEDS_FIX` のままなら
`advise` と `plan` の根拠から `clean` / `delete` のどちらかを選びます。

---

## 4. ユースケース索引（症状から選ぶ）

**「何に困っているか」**から入る索引です。各項目に最短手順と選べばよい
コマンドを書いてあります。コマンドのオプション詳細は [6 章](#6-コマンド詳細) を参照。

### UC-1 タブが閉じられない（× を押しても消えない）

```sh
./bin/neodax-ui check  --state "$STATE"     # 閉じられない surface が何件あるか
./bin/neodax-ui plan   --state "$STATE"     # 何をするかの提案
./bin/neodax-ui repair --state "$STATE"     # 孤立 surface（owner が無いタブ）を除去
```

- **owner スレッドが消えた surface** はこれで直ります（アプリ側から閉じられない
  ので、永続層から取り除きます）。
- **owner が生きている surface**（`surface(live)`）は `repair` は触りません。
  強制で消すのは `delete --force` だけです。
- 1 件だけ消したい: `./bin/neodax-ui delete --state "$STATE" --id "<ID>"`。
- 削除可否の確認: `./bin/neodax-ui advise --state "$STATE" --id "<ID>"` の
  `removable=yes/no`。

### UC-2 消したはずのタブが復活する

復活の原因は 3 つあり、どれか 1 つずつ潰します。

| 原因 | 見分け方 | 対処 |
|---|---|---|
| 稼働中ストアが古い値を持ったまま | `keys` の `RESOLVED_IS_BACKUP=0` でも `tabs` が修理前 | `delete`（既定で ② も直す）/ `clean` |
| 退避キー（`.nx-backup`）が解決先になっている | `keys` の `RESOLVED_IS_BACKUP=1` | `clean`（`purge-backup` 段）または `delete` |
| 起動時に古い状態を復元している | 起動直後に復活する | `stop`（復元を止める）/ `baseline`（復元先を空にする） |

```sh
./bin/neodax-ui keys  --state "$STATE"      # RESOLVED_KEY / RESOLVED_IS_BACKUP を見る
./bin/neodax-ui clean --state "$STATE"      # 残滓を種類・安全度から判定して順に消す
```

`clean` は「基盤（seq）→ 永続層 → 退避の掃除 → 稼働中ストア」の順で段を
選びます（稼働中ストアを先に直すと、次の persist で元に戻ります）。
実行後は `VERDICT=CLEAN` / `VERDICT=STILL_DIRTY`、
`persisted=yes|no` / `live=synced|untouched(--no-live)` で確認します。

### UC-3 チャット（会話）が消えない・重複している

チャット台帳はタブ状態とは別の **SQLite** にあります（既定
`~/.deepai-code/userdata/statev2.sqlite`）。

```sh
./bin/neodax-ui threads --list                       # 一覧
./bin/neodax-ui threads --within 3600                # 直近 1 時間内の重複を検出（既定 600 秒）
./bin/neodax-ui threads --within 3600 --apply        # 検出した重複を実際に削除
./bin/neodax-ui threads --delete "<thread-id>"       # 1 件だけ削除
```

- `--apply` なし = 重複検出のみで削除しません。
- 現在タブが参照している会話は削除対象から保護されます。

### UC-4 起動のたびに前の状態が復元される／復元されたくない

```sh
./bin/neodax-ui stop     --app "DeepAI Code"     # 起動時の状態復元を止める
./bin/neodax-ui baseline --app "DeepAI Code"     # 復元先を「空の妥当な状態」に置き換える
```

- `stop` は「復元しない」、`baseline` は「復元するが中身は空のセッション」。
  前回の状態を全部消したくない限り `baseline` の方が安全です。
- どちらも稼働中アプリに JS を差し込んで行います（reload はしません）。

### UC-5 状態が壊れている（seq が異常・退避キーが正準を奪っている）

```sh
./bin/neodax-ui clean --state "$STATE" --dry-run   # まず段を選ぶだけ
./bin/neodax-ui clean --state "$STATE"             # 実際に消す
```

- `clean` は状態を「形」で見ず、**種類と安全度**（`risk=destructive / order /
  live / subtle` の 4 指標）から手順を選びます。
- `max seq` が異常値（`100000000` 以上）と判定されると `repair-seq` 段が先に
  入ります。この状態では正しい値を書いても読まれません。

### UC-6 アプリ本体が起動しない / ライセンスで止まる（app fix）

現在 app fix は **AE 向け**です。

```sh
./bin/neodax-ui recipe --path "/path/AE.app" --target ShouldCheckLicense -o /tmp/ae.recipe  # 手順書を作る
./bin/neodax-ui recipe-show --file /tmp/ae.recipe   # 手順書を見る
./bin/neodax-ui recipe-run --file /tmp/ae.recipe    # 検証だけ（dry-run）
./bin/neodax-ui recipe-run --file /tmp/ae.recipe --apply  # 作業コピー APP.neodax-work を作る
```

- **原版は絶対に変更しません。** 変更は `APP.neodax-work` に起きます。
- 対象シンボルが決まっていない場合は `best --path ... --apply`（問題を 1 個に
  絞って最善を尽くす）、探索してから決める場合は `search --path ... --symbol ... --apply`。
- 途中再開は `cut --step`、破棄は `cut --abort`。

### UC-7 何が起きているのか分からない（調査）

```sh
./bin/neodax-ui tabs     --state "$STATE"   # タブの一覧（TSV）
./bin/neodax-ui keys     --state "$STATE"   # キー・seq・生存・実際の解決先
./bin/neodax-ui tab-keys --state "$STATE"   # タブ 1 件が持つキー
./bin/neodax-ui advise   --state "$STATE"   # 壊れているかの判定
./bin/neodax-ui plan     --state "$STATE"   # 事実 → 問題 → 計画
./bin/neodax-ui case --path PATH --kind app|state|dir   # 案件ごとの自発解析
```

- アプリの内部まで見るなら `probe`（renderer の origin / scripts /
  localStorage キー）、`asar --find TEXT`（asar 内の本文検索）。

### UC-8 画面・ウィンドウを確認したい

```sh
./bin/neodax-ui windows                     # ウィンドウ一覧
./bin/neodax-ui capture --app "DeepAI Code" -o /tmp/app.png
./bin/neodax-ui display --app "DeepAI Code" -o /tmp/display.png
./bin/neodax-ui layout --app "DeepAI Code"  # 復元されたかどうかの比較用署名
./bin/neodax-ui replay --state "$STATE" -o /tmp/replay.png
```

- `layout` はキャプチャ無しで「起動で復元されたか」を判定する署名です。
  起動前後の出力を diff してください。
- `replay` は state から画面を再現します（キャプチャの代替）。

---

## 5. ユースケース索引（別尺度で選ぶ）

症状索引（4 章）と設計層（9 章）とは**違う尺度**の索引です。

### 5.A 「今触りたい対象」で選ぶ

| 対象 | コマンド |
|---|---|
| LevelDB のタブ状態（① 永続層） | 読むだけ: `tabs` `keys` `tab-keys` `advise` `check` `plan` ／ 書く: `apply` `delete` `fix` `close` `heal` `clean` `repair` `advise-proc` |
| 稼働中ストア（②） | `attach` `apply --attach` `heal` `stop` `baseline`、および `delete` / `clean` の既定の同期段 |
| チャット台帳（SQLite） | `threads` |
| アプリ本体（③ .app） | `cut` `best` `search` `recipe` `recipe-run` `app-analyze` |
| 画面・ウィンドウ | `windows` `capture` `display` `layout` `replay` `simulate` `probe` |
| ソース・アーカイブ・バイナリ | `seek` `asar` `spec` `gen` `pe` `pipeline` `tpl-cpp` `case` |

### 5.B 「作業の段階」で選ぶ

| 段階 | コマンド | 書き込み |
|---|---|---|
| 1. 調査（事実を出す） | `tabs` `keys` `tab-keys` `probe` `asar` `seek` `windows` `capture` `layout` | なし |
| 2. 判定（壊れているか） | `advise` `check` `plan` `case` `simulate` | なし |
| 3. 実行（直す） | 自動: `repair` `heal` / 段を選ぶ: `clean` `advise-proc --apply` / 1 件: `apply` `delete` / 台帳: `threads --apply` / バンドル: `cut` `best` `search` `recipe-run` | ①②（app fix は ③ の作業コピー） |
| 4. 検証（直ったか） | `tabs` `plan` `layout`（before/after の diff） `check` | なし |

### 5.C 「実行前の確信度」で選ぶ

| 確信度 | 選び方 |
|---|---|
| 何もしるか分からない | 段階 1〜2 のコマンドだけ。全部変更しません |
| 変更内容を見てから決めたい | `clean --dry-run`、`heal --dry-run`、`recipe-run`（`--apply` なし）、`threads`（`--apply` なし）、`close` / `fix`（ファイルに書くだけでストアは触らない） |
| 一括で任せたい | `repair`（健全になるまで最大 4 ラウンド自動実行） |
| 1 件だけ確実に消したい | `advise --id` で `removable=yes` を確認 → `delete --id` |

---

## 6. コマンド詳細

各コマンドは「説明 → 使用例 → オプションの有無で変わる挙動」の順で
書いています。例は `$STATE` を [1 章](#1-ビルド) のディレクトリとした場合の
ものですが、`--state` を省略すれば同じコマンドが自動解決でも動きます。

### 6.1 観測（状態を変えない）

#### `tabs [--state PATH]`

永続層のタブ一覧を TSV で出力します。実際に読んだ場所は `STATE_SOURCE` に
出ます。

```sh
./bin/neodax-ui tabs --state "$STATE"
```

```
STATE_SOURCE	/Users/you/Library/.../Local Storage/leveldb
IDX	ACTIVE	KIND	SURFACE	OWNER_ENV	OWNER_THREAD	ID
0	no	surface	Untitled-1	env-1	thread-abc	tab:1
1	yes	chat	-	env-1	thread-def	tab:2
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--state PATH` | その LevelDB を読む |
| 省略 | [2 章の 3 か所](#state-を省略したときの自動解決)を順に自動解決（stderr に通知） |

#### `keys [--state PATH]`

LevelDB のキー・seq・生存を C 側で列挙します。キーに NUL が含まれるため
`\xNN` エスケープで表示します。主に見るのは `RESOLVED_KEY` /
`RESOLVED_IS_BACKUP` ＝ **読み書きに実際に使われているキー**です。

```sh
./bin/neodax-ui keys --state "$STATE"
```

```
STATE_SOURCE	/Users/you/.../leveldb
MAX_SEQ	4821
COUNT	57
KEY	43	_deepai-code://app\x00\x01deepai-code:workspace-tabs
LIVE	workspace-tabs=1	nx-backup=1
RESOLVED_KEY	_deepai-code://app\x00\x01deepai-code:workspace-tabs\nx-backup
RESOLVED_IS_BACKUP	1
```

- `RESOLVED_IS_BACKUP=1` は、退避コピー（`.nx-backup`）が解決先になっている
  状態です。本体に書いても読まれません（→ UC-2 の `clean`）。
- `--state` の有無の挙動は `tabs` と同じ。

#### `tab-keys [--state PATH]`

タブ 1 件がどのキーに入っているかを列挙します。

```sh
./bin/neodax-ui tab-keys --state "$STATE"
```

- `--state` の有無の挙動は `tabs` と同じ。

#### `advise [--state PATH] [--id ID]`

「閉じられないタブがあるか」の判定だけを出します。

```sh
./bin/neodax-ui advise --state "$STATE"
./bin/neodax-ui advise --state "$STATE" --id "tab:1"
```

```
=== NeoDAX advisory (workspace tabs) ===
  surface(orphan)    Untitled-1                  owner=(gone)
  surface(live)      Untitled-2                  owner=env-1/thread-xyz
  chat               (chat)                     owner=env-1/thread-def
VERDICT=YES
  owner thread is gone; app cannot close these tabs
  - remove orphan surface tabs from the persisted store
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--id ID` あり | その 1 件について「削除してよいか」も判定し `removable=yes/no` と理由を出す |
| `--id` なし | 全体の健全性判定のみ（個別の削除可否は出さない） |

終了コード: `0` = 健全、`1` = 閉じられないタブあり / 当該 ID は削除不可、
`2` = 判定不能。

#### `check [--state PATH]`

閉じられない surface タブの件数だけを出します。

```sh
./bin/neodax-ui check --state "$STATE"
```

```
  [check] stuck surface tabs: 2
```

#### `plan [--state PATH]`

事実 → 問題 → 計画（順序・並列/代替）を、自信の度合い（`confidence=`）付きで
提案します。変更はしません。タブ名に依存せず観測から組み立てます。

```sh
./bin/neodax-ui plan --state "$STATE"
```

```
VERDICT=NEEDS_FIX
confidence=0.82
...
```

#### `case --path PATH [--kind app|state|dir] [--case tpl[N]]`

対象ごとの自発解析です。合う案件を登録順に試し、問題と計画を作ります。

```sh
./bin/neodax-ui case --path "/path/AE.app" --kind app
./bin/neodax-ui case --path "/path/AE.app" --case "tpl[0]"
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--kind` なし | `app` として扱う |
| `--kind state` / `dir` | LevelDB / フォルダを案件に合わせる |
| `--case tpl[N]` あり | 使う案件を明示（組み込みは現在 `tpl[0]` = AE のライセンス） |
| `--case` なし | 対象に合う案件を登録順に試す |

#### `advise-proc [--state PATH] [--apply]`

状態から修復手順を導出します（`PROCEDURE` / `STEP` / `ITEM` / `CONFIDENCE`）。

```sh
./bin/neodax-ui advise-proc --state "$STATE"          # 手順を見るだけ
./bin/neodax-ui advise-proc --state "$STATE" --apply   # 実行する
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | 導出のみ。変更しない |
| `--apply` | 導出手順を実行する。順序は normalize（ID 正規化）→ dedupe（同じ thread+種別の重複除去）→ persist（永続層へ書戻）→ appfix（稼働中ストアを同じ値に）。実行後に再導出（`RE-DERIVE`）して結果を出す |

#### `simulate [--state PATH] [--asar ASAR]`

アプリのロード結果を state から再現します。

```sh
./bin/neodax-ui simulate --state "$STATE"
./bin/neodax-ui simulate --state "$STATE" --asar "/path/app.asar"
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--asar` あり | asar を解釈してアプリ側の挙動も再現に反映 |
| `--asar` なし | state のみから再現 |

#### `replay [--state PATH] [-o FILE.png]`

キャプチャ無しで画面を再現します。ASCII レイアウトを stdout に出します。

```sh
./bin/neodax-ui replay --state "$STATE"
./bin/neodax-ui replay --state "$STATE" -o /tmp/replay.png
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `-o` あり | ASCII に加えて PNG も書く |
| `-o` なし | ASCII レイアウトのみ |

#### `layout [--app NAME]`

ウィンドウの所有・タイトル・座標を正規化した**署名**を出します
（`LAYOUT_BEGIN` / `WIN` / `LAYOUT_END`）。同じ内容なら同じ署名になるため、
起動前後で diff して「復元されたか」をキャプチャ無しで判定します。

```sh
./bin/neodax-ui layout --app "DeepAI Code" > /tmp/before.txt
# アプリを起動
./bin/neodax-ui layout --app "DeepAI Code" > /tmp/after.txt
diff /tmp/before.txt /tmp/after.txt
```

#### `probe [--app NAME]`

renderer の origin / scripts / localStorage キーを JSON で観測します。

```sh
./bin/neodax-ui probe --app "DeepAI Code"
```

#### `asar --file APP.asar [--grep S] [--find TEXT] [--context N] [--limit N]`

asar のエントリ列挙 / 中身検索を C 側で行います。

```sh
./bin/neodax-ui asar --file "/path/app.asar"
./bin/neodax-ui asar --file "/path/app.asar" --grep renderer
./bin/neodax-ui asar --file "/path/app.asar" --find "workspace-tabs" --context 80
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | エントリ一覧（`ENTRIES`） |
| `--grep S` | エントリ名を S で絞った一覧 |
| `--find TEXT` | 一覧の代わりに本文（JS）を検索し、`HITS` と該当行を出す |
| `--context N` | `--find` のヒット前後 N 文字を表示 |
| `--limit N` | 表示件数の上限 |

#### `seek PATTERN [ROOT] [--ignore-case] [--regex] [--word] [--binary] [--files] [--include G] [--context N] [--limit N] [--time MS]`

ABD 安全化済みの再帰検索です（全体バイト数・行長・実行時間に上限、symlink を
追わない、危険な regex を拒否）。

```sh
./bin/neodax-ui seek "workspace-tabs" ./src
./bin/neodax-ui seek --regex --ignore-case "TODO|FIXME" . --include "*.c"
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `ROOT` なし | カレントディレクトリ配下を検索 |
| `--regex` なし | PATTERN は完全一致の部分文字列。ありなら正規表現（危険パターンは拒否） |
| `--files` あり | マッチ行ではなく文件名だけ一覧 |
| `--include G` | glob で対象ファイルを絞り込む |
| `--limit N` なし | 既定 1000 件で打ち切り。`--time MS` なしなら既定 15000 ms で打ち切り |

#### `tpl-cpp [--dir DIR] [--svg FILE] [--record FILE] [--ng]`

テンプレート `.cpp` の構成・annulus・学習記録を出します（解析専用）。

```sh
./bin/neodax-ui tpl-cpp --svg /tmp/tpl.svg
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--svg FILE` あり | SVG も書く |
| `--record FILE` あり | 学習記録を保存 |
| `--ng` あり | 学習記録を保存しない |

#### `app [--path APP]` / `apps` / `templates` / `cut-status` / `strategy-memory`

保存済みの作業物を閲覧するだけのコマンド群です。

```sh
./bin/neodax-ui app               # 直前に起動した app と保存フォルダ
./bin/neodax-ui apps              # ~/.neodax/apps の一覧
./bin/neodax-ui templates         # ~/.neodax/templates.tsv
./bin/neodax-ui cut-status        # cut の途中チェックポイント
./bin/neodax-ui strategy-memory   # search の失敗履歴と枝刈り指数
```

- `strategy-memory --record-app A --record-symbol S --record-branch B
  --record-ok 0|1` は記録用の引数で、通常は `search` / `cut` が内部から使います。

### 6.2 修復（永続層・稼働中ストアを変える）

#### `repair [--state PATH]`

**観測 → 計画 → 段順に実行 → 再観測** を、健全になるまで最大 4 ラウンド
繰り返します。計画段は `persist-fix`（永続層から孤児を除く）/
`purge-backup`（退避キーを tombstone）/ `live-sync`（稼働中ストアを同じ値に）。
`live-sync` は fallback 扱いで `live-sync-alt` まで試します。どの段も効果が
なければ止まります。アプリ名に依存しません。

```sh
./bin/neodax-ui repair --state "$STATE"
```

```
-- round 1: orphans=2 findings=3 verdict=NEEDS_FIX conf=0.80
    persist-fix: removed=2 rc=0
    live-sync: rc=0
  stage done: ok
-- round 2: orphans=0 findings=0 verdict=HEALTHY conf=0.95
  [repair] healthy; nothing to do
```

#### `clean [--state PATH] [--dry-run] [--no-live] [--app A] [--store S] [--key K]`

残滓を状態に依存せず判定し、種類と安全度から手順を選んで消します。段の順序は
「seq の健全性 → 永続層 → 退避の掃除 → 稼働中ストア」。

```sh
./bin/neodax-ui clean --state "$STATE" --dry-run   # 段の計画だけ
./bin/neodax-ui clean --state "$STATE"             # 実行
```

```
=== clean: 残滓を状態から判定して消す ===
  resolution: backup (要修復)
  max seq   : 4821
  miss kind : store-split
  risk      : destructive=0.40 order=0.10 live=0.70 subtle=0.20

  chosen steps (4):
    1. repair-seq   cmd=apply --live  risk=1.00  seq が異常値...
    2. persist      cmd=apply         risk=0.50  残滓を永続層から除く
    3. purge-backup cmd=delete        risk=0.60  解決先が退避コピー...
    4. live-sync    cmd=heal          risk=0.70  永続層だけ直すと次の persist で戻る

  VERDICT=CLEAN
  persisted=yes  live=synced
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | 段を選んで実行。既定は ① 永続層 **と** ② 稼働中ストアの両方を直す |
| `--dry-run` | 何も変更しない。選ばれる段を表示して終了 |
| `--no-live` | `live-sync` 段**だけ**省略。`persist` / `repair-seq` / `purge-backup` は実行され永続層には書く。出力は `live=untouched(--no-live)` |
| `--app` / `--store` / `--key` | 稼働中ストアの同期先（アプリ名 / store 名 / キー）を絞り込む |

- `--no-live` は `live-sync` 段が選ばれているとき（`risk.live >= 0.5` のとき）
  だけ効きます。
- `--dry-run`（永続層にも書かない）と `--no-live`（永続層には書く）は別の
  指定です。

#### `heal [--state PATH] [--app A] [--store S] [--key K] [--dry-run]`

「検出 → 永続層へ書戻 → 稼働中ストア再水和」を 1 回で行います。対象は
`fix` と同じ「所有スレッドがいない surface タブ」で、owner が生きているタブは
対象になりません。永続層への書き込みは常に WAL 追記（`apply --live` と同じ
経路）です。

```sh
./bin/neodax-ui heal --state "$STATE" --dry-run   # 検出だけ
./bin/neodax-ui heal --state "$STATE"             # 検出 → 書戻 → 再水和
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--dry-run` | 検出だけ。永続層にも稼働中ストアにも触らない |
| （なし） | 永続層へ書き戻し、稼働中ストアを再水和まで進める |
| `--force` 相当 | なし（強制対象拡大のオプションはこのコマンドにありません） |

#### `apply [--state PATH] [--id ID] [--key KEY] [--live]` / `apply --attach [...]`

修復したタブ状態を永続層へ書き戻します。

```sh
./bin/neodax-ui apply --state "$STATE" --live                # stale surface をまとめて除去して書戻
./bin/neodax-ui apply --state "$STATE" --id "tab:1" --live   # 1 件だけ
./bin/neodax-ui apply --state "$STATE" --attach              # 永続層は触らず稼働中ストアだけ再水和
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | **stale surface**（所有者がいない surface タブ）を全部除去して永続層へ書く |
| `--id ID` | そのタブ 1 件だけ除去して永続層へ書く |
| `--key HINT` | 書き込むキーを明示（なしなら `workspace-tabs` を含むキーを自動判別） |
| `--live` | 永続層への書き込みを **WAL 追記**にする。WAL（Write-Ahead Log）＝ LevelDB が各ディレクトリに置く追記ジャーナル（`.log` ファイル）で、アプリが LevelDB を開いたままでもその書き込みが次回の読み出しに反映される経路です。なしなら通常のオフライン書き込み（`nx_leveldb_write_kv`） |
| `--attach` | **永続層には書かず**、稼働中ストアの再水和だけを行う。このモードで `--live` は使われません |

補足（挙動に直接響く事実）:

- どちらの書き込み経路も **seq は「実測最大 + 1」** を使います。桁違いに大きな
  seq を書くと、以降アプリ自身の書き込みが「古い」と判定されて無視され、
  neodax の値が永久に勝ち続けます。
- 書き込み後はストアを読み直して「意図した値が実際に解決されるか」を検証し、
  失敗すると `VERIFY FAILED` と出て終了コードは非 0 です。
- `--attach` は `--state` があればそこから修復 JSON を作り、なければ現在の
  localStorage の値をそのまま再水和します。

#### `delete --state PATH --id ID [--live] [--force] [--no-store-sync] [--app A] [--store S] [--key K]`

指定タブを削除します。`apply` と違い、owner が生きている surface / chat も
対象にできるのが `--force` です。

```sh
./bin/neodax-ui advise --state "$STATE" --id "tab:1"       # 削除可否の根拠を先に確認
./bin/neodax-ui delete --state "$STATE" --id "tab:1" --live
./bin/neodax-ui delete --state "$STATE" --id "tab:1" --force --live   # 拒否されたが消す判断をした
```

```
  [delete] advisory (before touching anything):
  [delete] removable: owner thread is gone (cannot be closed from the UI)
  [delete] tabs removed: 1 (id=tab:1)
  [delete] purged backup copy (key #0)
  [delete] syncing live store (no reload)
  [delete] persisted
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--id ID` | 必須。削除するタブの ID |
| `--force` なし | 削除前に判定を行い、owner が生きている surface と chat は拒否（`REFUSED`）。owner が消えた surface はそのまま削除可。ID が状態に無いときは退避コピーだけを掃除して "nothing to persist" と報告 |
| `--force` あり | 削除拒否の判定を無効化して削除する |
| `--live` | 永続層への書き込みを WAL 追記にする（`apply` と同じ意味。なしはオフライン書き込み） |
| `--no-store-sync` | 稼働中ストアの同期**だけ**止める。永続層への書き込みと退避キーの tombstone は実行される。アプリが起動中のまま使うと、次の persist で古い値が戻ることがあります |
| `--app` / `--store` / `--key` | 稼働中ストア同期先の絞り込み |

- 退避コピー（`.nx-backup`）が読み書きの解決先になっている場合は、本体と
  同時に退避側も tombstone で消します。

#### `close --state PATH --id ID` / `fix [--state PATH]`

修復後の状態を **`<path>.fixed.json` にファイルとして書くだけ**で、
ストア（①② どちら）も触りません。内容を確認してから自分で反映する場合に
使います（反映経路は `apply` / `attach`）。

```sh
./bin/neodax-ui close --state "$STATE" --id "tab:1"   # 指定タブを除いた fixed.json
./bin/neodax-ui fix   --state "$STATE"                # 孤児 surface を除いた fixed.json
```

### 6.3 稼働中ストア（アプリに今見えている値）

#### `attach [--state PATH] [--app A] [--store S] [--key K]`

稼働中 renderer のストアを再水和します。永続層には書きません。

```sh
./bin/neodax-ui attach --state "$STATE"     # state から修復 JSON を作って再水和
./bin/neodax-ui attach --app "DeepAI Code"  # 現在の localStorage をそのまま再水和
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--state` あり | その LevelDB から修復 JSON を作って再水和 |
| `--state` なし | 現在の localStorage の値をそのまま再水和 |

- `apply --attach --id ID` との違い: `attach` には「どのタブを除去するか」の
  `--id` がないことだけです（対象は `--state` からの全体修復）。

#### `stop [--app A] [--store S] [--key PREFIX]` / `baseline [--app A] [--store S] [--key KEY]`

次回起動時の「状態復元」の仕方を切り替えます。どちらも稼働中アプリに JS を
差し込んで行い、reload はしません。

```sh
./bin/neodax-ui stop     --app "DeepAI Code"   # 復元しない
./bin/neodax-ui baseline --app "DeepAI Code"   # 復元するが中身は空のセッション
```

**2 つの挙動の違い**

| コマンド | 起動時の挙動 |
|---|---|
| `stop` | 保存された状態を復元しない（アプリは初期状態で開く） |
| `baseline` | 復元するが、復元先が空の妥当な状態（タブ 0 個）に置き換わっている |

- 前回の状態を完全に消したくないだけなら `baseline` の方が安全です。
- `--app` / `--store` / `--key`（`--key PREFIX`）は絞り込みで、なしなら
  DeepAI Code の既定プロファイルが対象です。

### 6.4 チャット台帳（`threads`）

タブ状態とは別の **SQLite 台帳**（既定 `~/.deepai-code/userdata/statev2.sqlite`）を
見ます。

```sh
./bin/neodax-ui threads --list                       # 一覧（変更しない）
./bin/neodax-ui threads --list --deleted             # 削除済みも含めた一覧
./bin/neodax-ui threads                              # 直近 600 秒の重複を検出だけ
./bin/neodax-ui threads --within 3600 --apply        # 直近 1 時間内の重複を削除
./bin/neodax-ui threads --delete "<thread-id>"       # 1 件だけ削除
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | 直近 600 秒の重複**検出のみ**。何も削除しない |
| `--apply` | 検出した重複を恒久削除する。projection テーブルと追記ログの両方に削除イベントを書く（片方だけだと再構築で戻ります） |
| `--within N` | 検出幅を N 秒に。既定 600 より大きくすると「近い時刻の重複」を広く拾う |
| `--list` | 一覧表示（変更しない）。`--deleted` と併用可 |
| `--deleted` | 削除済みレコードも含める。単独では一覧を出さない（`--list` と組む） |
| `--delete ID` | 指定チャットを削除。`--list` と同時指定は不可（削除が優先される） |
| `--db PATH` | 対象 SQLite を明示。全オプションと併用可 |

- いまタブが参照している会話は保護されます（何をして削除しません）。
- 残す基準は「タブが参照しているもの → 原本（最古）」です。

### 6.5 app fix（アプリ本体の書き換え）

原版（③）は絶対に変更せず、すべて `APP.neodax-work` の作業コピーに対して
行います。

#### `recipe --path APP [-o FILE] [--target S]`

書き換え手順書（NXRECIPE）を作ります。アプリは変更しません。手順書は
7 段（clone → verify-clone → patch → verify-range → observe-stub → resign →
verify-sign）を 1 枚に綴ったものです。

```sh
./bin/neodax-ui recipe --path "/path/AE.app" -o /tmp/ae.recipe                       # 既定候補から自動解決
./bin/neodax-ui recipe --path "/path/AE.app" --target ShouldCheckLicense -o /tmp/ae.recipe
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| `--target S` なし | `NglControllerC1` / `NglControllerC2` / `DoSetup` / `ShouldCheckLicense` の順で解決できるものを選ぶ |
| `--target S` あり | シンボルを直接指定 |
| 解決できなかった場合 | 手順書は `complete=0` になり、`recipe-run` が実行を拒否する |

#### `recipe-show --file R`

保存した手順書を表示します。変更はしません。

```sh
./bin/neodax-ui recipe-show --file /tmp/ae.recipe
```

#### `recipe-run --file R [--apply] [--upto N]`

手順を 1 段ずつ検証して実行します。

```sh
./bin/neodax-ui recipe-run --file /tmp/ae.recipe             # 検証だけ
./bin/neodax-ui recipe-run --file /tmp/ae.recipe --upto 3    # patch まで
./bin/neodax-ui recipe-run --file /tmp/ae.recipe --apply     # 最後まで
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | dry-run（検証だけ）。作業コピーも作りません |
| `--apply` | clone → patch → 検証 → 再署名まで実行。失敗時は作業コピーを破棄（rollback） |
| `--upto N` | N 段目で止める（なしなら全段） |

段ごとの検証（出力は `NXRECIPE` / `diff` / `diffs=` / `PASS` / `FAIL`）:

| 段 | 内容 | 検証 |
|---|---|---|
| 1 | `clone` | `APP.neodax-work` を作る |
| 2 | `verify-clone` | クローン後のライブラリ SHA-256 を照合 |
| 3 | `patch` | シンボル先頭を `mov w0,#0; ret`（8 バイト）に置換 |
| 4 | `verify-range` | 差分が指定範囲内だけか |
| 5 | `observe-stub` | 書き込んだ stub を読み直す |
| 6 | `resign` | ad-hoc 再署名 |
| 7 | `verify-sign` | `codesign --verify --strict` |

#### `cut --path APP --symbol SYM [--apply] [--step] [--abort]`

解析 → テンプレ保存 → clone/patch/検証 を 1 コマンドで進めます。段の状態は
`~/.neodax/cut/<app>__<symbol>.state` に残ります。

```sh
./bin/neodax-ui cut --path "/path/AE.app" --symbol ShouldCheckLicense          # 解析だけ
./bin/neodax-ui cut --step  --path "/path/AE.app" --symbol ShouldCheckLicense  # 1 段進める
./bin/neodax-ui cut --apply --path "/path/AE.app" --symbol ShouldCheckLicense  # 最後まで
./bin/neodax-ui cut --abort --path "/path/AE.app" --symbol ShouldCheckLicense  # 途中状態を破棄
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | 解析だけ（同じ工程を進めるが作業コピーは作らない） |
| `--apply` | 最後まで実行して `APP.neodax-work` を作る |
| `--step` | 保存された段状態から 1 段だけ進める |
| `--abort` | チェックポイントを削除して途中状態を捨てる |

#### `search --path APP --symbol SYM [--apply] [--probe-ms N]`

呼び出し元と先頭命令を観測し、テンプレート（data < edge < entry の順）を選んで
介入します。

```sh
./bin/neodax-ui search --path "/path/AE.app" --symbol ShouldCheckLicense
./bin/neodax-ui search --path "/path/AE.app" --symbol ShouldCheckLicense --apply --probe-ms 3000
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | 観測とテンプレ選択の表示のみ |
| `--apply` | 最安のテンプレを作業コピーに適用する |
| `--probe-ms N` | 適用後に N ms 起動して生存を確かめ、成功/失敗を判定（なしなら起動判定なし） |

#### `best --path APP [--kind app|state|dir] [--apply]`

動かない問題を 1 個に絞り、その 1 個に最善を尽くします。

```sh
./bin/neodax-ui best --path "/path/AE.app"
./bin/neodax-ui best --path "/path/AE.app" --apply   # APP.neodax-work まで作成
```

#### `app-analyze [--path APP] [--fix]`

分析して app ごとのフォルダ（`~/.neodax/apps/<name>_<hash>/`）に保存します。

```sh
./bin/neodax-ui app-analyze --path "/path/AE.app"
./bin/neodax-ui app-analyze --path "/path/AE.app" --fix
```

**オプションの有無でどう違うか**

| 形 | 挙動 |
|---|---|
| （なし） | 分析して保存するだけ（`after.txt` を更新） |
| `--fix` | 前回の分析（`after.txt`）と違うときだけ修正する。既に `ShouldCheckLicense` が `mov w0,#0` になっている作業コピーがあれば `already` と出して再 clone しません |

#### `kill [--match SUBSTR]`

`SUBSTR` を含むプロセスを SIGKILL します。既定の `SUBSTR` は `.neodax-work`
（作業コピーの残存プロセスを落とすためだけに使います）。

```sh
./bin/neodax-ui kill --match ".neodax-work"
```

### 6.6 画面・解析・その他

| コマンド | 概要 |
|---|---|
| `windows` | 画面上のウィンドウ一覧（`WINID` / `PID` / `AREA` / `OWNER` / `NAME`） |
| `capture --app NAME -o FILE.png` | 対象アプリの画面を PNG 取得（`-o` / `--out` 必須。CoreGraphics を試し、失敗したら `screencapture` にフォールバック） |
| `display [--app NAME] [-o FILE] [--state PATH]` | 複数バックエンドを試して情報量の多いフレームを選ぶ（既定 `/tmp/neodax-display.png`） |
| `spec --hex "0x81 0x05 ..." / spec --file PATH [--ws]` | 構造認識（`--ws` は WebSocket フレーム） |
| `gen --spec NAME\|all [--class NAME] [-o FILE]` | spec から C++ クラスを生成 |
| `pe --file FILE.exe [--disasm] [--limit N]` | Windows PE の解析 / 逆アセンブル |
| `pipeline --file FILE.exe [--strategy auto\|seq\|sim] [--limit N] [-o out.cpp]` | decode + encode の戦略選択 |
| `serve` | パイプ区切りのコマンドを 1 行ずつ読む常駐モード（GUI ではなく別プロセスから使う） |
| `--gui` / `-g` / 引数なし | GUI 起動（下記 6.7） |
| `--selftest` | GUI と同じ実行経路を CLI から点検。**`repair` を含むため状態を変えます** |

```sh
./bin/neodax-ui capture --app "DeepAI Code" -o /tmp/app.png
./bin/neodax-ui spec --hex "0x81 0x05 0x00"
./bin/neodax-ui pe --file "/path/app.exe" --disasm --limit 40
```

### 6.7 GUI

- 起動: 引数なし / `--gui` / `-g`。`bin/neodax-ui` は GUI と CLI を両方持ち、
  GUI は CLI の `nx_cli_dispatch` をそのまま呼びます（別実装がありません）。
- **メインウィンドウ**: 左にコマンド一覧（グループ見出し付き）、中央に `|` 区切りの
  argv を編集する引数欄、右に出力。Enter でも実行でき、出力には
  `$ neodax-ui <argv>` と `[exit N]` が出ます。
- 左の一覧はショートカットで全コマンドの網羅はしていません。`close` /
  `delete` / `strategy-memory` / `serve` などは引数欄に直接入力します。
- **app 分析ウィンドウ**: 最近起動した app の一覧から `分析` / `分析して修正` /
  `1個特定して最善` を実行できます。
- **AE エミュレータウィンドウ**: メインとは別フレーム。ウィジェット操作が
  対応する neodax コマンドの実行になります。
- **点検**: `--selftest` は `tabs` / `advise` / `check` / `plan` / `repair` を
  GUI と同じ経路で実行します。`repair` を含むので、観測だけしたい場合は
  個別に実行してください。

`serve` のコマンド（1 行 1 コマンド、区切りは `|`。各出力の最後に `END`）:

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

---

## 7. オプション早見表

「そのオプションを付けるか付けないかで、どこが変わるか」の一覧です。
各行の詳細は該当コマンド節（6 章）にあります。

| オプション | 効果 | つけないとの差 | 詳細 |
|---|---|---|---|
| `--state PATH` | 対象の LevelDB ディレクトリ | 2 章の 3 か所から自動解決 | 6.1 |
| `--live` | 永続層への書き込みを **WAL（Write-Ahead Log＝LevelDB の追記ジャーナル `.log`）経由**にする | 通常のオフライン書き込みになる。どちらでも ① だけを書き、② 稼働中ストアには触らない | 6.2 `apply` |
| `--attach` | ① には書かず ② の再水和だけ | ② には触らない（① への書戻になる） | 6.2 `apply` / 6.3 `attach` |
| `--no-live` | `clean` の `live-sync` 段だけを省略 | ①② 両方を直す | 6.2 `clean` |
| `--no-store-sync` | `delete` の稼働中ストア同期だけを止める（① への書込と退避キーの tombstone は実行） | ①② 両方を直す | 6.2 `delete` |
| `--dry-run` | 何も変更しない（`clean` は段の表示、`heal` は検出だけ） | 実際に変更する | 6.2 |
| `--force` | `delete` の削除拒否（owner が生きている surface / chat）を無効化 | 壊れていない live surface と chat は拒否される | 6.2 `delete` |
| `--id ID` | 対象を 1 タブに限定（`apply` / `delete` / `close` / `advise`） | `apply` は stale surface 全体、`advise` は全体判定 | 6.1 / 6.2 |
| `--key HINT` | 書き込む / 対象のキーを指定 | `apply` は `workspace-tabs` を含むキーを自動判別 | 6.2 `apply` |
| `--app` / `--store` | ② の同期先（アプリ名 / store 名）の絞り込み | DeepAI Code の既定プロファイルが対象 | 6.2 / 6.3 |
| `--within N` | `threads` の重複検出幅（秒） | 既定 600 秒の窓で検出 | 6.4 |
| `--apply` | 「検出だけ」から「実行」に切り替える共通のつなぎ目。`threads` = 重複削除 / `recipe-run` = 手順実行 / `cut`・`best`・`search` = 作業コピー作成 / `advise-proc` = 導出手順実行 / `app-analyze` = 前回と違う時だけ修正 | どれも検出・表示・検証だけで変更しない | 6.1 / 6.4 / 6.5 |
| `--upto N` | `recipe-run` を N 段で止める | 全段実行（`--apply` 時） | 6.5 |
| `--probe-ms N` | `search` 適用後に N ms 起動して生存判定 | 起動判定なし | 6.5 |
| `--kind` | `case` / `best` の対象種別（`app` / `state` / `dir`） | `app` として扱う | 6.1 / 6.5 |
| `--target S` | `recipe` で対象シンボルを直接指定 | 既定候補順で自動解決 | 6.5 |
| `--case tpl[N]` | `case` で使う案件を明示 | 対象に合う案件を登録順に試す | 6.1 |

---

## 8. 出力マーカー

| マーカー | 出るコマンド | 意味 |
|---|---|---|
| `STATE_SOURCE` | `tabs`, `keys` | 実際に読んだ状態の場所 |
| `MAX_SEQ` / `COUNT` | `keys` | 観測した最大 seq / キー数 |
| `LIVE` | `keys` | `workspace-tabs` と `nx-backup` の生存 |
| `RESOLVED_KEY` / `RESOLVED_IS_BACKUP` | `keys` | 読み書きに使われるキーと、退避側へのすり替わり |
| `VERDICT=HEALTHY` / `NEEDS_FIX` / `UNKNOWN` | `plan`, `case`, `repair` | 判定 |
| `VERDICT=YES` / `NO` / `UNKNOWN` | `advise` | 閉じられないタブがあるか |
| `VERDICT=CLEAN` / `STILL_DIRTY` | `clean` | 残滓がなくなったか |
| `confidence=` | `plan`, `case` | 根拠の強さ |
| `stuck surface tabs: N` | `check`, `heal` | 閉じられない surface の件数 |
| `removable=yes/no` | `advise --id` | その ID を削除してよいか |
| `PROCEDURE` / `STEP` / `ITEM` / `CONFIDENCE` | `advise-proc` | 導出した手順と根拠 |
| `RE-DERIVE` | `advise-proc --apply` | 実行後の再導出 |
| `risk = destructive/order/live/subtle` | `clean` | 段選びの 4 指標 |
| `LAYOUT_BEGIN` / `WIN` / `LAYOUT_END` | `layout` | 復元判定の比較用署名 |
| `NXRECIPE` | `recipe*` | 手順書フォーマット |
| `complete=0` | `recipe` | 対象シンボル未解決。実行不可 |
| `diff` / `diffs=` / `PASS` / `FAIL` | `recipe-run`, `cut` | 差分範囲・stub・署名の検証 |
| `[delete] REFUSED` / `removable` / `forced despite` | `delete` | 削除の可否と根拠 |
| `persisted=yes/no` / `live=synced/untouched(--no-live)` | `clean` | ① に書けたか / ② を同期したか |
| `VERIFY FAILED` | `apply`, `delete` | 書き込んだ値が実際に解決されなかった |

---

## 9. 内部の層（設計の視点）

症状・対象・段階という尺度（4・5 章）とは別に、**実装の責務**での分類。
どのコマンドがどの層を踏むかを知っておくと、内部で何が動くか分かります。

| 層 | 主な利用者 | 目的 | 代表コマンド | 状態を変えるか |
|---|---|---|---|---|
| **A. エージェント / 状態修復** | AGENTS.md の作業者 | タブ・チャット・永続層の観測と修復 | `tabs`, `keys`, `advise`, `plan`, `repair`, `clean`, `apply`, `delete` | 観測系は変えない。修復系は変える |
| **B. app fix / アプリ本体の書き換え** | アプリを直す作業者 | 動かないアプリを解析し、作業コピーを作る | `case`, `best`, `cut`, `search`, `recipe`, `recipe-run`, `app-analyze --fix` | 原版は変えない。`APP.neodax-work` を変える |
| **C. appfix / 稼働中アプリへの再適用** | A/B の内部 | 起動中の renderer に状態を再適用する | `apply --attach`, `delete`, `clean`, `heal`, `stop`, `baseline` | ② を変える（reload しない） |
| **D. 解析・共通** | 全員 | 表示・解析・検索・自動化 | `windows`, `capture`, `display`, `seek`, `asar`, `spec`, `pe`, `serve` | 原則として変えない |

呼び出し関係（要点）:

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

- `appfix` は CLI サブコマンドではなく C の内部 API（`nx_appfix_*`）です。
  CLI からは A の修復コマンド越しにだけ使われます。

---

## 10. 保存場所

| パス | 内容 |
|---|---|
| `~/.neodax/templates.tsv` | テンプレート（kind / target / symbol / vaddr / action / status） |
| `~/.neodax/cut/` | `cut` のチェックポイント（`<app>__<symbol>.state`） |
| `~/.neodax/recipes/` | `cut` が内部で作る手順書 |
| `~/.neodax/strategy-memory.tsv` | `search` の失敗履歴（作業 >10 / 全体 >200 で枝刈り） |
| `~/.neodax/apps/<name>_<hash>/` | `app-analyze` の保存先（`app.path`, `analysis.txt`, `after.txt`, `history.tsv`） |
| `~/.deepai-code/userdata/statev2.sqlite` | `threads` の既定 DB |
| `<state>.fixed.json` | `close` / `fix` の出力（ストアには書きません） |
| `APP.neodax-work` | `cut` / `best` / `search --apply` / `recipe-run --apply` の作業コピー |

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
