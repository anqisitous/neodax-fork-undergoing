# neodax-java-review

Java の「どこを・何を・何を使って書くか」を、`javac` の解析結果から出す reviewer。

Java パーサは持たない。JDK の Compiler Tree API (`JavacTask` / `Trees` /
`Elements` / `Types`) に parse と analyze を任せ、解決済みの要素・型・位置だけを使う。

## 使い方

```sh
tools/java-review/neodax-java-review <repo-dir>
tools/java-review/neodax-java-review <repo-dir> --next
tools/java-review/neodax-java-review <repo-dir> --map
```

`--next` はスタブをスコア順に短く出す。`--map` は「名前 → 直す場所」の対応表だけを出す。

## 依存

- 実行時のみ **JDK**（`javac` 入り）が必要。
- `make` / `gui/build.sh` など neodax 本体のビルドには不要。
- JRE だけでは `ToolProvider.getSystemJavaCompiler()` が `null` になるため動かない。

コンパイル確認:

```sh
make -C tools/java-review check
```

## 実装箇所の自動判定

マーカー (`task1` / `TODO`) が無くても、次の材料から候補を決める。

- override / interface の契約がある
- 本体が空
- 本体が `0` / `null` / `false` / `zero(...)` などの中立値を返す
- 引数を読んでいない
- テストが非中立な期待値を書いているのに、本体は中立値を返す
- `UnsupportedOperationException` を投げる

確度は `high` / `medium` / `low`。`why` に判定理由を出す。

## 修正手順 (plan)

neodax の「観測 → 問題 → 計画 → 検証」に合わせて、デフォルト出力に `## plan` を出す。

- `verdict=NEEDS_FIX` のときだけ、`implement` → `verify` → `re-review` の順に step を出す。
- `implement` step は `where` / `body` / `contract` / `uses`（既存の材料）を出す。
- `verify` step はテスト期待値の位置を出す。
- `re-review` step は `stubs=0 errors=0` を期待する。
- スタブもコンパイルエラーも無ければ `verdict=HEALTHY`, `steps=0`。手順は出さない。

## 出力

- `fix map`: `task1` / `task2` / `task3` などの名前 → `file:line:column` → 契約 → 期待値 → 確度の対応表
- `plan`: 直す順番、`where`、使う既存メソッド、検証する期待値
- `where`: メソッド宣言の `file:line:column`（`@Override` ではなく宣言行）
- `signature`: 戻り値・引数を含むシグネチャ
- `marker`: `TODO` / `task1` / `ここを実装` などの本文コメント
- `confidence` / `why`: 自動判定の確度と根拠（マーカー、空本体、中立 return、契約、テスト期待値）
- `contract`: override 元 / interface 宣言の場所と Javadoc
- `call` / `call(contract)`: 直接呼び出しと、interface 経由の呼び出し
- `expect`: `expect(label, expected, actual)` 形状から拾った期待値
- `material`: 戻り値・引数の型にある既存メソッド/フィールド（実ファイル位置つき）
- `original`: 元のメソッドをそのまま行番号つきで表示（置き換える範囲）
- `skeleton`: そのまま書ける骨組み
- `javac diagnostics`: `-Xlint:all` の警告（Javadoc の付け忘れなど）
