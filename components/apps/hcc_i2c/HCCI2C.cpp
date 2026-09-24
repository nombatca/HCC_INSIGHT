#include "HCCI2C.hpp"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

HCCI2C::HCCI2C()
    : ESP_Brookesia_PhoneApp("I2C", nullptr, false),
      _status(nullptr),
      _result(nullptr),
      _scan_status{},
      _scan_result{},
      _scanning(false)
{
}

HCCI2C::~HCCI2C()
{
}

bool HCCI2C::run(void)
{
    lv_obj_t *screen = lv_scr_act();

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "I2C DIAGNOSTICS");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 20);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_be_vietnam_28, 0);

    lv_obj_t *bus = lv_label_create(screen);
    lv_label_set_text(bus, "SDA: GPIO 7    SCL: GPIO 8");
    lv_obj_align(bus, LV_ALIGN_TOP_LEFT, 28, 58);
    lv_obj_set_style_text_color(bus, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(bus, &lv_font_be_vietnam_18, 0);

    _status = lv_label_create(screen);
    lv_label_set_text(_status, "Ready");
    lv_obj_align(_status, LV_ALIGN_TOP_LEFT, 28, 92);
    lv_obj_set_style_text_color(_status, lv_color_hex(0x69E0C0), 0);
    lv_obj_set_style_text_font(_status, &lv_font_be_vietnam_18, 0);

    _result = lv_label_create(screen);
    lv_label_set_text(_result, "Press SCAN I2C");
    lv_obj_align(_result, LV_ALIGN_TOP_LEFT, 28, 145);
    lv_obj_set_style_text_color(_result, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_result, &lv_font_be_vietnam_18, 0);

    lv_obj_t *scan_btn = lv_btn_create(screen);
    lv_obj_set_size(scan_btn, 240, 60);
    lv_obj_align(scan_btn, LV_ALIGN_BOTTOM_LEFT, 28, -25);
    lv_obj_add_event_cb(scan_btn, onScanClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *scan_label = lv_label_create(scan_btn);
    lv_label_set_text(scan_label, "SCAN I2C");
    lv_obj_center(scan_label);
    lv_obj_set_style_text_font(scan_label, &lv_font_be_vietnam_18, 0);

    return true;
}

bool HCCI2C::back(void)
{
    notifyCoreClosed();
    return true;
}

void HCCI2C::onScanClicked(lv_event_t *event)
{
    HCCI2C *app =
        static_cast<HCCI2C *>(lv_event_get_user_data(event));

    if (app != nullptr && !app->_scanning) {
        app->startScan();
    }
}

void HCCI2C::startScan()
{
    if (_scanning) {
        return;
    }

    _scanning = true;

    if (_status != nullptr) {
        lv_label_set_text(_status, "Scanning...");
    }

    if (_result != nullptr) {
        lv_label_set_text(_result, "Scanning 0x00 - 0x7F...");
    }

    xTaskCreate(
        scanTask,
        "hcc_i2c_scan",
        4096,
        this,
        5,
        nullptr
    );
}

void HCCI2C::scanTask(void *arg)
{
    HCCI2C *app = static_cast<HCCI2C *>(arg);

    if (app != nullptr) {
        app->performScan();
        lv_async_call(applyScanResult, app);
    }

    vTaskDelete(nullptr);
}

void HCCI2C::performScan()
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    _scan_result[0] = '\0';

    if (bus == nullptr) {
        snprintf(
            _scan_status,
            sizeof(_scan_status),
            "I2C bus unavailable"
        );
        return;
    }

    int found = 0;

    for (uint8_t address = 0x00; address <= 0x7F; address++) {
        esp_err_t ret = i2c_master_probe(bus, address, 50);

        if (ret == ESP_OK) {
            char line[16];

            snprintf(
                line,
                sizeof(line),
                "0x%02X  ",
                address
            );

            strncat(
                _scan_result,
                line,
                sizeof(_scan_result) -
                strlen(_scan_result) - 1
            );

            found++;

            if ((found % 6) == 0) {
                strncat(
                    _scan_result,
                    "\n",
                    sizeof(_scan_result) -
                    strlen(_scan_result) - 1
                );
            }
        }
    }

    snprintf(
        _scan_status,
        sizeof(_scan_status),
        "Scan complete - %d device(s)",
        found
    );

    if (found == 0) {
        snprintf(
            _scan_result,
            sizeof(_scan_result),
            "No I2C device found"
        );
    }
}

void HCCI2C::applyScanResult(void *arg)
{
    HCCI2C *app = static_cast<HCCI2C *>(arg);

    if (app == nullptr) {
        return;
    }

    if (app->_status != nullptr) {
        lv_label_set_text(
            app->_status,
            app->_scan_status
        );
    }

    if (app->_result != nullptr) {
        lv_label_set_text(
            app->_result,
            app->_scan_result
        );
    }

    app->_scanning = false;
}
