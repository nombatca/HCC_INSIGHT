/*
 * HCCSpmiMonitorView: màn "SPMI MONITOR" 1024 x 600 - nghe thụ động bus SPMI (không phát lệnh).
 *
 *   Thanh trên : SPMI MONITOR, trạng thái (NO SIGNAL / CAPTURING / STOPPED / OVERFLOW / HARDWARE NOT SUPPORTED /
 *                REPLAY), nhãn DEMO khi dữ liệu mô phỏng, nguồn + tần số lấy mẫu, SCLK ước lượng, số frame / lỗi / mất.
 *   Dạng sóng  : SCLK / SDATA từ sóng thô đã lưu, kéo ngang để dời, nút - / + phóng to, GÓI = vừa gói đang chọn.
 *   Bảng gói   : TIME | SID | CMD | REGISTER | DATA | STATUS, lọc SID và READ / WRITE / LỖI, trang 50 gói, CUỐI =
 *                bám gói mới nhất. Chạm một gói để xem chi tiết (khung, parity, bit dư) và nhảy sóng tới gói.
 *   Nút        : START, STOP, CLEAR, SAVE SD, OPEN (phân tích lại file .spmc / .csv), ANALYZE (PMIC ANALYZER).
 * Giao diện chỉ đọc dữ liệu của HCCSpmiMonitor bằng timer 100 ms - không chờ task thu.
 */
#pragma once

#include "lvgl.h"

namespace HCCSpmiMonitorView {

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user);

}  // namespace HCCSpmiMonitorView
