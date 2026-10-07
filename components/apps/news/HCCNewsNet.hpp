/*
 * HCCNewsNet: tải trang / ảnh qua HTTP(S) và giải mã ảnh cho app Đọc báo.
 *
 * httpGet: esp_http_client + bộ chứng chỉ gốc, tự theo chuyển hướng (tối đa 5 lần), dữ liệu nằm trong PSRAM.
 * decodeImage: JPEG baseline (bộ giải mã phần cứng ESP32-P4) hoặc PNG (libpng); WEBP / GIF / JPEG progressive
 * chưa hỗ trợ -> trả lỗi. Ảnh ra RGB565 (PNG: RGB565 + alpha) co theo khung:
 *   cover = true : phủ kín đúng w x h (cắt đều hai bên) - ảnh nhỏ trong danh sách tin
 *   cover = false: vừa trong w x h, giữ tỉ lệ, không phóng to - ảnh trong bài
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "lvgl.h"

namespace HCCNewsNet {

struct Buffer {
    uint8_t *data = nullptr;
    size_t len = 0;
    std::string final_url;     // URL sau chuyển hướng
    std::string content_type;
    void release();
};

bool wifiConnected(void);
bool httpGet(const std::string &url, size_t max_bytes, Buffer &out, char *err, size_t err_size);

struct Image {
    lv_img_dsc_t dsc;
    uint8_t *buf;
    int src_w;
    int src_h;
};

Image *decodeImage(const uint8_t *data, size_t len, int w, int h, bool cover, char *err, size_t err_size);
void freeImage(Image *img);

}  // namespace HCCNewsNet
