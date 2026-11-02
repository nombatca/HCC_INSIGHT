#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

// App "VON KE" chuyen nghiep (ADC do dien ap) cho ESP32-P4.
//  - SAR ADC1 oneshot + hieu chuan curve-fitting -> mV chinh xac.
//  - Chan do: ADC1 CH4..7 = GPIO20..23 (CH0-3/GPIO16-19 danh cho SDIO WiFi -> cam).
//  - Atten 12dB: do truc tiep <=~3.3V tai chan; ap cao hon qua CAU PHAN AP + he so chia.
//  - Loc EMA cho so on dinh, thong ke MIN/MAX/AVG/P-P, do thi trend, REL (zero), HOLD.
//
// ⚠️ Khong dua qua ~3.3V thang vao chan ADC. Noi GND chung.
class HCCAdcMeter : public ESP_Brookesia_PhoneApp {
public:
    HCCAdcMeter();
    ~HCCAdcMeter();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    static constexpr int kChartPts = 120;

    void buildUi(lv_obj_t *parent);
    bool adcInit(void);
    void adcDeinit(void);
    bool configChannel(int ch);
    bool readMilliVolt(int *pin_mv);
    void resetStats(void);

    static void tickCb(lv_timer_t *t);
    static void chBtnCb(lv_event_t *e);
    static void ratioBtnCb(lv_event_t *e);
    static void relBtnCb(lv_event_t *e);
    static void holdBtnCb(lv_event_t *e);
    static void resetBtnCb(lv_event_t *e);
    void refreshUi(void);

    // ADC
    adc_oneshot_unit_handle_t _adc = nullptr;
    adc_cali_handle_t _cali = nullptr;
    bool _cali_ok = false;
    int _channel = 4;                   // CH4..7 = GPIO20..23
    bool _adc_ready = false;

    // Do + loc
    float _ratio = 1.0f;
    float _ema = 0.0f; bool _ema_init = false;  // loc EMA hien thi
    float _rel_offset = 0.0f; bool _rel_on = false;
    bool _hold = false;

    // Thong ke (tren V thuc, da nhan ratio)
    float _vmin = 1e9f, _vmax = -1e9f;
    double _vsum = 0.0; uint32_t _vn = 0;
    float _ring[kChartPts] = {0}; int _rhead = 0; int _rcount = 0;  // cua so do thi

    // UI
    lv_timer_t *_tick = nullptr;
    lv_obj_t *_big = nullptr;
    lv_obj_t *_sub = nullptr;           // pin mV / raw / mode
    lv_obj_t *_bar = nullptr;
    lv_obj_t *_ch_lbl = nullptr;
    lv_obj_t *_ratio_lbl = nullptr;
    lv_obj_t *_st_min = nullptr, *_st_max = nullptr, *_st_avg = nullptr, *_st_pp = nullptr, *_st_n = nullptr;
    lv_obj_t *_rel_tag = nullptr, *_hold_tag = nullptr;
    lv_obj_t *_chart = nullptr;
    lv_chart_series_t *_ser = nullptr;
    lv_obj_t *_hold_lbl = nullptr;
};
