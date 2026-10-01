#!/bin/bash
# Test host cho Panic Parser v2. Cần g++ và cJSON của ESP-IDF ($IDF_PATH).
#   test/panic/run_tests.sh                 chạy samples/ (+ samples/real/ nếu có)
#   test/panic/run_tests.sh --dump file.ips in fact của một file .ips
#   test/panic/run_tests.sh --kb-dump f.ips in kết quả đối chiếu KB V1 của một file .ips
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/components/apps/hcc_home"
CJSON="${CJSON_DIR:-${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON}"
if [ ! -f "$CJSON/cJSON.c" ]; then
    echo "Không thấy cJSON.c: đặt IDF_PATH (vd ~/esp/esp-idf) hoặc CJSON_DIR" >&2
    exit 2
fi
BIN="${TMPDIR:-/tmp}/hcc_panic_facts_test"
g++ -std=gnu++17 -O1 -Wall -Wextra -I"$HERE/stub" -I"$CJSON" -I"$SRC" \
    "$HERE/panic_facts_test.cpp" "$SRC/HCCPanicParser.cpp" "$SRC/HCCPanicFacts.cpp" "$CJSON/cJSON.c" \
    -o "$BIN"
if [ "$1" = "--dump" ]; then
    exec "$BIN" --dump "$2"
fi
KBBIN="${TMPDIR:-/tmp}/hcc_panic_kb_test"
g++ -std=gnu++17 -O1 -Wall -Wextra -I"$HERE/stub" -I"$CJSON" -I"$SRC" \
    "$HERE/kb_test.cpp" "$SRC/HCCPanicKBv1.cpp" "$SRC/HCCPanicKBView.cpp" "$SRC/HCCPanicParser.cpp" "$SRC/HCCPanicFacts.cpp" "$CJSON/cJSON.c" \
    -o "$KBBIN"
if [ "$1" = "--kb-dump" ]; then
    exec "$KBBIN" "$HERE" --dump "$2"
fi
DIRS=("$HERE/samples")
[ -d "$HERE/samples/real" ] && DIRS+=("$HERE/samples/real")
echo "== Parser v2"
"$BIN" "${DIRS[@]}"
echo "== Knowledge Base V1"
exec "$KBBIN" "$HERE"
