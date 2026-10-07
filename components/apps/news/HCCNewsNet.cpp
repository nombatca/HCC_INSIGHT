#include "HCCNewsNet.hpp"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "driver/jpeg_decode.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "xiaozhi/XiaozhiPng.h"

namespace HCCNewsNet {

static const char *TAG = "HCCNewsNet";
static const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
static constexpr size_t JPG_MAX_OUT = 12 * 1024 * 1024;  // RGB565 cả ảnh (~6 MP)

void Buffer::release()
{
    heap_caps_free(data);
    data = nullptr;
    len = 0;
}

bool wifiConnected(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr || !esp_netif_is_netif_up(netif)) {
        return false;
    }
    esp_netif_ip_info_t ip = {};
    return esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

namespace {

struct HeaderCtx {
    bool gzip = false;
    std::string content_type;
};

esp_err_t onHttpEvent(esp_http_client_event_t *e)
{
    auto *ctx = static_cast<HeaderCtx *>(e->user_data);
    if (ctx && e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        if (strcasecmp(e->header_key, "Content-Encoding") == 0 && strcasestr(e->header_value, "gzip")) {
            ctx->gzip = true;
        } else if (strcasecmp(e->header_key, "Content-Type") == 0) {
            ctx->content_type = e->header_value;
        }
    }
    return ESP_OK;
}

}  // namespace

bool httpGet(const std::string &url, size_t max_bytes, Buffer &out, char *err, size_t err_size)
{
    out.release();
    out.final_url = url;
    out.content_type.clear();
    if (!wifiConnected()) {
        snprintf(err, err_size, "Chưa kết nối Wi-Fi");
        return false;
    }
    HeaderCtx hdr;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.user_agent = USER_AGENT;
    cfg.max_redirection_count = 5;
    cfg.event_handler = onHttpEvent;
    cfg.user_data = &hdr;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        snprintf(err, err_size, "Không tạo được kết nối");
        return false;
    }
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Accept-Language", "vi-VN,vi;q=0.9");

    bool ok = false;
    int status = 0;
    for (int hop = 0; hop < 6; ++hop) {
        esp_err_t r = esp_http_client_open(client, 0);
        if (r != ESP_OK) {
            snprintf(err, err_size, "Không kết nối được (%s)", esp_err_to_name(r));
            break;
        }
        const int64_t clen = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_flush_response(client, nullptr);
            esp_http_client_close(client);
            if (esp_http_client_set_redirection(client) != ESP_OK) {
                snprintf(err, err_size, "Chuyển hướng lỗi");
                break;
            }
            hdr = HeaderCtx();
            continue;
        }
        if (status != 200) {
            snprintf(err, err_size, "Máy chủ trả mã %d", status);
            break;
        }
        if (hdr.gzip) {
            snprintf(err, err_size, "Máy chủ gửi dữ liệu nén gzip (chưa hỗ trợ)");
            break;
        }
        if (clen > 0 && (size_t)clen > max_bytes) {
            snprintf(err, err_size, "Dữ liệu quá lớn (%lld KB)", (long long)(clen / 1024));
            break;
        }
        size_t cap = clen > 0 ? (size_t)clen + 1 : 64 * 1024;
        out.data = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out.data == nullptr) {
            snprintf(err, err_size, "Hết bộ nhớ");
            break;
        }
        bool fail = false;
        while (true) {
            if (out.len + 1 >= cap) {
                if (cap >= max_bytes + 1) {
                    snprintf(err, err_size, "Dữ liệu quá lớn (> %u KB)", (unsigned)(max_bytes / 1024));
                    fail = true;
                    break;
                }
                size_t ncap = cap * 2 > max_bytes + 1 ? max_bytes + 1 : cap * 2;
                uint8_t *n = (uint8_t *)heap_caps_realloc(out.data, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (n == nullptr) {
                    snprintf(err, err_size, "Hết bộ nhớ");
                    fail = true;
                    break;
                }
                out.data = n;
                cap = ncap;
            }
            const int got = esp_http_client_read(client, (char *)out.data + out.len, (int)(cap - 1 - out.len));
            if (got < 0) {
                snprintf(err, err_size, "Mất kết nối khi tải");
                fail = true;
                break;
            }
            if (got == 0) {
                if (esp_http_client_is_complete_data_received(client) || clen <= 0) {
                    break;
                }
                snprintf(err, err_size, "Tải không đủ dữ liệu");
                fail = true;
                break;
            }
            out.len += (size_t)got;
        }
        if (!fail) {
            out.data[out.len] = 0;  // để phân tích như chuỗi
            char u[512];
            if (esp_http_client_get_url(client, u, sizeof(u)) == ESP_OK) {
                out.final_url = u;
            }
            out.content_type = hdr.content_type;
            ok = true;
        }
        break;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok) {
        out.release();
        ESP_LOGW(TAG, "GET %s: %s", url.c_str(), err);
    }
    return ok;
}

