#include "HCCRffeImage.hpp"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/jpeg_decode.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "hcc_cores.h"
#include "HCCUsbDisk.hpp"
#include "xiaozhi/XiaozhiPng.h"

namespace HCCRffeImage {

static const char *TAG = "HCCRffeImg";
static const char *IMG_DIR = BSP_SD_MOUNT_POINT "/rffe/img";

static const size_t JPG_MAX_FILE = 8 * 1024 * 1024;
static const size_t JPG_MAX_OUT = 12 * 1024 * 1024;  // RGB565 cả ảnh (~6 MP)

static volatile bool s_busy = false;
static volatile uint32_t s_serial = 0;
static char s_path[256];
static int s_max_w = 0, s_max_h = 0;

// Kết quả (chỉ đọc khi !s_busy).
static uint8_t *s_buf = nullptr;
static bool s_buf_png = false;  // true: bộ đệm của XiaozhiPng (xz_png_free)
static lv_img_dsc_t s_dsc;
static int s_src_w = 0, s_src_h = 0;
static bool s_ok = false;
static char s_err[96] = "";

// ==================== Danh sách ảnh ====================

static bool sdReady(void)
{
    if (HCCUsbDisk::state() != HCCUsbDisk::State::Off) {
        return false;  // thẻ đang giao cho máy tính (USB DISK)
    }
    const esp_err_t ret = bsp_sdcard_mount();
    return ret == ESP_OK || ret == ESP_ERR_INVALID_STATE;
}

// Chỉ giữ chữ / số, viết hoa: "IP 15 PROMAX" == "IP_15_PROMAX" == "ip15promax".
static std::string norm(const char *s)
{
    std::string out;
    for (; *s; ++s) {
        if (isalnum(static_cast<unsigned char>(*s))) {
            out += static_cast<char>(toupper(static_cast<unsigned char>(*s)));
        }
    }
    return out;
}

static bool isImage(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
}

bool list(const char *model, const char *line, std::vector<std::string> &out, std::string &folder, char *err,
          size_t err_size)
{
    out.clear();
    folder = std::string(IMG_DIR) + "/" + model;
    if (!sdReady()) {
        snprintf(err, err_size, "Không đọc được thẻ nhớ (chưa cắm hoặc đang bật USB DISK)");
        return false;
    }
    DIR *root = opendir(IMG_DIR);
    if (root == nullptr) {
        snprintf(err, err_size, "Chưa có thư mục /rffe/img trên thẻ nhớ");
        return false;
    }
    const std::string want = norm(model);
    std::string dir_name;
    struct dirent *ent;
    while ((ent = readdir(root)) != nullptr) {
        if (ent->d_type == DT_DIR && ent->d_name[0] != '.' && norm(ent->d_name) == want) {
            dir_name = ent->d_name;
            break;
        }
    }
    closedir(root);
    if (dir_name.empty()) {
        snprintf(err, err_size, "Chưa có ảnh: tạo thư mục /rffe/img/%s/ và chép ảnh .jpg / .png vào", model);
        return false;
    }
    folder = std::string(IMG_DIR) + "/" + dir_name;
    DIR *dir = opendir(folder.c_str());
    if (dir == nullptr) {
        snprintf(err, err_size, "Không mở được %s", folder.c_str());
        return false;
    }
    std::vector<std::string> match, other;
    const std::string line_key = line ? norm(line) : std::string();
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type == DT_DIR || ent->d_name[0] == '.' || !isImage(ent->d_name)) {
            continue;
        }
        const std::string full = folder + "/" + ent->d_name;
        if (!line_key.empty() && norm(ent->d_name).find(line_key) != std::string::npos) {
            match.push_back(full);
        } else {
            other.push_back(full);
        }
    }
    closedir(dir);
    auto by_name = [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; };
    std::sort(match.begin(), match.end(), by_name);
    std::sort(other.begin(), other.end(), by_name);
    out = match;
    out.insert(out.end(), other.begin(), other.end());
    if (out.empty()) {
        snprintf(err, err_size, "Thư mục %s chưa có ảnh .jpg / .png", dir_name.c_str());
        return false;
    }
    err[0] = '\0';
    return true;
}

// ==================== Giải mã ====================

// Khung vừa ảnh (giữ tỉ lệ, không cắt, không phóng quá 1:1 trừ khi ảnh nhỏ hơn 1/2 khung).
static void fitSize(int sw, int sh, int max_w, int max_h, int *dw, int *dh)
{
    double k = std::min((double)max_w / sw, (double)max_h / sh);
    *dw = std::max(1, (int)(sw * k));
    *dh = std::max(1, (int)(sh * k));
}

