/*
 * HCCSpmiAnalyzerView: màn "PMIC ANALYZER" - so sánh capture main tốt (A) với main lỗi (B).
 *   TRÌNH TỰ       : dóng hàng lệnh READ / WRITE theo thời gian (HCCSpmiAnalyzer::compare), đánh dấu GIỐNG /
 *                    KHÁC DỮ LIỆU / THIẾU Ở B / THỪA Ở B / LỖI GIAO TIẾP; CHỈ KHÁC BIỆT = lọc bỏ dòng GIỐNG.
 *   THANH GHI CUỐI : giá trị ghi cuối cùng theo (SID, địa chỉ) ở A và B.
 *   RAIL (MAPPING) : chỉ khi có /sdcard/spmi/pmic_map.csv do người dùng cung cấp kèm nguồn đã xác minh; kết quả
 *                    là "lệnh ghi khớp / khác", KHÔNG kết luận rail đã có điện áp.
 *   BASEBAND / BOARDVIEW: nút liên kết (HCCSpmiLink) - khoá cho đến khi module đích đăng ký.
 * Nguồn A / B: capture hiện tại của SPMI MONITOR hoặc file .spmc / .csv trong /sdcard/spmi.
 * So sánh chạy ở task nền (nhân 0); giao diện chỉ đọc kết quả khi xong.
 */
#pragma once

#include "lvgl.h"

namespace HCCSpmiAnalyzerView {

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user);

}  // namespace HCCSpmiAnalyzerView
