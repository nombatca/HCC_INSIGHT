#include "HCCAdcMeter.hpp"

#include <math.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_adc/adc_cali_scheme.h"

#include "vn_fonts.h"

LV_IMG_DECLARE(img_app_adc_meter);

static const char *TAG = "AdcMeter";

static inline int ch_to_gpio(int ch) { return 16 + ch; }   // ADC1 CHn -> GPIO(16+n)

HCCAdcMeter::HCCAdcMeter()
    : ESP_Brookesia_PhoneApp("Von ke", &img_app_adc_meter, true) {}
HCCAdcMeter::~HCCAdcMeter() {}
bool HCCAdcMeter::init(void) { return true; }

// ---------------- ADC ----------------
bool HCCAdcMeter::adcInit(void)
{
    adc_oneshot_unit_init_cfg_t icfg = {};
    icfg.unit_id = ADC_UNIT_1;
    icfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&icfg, &_adc) != ESP_OK) { _adc = nullptr; return false; }
    _adc_ready = configChannel(_channel);
    return _adc_ready;
}
void HCCAdcMeter::adcDeinit(void)
{
    if (_cali) { adc_cali_delete_scheme_curve_fitting(_cali); _cali = nullptr; }
    if (_adc)  { adc_oneshot_del_unit(_adc); _adc = nullptr; }
    _adc_ready = false; _cali_ok = false;
}
bool HCCAdcMeter::configChannel(int ch)
{
    if (_adc == nullptr) return false;
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(_adc, (adc_channel_t)ch, &ccfg) != ESP_OK) return false;
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
        if (adc_oneshot_read(_adc, (adc_channel_t)_channel, &raw) == ESP_OK) { acc += raw; got++; }
    }
    if (got == 0) return false;
    int raw_avg = (int)(acc / got);
    if (_cali_ok && _cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(_cali, raw_avg, &mv) == ESP_OK) { *pin_mv = mv; return true; }
    }
    *pin_mv = (int)(raw_avg * 3300.0f / 4095.0f);
    return true;
}
void HCCAdcMeter::resetStats(void)
{
    _vmin = 1e9f; _vmax = -1e9f; _vsum = 0.0; _vn = 0;
    _rhead = 0; _rcount = 0;
    _ema_init = false;
    if (_chart && _ser) {
        for (int i = 0; i < kChartPts; ++i) lv_chart_set_next_value(_chart, _ser, LV_CHART_POINT_NONE);
    }
}

