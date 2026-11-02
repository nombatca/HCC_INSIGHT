#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

// App "VON KE" (ADC đo điện áp) cho ESP32-P4.
//  - Dùng SAR ADC1 (oneshot) + hiệu chuẩn curve-fitting -> ra mV chính xác.
//  - Chân đo: ADC1 kênh 0..7 = GPIO16..23 (các chân này KHÔNG bị board dùng).
//  - Atten 12dB: đo trực tiếp ~0..3.3V tại CHÂN. Muốn đo áp cao hơn (pin 3.8-4.4V,
//    rail 5V...) phải mắc CẦU PHÂN ÁP ngoài rồi đặt "hệ số chia" cho đúng.
//
// ⚠️ TUYỆT ĐỐI không đưa quá ~3.3V vào chân ADC (hỏng chân). Áp cao -> qua cầu chia.
class HCCAdcMeter : public ESP_Brookesia_PhoneApp {
public:
    HCCAdcMeter();
    ~HCCAdcMeter();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    void buildUi(lv_obj_t *parent);
    bool adcInit(void);
    void adcDeinit(void);
    bool configChannel(int ch);         // chuyển kênh (atten 12dB)
    bool readMilliVolt(int *pin_mv);    // đọc, đa mẫu, ra mV tại chân

    static void tickCb(lv_timer_t *t);
    static void chBtnCb(lv_event_t *e);
    static void ratioBtnCb(lv_event_t *e);
    static void resetBtnCb(lv_event_t *e);
    void refreshUi(void);

    // ADC
    adc_oneshot_unit_handle_t _adc = nullptr;
    adc_cali_handle_t _cali = nullptr;
    bool _cali_ok = false;
    int _channel = 0;                   // 0..7
    bool _adc_ready = false;

    // Đo
    float _ratio = 1.0f;                // hệ số chia (nhân với áp chân)
    float _vmin = 1e9f, _vmax = -1e9f;  // V thực (đã nhân ratio)
    float _vnow = 0.0f;

    // UI
    lv_timer_t *_tick = nullptr;
    lv_obj_t *_big = nullptr;           // số V lớn
    lv_obj_t *_pin_lbl = nullptr;       // áp tại chân + raw
    lv_obj_t *_ch_lbl = nullptr;        // kênh + GPIO
    lv_obj_t *_ratio_lbl = nullptr;     // hệ số chia
    lv_obj_t *_minmax_lbl = nullptr;
    lv_obj_t *_bar = nullptr;
    lv_obj_t *_hint = nullptr;
};
