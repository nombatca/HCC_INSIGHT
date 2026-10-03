#pragma once

#include <stdint.h>

// USB DISK: cho Mac/PC thấy thẻ SD của board như một ổ USB (USB Mass Storage, TinyUSB).
//
// ESP32-P4 có 2 PHY USB Full-Speed trong chip: PHY0 (GPIO24/25) nối cổng Type-C của board, mặc định
// dùng cho USB-Serial/JTAG (flash + monitor); PHY1 (GPIO26/27) cho bộ USB OTG Full-Speed. Khi bật,
// module đổi chéo hai PHY (LP_SYS.usb_ctrl) để OTG FS ra cổng Type-C -> máy tính thấy ổ đĩa; khi tắt
// thì đổi lại -> monitor/flash như cũ. Cổng USB-A (OTG High-Speed, đọc iPhone/Android) không đụng tới.
//
// Trong lúc bật, board KHÔNG dùng thẻ SD (máy tính sở hữu hệ thống file): panic/case/nhạc trên thẻ tạm
// không truy cập được. start()/stop() chạy trên task riêng; UI đọc state().
namespace HCCUsbDisk {

enum class State : uint8_t {
    Off,        // thẻ SD thuộc về board như bình thường
    Starting,
    Ready,      // đã mở USB, chờ máy tính nhận
    Connected,  // máy tính đã nhận ổ đĩa
    Stopping,
    Error,      // xem lastError()
};

// Gọi một lần lúc khởi động (sau NVS): trả PHY USB về mặc định, đọc dấu vết lần crash trước.
void bootInit();
// "" hoặc mô tả bước đang làm khi board dừng ở lần chạy trước.
const char *lastCrash();

// Bật / tắt (bất đồng bộ). Trả false nếu đang bận.
bool requestStart();
bool requestStop();

State state();
const char *stateText();
const char *lastError();
// Dung lượng thẻ (MB) khi đang bật, 0 nếu chưa biết.
uint32_t capacityMb();
// Thông tin chẩn đoán PHY (eFuse, PHY được chọn) - hiện trên màn hình vì lúc bật không có monitor.
const char *phyNote();

}  // namespace HCCUsbDisk
