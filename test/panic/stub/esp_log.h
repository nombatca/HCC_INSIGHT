#pragma once
// Stub ESP-IDF log cho test host: in ra stderr khi HCC_TEST_LOG=1.
#include <cstdio>
#include <cstdlib>
#define HCC_TEST_LOG_(lvl, tag, fmt, ...)                                                  \
    do {                                                                                   \
        if (getenv("HCC_TEST_LOG")) fprintf(stderr, lvl " %s: " fmt "\n", tag, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGE(tag, fmt, ...) HCC_TEST_LOG_("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HCC_TEST_LOG_("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HCC_TEST_LOG_("I", tag, fmt, ##__VA_ARGS__)
