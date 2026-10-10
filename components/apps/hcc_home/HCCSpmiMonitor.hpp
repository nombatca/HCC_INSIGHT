/*
 * HCCSpmiMonitor: lõi SPMI MONITOR - task thu (nhân 0) chạy nguồn (HCCSpmiSource), giải mã (HCCSpmiDecode),
 * ghi gói vào PacketBuf và sóng vào RawStore (PSRAM), thống kê, lưu thẻ SD (HCCSpmiCapFile).
 * Giao diện chỉ đọc qua snapshot() / packets() / raw() từ timer LVGL - không bao giờ chờ task thu.
 *
 * Mất dữ liệu được phát hiện và báo (không giấu):
 *   - tràn hàng đợi / bộ đệm vòng DMA -> lost_chunks, gói kế tiếp mang F_LOST_BEFORE, trạng thái OVERFLOW
 *   - đầy bộ đệm gói (MAX_PACKETS) -> dừng thu, dropped_pkts, trạng thái OVERFLOW
 *   - đầy bộ nhớ sóng -> raw_truncated (gói vẫn giải mã tiếp, sóng sau đó hiện "không lưu")
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "HCCSpmiBuffers.hpp"
#include "HCCSpmiSource.hpp"

namespace HCCSpmiMonitor {

constexpr uint32_t MAX_PACKETS = 150000;

enum class Src : uint8_t { PARLIO, DEMO, EXTERNAL, FILE };

enum class State : uint8_t {
    IDLE,              // chưa thu
    CAPTURING,         // đang thu, có tín hiệu
    NO_SIGNAL,         // đang thu, > 1 s không có cạnh SCLK
    STOPPED,           // đã dừng (có dữ liệu)
    OVERFLOW,          // đang thu / đã dừng nhưng có dữ liệu bị mất
    HW_NOT_SUPPORTED,  // nguồn chưa được hỗ trợ / chưa xác nhận phần cứng
    REPLAY,            // đang phân tích file
    ERROR,
};

const char *stateName(State s);  // chữ hiển thị (NO SIGNAL, CAPTURING...)

struct Snapshot {
    State state = State::IDLE;
    Src src = Src::PARLIO;
    bool running = false;
    bool demo = false;          // dữ liệu mô phỏng
    bool replay = false;        // dữ liệu từ file
    bool swapped = false;       // dây 1 là SCLK
    bool lines_known = false;
    bool raw_truncated = false;
    bool undersampled = false;  // SCLK > tần số lấy mẫu / 4: giải mã không tin cậy
    uint32_t sample_hz = 0;
    float sclk_mhz = 0;
    uint32_t elapsed_ms = 0;
    uint32_t frames = 0;        // gói đã ghi
    uint32_t errors = 0;        // gói có trạng thái khác OK / LỆNH KHÁC
    uint32_t parity = 0, short_frames = 0, bad_cmd = 0, unknown_cmd = 0;
    uint32_t reads = 0, writes = 0;
    uint32_t dropped_pkts = 0;
    uint32_t lost_chunks = 0;
    uint32_t sid_count[16] = {};
    uint64_t total_samples = 0;
    char source[16] = "";
    char file[64] = "";         // tên file khi phân tích lại
    char msg[256] = "";
    char hw_why[320] = "";      // lý do khi HW_NOT_SUPPORTED
};

// Bắt đầu thu từ nguồn src (FILE: p.file là đường dẫn). false + err nếu không bắt đầu được
// (với nguồn không hỗ trợ: trạng thái chuyển HW_NOT_SUPPORTED, err = lý do).
bool start(Src src, const HCCSpmiSource::Params &p, char *err, size_t err_size);
void stop(void);
bool running(void);
void clear(void);  // xoá dữ liệu (khi không thu)
Snapshot snapshot(void);

const HCCSpmiBuffers::PacketBuf &packets(void);
const HCCSpmiBuffers::RawStore &raw(void);
uint32_t sampleHz(void);

// Xác nhận phần cứng thu (lưu NVS).
bool hwConfirmed(void);
void setHwConfirmed(bool ok);

// Lưu /sdcard/spmi/mon_NNN.spmc + .csv ở task nền.
bool startSave(void);
bool saving(void);
bool saveResult(char *msg, size_t size);  // true nếu thành công

}  // namespace HCCSpmiMonitor
