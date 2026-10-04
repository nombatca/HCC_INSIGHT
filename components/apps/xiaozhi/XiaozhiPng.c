/*
 * XiaozhiPng: xem XiaozhiPng.h. Viết bằng C (setjmp/longjmp của libpng không an toàn với đối tượng C++),
 * biên dịch được cả trên máy tính để kiểm thử (test/xiaozhi/png_test.c).
 */
#include "XiaozhiPng.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_alloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void big_free(void *p)
{
    heap_caps_free(p);
}
#else
static void *big_alloc(size_t n)
{
    return malloc(n);
}
static void big_free(void *p)
{
    free(p);
}
#endif

#define XZ_PNG_MAX_DIM     8192
#define XZ_PNG_MAX_FULL    (24u * 1024u * 1024u)  // giới hạn đọc cả ảnh (interlaced / phóng to)

typedef struct {
    char *err;
    size_t err_len;
} err_ctx_t;

static void on_png_error(png_structp png, png_const_charp msg)
{
    err_ctx_t *ctx = (err_ctx_t *)png_get_error_ptr(png);
    if (ctx != NULL && ctx->err != NULL && ctx->err_len > 0) {
        snprintf(ctx->err, ctx->err_len, "libpng: %s", msg);
    }
    png_longjmp(png, 1);
}

static void on_png_warning(png_structp png, png_const_charp msg)
{
    (void)png;
    (void)msg;
}

static void set_err(char *err, size_t len, const char *msg)
{
    if (err != NULL && len > 0) {
        snprintf(err, len, "%s", msg);
    }
}

void xz_png_free(void *buf)
{
    if (buf != NULL) {
        big_free(buf);
    }
}

static FILE *open_png(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    png_byte sig[8];
    if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        fclose(f);
        return NULL;
    }
    return f;
}

