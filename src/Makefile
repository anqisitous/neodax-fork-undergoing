# NeoDAX Static Analysis Framework Makefile
# Builds static Mach-O binary analysis with Shogi AI support

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -g
INCLUDES = -I. -I../include
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
	integration/nx_leveldb.c \
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
	integration/track/search.cpp \
	integration/track/advise.cpp \
	integration/track/cond.cpp \
	integration/track/learn.cpp \
	integration/track/widget.cpp
TRACK_OBJS = $(patsubst %.cpp,%.o,$(TRACK_CPP_SRCS))

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
# NOTE: この Makefile は旧来のビルド経路です。CLI 本体 (neodax) は
#       リポジトリ直下の setup.sh が、GUI は gui/build.sh が担当します。
#       解析フレームワークを単体ビルドしたい場合のみ `make framework` を使ってください。
all:
	@echo "  [i] src/Makefile: 旧ビルド経路のため 'all' では何もビルドしません。"
	@echo "      CLI  : make        (リポジトリ直下 → setup.sh)"
	@echo "      GUI  : bash gui/build.sh"
	@echo "      FW   : make -C src neodax"

framework: $(TARGET) $(TARGET_INTEGRATED)

# 単体 CLI: make -C src neodax
neodax: $(TARGET)

# Main static analysis tool
$(TARGET): $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(TRACK_OBJS) main.o
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

# Integrated version with full pipeline
$(TARGET_INTEGRATED): $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(INTEGRATION_OBJS) main_integrated.o
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

# Object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Clean — in-tree オブジェクトも消す (以前は BUILD_DIR を見ていて実体を消せていなかった)
clean:
	rm -f $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(INTEGRATION_OBJS) $(TRACK_OBJS) main.o main_integrated.o
	rm -rf $(BUILD_DIR)/neodax $(BUILD_DIR)/neodax-integrated

# Very clean
veryclean: clean
	rm -rf $(BUILD_DIR)

# Dependencies
dep:
	$(CC) -MM $(CFLAGS) $(INCLUDES) $(ALL_SRCS)

# Phony targets
.PHONY: all neodax framework clean veryclean dep

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
