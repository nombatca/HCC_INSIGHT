#!/bin/sh
# Kiểm thử bộ giải mã MIPI RFFE trên host (không cần ESP-IDF / phần cứng).
# HCC_TEST_FLAGS="-fsanitize=address,undefined" để chạy kèm sanitizer.
set -eu
cd "$(dirname "$0")"
mkdir -p build
SRC=../../components/apps/hcc_home
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I"$SRC" \
    "$SRC/HCCSpmiDecode.cpp" "$SRC/HCCRffeDecode.cpp" tests/test_decode.cpp \
    -o build/rffe_decode_tests
./build/rffe_decode_tests
