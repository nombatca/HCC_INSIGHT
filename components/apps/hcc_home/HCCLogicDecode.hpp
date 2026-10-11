/*
 * HCCLogicDecode: giải mã giao thức trên dữ liệu LOGIC ANALYZER (1 byte / mẫu, bit k = kênh k).
 * Không phụ thuộc ESP-IDF. Kết quả là danh sách chú thích (Annot) sắp theo thời gian để vẽ dưới các kênh.
 *
 *   I2C  : START / repeated START / STOP, byte lấy mẫu SDA ở cạnh lên SCL, bit 9 = ACK / NACK.
 *          Byte đầu sau START hiện "địa chỉ 7 bit + R/W".
 *   UART : mức nghỉ 1 (đảo được), 8 bit dữ liệu LSB trước, không parity, 1 stop bit; lấy mẫu giữa bit.
 *          Stop bit = 0 -> lỗi khung. baud = 0: tự đoán từ xung ngắn nhất (làm tròn về tốc độ chuẩn).
 *   SPI  : CS (tuỳ chọn, mức 0 tích cực), CLK, MOSI, MISO (tuỳ chọn), mode 0-3, MSB hoặc LSB trước, 8 bit.
 *   SPMI : dùng HCCSpmiDecode trên hai kênh SCLK / SDATA.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace HCCLogicDecode {

enum Proto : uint8_t { P_NONE, P_I2C, P_UART, P_SPI, P_SPMI };
const char *protoName(Proto p);

enum AnnKind : uint8_t {
    A_START,  // START / SSC
    A_STOP,
    A_ADDR,
    A_DATA,
    A_ACK,
    A_NACK,
    A_ERR,
};

struct Annot {
    uint32_t t0;
    uint32_t t1;
    AnnKind kind;
    char text[22];
};

struct Config {
    Proto proto = P_NONE;
    int8_t ch[4] = {0, 1, -1, -1};  // I2C: SCL, SDA | UART: RX | SPI: CLK, MOSI, MISO, CS | SPMI: SCLK, SDATA
    uint32_t baud = 115200;         // UART; 0 = tự đoán
    bool invert = false;            // UART mức nghỉ 0
    uint8_t spi_mode = 0;           // 0..3
    bool lsb_first = false;         // SPI
};

struct Result {
    std::vector<Annot> ann;
    uint32_t errors = 0;
    uint32_t baud_used = 0;  // UART
    bool truncated = false;  // quá MAX_ANN chú thích
};

constexpr size_t MAX_ANN = 200000;

void decode(const Config &cfg, const uint8_t *d, size_t n, uint32_t sample_hz, Result &out);

// UART: đoán baud từ độ rộng xung ngắn nhất của kênh (0 nếu không đủ dữ liệu).
uint32_t guessBaud(const uint8_t *d, size_t n, int ch, uint32_t sample_hz);

// Chỉ số chú thích đầu tiên có t1 >= t (để vẽ phần nhìn thấy).
size_t firstVisible(const std::vector<Annot> &a, uint32_t t);

}  // namespace HCCLogicDecode
