/*
 * HCCSpmiLog: nghe thụ động bus SPMI (không gửi gì lên bus) trong 10-90 giây và ghi lại mọi giao dịch.
 *
 * Phần cứng: PARLIO RX của ESP32-P4 lấy mẫu liên tục 2 dây (SCLK, SDATA trên chân RFFE: GPIO2 / GPIO3) ở
 * 20 / 40 / 80 MHz bằng DMA vào bộ đệm vòng trong RAM nội; task nền giải mã ngay (HCCSpmiDecode) nên không cần
 * lưu dữ liệu thô. Vai SCLK / SDATA tự nhận theo dây nào đổi mức nhiều hơn. Tắt điện trở kéo trong chip.
 * Bus SPMI chạy 1,2 V / 1,8 V: phải qua mạch chuyển mức đủ nhanh.
 *
 * Chỉ lưu giá trị thô (SID, lệnh, địa chỉ, dữ liệu, lỗi parity); không gán tên đường nguồn / IC.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "HCCSpmiDecode.hpp"

namespace HCCSpmiLog {

struct Config {
    int seconds = 30;               // 10..90
    uint32_t sample_hz = 80000000;  // 20 / 40 / 80 MHz
};

struct Status {
    bool running = false;
    bool done = false;              // đã có một lần bắt xong
    uint32_t elapsed_ms = 0;
    uint32_t total_ms = 0;
    uint32_t txns = 0;
    uint32_t dropped_txns = 0;      // vượt sức chứa bộ nhớ
    uint32_t parity_errs = 0;       // số giao dịch có khung sai parity
    uint32_t bad_frames = 0;        // khung lệnh hỏng
    uint32_t short_frames = 0;      // thiếu bit
    uint32_t overruns = 0;          // mất dữ liệu do giải mã không kịp
    float sclk_mhz = 0;             // tần số SCLK ước lượng
    bool swapped = false;           // SCLK nằm ở chân SDATA cấu hình
    bool lines_found = false;       // đã thấy hoạt động để nhận vai dây
    uint32_t sample_hz = 0;
    char msg[128] = "";
};

struct Stat {
    uint8_t sid;
    uint16_t addr;
    bool ext;            // địa chỉ từ lệnh Extended (8 / 16 bit) thay vì 5 bit
    uint32_t writes;
    uint32_t reads;
    uint8_t first_w;
    uint8_t last_w;
    uint8_t last_r;
    bool has_w;
    bool has_r;
};

bool start(const Config &cfg, char *err, size_t err_size);
void stop(void);  // dừng sớm
Status status(void);

// Giao dịch đã ghi: chỉ số [0, count) không đổi sau khi ghi (an toàn đọc từ giao diện).
uint32_t count(void);
const HCCSpmiDecode::Txn *txn(uint32_t i);
uint32_t sampleHz(void);
// Thống kê theo (SID, địa chỉ), sắp theo SID rồi địa chỉ.
std::vector<Stat> stats(void);

// Lưu CSV ra /sdcard/spmi/ ở task nền. saving() true khi đang lưu; saveResult() lấy đường dẫn / lỗi khi xong.
bool startSave(void);
bool saving(void);
bool saveResult(char *msg, size_t size);  // true nếu lưu thành công

}  // namespace HCCSpmiLog
