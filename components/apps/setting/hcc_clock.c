#include "hcc_clock.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "hcc_clock";

#define HCC_TZ "ICT-7"                       // Việt Nam UTC+7 (giống app_sntp.c)
#define HCC_VALID_AFTER ((time_t)1704067200) // 2024-01-01: giờ trước mốc này là chưa đặt
#define NVS_NS "hcc_clock"

static hcc_clock_source_t s_source = HCC_CLOCK_NONE;
// Giờ lúc khởi động (nếu còn từ RTC) + mốc esp_timer tương ứng: để đo độ trôi khi SNTP về.
// Không đo trong callback bằng time(): ESP-IDF đã settimeofday() trước khi gọi callback.
static time_t s_boot_unix = 0;
static int64_t s_boot_timer_us = 0;

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

void hcc_clock_on_sntp_sync(int64_t sntp_unix)
{
    if (s_source == HCC_CLOCK_RTC) {
        // Giờ RTC (pin) dự kiến tại lúc này = giờ lúc khởi động + thời gian đã chạy.
        const int64_t expected = (int64_t)s_boot_unix + (esp_timer_get_time() - s_boot_timer_us) / 1000000;
        const int64_t drift = expected - sntp_unix;
        ESP_LOGI(TAG, "[HCC TIME DIAG] SNTP sync: giờ giữ bằng pin RTC lệch %+lld s so với NTP", (long long)drift);
        nvs_put_i64("last_drift", drift);
    } else if (s_source == HCC_CLOCK_NONE) {
        ESP_LOGI(TAG, "[HCC TIME DIAG] SNTP sync: trước đó chưa có giờ (pin RTC không giữ được qua lần khởi động này)");
    }
    s_source = HCC_CLOCK_SNTP;
    nvs_put_i64("last_sync", sntp_unix);
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
    default: return "NONE";
    }
}
