/*
 * HCCSpmiLogView: màn "BẮT DỮ LIỆU SPMI" - nghe thụ động bus SPMI trong 10-90 s (HCCSpmiLog), hiện danh sách
 * giao dịch và thống kê theo (SID, địa chỉ) bằng giá trị thô, lưu CSV ra thẻ SD.
 */
#pragma once

#include "lvgl.h"

namespace HCCSpmiLogView {

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user);

}  // namespace HCCSpmiLogView
