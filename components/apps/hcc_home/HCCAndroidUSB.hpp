#pragma once

#include <stddef.h>
#include <stdint.h>

// Nhận diện Android / Samsung qua USB descriptor (P0 - chưa nói chuyện ADB).
// Logic thuần, không phụ thuộc ESP-IDF để test được trên máy host.
namespace HCCAndroidUSB {

enum class DeviceKind {
    NotAndroid,            // không có dấu hiệu Android
    Android,               // có ADB/MTP/RNDIS... hoặc VID hãng Android
    Fastboot,              // bootloader fastboot (FF/42/03)
    SamsungDownloadMode,   // Samsung Download mode (Odin) - chỉ báo, không giao tiếp
};

struct Profile {
    DeviceKind kind = DeviceKind::NotAndroid;
    uint16_t vid = 0;
    uint16_t pid = 0;
    const char *vendor = nullptr;  // tên hãng theo VID, nullptr nếu không biết
    bool samsung = false;

    // Các chức năng USB điện thoại đang bật.
    bool has_adb = false;     // FF/42/01 - USB debugging đã bật
    bool has_mtp = false;     // 06/01/01 - truyền file
    bool has_rndis = false;   // E0/01/03 hoặc EF/04/01 - chia sẻ mạng
    bool has_acm = false;     // 02/02/xx - modem/serial (Samsung DM, Download mode)
    bool has_midi = false;    // 01/03/xx

    // Endpoint ADB (dùng ở P1).
    uint8_t adb_interface = 0;
    uint8_t adb_bulk_in = 0;
    uint8_t adb_bulk_out = 0;
    uint16_t adb_bulk_in_mps = 0;
};

// Tên hãng theo USB VID (các hãng Android phổ biến). nullptr nếu không có trong bảng.
const char *vendorName(uint16_t vid);

// Phân tích configuration descriptor (toàn bộ wTotalLength byte) + VID/PID.
// Trả về profile; kind == NotAndroid nếu không phải Android.
Profile analyze(uint16_t vid, uint16_t pid, const uint8_t *config, size_t config_length);

// Chuỗi mô tả chức năng, vd "MTP + ADB". Trả "-" nếu không có.
void describeFunctions(const Profile &profile, char *out, size_t out_size);

// Giải mã string descriptor USB (UTF-16LE) thành ASCII (ký tự ngoài ASCII -> '?').
// Trả false nếu descriptor không hợp lệ.
bool decodeStringDescriptor(const uint8_t *descriptor, size_t length, char *out, size_t out_size);

}  // namespace HCCAndroidUSB
