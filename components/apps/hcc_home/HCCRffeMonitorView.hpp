/*
 * HCCRffeMonitorView: màn "RFFE MONITOR" 1024 x 600 - nghe thụ động bus RFFE (chỉ nghe, không phát lệnh).
 *
 *   Thanh trên : RFFE MONITOR, trạng thái (NO SIGNAL / CAPTURING / STOPPED / OVERFLOW / HARDWARE NOT SUPPORTED),
 *                nguồn + tần số lấy mẫu, SCLK ước lượng (MHz), số frame / lỗi / mất.
 *   Điều khiển : tần số lấy mẫu (20/40/80 MHz), START, STOP, CLEAR, SAVE SD, XÁC NHẬN MẠCH, BACK.
 *   Bộ lọc     : USID, IC, READ / WRITE / ERROR; PREV / NEXT trang; CUỐI (bám gói mới nhất).
 *   Bảng gói   : USID | IC | TIME | CMD | REGISTER | DATA | STATUS, trang 50 gói.
 *   Thống kê   : số gói / read / write / lỗi theo từng USID (IC).
 *
 * Phần THU dùng lại HCCSpmiMonitor (PARLIO + DMA + ring buffer); diễn giải gói theo RFFE qua
 * HCCRffeMonitor. Giao diện chỉ đọc dữ liệu bằng timer - không chờ task thu. Rời màn thì STOP
 * để trả chân SCLK / SDATA cho RFFE Scan.
 */
#pragma once

#include <vector>

#include "HCCRffe.hpp"
#include "lvgl.h"

namespace HCCRffeMonitorView {

// ref: dữ liệu tham chiếu RFFE đang nạp (USID -> IC). Rỗng cũng chạy (IC hiện "—").
void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user, const std::vector<HCCRffe::RefEntry> &ref);

}  // namespace HCCRffeMonitorView
