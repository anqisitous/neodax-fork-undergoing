# AGENTS.md — neodax で作業するエージェントへの規約

## 検索は `grep` ではなく `seek`

**`grep` / `rg` / `sed` / `awk` をテキスト走査に使わない。代わりに `seek` を使う。**
`python3` も使わない。

```sh
./bin/neodax-ui seek "nx_cli_dispatch" src
./bin/neodax-ui seek "todo|fixme" . --regex --ignore-case --include "*.cpp"
./bin/neodax-ui seek "workspace-tabs" . --files
```

- `PATTERN` は `seek` の直後。`ROOT` はその後の最初の `-` 以外の引数。
  `seek --regex "a|b" .` は `--regex` を探しに行くので誤り。
- 末尾の `[limit]` / `[time]` / `[bytes]` は**打ち切った印**。見つからなかった
  わけではない。`--limit` / `--time` を上げるか `--include` で絞って再実行する。
- `seek` はソース・ドキュメント・asar を探す道具。**状態の判定に使わない**。

| 探したいもの | 使うもの |
|---|---|
| リポジトリ内の文字列 | `seek` |
| asar の中身 | `asar --file APP.asar --find TEXT` |
| バイナリの構造 | `pe` / `spec` / `pipeline` |
| 実行中アプリの localStorage | `probe --app NAME` |
| タブ状態 | `tabs` / `keys` / `advise` / `check` / `plan` / `splice` |

ソースを読むときは `read` ツール（行番号付き）。一覧が要るなら neodax に
サブコマンドを足して C 側で出す。

## 状態は neodax のコマンドだけで扱う

LevelDB はバイナリなので、行指向の走査では正しく読めない。状態の観測・判定・
修復・検証はすべて `./bin/neodax-ui` のサブコマンドで行う。

```sh
./bin/neodax-ui tabs        # タブ一覧
./bin/neodax-ui advise      # 壊れているか
./bin/neodax-ui advise --count   # 閉じられない surface の件数だけ (check と同じ)
./bin/neodax-ui plan        # 事実 → 問題 → 計画
./bin/neodax-ui splice      # 観測をデータとして渡し、検査コードに判定させる
```

`--state` は省略可（既知の場所を自動で探す）。

### 中身が同じコマンドを増やさない

コマンドとオプションは `gui/nx_cli_registry.c` に一元化されている。
新しいコマンドを足すのは、**他のどれとも中身が違うとき**だけ。

- 名前が違っても同じ経路なら別名にする（`check` → `advise --count`、
  `heal` → `clean`）。実装を 2 つ持たない。
- オプションは「名前が似ているか」ではなく「**同じものを指すか**」で
  族に分ける。`--dir`（資産の場所）と `--state`（LevelDB）は別物。
  `--show`（実行しない）と `--dry-run`（書かない）も別の軸。
- 宣言していないオプションを渡すと `rc=2` で止まる。黙って無視されない。

### 直すとき

```sh
./bin/neodax-ui repair      # 観測 → 計画 → 実行 → 再観測
./bin/neodax-ui splice      # 直後に必ず確認する
```

**`repair` が `VERDICT=HEALTHY` を出しても、それだけで直ったと判断しない。**
修復中に値が解決したことを確認しているだけで、その後にアプリが古い状態を
書き戻すと戻る。`splice` は観測を事実 (TSV) として渡し、検査する側に判定
させるので、`FINDING` が出るかどうかで戻ったかが分かる。

### `splice` の流れ（抽象ノード → データ/コード → バイナリ → 実行）

段を追うとこうなる。**コードを焼く段と実行する段は別**。

```
observe → tree → emit → bake → run
  │        │       │       │      └─ バイナリに TSV を stdin で渡す
  │        │       │       └──────── C++ をバイナリにし、hash でキャッシュ
  │        │       └──────────────── node が data 行と rule コードを出す
  │        └──────────────────────── Node 抽象体を依存で並べる
  └───────────────────────────── 事実を集める
```

- **Node**（抽象基底）: `data()` で TSV 行、`rule()` で判定コードを出すだけ。
  `emit` / `bake` / `run` はノードの種類を知らない。検査を足すのは
  **Node を 1 つ書いて木に入れる**だけで、生成器には触らない。
- **code** = 何を検査するか。neodax が書く。**状態を焼き込まない**ので、
  同じコードは何度でも・どんな状態にも使える。
- **data** = 検査の材料。観測のたびに作る TSV。1 行 3 列 `<kind>\t<key>\t<value>`。
- 渡し方は **stdin**。argv でも埋め込みでもないので、長さ・NUL・改行の
  エスケープを気にせずに済む。
