#!/usr/bin/env python3
"""Copy the exact user-supplied sample to SD. Never contacts an iOS device.

No backup payload, decrypted data or keys are distributed with this tool.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import zipfile
import zlib

ARCHIVE_SHA256 = '0d1dedeeedebb9ff477a83dfdcf0ed17c5b64cf3951d1ea9523a83e57ac480c9'


def profile():
    header = Path(__file__).resolve().parents[1] / 'components/hcc_backup/include/hcc_restore_sample_index.hpp'
    rows = re.findall(r'\{"([^"]+)",(\d+)u,0x([0-9a-f]+)u\}', header.read_text())
    if len(rows) != 297:
        raise ValueError('Invalid firmware sample index')
    return [(n, int(size), int(crc, 16)) for n, size, crc in rows]


def trusted_directory(path):
    path = Path(os.path.abspath(path))
    if not path.is_dir() or any(p.is_symlink() for p in [path, *path.parents]):
        raise ValueError('Destination must be an existing directory without symlink ancestors')
    return path


def prepare(archive, mount, *, expected_hash=ARCHIVE_SHA256, entries=None):
    archive = Path(archive)
    with archive.open('rb') as f:
        actual = hashlib.file_digest(f, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(f.read()).hexdigest()
    if actual != expected_hash:
        raise ValueError('Wrong archive SHA-256; no SD files written')
    entries = profile() if entries is None else entries
    mount = trusted_directory(mount)
    parent = mount / 'hcc_restore'
    if parent.is_symlink() or (parent.exists() and not parent.is_dir()):
        raise ValueError('Invalid hcc_restore directory')
    parent.mkdir(exist_ok=True)
    target = parent / 'restoremdm'
    if target.exists() or target.is_symlink():
        raise ValueError('hcc_restore/restoremdm already exists; existing data was not changed')
    staging = Path(tempfile.mkdtemp(prefix='.restore-preparing-', dir=parent))
    try:
        with zipfile.ZipFile(archive) as z:
            names = z.namelist()
            for name, size, crc in entries:
                if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')):
                    raise ValueError('Invalid sample basename')
                entry = 'restoremdm/' + name
                if names.count(entry) != 1:
                    raise ValueError('Missing/duplicate sample file: ' + name)
                info = z.getinfo(entry)
                if info.file_size != size or info.CRC != crc:
                    raise ValueError('Sample metadata mismatch: ' + name)
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError('Symlink entry refused')
                (staging / name).parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, (staging / name).open('xb') as dst:
                    total = checksum = 0
                    while True:
                        block = src.read(32768)
                        if not block:
                            break
                        dst.write(block)
                        total += len(block)
                        checksum = zlib.crc32(block, checksum)
                    dst.flush()
                    os.fsync(dst.fileno())
                if total != size or checksum != crc:
                    raise ValueError('Sample data mismatch: ' + name)
        if target.exists() or target.is_symlink():
            raise ValueError('Destination appeared during preparation; refusing overwrite')
        staging.rename(target)
    except BaseException:
        shutil.rmtree(staging)
        raise
    return target


def main():
    if len(sys.argv) != 3:
        print('Usage: python3 tools/prepare_restore_sample.py /path/restoremdm.zip /Volumes/SD_CARD', file=sys.stderr)
        return 2
    try:
        target = prepare(sys.argv[1], sys.argv[2])
    except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as error:
        print('STOP:', error, file=sys.stderr)
        return 1
    print('Prepared 297 unchanged sample files:', target)
    print('Eject the card safely. No Restore command has been sent to any device.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
