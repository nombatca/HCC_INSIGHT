#include "WifiRadar.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_wifi.h"
#include "esp_timer.h"
#include "vn_fonts.h"

#define TAG "WifiRadar"

LV_IMG_DECLARE(img_app_wifi_radar);

namespace {
constexpr int SAMPLE_MS = 125;      // 8 Hz
constexpr int CHART_POINTS = 120;
inline uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }
}  // namespace

HCCWifiRadar::HCCWifiRadar()
    : ESP_Brookesia_PhoneApp("WiFi Radar", &img_app_wifi_radar, true) {}

HCCWifiRadar::~HCCWifiRadar() {}

bool HCCWifiRadar::init(void)
{
    if (_lock == nullptr) _lock = xSemaphoreCreateMutex();
    return _lock != nullptr;
}

bool HCCWifiRadar::run(void)
{
    _radar.reset();
    buildUi(lv_scr_act());
    startSampling();
    if (_tick == nullptr) _tick = lv_timer_create(uiTickCb, 100, this);
    return true;
}

bool HCCWifiRadar::back(void)
{
    notifyCoreClosed();
    return true;
}

bool HCCWifiRadar::close(void)
{
    stopSampling();
    if (_tick) { lv_timer_del(_tick); _tick = nullptr; }
    return true;
}

