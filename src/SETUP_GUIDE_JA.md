# 🚀 NeoDAX 静的解析フレームワーク - 仮セットアップガイド

## 現在の状態

**⚠️ 重要**: 現在、フレームワークは**開発中**で、コンパイルエラーが残っています。

このガイドでは、現時点での**仮セットアップ方法**を説明します。

---

## 1️⃣ 前提条件

### 必要なツール

```bash
# GCCまたはClang（C99に対応）
gcc --version
# または
clang --version

# make
make --version

# pthreadライブラリ（通常は標準でインストール済み）
```

### 対応プラットフォーム

- ✅ **macOS** (Intel / Apple Silicon)
- ✅ **Linux** (x86_64 / ARM64)
- ❌ Windows (非対応)

---

## 2️⃣ リポジトリの準備

```bash
# 既にクローン済みの場合
cd /workspace/github__anqisitous__neodax-fork-undergoing

# src_newディレクトリに移動
cd src_new
```

---

## 3️⃣ コンパイルエラーの修正

### 主なエラーと修正方法

#### エラー1: `unknown type name 'FILE'`

**原因**: `stdio.h` がインクルードされていません。

**修正**:
```bash
# core/macho_static.hの先頭に追加
#include <stdio.h>
```

**ファイル**: `src_new/core/macho_static.h`

#### エラー2: `implicit declaration of function 'strstr'`

**原因**: `string.h` がインクルードされていません。

**修正**:
```bash
# 以下のファイルの先頭に追加
#include <string.h>

# 対象ファイル:
# - src_new/flow/parallel_detection.h
# - src_new/tracking/state_tracking.h
# - src_new/analysis/variable_classifier.h
```

#### エラー3: `implicit declaration of function 'va_start'`

**原因**: `stdarg.h` がインクルードされていません。

**修正**:
```bash
# 以下のファイルの先頭に追加
#include <stdarg.h>

# 対象ファイル:
# - src_new/integration/neodax_bridge.c
```

#### エラー4: `unknown type name 'execution_graph_t'`

**原因**: 型名の不一致です。

**修正**:
```bash
# src_new/integration/neodax_bridge.h
# execution_graph_t → exec_order_graph_t

# src_new/integration/neodax_bridge.h
# parallel_detection_t → parallel_analysis_t
```

**具体的な修正**：
```c
// 変更前
execution_graph_t exec_graph;
parallel_detection_t parallel_detector;

// 変更後
exec_order_graph_t exec_graph;
parallel_analysis_t parallel_detector;
```

#### エラー5: `has no member named 'node_count'`

**原因**: 構造体メンバー名の不一致です。

**修正**:
```bash
# src_new/integration/neodax_bridge.c
# ibin->exec_graph.node_count → ibin->exec_graph.nnodes
# ibin->dep_graph.node_count → ibin->dep_graph.node_count
```

**具体的な修正**：
```c
// 変更前
if (ibin->exec_graph.node_count > 0) {

// 変更後
if (ibin->exec_graph.nnodes > 0) {
```

#### エラー6: `incompatible pointer type`

**原因**: 関数ポインタ型の不一致です。

**修正**:
```bash
# src_new/integration/neodax_bridge.c
# var_classifier_export_dotの引数を修正

# 変更前
var_classifier_export_dot(&ibin->classifier, fp);

# 変更後
var_classifier_export_dot(&ibin->classifier, NULL);
```

---

## 4️⃣ 修正後のビルド

### 全ての修正を行った後

```bash
# クリーン
make clean

# ビルド（並列コンパイル）
make -j4
```

### ビルド成功の確認

```bash
# ビルドされたファイルを確認
ls -la bin/

# 期待されるファイル:
# - bin/neodax_static
# - bin/neodax_static_integrated
```

---

## 5️⃣ 実行方法

### 基本的な使い方

```bash
# 静的解析を実行
./bin/neodax_static --static-analysis 対象ファイル.mach-o

# 将棋AI向け解析
./bin/neodax_static --shogi 将棋エンジン.mach-o

# 変数分類
./bin/neodax_static --classify-vars 対象ファイル.mach-o
```

### 主なオプション