static void freeBuf(void)
{
    if (s_buf) {
        if (s_buf_png) {
            xz_png_free(s_buf);
        } else {
            heap_caps_free(s_buf);
        }
    }
    s_buf = nullptr;
    s_ok = false;
}

static bool loadPng(const char *path, int max_w, int max_h)
{
    int sw = 0, sh = 0;
    if (!xz_png_size(path, &sw, &sh) || sw <= 0 || sh <= 0) {
        snprintf(s_err, sizeof(s_err), "Không đọc được PNG");
        return false;
    }
    int dw, dh;
    fitSize(sw, sh, max_w, max_h, &dw, &dh);
    // Khung cùng tỉ lệ với ảnh -> "phủ kín" của XiaozhiPng không cắt gì.
    uint8_t *buf = xz_png_load_cover(path, dw, dh, nullptr, nullptr, s_err, sizeof(s_err));
    if (buf == nullptr) {
        return false;
    }
    s_buf = buf;
    s_buf_png = true;
    s_src_w = sw;
    s_src_h = sh;
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_dsc.header.w = dw;
    s_dsc.header.h = dh;
    s_dsc.data_size = (uint32_t)dw * dh * LV_IMG_PX_SIZE_ALPHA_BYTE;
    s_dsc.data = buf;
    return true;
}

static bool loadJpg(const char *path, int max_w, int max_h)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        snprintf(s_err, sizeof(s_err), "Không mở được file");
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || (size_t)fsize > JPG_MAX_FILE) {
        fclose(f);
        snprintf(s_err, sizeof(s_err), "File JPG quá lớn (%ld KB, tối đa %u KB)", fsize / 1024,
                 (unsigned)(JPG_MAX_FILE / 1024));
        return false;
    }
    jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t in_cap = 0;
    uint8_t *in = (uint8_t *)jpeg_alloc_decoder_mem(((size_t)fsize + 15) & ~(size_t)15, &in_cfg, &in_cap);
    if (in == nullptr) {
        fclose(f);
        snprintf(s_err, sizeof(s_err), "Hết bộ nhớ đọc JPG");
        return false;
    }
    const size_t got = fread(in, 1, fsize, f);
    fclose(f);
    if (got != (size_t)fsize) {
        heap_caps_free(in);
        snprintf(s_err, sizeof(s_err), "Đọc file lỗi");
        return false;
    }
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(in, got, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        heap_caps_free(in);
        snprintf(s_err, sizeof(s_err), "JPG không hợp lệ (chỉ hỗ trợ JPG baseline)");
        return false;
    }
    // Bộ giải mã xuất theo khối MCU: rộng / cao làm tròn lên bội 16 (đủ cho mọi kiểu lấy mẫu).
    const uint32_t aw = (info.width + 15) & ~15u;
    const uint32_t ah = (info.height + 15) & ~15u;
    const size_t out_need = (size_t)aw * ah * 2;
    if (out_need > JPG_MAX_OUT) {
        heap_caps_free(in);
        snprintf(s_err, sizeof(s_err), "Ảnh JPG %ux%u quá lớn - thu nhỏ còn khoảng 3000 px hoặc dùng PNG",
                 (unsigned)info.width, (unsigned)info.height);
        return false;
    }
    size_t out_cap = 0;
    uint8_t *raw = (uint8_t *)jpeg_alloc_decoder_mem(out_need, &out_cfg, &out_cap);
    if (raw == nullptr) {
        heap_caps_free(in);
        snprintf(s_err, sizeof(s_err), "Hết bộ nhớ giải mã JPG (%ux%u)", (unsigned)info.width, (unsigned)info.height);
        return false;
    }
    jpeg_decoder_handle_t dec = nullptr;
    jpeg_decode_engine_cfg_t eng = {.intr_priority = 0, .timeout_ms = 3000};
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,  // giống trình phát video: đúng màu LVGL RGB565
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t ret = jpeg_new_decoder_engine(&eng, &dec);
    if (ret == ESP_OK) {
        ret = jpeg_decoder_process(dec, &cfg, in, (got + 15) & ~(size_t)15, raw, out_cap, &out_size);
        jpeg_del_decoder_engine(dec);
    }
    heap_caps_free(in);
    if (ret != ESP_OK) {
        heap_caps_free(raw);
        snprintf(s_err, sizeof(s_err), "Giải mã JPG lỗi (%s) - ảnh progressive? lưu lại dạng baseline / PNG",
                 esp_err_to_name(ret));
        return false;
    }
    // Hàng của ảnh giải mã: rộng = bội số MCU. Suy ra từ kích thước ra (an toàn nếu driver khác bội 16).
    uint32_t stride = aw;
    if (out_size >= (size_t)info.width * info.height * 2 && out_size % (2 * info.height) == 0) {
        const uint32_t s = out_size / (2 * info.height);
        if (s >= info.width && s <= aw) {
            stride = s;
        }
    }

    int dw, dh;
    fitSize(info.width, info.height, max_w, max_h, &dw, &dh);
    uint16_t *dst = (uint16_t *)heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst == nullptr) {
        heap_caps_free(raw);
        snprintf(s_err, sizeof(s_err), "Hết bộ nhớ");
        return false;
    }
    // Co bằng trung bình vùng (thu nhỏ) / lấy điểm gần nhất (phóng to).
    const uint16_t *src = (const uint16_t *)raw;
    const uint32_t sw = info.width, sh = info.height;
    for (int y = 0; y < dh; ++y) {
        uint32_t y0 = (uint64_t)y * sh / dh, y1 = (uint64_t)(y + 1) * sh / dh;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        for (int x = 0; x < dw; ++x) {
            uint32_t x0 = (uint64_t)x * sw / dw, x1 = (uint64_t)(x + 1) * sw / dw;
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            uint32_t r = 0, g = 0, b = 0, n = 0;
            const uint32_t ys = (y1 - y0 > 4) ? (y1 - y0) / 4 : 1;  // lấy mẫu thưa khi vùng lớn
            const uint32_t xs = (x1 - x0 > 4) ? (x1 - x0) / 4 : 1;
            for (uint32_t yy = y0; yy < y1 && yy < sh; yy += ys) {
                const uint16_t *row = src + (size_t)yy * stride;
                for (uint32_t xx = x0; xx < x1 && xx < sw; xx += xs) {
                    const uint16_t p = row[xx];
                    r += p >> 11;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                    ++n;
                }
            }
            dst[(size_t)y * dw + x] = n ? (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n)) : 0;
        }
    }
    heap_caps_free(raw);
    s_buf = (uint8_t *)dst;
    s_buf_png = false;
    s_src_w = sw;
    s_src_h = sh;
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.header.w = dw;
    s_dsc.header.h = dh;
    s_dsc.data_size = (uint32_t)dw * dh * 2;
    s_dsc.data = s_buf;
    return true;
}

