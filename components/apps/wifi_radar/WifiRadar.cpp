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

    // ---- Radar tròn (trái) ----
    const int RS = 300;
    _radar_area = lv_obj_create(parent);
    lv_obj_set_size(_radar_area, RS, RS);
    lv_obj_align(_radar_area, LV_ALIGN_TOP_LEFT, 40, 70);
    lv_obj_set_style_bg_color(_radar_area, lv_color_hex(0x0A1A28), 0);
    lv_obj_set_style_radius(_radar_area, RS / 2, 0);
    lv_obj_set_style_border_color(_radar_area, lv_color_hex(0x16D0A0), 0);
    lv_obj_set_style_border_width(_radar_area, 2, 0);
    lv_obj_clear_flag(_radar_area, LV_OBJ_FLAG_SCROLLABLE);
    _rcx = RS / 2; _rcy = RS / 2; _rR = RS / 2 - 10;
    // vòng đồng tâm
    for (int k = 1; k <= 3; ++k) {
        int rr = _rR * k / 3;
        lv_obj_t *ring = lv_obj_create(_radar_area);
        lv_obj_set_size(ring, rr * 2, rr * 2);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ring, rr, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0x1C5E52), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }
    // đường quét
    _sweep = lv_line_create(_radar_area);
    lv_obj_set_style_line_color(_sweep, lv_color_hex(0x3DF2B0), 0);
    lv_obj_set_style_line_width(_sweep, 3, 0);
    _sweep_pts[0] = {(lv_coord_t)_rcx, (lv_coord_t)_rcy};
    _sweep_pts[1] = {(lv_coord_t)(_rcx + _rR), (lv_coord_t)_rcy};
    lv_line_set_points(_sweep, _sweep_pts, 2);
    // blip (chấm báo động)
    _blip = lv_obj_create(_radar_area);
    lv_obj_set_size(_blip, 18, 18);
    lv_obj_set_style_radius(_blip, 9, 0);
    lv_obj_set_style_bg_color(_blip, lv_color_hex(0xFF5C5C), 0);
    lv_obj_set_style_border_width(_blip, 0, 0);
    lv_obj_clear_flag(_blip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_blip, LV_OBJ_FLAG_HIDDEN);

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
    lv_slider_set_value(sld, 50, LV_ANIM_OFF);
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
        app->_s_motion = app->_radar.motion();
        app->_s_level = app->_radar.level();
        xSemaphoreGive(app->_lock);
    }
    app->refreshUi();
}

void HCCWifiRadar::refreshUi(void)
{
    // quét radar
    _sweep_angle = (_sweep_angle + 7) % 360;
    float a = _sweep_angle * 3.14159265f / 180.0f;
    _sweep_pts[0] = {(lv_coord_t)_rcx, (lv_coord_t)_rcy};
    _sweep_pts[1] = {(lv_coord_t)(_rcx + _rR * cosf(a)), (lv_coord_t)(_rcy + _rR * sinf(a))};
    lv_line_set_points(_sweep, _sweep_pts, 2);

    if (!_s_connected) {
        lv_label_set_text(_status, "CHƯA KẾT NỐI WIFI");
        lv_obj_set_style_text_color(_status, lv_color_hex(0xFFB347), 0);
        lv_label_set_text(_ap_lbl, "Vào Cài đặt kết nối WiFi để dò chuyển động");
        lv_obj_add_flag(_blip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_line_color(_sweep, lv_color_hex(0x3A5566), 0);
        return;
    }

    if (_s_motion) {
        lv_label_set_text(_status, LV_SYMBOL_WARNING " CÓ CHUYỂN ĐỘNG");
        lv_obj_set_style_text_color(_status, lv_color_hex(0xFF6B6B), 0);
        lv_obj_set_style_line_color(_sweep, lv_color_hex(0xFF6B6B), 0);
        // blip ở đầu đường quét
        lv_obj_set_pos(_blip, _sweep_pts[1].x - 9, _sweep_pts[1].y - 9);
        lv_obj_clear_flag(_blip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(_status, LV_SYMBOL_OK " YÊN TĨNH");
        lv_obj_set_style_text_color(_status, lv_color_hex(0x3DD66E), 0);
        lv_obj_set_style_line_color(_sweep, lv_color_hex(0x3DF2B0), 0);
        lv_obj_add_flag(_blip, LV_OBJ_FLAG_HIDDEN);
    }

    char b[64];
    snprintf(b, sizeof(b), "RSSI: %d dBm   (nền %d)", (int)lroundf(_s_rssi), (int)lroundf(_s_base));
    lv_label_set_text(_rssi_lbl, b);
    snprintf(b, sizeof(b), "Biến động: %.1f dB   mức %d%%", _s_act, _s_level);
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
    // phải = nhạy hơn = ngưỡng thấp hơn. Ngưỡng 0.8 .. 6.0 dB.
    float thr = 6.0f - (v / 100.0f) * 5.2f;
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