// ---------------- UI ----------------
void HCCAdcMeter::buildUi(lv_obj_t *parent)
{
    const lv_font_t *f16 = &lv_font_be_vietnam_16;
    const lv_font_t *f18 = &lv_font_be_vietnam_18;
    const lv_font_t *f22 = &lv_font_be_vietnam_22;
    const lv_font_t *f48 = &lv_font_be_vietnam_48;
    const uint32_t CARD = 0x12202C, BD = 0x24404F;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A1016), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h1 = lv_label_create(parent);
    lv_label_set_text(h1, LV_SYMBOL_CHARGE "  VON KE");
    lv_obj_set_style_text_font(h1, f22, 0);
    lv_obj_set_style_text_color(h1, lv_color_hex(0x5FD0FF), 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 12);

    _ch_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_ch_lbl, f18, 0);
    lv_obj_set_style_text_color(_ch_lbl, lv_color_hex(0x9FD8EE), 0);
    lv_obj_align(_ch_lbl, LV_ALIGN_TOP_LEFT, 150, 16);
    lv_label_set_text(_ch_lbl, "ADC1 CH4 (GPIO20)");

    _ratio_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_ratio_lbl, f18, 0);
    lv_obj_set_style_text_color(_ratio_lbl, lv_color_hex(0xF2C94C), 0);
    lv_obj_align(_ratio_lbl, LV_ALIGN_TOP_RIGHT, -24, 16);
    lv_label_set_text(_ratio_lbl, "chia x1.0");

    // --- Khung so lon (trai) ---
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 470, 180);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 24, 50);
    lv_obj_set_style_bg_color(card, lv_color_hex(CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(BD), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    _big = lv_label_create(card);
    lv_obj_set_style_text_font(_big, f48, 0);
    lv_obj_set_style_text_color(_big, lv_color_hex(0xEAFBFF), 0);
    lv_obj_set_pos(_big, 20, 24);
    lv_label_set_text(_big, "-.--- V");

    _rel_tag = lv_label_create(card);
    lv_obj_set_style_text_font(_rel_tag, f16, 0);
    lv_obj_set_style_text_color(_rel_tag, lv_color_hex(0x7CF6B0), 0);
    lv_obj_set_pos(_rel_tag, 360, 16);
    lv_label_set_text(_rel_tag, "");

    _hold_tag = lv_label_create(card);
    lv_obj_set_style_text_font(_hold_tag, f16, 0);
    lv_obj_set_style_text_color(_hold_tag, lv_color_hex(0xFFB347), 0);
    lv_obj_set_pos(_hold_tag, 360, 38);
    lv_label_set_text(_hold_tag, "");

    _sub = lv_label_create(card);
    lv_obj_set_style_text_font(_sub, f16, 0);
    lv_obj_set_style_text_color(_sub, lv_color_hex(0x9FB6C6), 0);
    lv_obj_set_pos(_sub, 22, 104);
    lv_label_set_text(_sub, "ap chan: - mV");

    _bar = lv_bar_create(card);
    lv_obj_set_size(_bar, 430, 16);
    lv_obj_set_pos(_bar, 20, 140);
    lv_bar_set_range(_bar, 0, 3300);
    lv_obj_set_style_bg_color(_bar, lv_color_hex(0x16303F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_bar, lv_color_hex(0x2FA8E0), LV_PART_INDICATOR);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);

    // --- Khung thong ke (phai) ---
    lv_obj_t *stat = lv_obj_create(parent);
    lv_obj_set_size(stat, 482, 180);
    lv_obj_align(stat, LV_ALIGN_TOP_LEFT, 510, 50);
    lv_obj_set_style_bg_color(stat, lv_color_hex(CARD), 0);
    lv_obj_set_style_border_color(stat, lv_color_hex(BD), 0);
    lv_obj_set_style_border_width(stat, 1, 0);
    lv_obj_set_style_radius(stat, 12, 0);
    lv_obj_set_style_pad_all(stat, 0, 0);
    lv_obj_clear_flag(stat, LV_OBJ_FLAG_SCROLLABLE);
    auto mkStat = [&](const char *name, int x, int y, lv_obj_t **val, uint32_t col) {
        lv_obj_t *n = lv_label_create(stat);
        lv_obj_set_style_text_font(n, f16, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(0x7E97A8), 0);
        lv_obj_set_pos(n, x, y);
        lv_label_set_text(n, name);
        lv_obj_t *v = lv_label_create(stat);
        lv_obj_set_style_text_font(v, f22, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(col), 0);
        lv_obj_set_pos(v, x, y + 22);
        lv_label_set_text(v, "-");
        *val = v;
    };
    mkStat("MIN",  22, 20, &_st_min, 0x8FD0FF);
    mkStat("MAX", 182, 20, &_st_max, 0xFF8A8A);
    mkStat("TB",  342, 20, &_st_avg, 0xEAFBFF);
    mkStat("P-P",  22, 96, &_st_pp, 0xF2C94C);
    mkStat("So mau",182,96,&_st_n,  0x9FB6C6);

    // --- Do thi trend ---
    _chart = lv_chart_create(parent);
    lv_obj_set_size(_chart, 968, 236);
    lv_obj_align(_chart, LV_ALIGN_TOP_LEFT, 24, 244);
    lv_obj_set_style_bg_color(_chart, lv_color_hex(0x0C1A24), 0);
    lv_obj_set_style_border_color(_chart, lv_color_hex(BD), 0);
    lv_obj_set_style_border_width(_chart, 1, 0);
    lv_obj_set_style_radius(_chart, 10, 0);
    lv_obj_set_style_pad_all(_chart, 6, 0);
    lv_obj_set_style_line_color(_chart, lv_color_hex(0x1C3545), LV_PART_MAIN);
    lv_chart_set_type(_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(_chart, kChartPts);
    lv_chart_set_update_mode(_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(_chart, 5, 8);
    lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 3300);
    _ser = lv_chart_add_series(_chart, lv_color_hex(0x37D0FF), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_width(_chart, 0, LV_PART_INDICATOR);   // an cham diem
    lv_obj_set_style_height(_chart, 0, LV_PART_INDICATOR);
    for (int i = 0; i < kChartPts; ++i) lv_chart_set_next_value(_chart, _ser, LV_CHART_POINT_NONE);

    // --- Nut ---
    auto mkBtn = [&](const char *txt, lv_event_cb_t cb, uint32_t col, int x, int w) {
        lv_obj_t *b = lv_btn_create(parent);
        lv_obj_set_size(b, w, 52);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, x, -14);
        lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, f16, 0);
        return l;
    };
    mkBtn(LV_SYMBOL_LEFT " KENH",  chBtnCb,    0x1B3A5C, 24,  186);
    mkBtn("HE SO CHIA",            ratioBtnCb, 0x5A4A12, 220, 186);
    mkBtn("REL (0)",               relBtnCb,   0x1C5E3E, 416, 186);
    _hold_lbl = mkBtn(LV_SYMBOL_PAUSE " HOLD", holdBtnCb, 0x5A2E12, 612, 186);
    mkBtn(LV_SYMBOL_REFRESH " RESET", resetBtnCb, 0x1C7A4E, 808, 186);
}

void HCCAdcMeter::refreshUi(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "ADC1 CH%d (GPIO%d)", _channel, ch_to_gpio(_channel));
    lv_label_set_text(_ch_lbl, buf);
    snprintf(buf, sizeof(buf), "chia x%.1f", _ratio);
    lv_label_set_text(_ratio_lbl, buf);
    lv_label_set_text(_rel_tag, _rel_on ? "REL" : "");
    lv_label_set_text(_hold_tag, _hold ? LV_SYMBOL_PAUSE " HOLD" : "");

    if (_hold) return;  // giu man, khong doc

    int pin_mv = 0;
    if (!readMilliVolt(&pin_mv)) { lv_label_set_text(_big, "ADC loi"); return; }
    float v = (pin_mv / 1000.0f) * _ratio;            // V thuc
    // Loc EMA cho hien thi
    if (!_ema_init) { _ema = v; _ema_init = true; }
    else _ema += 0.30f * (v - _ema);
    float disp = _rel_on ? (_ema - _rel_offset) : _ema;

    // So lon
    if (fabsf(disp) < 10.0f) snprintf(buf, sizeof(buf), "%.3f V", disp);
    else                     snprintf(buf, sizeof(buf), "%.2f V", disp);
    lv_label_set_text(_big, buf);
    snprintf(buf, sizeof(buf), "ap chan: %d mV   raw~%.3fV", pin_mv, pin_mv / 1000.0f);
    lv_label_set_text(_sub, buf);

    // Bar + canh bao
    int bv = pin_mv; if (bv < 0) bv = 0; if (bv > 3300) bv = 3300;
    lv_bar_set_value(_bar, bv, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_bar, lv_color_hex(pin_mv > 3000 ? 0xE0574A : 0x2FA8E0), LV_PART_INDICATOR);

    // Thong ke tren V thuc (khong tru REL)
    if (v < _vmin) _vmin = v;
    if (v > _vmax) _vmax = v;
    _vsum += v; _vn++;
    _ring[_rhead] = v; _rhead = (_rhead + 1) % kChartPts; if (_rcount < kChartPts) _rcount++;

    snprintf(buf, sizeof(buf), "%.3f V", _vmin); lv_label_set_text(_st_min, buf);
    snprintf(buf, sizeof(buf), "%.3f V", _vmax); lv_label_set_text(_st_max, buf);
    snprintf(buf, sizeof(buf), "%.3f V", (float)(_vsum / (_vn ? _vn : 1))); lv_label_set_text(_st_avg, buf);
    snprintf(buf, sizeof(buf), "%.3f V", _vmax - _vmin); lv_label_set_text(_st_pp, buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)_vn); lv_label_set_text(_st_n, buf);

    // Do thi: auto-range Y theo cua so, don vi mV
    float wmin = 1e9f, wmax = -1e9f;
    for (int i = 0; i < _rcount; ++i) { float x = _ring[i]; if (x < wmin) wmin = x; if (x > wmax) wmax = x; }
    if (_rcount == 0) { wmin = 0; wmax = 1; }
    float pad = (wmax - wmin) * 0.15f + 0.05f;
    int ylo = (int)((wmin - pad) * 1000.0f);
    int yhi = (int)((wmax + pad) * 1000.0f);
    if (yhi <= ylo) yhi = ylo + 100;
    lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, ylo, yhi);
    lv_chart_set_next_value(_chart, _ser, (int)(disp * 1000.0f));
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
    app->_channel = ((app->_channel - 4 + 1) % 4) + 4;   // 4..7 (GPIO20..23)
    app->configChannel(app->_channel);
    app->resetStats();
}
void HCCAdcMeter::ratioBtnCb(lv_event_t *e)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(lv_event_get_user_data(e));
    if (!app) return;
    static const float R[] = {1.0f, 2.0f, 5.0f, 11.0f, 21.0f, 101.0f};
    int n = sizeof(R) / sizeof(R[0]);
    int idx = 0;
    for (int i = 0; i < n; ++i) if (fabsf(app->_ratio - R[i]) < 0.01f) { idx = i; break; }
    app->_ratio = R[(idx + 1) % n];
    app->resetStats();
}
void HCCAdcMeter::relBtnCb(lv_event_t *e)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(lv_event_get_user_data(e));
    if (!app) return;
    if (!app->_rel_on) { app->_rel_offset = app->_ema; app->_rel_on = true; }
    else app->_rel_on = false;
}
void HCCAdcMeter::holdBtnCb(lv_event_t *e)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(lv_event_get_user_data(e));
    if (!app) return;
    app->_hold = !app->_hold;
    if (app->_hold_lbl) lv_label_set_text(app->_hold_lbl, app->_hold ? LV_SYMBOL_PLAY " TIEP" : LV_SYMBOL_PAUSE " HOLD");
}
void HCCAdcMeter::resetBtnCb(lv_event_t *e)
{
    HCCAdcMeter *app = static_cast<HCCAdcMeter *>(lv_event_get_user_data(e));
    if (!app) return;
    app->resetStats();
}

bool HCCAdcMeter::run(void)
{
    lv_obj_t *screen = lv_scr_act();
    buildUi(screen);
    adcInit();
    resetStats();
    if (_tick == nullptr) _tick = lv_timer_create(tickCb, 100, this);
    return true;
}
bool HCCAdcMeter::back(void) { notifyCoreClosed(); return true; }
bool HCCAdcMeter::close(void)
{
    if (_tick) { lv_timer_del(_tick); _tick = nullptr; }
    adcDeinit();
    _big = _sub = _bar = _ch_lbl = _ratio_lbl = nullptr;
    _st_min = _st_max = _st_avg = _st_pp = _st_n = nullptr;
    _rel_tag = _hold_tag = _chart = _hold_lbl = nullptr; _ser = nullptr;
    return true;
}
