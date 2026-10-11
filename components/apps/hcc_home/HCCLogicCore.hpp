/*
 * HCCLogicCore: phần lõi LOGIC ANALYZER 8 kênh, không phụ thuộc ESP-IDF (test được trên máy tính).
 *
 * Dữ liệu: 1 byte / mẫu, bit k = kênh Dk.
 *   Trigger  : tìm điểm kích trong một đoạn mẫu (cạnh lên / xuống / bất kỳ của một kênh, hoặc mẫu 8 bit có mask).
 *   Mipmap   : tóm tắt theo khối 256 mẫu (OR / AND) để vẽ nhanh khi thu nhỏ.
 *   columns(): mỗi cột điểm ảnh -> với từng kênh: có mức 0 / có mức 1 (bit k / bit 8+k của uint16).
 *   measure(): tần số, chu kỳ, độ rộng xung, duty của một kênh trong một khoảng mẫu.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace HCCLogicCore {

constexpr int NCH = 8;

enum TrigType : uint8_t {
    T_NONE,     // chạy ngay
    T_RISE,     // cạnh lên kênh ch
    T_FALL,     // cạnh xuống kênh ch
    T_EDGE,     // cạnh bất kỳ kênh ch
    T_PATTERN,  // (mẫu & mask) == (value & mask), tính khi vừa chuyển sang khớp
};

const char *trigName(TrigType t);

struct Trigger {
    TrigType type = T_NONE;
    uint8_t ch = 0;
    uint8_t mask = 0;
    uint8_t value = 0;
};

// Tìm điểm kích đầu tiên trong d[0..n). prev = mẫu ngay trước d[0] (-1 nếu không có).
// Trả chỉ số mẫu (trong d) hoặc -1.
long findTrigger(const Trigger &t, const uint8_t *d, size_t n, int prev);

class Mipmap {
public:
    static constexpr uint32_t BLOCK = 256;
    void build(const uint8_t *d, size_t n);
    void clear()
    {
        _or.clear();
        _and.clear();
        _n = 0;
    }
    size_t blocks() const { return _or.size(); }
    uint8_t orAt(size_t b) const { return _or[b]; }
    uint8_t andAt(size_t b) const { return _and[b]; }
    size_t samples() const { return _n; }

private:
    std::vector<uint8_t> _or, _and;
    size_t _n = 0;
};

// Cột c phủ mẫu [t0 + c*spp, t0 + (c+1)*spp). out[c]: bit k = kênh k có mức 0, bit 8+k = có mức 1;
// 0 = ngoài dữ liệu.
void columns(const uint8_t *d, size_t n, const Mipmap *mm, double t0, double spp, int ncols, uint16_t *out);

struct Measure {
    bool valid = false;     // có ít nhất 2 cạnh lên
    uint32_t rises = 0, falls = 0;
    double freq_hz = 0;     // theo khoảng cách cạnh lên đầu - cuối
    double period_s = 0;
    double duty = 0;        // 0..1, trong các chu kỳ trọn vẹn
    double high_min_s = 0;  // xung mức 1 ngắn nhất (giữa 2 cạnh)
    double low_min_s = 0;
    int level = -1;         // mức không đổi (nếu không có cạnh nào): 0 / 1
};

Measure measure(const uint8_t *d, size_t from, size_t to, int ch, uint32_t sample_hz);

// DỮ LIỆU MÔ PHỎNG (nguồn DEMO - luôn gắn nhãn DEMO): D0/D1 = I2C (SCL/SDA), D2 = UART 115200 8N1,
// D3..D6 = SPI mode 0 (CLK, MOSI, MISO, CS), D7 = xung vuông 10 kHz. Trả vị trí "kích" gợi ý.
uint32_t makeDemo(uint8_t *d, size_t n, uint32_t sample_hz);

}  // namespace HCCLogicCore
