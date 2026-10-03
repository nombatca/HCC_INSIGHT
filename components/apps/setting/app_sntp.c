/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "hcc_clock.h"

#define TIMEZONE        "CST-8"
#define SERVER_NAME_0   "216.239.35.0"  // Google Public NTP; numeric fallback avoids broken DNS
#define SERVER_NAME_1   "216.239.35.4"  // Google Public NTP
#define SERVER_NAME_2   "216.239.35.8"  // Google Public NTP

static const char *TAG = "sntp";

static void obtain_time(void);
static void initialize_sntp(void);

static void diagnostic_dns_result(const char *name, const ip_addr_t *address, void *arg)
{
    (void)name;
    (void)arg;
    char ip[IPADDR_STRLEN_MAX];
    if (address != NULL && IP_IS_V4(address) &&
        ipaddr_ntoa_r(address, ip, sizeof(ip)) != NULL) {
        ESP_LOGI(TAG, "[HCC TIME DIAG] DNS pool.ntp.org result=success ip=%s", ip);
    } else {
        ESP_LOGI(TAG, "[HCC TIME DIAG] DNS pool.ntp.org result=fail");
    }
}

// Run raw DNS APIs on the existing lwIP TCP/IP thread; never wait for DNS here.
static void diagnostic_dns_start(void *arg)
{
    (void)arg;
    for (u8_t index = 0; index < DNS_MAX_SERVERS; ++index) {
        const ip_addr_t *server = dns_getserver(index);
        char ip[IPADDR_STRLEN_MAX];
        const char *text = server ? ipaddr_ntoa_r(server, ip, sizeof(ip)) : NULL;
        ESP_LOGI(TAG, "[HCC TIME DIAG] DNS server%u=%s", (unsigned)index,
                 text ? text : "(unset)");
    }
    ip_addr_t address;
    const err_t result = dns_gethostbyname_addrtype("pool.ntp.org", &address,
                                                   diagnostic_dns_result, NULL,
                                                   LWIP_DNS_ADDRTYPE_IPV4);
    if (result == ERR_OK) {
        diagnostic_dns_result(NULL, &address, NULL);
    } else if (result != ERR_INPROGRESS) {
        diagnostic_dns_result(NULL, NULL, NULL);
    }
}

#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_CUSTOM
void sntp_sync_time(struct timeval *tv)
{
    settimeofday(tv, NULL);
    ESP_LOGI(TAG, "Time is synchronized from custom code");
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}
#endif

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "[HCC TIME DIAG] SNTP sync callback unix=%lld", (long long)time(NULL));
    ESP_LOGI(TAG, "Notification of a time synchronization event, sec=%lu", tv->tv_sec);
    hcc_clock_on_sntp_sync((int64_t)tv->tv_sec);  // đo độ trôi của giờ giữ bằng pin RTC
    const int result = settimeofday(tv, NULL);
    ESP_LOGI(TAG, "[HCC TIME DIAG] settimeofday result=%d unix=%lld", result, (long long)time(NULL));
}

void app_sntp_init(void)
{
    static bool sntp_initialized = false;
    time_t now;
    struct tm timeinfo;

    if (sntp_initialized) {
        return;
    }

    time(&now);
    ESP_LOGI(TAG, "[HCC TIME DIAG] app_sntp_init enter unix=%lld", (long long)now);
    localtime_r(&now, &timeinfo);

    // Set timezone to Vietnam / Indochina Time (UTC+7)
    setenv("TZ", "ICT-7", 1);
    tzset();
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }
#ifndef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    else {
        // Giờ còn từ pin RTC: vẫn đồng bộ (không chờ) để chỉnh độ trôi của đồng hồ RTC.
        ESP_LOGI(TAG, "[HCC TIME DIAG] time already set (RTC), starting SNTP to correct drift");
        initialize_sntp();
    }
#endif
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    else {
        // add 500 ms error to the current system time.
        // Only to demonstrate a work of adjusting method!
        {
            ESP_LOGI(TAG, "Add a error for test adjtime");
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);
            int64_t cpu_time = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
            int64_t error_time = cpu_time + 500 * 1000L;
            struct timeval tv_error = {.tv_sec = error_time / 1000000L, .tv_usec = error_time % 1000000L};
            settimeofday(&tv_error, NULL);
        }

        ESP_LOGI(TAG, "Time was set, now just adjusting it. Use SMOOTH SYNC method.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }
#endif

    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in Vietnam is: %s", strftime_buf);

    if (sntp_get_sync_mode() == SNTP_SYNC_MODE_SMOOTH) {
        struct timeval outdelta;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS) {
            adjtime(NULL, &outdelta);
            ESP_LOGI(TAG, "Waiting for adjusting time ... outdelta = %li sec: %li ms: %li us",
                     (long)outdelta.tv_sec,
                     outdelta.tv_usec / 1000,
                     outdelta.tv_usec % 1000);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    sntp_initialized = true;
}

static void obtain_time(void)
{
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "[HCC TIME DIAG] obtain_time exit synced=%d unix=%lld",
             now >= (time_t)1704067200 ? 1 : 0, (long long)now);
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SERVER_NAME_0);
    esp_sntp_setservername(1, SERVER_NAME_1);
    esp_sntp_setservername(2, SERVER_NAME_2);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
    const char *server0 = esp_sntp_getservername(0);
    ESP_LOGI(TAG, "[HCC TIME] SNTP servers: %s, %s, %s",
             server0 ? server0 : "(unset)",
             esp_sntp_getservername(1) ? esp_sntp_getservername(1) : "(unset)",
             esp_sntp_getservername(2) ? esp_sntp_getservername(2) : "(unset)");
    static bool dns_diagnostic_requested = false;
    if (!dns_diagnostic_requested) {
        dns_diagnostic_requested = true;
        if (tcpip_callback(diagnostic_dns_start, NULL) != ERR_OK) {
            ESP_LOGI(TAG, "[HCC TIME DIAG] DNS pool.ntp.org result=fail");
        }
    }
    esp_sntp_init();
}
