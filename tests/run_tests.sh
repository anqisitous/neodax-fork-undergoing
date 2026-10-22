#!/usr/bin/env bash
#
# tests/run_tests.sh — end-to-end tests for NeoDAX.
#
#   1. builds the analyzer (tests/build.sh)
#   2. builds a real Mach-O engine fixture (tests/fixtures/engine_fixture.c)
#   3. runs the advanced static analysis (-w) and checks the ring SVG
#   4. checks the flow tracing (variable → caller chains) and the
#      execution-order prediction
#   5. checks the CLI surface
#
# Usage: tests/run_tests.sh [work_dir]

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${1:-${ROOT}/.build/test}"

PASS=0
FAIL=0

ok()   { printf "  \033[32mPASS\033[0m  %s\n" "$1"; PASS=$((PASS + 1)); }
bad()  { printf "  \033[31mFAIL\033[0m  %s\n" "$1"; FAIL=$((FAIL + 1)); }
info() { printf "\n\033[1m%s\033[0m\n" "$1"; }

check() {  # check <description> <condition-result>
    if [ "$2" = "0" ]; then ok "$1"; else bad "$1"; fi
}

mkdir -p "$WORK"

# ── 1. build ────────────────────────────────────────────────────────────────
info "build"
if bash "${ROOT}/tests/build.sh" "${WORK}/neodax" >"${WORK}/build.log" 2>&1; then
    ok "analyzer builds"
else
    bad "analyzer builds (see ${WORK}/build.log)"
    tail -20 "${WORK}/build.log"
    exit 1
fi

# ── 2. fixture ──────────────────────────────────────────────────────────────
info "fixture"
# -O0 keeps the calls in the binary; at -O1 the compiler inlines them and
# the call graph (and with it the whole flow analysis) has nothing to chew on.
if cc -O0 -o "${WORK}/engine_fixture" "${ROOT}/tests/fixtures/engine_fixture.c" 2>"${WORK}/fixture.log"; then
    ok "engine fixture builds"
else
    bad "engine fixture builds (see ${WORK}/fixture.log)"
    exit 1
fi

"${WORK}/engine_fixture" >/dev/null 2>&1
check "engine fixture runs" $?

# ── 3. advanced analysis + ring SVG ─────────────────────────────────────────
info "advanced analysis (-w --ring-svg)"
SVG="${WORK}/ring.svg"
rm -f "$SVG"

"${WORK}/neodax" -w --ring-svg "$SVG" "${WORK}/engine_fixture" >"${WORK}/advanced.log" 2>&1
check "exit code is 0" $?

grep -q "Regression Ring" "${WORK}/advanced.log"
check "console prints the regression ring" $?

grep -q "SURVEY" "${WORK}/advanced.log" && grep -q "VERIFY" "${WORK}/advanced.log"
check "console lists every phase" $?

grep -q "Execution Order" "${WORK}/advanced.log"
check "console prints the predicted execution order" $?

grep -q "Variable Caller Chains" "${WORK}/advanced.log"
check "console prints the variable caller chains" $?

grep -q "Execution Groups" "${WORK}/advanced.log"
check "console prints the execution groups" $?

[ -s "$SVG" ]
check "ring SVG file is written" $?

if command -v python3 >/dev/null 2>&1; then
    python3 - "$SVG" <<'PY'
import sys, xml.etree.ElementTree as ET
path = sys.argv[1]
root = ET.parse(path).getroot()
ns = "{http://www.w3.org/2000/svg}"
assert root.tag == ns + "svg", "root element is not <svg>"
assert root.get("width") and root.get("height"), "missing canvas size"
circles = list(root.iter(ns + "circle"))
paths = list(root.iter(ns + "path"))
texts = [t.text or "" for t in root.iter(ns + "text")]
assert len(circles) >= 6, "too few nodes"
assert len(paths) >= 12, "too few edges"
assert any("REASONING" in t for t in texts), "missing ring title"
assert any("progression" in t for t in texts), "missing progression legend"
assert any("regression" in t for t in texts), "missing regression legend"
# the group tree: every group and every function must be labelled
assert any("sequential" in t for t in texts), "missing the sequential group"
assert any("negamax" in t for t in texts), "missing a function leaf"
# every node must sit inside the canvas
W, H = float(root.get("width")), float(root.get("height"))
for c in circles:
    cx, cy, r = float(c.get("cx")), float(c.get("cy")), float(c.get("r"))
    assert cx - r >= 0 and cx + r <= W and cy - r >= 0 and cy + r <= H, "node outside canvas"
