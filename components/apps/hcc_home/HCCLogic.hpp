/*
 * HCCLogic: engine LOGIC ANALYZER 8 kênh trên ESP32-P4.
 *
 * Phần cứng: PARLIO RX 8 bit lấy mẫu liên tục D0..D7 (chân đặt trong menuconfig > HCC LOGIC ANALYZER) bằng DMA
 * vào bộ đệm vòng 16 x 4032 byte RAM nội; task nhân 0 chép sang bộ đệm vòng PSRAM (độ sâu chọn được) và tìm điểm
 * kích. Sau khi kích, thu thêm phần sau kích rồi dừng; dữ liệu được xếp lại thẳng hàng (mẫu cũ nhất ở đầu).
 * Chỉ NGHE: chân cấu hình vào, tắt điện trở kéo. Mức vào 0 / 3,3 V.
 *
 * Mất dữ liệu (giải mã / chép không kịp, thường ở 40-80 MHz):
 *   - trước khi kích: bỏ phần đã thu (không liên tục) và thu lại từ đó
 *   - sau khi kích: dừng ngay, giữ phần liên tục đến chỗ mất, trạng thái OVERFLOW
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "HCCLogicCore.hpp"

namespace HCCLogic {

enum class State : uint8_t {
    IDLE,
    ARMED,       // đang chờ điểm kích
    CAPTURING,   // đã kích, đang thu phần sau
    DONE,
    OVERFLOW,    // dừng do mất dữ liệu sau khi kích (dữ liệu đến chỗ mất vẫn dùng được)
    ERROR,
    NO_PINS,     // chưa cấu hình / chân không hợp lệ
};
const char *stateName(State s);

struct Config {
    uint32_t sample_hz = 10000000;
    uint32_t depth = 2 * 1024 * 1024;  // số mẫu (byte)
    uint8_t pre_pct = 10;              // % trước điểm kích
    HCCLogicCore::Trigger trig;
};

struct Status {
    State state = State::IDLE;
    bool running = false;
    bool demo = false;
    bool file = false;
    uint32_t sample_hz = 0;
    uint32_t written = 0;     // số mẫu đã nhận (phiên hiện tại)
    uint32_t samples = 0;     // số mẫu dữ liệu đang có
    uint32_t trig = 0xFFFFFFFF;
    uint32_t lost = 0;        // số lần mất dữ liệu
    uint32_t elapsed_ms = 0;
    uint32_t serial = 0;      // tăng mỗi lần có dữ liệu mới (thu / DEMO / mở file xong)
    char msg[200] = "";
};

// Chân kênh k (-1 = không dùng). pinError(): nullptr nếu cấu hình hợp lệ.
int pin(int ch);
const char *pinError(void);

bool start(const Config &cfg, char *err, size_t es);
bool startDemo(const Config &cfg, char *err, size_t es);  // dữ liệu MÔ PHỎNG, gắn nhãn DEMO
bool startLoad(const char *path, char *err, size_t es);   // đọc file .lab ở task nền
void stop(void);  // ARMED: lấy phần đã thu (không có điểm kích); CAPTURING: dừng sớm
Status status(void);

// Chỉ đọc khi !running.
const uint8_t *data(void);
const HCCLogicCore::Mipmap &mipmap(void);

// Lưu /sdcard/logic/la_NNN.lab + .vcd (task nền). names: 8 tên kênh (tối đa 7 ký tự).
bool startSave(const char names[8][8]);
bool saving(void);
// Tên kênh đọc từ file .lab vừa mở (false nếu dữ liệu không phải từ file).
bool fileNames(char out[8][8]);
bool saveResult(char *msg, size_t size);

}  // namespace HCCLogic
