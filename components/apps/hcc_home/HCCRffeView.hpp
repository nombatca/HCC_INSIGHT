/*
 * Màn hình RFFE trong HCC INSIGHT:
 *   SELECT MODEL (thư mục /sdcard/rffe/<MODEL>) - RFFE DATA (file .csv trong thư mục model)
 *   khung kết quả: hàng thẻ USID + bảng thanh ghi của USID đang chọn (Đọc được / Chuẩn / Mask / Kết quả)
 *   nút SCAN và BACK.
 */
#pragma once

#include "lvgl.h"

namespace HCCRffeView {

// Dựng màn RFFE lên screen (đã được làm sạch). back_cb gắn cho nút BACK với user_data.
void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user_data);

}  // namespace HCCRffeView
