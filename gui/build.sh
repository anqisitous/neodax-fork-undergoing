#!/bin/bash
# Build the native NeoDAX viewer: AppKit + Core Graphics (CPU), analyzer linked
# in-process (no JSON/TSV).  usage: bash gui/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# analysis objects (framework), excluding the CLI mains
make -C src framework >/dev/null
mkdir -p bin
OBJS="src/macho.o src/arm64_decode.o src/x86_decode.o \
      src/core/macho_static.o src/analysis/depflow.o src/analysis/variable_classifier.o \
      src/flow/execution_order.o src/flow/parallel_detection.o \
      src/tracking/state_tracking.o \
      src/integration/nx_engine.o src/integration/nx_decode.o \
      src/integration/nx_provider_macho.o src/integration/nx_provider_dax.o \
      src/integration/nx_json.o src/integration/nx_leveldb.o \
      src/integration/nx_state.o src/integration/nx_deepai.o \
      src/integration/neodax_bridge.o"

clang++ -std=c++17 -O2 \
    -I src -I include -I gui \
    gui/viewer_main.cpp gui/model/nx_graph.cpp gui/model/nx_render.cpp \
    $OBJS -lpthread -lm \
    -o bin/neodax-viewer
echo "built: bin/neodax-viewer"

# --- 残骸検出 (純 C++ / AppKit 非依存) ---
# 「消しても復活する」原因になる checkpoint / patch / archive / 手順書 / backup を検出する。
clang++ -std=c++17 -O2 -Wall \
    -I gui \
    gui/artifact_main.cpp gui/model/nx_artifact.cpp \
    -o bin/neodax-artifact
 echo "built: bin/neodax-artifact"

# --- 画面プローブ (純 C / Objective-C なし) ---
# CoreGraphics / ApplicationServices は C API なので .c で書ける。
for t in detect screen; do
    cc -std=c11 -O2 -Wall \
        -framework CoreGraphics -framework CoreFoundation -framework ApplicationServices \
        gui/neodax-$t.c -o bin/neodax-$t
    echo "built: bin/neodax-$t"
done

# --- minimal CLI (GUI とは独立の別プロセス) ---
# 画面キャプチャ + タブ状態の可読出力/選択削除/シミュレーション。
# GUI 版の描画・操作は変更しない。
# --- attach package (self-contained): inspect + renderer + payload + appfix ---
ATTACH_OBJS="src/integration/attach/nx_inspect.o src/integration/attach/nx_renderer.o src/integration/attach/nx_payload.o src/integration/attach/nx_appfix.o"
SPEC_OBJS="src/integration/spec/nx_spec.o src/integration/spec/nx_specs_builtin.o src/integration/spec/nx_recog.o src/integration/spec/nx_gen.o src/integration/spec/nx_pe.o src/integration/spec/nx_pe_disasm.o src/integration/spec/nx_pipeline.o"
UI_OBJS="src/integration/nx_decode.o src/x86_decode.o src/arm64_decode.o src/integration/nx_json.o src/integration/nx_leveldb.o src/integration/nx_state.o src/integration/nx_deepai.o src/integration/nx_sim.o $ATTACH_OBJS $SPEC_OBJS"
clang++ -std=c++17 -ObjC++ -fobjc-arc -O2 \
    -I src -I include \
    -framework Cocoa -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
    -framework ApplicationServices \
    gui/neodax-ui.mm $UI_OBJS -lpthread -lm \
    -o bin/neodax-ui
echo "built: bin/neodax-ui"
