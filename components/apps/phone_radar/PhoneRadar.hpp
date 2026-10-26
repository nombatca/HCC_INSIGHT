#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "PhoneRadarCore.hpp"

// App "RADAR THIẾT BỊ": quét các mạng/thiết bị WiFi ở gần và vẽ lên màn radar quét.
//  - Dùng esp_wifi_scan (chạy ổn qua đường esp-hosted của board) để quét Access
//    Point quanh đó: router, HOTSPOT điện thoại, thiết bị IoT. Mỗi mạng = 1 chấm,
//    vị trí theo RSSI (gần vào tâm, xa ra mép), tô màu theo loại.
//
// Vì sao không "bắt mọi điện thoại ở gần": board P4 không có sóng WiFi riêng mà
// điều khiển ESP32-C6 qua esp-hosted, lớp này KHÔNG chuyển gói raw (promiscuous/
// probe-request) lên nên không sniff được thiết bị client. Điện thoại chỉ hiện khi
// nó tự phát sóng (bật hotspot). Ngoài ra điện thoại ngẫu nhiên hoá MAC nên không
// định danh máy cụ thể; chỉ ước lượng hiện diện + khoảng cách tương đối.
class PhoneRadar : public ESP_Brookesia_PhoneApp {
public:
    PhoneRadar();
    ~PhoneRadar();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    void buildUi(lv_obj_t *parent);
    void startScanning(void);
    void stopScanning(void);

    static void scanTask(void *arg);
    static void uiTickCb(lv_timer_t *t);
    static void runBtnCb(lv_event_t *e);

    void doWifiScan(uint32_t now_ms);
    void refreshUi(void);

    PhoneRadarCore _core;
    SemaphoreHandle_t _lock = nullptr;
    TaskHandle_t _task = nullptr;
    volatile bool _running = false;
    volatile int _last_scan_err = 0;       // esp_err_t lần quét gần nhất

    // ---- UI ----
    lv_timer_t *_tick = nullptr;
    lv_obj_t *_radar_area = nullptr;
    lv_obj_t *_sweep = nullptr;
    lv_point_t _sweep_pts[2];
    int _sweep_angle = 0;

    static constexpr int kBlip = PhoneRadarCore::kMax;
    lv_obj_t *_blip[kBlip] = {nullptr};
    lv_obj_t *_count_lbl = nullptr;
    lv_obj_t *_phone_lbl = nullptr;
    lv_obj_t *_hint_lbl = nullptr;
    lv_obj_t *_run_lbl = nullptr;
    lv_obj_t *_list = nullptr;
    lv_obj_t *_rows[12] = {nullptr};

    int _radar_cx = 0, _radar_cy = 0, _radar_r = 0;
};