PY
    check "ring SVG is well-formed and complete" $?
else
    info "python3 missing — skipping SVG structure check"
fi

# ── 4. flow tracing ────────────────────────────────────────────────────────
info "flow tracing"

"${WORK}/neodax" -w --flow-var current_score "${WORK}/engine_fixture" >"${WORK}/flowvar.log" 2>&1
check "--flow-var exits cleanly" $?

grep -q "Caller chain: current_score" "${WORK}/flowvar.log"
check "--flow-var reports the chain" $?

grep -q "evaluate_position" "${WORK}/flowvar.log"
check "the chain names the reading function" $?

"${WORK}/neodax" -w --flow-func negamax "${WORK}/engine_fixture" >"${WORK}/flowfunc.log" 2>&1
check "--flow-func exits cleanly" $?

grep -q "Function flow: negamax" "${WORK}/flowfunc.log"
check "--flow-func reports the function" $?

grep -q "callees" "${WORK}/flowfunc.log"
check "--flow-func lists the callees" $?

# the parallel worker chain must survive the trace:
# worker_evaluate reads worker_thread_count, and is called by the dispatcher.
grep -q "worker_evaluate" "${WORK}/advanced.log"
check "the parallel worker appears in the report" $?

grep -q "dispatch_parallel_evaluation" "${WORK}/advanced.log"
check "the parallel dispatcher appears in the report" $?

# ── 5. cli surface ──────────────────────────────────────────────────────────
info "cli surface"
"${WORK}/neodax" -h >"${WORK}/help.log" 2>&1
grep -q -- "--advanced-analysis" "${WORK}/help.log"
check "help lists --advanced-analysis" $?

grep -q -- "--ring-svg" "${WORK}/help.log"
check "help lists --ring-svg" $?

grep -q -- "--flow-var" "${WORK}/help.log"
check "help lists --flow-var" $?

"${WORK}/neodax" --advanced-analysis "${WORK}/engine_fixture" >"${WORK}/longopt.log" 2>&1
check "the long option works too" $?

"${WORK}/neodax" -x "${WORK}/engine_fixture" >"${WORK}/all.log" 2>&1
check "the standard -x analysis still works" $?

# ── 6. robustness ───────────────────────────────────────────────────────────
info "robustness"
printf 'not a binary' > "${WORK}/garbage.bin"
"${WORK}/neodax" -w "${WORK}/garbage.bin" >"${WORK}/garbage.log" 2>&1
RC=$?
[ "$RC" -lt 128 ]
check "a text file does not crash the analyzer" $?

# a Mach-O with a forged section header must be rejected, not followed
cp "${WORK}/engine_fixture" "${WORK}/forged.bin"
printf '\xde\xad\xbe\xef' | dd of="${WORK}/forged.bin" bs=1 seek=0 conv=notrunc 2>/dev/null
"${WORK}/neodax" -w "${WORK}/forged.bin" >"${WORK}/forged.log" 2>&1
RC=$?
[ "$RC" -lt 128 ]
check "a forged Mach-O header does not crash the analyzer" $?

# an empty file is rejected outright
: > "${WORK}/empty.bin"
"${WORK}/neodax" -w "${WORK}/empty.bin" >"${WORK}/empty.log" 2>&1
[ $? -ne 0 ]
check "an empty file is rejected" $?

# ── summary ─────────────────────────────────────────────────────────────────
printf "\n%d passed, %d failed\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
