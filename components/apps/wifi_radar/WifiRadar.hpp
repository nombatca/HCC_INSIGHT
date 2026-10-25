#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "WifiRadarCore.hpp"

// App "WiFi Radar": dò CÓ NGƯỜI / CHUYỂN ĐỘNG trong phòng dựa trên biến động RSSI
// của AP WiFi đang kết nối. Không camera, không CSI (đường hosted của board không
// chuyển CSI); chỉ dùng RSSI nên nhẹ và chạy được ngay.
class HCCWifiRadar : public ESP_Brookesia_PhoneApp {
public:
    HCCWifiRadar();
    ~HCCWifiRadar();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    void buildUi(lv_obj_t *parent);
    void startSampling(void);
    void stopSampling(void);

    static void samplerTask(void *arg);
    static void uiTickCb(lv_timer_t *t);
    static void sensCb(lv_event_t *e);
    static void runBtnCb(lv_event_t *e);

    void refreshUi(void);

    WifiRadar _radar;
    SemaphoreHandle_t _lock = nullptr;
    TaskHandle_t _task = nullptr;
    volatile bool _running = false;

    lv_timer_t *_tick = nullptr;
    lv_obj_t *_status = nullptr;    // YÊN TĨNH / CÓ CHUYỂN ĐỘNG
    lv_obj_t *_rssi_lbl = nullptr;
    lv_obj_t *_act_lbl = nullptr;
    lv_obj_t *_ap_lbl = nullptr;
    lv_obj_t *_sens_lbl = nullptr;
    lv_obj_t *_chart = nullptr;
    lv_chart_series_t *_ser = nullptr;
    lv_obj_t *_run_lbl = nullptr;

    // Radar quét kiểu quân sự.
    static constexpr int kTrail = 10;   // số đoạn đuôi tia quét
    static constexpr int kBlip = 14;    // số chấm "mục tiêu" tối đa

    lv_obj_t *_radar_area = nullptr;
    lv_obj_t *_sweep = nullptr;
    lv_obj_t *_trail[kTrail] = {nullptr};
    lv_point_t _trail_pts[kTrail][2];
    lv_point_t _sweep_pts[2];
    int _sweep_angle = 0;
    int _rcx = 0, _rcy = 0, _rR = 0;

    lv_obj_t *_blips[kBlip] = {nullptr};
    float _blip_life[kBlip] = {0};     // 1.0 -> 0.0 (mờ dần)
    int _blip_slot = 0;
    uint32_t _last_blip_ms = 0;
    uint32_t _rng = 0x12345678;
    uint32_t rnd() { _rng = _rng * 1103515245u + 12345u; return (_rng >> 16) & 0x7fff; }

    // Snapshot đọc từ core (dưới khoá) để vẽ.
    float _s_rssi = 0, _s_base = 0, _s_act = 0, _s_p2p = 0;
    int _s_min = 0, _s_max = 0;
    bool _s_motion = false;
    int _s_level = 0;
    bool _s_connected = false;
    char _s_ssid[33] = {0};
};
