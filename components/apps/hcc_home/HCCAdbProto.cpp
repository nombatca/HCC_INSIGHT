#include "HCCAdbProto.hpp"

#include <cstring>
#include <cstdlib>

namespace HCCAdb {

uint32_t payloadChecksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    return sum;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t buildMessage(uint32_t command, uint32_t arg0, uint32_t arg1,
                    const uint8_t *payload, size_t len, uint8_t *out)
{
    put32(out + 0, command);
    put32(out + 4, arg0);
    put32(out + 8, arg1);
    put32(out + 12, (uint32_t)len);
    put32(out + 16, payloadChecksum(payload, len));
    put32(out + 20, command ^ 0xffffffffu);
    if (payload && len) memcpy(out + 24, payload, len);
    return 24 + len;
}

bool parseHeader(const uint8_t *data, size_t len, Message &msg)
{
    if (len < 24) return false;
    msg.command = get32(data + 0);
    msg.arg0 = get32(data + 4);
    msg.arg1 = get32(data + 8);
    msg.data_length = get32(data + 12);
    msg.data_check = get32(data + 16);
    msg.magic = get32(data + 20);
    return msg.magic == (msg.command ^ 0xffffffffu);
}

bool checkPayload(const Message &hdr, const uint8_t *payload, size_t len)
{
    if (len != hdr.data_length) return false;
    return payloadChecksum(payload, len) == hdr.data_check;
}

// ------------------- Android info -------------------

const char *combinedShellCommand()
{
    // 1 stream lấy mọi thứ; các mốc @@...@@ để tách đoạn khi parse.
    return "getprop;echo @@BATT@@;dumpsys battery;echo @@BT@@;"
           "settings get secure bluetooth_address;echo @@WIFI@@;"
           "cat /sys/class/net/wlan0/address 2>/dev/null";
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Lấy value của 1 key trong output getprop: dòng "[key]: [value]".
static std::string getProp(const std::string &props, const char *key)
{
    std::string pat = "[" + std::string(key) + "]: [";
    size_t p = props.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = props.find(']', p);
    if (e == std::string::npos) return "";
    return trim(props.substr(p, e - p));
}

// Lấy số nguyên sau "key:" trong 1 đoạn (dumpsys battery).
static int getIntAfter(const std::string &sec, const char *key)
{
    size_t p = sec.find(key);
    if (p == std::string::npos) return -1000;
    p += strlen(key);
    while (p < sec.size() && (sec[p] == ' ' || sec[p] == ':')) ++p;
    bool neg = false;
    if (p < sec.size() && sec[p] == '-') { neg = true; ++p; }
    if (p >= sec.size() || sec[p] < '0' || sec[p] > '9') return -1000;
    long v = 0;
    while (p < sec.size() && sec[p] >= '0' && sec[p] <= '9') { v = v * 10 + (sec[p] - '0'); ++p; }
    return (int)(neg ? -v : v);
}

// Tìm 1 chuỗi MAC "xx:xx:xx:xx:xx:xx" trong đoạn.
static std::string findMac(const std::string &sec)
{
    for (size_t i = 0; i + 17 <= sec.size(); ++i) {
        bool ok = true;
        for (int k = 0; k < 17; ++k) {
            char c = sec[i + k];
            if (k % 3 == 2) { if (c != ':') { ok = false; break; } }
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { ok = false; break; }
        }
        if (ok) return sec.substr(i, 17);
    }
    return "";
}

AndroidInfo parseInfo(const std::string &out)
{
    AndroidInfo in;
    size_t pB = out.find("@@BATT@@");
    size_t pT = out.find("@@BT@@");
    size_t pW = out.find("@@WIFI@@");

    std::string props = out.substr(0, pB == std::string::npos ? out.size() : pB);
    std::string batt = (pB != std::string::npos)
        ? out.substr(pB, (pT == std::string::npos ? out.size() : pT) - pB) : "";
    std::string bt = (pT != std::string::npos)
        ? out.substr(pT, (pW == std::string::npos ? out.size() : pW) - pT) : "";
    std::string wifi = (pW != std::string::npos) ? out.substr(pW) : "";

    in.brand = getProp(props, "ro.product.brand");
    if (in.brand.empty()) in.brand = getProp(props, "ro.product.manufacturer");
    in.model = getProp(props, "ro.product.model");
    in.serial = getProp(props, "ro.serialno");
    if (in.serial.empty()) in.serial = getProp(props, "ro.boot.serialno");
    in.android_ver = getProp(props, "ro.build.version.release");

    in.batt_level = getIntAfter(batt, "level");
    in.batt_status = getIntAfter(batt, "status");
    in.batt_temp_dC = getIntAfter(batt, "temperature");
    in.batt_mv = getIntAfter(batt, "voltage");

    std::string m = findMac(bt);
    // "02:00:00:00:00:00" = MAC bị ẩn do riêng tư -> coi như không có.
    in.bt_mac = (m == "02:00:00:00:00:00") ? "" : m;
    in.wifi_mac = findMac(wifi);

    in.ok = !in.brand.empty() || !in.model.empty() || !in.serial.empty() || in.batt_level >= 0;
    return in;
}

const char *batteryStatusName(int status)
{
    switch (status) {
        case 2: return "Đang sạc";
        case 3: return "Đang xả";
        case 4: return "Không sạc";
        case 5: return "Đầy";
        default: return "Không rõ";
    }
}

}  // namespace HCCAdb
