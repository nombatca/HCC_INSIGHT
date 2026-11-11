#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp tests/test_main.cpp -o build/hcc_backup_tests
./build/hcc_backup_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    tests/test_protocol.cpp -o build/hcc_protocol_tests
./build/hcc_protocol_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    examples/protocol_demo.cpp -o build/hcc_protocol_demo
./build/hcc_protocol_demo
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    ../../components/hcc_backup/src/hcc_backup_link.cpp tests/test_link.cpp -o build/hcc_link_tests
./build/hcc_link_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    ../../components/hcc_backup/src/hcc_backup_link.cpp examples/link_demo.cpp -o build/hcc_link_demo
./build/hcc_link_demo
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/hcc_backup_stream.cpp ../../components/hcc_backup/src/hcc_plist.cpp ../../components/hcc_backup/src/hcc_backup_session.cpp \
    ../../components/hcc_backup/src/hcc_backup_link.cpp tests/test_status.cpp -o build/hcc_status_tests
./build/hcc_status_tests
python3 - <<'PY'
import plistlib
with open('build/status.bplist', 'rb') as stream:
    assert plistlib.load(stream) == ['DLMessageStatusResponse', -1, '___EmptyParameterString___', {}]
with open('build/free_space.bplist', 'rb') as stream:
    assert plistlib.load(stream) == ['DLMessageStatusResponse', 0, '___EmptyParameterString___', 1 << 40]
print('Independent plistlib status and capacity decoding PASS')
PY
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/*.cpp tests/test_batch.cpp -o build/hcc_batch_tests
./build/hcc_batch_tests
python3 - <<'PYCODE'
import plistlib
for name, path in [('download_missing.bplist', 'Status.plist'), ('download_unicode.bplist', 'backup/Thiết bị 😀')]:
    with open('build/' + name, 'rb') as stream:
        assert plistlib.load(stream) == ['DLMessageStatusResponse', -13, 'Multi status', {path: {'DLFileErrorString': 'No such file or directory', 'DLFileErrorCode': -6}}]
print('Independent plistlib DownloadFiles multi-status decoding PASS')
PYCODE
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/*.cpp tests/test_receive.cpp -o build/hcc_receive_tests
./build/hcc_receive_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/*.cpp tests/test_storage.cpp -o build/hcc_storage_tests
./build/hcc_storage_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/*.cpp tests/test_listing.cpp -o build/hcc_listing_tests
./build/hcc_listing_tests
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
    -I../../components/hcc_backup/include ../../components/hcc_backup/src/*.cpp tests/test_backup_start.cpp -o build/hcc_backup_start_tests
./build/hcc_backup_start_tests
python3 - <<'PY'
import plistlib
for full in (False, True):
    name = 'build/backup_full.bplist' if full else 'build/backup_default.bplist'
    with open(name, 'rb') as stream:
        value = plistlib.load(stream)
    expected = {'MessageName': 'Backup', 'TargetIdentifier': '00008101-001234567890ABCD', 'SourceIdentifier': '00008101-001234567890ABCD'}
    if full:
        expected['Options'] = {'ForceFullBackup': True}
    assert value == ['DLMessageProcessMessage', expected]
print('Independent plistlib Backup request decoding PASS')
PY

python3 - <<'PYLIST'
import plistlib
with open('build/contents_1024.bplist','rb') as stream:
    result=plistlib.load(stream)
assert result[:3]==['DLMessageStatusResponse',0,'___EmptyParameterString___']
assert len(result[3])==1024
for i in (0,1023):
    assert result[3][f'{i:04d}'+('x'*249)+'é']['DLFileSize']==i
print('Independent plistlib 1024-entry Unicode directory reply decoding PASS')
PYLIST
