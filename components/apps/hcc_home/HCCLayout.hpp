/*
 * HCCLayout: chiều cao vùng app NHÌN THẤY được trên màn 1024 x 600.
 *
 * Màn app (lv_scr_act) cao 600 nhưng Brookesia đặt nó dưới thanh trạng thái, nên phần dưới cùng tràn ra ngoài
 * màn hình; thêm nữa ~20 px sát mép dưới là vùng vuốt (thanh chỉ báo trắng của Brookesia) - nút đặt ở đó khó bấm.
 * usableHeight() = phần còn nằm trên màn (tính từ toạ độ thật của root) trừ lề vùng vuốt.
 */
#pragma once

#include "lvgl.h"

namespace HCCLayout {

constexpr lv_coord_t GESTURE_MARGIN = 24;

inline lv_coord_t usableHeight(lv_obj_t *root, lv_coord_t fallback = 500)
{
    lv_obj_update_layout(root);
    lv_area_t a;
    lv_obj_get_coords(root, &a);
    lv_coord_t h = lv_obj_get_height(root);
    const lv_coord_t screen_h = lv_disp_get_ver_res(lv_obj_get_disp(root));
    if (a.y1 >= 0 && a.y1 < screen_h && a.y1 + h > screen_h) {
        h = screen_h - a.y1;  // phần tràn khỏi mép dưới màn
    }
    h -= GESTURE_MARGIN;
    if (h < 360 || h > 800) {
        h = fallback;
    }
    return h;
}

}  // namespace HCCLayout
