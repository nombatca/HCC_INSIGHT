#include "hcc_clock.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "driver/i2c_master.h"

static const char *TAG = "hcc_clock";

#define HCC_TZ "ICT-7"                       // Việt Nam UTC+7 (giống app_sntp.c)
#define HCC_VALID_AFTER ((time_t)1704067200) // 2024-01-01: giờ trước mốc này là chưa đặt
#define NVS_NS "hcc_clock"

static hcc_clock_source_t s_source = HCC_CLOCK_NONE;
// Giờ lúc khởi động (nếu còn từ RTC) + mốc esp_timer tương ứng: để đo độ trôi khi SNTP về.
// Không đo trong callback bằng time(): ESP-IDF đã settimeofday() trước khi gọi callback.
static time_t s_boot_unix = 0;
static int64_t s_boot_timer_us = 0;

// ---- DS3231 (I2C 0x68): thanh ghi 0x00..0x06 = giây..năm (BCD), 0x0E control, 0x0F status
// (bit7 OSF = dao động đã dừng -> giờ không tin được), 0x11..0x12 nhiệt độ. Giữ giờ UTC.
#define DS3231_ADDR 0x68
#define DS3231_TIMEOUT_MS 50
static i2c_master_dev_handle_t s_ds3231 = NULL;

static bool bcd_ok(uint8_t v, uint8_t max_dec)
{
    if ((v & 0x0F) > 9 || (v >> 4) > 9) return false;
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F)) <= max_dec;
}
static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// Ngày dân dụng (UTC) -> số ngày kể từ 1970-01-01 (thuật toán Howard Hinnant; newlib không có timegm).
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// Đọc giờ DS3231. *is_ds3231 = đọc được và byte nhiệt độ đúng dạng DS3231 (0x12 chỉ dùng bit 7:6).
// Trả false nếu không phải DS3231, BCD sai, hoặc OSF bật (mất nguồn pin / chưa từng đặt giờ).
static bool ds3231_read(time_t *out, float *temp_c, bool *osf, bool *is_ds3231)
{
    uint8_t r[19] = {0};
    const uint8_t reg = 0x00;
    *is_ds3231 = false;
    if (i2c_master_transmit_receive(s_ds3231, &reg, 1, r, sizeof(r), DS3231_TIMEOUT_MS) != ESP_OK) return false;
    *osf = (r[0x0F] & 0x80) != 0;
    *is_ds3231 = (r[0x12] & 0x3F) == 0 && (int8_t)r[0x11] >= -40 && (int8_t)r[0x11] <= 85;
    *temp_c = (float)(int8_t)r[0x11] + (float)(r[0x12] >> 6) * 0.25f;
    const uint8_t hour_reg = r[2];
    if ((hour_reg & 0x40) != 0) return false; // chế độ 12h: không phải giờ do HCC ghi
    if (!bcd_ok(r[0] & 0x7F, 59) || !bcd_ok(r[1] & 0x7F, 59) || !bcd_ok(hour_reg & 0x3F, 23) ||
        !bcd_ok(r[4] & 0x3F, 31) || !bcd_ok(r[5] & 0x1F, 12) || !bcd_ok(r[6], 99)) {
        return false;
    }
    const int year = 2000 + bcd2dec(r[6]) + ((r[5] & 0x80) ? 100 : 0);
    const unsigned month = bcd2dec(r[5] & 0x1F), day = bcd2dec(r[4] & 0x3F);
    if (month < 1 || day < 1) return false;
    const int64_t days = days_from_civil(year, month, day);
    *out = (time_t)(days * 86400 + bcd2dec(hour_reg & 0x3F) * 3600 + bcd2dec(r[1] & 0x7F) * 60 + bcd2dec(r[0] & 0x7F));
    return !*osf;
}

static bool ds3231_write(time_t t)
{
    struct tm u;
    gmtime_r(&t, &u);
    if (u.tm_year < 100 || u.tm_year > 199) return false; // DS3231: 2000..2099
    uint8_t buf[8] = {0x00,
                      dec2bcd((uint8_t)u.tm_sec), dec2bcd((uint8_t)u.tm_min), dec2bcd((uint8_t)u.tm_hour), // 24h
                      (uint8_t)(u.tm_wday + 1), dec2bcd((uint8_t)u.tm_mday), dec2bcd((uint8_t)(u.tm_mon + 1)),
                      dec2bcd((uint8_t)(u.tm_year - 100))};
    if (i2c_master_transmit(s_ds3231, buf, sizeof(buf), DS3231_TIMEOUT_MS) != ESP_OK) return false;
    // Xoá OSF (giữ nguyên các bit khác của status).
    const uint8_t sreg = 0x0F;
    uint8_t status = 0;
    if (i2c_master_transmit_receive(s_ds3231, &sreg, 1, &status, 1, DS3231_TIMEOUT_MS) != ESP_OK) return false;
    const uint8_t wr[2] = {0x0F, (uint8_t)(status & ~0x80)};
    return i2c_master_transmit(s_ds3231, wr, sizeof(wr), DS3231_TIMEOUT_MS) == ESP_OK;
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    default: return "OTHER";
    }
}

static void nvs_put_i64(const char *key, int64_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static int64_t nvs_get_i64_or(const char *key, int64_t fallback)
{
    nvs_handle_t h;
    int64_t value = fallback;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i64(h, key, &value) != ESP_OK) value = fallback;
        nvs_close(h);
    }
    return value;
}

