#!/bin/bash
# Build the native NeoDAX viewer: AppKit + Core Graphics (CPU), analyzer linked
# in-process (no JSON/TSV).  usage: bash gui/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# analysis objects (framework), excluding the CLI mains
make -C src all >/dev/null
OBJS="src/macho.o src/arm64_decode.o src/x86_decode.o \
      src/core/macho_static.o src/analysis/depflow.o src/analysis/variable_classifier.o \
      src/flow/execution_order.o src/flow/parallel_detection.o \
      src/tracking/state_tracking.o \
      src/integration/nx_engine.o src/integration/nx_decode.o \
      src/integration/nx_provider_macho.o src/integration/nx_provider_dax.o \
      src/integration/nx_json.o src/integration/nx_leveldb.o \
      src/integration/nx_state.o src/integration/nx_deepai.o \
      src/integration/neodax_bridge.o"

clang++ -std=c++17 -ObjC++ -fobjc-arc -O2 \
    -I src -I include \
    -framework Cocoa \
    gui/neodax-viewer.mm $OBJS -lpthread -lm \
    -o build_neodax/neodax-viewer
echo "built: build_neodax/neodax-viewer"

# --- minimal CLI (GUI とは独立の別プロセス) ---
# 画面キャプチャ + タブ状態の可読出力/選択削除/シミュレーション。
# GUI 版の描画・操作は変更しない。
# --- attach package (self-contained): inspect + renderer + payload + appfix ---
ATTACH_OBJS="src/integration/attach/nx_inspect.o src/integration/attach/nx_renderer.o src/integration/attach/nx_payload.o src/integration/attach/nx_appfix.o"
SPEC_OBJS="src/integration/spec/nx_spec.o src/integration/spec/nx_specs_builtin.o src/integration/spec/nx_recog.o src/integration/spec/nx_gen.o src/integration/spec/nx_pe.o"
UI_OBJS="src/integration/nx_json.o src/integration/nx_leveldb.o src/integration/nx_state.o src/integration/nx_deepai.o src/integration/nx_sim.o $ATTACH_OBJS $SPEC_OBJS"
clang++ -std=c++17 -ObjC++ -fobjc-arc -O2 \
    -I src -I include \
    -framework Cocoa -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
    -framework ApplicationServices \
    gui/neodax-ui.mm $UI_OBJS -lpthread -lm \
    -o build_neodax/neodax-ui
echo "built: build_neodax/neodax-ui"
