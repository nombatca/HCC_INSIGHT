import sys
from pathlib import Path
import tempfile
import unittest
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import prepare_restore_sample17 as prep

class Prepare17Tests(unittest.TestCase):
    def test_profile(self):
        self.assertEqual(len(prep.profile()),857)
        self.assertEqual(sum(x[1] for x in prep.profile()),21074143)

    def test_copy_crc_no_overwrite_and_cleanup(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);src=root/'source';sd=root/'sd';src.mkdir();sd.mkdir()
            data=b'encrypted-fixture';(src/'Info.plist').write_bytes(data)
            entries=[('Info.plist',len(data),zlib.crc32(data))]
            with self.assertRaises(ValueError):prep.prepare(src,sd,[('Info.plist',len(data),0)])
            self.assertEqual(list((sd/'hcc_restore').iterdir()),[])
            target=prep.prepare(src,sd,entries)
            self.assertEqual((target/'Info.plist').read_bytes(),data)
            with self.assertRaises(ValueError):prep.prepare(src,sd,entries)
            self.assertEqual((target/'Info.plist').read_bytes(),data)

if __name__=='__main__':unittest.main()
