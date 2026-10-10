/*
 * HCCSpmiSynth: tạo dạng sóng SPMI (2 bit / mẫu như HCCSpmiDecode) từ lệnh cho trước.
 *
 * CHỈ DÙNG CHO KIỂM THỬ VÀ NGUỒN "DEMO". Dữ liệu sinh ra là mô phỏng, không phải kết quả đo; mọi nơi hiển thị /
 * lưu dữ liệu từ nguồn này phải ghi rõ DEMO.
 *
 * Quy ước giống bộ giải mã: master đổi SDATA ở cạnh lên SCLK, bên nhận lấy mẫu ở cạnh xuống; SSC = SDATA lên
 * rồi xuống khi SCLK thấp; khung = 8 bit (lệnh: SID 4 bit + 8 bit) + parity lẻ; lệnh đọc có 1 chu kỳ bus park
 * trước dữ liệu và mọi chuỗi kết thúc bằng 1 chu kỳ bus park.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace HCCSpmiSynth {

class Synth {
public:
    explicit Synth(int half_period_samples = 2) : _h(half_period_samples < 1 ? 1 : half_period_samples) {}

    void clear()
    {
        _buf.clear();
        _n = 0;
    }
    void level(int sclk, int sdata, int n);  // giữ mức n mẫu
    void idle(int n) { level(0, 0, n); }
    void ssc();
    void bit(int b);
    // Khung n bit (MSB trước) + parity lẻ; bad_parity = đảo bit parity (để thử lỗi).
    void frame(uint32_t v, int nbits, bool bad_parity = false);
    void park();

    // Các lệnh đầy đủ (SSC ... bus park). bad_* đảo parity khung tương ứng.
    void reg0Write(uint8_t sid, uint8_t v7);
    void regWrite(uint8_t sid, uint8_t addr5, uint8_t data, bool bad_data = false);
    void regRead(uint8_t sid, uint8_t addr5, uint8_t data, bool bad_data = false);
    void extWrite(uint8_t sid, uint8_t addr, const uint8_t *d, int n);
    void extRead(uint8_t sid, uint8_t addr, const uint8_t *d, int n);
    void extLongWrite(uint8_t sid, uint16_t addr, const uint8_t *d, int n);
    void extLongRead(uint8_t sid, uint16_t addr, const uint8_t *d, int n);
    void command(uint8_t sid, uint8_t cmd, bool bad_cmd = false);  // chỉ khung lệnh (Reset / Sleep...)

    size_t samples() const { return _n; }
    // 4 mẫu / byte, mẫu đầu ở 2 bit thấp, bit0 = SCLK, bit1 = SDATA (swap: đổi chỗ hai dây).
    const std::vector<uint8_t> &packed() const { return _buf; }
    std::vector<uint8_t> packedSwapped() const;

private:
    void put(int c, int d);
    int _h;
    std::vector<uint8_t> _buf;
    size_t _n = 0;
};

}  // namespace HCCSpmiSynth
