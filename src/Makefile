# NeoDAX Static Analysis Framework Makefile
# Builds static Mach-O binary analysis with Shogi AI support

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -g
INCLUDES = -I. -I../include -I../gui
LIBS = -lpthread -lm -framework CoreFoundation -framework CFNetwork -framework Carbon -framework Security -framework CoreGraphics -framework ApplicationServices

# C++ の解析器 (逆track / 起動条件)。リンクは C++ ドライバで行う。
CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wno-deprecated-declarations

# Directories
SRC_DIR = .
BUILD_DIR = ../bin

# Source files
# Canonical Mach-O parser (dax_parse_macho) — composed by core/macho_static.c
CORE_SRCS = \
	macho.c \
	core/macho_static.c \
	integration/nx_decode.c


CORE_SRCS += integration/nx_update.c integration/nx_asar_patch.c
# 旧 neodax エンジンのデコーダ。src/ 直下が正準で、framework もこれを compose する。
LEGACY_SRCS = \
	arm64_decode.c \
	x86_decode.c \
	demangle.c

ANALYSIS_SRCS = \
	analysis/depflow.c \
	analysis/variable_classifier.c

FLOW_SRCS = \
	flow/execution_order.c \
	flow/parallel_detection.c

TRACKING_SRCS = \
	tracking/state_tracking.c

# Integration: nx autonomous dispatch core + bridge
INTEGRATION_SRCS = \
	integration/nx_engine.c \
	integration/nx_provider_macho.c \
	integration/nx_provider_dax.c \
	integration/nx_json.c \
	integration/nx_state.c \
	integration/nx_deepai.c \
	integration/attach/nx_inspect.c \
	integration/attach/nx_renderer.c \
	integration/attach/nx_payload.c \
	integration/attach/nx_appfix.c \
	integration/spec/nx_spec.c \
	integration/spec/nx_specs_builtin.c \
	integration/spec/nx_recog.c \
	integration/spec/nx_gen.c \
	integration/spec/nx_pe.c \
	integration/spec/nx_pe_disasm.c \
	integration/spec/nx_pipeline.c \
	integration/nx_sim.c \
	integration/neodax_bridge.c

#MAIN_SRCS = \
#main.c \
#main_integrated.c

TRACK_CPP_SRCS = \
	integration/nx_track.cpp \
	integration/nx_appfix_flow.cpp \
	integration/track/search.cpp \
	integration/track/advise.cpp \
	integration/track/cond.cpp \
	integration/track/learn.cpp \
	integration/track/widget.cpp \
	../gui/model/nx_boxlayout.cpp
TRACK_OBJS = $(patsubst %.cpp,%.o,$(TRACK_CPP_SRCS))

# --- GUI (bin/neodax-ui) が使う実装は「分類ごとの静的ライブラリ」で与える ---
# 分類: core / attach / spec / plan / track
# plan/ 配下は列挙しない (新しい部品を置くだけでアーカイブに入る)。
# リンカはアーカイブ単位で引き抜くので、どの分類を使うかは
# コードの include (src/integration/nx_all.h) で決まる。
LIB_DIR = ../src/lib
CAT_CORE = \
	$(LIB_DIR)/core_macho.o $(LIB_DIR)/core_macho_static.o $(LIB_DIR)/core_nx_decode.o \
	$(LIB_DIR)/core_nx_update.o $(LIB_DIR)/core_nx_asar_patch.o \
	$(LIB_DIR)/core_x86_decode.o $(LIB_DIR)/core_arm64_decode.o \
	$(LIB_DIR)/core_nx_json.o $(LIB_DIR)/core_nx_state.o \
	$(LIB_DIR)/core_nx_deepai.o $(LIB_DIR)/core_nx_sim.o \
