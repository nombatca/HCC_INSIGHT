#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp tests/test_main.cpp -o build/hcc_backup_tests
./build/hcc_backup_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    tests/test_protocol.cpp -o build/hcc_protocol_tests
./build/hcc_protocol_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    examples/protocol_demo.cpp -o build/hcc_protocol_demo
./build/hcc_protocol_demo
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    ../../components/hcc_backup/src/hcc_backup_link.cpp tests/test_link.cpp -o build/hcc_link_tests
./build/hcc_link_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    ../../components/hcc_backup/src/hcc_backup_link.cpp examples/link_demo.cpp -o build/hcc_link_demo
./build/hcc_link_demo
