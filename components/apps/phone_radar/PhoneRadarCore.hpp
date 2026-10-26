#pragma once

#include <stdint.h>
#include <string.h>

// Lõi "RADAR THIẾT BỊ": gom các mục tiêu quét được (WiFi AP, hotspot điện thoại,
// hoặc thiết bị phát probe-request) thành danh sách có RSSI làm mượt, tự già đi khi
// lâu không thấy, và tính vị trí chấm trên màn radar. Thuần C++ (test host được).
//
// Lưu ý thực tế: điện thoại ngẫu nhiên hoá MAC nên KHÔNG định danh được máy cụ thể;
// chỉ đếm hiện diện + ước lượng khoảng cách tương đối theo cường độ sóng (RSSI).

enum PhoneRadarKind : uint8_t {
    PR_AP = 0,        // WiFi access point (router / IoT)
    PR_HOTSPOT = 1,   // AP trông giống hotspot điện thoại (theo tên)
    PR_DEVICE = 2,    // thiết bị phát probe-request (có thể là điện thoại ở gần)
};

struct PhoneRadarTarget {
    uint8_t mac[6];
    char name[33];       // SSID (AP); rỗng với probe-device
    float rssi;          // dBm đã làm mượt
    int rssi_raw;        // dBm mẫu cuối
    uint8_t channel;
    uint8_t kind;        // PhoneRadarKind
    uint32_t last_ms;    // lần thấy gần nhất
    uint32_t hits;       // số lần quan sát
    float angle_deg;     // góc ổn định trên radar (từ hash MAC)
};

class PhoneRadarCore {
public:
    static constexpr int kMax = 48;

    void reset() { _n = 0; }

    // Ghi nhận một quan sát. Gộp theo MAC (cập nhật RSSI/last_ms), hoặc thêm mới.
    // Trả về chỉ số mục tiêu, hoặc -1 nếu đầy.
    int observe(const uint8_t mac[6], int rssi, const char *name, int channel,
                uint8_t kind, uint32_t now_ms);

    // Xoá mục tiêu không thấy trong ttl_ms.
    void age(uint32_t now_ms, uint32_t ttl_ms);

    int count() const { return _n; }
    const PhoneRadarTarget &at(int i) const { return _t[i]; }

    // Đếm theo loại.
    int countKind(uint8_t kind) const;
    // Số mục tiêu có thể là điện thoại (HOTSPOT + DEVICE).
    int countPhones() const { return countKind(PR_HOTSPOT) + countKind(PR_DEVICE); }

    // Bán kính chuẩn hoá 0..1 theo RSSI: mạnh (gần) -> gần tâm; yếu (xa) -> ra mép.
    static float radiusForRssi(float rssi);
    // Tên trông giống hotspot điện thoại không? (dò chuỗi phổ biến)
    static bool looksLikePhone(const char *ssid);

private:
    PhoneRadarTarget _t[kMax];
    int _n = 0;

    int find(const uint8_t mac[6]) const;
    static float angleForMac(const uint8_t mac[6]);
};
