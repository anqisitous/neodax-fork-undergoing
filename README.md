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
4. [タスク起点インデックス（症状から選ぶ）](#4-タスク起点インデックス症状から選ぶ)
5. [コマンド詳細（オプションと挙動の相違つき）](#5-コマンド詳細オプションと挙動の相違つき)
6. [オプション早見表](#6-オプション早見表)
7. [出力マーカー](#7-出力マーカー)
8. [内部の層（設計の視点）](#8-内部の層設計の視点)
9. [保存場所](#9-保存場所)
10. [やってはいけないこと](#10-やってはいけないこと)
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

環境変数 `$STATE` に状態のディレクトリを入れておくと、以降の例がそのまま
動きます。

```sh
export STATE="$HOME/Library/Application Support/deepai-code-v2/Local Storage/leveldb"
```

---

## 2. 30 秒で分かる全体像

`bin/neodax-ui` のコマンドは、**何に書き込むか**で 3 つに分かれます。
オプション `--live` / `--attach` / `--no-live` を選ぶ前に、この区別を
押さえてください。

| 書き込む層 | 中身 | 変更する主なコマンド |
|---|---|---|
| **① 永続層**（LevelDB のファイル） | ディスク上のタブ状態。アプリが終了しても残る | `apply` `delete` `clean` `heal` `repair` `advise-proc --apply` |
| **② 稼働中ストア**（renderer の localStorage / メモリ） | 起動中のアプリが今見ている値 | `apply --attach` `attach` `stop` `baseline` `delete`（既定） `clean`（live-sync 段） `heal` `repair` |
| **③ アプリ本体**（.app バンドル） | 起動時の挙動・ライセンス判定のコード | `cut` `best` `search` `recipe-run --apply` `app-analyze --fix` |

鉄則:

- **反映は「状態そのもの」を書き換えて行う。** アプリを reload / 再起動して
  反映させることはしません（ユーザーの作業が壊れます）。
- **① だけ直しても ② が遅れていると、次の persist で ① に戻り、修復が
  「復活」します。** そのため、既定では ① と ② を同時に直します。
- 判定・解析は C 側で行います。LevelDB はバイナリなので、行指向の
  `grep` / `sed` / `awk` で読むと誤読します。

`--state` は状態系コマンドなら省略できます。次の場所を順に探し、
`CURRENT` のある LevelDB を使います（見つからなければ `--state` が必要）。
省略時は stderr に `[cmd] --state 未指定: ... を使用` と出ます。

```
~/Library/Application Support/deepai-code-v2/Local Storage/leveldb
~/Library/Application Support/DeepAI Code/Local Storage/leveldb
~/Library/Application Support/deepai-code/Local Storage/leveldb
```

---

## 3. 最短手順（コピペ用）

```sh
# 1) 事実を見る（変更しない）
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

## 4. タスク起点インデックス（症状から選ぶ）

設計の視点を外して、**「何に困っているか」**から入る索引です。
各ユースケースに「最短手順」と「どれを選ぶか」を書いています。

### UC-1 タブが閉じられない（× を押しても消えない）

```sh
./bin/neodax-ui check  --state "$STATE"     # 閉じられない surface が何件あるか
./bin/neodax-ui plan   --state "$STATE"     # 何をするかの提案
./bin/neodax-ui repair --state "$STATE"     # 孤立 surface（owner が無いタブ）を除去
```

- 直るケース: **owner スレッドが消えた surface**。アプリ側からは閉じられないので、
  永続層から取り除きます。
- 直らないケース: **owner が生きている surface**（`surface(live)`）。
  アプリが自分で閉じられるタブなので、`repair` は触りません。強制で
  消すのは `delete --force` だけです（根拠は `advise --id` に出ます）。
- 1 件だけ消したいとき: `./bin/neodax-ui delete --state "$STATE" --id "<ID>"`。

判定区別は `advise --state "$STATE" --id "<ID>"` の `removable=yes/no` で
確認できます。

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

`clean` は「基盤（seq）→ 永続層 → 退避の掃除 → 稼働中ストア」の順で段を選びます。
この順序が肝心です（稼働中ストアを先に直すと、次の persist で元に戻ります）。
実行後は `VERDICT=CLEAN` / `VERDICT=STILL_DIRTY`、
`persisted=yes|no` / `live=synced|untouched(--no-live)` が出ます。

### UC-3 チャット（会話）が消えない・重複している

チャット台帳はタブ状態とは別の **SQLite** にあります（既定
`~/.deepai-code/userdata/statev2.sqlite`）。

```sh
./bin/neodax-ui threads --list                       # 一覧
./bin/neodax-ui threads --within 3600               # 直近 1 時間内の重複を検出（既定 600 秒）
./bin/neodax-ui threads --within 3600 --apply        # 検出した重複を実際に削除
./bin/neodax-ui threads --delete "<thread-id>"      # 1 件だけ削除
```

- オプションなし = 重複検出のみ（削除しない）。
- `--apply` を付けると重複を恒久削除します。削除は projection テーブルだけでなく
  追記ログにも削除イベントを書きます（片方だけだと再構築で戻ります）。
- いまタブが参照している会話は保護されます（消しません）。
- `--within N` の N は秒。大きいほど「近い時刻に作られた重複」と広く拾います。
- `--list` と `--deleted` は併用可。`--deleted` を付けると削除済みも表示します。

### UC-4 起動のたびに前の状態（タブ・履歴）が復元される／復元されたくない

```sh
./bin/neodax-ui stop     --app "DeepAI Code"     # 起動時の状態復元を止める
./bin/neodax-ui baseline --app "DeepAI Code"     # 復元先を「空の妥当な状態」に置き換える
```

- `stop` は「復元しない」。`baseline` は「復元するが、中身は空のセッション」。
  「前のが全部戻ってくるのが嫌」でなければ `baseline` の方が安全です。
- どちらも稼働中アプリに JS を差し込みます（reload はしません）。
- `--app` / `--store` / `--key` は対象を特定するための絞り込みです。
  既定は DeepAI Code の既定プロファイルです。

### UC-5 状態が壊れている（seq が異常・退避キーが正準を奪っている）

```sh
./bin/neodax-ui clean --state "$STATE" --dry-run   # まず段を選ぶだけ
./bin/neodax-ui clean --state "$STATE"             # 実際に消す
```

- `clean` は状態を「形」で見ず、**種類と安全度**から呼ぶ手順を選びます。
  `risk=destructive / order / live / subtle` が 4 つの指標です。
- `max seq` が異常に大きい（`100000000` 以上）と判定されると `repair-seq` 段が
  先に挿入されます。この状態では正しい値を書いても読まれません。
- `--dry-run` は何も変更せず「選ばれる段」だけを表示します。

### UC-6 アプリ本体が起動しない / ライセンスで止まる（app fix）

現在 app fix は **AE 向け**です。

```sh
# まず計画だけ（アプリも作業コピーも変えない）
./bin/neodax-ui recipe --path "/path/AE.app" --target ShouldCheckLicense -o /tmp/ae.recipe
./bin/neodax-ui recipe-show --file /tmp/ae.recipe

# 検証だけ（dry-run）
./bin/neodax-ui recipe-run --file /tmp/ae.recipe

# 作業コピー APP.neodax-work を作る
./bin/neodax-ui recipe-run --file /tmp/ae.recipe --apply
```

- **原版は絶対に変更しません。** 変更は `APP.neodax-work` に起きます。
- `--apply` なしで `recipe-run` は検証だけ、`--apply` で clone → patch → 検証 →
  再署名 を実行します。失敗時は作業コピーを破棄します（rollback）。
- どの関数を切るか決まっていない場合は `best --path ... --apply`（問題を 1 個に
  絞って最善を尽くす）、探索してから決めたい場合は
  `search --path ... --symbol ... --apply` を使います。
- 途中から再開: `cut --step --path APP --symbol SYM`（1 段進める）、
  `cut --abort --path APP --symbol SYM`（破棄）。

### UC-7 何が起きているのか分からない（調査）

```sh
./bin/neodax-ui tabs     --state "$STATE"   # タブの一覧（TSV）
./bin/neodax-ui keys    --state "$STATE"   # キー・seq・生存・実際の解決先
./bin/neodax-ui tab-keys --state "$STATE"  # タブ 1 件が持つキー
./bin/neodax-ui advise  --state "$STATE"   # 壊れているかの判定
./bin/neodax-ui plan    --state "$STATE"   # 事実 → 問題 → 計画
./bin/neodax-ui case    --path PATH --kind app|state|dir   # 案件ごとの自発解析
```

- `tabs` / `keys` / `tab-keys` は LevelDB を C 側で読むため、grep の
  代替になります（バイナリ値を正確に扱うため）。
- `plan` / `case` は変更しません。どの順で何を直すかだけを提案します。
- アプリの内部まで見たいときは `probe`（renderer の origin / scripts /
  localStorage キー）、`asar --find TEXT`（asar の中身を検索）を使います。

### UC-8 画面・ウィンドウを確認したい

```sh
./bin/neodax-ui windows                     # ウィンドウ一覧
./bin/neodax-ui capture --app "DeepAI Code" -o /tmp/app.png
./bin/neodax-ui display --app "DeepAI Code" -o /tmp/display.png
./bin/neodax-ui layout --app "DeepAI Code"  # 復元されたかどうかの比較用署名
./bin/neodax-ui replay --state "$STATE" -o /tmp/replay.png
```

- `layout` は「起動で復元されたか」を**キャプチャ無し**で判定するための署名です。
  起動前後で出力を比較してください。
- `replay` は state から画面を再現します（キャプチャの代替）。

---

## 5. コマンド詳細（オプションと挙動の相違つき）

各コマンドは「目的 → オプション → オプション有无の挙動差 → 使用例」の順で
書いています。

### 5.1 観測（状態を変えない）

#### `tabs [--state PATH]`

永続層のタブ一覧を TSV で出力します。`STATE_SOURCE` に出力先が書かれます。

- `--state` 省略時は既定の場所から自動解決します（stderr に通知）。
- 変更はしません。

```sh
./bin/neodax-ui tabs --state "$STATE"
```

```
STATE_SOURCE	/Users/you/Library/.../Local Storage/leveldb
IDX	ACTIVE	KIND	SURFACE	OWNER_ENV	OWNER_THREAD	ID
0	no	surface	Untitled-1	env-1	thread-abc	tab:1
1	yes	chat	-	env-1	thread-def	tab:2
```

#### `keys [--state PATH]`

LevelDB のキー・seq・生存を C 側で列挙します。キーには NUL が含まれるため
`\xNN` にエスケープして表示します。**どのキーが実際に読み書きに使われて
いるか**（`RESOLVED_KEY` / `RESOLVED_IS_BACKUP`）が重要です。

- `RESOLVED_IS_BACKUP=1` のときは、退避コピー（`.nx-backup`）が解決先です。
  そこに書くと本体は直らず、次の起動で古い値が戻ります。
- 変更はしません。

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

#### `tab-keys [--state PATH]`

タブ 1 件が持つキーを列挙します。「今の状態はどのキーに入入了か」を
確認する用途です。変更はしません。

```sh
./bin/neodax-ui tab-keys --state "$STATE"
```

#### `advise [--state PATH] [--id ID]`

「壊れているか」だけを判定します。変更はしません。

- `--id` を付けると、その 1 件について削除してよいかも判定し、
  `removable=yes/no` と理由を出します。
- 終了コード: `0` = 健全、`1` = 閉じられないタブがある / 当該 ID は削除不可、
  `2` = 判定不能。

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

#### `check [--state PATH]`

閉じられない surface タブの件数だけを出します。変更はしません。

```sh
./bin/neodax-ui check --state "$STATE"
```

```
  [check] stuck surface tabs: 2
```

#### `plan [--state PATH]`

事実 → 問題 → 計画（順序・並列/代替）を出して、自信の度合いも出します。
**変更はしません。** タブ名に依存せず、観測から組み立てます。

```sh
./bin/neodax-ui plan --state "$STATE"
```

```
VERDICT=NEEDS_FIX
confidence=0.82
...
```

#### `case --path PATH [--kind app|state|dir] [--case tpl[N]]`

案件ごとの自発解析です。対象に合う案件を登録順に試し、問題と計画を作ります。
変更はしません。

- `--kind` を省略すると `app` として扱います。
- 現在組み込みの案件は `tpl[0]`（AE のライセンス）です。`--case tpl[0]` で明示も
  できます。

```sh
./bin/neodax-ui case --path "/path/AE.app" --kind app
./bin/neodax-ui case --path "/path/AE.app" --case "tpl[0]"
```

#### `advise-proc [--state PATH] [--apply]`

状態から修復手順を導出します。`PROCEDURE` / `STEP` / `ITEM` / `CONFIDENCE` が
出ます。

- オプションなし = 導出のみ（変更しない）。
- `--apply` = 導出した手順を実行します。順序は
  **normalize（ID 正規化） → dedupe（同じ thread+種別の重複を除去） → persist
  （永続層へ書き戻し） → appfix（稼働中ストアを同じ値にする）**。実行後に
  再導出（`RE-DERIVE`）して結果を出します。

```sh
./bin/neodax-ui advise-proc --state "$STATE"          # 手順を見るだけ
./bin/neodax-ui advise-proc --state "$STATE" --apply   # 実行する
```

#### `simulate [--state PATH] [--asar ASAR]`

アプリのロード結果を state から再現します。変更はしません。
`--asar` を渡すと asar を解釈して、アプリ側の挙動も反映します。

```sh
./bin/neodax-ui simulate --state "$STATE" --asar "/path/app.asar"
```

#### `replay [--state PATH] [-o FILE.png]`

キャプチャ無しで画面を再現します。ASCII レイアウトを標準出力に出し、`-o` を
付けると PNG も書きます。変更はしません。

```sh
./bin/neodax-ui replay --state "$STATE" -o /tmp/replay.png
```

#### `layout [--app NAME]`

「復元されたかどうか」をキャプチャ無しで判定するための署名です。ウィンドウの
所有・タイトル・座標を正規化して `LAYOUT_BEGIN` / `WIN` / `LAYOUT_END` を出します。
同じ内容なら同じ署名になるので、**起動前後の出力を diff** します。

```sh
./bin/neodax-ui layout --app "DeepAI Code" > /tmp/before.txt
# アプリを起動
./bin/neodax-ui layout --app "DeepAI Code" > /tmp/after.txt
diff /tmp/before.txt /tmp/after.txt
```

#### `probe [--app NAME]`

renderer の origin / scripts / localStorage キーを観測します（JSON）。
変更はしません。

```sh
./bin/neodax-ui probe --app "DeepAI Code"
```

#### `asar --file APP.asar [--grep S] [--find TEXT] [--context N] [--limit N]`

asar のエントリ列挙 / 中身検索を C 側で行います。

- `--grep S` を付けるとエントリ名で絞ります。
- `--find TEXT` を付けると本文（JS）を検索し、`ENTRIES` の代わりに `HITS` と
  該当行を出します。`--context N` は前後の文字数、`--limit N` は表示上限です。

```sh
./bin/neodax-ui asar --file "/path/app.asar"
./bin/neodax-ui asar --file "/path/app.asar" --grep renderer
./bin/neodax-ui asar --file "/path/app.asar" --find "workspace-tabs" --context 80
```

#### `seek PATTERN [ROOT] [--ignore-case] [--regex] [--word] [--binary] [--files] [--include G] [--context N] [--limit N] [--time MS]`

ABD 安全化（全体バイト数・行長・期限の上限、symlink 非追跡、危険な regex の
拒否）済みの再帰検索です。`--include G` は glob での絞り込み、`--files` は
文件名のみ一覧します。`--limit`（既定 1000）と `--time`（既定 15000 ms）が
暴走を防ぎます。

```sh
./bin/neodax-ui seek "workspace-tabs" ./src
./bin/neodax-ui seek --regex --ignore-case "TODO|FIXME" . --include "*.c"
```

#### `tpl-cpp [--dir DIR] [--svg FILE] [--record FILE] [--ng]`

テンプレート `.cpp` の構成・annulus・学習記録を出します。解析専用で、アプリ
の表示は置き換えません。

- `--svg FILE` を付けると SVG も書きます。
- `--record FILE` を付けると学習記録を保存します。`--ng` を付けると保存しません。

```sh
./bin/neodax-ui tpl-cpp --svg /tmp/tpl.svg
```

#### `app [--path APP]` / `apps` / `templates` / `cut-status` / `strategy-memory`

保存済みの作業物を一覧するだけのコマンド群です。

- `app` は直前に起動した app と保存フォルダを、`apps` は `~/.neodax/apps` を、
  `templates` は `~/.neodax/templates.tsv` を、`cut-status` は途中のチェック
  ポイントを、`strategy-memory` は失敗履歴と間違い指数を表示します。
- `strategy-memory --record-app A --record-symbol S --record-branch B
  --record-ok 0|1` は記録用で、通常は `search` / `cut` が内部で使います。

```sh
./bin/neodax-ui app
./bin/neodax-ui apps
./bin/neodax-ui templates
./bin/neodax-ui cut-status
./bin/neodax-ui strategy-memory
```

### 5.2 修復（永続層・稼働中ストアを変える）

#### `repair [--state PATH]`

1 つの永続手続きとして、**観測 → 計画 → 段順に実行 → 再観測** を
健全になるまで最大 4 ラウンド繰り返します。

- 計画段は `persist-fix`（永続層から孤児を除く）/ `purge-backup`（退避キーを
  tombstone）/ `live-sync`（稼働中ストアを同じ値にする）です。
- `live-sync` は fallback として扱い、`live-sync-alt` まで試します。
- どの段も効果がなかった場合はそこで止まります（意味のないループをしません）。
- アプリ名に依存しません。

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

残滓（消されずに残ったもの）を**状態の形に依存せず**に判定し、種類と安全度から
呼ぶ手順を選んで消します。

段は「seq の健全性 → 永続層 → 退避の掃除 → 稼働中ストア」の順です。
稼働中ストアを先に直すと、次の persist で元に戻ります。

| オプション | 効果 |
|---|---|
| （なし） | 段を選んで実行する。既定は永続層 **と** 稼働中ストアの両方 |
| `--dry-run` | **何も変更しない。** 選ばれる段を表示して終了 |
| `--no-live` | `live-sync` 段**だけ**実行しない。`persist` / `repair-seq` / `purge-backup` は実行し、**永続層には書く** |
| `--app` / `--store` / `--key` | 稼働中ストアの同期先（アプリ名 / store 名 / キー）の絞り込み |

- `--no-live` が効くのは `risk.live_sensitive >= 0.5` のときだけ（`live-sync`
  段が選ばれているとき）です。出力は `live=untouched(--no-live)` になります。
- `--dry-run` と `--no-live` は別物です。`--dry-run` は永続層にも書きません。

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

#### `heal [--state PATH] [--app A] [--store S] [--key K] [--dry-run]`

「検出 → 永続層へ書き戻し → 稼働中ストア再水和」を一度に行います。

- `--dry-run` を付けると**検出だけ**して終了します（永続層にも、稼働中ストアにも
  触りません）。
- 永続層への書き込みは常に WAL 追記（`apply --live` と同じ経路）です。
- 対象は `fix` と同じ「所有スレッドがいない surface タブ」です。owner が
  生きているタブは対象になりません（`heal` に `--force` はありません）。

```sh
./bin/neodax-ui heal --state "$STATE" --dry-run
./bin/neodax-ui heal --state "$STATE"
```

#### `apply [--state PATH] [--id ID] [--key KEY] [--live]` / `apply --attach [...]`

永続層へ書き戻します。

| オプション | 効果 |
|---|---|
| （なし） | **stale surface**（所有者がいない surface タブ）を除去して永続層へ書く |
| `--id ID` | そのタブ 1 件だけを除去して永続層へ書く |
| `--key HINT` | 書き込むキーを指定する（既定は `workspace-tabs` を含むキーを自動判別） |
| `--live` | 書き込み方を **WAL 追記**にする。未指定ならオフライン書き込み（`nx_leveldb_write_kv`） |
| `--attach` | **永続層には書かず**、稼働中ストアの再水和だけを行う |

- `--live` は「メモリ側のストア触るか」ではありません。**永続層への書き込み方法**
  の違いです。`--live` なしでも `--attach` なしでも、稼働中 renderer のメモリ側
  には触れません。
- どちらの書き込みでも **seq は「実測最大 + 1」** を使います。桁違いの
  大きい seq を書くと、以降アプリ自身の書き込みが「古い」と判定されて無視され、
  neodax の値が永久に勝ち続けます。
- 書き込み後はストアを読み直して「意図した値が実際に解決されるか」を確かめます。
  検証に失敗したら `VERIFY FAILED` と出て終了コードは非 0 です。
- `--attach` モードでは `--live` は使われません。`--state` があればそこから修復
  JSON を作って再水和し、無ければ現在の localStorage をそのまま再水和します。

```sh
# stale surface をまとめて除去して永続層へ（起動中 LevelDB の WAL に追記）
./bin/neodax-ui apply --state "$STATE" --live

# 1 件だけ
./bin/neodax-ui apply --state "$STATE" --id "tab:1" --live

# 永続層には書かず、稼働中ストアだけ今この値を再水和
./bin/neodax-ui apply --state "$STATE" --attach
```

#### `delete --state PATH --id ID [--live] [--force] [--no-store-sync] [--app A] [--store S] [--key K]`

指定したタブを**無条件に**削除します。`apply` と違うのは、**owner が生きて
いる surface / chat も消せる**ことです（閉じられないタブを消すのが目的）。

| オプション | 効果 |
|---|---|
| `--id ID` | 必須。削除するタブの ID |
| `--live` | 永続層への書き込みを WAL 追加にする（未指定はオフライン書き込み） |
| `--no-store-sync` | **稼働中ストアの同期だけ**止める。永続層への書き込みと退避キーの tombstone は実行する |
| `--force` | 削除拒否の判定を無効化する |
| `--app` / `--store` / `--key` | 稼働中ストア同期先の絞り込み |

- **削除の安全弁**: 削除前に `advise` と同じ判定を行い、
  * owner スレッドが既に消えている surface → そのまま削除可
    （`removable: owner thread is gone ...`）。
  * owner が生きている surface → **拒否**（`REFUSED`。アプリは自分で閉じられる）。
  * chat タブ → **拒否**（`REFUSED`。チャットは自動削除しない）。
  * その ID が状態に無い → 削除対象にせず、**退避コピーだけを掃除**して
    「nothing to persist」と報告します。

  拒否されたときに消すなら `--force` を付けます。
- `--no-store-sync` は「永続層だけ直したい」「アプリが起動していない」場合だけ
  使います。アプリが起動中だと、次の persist で古い値が書き戻され、修復が
  復活することがあります。
- 退避コピー（`.nx-backup`）が読み書きの解決先になっている場合は、本体だけでなく
  退避側も tombstone で消します。

```sh
# まず根拠を確認する
./bin/neodax-ui advise --state "$STATE" --id "tab:1"

# 削除する（起動中 LevelDB への WAL 追記 + 稼働中ストア同期）
./bin/neodax-ui delete --state "$STATE" --id "tab:1" --live

# 拒否されたが、消す判断をした場合
./bin/neodax-ui delete --state "$STATE" --id "tab:1" --force --live
```

```
  [delete] advisory (before touching anything):
  [delete] removable: owner thread is gone (cannot be closed from the UI)
  [delete] tabs removed: 1 (id=tab:1)
  [delete] purged backup copy (key #0)
  [delete] syncing live store (no reload)
  [delete] persisted
```

#### `close --state PATH --id ID` / `fix [--state PATH]`

どちらも**ファイルに書き出すだけで、ストアは触りません**。

- `close` は指定タブを除いた `<path>.fixed.json` を書きます。
- `fix` は孤児 surface を除いた `<path>.fixed.json` を書きます。
- 出力先を見てから自分で反映する、という使い方ができます。

```sh
./bin/neodax-ui close --state "$STATE" --id "tab:1"
./bin/neodax-ui fix   --state "$STATE"
```

### 5.3 稼働中ストア（アプリに今見えている値）

#### `attach [--state PATH] [--app A] [--store S] [--key K]`

稼働中 renderer のストアを再水和します。

- `--state` があればそこから修復 JSON を作って再水和し、無ければ現在の
  localStorage の値をそのまま再水和します。
- 永続層には書きません。
- `apply --attach --id ID` との違いは、`attach` には「どのタブを除去するか」を
  指定する `--id` がないことです（対象は `--state` からの全体修復）。

```sh
./bin/neodax-ui attach --state "$STATE"
./bin/neodax-ui attach --app "DeepAI Code"
```

#### `stop [--app A] [--store S] [--key PREFIX]`

起動時の状態復元を止める JS を差し込みます。次回起動は既定状態になります。

#### `baseline [--app A] [--store S] [--key KEY]`

復元を止めるのではなく、**復元先を空の妥当な状態**に置き換えます。

| コマンド | 起動時の挙動 |
|---|---|
| `stop` | 保存された状態を復元しない |
| `baseline` | 復元するが、中身は空のセッション（タブ 0 個のクリーンな状態） |

```sh
./bin/neodax-ui stop     --app "DeepAI Code"
./bin/neodax-ui baseline --app "DeepAI Code"
```

### 5.4 チャット台帳（`threads`）

別 DB（SQLite）を見ます。各オプションの意味（同時指定の可否を含む）:

| オプション | 意味 | 他との併用 |
|---|---|---|
| `--db PATH` | 対象 SQLite を明示（既定 `~/.deepai-code/userdata/statev2.sqlite`） | 全部と併用可 |
| `--list` | チャット一覧を出す（変更しない） | `--deleted` 併用可 |
| `--deleted` | 削除済みも含める | `--list` と併用（単独では一覧表示なし） |
| `--delete ID` | 指定チャットを削除する | `--list` と同時は不可（削除が優先） |
| `--within N` | 直近 N 秒以内の重複を検出（既定 600） | `--apply` と併用可 |
| `--apply` | 検出した重複を実際に削除する | `--within` 併用可 |

- オプションなし = 重複検出のみ（削除しません）。
- `--apply` を付けない限り **何も削除されません**。
- いまタブが参照している会話は保護されます。
- 残す基準は「タブが参照しているもの → 原本（最古）」です。`updated_at` は
  同期の遅れしか表さないので判定には使いません。

### 5.5 app fix（アプリ本体の書き換え）

#### `recipe --path APP [-o FILE] [--target S]`

書き換え手順書を先に作って保存します（アプリは変更しません）。手順書は
clone → verify-clone → patch → verify-range → observe-stub → resign →
verify-sign の 7 段を 1 枚に綴ったものです。

- `--target` なしなら `NglControllerC1` / `NglControllerC2` / `DoSetup` /
  `ShouldCheckLicense` の順で解決できるものを選びます。
- `--target S` でシンボルを直接指定できます。
- 解決できなかった手順書は `complete=0` になり、`recipe-run` は実行を拒否します。

```sh
./bin/neodax-ui recipe --path "/path/AE.app" --target ShouldCheckLicense -o /tmp/ae.recipe
```

#### `recipe-show --file R`

保存した手順書を表示します。変更はしません。

```sh
./bin/neodax-ui recipe-show --file /tmp/ae.recipe
```

#### `recipe-run --file R [--apply] [--upto N]`

手順を 1 段ずつ検証して実行します。

| オプション | 効果 |
|---|---|
| （なし） | **dry-run**（検証だけ）。作業コピーは作りません |
| `--apply` | 実行する。clone → patch → verify → resign。失敗時は作業コピーを破棄（rollback） |
| `--upto N` | N 段目まで実行して止める（途中経過の確認用） |

```sh
./bin/neodax-ui recipe-run --file /tmp/ae.recipe             # 検証だけ
./bin/neodax-ui recipe-run --file /tmp/ae.recipe --upto 3    # patch まで
./bin/neodax-ui recipe-run --file /tmp/ae.recipe --apply     # 最後まで
```

工程中に確認できる検証結果（`NXRECIPE` / `diff` / `diffs=` / `PASS` / `FAIL`）:

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

| オプション | 効果 |
|---|---|
| （なし） | 解析だけ。`--apply` と同じ工程まで進めるが作業コピーは作らない |
| `--apply` | 最後まで実行して `APP.neodax-work` を作る |
| `--step` | 1 段だけ進める（途中状態を保持） |
| `--abort` | 途中状態を捨てる（チェックポイント削除） |

```sh
./bin/neodax-ui cut --path "/path/AE.app" --symbol ShouldCheckLicense
./bin/neodax-ui cut --step  --path "/path/AE.app" --symbol ShouldCheckLicense
./bin/neodax-ui cut --apply --path "/path/AE.app" --symbol ShouldCheckLicense
./bin/neodax-ui cut --abort --path "/path/AE.app" --symbol ShouldCheckLicense
```

#### `search --path APP --symbol SYM [--apply] [--probe-ms N]`

呼び出し元と先頭命令を観測し、テンプレート（data < edge < entry の順）を選んで
介入します。

- オプションなし = 表示のみ。
- `--apply` = 最安のテンプレを作業コピーに適用します。
- `--probe-ms N` を付けると、適用後に N ms 起動して**生存したら成功**とみなします。

```sh
./bin/neodax-ui search --path "/path/AE.app" --symbol ShouldCheckLicense
./bin/neodax-ui search --path "/path/AE.app" --symbol ShouldCheckLicense --apply --probe-ms 3000
```

#### `best --path APP [--kind app|state|dir] [--apply]`

動かない問題を 1 個に絞り、その 1 個に最善を尽くします。

- `--apply` なしで観測だけ、`--apply` で `APP.neodax-work` まで作成します。

```sh
./bin/neodax-ui best --path "/path/AE.app" --apply
```

#### `app-analyze [--path APP] [--fix]`

分析して app ごとのフォルダに保存します。

- `--fix` を付けると**前回の分析（`after.txt`）と違う時だけ**修正します。
  既に `ShouldCheckLicense` が `mov w0,#0` になっている作業コピーがあれば
  `already` と表示して再 clone しません。

```sh
./bin/neodax-ui app-analyze --path "/path/AE.app"
./bin/neodax-ui app-analyze --path "/path/AE.app" --fix
```

#### `kill [--match SUBSTR]`

`SUBSTR` を含むプロセスを SIGKILL します（既定は `.neodax-work`）。
作業コピーの残存プロセスを落とすときに使います。

```sh
./bin/neodax-ui kill --match ".neodax-work"
```

### 5.6 画面・解析・その他

| コマンド | 概要 | 主なオプション / 使いどころ |
|---|---|---|
| `windows` | 画面上のウィンドウ一覧（`WINID` / `PID` / `AREA` / `OWNER` / `NAME`） | 変更なし |
| `capture --app NAME -o FILE.png` | 対象アプリの画面を PNG 取得 | `-o` 必須（`--out` も可）。CoreGraphics を試し、万一失敗したら `screencapture` にフォールバック |
| `display [--app NAME] [-o FILE] [--state PATH]` | 複数バックエンドを試し、情報量の多いフレームを選ぶ（既定 `/tmp/neodax-display.png`） | 変更なし |
| `spec --hex "0x81 0x05 ..."` / `spec --file PATH [--ws]` | 構造認識（`--ws` は WebSocket フレーム） | 変更なし |
| `gen --spec NAME\|all [--class NAME] [-o FILE]` | spec から C++ クラスを生成 | 変更なし（生成物のみ） |
| `pe --file FILE.exe [--disasm] [--limit N]` | Windows PE の解析 / 逆アセンブル | `--disasm` で逆アセンブル、`--limit` で表示数 |
| `pipeline --file FILE.exe [--strategy auto\|seq\|sim] [--limit N] [-o out.cpp]` | decode + encode の戦略選択（`auto` / `seq` / `sim`） | `-o` を付けると C++ も出力 |
| `serve` | パイプ区切りのコマンドを 1 行ずつ読む常駐モード | GUI ではなく別プロセスで使います |
| `--gui` / `-g` / 引数なし | GUI を起動 | 下記 5.7 |
| `--selftest` | GUI と同じ実行経路を CLI から点検 | **`repair` を含むため状態を変えます** |

```sh
./bin/neodax-ui capture --app "DeepAI Code" -o /tmp/app.png
./bin/neodax-ui spec --hex "0x81 0x05 0x00"
./bin/neodax-ui pe --file "/path/app.exe" --disasm --limit 40
```

### 5.7 GUI

- 起動: `./bin/neodax-ui`（引数なし）/ `--gui` / `-g`。`bin/neodax-ui` は GUI と
  CLI の両方を持ち、GUI は CLI の `nx_cli_dispatch` をそのまま呼びます
  （別実装がありません）。
- **メインウィンドウ**: 左にコマンド一覧（グループ見出し付き）、中央に `|` 区切りの
  argv を編集する引数欄、右に出力。Enter でも実行、出力には `$ neodax-ui <argv>` と
  `[exit N]` が出ます。
- 左の一覧はショートカットで、全コマンドを網羅していません。`close` / `delete` /
  `strategy-memory` / `serve` などは引数欄に直接入力して実行します。
- **app 分析ウィンドウ**: 最近起動した app の一覧から、`分析` / `分析して修正` /
  `1個特定して最善` を実行できます。
- **AE エミュレータウィンドウ**: メインとは別フレーム。ウィジェットを操作すると
  対応する neodax コマンドが実行され、下のログに `$ neodax-ui ...` が出ます。
- **点検**: `./bin/neodax-ui --selftest` は `tabs` / `advise` / `check` / `plan` /
  `repair` を GUI と同じ経路で実行します。**`repair` を含むため状態を変えます。**
  観測だけしたいときは個別に実行してください。

`serve` のコマンド（1 行 1 コマンド、区切りは `|`）:

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

---

## 6. オプション早見表

「そのオプションの有無で挙動がどう変わるか」を 1 枚にまとめました。

| オプション | 効果 | なしとの差 |
|---|---|---|
| `--state PATH` | 対象の LevelDB ディレクトリ | 省略時は既定の場所から自動解決（stderr に通知） |
| `--live` | 永続層への書き込みを **WAL 追記**にする | 未指定はオフライン書き込み。**どちらもメモリ側ストアには触らない** |
| `--attach` | 永続層には書かず、**稼働中ストアの再水和だけ** | `--live` とは別物。このモードで `--live` は使わない |
| `--no-live` | `clean` の `live-sync` 段だけが実行されない | 永続層には**書く**（`--dry-run` は永続層にも書かない） |
| `--no-store-sync` | `delete` の**稼働中ストア同期だけ**止める | 永続層への書き込みと退避キーの tombstone は実行される |
| `--dry-run` | 何も変更しない（`clean` は段を表示、`heal` は検出だけ） | 未指定は実際に変更する |
| `--force` | `delete` の削除拒否を無効化 | 未指定だと壊れていない live surface / chat は拒否される |
| `--id ID` | 対象を 1 タブに限定（`apply` / `delete` / `close` / `advise` / `apply --attach`） | `apply` は stale surface 全体、`advise` は全体判定 |
| `--key HINT` | 書き込むキー / 対象キーを指定 | `apply` は `workspace-tabs` を含むキーを自動判別 |
| `--app` / `--store` | 稼働中ストア同期先の絞り込み（アプリ名 / store 名） | 既定は DeepAI Code の既定プロファイル |
| `--within N` | `threads` の重複検出幅（秒、既定 600） | 大きいほど広く拾う |
| `--apply` | それぞれ「実際に実行するの意味」 | `threads` = 重複を削除 / `recipe-run` = 手順を実行 / `cut`・`best`・`search` = 作業コピー作成 / `advise-proc` = 導出手順を実行 / `app-analyze` = 前回と違う時だけ修正 |
| `--upto N` | `recipe-run` を N 段で止める | なしなら全段 |
| `--probe-ms N` | `search` の適用後に N ms 起動し生存を判定 | なしなら起動判定なし |
| `--kind` | `case` / `best` の対象種別（`app` / `state` / `dir`） | 省略時は `app` |
| `--target S` | `recipe` で対象のシンボルを直接指定 | なしなら既定の候補順で自動解決 |
| `--case tpl[N]` | `case` で使う案件を明示 | なしなら対象に合う案件を登録順に試す |

---

## 7. 出力マーカー

| マーカー | 出るコマンド | 意味 |
|---|---|---|
| `STATE_SOURCE` | `tabs`, `keys` | 実際に読んだ状態の場所 |
| `MAX_SEQ` / `COUNT` | `keys` | 観測した最大 seq / キー数 |
| `LIVE` | `keys` | `workspace-tabs` と `nx-backup` の生存 |
| `RESOLVED_KEY` / `RESOLVED_IS_BACKUP` | `keys` | 読み書きに使われるキーと、退避側へのすり替わり |
| `VERDICT=HEALTHY` / `NEEDS_FIX` / `UNKNOWN` | `plan`, `case`, `repair` | 判定 |
| `VERDICT=YES` / `NO` / `UNKNOWN` | `advise` | 判定（閉じられないタブがあるか） |
| `VERDICT=CLEAN` / `STILL_DIRTY` | `clean` | 残滓がなくなったか |
| `confidence=` | `plan`, `case` | 根拠の強さ |
| `stuck surface tabs: N` | `check`, `heal` | 閉じられない surface の件数 |
| `removable=yes/no` | `advise --id` | その ID を削除してよいか |
| `PROCEDURE` / `STEP` / `ITEM` / `CONFIDENCE` | `advise-proc` | 導出した手順と根拠 |
| `RE-DERIVE` | `advise-proc --apply` | 実行後の再導出 |
| `risk = destructive/order/live/subtle` | `clean` | 4 つの安全度指標 |
| `LAYOUT_BEGIN` / `WIN` / `LAYOUT_END` | `layout` | 復元されたかどうかの比較用署名 |
| `NXRECIPE` | `recipe*` | 手順書フォーマット |
| `complete=0` | `recipe` | 対象シンボル未解決。実行不可 |
| `diff` / `diffs=` / `PASS` / `FAIL` | `recipe-run`, `cut` | 差分範囲・stub・署名の検証 |
| `[delete] REFUSED` / `removable` / `forced despite` | `delete` | 削除の可否と根拠 |
| `persisted=yes/no` / `live=synced/untouched(--no-live)` | `clean` | 永続層に書けたか / 稼働中ストアを同期したか |
| `VERIFY FAILED` | `apply`, `delete` | 書き込んだ値が実際に解決されなかった |

---

## 8. 内部の層（設計の視点）

利用者から見た分類（タスク起点インデックス）とは別に、**実装の責務**で
分けると次の 4 層になります。どのコマンドがどの層を踏むかを知っておくと
「何が内部で動くか」が分かります。

| 層 | 主な利用者 | 目的 | 代表コマンド | 状態を変えるか |
|---|---|---|---|---|
| **A. エージェント / 状態修復** | AGENTS.md の作業者 | タブ・チャット・永続層の観測と修復 | `tabs`, `keys`, `advise`, `plan`, `repair`, `clean`, `apply`, `delete` | 観測系は変えない。修復系は変える |
| **B. app fix / アプリ本体の書き換え** | アプリを直す作業者 | 動かないアプリを解析し、作業コピーを作る | `case`, `best`, `cut`, `search`, `recipe`, `recipe-run`, `app-analyze --fix` | 原版は変えない。`APP.neodax-work` を変える |
| **C. appfix / 稼働中アプリへの再適用** | A/B の内部 | 起動中の renderer に状態を再適用する | `apply --attach`, `delete`, `clean`, `heal`, `stop`, `baseline` | 稼働中ストアを変える（reload しない） |
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

**重要:** `appfix` は CLI サブコマンドではありません。C の内部 API
(`nx_appfix_*`) です。CLI からは A の修復コマンド越しにだけ使います。

---

## 9. 保存場所

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

## 10. やってはいけないこと

- 原版の状態を消してメモリに残す（次の persist で復活します）。
- 稼働中アプリを reload / 再起動して反映する（ユーザーの作業を壊します）。
- `advise` の根拠なしに `delete --force` する。
- LevelDB を行指向の `grep` / `sed` / `awk` で読む（バイナリで誤読します）。
  状態を見るときは `tabs` / `keys` / `advise` / `check` / `plan` を使ってください。
- 稼働中のアプリを `open -a` などで再起動して状態を検証する（反映は状態そのもの
  を書き換えて行います）。

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
