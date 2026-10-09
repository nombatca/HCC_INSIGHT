/*
 * HCCSpmiView: màn "SPMI" - quét SID 0..15 trên bus SPMI (dùng chung chân SCLK / SDATA với RFFE).
 * Mỗi SID là một ô: xanh = có phản hồi (hiện thanh ghi 0x00 và 0x0100..0x0105 dạng thô), xám = không thấy.
 */
#pragma once

#include "lvgl.h"

namespace HCCSpmiView {

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user);

}  // namespace HCCSpmiView