// ---------------- UI ----------------
void HCCWifiRadar::buildUi(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0B1420), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    const lv_font_t *f20 = &lv_font_be_vietnam_20, *f16 = &lv_font_be_vietnam_16, *f14 = &lv_font_be_vietnam_14;

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_WIFI " WIFI RADAR");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(title, f20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 18);

    // ---- Radar tròn kiểu quân sự (trái) ----
    const int RS = 300;
    const uint32_t GREEN = 0x2BE06A;     // xanh lân tinh
    _radar_area = lv_obj_create(parent);
    lv_obj_set_size(_radar_area, RS, RS);
    lv_obj_align(_radar_area, LV_ALIGN_TOP_LEFT, 40, 70);
    lv_obj_set_style_bg_color(_radar_area, lv_color_hex(0x031207), 0);
    lv_obj_set_style_radius(_radar_area, RS / 2, 0);
    lv_obj_set_style_border_color(_radar_area, lv_color_hex(0x1F8A42), 0);
    lv_obj_set_style_border_width(_radar_area, 2, 0);
    lv_obj_set_style_clip_corner(_radar_area, true, 0);
    lv_obj_clear_flag(_radar_area, LV_OBJ_FLAG_SCROLLABLE);
    _rcx = RS / 2; _rcy = RS / 2; _rR = RS / 2 - 8;
    // vòng cự ly
    for (int k = 1; k <= 3; ++k) {
        int rr = _rR * k / 3;
        lv_obj_t *ring = lv_obj_create(_radar_area);
        lv_obj_set_size(ring, rr * 2, rr * 2);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ring, rr, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0x15521F + k * 0x061A08), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }
    // thập tự + 2 đường chéo
    for (int d = 0; d < 4; ++d) {
        static lv_point_t cl[4][2];
        float ang = d * 45.0f * 3.14159265f / 180.0f;
        cl[d][0] = {(lv_coord_t)(_rcx - _rR * cosf(ang)), (lv_coord_t)(_rcy - _rR * sinf(ang))};
        cl[d][1] = {(lv_coord_t)(_rcx + _rR * cosf(ang)), (lv_coord_t)(_rcy + _rR * sinf(ang))};
        lv_obj_t *ln = lv_line_create(_radar_area);
        lv_obj_set_style_line_color(ln, lv_color_hex(0x154A20), 0);
        lv_obj_set_style_line_width(ln, 1, 0);
        lv_line_set_points(ln, cl[d], 2);
    }
    // đuôi tia quét (mờ dần) — vẽ trước, nằm dưới tia chính
    for (int i = 0; i < kTrail; ++i) {
        _trail[i] = lv_line_create(_radar_area);
        lv_obj_set_style_line_color(_trail[i], lv_color_hex(GREEN), 0);
        lv_obj_set_style_line_width(_trail[i], 3, 0);
        lv_obj_set_style_line_opa(_trail[i], LV_OPA_TRANSP, 0);
        _trail_pts[i][0] = {(lv_coord_t)_rcx, (lv_coord_t)_rcy};
        _trail_pts[i][1] = {(lv_coord_t)_rcx, (lv_coord_t)_rcy};
        lv_line_set_points(_trail[i], _trail_pts[i], 2);
    }
    // tia quét chính
    _sweep = lv_line_create(_radar_area);
    lv_obj_set_style_line_color(_sweep, lv_color_hex(0x8DFFB0), 0);
    lv_obj_set_style_line_width(_sweep, 3, 0);
    _sweep_pts[0] = {(lv_coord_t)_rcx, (lv_coord_t)_rcy};
    _sweep_pts[1] = {(lv_coord_t)(_rcx + _rR), (lv_coord_t)_rcy};
    lv_line_set_points(_sweep, _sweep_pts, 2);
    // tâm
    lv_obj_t *dot = lv_obj_create(_radar_area);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(GREEN), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    // pool chấm mục tiêu
    for (int i = 0; i < kBlip; ++i) {
        _blips[i] = lv_obj_create(_radar_area);
        lv_obj_set_size(_blips[i], 16, 16);
        lv_obj_set_style_radius(_blips[i], 8, 0);
        lv_obj_set_style_bg_color(_blips[i], lv_color_hex(0xB6FF3A), 0);
        lv_obj_set_style_border_width(_blips[i], 0, 0);
        lv_obj_clear_flag(_blips[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_blips[i], LV_OBJ_FLAG_HIDDEN);
        _blip_life[i] = 0;
    }

    // ---- Cột phải: trạng thái + số liệu ----
    const int RX = 380;
    _status = lv_label_create(parent);
    lv_obj_set_style_text_font(_status, f20, 0);
    lv_obj_align(_status, LV_ALIGN_TOP_LEFT, RX, 74);
    lv_label_set_text(_status, "ĐANG KHỞI ĐỘNG...");
    lv_obj_set_style_text_color(_status, lv_color_hex(0xB8C5CF), 0);

    _rssi_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_rssi_lbl, f16, 0);
    lv_obj_set_style_text_color(_rssi_lbl, lv_color_hex(0xB8C5CF), 0);
    lv_obj_align(_rssi_lbl, LV_ALIGN_TOP_LEFT, RX, 118);
    lv_label_set_text(_rssi_lbl, "RSSI: -");

    _act_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_act_lbl, f16, 0);
    lv_obj_set_style_text_color(_act_lbl, lv_color_hex(0xB8C5CF), 0);
    lv_obj_align(_act_lbl, LV_ALIGN_TOP_LEFT, RX, 148);
    lv_label_set_text(_act_lbl, "Biến động: -");

    _ap_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_ap_lbl, f14, 0);
    lv_obj_set_style_text_color(_ap_lbl, lv_color_hex(0x8AA0B4), 0);
    lv_label_set_long_mode(_ap_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_ap_lbl, 560);
    lv_obj_align(_ap_lbl, LV_ALIGN_TOP_LEFT, RX, 178);
    lv_label_set_text(_ap_lbl, "AP: -");

    // Độ nhạy
    _sens_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_sens_lbl, f14, 0);
    lv_obj_set_style_text_color(_sens_lbl, lv_color_hex(0x8AA0B4), 0);
    lv_obj_align(_sens_lbl, LV_ALIGN_TOP_LEFT, RX, 212);
    lv_label_set_text(_sens_lbl, "Độ nhạy");
    lv_obj_t *sld = lv_slider_create(parent);
    lv_obj_set_size(sld, 560, 14);
    lv_obj_align(sld, LV_ALIGN_TOP_LEFT, RX, 238);
    lv_slider_set_range(sld, 0, 100);
    lv_slider_set_value(sld, 75, LV_ANIM_OFF);
    lv_obj_add_event_cb(sld, sensCb, LV_EVENT_VALUE_CHANGED, this);

    // START/STOP
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 160, 56);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, RX, 268);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2478D0), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, runBtnCb, LV_EVENT_CLICKED, this);
    _run_lbl = lv_label_create(btn);
    lv_label_set_text(_run_lbl, "DỪNG");
    lv_obj_set_style_text_font(_run_lbl, f16, 0);
    lv_obj_center(_run_lbl);

    // Đồ thị RSSI (dưới)
    _chart = lv_chart_create(parent);
    lv_obj_set_size(_chart, 944, 150);
    lv_obj_align(_chart, LV_ALIGN_TOP_LEFT, 40, 396);
    lv_obj_set_style_bg_color(_chart, lv_color_hex(0x0E1A26), 0);
    lv_obj_set_style_border_color(_chart, lv_color_hex(0x24405A), 0);
    lv_obj_set_style_border_width(_chart, 1, 0);
    lv_chart_set_type(_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(_chart, CHART_POINTS);
    lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, -100, -30);
    lv_chart_set_div_line_count(_chart, 4, 0);
    _ser = lv_chart_add_series(_chart, lv_color_hex(0x00D9FF), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < CHART_POINTS; ++i) lv_chart_set_next_value(_chart, _ser, LV_CHART_POINT_NONE);
}

