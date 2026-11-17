"""Bundle only IDF-declared flash images, preserving their relative paths."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import zipfile


def package(build_dir: Path, destination: Path) -> None:
    build_dir = build_dir.resolve()
    manifest_path = build_dir / "flasher_args.json"
    manifest = json.loads(manifest_path.read_text())
    flash_files = manifest.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError("flasher_args.json has no flash_files")

    images = {}
    for offset, name in flash_files.items():
        # Offsets and paths come from IDF, never infer them from the board name.
        if not isinstance(offset, str) or int(offset, 0) < 0:
            raise ValueError("Invalid flash offset")
        if not isinstance(name, str) or not name or "\\" in name:
            raise ValueError("Invalid image path")
        relative = Path(name)
        source = (build_dir / relative).resolve()
        if relative.is_absolute() or ".." in relative.parts or not source.is_relative_to(build_dir):
            raise ValueError("Flash image is outside build directory")
        if not source.is_file():
            raise FileNotFoundError(source)
        archive_name = relative.as_posix()
        if archive_name in ("flasher_args.json", "flash_args", "SHA256SUMS", "README_VI.txt"):
            raise ValueError("Image path collides with bundle metadata")
        images[archive_name] = source.read_bytes()

    metadata = {"flasher_args.json": manifest_path.read_bytes()}
    if (build_dir / "flash_args").is_file():
        metadata["flash_args"] = (build_dir / "flash_args").read_bytes()
    entries = {**images, **metadata}
    entries["README_VI.txt"] = (
        "HCC INSIGHT — firmware ESP32-P4\n\n"
        "Gói chứa toàn bộ ảnh flash được ESP-IDF khai báo trong flasher_args.json.\n"
        "Giữ cấu trúc thư mục khi giải nén. Dùng đúng offset và flash_settings\n"
        "trong manifest; không nạp riêng app .bin vào địa chỉ 0.\n"
        "Nếu dùng flash_args, chạy công cụ nạp từ thư mục đã giải nén.\n\n"
        "Build thành công không thay thế thử nghiệm trên board.\n"
        "Nút MDM hiện chỉ kiểm tra kết nối/thương lượng dịch vụ, chưa gỡ MDM.\n"
    ).encode("utf-8")
    entries["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(entries.items())
    ).encode("utf-8")

    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    # A failed validation or write leaves any earlier successful bundle intact.
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        temporary_zip = Path(temporary) / "firmware.zip"
        with zipfile.ZipFile(temporary_zip, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(entries.items()):
                archive.writestr(name, data)
        temporary_zip.replace(destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output", type=Path, default=Path("dist/HCC_INSIGHT_firmware.zip"))
    args = parser.parse_args()
    package(args.build_dir, args.output)
    print(args.output)
