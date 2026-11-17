#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti ${HCC_TEST_FLAGS:-} \
  -I../sd_access/stubs -I../../components/apps/hcc_home -I../../components/hcc_backup/include \
  ../../components/apps/hcc_home/HCCSdFileSystem.cpp ../../components/apps/hcc_home/HCCSdFiles.cpp \
  test_sd_files.cpp -o build/sd_files_tests
./build/sd_files_tests

"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti ${HCC_TEST_FLAGS:-} \
  -DCONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL=1 \
  -I../sd_access/stubs -I../../components/apps/hcc_home -I../../components/hcc_backup/include \
  ../../components/apps/hcc_home/HCCSdFileSystem.cpp ../../components/apps/hcc_home/HCCSdFiles.cpp \
  test_noformat.cpp -o build/noformat_tests
./build/noformat_tests