# wildcard が空だと patsubst が空文字 1 個を返し、CAT_ATTACH に名前が残る。
# 空のときは明示的に空にする。
CAT_ATTACH = $(if $(wildcard integration/attach/*.c),$(patsubst %.c,$(LIB_DIR)/attach_%.o,$(notdir $(wildcard integration/attach/*.c))))
CAT_SPEC = $(patsubst %.c,$(LIB_DIR)/spec_%.o,$(notdir $(wildcard integration/spec/*.c)))
CAT_PLAN = $(patsubst %.cpp,$(LIB_DIR)/plan_%.o,$(notdir $(wildcard integration/plan/*.cpp)))
CAT_TRACK = $(LIB_DIR)/track_advise.o $(LIB_DIR)/track_search.o

# OS 固有の操作 (mac / win) は os/ 配下に分かれている。
# 分析 (integration/) はどちらの OS でも同じ。ここで選択するのは
# 「どの操作実装をリンクするか」だけ。
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
OS_DIR = os/mac
else
OS_DIR = os/win
endif
OS_OBJS = $(patsubst %.c,%.o,$(wildcard $(OS_DIR)/*.c)) \
          $(patsubst %.cpp,%.o,$(wildcard $(OS_DIR)/*.cpp))
# integrated は C ドライバでリンクするので、C++ の OS 実装は含めない。
OS_C_OBJS = $(patsubst %.c,%.o,$(wildcard $(OS_DIR)/*.c))
CAT_OS = $(patsubst %.c,$(LIB_DIR)/os_%.o,$(notdir $(wildcard $(OS_DIR)/*.c))) \
         $(patsubst %.cpp,$(LIB_DIR)/os_%.o,$(notdir $(wildcard $(OS_DIR)/*.cpp)))

CAT_OBJS = $(CAT_CORE) $(CAT_ATTACH) $(CAT_SPEC) $(CAT_PLAN) $(CAT_TRACK) $(CAT_OS)
LIB_ARCHIVES = $(LIB_DIR)/libnxc_core.a $(LIB_DIR)/libnxc_attach.a \
	$(LIB_DIR)/libnxc_spec.a $(LIB_DIR)/libnxc_plan.a $(LIB_DIR)/libnxc_track.a \
	$(LIB_DIR)/libnxc_os.a
# build.sh と同じフラグ (挙動を変えない)
CAT_CXXFLAGS = -std=c++17 -O2
# 分類の中身を「分類名なし」で include できるようにする (ヘッダを増やさない)。
CAT_INCLUDES = -I. -I../include -Iintegration -Iintegration/attach \
	-Iintegration/plan -I$(OS_DIR) -Ianalysis

# All source files
ALL_SRCS = $(CORE_SRCS) $(LEGACY_SRCS) $(ANALYSIS_SRCS) $(FLOW_SRCS) $(TRACKING_SRCS) $(INTEGRATION_SRCS) $(MAIN_SRCS)

# Object files
CORE_OBJS = $(patsubst %.c,%.o,$(CORE_SRCS))
LEGACY_OBJS = $(patsubst %.c,%.o,$(LEGACY_SRCS))
ANALYSIS_OBJS = $(patsubst %.c,%.o,$(ANALYSIS_SRCS))
FLOW_OBJS = $(patsubst %.c,%.o,$(FLOW_SRCS))
TRACKING_OBJS = $(patsubst %.c,%.o,$(TRACKING_SRCS))
INTEGRATION_OBJS = $(patsubst %.c,%.o,$(INTEGRATION_SRCS))
MAIN_OBJS =

ALL_OBJS = $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(INTEGRATION_OBJS) $(MAIN_OBJS)

# Targets
TARGET = $(BUILD_DIR)/neodax
TARGET_INTEGRATED = $(BUILD_DIR)/neodax-integrated

# Default target
#
# この Makefile は「ビルド定義の唯一の場所」です。
#   nx-categories : GUI (bin/neodax-ui) が使う分類ライブラリを生成
#                   (core / attach / spec / plan / track / os)
#   framework     : 単体 CLI (bin/neodax) と neodax-integrated
# 入口スクリプトはこれを呼ぶだけで、オブジェクトを列挙しない。
#   GUI : gui/build.sh  → make -C src nx-categories
#   CLI : setup.sh      → make -C src (framework)
all:
	@echo "  [i] src/Makefile: ビルド定義の唯一の場所です。"
	@echo "      GUI 部品: make nx-categories  (gui/build.sh が呼ぶ)"
	@echo "      CLI     : make framework     (setup.sh が呼ぶ)"
	@echo "      GUI 起動: bash gui/build.sh"
framework: $(TARGET) $(TARGET_INTEGRATED)

# 単体 CLI: make -C src neodax
neodax: $(TARGET)

# Main static analysis tool
$(TARGET): $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(TRACK_OBJS) $(OS_OBJS) main.o
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

# Integrated version with full pipeline
$(TARGET_INTEGRATED): $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(INTEGRATION_OBJS) $(OS_C_OBJS) main_integrated.o
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

# OS 固有の操作は分類フォルダを include パスに足してコンパイルする。
# (汎用の %.o より先に置く。make は先に一致したパターン規則を使う)
OS_INCLUDES = $(INCLUDES) -Iintegration/attach -Iintegration -Iintegration/plan
os/mac/%.o: os/mac/%.c
	$(CC) $(CFLAGS) $(OS_INCLUDES) -c $< -o $@
os/mac/%.o: os/mac/%.cpp
	$(CXX) $(CXXFLAGS) $(OS_INCLUDES) -c $< -o $@
os/win/%.o: os/win/%.c
	$(CC) $(CFLAGS) $(OS_INCLUDES) -c $< -o $@
os/win/%.o: os/win/%.cpp
	$(CXX) $(CXXFLAGS) $(OS_INCLUDES) -c $< -o $@

# Object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# UI 部品は build.sh と同じフラグで、分類別のライブラリへ入れる。
# (オブジェクトは lib/ にまとめ、どの中間ファイルもソース隣に散らかさない)
$(LIB_DIR):
	mkdir -p $@

$(LIB_DIR)/core_%.o: %.c | $(LIB_DIR)
	$(CC) $(CFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/core_macho_static.o: core/macho_static.c | $(LIB_DIR)
	$(CC) $(CFLAGS) $(CAT_INCLUDES) -c $< -o $@
	$(CC) $(CFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/core_nx_%.o: integration/nx_%.c | $(LIB_DIR)
	$(CC) $(CFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/attach_%.o: integration/attach/%.c | $(LIB_DIR)
	$(CC) $(CFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/spec_%.o: integration/spec/%.c | $(LIB_DIR)
	$(CC) $(CFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/plan_%.o: integration/plan/%.cpp | $(LIB_DIR)
	$(CXX) $(CAT_CXXFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/track_advise.o: integration/track/advise.cpp | $(LIB_DIR)
	$(CXX) $(CAT_CXXFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/track_search.o: integration/track/search.cpp | $(LIB_DIR)
	$(CXX) $(CAT_CXXFLAGS) $(CAT_INCLUDES) -c $< -o $@
# OS 固有の操作は選択されたフォルダ (os/mac または os/win) から取る。
$(LIB_DIR)/os_%.o: $(OS_DIR)/%.c | $(LIB_DIR)
	$(CC) $(CFLAGS) $(CAT_INCLUDES) -c $< -o $@
$(LIB_DIR)/os_%.o: $(OS_DIR)/%.cpp | $(LIB_DIR)
	$(CXX) $(CAT_CXXFLAGS) $(CAT_INCLUDES) -c $< -o $@

# 分類ごとの静的ライブラリ (リンク時の引き抜き単位)
$(LIB_DIR)/libnxc_core.a: $(CAT_CORE)
	rm -f $@ && ar rcs $@ $^
$(LIB_DIR)/libnxc_attach.a: $(CAT_ATTACH)
	rm -f $@ && ar rcs $@ $^
$(LIB_DIR)/libnxc_spec.a: $(CAT_SPEC)
	rm -f $@ && ar rcs $@ $^
$(LIB_DIR)/libnxc_plan.a: $(CAT_PLAN)
	rm -f $@ && ar rcs $@ $^
$(LIB_DIR)/libnxc_track.a: $(CAT_TRACK)
	rm -f $@ && ar rcs $@ $^
$(LIB_DIR)/libnxc_os.a: $(CAT_OS)
	rm -f $@ && ar rcs $@ $^

# GUI のリンクに必要なのはこの 1 つだけ。
nx-categories: $(LIB_ARCHIVES)

# Clean — in-tree オブジェクトも消す (以前は BUILD_DIR を見ていて実体を消せていなかった)
clean:
	rm -f $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(INTEGRATION_OBJS) $(TRACK_OBJS) main.o main_integrated.o
	rm -rf $(LIB_DIR)
	rm -rf $(BUILD_DIR)/neodax $(BUILD_DIR)/neodax-integrated

# Very clean
veryclean: clean
	rm -rf $(BUILD_DIR)

# Dependencies
dep:
	$(CC) -MM $(CFLAGS) $(INCLUDES) $(ALL_SRCS)

# Phony targets
.PHONY: all neodax framework clean veryclean dep nx-categories

# Build individual components
build_core: $(CORE_OBJS)

build_analysis: $(ANALYSIS_OBJS)

build_flow: $(FLOW_OBJS)

build_tracking: $(TRACKING_OBJS)

build_integration: $(INTEGRATION_OBJS)

# Test targets
test: $(TARGET)
	@echo "Building test..."
	# Add test commands here

# Install
targets: all

# Help
help:
	@echo "NeoDAX Static Analysis Framework Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make framework        - Build the static analysis framework (neodax_static, neodax_static_integrated)"
	@echo "  make all              - (no-op) CLI/GUI are built by setup.sh / gui/build.sh"
	@echo "  make neodax_static    - Build main static analysis tool"
	@echo "  make clean            - Clean build artifacts"
	@echo "  make veryclean        - Clean everything"
	@echo "  make dep              - Generate dependencies"
	@echo ""
	@echo "Usage:"
	@echo "  ./$(TARGET) --help     - Show usage"
	@echo "  ./$(TARGET) --shogi binary - Analyze Shogi AI binary"
