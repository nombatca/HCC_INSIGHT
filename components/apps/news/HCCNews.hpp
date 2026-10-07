/*
 * HCCNews: app "Đọc báo" (thay trình duyệt cũ).
 *
 * Danh sách tin lấy từ RSS của các báo (VnExpress, Tuổi Trẻ, Thanh Niên, Dân Trí) theo chuyên mục, có ảnh nhỏ.
 * Bấm một tin: tải trang bài, tách đoạn văn / tiêu đề phụ / ảnh / chú thích và hiển thị kèm ảnh.
 * Thêm nguồn riêng: /sdcard/news/feeds.txt, mỗi dòng "Tên|https://.../rss" (dòng '#' là ghi chú).
 * Ảnh: JPEG baseline và PNG (WEBP / GIF / JPEG progressive chưa hỗ trợ -> hiện dòng thông báo thay ảnh).
 */
#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

class HCCNews : public ESP_Brookesia_PhoneApp {
public:
    HCCNews();
    ~HCCNews() override;

    bool init(void) override;
    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
};
