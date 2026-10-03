#include "hcc_clock.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "hcc_clock";

#define HCC_TZ "ICT-7"                       // Việt Nam UTC+7 (giống app_sntp.c)
#define HCC_VALID_AFTER ((time_t)1704067200) // 2024-01-01: giờ trước mốc này là chưa đặt
#define NVS_NS "hcc_clock"

static hcc_clock_source_t s_source = HCC_CLOCK_NONE;

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
    const time_t before = time(NULL);
    if (before >= HCC_VALID_AFTER) {
        // Độ trôi thật của giờ đang chạy (giờ RTC giữ bằng pin hoặc lần đồng bộ trước) so với NTP.
        const int64_t drift = (int64_t)before - sntp_unix;
        ESP_LOGI(TAG, "[HCC TIME DIAG] SNTP sync: giờ trước đó lệch %+lld s (nguồn trước=%s)",
                 (long long)drift, hcc_clock_source_name());
        nvs_put_i64("last_drift", drift);
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