| オプション | 説明 |
|----------|------|
| `--static-analysis` | 静的解析を有効化 |
| `--classify-vars` | 変数分類を実行 |
| `--predict-execution` | 実行順序予測を実行 |
| `--detect-parallel` | 並列実行検出を実行 |
| `--track-state` | 状態追跡を実行 |
| `--beam-search` | ビームサーチ解析を実行 |
| `--shogi` | 将棋AI向け解析を有効化 |
| `--parallel-complement` | 並列処理を使った補完を有効化 |
| `--threads N` | 並列処理のスレッド数（デフォルト: 4） |
| `-o FILE` | DOT出力ファイル名 |
| `--show-dep-graph` | 依存関係グラフを表示 |
| `--show-exec-graph` | 実行グラフを表示 |
| `--show-classification` | 変数分類を表示 |

### 例: 将棋AIエンジンの解析

```bash
# 全ての解析を実行
./bin/neodax_static \
  --static-analysis \
  --classify-vars \
  --detect-parallel \
  --track-state \
  --shogi \
  shogi_engine.mach-o
```

### 例: DOTファイルの生成

```bash
# DOTファイルを生成
./bin/neodax_static \
  --static-analysis \
  --shogi \
  -o analysis.dot \
  shogi_engine.mach-o

# Graphvizで可視化
.dot analysis.dot -Tpng -o analysis.png
```

---

## 6️⃣ 既存のNeoDAXとの統合

### 方法1: 独立したツールとして使用

```bash
# src_newを別途ビルド
cd src_new
make

# 実行
./bin/neodax_static [オプション] ファイル
```

### 方法2: 既存のNeoDAXに統合

#### Step 1: CLIオプションを追加

`src/main.c` に以下のオプションを追加：

```c
// src/main.cのarg parsingに追加
if (!strcmp(argv[i], "--static-analysis")) { opts.static_analysis = 1; continue; }
if (!strcmp(argv[i], "--classify-vars")) { opts.classify_vars = 1; continue; }
if (!strcmp(argv[i], "--shogi")) { opts.shogi = 1; continue; }
if (!strcmp(argv[i], "--detect-parallel")) { opts.detect_parallel = 1; continue; }
if (!strcmp(argv[i], "--predict-execution")) { opts.predict_execution = 1; continue; }
if (!strcmp(argv[i], "--track-state")) { opts.track_state = 1; continue; }
if (!strcmp(argv[i], "--beam-search")) { opts.beam_search = 1; continue; }
if (!strcmp(argv[i], "--parallel-complement")) { opts.parallel_complement = 1; continue; }
if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
    opts.num_threads = atoi(argv[++i]);
    continue;
}
```

#### Step 2: 解析パスを追加

`src/main.c` の解析パイプラインに以下を追加：

```c
// 静的解析を実行
if (opts.static_analysis || opts.classify_vars || opts.detect_parallel || 
    opts.predict_execution || opts.track_state || opts.shogi) {
    
    // 静的解析モジュールを初期化
    integrated_binary_t ibin;
    static_analysis_opts_t static_opts;
    
    // オプションを設定
    static_opts.enable_static_analysis = opts.static_analysis;
    static_opts.classify_variables = opts.classify_vars || opts.shogi;
    static_opts.detect_parallel = opts.detect_parallel || opts.shogi;
    static_opts.predict_execution_order = opts.predict_execution || opts.shogi;
    static_opts.track_state = opts.track_state || opts.shogi;
    static_opts.beam_search_analysis = opts.beam_search || opts.shogi;
    static_opts.shogi_mode = opts.shogi;
    static_opts.parallel_complement = opts.parallel_complement;
    static_opts.num_threads = opts.num_threads;
    
    // 静的解析を実行
    if (neodax_bridge_init(&ibin, bin->data, bin->size) == 0) {
        neodax_run_static_analysis(&ibin, &static_opts, stdout);
        
        if (static_opts.parallel_complement) {
            neodax_parallel_complement(&ibin, &static_opts, stdout);
        }
        
        if (static_opts.shogi_mode) {
            neodax_shogi_analysis(&ibin, &static_opts, stdout);
        }
        
        neodax_bridge_free(&ibin);
    }
}
```

#### Step 3: Makefileを更新

`Makefile` に `src_new/` のモジュールを追加：

```makefile
# src_newのソースファイルを追加
SRC_NEW = \
    src_new/core/macho_static.c \
    src_new/analysis/depflow.c \
    src_new/analysis/variable_classifier.c \
    src_new/flow/execution_order.c \
    src_new/flow/parallel_detection.c \
    src_new/tracking/state_tracking.c \
    src_new/integration/neodax_bridge.c

# オブジェクトファイル
OBJ_NEW = $(SRC_NEW:.c=.o)

# 依存関係
neodax: $(OBJ) $(OBJ_NEW)
```

---

## 7️⃣ トラブルシューティング

### 問題1: コンパイルエラーが解決しない

