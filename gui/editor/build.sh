#!/bin/bash
# Optional build. This is NOT part of NeoDAX.app and is NOT called by gui/build.sh.
# usage: bash gui/editor/build.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p bin
clang++ -std=c++17 -ObjC++ -fobjc-arc -O2 -Wall \
    -I gui/editor \
    gui/editor/neodax_editor.mm gui/editor/nx_editor_model.cpp \
    -framework Cocoa -o bin/neodax-editor
echo "built: bin/neodax-editor (standalone; not in NeoDAX.app)"
