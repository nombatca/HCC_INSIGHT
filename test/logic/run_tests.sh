#!/bin/bash
# Unit test LOGIC ANALYZER (trigger, tóm tắt sóng, đo, giải mã I2C / UART / SPI / SPMI, file .lab / .vcd).
#   test/logic/run_tests.sh      - chỉ cần g++
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../components/apps/hcc_home"
BIN="${TMPDIR:-/tmp}/hcc_logic_test"
g++ -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$SRC" \
    "$HERE/logic_test.cpp" "$SRC/HCCLogicCore.cpp" "$SRC/HCCLogicDecode.cpp" "$SRC/HCCLogicFile.cpp" \
    "$SRC/HCCSpmiDecode.cpp" "$SRC/HCCSpmiSynth.cpp" -o "$BIN"
"$BIN"
