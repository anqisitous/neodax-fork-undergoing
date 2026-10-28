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
      src/integration/neodax_bridge.o"

clang++ -std=c++17 -ObjC++ -fobjc-arc -O2 \
    -I src -I include \
    -framework Cocoa \
    gui/neodax-viewer.mm $OBJS -lpthread -lm \
    -o build_neodax/neodax-viewer
echo "built: build_neodax/neodax-viewer"