static void loadTask(void *)
{
    s_err[0] = '\0';
    const char *dot = strrchr(s_path, '.');
    const bool png = dot && strcasecmp(dot, ".png") == 0;
    s_ok = png ? loadPng(s_path, s_max_w, s_max_h) : loadJpg(s_path, s_max_w, s_max_h);
    if (s_ok) {
        ESP_LOGI(TAG, "%s: %dx%d -> %dx%d", s_path, s_src_w, s_src_h, (int)s_dsc.header.w, (int)s_dsc.header.h);
    } else {
        ESP_LOGW(TAG, "%s: %s", s_path, s_err);
    }
    s_serial = s_serial + 1;
    s_busy = false;
    vTaskDelete(nullptr);
}

bool startLoad(const char *path, int max_w, int max_h)
{
    if (s_busy) {
        return false;
    }
    freeBuf();  // UI đã gỡ ảnh cũ khỏi lv_img trước khi gọi
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_max_w = max_w;
    s_max_h = max_h;
    s_busy = true;
    if (xTaskCreatePinnedToCore(loadTask, "hcc_rffe_img", 8192, nullptr, 4, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_busy = false;
        snprintf(s_err, sizeof(s_err), "Không tạo được task giải mã");
        return false;
    }
    return true;
}

bool busy(void)
{
    return s_busy;
}

uint32_t serial(void)
{
    return s_serial;
}

bool result(const lv_img_dsc_t **dsc, int *src_w, int *src_h, char *err, size_t err_size)
{
    if (!s_ok || s_buf == nullptr) {
        snprintf(err, err_size, "%s", s_err[0] ? s_err : "Chưa có ảnh");
        return false;
    }
    *dsc = &s_dsc;
    *src_w = s_src_w;
    *src_h = s_src_h;
    return true;
}

void release(void)
{
    if (!s_busy) {
        freeBuf();
    }
}

}  // namespace HCCRffeImage
