# neodax-java-review

Java の「どこを・何を・何を使って書くか」を、`javac` の解析結果から出す reviewer。

Java パーサは持たない。JDK の Compiler Tree API (`JavacTask` / `Trees` /
`Elements` / `Types`) に parse と analyze を任せ、解決済みの要素・型・位置だけを使う。

## 使い方

```sh
tools/java-review/neodax-java-review <repo-dir>
tools/java-review/neodax-java-review <repo-dir> --next
```

`--next` はスタブをスコア順に短く出す。

## 依存

- 実行時のみ **JDK**（`javac` 入り）が必要。
- `make` / `gui/build.sh` など neodax 本体のビルドには不要。
- JRE だけでは `ToolProvider.getSystemJavaCompiler()` が `null` になるため動かない。

コンパイル確認:

```sh
make -C tools/java-review check
```

## 出力

- `where`: メソッド宣言の `file:line:column`（`@Override` ではなく宣言行）
- `signature`: 戻り値・引数を含むシグネチャ
- `marker`: `TODO` / `task1` / `ここを実装` などの本文コメント
- `contract`: override 元 / interface 宣言の場所と Javadoc
- `call` / `call(contract)`: 直接呼び出しと、interface 経由の呼び出し
- `expect`: `expect(label, expected, actual)` 形状から拾った期待値
- `material`: 戻り値・引数の型にある既存メソッド/フィールド（実ファイル位置つき）
- `skeleton`: そのまま書ける骨組み
- `javac diagnostics`: `-Xlint:all` の警告（Javadoc の付け忘れなど）
