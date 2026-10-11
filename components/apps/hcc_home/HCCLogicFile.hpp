/*
 * HCCLogicFile: lưu / đọc capture LOGIC ANALYZER (không phụ thuộc ESP-IDF).
 *
 * .lab (nhị phân, little-endian): header 128 byte
 *   "HCCLOGIC", u32 version (1), u32 sample_hz, u32 số mẫu, u32 vị trí kích (0xFFFFFFFF = không có),
 *   u32 flags (LF_*), 8 x 8 byte tên kênh, dự trữ; sau đó 1 byte / mẫu (bit k = kênh k).
 * .vcd: Value Change Dump - mở bằng PulseView / GTKWave (timescale 1 ps, chỉ ghi điểm đổi mức).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace HCCLogicFile {

enum : uint32_t {
    LF_PARTIAL = 1,  // dừng sớm / tràn - dữ liệu ít hơn yêu cầu
    LF_DEMO = 2,     // dữ liệu mô phỏng
};

struct Info {
    uint32_t sample_hz = 0;
    uint32_t samples = 0;
    uint32_t trig = 0xFFFFFFFF;
    uint32_t flags = 0;
    char names[8][8] = {};
};

bool saveLab(const char *path, const Info &info, const uint8_t *d, char *err, size_t es);
bool readLabInfo(const char *path, Info &info, char *err, size_t es);
// Đọc tối đa cap mẫu vào d. info.samples = số mẫu đã đọc.
bool loadLab(const char *path, Info &info, uint8_t *d, uint32_t cap, char *err, size_t es);
// mask: kênh được ghi (bit k).
bool exportVcd(const char *path, const Info &info, const uint8_t *d, uint8_t mask, char *err, size_t es);

}  // namespace HCCLogicFile
