/*
 * XiaozhiPng: giải mã PNG (libpng) thẳng ra ảnh LVGL LV_IMG_CF_TRUE_COLOR_ALPHA (RGB565 little-endian + alpha,
 * 3 byte/điểm, LV_COLOR_DEPTH 16, không swap), đã co giãn kiểu "phủ kín" (cover) vào khung dst_w x dst_h:
 * giữ đúng tỉ lệ, cắt đều hai bên, theo chiều dọc giữ phần trên (để không mất đầu nhân vật).
 *
 * Thu nhỏ: đọc từng dòng và lấy trung bình vùng (có tính alpha), không giữ cả ảnh gốc trong RAM.
 * Ảnh xen kẽ (interlaced) hoặc nhỏ hơn khung (phải phóng to): đọc cả ảnh vào PSRAM rồi lấy mẫu.
 * Mọi lỗi của libpng được bắt bằng setjmp -> trả NULL, không crash.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Kích thước ảnh gốc (chỉ đọc header). false nếu không phải PNG / không mở được.
bool xz_png_size(const char *path, int *w, int *h);

// Trả bộ đệm dst_w * dst_h * 3 byte (cấp phát trong PSRAM, giải phóng bằng xz_png_free), NULL nếu lỗi.
// *src_w / *src_h (có thể NULL) nhận kích thước ảnh gốc. err nhận mô tả lỗi.
uint8_t *xz_png_load_cover(const char *path, int dst_w, int dst_h, int *src_w, int *src_h, char *err, size_t err_len);

// Như trên nhưng đọc PNG từ bộ nhớ (vd tải qua mạng).
bool xz_png_size_mem(const uint8_t *data, size_t len, int *w, int *h);
uint8_t *xz_png_load_cover_mem(const uint8_t *data, size_t len, int dst_w, int dst_h, int *src_w, int *src_h,
                               char *err, size_t err_len);

void xz_png_free(void *buf);

#ifdef __cplusplus
}
#endif
