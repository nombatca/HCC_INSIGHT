/*
 * HCCRffeImage: ảnh testpoint RFFE trên thẻ SD, xem theo model.
 *
 *   /sdcard/rffe/img/<MODEL>/ chứa ảnh .png, .jpg hoặc .jpeg
 *   <MODEL> là tên model đang hiện trên màn RFFE (vd "IP 15 PROMAX"); so khớp bỏ qua hoa / thường, dấu cách
 *   và '_' (thư mục "IP_15_PROMAX" hay "ip15promax" đều được).
 *   Ảnh có tên chứa tên đường RFFE đang chọn (vd "SDR1_RFFE4_DATA.jpg") được xếp lên đầu.
 *
 * PNG: giải mã bằng XiaozhiPng (libpng, đọc từng dòng, không giữ cả ảnh gốc).
 * JPG: bộ giải mã JPEG phần cứng của ESP32-P4 (ảnh baseline; tối đa ~6 MP do cần bộ đệm RGB565 cả ảnh).
 * Ảnh được co (giữ tỉ lệ, không cắt) vừa khung max_w x max_h; giải mã chạy ở task nền.
 */
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "lvgl.h"

namespace HCCRffeImage {

// Danh sách đường dẫn ảnh của model (ảnh khớp đường RFFE lên đầu). Trả false + err nếu không có thẻ / thư mục.
bool list(const char *model, const char *line, std::vector<std::string> &out, std::string &folder, char *err,
          size_t err_size);

// Bắt đầu giải mã ở task nền, co vừa max_w x max_h. Trả false nếu đang bận.
bool startLoad(const char *path, int max_w, int max_h);
bool busy(void);
// Tăng mỗi lần giải mã xong (thành công hay lỗi).
uint32_t serial(void);

// Lấy kết quả lần giải mã gần nhất (chỉ gọi khi !busy()). Trả false + err nếu lỗi.
// Ảnh đang hiện (current) vẫn hợp lệ trong lúc giải mã ảnh mới. Sau khi lv_img đã chuyển sang ảnh mới,
// gọi commit(): ảnh cũ được giải phóng, ảnh mới thành current.
bool result(const lv_img_dsc_t **dsc, int *src_w, int *src_h, char *err, size_t err_size);
void commit(void);
const lv_img_dsc_t *current(void);

// Giải phóng mọi bộ đệm ảnh (gọi khi đóng màn xem ảnh, sau khi không còn lv_img nào dùng chúng).
void release(void);

}  // namespace HCCRffeImage