**解決策**：
1. エラーメッセージをよく読む
2. 指定されたファイルと行番号を確認
3. 型名や関数名の不一致を修正
4. 必要なヘッダーを追加

**例**：
```
error: 'FILE' undeclared
```
→ `#include <stdio.h>` を追加

### 問題2: リンクエラー

**解決策**：
1. Makefileの依存関係を確認
2. 必要なライブラリを追加
3. オブジェクトファイルが生成されているか確認

**例**：
```
undefined reference to 'pthread_create'
```
→ Makefileに `-lpthread` を追加

### 問題3: セグメントーションフォルト

**解決策**：
1. NULLポインタのチェックを追加
2. 配列の境界チェックを追加
3. メモリ割り当てを確認

### 問題4: 遅い

**解決策**：
1. 並列処理を有効化：`--parallel-complement --threads 4`
2. 解析範囲を限定：特定の関数のみ解析
3. 最適化フラグを使用：`-O2` 或は `-O3`

---

## 8️⃣ 開発環境のセットアップ

### macOSの場合

```bash
# Xcode Command Line Toolsをインストール
xcode-select --install

# Homebrewをインストール（まだの場合）
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 必要なツールをインストール
brew install make gcc
```

### Linuxの場合

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install build-essential make gcc

# Fedora/RHEL
sudo dnf install make gcc gcc-c++
```

---

## 9️⃣ テスト

### 簡単なテスト

```bash
# 自分自身を解析（テスト用）
./bin/neodax_static --static-analysis ./bin/neodax_static

# 簡単なCプログラムをコンパイルして解析
cat > test.c << 'EOF'
#include <stdio.h>
int main() {
    int board[9][9];
    int move = 0;
    for (int i = 0; i < 9; i++) {
        for (int j = 0; j < 9; j++) {
            board[i][j] = 0;
        }
    }
    return 0;
}
EOF

# macOSの場合
gcc -o test test.c
./bin/neodax_static --classify-vars test
```

---

## 📋 チェックリスト

- [ ] GCC/Clangがインストールされている
- [ ] makeがインストールされている
- [ ] リポジトリがクローンされている
- [ ] `src_new/` ディレクトリに移動
- [ ] コンパイルエラーを修正
  - [ ] `#include <stdio.h>` を追加
  - [ ] `#include <string.h>` を追加
  - [ ] `#include <stdarg.h>` を追加
  - [ ] 型名の不一致を修正
  - [ ] 構造体メンバー名を修正
- [ ] `make clean && make -j4` を実行
- [ ] ビルドが成功
- [ ] 実行テスト

---

## 💡 Tips

### 1. 部分的なビルド

特定のモジュールのみをビルド：

```bash
# coreモジュールのみ
make core/macho_static.o

# analysisモジュールのみ
make analysis/depflow.o analysis/variable_classifier.o
```

### 2. デバッグビルド

```bash
# デバッグシンボル付きでビルド
make CFLAGS="-g -O0"

# デバッガで実行
gdb ./bin/neodax_static
```

### 3. 警告を表示

```bash
# 警告を表示してビルド
make CFLAGS="-Wall -Wextra -Werror"
```

---

## 📞 サポート

### 一般的な質問

1. **Q: コンパイルエラーが解決しません**
   - A: エラーメッセージをコピーして、GitHub Issuesに投稿してください

2. **Q: どのオプションを使えばいいですか？**
   - A: 将棋AIの解析には `--shogi` オプションを使用してください

3. **Q: 並列処理は安全ですか？**
   - A: はい、pthreadを使用して安全に実装されています

4. **Q: 大きなファイルを解析できますか？**
   - A: 理論的には可能ですが、メモリが不足する可能性があります

---

## 🎯 次のステップ

1. **コンパイルエラーを修正** → ビルド成功
2. **簡単なテスト** → 基本機能の確認
3. **既存のNeoDAXと統合** → 完全な統合
4. **実バイナリでテスト** → 実用性の確認
5. **パフォーマンスチューニング** → 高速化

---

## 📚 関連ドキュメント

- [README_JA.md](./README_JA.md) - 日本語ドキュメント
- [INTEGRATION_SUMMARY.md](../INTEGRATION_SUMMARY.md) - 統合サマリー
- [INTEGRATION_PLAN.md](../INTEGRATION_PLAN.md) - 統合計画
- [ARCHITECTURE_COMPARISON.md](../ARCHITECTURE_COMPARISON.md) - アーキテクチャ比較

---

**最後の更新**: 2024年
**状態**: 開発中（コンパイルエラー修正中）
