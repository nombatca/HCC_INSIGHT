#include "HCCAdcMeter.hpp"

#include <math.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_adc/adc_cali_scheme.h"

#include "vn_fonts.h"

LV_IMG_DECLARE(img_app_adc_meter);

static const char *TAG = "AdcMeter";

// ADC1 kênh 0..7 -> GPIO16..23 trên ESP32-P4.
static inline int ch_to_gpio(int ch) { return 16 + ch; }

HCCAdcMeter::HCCAdcMeter()
    : ESP_Brookesia_PhoneApp("Von ke", &img_app_adc_meter, true) {}

HCCAdcMeter::~HCCAdcMeter() {}

bool HCCAdcMeter::init(void) { return true; }

bool HCCAdcMeter::adcInit(void)
{
    adc_oneshot_unit_init_cfg_t icfg = {};
    icfg.unit_id = ADC_UNIT_1;
    icfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&icfg, &_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        _adc = nullptr;
        return false;
    }
    _adc_ready = configChannel(_channel);
    return _adc_ready;
}

void HCCAdcMeter::adcDeinit(void)
{
    if (_cali) {
        adc_cali_delete_scheme_curve_fitting(_cali);
        _cali = nullptr;
    }
    if (_adc) {
        adc_oneshot_del_unit(_adc);
        _adc = nullptr;
    }
    _adc_ready = false;
    _cali_ok = false;
}

bool HCCAdcMeter::configChannel(int ch)
{
    if (_adc == nullptr) return false;
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(_adc, (adc_channel_t)ch, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "config_channel %d failed", ch);
        return false;
    }
    // Hiệu chuẩn curve-fitting cho (unit, channel, atten). Tạo lại khi đổi kênh.
    if (_cali) { adc_cali_delete_scheme_curve_fitting(_cali); _cali = nullptr; }
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id = ADC_UNIT_1;
    cal.chan = (adc_channel_t)ch;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    _cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &_cali) == ESP_OK);
    if (!_cali_ok) _cali = nullptr;
    return true;
}

bool HCCAdcMeter::readMilliVolt(int *pin_mv)
{
    if (!_adc_ready) return false;
    const int N = 64;
    long acc = 0; int got = 0;
    for (int i = 0; i < N; ++i) {
        int raw = 0;
        if (adc_oneshot_read(_adc, (adc_channel_t)_channel, &raw) == ESP_OK) {
            acc += raw; got++;
        }
    }
    if (got == 0) return false;
    int raw_avg = (int)(acc / got);
    if (_cali_ok && _cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(_cali, raw_avg, &mv) == ESP_OK) { *pin_mv = mv; return true; }
    }
    // Dự phòng khi không hiệu chuẩn được: quy đổi thô 12-bit / ~3.3V.
    *pin_mv = (int)(raw_avg * 3300.0f / 4095.0f);
    return true;
}

// ============================ UI ============================

void HCCAdcMeter::buildUi(lv_obj_t *parent)
{
    const lv_font_t *f14 = &lv_font_be_vietnam_14;
    const lv_font_t *f16 = &lv_font_be_vietnam_16;
    const lv_font_t *f18 = &lv_font_be_vietnam_18;
    const lv_font_t *f22 = &lv_font_be_vietnam_22;
    const lv_font_t *f48 = &lv_font_be_vietnam_48;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A1016), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h1 = lv_label_create(parent);
    lv_label_set_text(h1, LV_SYMBOL_CHARGE "  VON KE (ADC)");
    lv_obj_set_style_text_font(h1, f22, 0);
    lv_obj_set_style_text_color(h1, lv_color_hex(0x5FD0FF), 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 14);

    // Số V lớn
    _big = lv_label_create(parent);
    lv_obj_set_style_text_font(_big, f48, 0);
    lv_obj_set_style_text_color(_big, lv_color_hex(0xE9FBFF), 0);
    lv_obj_align(_big, LV_ALIGN_TOP_LEFT, 40, 70);
    lv_label_set_text(_big, "-.--- V");

    _pin_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_pin_lbl, f16, 0);
    lv_obj_set_style_text_color(_pin_lbl, lv_color_hex(0x9FB6C6), 0);
    lv_obj_align(_pin_lbl, LV_ALIGN_TOP_LEFT, 42, 140);
    lv_label_set_text(_pin_lbl, "ap chan: - mV");

    // Thanh bar (mức áp tại chân so 3.3V)
    _bar = lv_bar_create(parent);
    lv_obj_set_size(_bar, 380, 18);
    lv_obj_align(_bar, LV_ALIGN_TOP_LEFT, 42, 176);
    lv_bar_set_range(_bar, 0, 3300);
    lv_obj_set_style_bg_color(_bar, lv_color_hex(0x16303F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_bar, lv_color_hex(0x2FA8E0), LV_PART_INDICATOR);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);

    _minmax_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_minmax_lbl, f16, 0);
    lv_obj_set_style_text_color(_minmax_lbl, lv_color_hex(0x9FB6C6), 0);
    lv_obj_align(_minmax_lbl, LV_ALIGN_TOP_LEFT, 42, 210);
    lv_label_set_text(_minmax_lbl, "min - / max -");

    // ---- Bảng phải: kênh + hệ số ----
    const int RX = 470;
    _ch_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_ch_lbl, f22, 0);
    lv_obj_set_style_text_color(_ch_lbl, lv_color_hex(0xE9FBFF), 0);
    lv_obj_align(_ch_lbl, LV_ALIGN_TOP_LEFT, RX, 74);
    lv_label_set_text(_ch_lbl, "ADC1 CH0 (GPIO16)");

    _ratio_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_ratio_lbl, f18, 0);
    lv_obj_set_style_text_color(_ratio_lbl, lv_color_hex(0xF2C94C), 0);
    lv_obj_align(_ratio_lbl, LV_ALIGN_TOP_LEFT, RX, 112);
    lv_label_set_text(_ratio_lbl, "He so chia: x1.0");

    _hint = lv_label_create(parent);
    lv_label_set_long_mode(_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_hint, 1024 - RX - 24);
    lv_obj_set_style_text_font(_hint, f14, 0);
    lv_obj_set_style_text_color(_hint, lv_color_hex(0xE08A8A), 0);
    lv_obj_align(_hint, LV_ALIGN_TOP_LEFT, RX, 146);
    lv_label_set_text(_hint,
        "Dua TRUC TIEP chi <=3.3V vao chan ADC. Ap cao hon (pin, 5V...) phai qua CAU PHAN AP roi dat he so chia. KHONG cham GND chung sai.");

    // ---- Nút ----
    auto mkBtn = [&](const char *txt, lv_event_cb_t cb, uint32_t col, lv_align_t al, int x, int y, int w) {
        lv_obj_t *b = lv_btn_create(parent);
        lv_obj_set_size(b, w, 52);
        lv_obj_align(b, al, x, y);
        lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, f16, 0);
    };
    mkBtn(LV_SYMBOL_LEFT "  KENH", chBtnCb, 0x1B3A5C, LV_ALIGN_BOTTOM_LEFT, 24, -18, 220);
    mkBtn("HE SO CHIA", ratioBtnCb, 0x5A4A12, LV_ALIGN_BOTTOM_LEFT, 260, -18, 220);
    mkBtn(LV_SYMBOL_REFRESH "  RESET MIN/MAX", resetBtnCb, 0x1C7A4E, LV_ALIGN_BOTTOM_LEFT, 496, -18, 260);
}

