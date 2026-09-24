#pragma once

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

class Browser : public ESP_Brookesia_PhoneApp {
public:
    Browser();
    ~Browser() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    static void onOpenClicked(lv_event_t *event);
    static void onClearClicked(lv_event_t *event);
    static void onKeyboardEvent(lv_event_t *event);
    static void fetchTask(void *arg);
    static void applyFetchResult(void *arg);

    void fetchUrl(const char *url);
    void setStatus(const char *text);
    void setPageText(const char *text);

    lv_obj_t *_url_input;
    lv_obj_t *_keyboard;
    lv_obj_t *_page;
    lv_obj_t *_status;
    TaskHandle_t _fetch_task;
};