void hcc_clock_init(void)
{
    // Múi giờ đặt ngay khi khởi động: giờ khôi phục từ pin RTC phải hiện giờ VN cả khi chưa có Wi-Fi.
    setenv("TZ", HCC_TZ, 1);
    tzset();

    const time_t now = time(NULL);
    const esp_reset_reason_t reason = esp_reset_reason();
    s_source = now >= HCC_VALID_AFTER ? HCC_CLOCK_RTC : HCC_CLOCK_NONE;
    s_boot_unix = now;
    s_boot_timer_us = esp_timer_get_time();

    char buf[32] = "-";
    if (s_source != HCC_CLOCK_NONE) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    }
    const int64_t last_sync = nvs_get_i64_or("last_sync", 0);
    ESP_LOGI(TAG, "[HCC TIME DIAG] boot reset=%s unix=%lld (%s) rtc_us=%llu source=%s last_sntp=%lld",
             reset_name(reason), (long long)now, buf, (unsigned long long)esp_rtc_get_time_us(),
             hcc_clock_source_name(), (long long)last_sync);
    if (s_source == HCC_CLOCK_NONE && reason == ESP_RST_POWERON && last_sync > 0) {
        ESP_LOGW(TAG, "[HCC TIME DIAG] mất giờ sau power-on: pin RTC không giữ được giờ (hết pin / chưa lắp / "
                      "chip reset cả vùng LP). Chờ Wi-Fi để đồng bộ.");
    }
}

void hcc_clock_ext_rtc_init(void)
{
    if (bsp_i2c_init() != ESP_OK || bsp_i2c_get_handle() == NULL) {
        ESP_LOGW(TAG, "[HCC TIME DIAG] DS3231: bus I2C chưa sẵn sàng");
        return;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (i2c_master_probe(bus, DS3231_ADDR, DS3231_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGI(TAG, "[HCC TIME DIAG] DS3231: không có module ở 0x%02X", DS3231_ADDR);
        return;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_ds3231) != ESP_OK) {
        s_ds3231 = NULL;
        return;
    }
    // Nhận diện: thanh ghi nhiệt độ 0x11/0x12 chỉ có ở DS3231 (0x12 chỉ dùng bit 7:6). Thiết bị khác
    // ở 0x68 (vd IMU, DS1307) không đạt -> bỏ, KHÔNG bao giờ ghi vào nó.
    time_t t = 0;
    float temp = 0;
    bool osf = false, is_ds3231 = false;
    const bool ok = ds3231_read(&t, &temp, &osf, &is_ds3231);
    if (!is_ds3231) {
        ESP_LOGW(TAG, "[HCC TIME DIAG] thiết bị ở 0x68 không giống DS3231 - bỏ qua, không ghi");
        i2c_master_bus_rm_device(s_ds3231);
        s_ds3231 = NULL;
        return;
    }
    ESP_LOGI(TAG, "[HCC TIME DIAG] DS3231: có module, nhiệt độ %.2f C, OSF=%d, giờ %s unix=%lld",
             temp, osf ? 1 : 0, ok ? "hợp lệ" : "KHÔNG hợp lệ", (long long)t);
    if (ok && t >= HCC_VALID_AFTER && !hcc_clock_is_valid()) {
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        s_source = HCC_CLOCK_DS3231;
        s_boot_unix = t;
        s_boot_timer_us = esp_timer_get_time();
        ESP_LOGI(TAG, "[HCC TIME DIAG] giờ hệ thống đặt từ DS3231");
    } else if (!ok) {
        ESP_LOGW(TAG, "[HCC TIME DIAG] DS3231 chưa có giờ (pin module hết / mới lắp) - sẽ ghi khi có Wi-Fi");
    }
}

bool hcc_clock_ext_rtc_present(void)
{
    return s_ds3231 != NULL;
}

void hcc_clock_on_sntp_sync(int64_t sntp_unix)
{
    if (s_source == HCC_CLOCK_RTC || s_source == HCC_CLOCK_DS3231) {
        // Giờ RTC (pin) dự kiến tại lúc này = giờ lúc khởi động + thời gian đã chạy.
        const int64_t expected = (int64_t)s_boot_unix + (esp_timer_get_time() - s_boot_timer_us) / 1000000;
        const int64_t drift = expected - sntp_unix;
        ESP_LOGI(TAG, "[HCC TIME DIAG] SNTP sync: giờ từ %s lệch %+lld s so với NTP", hcc_clock_source_name(),
                 (long long)drift);
        nvs_put_i64("last_drift", drift);
    } else if (s_source == HCC_CLOCK_NONE) {
        ESP_LOGI(TAG, "[HCC TIME DIAG] SNTP sync: trước đó chưa có giờ (pin RTC không giữ được qua lần khởi động này)");
    }
    s_source = HCC_CLOCK_SNTP;
    nvs_put_i64("last_sync", sntp_unix);
    if (s_ds3231 != NULL) {
        const bool w = ds3231_write((time_t)sntp_unix);
        ESP_LOGI(TAG, "[HCC TIME DIAG] DS3231: ghi giờ NTP %s", w ? "OK" : "LỖI");
    }
}

bool hcc_clock_is_valid(void)
{
    return s_source != HCC_CLOCK_NONE && time(NULL) >= HCC_VALID_AFTER;
}

hcc_clock_source_t hcc_clock_source(void)
{
    return s_source;
}

const char *hcc_clock_source_name(void)
{
    switch (s_source) {
    case HCC_CLOCK_RTC: return "RTC";
    case HCC_CLOCK_SNTP: return "SNTP";
    case HCC_CLOCK_DS3231: return "DS3231";
    default: return "NONE";
    }
}
