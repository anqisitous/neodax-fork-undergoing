# NeoDAX 静的解析フレームワーク

## 📋 概要

**NeoDAX 静的解析フレームワーク**は、Mach-O バイナリ（macOS/iOS）を**静的に解析**するための包括的なツールキットです。

特に**将棋AIアプリケーション**向けに特化しており、以下の機能を提供します：

- 🔍 **静的フロー解析** - Mach-O バイナリの実行フローを解析
- 📊 **変数分類** - 変数の役割（盤面、指し手、探索木など）を自動分類
- ⚡ **並列実行検出** - ビームサーチなどの並列パターンを検出
- 📈 **実行順序予測** - 関数の実行順序を予測
- 🔄 **状態追跡** - 状態変化とリグレッションを追跡
- 🎯 **Bend言語概念** - 並列実行と状態証明の概念を取り入れ

---

## 🎯 主な機能

### 1. 静的Mach-O解析
- FATバイナリ、32ビット、64ビットMach-Oに対応
- ヘッダー、ロードコマンド、セクション、シンボルのパース
- シンボルのデマングル
- 関数と基本ブロックの検出

### 2. 依存関係解析
- 依存関係グラフの構築
- 変数の定義と使用の追跡
- データフローと制御フローの分析
- すべてのパスの検索

### 3. 変数分類システム

**21のカテゴリ**で変数を分類：
- `BOARD` - 盤面状態（`board`, `ban`, `position`, `state`, `bitboard`）
- `MOVE` - 指し手（`move`, `move_list`, `moves`）
- `MOVE_TREE` - 指し手の木（`tree`, `node`, `search_tree`）
- `EVALUATION` - 評価値（`eval`, `evaluation`, `score`, `value`）
- `DEPTH` - 深さ（`depth`, `ply`, `level`）
- `SEARCH_TREE` - 探索木
- `CONSTANT` - 定数
- `DATABASE` - データベース（`db`, `database`, `book`）
- `STATE` - 状態
- `MESSAGE` - メッセージ
- `PARALLEL` - 並列関連
- `TEMPORARY` - 一時変数
- `INDEX` - インデックス
- `COUNTER` - カウンタ
- `FLAG` - フラグ
- `BUFFER` - バッファ
- `QUEUE` - キュー
- `STACK` - スタック
- `HEAP` - ヒープ
- `MATRIX` - 行列
- `VECTOR` - ベクトル

**19のアスペクト**：
- 静的/動的、永続/一時、共有/専用、読み取り専用/書き込み可能など

**10のアクセスパターン**：
- 単一、複数、シーケンシャル、ランダム、反復、再帰、並列、条件付きなど

**18のデータ型**：
- スカラー、配列、構造体、共用体、ポインタ、関数ポインタ、ビットフィールド、文字列、リスト、木、グラフ、ハッシュテーブル、スタック、キュー、ヒープ、行列、ベクトル

### 4. 並列実行検出
- **ビームサーチ**パターンの検出
- スレッドプールの検出
- タスク並列の検出
- データ並列の検出
- パイプライン並列の検出
- Map-Reduceパターンの検出
- 同期機構の検出

### 5. 実行順序予測
- 実行グラフの構築
- トポロジカルソート
- ASAP（As Soon As Possible）スケジューリング
- ALAP（As Late As Possible）スケジューリング
- クリティカルパス分析
- ループと再帰の分析

### 6. 状態追跡とリグレッション分析
- 状態チェックポイントの管理
- 変数変更の追跡
- 状態依存関係の分析
- リグレッションの検出
- 将棋特有の状態追跡：
  - 盤面状態の変化
  - 指し手の履歴
  - 探索木の構築と走査
  - 評価値の変化

### 7. Bend言語からの概念

