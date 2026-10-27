#!/bin/bash
# neodax-fork-undergoing: src と src_new の最小統合（削除は1ブロックに集約）
# 使い方: リポジトリのルートで実行  bash integrate.sh
set -euo pipefail
cd "$(dirname "$0")"

echo "== 1) src_new を正として src 側に同期 =="
mkdir -p src/analysis src/core src/flow src/integration src/tracking
cp src_new/core/macho_static.c      src/core/macho_static.c
cp src_new/core/macho_static.h      src/core/macho_static.h
cp src_new/flow/execution_order.c   src/flow/execution_order.c
cp src_new/flow/execution_order.h   src/flow/execution_order.h
cp src_new/flow/parallel_detection.c src/flow/parallel_detection.c
cp src_new/flow/parallel_detection.h src/flow/parallel_detection.h
cp src_new/tracking/state_tracking.c src/tracking/state_tracking.c
cp src_new/tracking/state_tracking.h src/tracking/state_tracking.h
cp src_new/integration/neodax_bridge.c src/integration/neodax_bridge.c
cp src_new/integration/neodax_bridge.h src/integration/neodax_bridge.h
cp src_new/analysis/depflow.c        src/analysis/depflow.c
cp src_new/analysis/depflow.h        src/analysis/depflow.h
cp src_new/analysis/variable_classifier.c src/analysis/variable_classifier.c
cp src_new/analysis/variable_classifier.h src/analysis/variable_classifier.h
# フレームワークの main は src_new 版で上書き（旧エンジンは engine/ に退避）
mkdir -p engine
for f in main.c main_integrated.c; do
  [ -f "src/$f" ] && mv "src/$f" "engine/legacy_$f" || true
done
cp src_new/main.c            src/main.c
cp src_new/main_integrated.c src/main_integrated.c
cp src_new/Makefile          src/Makefile

echo "== 2) parallel_detection.h: MUTEX重複削除 + BEAM型追加 =="
# 二重定義の片方を除去
awk '!(seen[$0]++ && $0 ~ /PARALLEL_SYNC_MUTEX/)' \
  src/flow/parallel_detection.h src_new/flow/parallel_detection.h > /tmp/pd.h && \
  mv /tmp/pd.h src/flow/parallel_detection.h && \
  cp src/flow/parallel_detection.h src_new/flow/parallel_detection.h
# beam dispatch 専用型を復元
for h in src/flow/parallel_detection.h src_new/flow/parallel_detection.h; do
  sed -i 's/#define PARALLEL_SYNC_MUTEX 6/#define PARALLEL_SYNC_MUTEX 6\n#define PARALLEL_DISPATCH_BEAM 7/' "$h"
done

echo "== 3) parallel_detection.c: beam分類を専用型に =="
for c in src/flow/parallel_detection.c src_new/flow/parallel_detection.c; do
  # beam search ブランチの dispatch を DIRECT から BEAM に戻す
  perl -0pi -e 's/(is_beam_search_function\(group->name\)\)\s*\{\s*\n\s*group->type = PARALLEL_TYPE_BEAM_SEARCH;[\s\S]*?)group->dispatch_type = PARALLEL_DISPATCH_DIRECT;/$1group->dispatch_type = PARALLEL_DISPATCH_BEAM;/' "$c"
done

echo "== 4) src/Makefile 修正: 空だった analysis 参照は解消済み。BUILD_DIR を共通化 =="
sed -i 's|BUILD_DIR = bin|BUILD_DIR = ../build_neodax|' src/Makefile src_new/Makefile

echo "== 5) 削除ブロック（stale成果物・空ディレクトリ・旧重複を一括） =="
find src src_new engine -type f \( -name '*.o' -o -name '*.tmp' -o -name '*.o.*' \) -delete
rmdir src/static_analysis 2>/dev/null || true
# 統合済みなので src_new をルートから残す場合はビルドのみ src から行う
find . -name '.DS_Store' -delete

echo "== 6) ビルド検証 =="
make -C src clean >/dev/null 2>&1 || true
make -C src all
echo "OK: bin は src/bin → ../build_neodax に出力"