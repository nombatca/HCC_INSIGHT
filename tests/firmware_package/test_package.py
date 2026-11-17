import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

MODULE = Path(__file__).resolve().parents[2] / "tools/package_firmware.py"
spec = importlib.util.spec_from_file_location("firmware_package", MODULE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FirmwarePackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.output = self.root / "firmware.zip"

    def manifest(self, files):
        content = {"flash_files": files, "flash_settings": {"flash_size": "32MB"}}
        (self.build / "flasher_args.json").write_text(json.dumps(content))

    def test_images_manifest_paths_and_checksums(self):
        self.manifest({"0x2000": "bootloader/bootloader.bin", "0x20000": "app.bin"})
        (self.build / "bootloader").mkdir()
        (self.build / "bootloader/bootloader.bin").write_bytes(b"test bootloader")
        (self.build / "app.bin").write_bytes(b"test app")
        (self.build / "unrelated.bin").write_bytes(b"not a flash image")
        (self.build / "flash_args").write_text("0x2000 bootloader/bootloader.bin 0x20000 app.bin")
        module.package(self.build, self.output)
        with zipfile.ZipFile(self.output) as archive:
            self.assertIsNone(archive.testzip())
            self.assertNotIn("unrelated.bin", archive.namelist())
            self.assertEqual(archive.read("app.bin"), b"test app")
            self.assertEqual(json.loads(archive.read("flasher_args.json"))["flash_files"],
                             {"0x2000": "bootloader/bootloader.bin", "0x20000": "app.bin"})
            for line in archive.read("SHA256SUMS").decode().splitlines():
                digest, name = line.split("  ", 1)
                self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)

    def test_missing_image_preserves_previous_bundle(self):
        self.manifest({"0x20000": "missing.bin"})
        self.output.write_bytes(b"previous bundle")
        with self.assertRaises(FileNotFoundError):
            module.package(self.build, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous bundle")

    def test_rejects_escaping_paths_and_bad_manifest(self):
        (self.root / "outside.bin").write_bytes(b"outside")
        (self.build / "symlink.bin").symlink_to(self.root / "outside.bin")
        for files in ({"0x0": "../outside.bin"}, {"0x0": "symlink.bin"},
                      {"0x0": str(self.root / "outside.bin")}, {}, {"-1": "app.bin"}):
            with self.subTest(files=files):
                self.manifest(files)
                with self.assertRaises(ValueError):
                    module.package(self.build, self.output)
                self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
