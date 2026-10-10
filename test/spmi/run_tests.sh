#!/bin/bash
# Unit test SPMI MONITOR (decoder, bộ đệm, file capture, analyzer) trên máy tính - chỉ cần g++.
#   test/spmi/run_tests.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../components/apps/hcc_home"
BIN="${TMPDIR:-/tmp}/hcc_spmi_test"
g++ -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$SRC" \
    "$HERE/spmi_test.cpp" "$SRC/HCCSpmiDecode.cpp" "$SRC/HCCSpmiSynth.cpp" "$SRC/HCCSpmiBuffers.cpp" \
    "$SRC/HCCSpmiCapFile.cpp" "$SRC/HCCSpmiAnalyzer.cpp" -o "$BIN"
"$BIN"
