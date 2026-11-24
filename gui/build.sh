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
      src/integration/nx_json.o \
      src/integration/nx_state.o src/integration/nx_deepai.o \
      src/integration/neodax_bridge.o"

clang++ -std=c++17 -O2 \
    -I src -I include -I gui \
    gui/viewer_main.cpp gui/model/nx_graph.cpp gui/model/nx_render.cpp gui/model/nx_emit.cpp gui/model/nx_uilayout.cpp \
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
#
# リンクする実装は「分類ごとの静的ライブラリ」で与える。
# リンカはアーカイブ単位で引き抜くので、どの分類を使うかは
# gui/neodax-ui.mm が include したヘッダで決まる。
# ここに個別の .o を並べない (コードとビルドで同じことを 2 回書かない)。
make -s -C src nx-categories
clang++ -std=c++17 -ObjC++ -fobjc-arc -O2 -DNX_REVIEWER=1 \
    -I src -I include \
    -framework Cocoa -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
    -framework ApplicationServices \
    gui/neodax-ui.mm gui/nx_cli_registry.c src/lib/*.a -lpthread -lm -lsqlite3 \
    -o bin/neodax-ui
echo "built: bin/neodax-ui"

# --- necessary-node editor (reviewer app のメイン画面) ---
# standalone の editor を別バイナリとして作り、.app の実行ファイルにする。
# 従来の neodax-ui は同じ .app 内に残す (editor のメニューから起動できる)。
clang++ -std=c++17 -ObjC++ -fobjc-arc -O2 -Wall \
    -I gui/editor \
    gui/editor/neodax_editor.mm gui/editor/nx_editor_model.cpp \
    -framework Cocoa -o bin/neodax-editor
echo "built: bin/neodax-editor"

# --- .app バンドル化 ---
# 既存のバイナリ群を 1 つの .app にまとめ、解析用フォルダを中に置く。
# 解析成果物 (SVG / display モデル / パッチ控え) の置き場所を
# リポジトリの外へ散らかさないための入れ物。
APP="bin/NeoDAX.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/Analysis"
mkdir -p "$APP/Contents/Resources/java-review"
cp -f tools/java-review/JavaReview.java tools/java-review/neodax-java-review \
      "$APP/Contents/Resources/java-review/" 2>/dev/null || true
for t in neodax neodax-ui neodax-viewer neodax-screen neodax-artifact neodax-editor; do
    [ -x "bin/$t" ] && cp -f "bin/$t" "$APP/Contents/MacOS/"
done

# --- アイコン (.icns) ---
# CoreGraphics で 1024px を描き、sips で各サイズを、iconutil で .icns を作る。
# python3 等に依存しない。
ICON_SRC="bin/neodax-icon-1024.png"
ICONSET="bin/AppIcon.iconset"
cc -std=c11 -O2 -Wall \
    -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
    gui/mkicon.c -o bin/neodax-mkicon
./bin/neodax-mkicon "$ICON_SRC" 1024 >/dev/null 2>&1
if [ -f "$ICON_SRC" ]; then
    rm -rf "$ICONSET"; mkdir -p "$ICONSET"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$ICON_SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null 2>&1
        d=$((s * 2))
        sips -z $d $d "$ICON_SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null 2>&1
    done
    if iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null; then
        echo "built: $APP/Contents/Resources/AppIcon.icns"
    else
        echo "  [warn] iconutil failed; app will use the default icon"
    fi
    rm -rf "$ICONSET"   # 中間ファイルは残さない (icns だけあればよい)
fi

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>NeoDAX</string>
  <key>CFBundleDisplayName</key><string>NeoDAX</string>
  <key>CFBundleIdentifier</key><string>org.neodax.tool</string>
  <key>CFBundleExecutable</key><string>neodax-editor</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
</dict>
</plist>
PLIST
# --- 解析プロジェクト (空の置き場) ---
# analysis/project は中身をコミットしない。ビルドでも作らない・消さない。
# 必要なときだけ手で `project --dir analysis/project ...` を実行する。
mkdir -p analysis/project
[ -f analysis/project/.gitkeep ] || : > analysis/project/.gitkeep
echo "kept: analysis/project (empty)"

echo "built: $APP (analysis dir: $APP/Contents/Resources/Analysis)"
