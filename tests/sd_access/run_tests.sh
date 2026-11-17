#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -pthread ${HCC_TEST_FLAGS:-} \
    -Istubs -I../../components/apps -I../../components/apps/hcc_home -I../../components/hcc_backup/include \
    ../../components/hcc_backup/src/*.cpp ../../components/apps/hcc_home/HCCBackupCapture.cpp ../../components/apps/hcc_home/HCCBackupOperation.cpp ../../components/apps/hcc_home/HCCBackupSdSelfTest.cpp test_sd_access.cpp -o build/sd_access_tests
./build/sd_access_tests
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti ${HCC_TEST_FLAGS:-} \
    -DCONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL=1 \
    -Istubs -I../../components/apps/hcc_home -I../../components/hcc_backup/include \
    ../../components/hcc_backup/src/*.cpp ../../components/apps/hcc_home/HCCBackupCapture.cpp test_noformat.cpp -o build/noformat_tests
./build/noformat_tests