bool xz_png_size(const char *path, int *w, int *h)
{
    FILE *f = open_png(path);
    if (f == NULL) {
        return false;
    }
    // IHDR luôn đứng ngay sau chữ ký: độ dài(4) "IHDR"(4) rộng(4) cao(4), big-endian.
    uint8_t b[16];
    bool ok = fread(b, 1, 16, f) == 16 && memcmp(b + 4, "IHDR", 4) == 0;
    fclose(f);
    if (!ok) {
        return false;
    }
    *w = (int)(((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) | ((uint32_t)b[10] << 8) | b[11]);
    *h = (int)(((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) | b[15]);
    return *w > 0 && *h > 0;
}

static inline void put_px(uint8_t *out, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    const uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    out[i * 3] = (uint8_t)(c & 0xFF);
    out[i * 3 + 1] = (uint8_t)(c >> 8);
    out[i * 3 + 2] = (uint8_t)a;
}

/* ---- thu nhỏ theo vùng, nhận từng dòng nguồn theo thứ tự ---- */

typedef struct {
    int src_w, src_h, dst_w, dst_h;
    double scale;          // dst / src (<= 1)
    double x0, y0;         // góc trên trái vùng cắt trong ảnh nguồn
    int16_t *col_map;      // cột nguồn -> cột đích (-1: bị cắt)
    uint32_t *acc;         // dst_w * 5: tổng r*a, g*a, b*a, a, số điểm
    int cur_dy;
    uint8_t *out;
} down_t;

static void down_flush(down_t *d)
{
    if (d->cur_dy < 0 || d->cur_dy >= d->dst_h) {
        return;
    }
    for (int x = 0; x < d->dst_w; ++x) {
        uint32_t *a = &d->acc[x * 5];
        uint32_t r = 0, g = 0, b = 0, al = 0;
        if (a[4] > 0) {
            al = a[3] / a[4];
            if (a[3] > 0) {
                r = a[0] / a[3];
                g = a[1] / a[3];
                b = a[2] / a[3];
            }
        }
        put_px(d->out, d->cur_dy * d->dst_w + x, r, g, b, al);
    }
    memset(d->acc, 0, sizeof(uint32_t) * 5 * (size_t)d->dst_w);
}

static void down_row(down_t *d, int sy, const uint8_t *rgba)
{
    const double ry = sy - d->y0;
    if (ry < 0) {
        return;
    }
    const int dy = (int)floor(ry * d->scale);
    if (dy >= d->dst_h) {
        return;
    }
    if (dy != d->cur_dy) {
        down_flush(d);
        d->cur_dy = dy;
    }
    for (int sx = 0; sx < d->src_w; ++sx) {
        const int dx = d->col_map[sx];
        if (dx < 0) {
            continue;
        }
        const uint8_t *p = rgba + sx * 4;
        uint32_t *a = &d->acc[dx * 5];
        a[0] += (uint32_t)p[0] * p[3];
        a[1] += (uint32_t)p[1] * p[3];
        a[2] += (uint32_t)p[2] * p[3];
        a[3] += p[3];
        a[4] += 1;
    }
}

/* ---- phóng to (ảnh nhỏ hơn khung): nội suy song tuyến từ ảnh đầy đủ ---- */

static void upscale_bilinear(const uint8_t *src, int sw, int sh, double scale, double x0, double y0, uint8_t *out,
                             int dw, int dh)
{
    for (int dy = 0; dy < dh; ++dy) {
        double fy = y0 + (dy + 0.5) / scale - 0.5;
        if (fy < 0) fy = 0;
        if (fy > sh - 1) fy = sh - 1;
        const int y1 = (int)fy;
        const int y2 = y1 + 1 < sh ? y1 + 1 : y1;
        const double ty = fy - y1;
        for (int dx = 0; dx < dw; ++dx) {
            double fx = x0 + (dx + 0.5) / scale - 0.5;
            if (fx < 0) fx = 0;
            if (fx > sw - 1) fx = sw - 1;
            const int x1 = (int)fx;
            const int x2 = x1 + 1 < sw ? x1 + 1 : x1;
            const double tx = fx - x1;
            double c[4];
            for (int k = 0; k < 4; ++k) {
                const double p11 = src[((size_t)y1 * sw + x1) * 4 + k];
                const double p21 = src[((size_t)y1 * sw + x2) * 4 + k];
                const double p12 = src[((size_t)y2 * sw + x1) * 4 + k];
                const double p22 = src[((size_t)y2 * sw + x2) * 4 + k];
                c[k] = (p11 * (1 - tx) + p21 * tx) * (1 - ty) + (p12 * (1 - tx) + p22 * tx) * ty;
            }
            put_px(out, dy * dw + dx, (uint32_t)(c[0] + 0.5), (uint32_t)(c[1] + 0.5), (uint32_t)(c[2] + 0.5),
                   (uint32_t)(c[3] + 0.5));
        }
    }
}

uint8_t *xz_png_load_cover(const char *path, int dst_w, int dst_h, int *src_w, int *src_h, char *err, size_t err_len)
{
    if (dst_w <= 0 || dst_h <= 0) {
        set_err(err, err_len, "khung đích không hợp lệ");
        return NULL;
    }
    FILE *f = open_png(path);
    if (f == NULL) {
        set_err(err, err_len, "không mở được hoặc không phải PNG");
        return NULL;
    }
    err_ctx_t ectx = {err, err_len};
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &ectx, on_png_error, on_png_warning);
    png_infop info = png != NULL ? png_create_info_struct(png) : NULL;
    if (png == NULL || info == NULL) {
        if (png != NULL) {
            png_destroy_read_struct(&png, NULL, NULL);
        }
        fclose(f);
        set_err(err, err_len, "thiếu bộ nhớ cho libpng");
        return NULL;
    }

    // Biến dùng sau longjmp phải là volatile.
    uint8_t *volatile out = NULL;
    uint8_t *volatile row = NULL;
    uint8_t *volatile full = NULL;
    int16_t *volatile col_map = NULL;
    uint32_t *volatile acc = NULL;
    png_bytep *volatile rows = NULL;

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        xz_png_free(out);
        xz_png_free(row);
        xz_png_free(full);
        xz_png_free(col_map);
        xz_png_free(acc);
        xz_png_free(rows);
        return NULL;
    }

    png_init_io(png, f);
    png_set_sig_bytes(png, 8);
    png_set_user_limits(png, XZ_PNG_MAX_DIM, XZ_PNG_MAX_DIM);
    png_read_info(png, info);

    const int w = (int)png_get_image_width(png, info);
    const int h = (int)png_get_image_height(png, info);
    const int color = png_get_color_type(png, info);
    const int depth = png_get_bit_depth(png, info);
    if (src_w != NULL) *src_w = w;
    if (src_h != NULL) *src_h = h;

    // Mọi định dạng -> RGBA 8 bit.
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const size_t rowbytes = png_get_rowbytes(png, info);
    if (rowbytes != (size_t)w * 4) {
        set_err(err, err_len, "định dạng điểm ảnh không hỗ trợ");
        png_longjmp(png, 1);
    }

    // Phủ kín: tỉ lệ lớn hơn trong 2 chiều; cắt giữa theo chiều ngang, giữ phần trên theo chiều dọc.
    const double sx = (double)dst_w / w;
    const double sy = (double)dst_h / h;
    const double scale = sx > sy ? sx : sy;
    const double crop_w = dst_w / scale;
    const double x0 = (w - crop_w) / 2.0;
    const double y0 = 0.0;

    out = (uint8_t *)big_alloc((size_t)dst_w * dst_h * 3);
    if (out == NULL) {
        set_err(err, err_len, "không đủ PSRAM cho khung ảnh");
        png_longjmp(png, 1);
    }

    const bool need_full = passes > 1 || scale > 1.0;
    if (need_full) {
        const size_t total = rowbytes * (size_t)h;
        if (total > XZ_PNG_MAX_FULL) {
            set_err(err, err_len, "ảnh xen kẽ / quá nhỏ cần đọc cả ảnh nhưng quá lớn");
            png_longjmp(png, 1);
        }
        full = (uint8_t *)big_alloc(total);
        rows = (png_bytep *)big_alloc(sizeof(png_bytep) * (size_t)h);
        if (full == NULL || rows == NULL) {
            set_err(err, err_len, "không đủ bộ nhớ để đọc cả ảnh");
            png_longjmp(png, 1);
        }
        for (int y = 0; y < h; ++y) {
            rows[y] = full + rowbytes * (size_t)y;
        }
        png_read_image(png, rows);
    }

    if (scale > 1.0) {
        upscale_bilinear(full, w, h, scale, x0, y0, out, dst_w, dst_h);
    } else {
        col_map = (int16_t *)big_alloc(sizeof(int16_t) * (size_t)w);
        acc = (uint32_t *)big_alloc(sizeof(uint32_t) * 5 * (size_t)dst_w);
        if (!need_full) {
            row = (uint8_t *)big_alloc(rowbytes);
        }
        if (col_map == NULL || acc == NULL || (!need_full && row == NULL)) {
            set_err(err, err_len, "thiếu bộ nhớ đệm");
            png_longjmp(png, 1);
        }
        for (int x = 0; x < w; ++x) {
            const double rx = x - x0;
            int dx = rx < 0 ? -1 : (int)floor(rx * scale);
            col_map[x] = (int16_t)(dx >= 0 && dx < dst_w ? dx : -1);
        }
        memset(acc, 0, sizeof(uint32_t) * 5 * (size_t)dst_w);
        memset(out, 0, (size_t)dst_w * dst_h * 3);
        down_t d = {w, h, dst_w, dst_h, scale, x0, y0, col_map, acc, -1, out};
        for (int y = 0; y < h; ++y) {
            if (need_full) {
                down_row(&d, y, rows[y]);
            } else {
                png_read_row(png, row, NULL);
                down_row(&d, y, row);
            }
        }
        down_flush(&d);
    }

    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    xz_png_free(row);
    xz_png_free(full);
    xz_png_free(col_map);
    xz_png_free(acc);
    xz_png_free(rows);
    return out;
}