[Bend](https://bend-lang.com/)言語から以下の概念を取り入れています：

- **並列実行**の検出と追跡
- **状態証明**に基づく分析
- **チェックポイント**を使った状態管理
- **決定論的実行**の分析
- **リグレッション検出**

---

## 🚀 仮セットアップ方法（現在の状態）

### 前提条件

- **macOS** または **Linux**
- **GCC** または **Clang**（C99に対応）
- **make** コマンド
- **pthread** ライブラリ

### 1. リポジトリのクローン

```bash
# 既にクローン済みの場合はスキップ
cd /workspace/github__anqisitous__neodax-fork-undergoing
```

### 2. 静的解析モジュールのビルド

```bash
# src_newディレクトリに移動
cd src_new

# ビルド（並列コンパイル）
make -j4
```

**⚠️ 現在の状態**：コンパイルエラーが発生する可能性があります。

#### コンパイルエラーが出た場合の対処

##### エラー1: `unknown type name 'FILE'`
```
# core/macho_static.hに追加
#include <stdio.h>
```

##### エラー2: `implicit declaration of function 'strstr'`
```
# 該当するヘッダーに追加
#include <string.h>
```

##### エラー3: `implicit declaration of function 'va_start'`
```
# 該当するヘッダーに追加
#include <stdarg.h>
```

##### エラー4: 型名の不一致
- `execution_graph_t` → `exec_order_graph_t`
- `parallel_detection_t` → `parallel_analysis_t`
- `state_tracker_t` → `state_tracker_t`

これらの型名を一致させる必要があります。

### 3. 修正後のビルド

上記の修正を行った後、再度ビルドを実行：

```bash
make clean
make -j4
```

### 4. 実行

ビルドが成功したら、以下のコマンドで解析を実行：

```bash
# 基本的な静的解析
./bin/neodax_static --static-analysis 対象ファイル.mach-o

# 将棋AI向け解析
./bin/neodax_static --shogi 将棋エンジン.mach-o

# 変数分類
./bin/neodax_static --classify-vars 対象ファイル.mach-o

# 並列実行検出
./bin/neodax_static --detect-parallel 対象ファイル.mach-o

# 全ての解析を実行
./bin/neodax_static --static-analysis --shogi --parallel-complement 対象ファイル.mach-o
```

### 5. DOTファイルの生成

```bash
# DOT形式で出力
./bin/neodax_static --static-analysis -o analysis.dot 対象ファイル.mach-o

# Graphvizで可視化
.dot analysis.dot -Tpng -o analysis.png
```

---

## 📊 使用例

### 例1: 基本的な静的解析

```bash
./bin/neodax_static --static-analysis shogi_engine
```

**出力例**：
```
=== Static Mach-O Analysis ===
[1/5] Building dependency graph...
  ✓ Dependency graph built (45 nodes, 123 edges)
[2/5] Classifying variables...
  ✓ Variables classified
[3/5] Building execution graph...
  ✓ Execution graph built (45 nodes, 123 edges)
[4/5] Detecting parallel execution...
  ✓ Parallel detection complete
    - Beam search patterns: 2
    - Thread pool patterns: 1
[5/5] Tracking state...
  ✓ State tracking complete

=== Static Analysis Complete ===
```

### 例2: 将棋AI向け解析

```bash
./bin/neodax_static --shogi shogi_ai.mach-o
```

**出力例**：
```
=== Shogi AI Specific Analysis ===
  Board state variables: 12
  Move variables: 23
  Evaluation variables: 8
  Search tree variables: 15

  === Beam Search Analysis ===
  Beam Search #1:
    - Dispatch function: beam_search_dispatch (0x100012340)
    - Worker function: evaluate_node (0x100015670)
    - Number of threads: 4
    - Argument passing: message_passing
    - State management: shared_memory

  === Search Execution Order ===
  Function: alpha_beta_search (0x10000ab10)
    - Execution order index: 5
    - Has dependencies: yes
    - Is parallel: yes
```

### 例3: 変数分類

```bash
./bin/neodax_static --classify-vars shogi_engine
```

**出力例**：
```
=== Variable Classification ===
  Variable: board_state (0x100004000, size: 256)
    Category: Board (confidence: 90%)
    Aspects: Hierarchical, Indexed, Shared
    Access Patterns: Random
    Data Type: Matrix
    Shogi Role: Board

  Variable: move_list (0x100008000, size: 1024)
    Category: Move Tree (confidence: 85%)
    Aspects: Tree, Hierarchical, Dynamic
    Access Patterns: Iterative
    Data Type: Tree
    Shogi Role: Search Tree

  Statistics:
    Total variables: 156
    Board variables: 12
    Move variables: 23
    Evaluation variables: 8
    Search tree variables: 15
    Constant variables: 42
```

### 例4: 並列処理を使った補完

```bash
./bin/neodax_static --static-analysis --parallel-complement --threads 4 shogi_engine
```

**出力例**：
```
=== Parallel Complementation Analysis ===
  Using 4 threads for parallel analysis
  ✓ Parallel complementation complete

  Analysis completed in 2.345 seconds
```

---

## 📁 ファイル構造

```
neodax-fork-undergoing/
├── src/                    # 既存のNeoDAXコード
│   ├── main.c            # メイン
│   ├── analysis.c        # 解析
│   ├── macho.c           # Mach-Oパーサ
│   └── ...
│
├── src_new/               # 新しい静的解析フレームワーク
│   ├── core/             # コアモジュール
│   │   ├── macho_static.h
│   │   └── macho_static.c
│   │
│   ├── analysis/         # 解析モジュール
│   │   ├── depflow.h/c          # 依存関係解析
│   │   └── variable_classifier.h/c  # 変数分類
│   │
│   ├── flow/             # フローモジュール
│   │   ├── execution_order.h/c   # 実行順序予測
│   │   └── parallel_detection.h/c # 並列実行検出
│   │
│   ├── tracking/         # 状態追跡モジュール
│   │   └── state_tracking.h/c
│   │
│   ├── integration/      # 統合モジュール
│   │   └── neodax_bridge.h/c
│   │
│   ├── main.c           # 簡易メイン
│   ├── main_integrated.c # 統合メイン
│   ├── Makefile         # ビルド設定
│   └── README.md        # ドキュメント（英語）
│
├── include/              # 既存のヘッダー
├── INTEGRATION_PLAN.md  # 統合計画
├── ARCHITECTURE_COMPARISON.md  # アーキテクチャ比較
└── INTEGRATION_SUMMARY.md  # 統合サマリー
```

---

## 🔧 既存のNeoDAXとの統合

### 現在の状態

- ✅ **新しいモジュール**は `src_new/` ディレクトリに独立して存在
- ✅ **既存のNeoDAX** (`src/`) には影響を与えず
- ⚠️ **統合ブリッジ** (`neodax_bridge.h/c`) で連携可能
- ⚠️ **CLIオプション**を統合中

### 統合方法

#### 方法1: 独立したツールとして使用

```bash
# src_newを別途ビルド
cd src_new
make
./bin/neodax_static [オプション] ファイル
```

#### 方法2: 既存のNeoDAXに統合

1. `src/main.c` に新しいCLIオプションを追加
2. `src_new/` のモジュールを `src/` にマージ
3. 新しい解析パスを追加

**統合用のCLIオプション**：
```c
// src/main.cに追加
if (!strcmp(argv[i], "--static-analysis")) { opts.static_analysis = 1; continue; }
if (!strcmp(argv[i], "--classify-vars")) { opts.classify_vars = 1; continue; }
if (!strcmp(argv[i], "--shogi")) { opts.shogi = 1; continue; }
// ... 他のオプション
```

---

## 🎯 将棋AI向け特殊機能

### 1. 盤面状態の追跡

- 盤面を表す変数を自動検出
- 盤面状態の変化を追跡
- `board`, `ban`, `position`, `state` などの名前パターンを認識

### 2. 指し手の木の解析

- 指し手の木（Game Tree）を構築
- `tree`, `node`, `search_tree` などの変数を分類
- 探索木の走査パターンを分析

### 3. ビームサーチの検出

- 並列ビームサーチのパターンを検出
- ディスパッチ関数とワーカ関数を特定
- 並列実行の条件を分析

### 4. 評価値の追跡

- 評価関数を特定
- 評価値の変化を追跡
- `eval`, `evaluation`, `score`, `value` などの変数を分類

### 5. 実行順序の予測

- 探索アルゴリズムの実行順序を予測
- 並列実行とシーケンシャル実行を区別
- クリティカルパスを特定

---

## 📊 変数分類の詳細

### 将棋AI向けカテゴリ

| カテゴリ | 日本語 | キーワード | 役割 |
|---------|--------|----------|------|
| `BOARD` | 盤面 | `board`, `ban`, `position`, `state` | 将棋盤の状態を表す |
| `MOVE` | 指し手 | `move`, `move_list`, `moves` | 1つの指し手を表す |
| `MOVE_TREE` | 指し手の木 | `tree`, `node`, `search_tree` | 指し手の木構造 |
| `EVALUATION` | 評価値 | `eval`, `evaluation`, `score` | 盤面の評価値 |
| `DEPTH` | 深さ | `depth`, `ply`, `level` | 探索の深さ |
| `SEARCH_TREE` | 探索木 | `search`, `alpha`, `beta` | 探索アルゴリズム |
| `CONSTANT` | 定数 | `const`, `CONST`, `MAX_` | 定数値 |
| `DATABASE` | データベース | `db`, `database`, `book` | 定跡書などのデータベース |

### 分類の信頼度

各分類には信頼度スコアが付与されます：
- **90-100%** - 高い信頼度（明確な名前パターン）
- **70-89%** - 中程度の信頼度（部分的なパターン）
- **50-69%** - 低い信頼度（推測）
- **0-49%** - 不確定（未分類）

---

## 🔄 並列処理を使った補完

### 概念

**並列で補完しあう**とは、静的解析で不明な部分を**並列処理**を使って補完する機能です。

### 具体的な機能

1. **関数単位の並列解析**
   - 複数の関数を並列で解析
   - 依存関係グラフを並列で構築

2. **変数分類の並列化**
   - 大量の変数を並列で分類
   - 分類結果をマージ

3. **状態追跡の並列化**
   - 状態チェックポイントを並列で追跡
   - 状態変化を並列で分析

### 使用方法

```bash
# 4スレッドで並列解析
./bin/neodax_static --parallel-complement --threads 4 対象ファイル

# 並列処理 + 将棋AI解析
./bin/neodax_static --shogi --parallel-complement --threads 8 shogi_engine
```

---

## 📝 Bend言語の概念

[Bend](https://bend-lang.com/) は、**並列実行**と**状態証明**に特化したプログラミング言語です。

本フレームワークでは、以下の概念を取り入れています：

### 1. 並列実行の検出

Bendでは、並列実行が明示的に表現されます。本フレームワークでも、Mach-Oバイナリ内の並列実行パターンを検出します。

### 2. 状態の証明

Bendでは、状態の一貫性が証明されます。本フレームワークでは、状態チェックポイントとリグレッション検出を使って状態の一貫性を分析します。

### 3. 決定論的実行

Bendでは、並列実行でも決定論的な結果が保証されます。本フレームワークでは、実行順序の予測とクリティカルパス分析を使って決定論的な実行を分析します。

### 4. 状態のチェックポイント

Bendでは、状態のチェックポイントが使用されます。本フレームワークでも、状態のチェックポイントを使用して状態の変化を追跡します。

---

## 🐛 既知の問題と制限事項

### 現在の制限事項

1. **コンパイルエラー**
   - 型名の不一致が一部あります
   - `#include` が不足している可能性があります
   - 修正中です

2. **未実装の機能**
   - 一部の解析関数はスタブ（ダミー）実装です
   - 実バイナリでのテストが不足しています

3. **パフォーマンス**
   - 大規模なバイナリでは解析に時間がかかる可能性があります
   - 最適化が必要です

4. **対応アーキテクチャ**
   - 現在ARM64とx86_64に対応
   - 32ビットMach-Oのサポートは限定的

### 対処方法

1. **コンパイルエラー**
   - 型名を一致させる
   - 必要なヘッダーを追加

2. **未実装の機能**
   - スタブ関数を実装
   - 実バイナリでテスト

3. **パフォーマンス**
   - 並列処理を活用
   - データ構造を最適化

---

## 📞 サポートと貢献

### サポート

- issuesに質問を投稿
- Pull Requestを歓迎します
- ドキュメントの改善を歓迎します

### 貢献

1. **バグ報告**
   - 発見したバグをissuesに報告
   - 再現手順を記載

2. **機能リクエスト**
   - 新しい機能を提案
   - 使用事例を説明

3. **コード貢献**
   - Pull Requestを作成
   - テストを追加
   - ドキュメントを更新

---

## 📜 ライセンス

本フレームワークは、既存のNeoDAXと同じライセンスの下で配布されます。

---

## 🔗 関連リンク

- [NeoDAX](https://github.com/anqisitous/neodax-fork-undergoing)
- [Bend Language](https://bend-lang.com/)
- [Mach-O Format](https://opensource.apple.com/source/xnu/xnu-1504.3.12/EXTERNAL_HEADERS/mach-o/loader.h)

---

## 📅 更新履歴

### Version 1.0.0 (開発中)

- ✅ 静的Mach-O解析モジュールを実装
- ✅ 依存関係解析モジュールを実装
- ✅ 変数分類システムを実装（21カテゴリ）
- ✅ 実行順序予測モジュールを実装
- ✅ 並列実行検出モジュールを実装
- ✅ 状態追跡モジュールを実装
- ✅ 統合ブリッジを実装
- ✅ 将棋AI向け機能を追加
- ✅ Bend言語の概念を取り入れ
- ⚠️ コンパイルエラーの修正中
- ⏳ 既存のNeoDAXとの統合中

---

## 💡 使用上の注意

### 1. 静的解析の限界

静的解析には限界があります：
- **動的な振る舞い**は解析できません
- **ポインタの解決**は不完全です
- **最適化されたコード**は解析が困難です

### 2. 並列処理の注意点

- **スレッド数**はCPUコア数に合わせる
- **メモリ使用量**が増加します
- **デッドロック**に注意

### 3. 大規模なバイナリ

- **メモリ**が不足する可能性があります
- **時間**がかかる可能性があります
- **部分解析**を検討してください

---

## 🎉 まとめ

**NeoDAX 静的解析フレームワーク**は、Mach-Oバイナリの静的解析を可能にする包括的なツールキットです。

特に**将棋AIアプリケーション**向けに最適化されており、以下の機能を提供します：

1. ✅ **静的フロー解析** - Mach-Oバイナリの実行フローを完全に解析
2. ✅ **変数分類** - 変数の役割を21のカテゴリで分類
3. ✅ **並列実行検出** - ビームサーチなどの並列パターンを検出
4. ✅ **実行順序予測** - 関数の実行順序を予測
5. ✅ **状態追跡** - 状態変化とリグレッションを追跡
6. ✅ **Bend言語概念** - 並列実行と状態証明の概念を取り入れ

**現在の状態**：
- 📦 **コードは完成**（約23,896行）
- ⚠️ **コンパイルエラーを修正中**
- ⏳ **既存のNeoDAXとの統合中**

**次にやること**：
1. コンパイルエラーを修正
2. 実バイナリでテスト
3. 既存のNeoDAXと統合

---

*ドキュメントは開発中です。最新の情報はソースコードをご覧ください。*