// ==================== Ảnh ====================

namespace {

void setErr(char *err, size_t n, const char *msg)
{
    if (err && n) {
        snprintf(err, n, "%s", msg);
    }
}

// JPEG progressive (SOF2) - bộ giải mã phần cứng không hỗ trợ.
bool jpegProgressive(const uint8_t *d, size_t len)
{
    size_t i = 2;
    while (i + 4 <= len) {
        if (d[i] != 0xFF) {
            return false;
        }
        const uint8_t m = d[i + 1];
        if (m == 0xFF) {
            ++i;
            continue;
        }
        if (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE) {
            return true;
        }
        if (m == 0xC0 || m == 0xC1 || m == 0xDA) {
            return false;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            i += 2;
            continue;
        }
        i += 2 + (((size_t)d[i + 2] << 8) | d[i + 3]);
    }
    return false;
}

void targetSize(int sw, int sh, int w, int h, bool cover, int *dw, int *dh)
{
    if (cover) {
        *dw = w;
        *dh = h;
        return;
    }
    double s = (double)w / sw;
    if ((double)h / sh < s) {
        s = (double)h / sh;
    }
    if (s > 1.0) {
        s = 1.0;  // không phóng to ảnh nhỏ
    }
    *dw = (int)(sw * s + 0.5);
    *dh = (int)(sh * s + 0.5);
    if (*dw < 1) {
        *dw = 1;
    }
    if (*dh < 1) {
        *dh = 1;
    }
}

Image *makeImage(uint8_t *buf, int dw, int dh, bool alpha, int sw, int sh)
{
    Image *img = (Image *)heap_caps_calloc(1, sizeof(Image), MALLOC_CAP_8BIT);
    if (img == nullptr) {
        return nullptr;
    }
    img->buf = buf;
    img->src_w = sw;
    img->src_h = sh;
    img->dsc.header.cf = alpha ? LV_IMG_CF_TRUE_COLOR_ALPHA : LV_IMG_CF_TRUE_COLOR;
    img->dsc.header.w = dw;
    img->dsc.header.h = dh;
    img->dsc.data_size = (uint32_t)dw * dh * (alpha ? LV_IMG_PX_SIZE_ALPHA_BYTE : 2);
    img->dsc.data = buf;
    return img;
}

Image *decodeJpeg(const uint8_t *data, size_t len, int w, int h, bool cover, char *err, size_t en)
{
    if (jpegProgressive(data, len)) {
        setErr(err, en, "JPEG progressive (chưa hỗ trợ)");
        return nullptr;
    }
    jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t in_cap = 0;
    uint8_t *in = (uint8_t *)jpeg_alloc_decoder_mem((len + 15) & ~(size_t)15, &in_cfg, &in_cap);
    if (in == nullptr) {
        setErr(err, en, "Hết bộ nhớ");
        return nullptr;
    }
    memcpy(in, data, len);
    memset(in + len, 0, in_cap - len);
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(in, len, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        heap_caps_free(in);
        setErr(err, en, "JPEG không hợp lệ");
        return nullptr;
    }
    const uint32_t aw = (info.width + 15) & ~15u;
    const uint32_t ah = (info.height + 15) & ~15u;
    const size_t out_need = (size_t)aw * ah * 2;
    if (out_need > JPG_MAX_OUT) {
        heap_caps_free(in);
        setErr(err, en, "Ảnh quá lớn");
        return nullptr;
    }
    size_t out_cap = 0;
    uint8_t *raw = (uint8_t *)jpeg_alloc_decoder_mem(out_need, &out_cfg, &out_cap);
    if (raw == nullptr) {
        heap_caps_free(in);
        setErr(err, en, "Hết bộ nhớ");
        return nullptr;
    }
    jpeg_decoder_handle_t dec = nullptr;
    jpeg_decode_engine_cfg_t eng = {.intr_priority = 0, .timeout_ms = 3000};
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,  // đúng màu LVGL RGB565 (như trình xem ảnh RFFE)
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t r = jpeg_new_decoder_engine(&eng, &dec);
    if (r == ESP_OK) {
        r = jpeg_decoder_process(dec, &cfg, in, (len + 15) & ~(size_t)15, raw, out_cap, &out_size);
        jpeg_del_decoder_engine(dec);
    }
    heap_caps_free(in);
    if (r != ESP_OK) {
        heap_caps_free(raw);
        setErr(err, en, "Giải mã JPEG lỗi");
        return nullptr;
    }
    uint32_t stride = aw;
    if (out_size >= (size_t)info.width * info.height * 2 && out_size % (2 * info.height) == 0) {
        const uint32_t s = out_size / (2 * info.height);
        if (s >= info.width && s <= aw) {
            stride = s;
        }
    }
    const int sw = (int)info.width, sh = (int)info.height;
    int dw, dh;
    targetSize(sw, sh, w, h, cover, &dw, &dh);
    // Vùng nguồn: cover cắt đều hai bên (hoặc trên dưới) cho đúng tỉ lệ khung.
    int cx = 0, cy = 0, cw = sw, ch = sh;
    if (cover) {
        if ((int64_t)sw * dh > (int64_t)sh * dw) {
            cw = (int)((int64_t)sh * dw / dh);
            cx = (sw - cw) / 2;
        } else {
            ch = (int)((int64_t)sw * dh / dw);
            cy = (sh - ch) / 4;  // giữ phần trên nhiều hơn (mặt người, tiêu đề)
        }
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
    }
    uint16_t *dst = (uint16_t *)heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst == nullptr) {
        heap_caps_free(raw);
        setErr(err, en, "Hết bộ nhớ");
        return nullptr;
    }
    const uint16_t *src = (const uint16_t *)raw;
    for (int y = 0; y < dh; ++y) {
        uint32_t y0 = cy + (uint64_t)y * ch / dh, y1 = cy + (uint64_t)(y + 1) * ch / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; ++x) {
            uint32_t x0 = cx + (uint64_t)x * cw / dw, x1 = cx + (uint64_t)(x + 1) * cw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r5 = 0, g6 = 0, b5 = 0, n = 0;
            const uint32_t ys = (y1 - y0 > 4) ? (y1 - y0) / 4 : 1;
            const uint32_t xs = (x1 - x0 > 4) ? (x1 - x0) / 4 : 1;
            for (uint32_t yy = y0; yy < y1 && yy < (uint32_t)sh; yy += ys) {
                const uint16_t *row = src + (size_t)yy * stride;
                for (uint32_t xx = x0; xx < x1 && xx < (uint32_t)sw; xx += xs) {
                    const uint16_t p = row[xx];
                    r5 += p >> 11;
                    g6 += (p >> 5) & 0x3F;
                    b5 += p & 0x1F;
                    ++n;
                }
            }
            dst[(size_t)y * dw + x] = n ? (uint16_t)(((r5 / n) << 11) | ((g6 / n) << 5) | (b5 / n)) : 0;
        }
    }
    heap_caps_free(raw);
    Image *img = makeImage((uint8_t *)dst, dw, dh, false, sw, sh);
    if (img == nullptr) {
        heap_caps_free(dst);
        setErr(err, en, "Hết bộ nhớ");
    }
    return img;
}

