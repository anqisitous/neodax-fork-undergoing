# NeoDAX Static Analysis Framework Makefile
# Builds static Mach-O binary analysis with Shogi AI support

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -g
INCLUDES = -I. -I../include
LIBS = -lpthread -lm

# Directories
SRC_DIR = .
BUILD_DIR = ../build_neodax

# Source files
# Canonical Mach-O parser (dax_parse_macho) — composed by core/macho_static.c
CORE_SRCS = \
	macho.c \
	core/macho_static.c \
	integration/nx_decode.c

# 旧 neodax エンジンのデコーダ。src/ 直下が正準で、framework もこれを compose する。
LEGACY_SRCS = \
	arm64_decode.c \
	x86_decode.c

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
	integration/neodax_bridge.c

#MAIN_SRCS = \
#main.c \
#main_integrated.c

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
TARGET = $(BUILD_DIR)/neodax_static
TARGET_INTEGRATED = $(BUILD_DIR)/neodax_static_integrated

# Default target
all: $(TARGET) $(TARGET_INTEGRATED)

# Main static analysis tool
$(TARGET): $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) main.o
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

# Integrated version with full pipeline
$(TARGET_INTEGRATED): $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(INTEGRATION_OBJS) main_integrated.o
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

# Object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean — in-tree オブジェクトも消す (以前は BUILD_DIR を見ていて実体を消せていなかった)
clean:
	rm -f $(CORE_OBJS) $(LEGACY_OBJS) $(ANALYSIS_OBJS) $(FLOW_OBJS) $(TRACKING_OBJS) $(INTEGRATION_OBJS) main.o main_integrated.o
	rm -rf $(BUILD_DIR)/neodax_static $(BUILD_DIR)/neodax_static_integrated

# Very clean
veryclean: clean
	rm -rf $(BUILD_DIR)

# Dependencies
dep:
	$(CC) -MM $(CFLAGS) $(INCLUDES) $(ALL_SRCS)

# Phony targets
.PHONY: all clean veryclean dep

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
	@echo "  make all              - Build all targets"
	@echo "  make neodax_static    - Build main static analysis tool"
	@echo "  make clean            - Clean build artifacts"
	@echo "  make veryclean        - Clean everything"
	@echo "  make dep              - Generate dependencies"
	@echo ""
	@echo "Usage:"
	@echo "  ./$(TARGET) --help     - Show usage"
	@echo "  ./$(TARGET) --shogi binary - Analyze Shogi AI binary"
