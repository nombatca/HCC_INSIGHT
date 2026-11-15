#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror ${HCC_TEST_FLAGS:-} \
 -I../../components/hcc_backup/include ../../components/hcc_backup/src/*.cpp test_restore.cpp -o build/restore_tests
./build/restore_tests
python3 - <<'PY'
import plistlib
with open('build/request.bplist','rb') as f: p=plistlib.load(f)
assert p[0]=='DLMessageProcessMessage'
r=p[1]
assert r['MessageName']=='Restore'
assert r['SourceIdentifier']==r['TargetIdentifier']=='00008101-001234567890ABCD'
assert r['Options']==dict(Password='a',RestoreDontCopyBackup=True,RemoveItemsNotRestored=False,RestoreSystemFiles=False,RestorePreserveSettings=False,RestorePreserveCameraRoll=False)
print('PASS: independent plistlib verifies exact experimental request')
PY

python3 -m unittest discover -s . -p 'test_prepare.py' -v
