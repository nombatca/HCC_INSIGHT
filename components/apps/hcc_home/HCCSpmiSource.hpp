/*
 * HCCSpmiSource: giao diện "Capture Driver" của SPMI MONITOR - tách nguồn dữ liệu khỏi bộ giải mã / giao diện.
 *
 * Mỗi nguồn chạy trong task thu của HCCSpmiMonitor (Source::run chặn đến khi xong / bị dừng) và đẩy dữ liệu
 * qua Feed:
 *   - luồng mẫu 2 bit (bit0 = dây 0, bit1 = dây 1, 4 mẫu / byte, mẫu đầu ở 2 bit thấp)  -> samples / idle / lost
 *   - hoặc gói đã giải mã sẵn (file CSV, sau này: FPGA / STM32 giải mã tại chỗ)        -> packet
 * Nguồn hiện có:
 *   parlio()   : PARLIO RX của ESP32-P4 trên chân SCLK / SDATA cấu hình cho RFFE. CHỈ NGHE (không cấu hình
 *                chân ra, tắt điện trở kéo). Bị khoá (HARDWARE NOT SUPPORTED) cho đến khi người dùng xác nhận
 *                đã lắp mạch chuyển mức một chiều tốc độ cao (xem docs/SPMI_MONITOR.md).
 *   demo()     : dữ liệu MÔ PHỎNG để thử giao diện / chuỗi xử lý - luôn gắn cờ DEMO.
 *   file()     : phân tích lại file .spmc (giải mã lại từ sóng nếu đủ) hoặc .csv.
 *   external() : chỗ cho bộ thu FPGA / STM32 sau này - hiện chưa có driver (HARDWARE NOT SUPPORTED).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "HCCSpmiBuffers.hpp"
#include "HCCSpmiDecode.hpp"

namespace HCCSpmiSource {

struct Params {
    uint32_t sample_hz = 80000000;  // PARLIO: 20 / 40 / 80 MHz
    bool hw_confirmed = false;      // người dùng đã xác nhận mạch chuyển mức (chỉ PARLIO)
    char file[160] = "";            // nguồn file
};

class Feed {
public:
    virtual ~Feed() {}
    virtual void samples(const uint8_t *d, size_t n) = 0;  // tiếp nối vị trí hiện tại
    virtual void idle(uint64_t n) = 0;                     // n mẫu bus nghỉ không gửi kèm
    virtual void lost(uint64_t n) = 0;                     // n mẫu bị mất (tràn bộ đệm)
    virtual void packet(const HCCSpmiDecode::Txn &t) = 0;  // gói đã giải mã sẵn
    virtual HCCSpmiBuffers::RawStore &rawStore() = 0;      // để nạp sóng từ file
    virtual void setSampleHz(uint32_t hz) = 0;
    virtual void setOrientation(int o) = 0;                // 0 tự nhận, 1 dây 0 = SCLK, 2 dây 1 = SCLK
    virtual void setDemo(bool demo) = 0;
    virtual void setFlags(uint32_t capfile_flags, uint32_t lost_chunks, uint32_t dropped) = 0;  // từ file
    virtual bool stopRequested() = 0;
    virtual void message(const char *msg) = 0;
};

class Source {
public:
    virtual ~Source() {}
    virtual const char *name() const = 0;
    // false: phần cứng / driver không hỗ trợ (why = lý do hiển thị dưới HARDWARE NOT SUPPORTED).
    virtual bool supported(const Params &p, char *why, size_t n) const = 0;
    virtual bool live() const { return true; }  // false: phân tích file
    // Chặn đến khi dừng / hết dữ liệu. false + err khi lỗi.
    virtual bool run(const Params &p, Feed &f, char *err, size_t n) = 0;
};

Source *parlio();
Source *demo();
Source *file();
Source *external();

}  // namespace HCCSpmiSource