void HCCAdcMeter::refreshUi(void)
{
    char buf[64];
    // Cập nhật nhãn kênh + hệ số
    snprintf(buf, sizeof(buf), "ADC1 CH%d (GPIO%d)", _channel, ch_to_gpio(_channel));
    lv_label_set_text(_ch_lbl, buf);
    snprintf(buf, sizeof(buf), "He so chia: x%.1f", _ratio);
    lv_label_set_text(_ratio_lbl, buf);

    int pin_mv = 0;
    if (!readMilliVolt(&pin_mv)) {
        lv_label_set_text(_big, "ADC loi");
        return;
    }
    _vnow = (pin_mv / 1000.0f) * _ratio;
    if (_vnow < _vmin) _vmin = _vnow;
    if (_vnow > _vmax) _vmax = _vnow;

    // Số lớn: tự chọn số chữ số thập phân
    if (fabsf(_vnow) < 10.0f) snprintf(buf, sizeof(buf), "%.3f V", _vnow);
    else                      snprintf(buf, sizeof(buf), "%.2f V", _vnow);
    lv_label_set_text(_big, buf);

    snprintf(buf, sizeof(buf), "ap chan: %d mV  (x%.1f)", pin_mv, _ratio);
    lv_label_set_text(_pin_lbl, buf);

    int bv = pin_mv; if (bv < 0) bv = 0; if (bv > 3300) bv = 3300;
    lv_bar_set_value(_bar, bv, LV_ANIM_OFF);
    // Đổi màu bar khi gần ngưỡng nguy hiểm (>3.0V tại chân)
    lv_obj_set_style_bg_color(_bar, lv_color_hex(pin_mv > 3000 ? 0xE0574A : 0x2FA8E0), LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "min %.3f / max %.3f V", _vmin, _vmax);
    lv_label_set_text(_minmax_lbl, buf);

    if (!_cali_ok) lv_label_set_text(_hint, "CHUA hieu chuan duoc ADC -> so la uoc luong tho. Van dung so sanh tuong doi duoc.");
}

void HCCAdcMeter::tickCb(lv_timer_t *t)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(t->user_data);
    if (app) app->refreshUi();
}

void HCCAdcMeter::chBtnCb(lv_event_t *e)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(lv_event_get_user_data(e));
    if (!app) return;
    app->_channel = (app->_channel + 1) % 8;
    app->configChannel(app->_channel);
    app->_vmin = 1e9f; app->_vmax = -1e9f;
}

void HCCAdcMeter::ratioBtnCb(lv_event_t *e)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(lv_event_get_user_data(e));
    if (!app) return;
    // Vòng các hệ số thông dụng: x1 (truc tiep) -> x2 -> x5 -> x11 (cau chia 10k:1k) -> x1
    float r = app->_ratio;
    if (r < 1.5f) r = 2.0f;
    else if (r < 3.5f) r = 5.0f;
    else if (r < 8.0f) r = 11.0f;
    else r = 1.0f;
    app->_ratio = r;
    app->_vmin = 1e9f; app->_vmax = -1e9f;
}

void HCCAdcMeter::resetBtnCb(lv_event_t *e)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(lv_event_get_user_data(e));
    if (!app) return;
    app->_vmin = 1e9f; app->_vmax = -1e9f;
}

bool HCCAdcMeter::run(void)
{
    lv_obj_t *screen = lv_scr_act();
    buildUi(screen);
    adcInit();
    if (_tick == nullptr) _tick = lv_timer_create(tickCb, 120, this);
    return true;
}

bool HCCAdcMeter::back(void)
{
    notifyCoreClosed();
    return true;
}

bool HCCAdcMeter::close(void)
{
    if (_tick) { lv_timer_del(_tick); _tick = nullptr; }
    adcDeinit();
    _big = _pin_lbl = _ch_lbl = _ratio_lbl = _minmax_lbl = _bar = _hint = nullptr;
    return true;
}
