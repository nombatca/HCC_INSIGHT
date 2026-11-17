#!/usr/bin/env python3
"""Verify the user-extracted restoremdm17 folder and stage it on SD; no device I/O."""
from pathlib import Path
import os
import re
import shutil
import sys
import tempfile
import zlib
from prepare_restore_sample import trusted_directory


def profile():
    header = Path(__file__).resolve().parents[1] / 'components/hcc_backup/include/hcc_restore_sample17_index.hpp'
    rows = re.findall(r'\{"([^"]+)",(\d+)u,0x([0-9a-f]+)u\}', header.read_text())
    if len(rows) != 857:
        raise ValueError('Invalid sample17 index')
    return [(n, int(s), int(c, 16)) for n, s, c in rows]


def prepare(source, mount, entries=None):
    source = trusted_directory(source)
    mount = trusted_directory(mount)
    entries = profile() if entries is None else entries
    parent = mount / 'hcc_restore'
    if parent.is_symlink() or (parent.exists() and not parent.is_dir()):
        raise ValueError('Invalid hcc_restore directory')
    parent.mkdir(exist_ok=True)
    target = parent / 'restoremdm17'
    if target.exists() or target.is_symlink():
        raise ValueError('restoremdm17 already exists; no overwrite')
    staging = Path(tempfile.mkdtemp(prefix='.restore17-preparing-', dir=parent))
    try:
        for name, size, crc in entries:
            if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')):
                raise ValueError('Invalid indexed path')
            src = source / name
            trusted_directory(src.parent)
            if src.is_symlink() or not src.is_file() or src.stat().st_size != size:
                raise ValueError('Missing/wrong sample17 file: ' + name)
            dst = staging / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            total = checksum = 0
            with src.open('rb') as inp, dst.open('xb') as out:
                while True:
                    block = inp.read(32768)
                    if not block:
                        break
                    out.write(block)
                    total += len(block)
                    checksum = zlib.crc32(block, checksum)
                out.flush()
                os.fsync(out.fileno())
            if total != size or checksum != crc:
                raise ValueError('Sample17 CRC mismatch: ' + name)
        if target.exists() or target.is_symlink():
            raise ValueError('Destination appeared; no overwrite')
        staging.rename(target)
    except BaseException:
        shutil.rmtree(staging)
        raise
    return target


def main():
    if len(sys.argv) != 3:
        print('Usage: python3 tools/prepare_restore_sample17.py /path/extracted/restoremdm17 /Volumes/SD_CARD')
        return 2
    try:
        print('Prepared 857 unchanged encrypted files:', prepare(sys.argv[1], sys.argv[2]))
    except (OSError, ValueError) as e:
        print('STOP:', e, file=sys.stderr)
        return 1
    print('No device contacted. This sample is experimental; iOS 27 compatibility is unverified.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
