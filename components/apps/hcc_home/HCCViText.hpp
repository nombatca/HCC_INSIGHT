#pragma once

#include "lvgl.h"

// Bộ gõ tiếng Việt (Telex) cho lv_keyboard + lv_textarea, và tiện ích tạo
// bàn phím to, dễ bấm. Gõ kiểu Telex: aa->â, aw->ă, ow->ơ, uw->ư, dd->đ,
// và dấu s/f/r/x/j (sắc/huyền/hỏi/ngã/nặng), z để xoá dấu.
class HCCViText {
public:
    // Tạo bàn phím lớn (font 22, cao ~320px) gắn vào textarea + bật gõ tiếng Việt.
    static lv_obj_t *createKeyboard(lv_obj_t *parent, lv_obj_t *textarea);

    // Bật gõ tiếng Việt cho một bàn phím đã có (gắn thêm xử lý Telex).
    static void enableTelex(lv_obj_t *keyboard);

    // Đánh dấu 1 textarea là ô nhập tiếng Việt (Telex chỉ áp cho ô đã đánh dấu,
    // tránh làm hỏng ô ASCII như MODEL/CODE HEX/SOURCE).
    static void markVietnamese(lv_obj_t *textarea);
};
