#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fno-exceptions -fno-rtti ${HCC_TEST_FLAGS:-} \
  -Istubs -I../../components/hcc_backup/include -I../../components/apps/hcc_home \
  ../../components/hcc_backup/src/hcc_backup_stream.cpp \
  ../../components/hcc_backup/src/hcc_plist.cpp \
  ../../components/hcc_backup/src/hcc_backup_session.cpp \
  ../../components/hcc_backup/src/hcc_backup_link.cpp \
  test_mdm_probe.cpp -o build/mdm_probe_tests
./build/mdm_probe_tests