Image *decodePng(const uint8_t *data, size_t len, int w, int h, bool cover, char *err, size_t en)
{
    int sw = 0, sh = 0;
    if (!xz_png_size_mem(data, len, &sw, &sh)) {
        setErr(err, en, "PNG không hợp lệ");
        return nullptr;
    }
    int dw, dh;
    targetSize(sw, sh, w, h, cover, &dw, &dh);
    char e[64] = "";
    uint8_t *buf = xz_png_load_cover_mem(data, len, dw, dh, nullptr, nullptr, e, sizeof(e));
    if (buf == nullptr) {
        snprintf(err, en, "PNG lỗi: %s", e);
        return nullptr;
    }
    Image *img = makeImage(buf, dw, dh, true, sw, sh);
    if (img == nullptr) {
        xz_png_free(buf);
        setErr(err, en, "Hết bộ nhớ");
    }
    return img;
}

}  // namespace

Image *decodeImage(const uint8_t *data, size_t len, int w, int h, bool cover, char *err, size_t err_size)
{
    if (data == nullptr || len < 16 || w <= 0 || h <= 0) {
        setErr(err, err_size, "Ảnh rỗng");
        return nullptr;
    }
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return decodeJpeg(data, len, w, h, cover, err, err_size);
    }
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return decodePng(data, len, w, h, cover, err, err_size);
    }
    if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        setErr(err, err_size, "Ảnh WEBP (chưa hỗ trợ)");
    } else if (memcmp(data, "GIF8", 4) == 0) {
        setErr(err, err_size, "Ảnh GIF (chưa hỗ trợ)");
    } else {
        setErr(err, err_size, "Định dạng ảnh chưa hỗ trợ");
    }
    return nullptr;
}

void freeImage(Image *img)
{
    if (img == nullptr) {
        return;
    }
    if (img->dsc.header.cf == LV_IMG_CF_TRUE_COLOR_ALPHA) {
        xz_png_free(img->buf);
    } else {
        heap_caps_free(img->buf);
    }
    heap_caps_free(img);
}

}  // namespace HCCNewsNet
