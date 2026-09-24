#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

class HCCI2C : public ESP_Brookesia_PhoneApp {
public:
    HCCI2C();
    ~HCCI2C();

    bool run(void) override;
    bool back(void) override;

private:
    static void onScanClicked(lv_event_t *event);
    static void scanTask(void *arg);
    static void applyScanResult(void *arg);

    void startScan();
    void performScan();

    lv_obj_t *_status;
    lv_obj_t *_result;

    char _scan_status[64];
    char _scan_result[512];

    bool _scanning;
};
