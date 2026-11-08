#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fno-exceptions -fno-rtti ${HCC_TEST_FLAGS:-} \
  -Istubs -I../sd_access/stubs -I../../components/apps -I../../components/hcc_backup/include -I../../components/apps/hcc_home \
  ../../components/hcc_backup/src/*.cpp \
  ../../components/apps/hcc_home/HCCBackupCapture.cpp \
  ../../components/apps/hcc_home/HCCBackupOperation.cpp \
  test_mdm_probe.cpp -o build/mdm_probe_tests
./build/mdm_probe_tests
