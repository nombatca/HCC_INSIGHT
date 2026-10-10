/*
 * HCCSpmiDecode: giải mã SPMI từ mẫu lấy liên tục của hai dây SCLK / SDATA (không phụ thuộc ESP-IDF).
 *
 * Mỗi mẫu 2 bit: bit0 = SCLK, bit1 = SDATA (Decoder::feed nhận byte gói 4 mẫu, mẫu đầu ở 2 bit thấp).
 *   SSC (bắt đầu chuỗi lệnh): SDATA lên rồi xuống trong khi SCLK ở mức 0.
 *   Bit: SDATA tại cạnh xuống của SCLK (master đổi dữ liệu ở cạnh lên, bên nhận lấy mẫu ở cạnh xuống).
 *   Kết thúc chuỗi: SSC tiếp theo hoặc SCLK đứng yên quá idle_samples mẫu.
 * Phân tích khung theo bảng lệnh SPMI: khung lệnh SID[3:0] + C[7:0] + P, khung địa chỉ / dữ liệu 8 bit + P,
 * parity lẻ; lệnh đọc có 1 chu kỳ bus park trước dữ liệu do slave gửi.
 * Chỉ ghi giá trị thô (SID, lệnh, địa chỉ, dữ liệu); không gán tên IC / đường nguồn.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace HCCSpmiDecode {

enum Kind : uint8_t {
    K_REG0_WRITE,     // 0x80-0xFF: ghi thanh ghi 0 (7 bit dữ liệu nằm trong lệnh)
    K_REG_WRITE,      // 0x40-0x5F
    K_REG_READ,       // 0x60-0x7F
    K_EXT_WRITE,      // 0x00-0x0F
    K_EXT_READ,       // 0x20-0x2F
    K_EXTL_WRITE,     // 0x30-0x37
    K_EXTL_READ,      // 0x38-0x3F
    K_RESET,          // 0x10
    K_SLEEP,          // 0x11
    K_SHUTDOWN,       // 0x12
    K_WAKEUP,         // 0x13
    K_OTHER,          // lệnh khác (Authenticate, Master R/W, TBO, DDB...) - chỉ ghi mã lệnh
    K_BAD,            // khung lệnh sai parity / thiếu bit
};

struct Txn {
    uint64_t t_sample;   // mẫu tại SSC
    uint8_t sid;
    uint8_t cmd;
    Kind kind;
    uint8_t len;         // số byte dữ liệu
    uint16_t addr;
    uint8_t data[8];
    uint8_t parity_err;  // số khung sai parity
    uint8_t short_bits;  // 1 nếu chuỗi ngắn hơn khung mong đợi
    uint16_t nbits;      // số bit thu được trong chuỗi
    uint16_t extra_bits; // số bit dư sau các khung mong đợi (vd ACK/NACK)
    uint8_t extra;       // tối đa 8 bit dư đầu tiên (MSB = bit đầu)
    uint16_t frame_err;  // bit i = khung thứ i sai parity (bit 0 = khung lệnh, sau đó địa chỉ rồi dữ liệu)
    uint8_t flags;       // F_*
    uint32_t dur;        // số mẫu từ SSC đến cạnh xuống SCLK cuối cùng
};

enum : uint8_t {
    F_LOST_BEFORE = 1,   // ngay trước gói này có dữ liệu bị mất (tràn bộ đệm) - thời gian / thứ tự có thể sai
};

// Trạng thái gói (cột STATUS). Thứ tự ưu tiên: lệnh hỏng > thiếu bit > sai parity > lệnh không giải mã > OK.
enum PktStatus : uint8_t {
    ST_OK,
    ST_PARITY,       // khung địa chỉ / dữ liệu sai parity
    ST_SHORT,        // chuỗi bị cắt (thiếu bit)
    ST_BAD_CMD,      // khung lệnh sai parity hoặc ngắn hơn 13 bit
    ST_UNKNOWN_CMD,  // lệnh hợp lệ nhưng ngoài các lớp được giải mã (chỉ có mã lệnh)
};

const char *kindName(Kind k);
const char *kindLongName(Kind k);  // tên lệnh theo chuẩn (Register Write...)
bool isRead(Kind k);
bool isWrite(Kind k);
// Lớp lệnh theo mã 8 bit (không xét parity).
Kind kindOf(uint8_t cmd);
PktStatus status(const Txn &t);
const char *statusName(PktStatus s);
// Số khung địa chỉ / dữ liệu mong đợi sau khung lệnh.
int addrFrames(Kind k);

// Phân tích một chuỗi bit (sau SSC). Trả false nếu không đủ 13 bit khung lệnh.
bool parse(const uint8_t *bits, int n, Txn &out);

typedef void (*TxnSink)(const Txn &t, void *user);

class Decoder {
public:
    void reset(uint32_t idle_samples, TxnSink sink, void *user);
    // data: byte gói 4 mẫu (2 bit / mẫu, mẫu đầu ở bit thấp). swap: đổi vai SCLK / SDATA.
    void feed(const uint8_t *data, size_t len);
    void flush();
    void resync();  // mất dữ liệu (tràn bộ đệm): bỏ chuỗi đang dở, gói kế tiếp mang cờ F_LOST_BEFORE
    // n mẫu bus nghỉ (SCLK = SDATA = 0) không có trong bộ đệm (khoảng trống giữa các đoạn sóng đã lưu).
    void idle(uint64_t n);
    // n mẫu bị mất (tràn bộ đệm): resync + tiến đồng hồ để thời gian các gói sau vẫn đúng.
    void lost(uint64_t n);

    uint64_t samples() const { return _t; }
    uint64_t sclkFalls() const { return _falls; }
    uint64_t sclkHighSamples() const { return _clk_samples; }  // tổng số mẫu trong các chuỗi (để ước tần số)
    bool swap = false;

private:
    void sample(int c, int d);
    void finish();

    TxnSink _sink = nullptr;
    void *_user = nullptr;
    uint32_t _idle = 64;
    uint64_t _t = 0;
    int _pc = 0, _pd = 0;
    bool _arm = false;     // đã thấy SDATA lên khi SCLK thấp
    bool _in = false;      // đang trong chuỗi
    uint64_t _start = 0;
    uint64_t _last_fall = 0;
    uint64_t _first_fall = 0;
    uint8_t _bits[512];
    int _n = 0;
    uint64_t _falls = 0;
    uint64_t _clk_samples = 0;
    bool _lost = false;
};

}  // namespace HCCSpmiDecode
