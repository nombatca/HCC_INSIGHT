#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

// Lớp giao thức ADB (Android Debug Bridge) qua USB - phần THUẦN LOGIC:
//   - đóng/mở gói message (header 24 byte + payload),
//   - checksum + magic,
//   - tách thông tin Android từ output shell (getprop / dumpsys battery ...).
// Phần RSA (AUTH) và truyền USB bulk là glue riêng ở lớp thiết bị.
namespace HCCAdb {

// Lệnh ADB (little-endian uint32).
enum : uint32_t {
    A_CNXN = 0x4e584e43,
    A_AUTH = 0x48545541,
    A_OPEN = 0x4e45504f,
    A_OKAY = 0x59414b4f,
    A_CLSE = 0x45534c43,
    A_WRTE = 0x45545257,
};

enum : uint32_t {
    A_VERSION = 0x01000000,
    A_MAXDATA = 256 * 1024,
};

// Loại AUTH.
enum : uint32_t {
    ADB_AUTH_TOKEN = 1,
    ADB_AUTH_SIGNATURE = 2,
    ADB_AUTH_RSAPUBLICKEY = 3,
};

struct Message {
    uint32_t command = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    uint32_t data_length = 0;
    uint32_t data_check = 0;
    uint32_t magic = 0;
};

// Checksum payload = tổng các byte (mod 2^32).
uint32_t payloadChecksum(const uint8_t *data, size_t len);

// Đóng 1 message (header + payload) vào out (phải đủ 24+len byte). Trả tổng số byte.
// payload có thể nullptr nếu len==0.
size_t buildMessage(uint32_t command, uint32_t arg0, uint32_t arg1,
                    const uint8_t *payload, size_t len, uint8_t *out);

// Đọc header 24 byte -> msg. Trả false nếu thiếu byte hoặc magic sai.
bool parseHeader(const uint8_t *data, size_t len, Message &msg);

// Kiểm tra payload khớp data_length + data_check của header.
bool checkPayload(const Message &hdr, const uint8_t *payload, size_t len);

// ---- Thông tin Android tách từ shell ----
struct AndroidInfo {
    std::string brand;
    std::string model;
    std::string serial;
    std::string android_ver;
    std::string wifi_mac;
    std::string bt_mac;
    int  batt_level = -1;      // %
    int  batt_status = -1;     // mã dumpsys (2=charging,3=discharging,5=full...)
    int  batt_temp_dC = -1000; // nhiệt độ 1/10 °C
    int  batt_mv = -1;         // điện áp mV
    bool ok = false;
};

// Câu lệnh shell gộp 1 lần (có dấu phân đoạn) để lấy mọi thứ trong 1 stream.
const char *combinedShellCommand();

// Tách AndroidInfo từ output của combinedShellCommand().
AndroidInfo parseInfo(const std::string &shell_output);

// Tên trạng thái pin theo mã dumpsys.
const char *batteryStatusName(int status);

}  // namespace HCCAdb
