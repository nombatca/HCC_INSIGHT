import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile
import zlib
spec = importlib.util.spec_from_file_location('prep', Path(__file__).resolve().parents[2] / 'tools/prepare_restore_sample.py')
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)

class PrepareTests(unittest.TestCase):
    def test_exact_profile_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d); mount = base/'sd'; mount.mkdir(); archive = base/'source.zip'
            data = b'encrypted-fixture-bytes'
            with zipfile.ZipFile(archive,'w') as z:
                z.writestr('restoremdm/info.plist', data)
                z.writestr('restoremdm/restoremdm/info.plist', b'duplicate nested source is ignored')
                z.writestr('restoremdm/ab/'+'ab'+'1'*38, data)
            sha = hashlib.sha256(archive.read_bytes()).hexdigest()
            args = dict(expected_hash=sha, entries=[('info.plist',len(data),zlib.crc32(data)),('ab/'+'ab'+'1'*38,len(data),zlib.crc32(data))])
            with self.assertRaises(ValueError): prep.prepare(archive,mount)
            self.assertEqual(list(mount.iterdir()), [])
            target = prep.prepare(archive,mount,**args)
            self.assertEqual(set(p.name for p in target.iterdir()), {'info.plist','ab'})
            self.assertEqual((target/'ab'/('ab'+'1'*38)).read_bytes(),data)
            self.assertEqual((target/'info.plist').read_bytes(),data)
            with self.assertRaises(ValueError): prep.prepare(archive,mount,**args)
            self.assertEqual((target/'info.plist').read_bytes(),data)

    def test_mismatch_cleans_only_staging(self):
        with tempfile.TemporaryDirectory() as d:
            base=Path(d); mount=base/'sd';mount.mkdir();keep=mount/'keep';keep.write_text('unchanged');archive=base/'x.zip'
            with zipfile.ZipFile(archive,'w') as z:z.writestr('restoremdm/info.plist',b'x')
            with self.assertRaises(ValueError):prep.prepare(archive,mount,expected_hash=hashlib.sha256(archive.read_bytes()).hexdigest(),entries=[('info.plist',2,0)])
            self.assertEqual(keep.read_text(),'unchanged')
            self.assertEqual(list((mount/'hcc_restore').iterdir()),[])

    def test_production_profile(self):
        self.assertEqual(len(prep.profile()),297)
        self.assertEqual(sum(x[1] for x in prep.profile()),12299936)

if __name__=='__main__':unittest.main()
