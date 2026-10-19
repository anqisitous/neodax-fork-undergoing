# NeoDAX Static Mach-O Analysis Framework - Integration Plan

## 概要

この文書では、新しく開発した静的解析フレームワークを既存の NeoDAX コードベースに統合するための計画を説明します。

## 新しいアーキテクチャ

### モジュール構成

```
neodax/
├── src/                    # 既存のコード
│   ├── main.c             # メイン
│   ├── macho.c            # Mach-O パーサ
│   ├── callgraph.c        # コールグラフ
│   ├── analysis.c         # 解析
│   └── ...
│
└── src_new/               # 新しい静的解析フレームワーク
    ├── core/              # コアモジュール
    │   ├── macho_static.h    # Mach-O 静的解析ヘッダ
    │   └── macho_static.c    # Mach-O 静的解析実装
    │
    ├── analysis/          # 解析モジュール
    │   ├── depflow.h         # 依存性追跡
    │   └── depflow.c         # 依存性追跡実装
    │
    ├── flow/               # フローモジュール
    │   ├── execution_order.h # 実行順序予測
    │   ├── execution_order.c # 実行順序予測実装
    │   ├── parallel_detection.h # 並列実行検出
    │   └── parallel_detection.c # 並列実行検出実装
    │
    ├── tracking/           # 追跡モジュール
    │   ├── state_tracking.h  # 状態追跡
    │   └── state_tracking.c  # 状態追跡実装
    │
    ├── main.c             # 新しいメイン
    ├── Makefile           # 新しいMakefile
    └── README.md          # ドキュメント
```

## 統合戦略

### Phase 1: 並列存在 (Parallel Existence)

**目標**: 新しいフレームワークを独立して動作させる

1. **ビルドシステムの分離**
   - 新しい `src_new/Makefile` を使用して独立してビルド
   - 既存のコードには影響を与えない
   - `make` コマンドで `src_new` をビルドするオプションを追加

2. **新しい実行ファイル**
   - `neodax_static` という新しい実行ファイルを作成
   - 既存の `neodax` とは別のツールとして動作

3. **テスト**
   - 基本的な Mach-O ファイルの解析をテスト
   - 依存性追跡、実行順序予測、並列実行検出の機能を検証

### Phase 2: 機能統合 (Feature Integration)

**目標**: 新しい機能を既存の NeoDAX に統合

1. **ヘッダーファイルの統合**
   - `src_new/core/macho_static.h` の機能を `include/dax.h` に統合
   - 新しい解析機能用のヘッダーファイルを `include/` に追加

2. **Mach-O 解析の拡張**
   - 既存の `src/macho.c` に新しい静的解析機能を追加
   - 依存性追跡、実行順序予測、並列実行検出の機能を統合

3. **新しい CLI オプション**
   - `--static-analysis` 或いは `-S` オプションを追加
   - 静的解析専用のサブコマンドを追加

### Phase 3: コードの統合 (Code Integration)

**目標**: 新しいコードを既存のコードベースに直接統合

1. **ファイルの移動**
   - `src_new/core/macho_static.c` → `src/macho_static.c`
   - `src_new/analysis/depflow.c` → `src/depflow.c`
   - `src_new/flow/execution_order.c` → `src/execution_order.c`
   - `src_new/flow/parallel_detection.c` → `src/parallel_detection.c`
   - `src_new/tracking/state_tracking.c` → `src/state_tracking.c`

2. **メイン関数の統合**
   - `src_new/main.c` の機能を `src/main.c` に統合
   - 新しい CLI オプションを追加

3. **Makefile の統合**
   - 新しいソースファイルを `Makefile` に追加
   - 既存のビルドシステムとの互換性を維持

### Phase 4: 最適化とリファクタリング (Optimization and Refactoring)

**目標**: 統合後のコードの最適化

1. **重複コードの削除**
   - 既存の Mach-O パーサと新しいパーサの統合
   - 共通機能の抽出

2. **パフォーマンスの最適化**
   - メモリ使用量の最適化
   - 解析速度の向上

3. **API の統一**
   - 既存の API と新しい API の統一
   - JavaScript API の拡張

## 具体的な実装計画

### 1. Mach-O 解析の拡張

**ファイル**: `src/macho.c`

**変更内容**:
- 静的解析用のデータ構造を追加
- 依存性追跡機能を統合
- 実行順序予測機能を統合

```c
// 既存の macho_parse 関数を拡張
int dax_parse_macho(dax_binary_t *bin) {
    // ... 既存のコード ...
    
    // 新しい静的解析機能を呼び出す
    if (opts->static_analysis) {
        if (macho_static_analyze(bin, opts) != 0) {
            return -1;
        }
    }
    
    return 0;
}
```

