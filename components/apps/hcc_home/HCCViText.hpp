#pragma once

#include "lvgl.h"

// Bộ gõ tiếng Việt (Telex) cho lv_keyboard + lv_textarea, và tiện ích tạo
// bàn phím to, dễ bấm. Gõ kiểu Telex: aa->â, aw->ă, ow->ơ, uw->ư, dd->đ,
// và dấu s/f/r/x/j (sắc/huyền/hỏi/ngã/nặng), z để xoá dấu.
class HCCViText {
public:
    // Tạo bàn phím lớn gắn vào textarea + áp giao diện/bố cục iOS + bật gõ tiếng Việt.
    static lv_obj_t *createKeyboard(lv_obj_t *parent, lv_obj_t *textarea);

    // Áp bố cục + giao diện iPhone/iPad (map QWERTY, handler riêng, style, popover,
    // Telex) lên MỘT bàn phím lv_keyboard đã tạo sẵn (vd bàn phím của SquareLine).
    // Không đổi size/vị trí/font -> người gọi tự đặt trước khi gọi.
    static void applyIosStyle(lv_obj_t *keyboard);

    // Bật gõ tiếng Việt cho một bàn phím đã có (gắn thêm xử lý Telex).
    static void enableTelex(lv_obj_t *keyboard);

    // Đánh dấu 1 textarea là ô nhập tiếng Việt (Telex chỉ áp cho ô đã đánh dấu,
    // tránh làm hỏng ô ASCII như MODEL/CODE HEX/SOURCE).
    static void markVietnamese(lv_obj_t *textarea);
};