// ---------------- sampling ----------------
void HCCWifiRadar::startSampling(void)
{
    if (_running) return;
    _running = true;
    xTaskCreatePinnedToCore(samplerTask, "wifiradar", 4096, this, 4, &_task, 0);
}

void HCCWifiRadar::stopSampling(void)
{
    _running = false;
    // task tự thoát; chờ ngắn để nó dọn xong.
    for (int i = 0; i < 20 && _task != nullptr; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

void HCCWifiRadar::samplerTask(void *arg)
{
    HCCWifiRadar *app = static_cast<HCCWifiRadar *>(arg);
    while (app->_running) {
        wifi_ap_record_t ap = {};
        bool ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        if (app->_lock && xSemaphoreTake(app->_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (ok) {
                app->_radar.push(ap.rssi, now_ms());
                app->_s_connected = true;
                strncpy(app->_s_ssid, (const char *)ap.ssid, sizeof(app->_s_ssid) - 1);
                app->_s_ssid[sizeof(app->_s_ssid) - 1] = 0;
            } else {
                app->_s_connected = false;
            }
            xSemaphoreGive(app->_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
    app->_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------- UI tick ----------------
void HCCWifiRadar::uiTickCb(lv_timer_t *t)
{
    HCCWifiRadar *app = static_cast<HCCWifiRadar *>(t->user_data);
    // snapshot
    if (app->_lock && xSemaphoreTake(app->_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        app->_s_rssi = app->_radar.rssi();
        app->_s_base = app->_radar.baseline();
        app->_s_act = app->_radar.activity();
        app->_s_p2p = app->_radar.p2p();
        app->_s_min = app->_radar.wmin();
        app->_s_max = app->_radar.wmax();
        app->_s_motion = app->_radar.motion();
        app->_s_level = app->_radar.level();
        xSemaphoreGive(app->_lock);
    }
    app->refreshUi();
}

void HCCWifiRadar::refreshUi(void)
{
    const float D2R = 3.14159265f / 180.0f;
    // quay tia quét
    _sweep_angle = (_sweep_angle + 6) % 360;
    float a = _sweep_angle * D2R;
    _sweep_pts[0] = {(lv_coord_t)_rcx, (lv_coord_t)_rcy};
    _sweep_pts[1] = {(lv_coord_t)(_rcx + _rR * cosf(a)), (lv_coord_t)(_rcy + _rR * sinf(a))};
    lv_line_set_points(_sweep, _sweep_pts, 2);
    // đuôi mờ dần phía sau tia
    for (int i = 0; i < kTrail; ++i) {
        float ta = (_sweep_angle - (i + 1) * 4) * D2R;
        _trail_pts[i][0] = {(lv_coord_t)_rcx, (lv_coord_t)_rcy};
        _trail_pts[i][1] = {(lv_coord_t)(_rcx + _rR * cosf(ta)), (lv_coord_t)(_rcy + _rR * sinf(ta))};
        lv_line_set_points(_trail[i], _trail_pts[i], 2);
        lv_opa_t op = (lv_opa_t)(110 * (kTrail - i) / kTrail);
        lv_obj_set_style_line_opa(_trail[i], op, 0);
    }

    uint32_t now = now_ms();

    // Sinh chấm mục tiêu khi có chuyển động (vị trí = tia quét lúc đó, xa theo mức).
    if (_s_connected && _s_motion && (now - _last_blip_ms) > 320) {
        _last_blip_ms = now;
        float jit = ((int)(rnd() % 20) - 10) * D2R;       // lệch góc ±10°
        float ba = a + jit;
        float rr = _rR * (0.30f + 0.60f * (_s_level / 100.0f));
        if (rr > _rR - 6) rr = _rR - 6;
        int bx = (int)(_rcx + rr * cosf(ba));
        int by = (int)(_rcy + rr * sinf(ba));
        int s = _blip_slot++ % kBlip;
        lv_obj_set_pos(_blips[s], bx - 8, by - 8);
        _blip_life[s] = 1.0f;
    }
    // Mờ dần tất cả chấm.
    for (int i = 0; i < kBlip; ++i) {
        if (_blip_life[i] > 0) {
            _blip_life[i] -= 0.035f;
            if (_blip_life[i] <= 0) {
                _blip_life[i] = 0;
                lv_obj_add_flag(_blips[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(_blips[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_opa(_blips[i], (lv_opa_t)(255 * _blip_life[i]), 0);
                int sz = 10 + (int)(10 * _blip_life[i]);
                lv_obj_set_size(_blips[i], sz, sz);
                lv_obj_set_style_radius(_blips[i], sz / 2, 0);
            }
        }
    }

    if (!_s_connected) {
        lv_label_set_text(_status, "CHƯA KẾT NỐI WIFI");
        lv_obj_set_style_text_color(_status, lv_color_hex(0xFFB347), 0);
        lv_label_set_text(_ap_lbl, "Vào Cài đặt kết nối WiFi để dò chuyển động");
        lv_obj_set_style_line_color(_sweep, lv_color_hex(0x3A5566), 0);
        return;
    }

    if (_s_motion) {
        lv_label_set_text(_status, LV_SYMBOL_WARNING " PHÁT HIỆN MỤC TIÊU");
        lv_obj_set_style_text_color(_status, lv_color_hex(0xFFD23F), 0);
        lv_obj_set_style_line_color(_sweep, lv_color_hex(0xFFE37A), 0);
    } else {
        lv_label_set_text(_status, LV_SYMBOL_OK " KHÔNG CÓ MỤC TIÊU");
        lv_obj_set_style_text_color(_status, lv_color_hex(0x2BE06A), 0);
        lv_obj_set_style_line_color(_sweep, lv_color_hex(0x8DFFB0), 0);
    }

    char b[80];
    snprintf(b, sizeof(b), "RSSI: %d dBm   (nền %d)", (int)lroundf(_s_rssi), (int)lroundf(_s_base));
    lv_label_set_text(_rssi_lbl, b);
    snprintf(b, sizeof(b), "Dao động: %d dB  (%d..%d)   mức %d%%",
             (int)lroundf(_s_p2p), _s_min, _s_max, _s_level);
    lv_label_set_text(_act_lbl, b);
    snprintf(b, sizeof(b), "AP: %s", _s_ssid[0] ? _s_ssid : "-");
    lv_label_set_text(_ap_lbl, b);

    lv_chart_set_next_value(_chart, _ser, (int)lroundf(_s_rssi));
}

// ---------------- callbacks ----------------
void HCCWifiRadar::sensCb(lv_event_t *e)
{
    HCCWifiRadar *app = static_cast<HCCWifiRadar *>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(lv_event_get_target(e));
    // phải = nhạy hơn = ngưỡng thấp hơn. Ngưỡng 0.6 .. 3.6 dB (nhạy hơn trước).
    float thr = 3.6f - (v / 100.0f) * 3.0f;
    if (app->_lock && xSemaphoreTake(app->_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        app->_radar.setThreshold(thr);
        xSemaphoreGive(app->_lock);
    }
    char b[48];
    snprintf(b, sizeof(b), "Độ nhạy (ngưỡng %.1f dB)", thr);
    lv_label_set_text(app->_sens_lbl, b);
}

void HCCWifiRadar::runBtnCb(lv_event_t *e)
{
    HCCWifiRadar *app = static_cast<HCCWifiRadar *>(lv_event_get_user_data(e));
    if (app->_running) {
        app->stopSampling();
        lv_label_set_text(app->_run_lbl, "CHẠY");
    } else {
        app->startSampling();
        lv_label_set_text(app->_run_lbl, "DỪNG");
    }
}
