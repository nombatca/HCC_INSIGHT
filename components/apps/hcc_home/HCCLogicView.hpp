/*
 * HCCLogicView: màn "LOGIC ANALYZER" 8 kênh (1024 x 600).
 *
 *   Thanh trên : trạng thái (READY / WAIT TRIGGER / CAPTURING / DONE / OVERFLOW / NO PINS / ERROR), nhãn DEMO,
 *                tần số lấy mẫu, độ sâu, điều kiện kích.
 *   Vùng sóng  : D0..D7 (tên tự đổi theo giao thức đang giải mã) + 2 hàng chú thích giải mã; kéo ngang để dời,
 *                - / + phóng to, VỪA = toàn bộ; chạm vùng sóng đặt con trỏ A / B (đo ΔT); chạm tên kênh để đo
 *                tần số / duty / độ rộng xung của kênh đó trong phần đang nhìn.
 *   Nút        : BACK, RUN, STOP, - , +, VỪA, TRIGGER, CÀI ĐẶT (tần số, độ sâu, % trước kích, nguồn THẬT / DEMO),
 *                GIẢI MÃ (2 bộ: I2C / UART / SPI / SPMI), FILE (lưu .lab + .vcd, mở lại .lab).
 * Thu và giải mã chạy ở task nền nhân 0; giao diện chỉ đọc dữ liệu khi đã thu xong.
 */
#pragma once

#include "lvgl.h"

namespace HCCLogicView {

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user);

}  // namespace HCCLogicView
