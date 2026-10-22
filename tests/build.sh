#!/usr/bin/env bash
# tests/build.sh — non-interactive build used by the test suite.
#
# Mirrors the flags from setup.sh (do_compile) but skips the interactive menu
# so CI / scripts can rebuild the analyzer in one shot.
#
# Usage: tests/build.sh [output_binary]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${ROOT}/neodax}"
OBJ_DIR="${ROOT}/.build/obj"

CC="${CC:-cc}"
case "$(uname -s)" in
    Darwin) PLATFORM_CFLAGS="-DBUILD_OS_BSD -DBUILD_OS_DARWIN"; PLATFORM_LDFLAGS="-lm" ;;
    Linux)  PLATFORM_CFLAGS="-DBUILD_OS_LINUX -D_GNU_SOURCE"; PLATFORM_LDFLAGS="-lm -ldl -rdynamic" ;;
    *)      PLATFORM_CFLAGS="-DBUILD_OS_UNIX"; PLATFORM_LDFLAGS="-lm" ;;
esac

CFLAGS="-O2 -Wall -Wextra \
    -Wno-unused-variable \
    -Wno-unused-parameter \
    -Wno-unused-function \
    -Wno-format-truncation \
    -I${ROOT}/include \
    -I${ROOT}/src \
    -std=c99 \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    ${PLATFORM_CFLAGS}"

SRCS="src/main.c src/loader.c src/disasm.c src/x86_decode.c \
      src/arm64_decode.c src/symbols.c src/demangle.c src/analysis.c \
      src/cfg.c src/daxc.c src/plugin.c src/interactive.c src/loops.c \
      src/callgraph.c src/correct.c src/riscv_decode.c src/unicode.c \
      src/sha256.c src/symexec.c src/decomp.c src/emulate.c \
      src/entropy.c src/dsa.c src/macho.c src/hardening.c src/config.c \
      src/report.c src/depflow.c src/static_analysis.c"

mkdir -p "${OBJ_DIR}"

OBJS=()
for src in $SRCS; do
    obj="${OBJ_DIR}/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')"
    # shellcheck disable=SC2086
    $CC $CFLAGS -c "${ROOT}/${src}" -o "$obj"
    OBJS+=("$obj")
done

# shellcheck disable=SC2086
$CC "${OBJS[@]}" -o "$OUT" $PLATFORM_LDFLAGS

echo "built: $OUT"