- **bake** はコードのハッシュをキーに `~/.neodax/splice/<hash>.bin` を 1 度
  だけ作る。同じコードなら焼き直さない。
- **run** はそのバイナリを実行する。**バイナリは単体で動く**ので、
  neodax を通さず `bin < data.tsv` でも同じ判定が出せる。

段を分けて叩ける:

```sh
./bin/neodax-ui splice --show   # データとコードを表示（焼かない）
./bin/neodax-ui splice --bake   # バイナリを焼くところまで（実行しない）
./bin/neodax-ui splice --run    # 焼かずにキャッシュ済みバイナリで実行
./bin/neodax-ui splice         # bake + run（キャッシュがあれば再利用）
```

焼いたものは直接実行できる。これが「バイナリファイルとして実行する」の意味:

```sh
~/.neodax/splice/<hash>.bin < data.tsv
```

### 資産の形は中核が auto で受ける

呼ぶ側が「これは cpp か svg か」を判定して関数を選ばない。
`nx_tplcpp_open(src, &g, &kind)` が中で振り分ける。

```sh
./bin/neodax-ui tpl-cpp --dir src/templates   # cpp 群 (dir)
./bin/neodax-ui tpl-cpp --dir asset.svg       # 図から読み戻す (svg)
./bin/neodax-ui tpl-cpp --dir graph.node      # 1 行 1 ノード/リンク (node)
```

- **部分資産を捨てない。** `id:` が無い資産はファイル名から id を作る。
  ノードを 1 つだけ書いたファイルを後から足せる。
- **link の端点は全体から名前で解決する。** 部分資産は `persist` を
  宣言せずに `link: prune converges persist` と書ける。
- **同名ノードは 1 つに寄せる。** 複数の部分資産が `persist` を宣言しても増えない。
- `--svg` の出力は `data-id` / `data-from` / `data-to` / `data-kind` を持つので
  そのまま読み戻せる（cpp → svg → cpp で構成が一致することを確認済み）。

`trace` は先に**門番**を通す。孤立も ring 矛盾も深い到達も無ければ、
分岐は全部切られてコンパイルしない（0.58s → 0.009s）。
「見なかった」と「異常なし」を混ぜないため、切ったことは `PRUNED` で明示する。

### 戻る原因を疑う順

1. **稼働中ストアが古い値を持っている** — `splice` の出力と `tabs` が食い違う。
2. **退避キーが解決先になっている** — `keys` の `RESOLVED_IS_BACKUP=1`、
   または `splice` の `FINDING 解決先が退避コピー`。
3. **起動時に古い状態を復元している** — `stop` / `baseline`。

`clean` はこの 3 つを種類と安全度から判定して段を選ぶ。`--dry-run` で
計画だけ見られる。1 件だけ消すなら `advise --id` で `removable=yes` を確かめて
から `delete --id`。

## 絶対禁止

- **対象アプリを reload / 再起動しない。** `location.reload()`、
  `app.relaunch()`、`open -a` による再起動、プロセス kill を「状態を反映させる
  ため」に使わない。反映は状態そのものを書き換えて行う。
- **消えてほしい状態を JS で parse / 走査しない。** `performance.getEntriesByType`
  の列挙や `await import()` による探索、JS での JSON 組み立ては禁止。解析と
  判定は C 側（`nx_deepai` / `nx_state` / `nx_leveldb` / `nx_json`）で行う。
- **ad-hoc な JS を注入して済ませない。** 変更は `gui/neodax-ui.mm` と
  `src/integration/**` に入れ、`bin/neodax-ui` をビルドし直し、そのバイナリの
  サブコマンドとして実行する。
- **頼まれていないコードを書き換えない。** 特に `append_store_resolve` のような
  共通部品。

reload が禁止なのは、ユーザーの作業セッションを壊すからです。永続層を直せば
再起動時に正しく復元されます。localStorage だけ消してメモリに残すと、次の
persist で復活します（これが「消しても復活する」の原因）。

## 状態を消す手順

1. 事実確認: `./bin/neodax-ui tabs`
2. 削除可否: `./bin/neodax-ui advise --id "<ID>"`
3. 修復内容の作成: `./bin/neodax-ui fix`（ファイルに書くだけ）
4. 永続層へ反映: `./bin/neodax-ui apply --live`
5. 検証: `./bin/neodax-ui splice`

1 件だけなら 2 の後に `delete --id "<ID>" --live`。
