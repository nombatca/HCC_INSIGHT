#include "HCCAndroidUSB.hpp"

#include <cstdio>
#include <cstring>

namespace HCCAndroidUSB {

namespace {

constexpr uint8_t kDescTypeInterface = 0x04;
constexpr uint8_t kDescTypeEndpoint = 0x05;
constexpr uint8_t kDescTypeString = 0x03;
constexpr size_t kInterfaceDescSize = 9;
constexpr size_t kEndpointDescSize = 7;
constexpr uint8_t kTransferTypeBulk = 0x02;

constexpr uint16_t kVidSamsung = 0x04E8;
constexpr uint16_t kPidSamsungDownload = 0x685D;  // Download mode (Odin)

struct VendorEntry {
    uint16_t vid;
    const char *name;
};

// VID của các hãng Android phổ biến (USB-IF).
constexpr VendorEntry kVendors[] = {
    {kVidSamsung, "Samsung"},
    {0x18D1, "Google"},
    {0x2717, "Xiaomi"},
    {0x22D9, "OPPO/Realme"},
    {0x2D95, "vivo"},
    {0x2A70, "OnePlus"},
    {0x22B8, "Motorola"},
    {0x1004, "LG"},
    {0x12D1, "Huawei"},
    {0x0FCE, "Sony"},
    {0x0BB4, "HTC"},
    {0x19D2, "ZTE"},
    {0x17EF, "Lenovo"},
    {0x0B05, "ASUS"},
    {0x2E04, "Nokia (HMD)"},
};

}  // namespace

const char *vendorName(uint16_t vid)
{
    for (const auto &v : kVendors) {
        if (v.vid == vid) return v.name;
    }
    return nullptr;
}

Profile analyze(uint16_t vid, uint16_t pid, const uint8_t *config, size_t config_length)
{
    Profile p;
    p.vid = vid;
    p.pid = pid;
    p.vendor = vendorName(vid);
    p.samsung = (vid == kVidSamsung);

    bool fastboot = false;
    bool in_adb_interface = false;
    uint8_t cur_interface = 0;
    uint8_t bulk_in = 0, bulk_out = 0;
    uint16_t bulk_in_mps = 0;

    for (size_t off = 0; config != nullptr && off + 2 <= config_length;) {
        const uint8_t len = config[off];
        const uint8_t type = config[off + 1];
        if (len < 2 || off + len > config_length) break;  // descriptor hỏng: dừng quét

        if (type == kDescTypeInterface && len >= kInterfaceDescSize) {
            cur_interface = config[off + 2];
            const uint8_t cls = config[off + 5];
            const uint8_t sub = config[off + 6];
            const uint8_t proto = config[off + 7];
            in_adb_interface = false;

            if (cls == 0xFF && sub == 0x42 && proto == 0x01) {
                if (!p.has_adb) {
                    in_adb_interface = true;
                    bulk_in = bulk_out = 0;
                    bulk_in_mps = 0;
                }
            } else if (cls == 0xFF && sub == 0x42 && proto == 0x03) {
                fastboot = true;
            } else if (cls == 0x06 && sub == 0x01 && proto == 0x01) {
                p.has_mtp = true;
            } else if ((cls == 0xE0 && sub == 0x01 && proto == 0x03) ||
                       (cls == 0xEF && sub == 0x04 && proto == 0x01)) {
                p.has_rndis = true;
            } else if (cls == 0x02 && sub == 0x02) {
                p.has_acm = true;
            } else if (cls == 0x01 && sub == 0x03) {
                p.has_midi = true;
            }
        } else if (type == kDescTypeEndpoint && len >= kEndpointDescSize && in_adb_interface) {
            const uint8_t addr = config[off + 2];
            const uint8_t attrs = config[off + 3];
            const uint16_t mps = static_cast<uint16_t>(config[off + 4] | (config[off + 5] << 8));
            if ((attrs & 0x03) == kTransferTypeBulk) {
                if (addr & 0x80) {
                    bulk_in = addr;
                    bulk_in_mps = mps;
                } else {
                    bulk_out = addr;
                }
                if (bulk_in != 0 && bulk_out != 0 && bulk_in_mps != 0) {
                    p.has_adb = true;
                    p.adb_interface = cur_interface;
                    p.adb_bulk_in = bulk_in;
                    p.adb_bulk_out = bulk_out;
                    p.adb_bulk_in_mps = bulk_in_mps;
                    in_adb_interface = false;
                }
            }
        }
        off += len;
    }

    if (p.samsung && pid == kPidSamsungDownload) {
        p.kind = DeviceKind::SamsungDownloadMode;
    } else if (fastboot) {
        p.kind = DeviceKind::Fastboot;
    } else if (p.has_adb || p.has_mtp || p.vendor != nullptr) {
        // ADB (FF/42/01) chỉ có trên Android; MTP gần như luôn là điện thoại/máy tính bảng.
        // RNDIS/ACM một mình (VID lạ) có thể là modem USB -> không kết luận Android.
        p.kind = DeviceKind::Android;
    }
    return p;
}

void describeFunctions(const Profile &p, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) return;
    out[0] = '\0';
    const struct {
        bool on;
        const char *name;
    } items[] = {
        {p.has_mtp, "MTP"}, {p.has_adb, "ADB"}, {p.has_rndis, "RNDIS"},
        {p.has_acm, "ACM"}, {p.has_midi, "MIDI"},
    };
    size_t used = 0;
    for (const auto &it : items) {
        if (!it.on) continue;
        int n = snprintf(out + used, out_size - used, "%s%s", used ? " + " : "", it.name);
        if (n < 0 || static_cast<size_t>(n) >= out_size - used) break;
        used += static_cast<size_t>(n);
    }
    if (used == 0) snprintf(out, out_size, "-");
}

bool decodeStringDescriptor(const uint8_t *d, size_t length, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) return false;
    out[0] = '\0';
    if (d == nullptr || length < 2 || d[1] != kDescTypeString) return false;
    size_t b_length = d[0];
    if (b_length < 2 || b_length > length) return false;

    size_t o = 0;
    for (size_t i = 2; i + 1 < b_length && o + 1 < out_size; i += 2) {
        const uint16_t ch = static_cast<uint16_t>(d[i] | (d[i + 1] << 8));
        if (ch == 0) break;
        out[o++] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
    }
    out[o] = '\0';
    // Bỏ khoảng trắng cuối.
    while (o > 0 && out[o - 1] == ' ') out[--o] = '\0';
    return true;
}

}  // namespace HCCAndroidUSB