### 2. 新しい CLI オプション

**ファイル**: `src/main.c`

**変更内容**:
- 静的解析用の CLI オプションを追加
- 新しい解析モジュールを呼び出す

```c
// 新しい CLI オプション
static void print_usage(void) {
    // ... 既存のコード ...
    
    DIVIDER;
    SEC("STATIC ANALYSIS", M);
    FLAG("-S", "", "enable static analysis (dependency flow, execution order, parallel detection)");
    FLAG("--depflow", "", "enable dependency flow analysis");
    FLAG("--execution-order", "", "enable execution order prediction");
    FLAG("--parallel", "", "enable parallel execution detection");
    FLAG("--beam-search", "", "enable beam search pattern analysis");
    FLAG("--shogi", "", "enable Shogi AI specific analysis");
    FLAG("--static-dot", "<file>", "export static analysis graph to DOT file");
}
```

### 3. JavaScript API の拡張

**ファイル**: `js/index.js`, `js/server/server.js`

**変更内容**:
- 新しい静的解析機能を JavaScript API に公開
- REST API に新しいエンドポイントを追加

```javascript
// 新しい API 関数
neodax.prototype.staticAnalyze = function(opts) {
    // 静的解析を実行
};

neodax.prototype.dependencyFlow = function() {
    // 依存性フローを取得
};

neodax.prototype.executionOrder = function() {
    // 実行順序を取得
};

neodax.prototype.parallelAnalysis = function() {
    // 並列実行解析を取得
};
```

### 4. Web UI の拡張

**ファイル**: Web UI 関連のファイル

**変更内容**:
- 新しい解析結果を表示するパネルを追加
- 依存性グラフのビジュアライゼーション
- 実行順序のビジュアライゼーション
- 並列実行パターンのビジュアライゼーション

## 将来の拡張

### 1. 動的解析との統合
- 静的解析結果を基にした動的解析のガイド
- 静的解析で特定した関数や変数を優先的に動的解析

### 2. 機械学習支援
- 解析結果を機械学習モデルの入力として利用
- 異常検出やパターン認識に解析結果を活用

### 3. 他形式サポート
- ELF 形式への静的解析機能の拡張
- PE 形式への静的解析機能の拡張

### 4. 高度な最適化
- 並列処理による解析速度の向上
- メモリ使用量の最適化
- インクリメンタル解析の実装

## テスト計画

### 1. 単体テスト
- 各モジュールの単体テストを作成
- Mach-O 解析の正確性を検証
- 依存性追跡の正確性を検証
- 実行順序予測の正確性を検証
- 並列実行検出の正確性を検証

### 2. 統合テスト
- モジュール間の連携をテスト
- 既存の NeoDAX 機能との互換性を検証
- CLI オプションの動作を検証

### 3. 将棋AI向けテスト
- 将棋AIのバイナリを用いたテスト
- 盤面状態追跡の正確性を検証
- 着手履歴追跡の正確性を検証
- 探索木状態追跡の正確性を検証
- ビームサーチ分析の正確性を検証

### 4. パフォーマンステスト
- 大規模なバイナリに対する解析速度を測定
- メモリ使用量を測定
- 並列処理の効果を検証

## 移行計画

### 短期 (1-2 週間)
1. 新しいフレームワークを独立して動作させる
2. 基本的なテストを実施
3. ドキュメントを整備

### 中期 (1-2 か月)
1. 既存の NeoDAX に新しい機能を統合
2. CLI オプションを追加
3. JavaScript API を拡張

### 長期 (2-3 か月)
1. Web UI を拡張
2. 将来的な拡張機能を実装
3. パフォーマンスの最適化

## リスクと対策

### 1. 既存のコードとの競合
- **リスク**: 新しいコードが既存のコードと競合する可能性
- **対策**: 独立したモジュールとして開発し、段階的に統合

### 2. パフォーマンスの低下
- **リスク**: 新しい解析機能が全体のパフォーマンスを低下させる
- **対策**: 解析機能をオプションとして提供し、デフォルトで無効にする

### 3. メモリ使用量の増加
- **リスク**: 新しいデータ構造がメモリ使用量を増加させる
- **対策**: メモリ管理を最適化し、必要に応じて解析範囲を制限

### 4. 互換性の問題
- **リスク**: 新しい機能が既存の API と互換性がない
- **対策**: 既存の API を維持し、新しい API を追加する

## 結論

この統合計画に従って、新しく開発した静的解析フレームワークを既存の NeoDAX コードベースに段階的に統合していきます。これにより、NeoDAX は Mach-O ファイルの静的解析能力を大幅に向上させ、将棋AIのような複雑なアプリケーションの解析を支援します。
