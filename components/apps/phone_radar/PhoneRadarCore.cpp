#include "PhoneRadarCore.hpp"

#include <ctype.h>
#include <math.h>

int PhoneRadarCore::find(const uint8_t mac[6]) const
{
    for (int i = 0; i < _n; ++i) {
        if (memcmp(_t[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

float PhoneRadarCore::angleForMac(const uint8_t mac[6])
{
    // Hash FNV-1a 32-bit -> góc 0..360 ổn định theo MAC (chấm không nhảy lung tung).
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; ++i) { h ^= mac[i]; h *= 16777619u; }
    return (float)(h % 3600) / 10.0f;
}

float PhoneRadarCore::radiusForRssi(float rssi)
{
    // -40 dBm (rất gần) -> 0.14 ; -92 dBm (xa) -> 0.96. Tuyến tính + chặn biên.
    const float near_dbm = -40.0f, far_dbm = -92.0f;
    float t = (rssi - near_dbm) / (far_dbm - near_dbm);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return 0.14f + t * (0.96f - 0.14f);
}

bool PhoneRadarCore::looksLikePhone(const char *ssid)
{
    if (ssid == nullptr || ssid[0] == '\0') return false;
    char low[40];
    int n = 0;
    for (const char *p = ssid; *p && n < (int)sizeof(low) - 1; ++p) {
        low[n++] = (char)tolower((unsigned char)*p);
    }
    low[n] = '\0';
    static const char *kHints[] = {
        "iphone", "ipad", "galaxy", "androidap", "android", "redmi", "xiaomi",
        "oppo", "vivo", "huawei", "honor", "oneplus", "pixel", "hotspot",
        "phone", "dien thoai", "realme", "nokia", "samsung",
    };
    for (const char *h : kHints) {
        if (strstr(low, h) != nullptr) return true;
    }
    return false;
}

int PhoneRadarCore::observe(const uint8_t mac[6], int rssi, const char *name, int channel,
                            uint8_t kind, uint32_t now_ms)
{
    if (rssi == 0 || rssi < -100 || rssi > 0) {
        // 0 hoặc ngoài dải hợp lệ -> bỏ.
        if (rssi == 0) return -1;
    }
    int idx = find(mac);
    if (idx < 0) {
        if (_n >= kMax) {
            // Đầy: thay thế mục tiêu yếu nhất (RSSI thấp nhất) nếu cái mới mạnh hơn.
            int weakest = 0;
            for (int i = 1; i < _n; ++i) {
                if (_t[i].rssi < _t[weakest].rssi) weakest = i;
            }
            if ((float)rssi <= _t[weakest].rssi) return -1;
            idx = weakest;
        } else {
            idx = _n++;
        }
        PhoneRadarTarget &t = _t[idx];
        memcpy(t.mac, mac, 6);
        t.rssi = (float)rssi;
        t.hits = 0;
        t.angle_deg = angleForMac(mac);
        t.name[0] = '\0';
    }
    PhoneRadarTarget &t = _t[idx];
    t.rssi_raw = rssi;
    // Làm mượt EMA để chấm không giật.
    t.rssi += 0.4f * ((float)rssi - t.rssi);
    t.channel = (uint8_t)channel;
    t.kind = kind;
    t.last_ms = now_ms;
    t.hits++;
    if (name != nullptr && name[0] != '\0') {
        strncpy(t.name, name, sizeof(t.name) - 1);
        t.name[sizeof(t.name) - 1] = '\0';
    }
    return idx;
}

void PhoneRadarCore::age(uint32_t now_ms, uint32_t ttl_ms)
{
    int w = 0;
    for (int i = 0; i < _n; ++i) {
        if (now_ms - _t[i].last_ms <= ttl_ms) {
            if (w != i) _t[w] = _t[i];
            ++w;
        }
    }
    _n = w;
}

int PhoneRadarCore::countKind(uint8_t kind) const
{
    int c = 0;
    for (int i = 0; i < _n; ++i) if (_t[i].kind == kind) ++c;
    return c;
}
