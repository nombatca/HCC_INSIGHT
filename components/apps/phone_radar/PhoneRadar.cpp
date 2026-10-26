#include "PhoneRadar.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "vn_fonts.h"

LV_IMG_DECLARE(img_app_phone_radar);

static const char *TAG = "PhoneRadar";

static inline uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

PhoneRadar::PhoneRadar()
    : ESP_Brookesia_PhoneApp("Radar thiet bi", &img_app_phone_radar, true) {}

PhoneRadar::~PhoneRadar() {}

bool PhoneRadar::init(void) { return true; }

// ---- Quét WiFi AP (chạy qua esp-hosted) ----
void PhoneRadar::doWifiScan(uint32_t t)
{
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = true;
    esp_err_t r = esp_wifi_scan_start(&cfg, true);  // blocking
    _last_scan_err = (int)r;
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %d", (int)r);
        return;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return;
    if (num > 40) num = 40;
    static wifi_ap_record_t recs[40];
    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) return;
    if (_lock) xSemaphoreTake(_lock, portMAX_DELAY);
    for (int i = 0; i < (int)num; ++i) {
        const char *ssid = (const char *)recs[i].ssid;
        uint8_t kind = PhoneRadarCore::looksLikePhone(ssid) ? PR_HOTSPOT : PR_AP;
        _core.observe(recs[i].bssid, recs[i].rssi, ssid, recs[i].primary, kind, t);
    }
    if (_lock) xSemaphoreGive(_lock);
}

void PhoneRadar::scanTask(void *arg)
{
    PhoneRadar *app = static_cast<PhoneRadar *>(arg);
    while (app->_running) {
        uint32_t t = now_ms();
        app->doWifiScan(t);
        if (app->_lock) {
            xSemaphoreTake(app->_lock, portMAX_DELAY);
            app->_core.age(t, 15000);
            xSemaphoreGive(app->_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    app->_task = nullptr;
    vTaskDelete(nullptr);
}

void PhoneRadar::startScanning(void)
{
    if (_running) return;
    _running = true;
    if (_lock == nullptr) _lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(scanTask, "pradar", 5120, this, 4, &_task, 0);
}

void PhoneRadar::stopScanning(void)
{
    _running = false;
    for (int i = 0; i < 60 && _task != nullptr; ++i) vTaskDelay(pdMS_TO_TICKS(50));
}

// ============================ UI ============================

void PhoneRadar::buildUi(lv_obj_t *parent)
{
    const lv_font_t *f14 = &lv_font_be_vietnam_14;
    const lv_font_t *f16 = &lv_font_be_vietnam_16;
    const lv_font_t *f18 = &lv_font_be_vietnam_18;
    const lv_font_t *f22 = &lv_font_be_vietnam_22;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x05140C), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h1 = lv_label_create(parent);
    lv_label_set_text(h1, LV_SYMBOL_GPS "  RADAR THIẾT BỊ");
    lv_obj_set_style_text_font(h1, f22, 0);
    lv_obj_set_style_text_color(h1, lv_color_hex(0x7CF6B0), 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 14);
    lv_obj_t *h2 = lv_label_create(parent);
    lv_label_set_text(h2, "quet wifi / hotspot dien thoai o gan theo cuong do song");
    lv_obj_set_style_text_font(h2, f14, 0);
    lv_obj_set_style_text_color(h2, lv_color_hex(0x4E8A69), 0);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 26, 46);

    // ---- Vùng radar (vuông, trái) ----
    const int RA = 384;
    lv_obj_t *ra = lv_obj_create(parent);
    _radar_area = ra;
    lv_obj_set_size(ra, RA, RA);
    lv_obj_align(ra, LV_ALIGN_TOP_LEFT, 24, 80);
    lv_obj_set_style_bg_color(ra, lv_color_hex(0x08241A), 0);
    lv_obj_set_style_border_color(ra, lv_color_hex(0x1C5E3E), 0);
    lv_obj_set_style_border_width(ra, 1, 0);
    lv_obj_set_style_radius(ra, 10, 0);
    lv_obj_set_style_pad_all(ra, 0, 0);
    lv_obj_clear_flag(ra, LV_OBJ_FLAG_SCROLLABLE);

    _radar_cx = RA / 2;
    _radar_cy = RA / 2;
    _radar_r = RA / 2 - 12;

    for (int k = 1; k <= 3; ++k) {
        int rr = _radar_r * k / 3;
        lv_obj_t *ring = lv_obj_create(ra);
        lv_obj_set_size(ring, rr * 2, rr * 2);
        lv_obj_set_pos(ring, _radar_cx - rr, _radar_cy - rr);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0x1E6B45), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }
    for (int a = 0; a < 2; ++a) {
        lv_obj_t *ln = lv_obj_create(ra);
        if (a == 0) { lv_obj_set_size(ln, _radar_r * 2, 1); lv_obj_set_pos(ln, _radar_cx - _radar_r, _radar_cy); }
        else        { lv_obj_set_size(ln, 1, _radar_r * 2); lv_obj_set_pos(ln, _radar_cx, _radar_cy - _radar_r); }
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x154A3A), 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_set_style_pad_all(ln, 0, 0);
    }

    _sweep = lv_line_create(ra);
    static lv_style_t st_sweep;
    static bool st_init = false;
    if (!st_init) {
        lv_style_init(&st_sweep);
        lv_style_set_line_width(&st_sweep, 2);
        lv_style_set_line_color(&st_sweep, lv_color_hex(0x59F6A6));
        lv_style_set_line_rounded(&st_sweep, true);
        st_init = true;
    }
    lv_obj_add_style(_sweep, &st_sweep, 0);
    _sweep_pts[0] = (lv_point_t){(lv_coord_t)_radar_cx, (lv_coord_t)_radar_cy};
    _sweep_pts[1] = (lv_point_t){(lv_coord_t)_radar_cx, (lv_coord_t)(_radar_cy - _radar_r)};
    lv_line_set_points(_sweep, _sweep_pts, 2);

    for (int i = 0; i < kBlip; ++i) {
        lv_obj_t *b = lv_obj_create(ra);
        lv_obj_set_size(b, 12, 12);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        _blip[i] = b;
    }

    // ---- Bảng phải ----
    const int RX = 430;
    _count_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_count_lbl, f22, 0);
    lv_obj_set_style_text_color(_count_lbl, lv_color_hex(0xE6FFF2), 0);
    lv_obj_align(_count_lbl, LV_ALIGN_TOP_LEFT, RX, 84);
    lv_label_set_text(_count_lbl, "Tong: 0");

    _phone_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(_phone_lbl, f18, 0);
    lv_obj_set_style_text_color(_phone_lbl, lv_color_hex(0x7CF6B0), 0);
    lv_obj_align(_phone_lbl, LV_ALIGN_TOP_LEFT, RX, 118);
    lv_label_set_text(_phone_lbl, "Hotspot dien thoai: 0");

    _hint_lbl = lv_label_create(parent);
    lv_label_set_long_mode(_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_hint_lbl, 1024 - RX - 24);
    lv_obj_set_style_text_font(_hint_lbl, f14, 0);
    lv_obj_set_style_text_color(_hint_lbl, lv_color_hex(0x9FD8BE), 0);
    lv_obj_align(_hint_lbl, LV_ALIGN_TOP_LEFT, RX, 148);
    lv_label_set_text(_hint_lbl, "Bam QUET de bat dau.");

    _list = lv_obj_create(parent);
    lv_obj_set_size(_list, 1024 - RX - 24, 236);
    lv_obj_align(_list, LV_ALIGN_TOP_LEFT, RX, 182);
    lv_obj_set_style_bg_color(_list, lv_color_hex(0x08241A), 0);
    lv_obj_set_style_border_color(_list, lv_color_hex(0x1C5E3E), 0);
    lv_obj_set_style_border_width(_list, 1, 0);
    lv_obj_set_style_radius(_list, 8, 0);
    lv_obj_set_style_pad_all(_list, 8, 0);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 12; ++i) {
        lv_obj_t *r = lv_label_create(_list);
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r, 1024 - RX - 24 - 16);
        lv_obj_set_style_text_font(r, f14, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(0xCDE8DA), 0);
        lv_obj_set_pos(r, 2, i * 18);
        lv_label_set_text(r, "");
        _rows[i] = r;
    }

    // ---- Nút QUÉT/DỪNG ----
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 300, 52);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 24, -18);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C7A4E), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, runBtnCb, LV_EVENT_CLICKED, this);
    _run_lbl = lv_label_create(b);
    lv_label_set_text(_run_lbl, LV_SYMBOL_PLAY "  QUET");
    lv_obj_center(_run_lbl);
    lv_obj_set_style_text_font(_run_lbl, f16, 0);

    // Chú thích màu.
    lv_obj_t *lg = lv_label_create(parent);
    lv_label_set_recolor(lg, true);
    lv_obj_set_style_text_font(lg, f14, 0);
    lv_obj_align(lg, LV_ALIGN_BOTTOM_LEFT, 340, -30);
    lv_label_set_text(lg, "#37D0FF Hotspot DT#   #8AA6B5 Router/IoT#");
}

void PhoneRadar::refreshUi(void)
{
    _sweep_angle = (_sweep_angle + 6) % 360;
    float a = _sweep_angle * (float)M_PI / 180.0f;
    _sweep_pts[0] = (lv_point_t){(lv_coord_t)_radar_cx, (lv_coord_t)_radar_cy};
    _sweep_pts[1] = (lv_point_t){(lv_coord_t)(_radar_cx + (int)(sinf(a) * _radar_r)),
                                 (lv_coord_t)(_radar_cy - (int)(cosf(a) * _radar_r))};
    lv_line_set_points(_sweep, _sweep_pts, 2);

    PhoneRadarTarget snap[PhoneRadarCore::kMax];
    int n = 0, total = 0, phones = 0;
    if (_lock && xSemaphoreTake(_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        total = _core.count();
        phones = _core.countKind(PR_HOTSPOT);
        for (int i = 0; i < total && n < PhoneRadarCore::kMax; ++i) snap[n++] = _core.at(i);
        xSemaphoreGive(_lock);
    } else {
        return;
    }

    for (int i = 0; i < kBlip; ++i) {
        if (i < n) {
            float rad = PhoneRadarCore::radiusForRssi(snap[i].rssi) * _radar_r;
            float ang = snap[i].angle_deg * (float)M_PI / 180.0f;
            int x = _radar_cx + (int)(sinf(ang) * rad);
            int y = _radar_cy - (int)(cosf(ang) * rad);
            uint32_t col = (snap[i].kind == PR_HOTSPOT) ? 0x37D0FF : 0x8AA6B5;
            int sz = (snap[i].kind == PR_HOTSPOT) ? 14 : 10;
            lv_obj_set_size(_blip[i], sz, sz);
            lv_obj_set_pos(_blip[i], x - sz / 2, y - sz / 2);
            lv_obj_set_style_bg_color(_blip[i], lv_color_hex(col), 0);
            lv_obj_clear_flag(_blip[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_blip[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    int order[PhoneRadarCore::kMax];
    for (int i = 0; i < n; ++i) order[i] = i;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (snap[order[j]].rssi > snap[order[i]].rssi) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }

    for (int r = 0; r < 12; ++r) {
        if (r < n) {
            const PhoneRadarTarget &t = snap[order[r]];
            const char *tag = (t.kind == PR_HOTSPOT) ? "HS" : "AP";
            char line[96];
            const char *nm = (t.name[0] != '\0') ? t.name : "(an)";
            snprintf(line, sizeof(line), "%s  %s  %d dBm  ch%d", tag, nm, (int)lroundf(t.rssi), t.channel);
            lv_label_set_text(_rows[r], line);
        } else {
            lv_label_set_text(_rows[r], "");
        }
    }

    char cb[32]; snprintf(cb, sizeof(cb), "Tong: %d", total); lv_label_set_text(_count_lbl, cb);
    char pb[48]; snprintf(pb, sizeof(pb), "Hotspot dien thoai: %d", phones); lv_label_set_text(_phone_lbl, pb);

    if (!_running) {
        lv_label_set_text(_hint_lbl, "Da dung. Bam QUET de bat dau.");
    } else if (_last_scan_err != 0) {
        lv_label_set_text(_hint_lbl, "Loi quet WiFi (WiFi chua bat?). Vao Cai dat bat WiFi roi thu lai.");
    } else {
        lv_label_set_text(_hint_lbl, "Dang quet. Dien thoai chi hien khi bat Hotspot (board khong sniff duoc may client).");
    }
}

void PhoneRadar::uiTickCb(lv_timer_t *t)
{
    PhoneRadar *app = static_cast<PhoneRadar *>(t->user_data);
    if (app) app->refreshUi();
}

void PhoneRadar::runBtnCb(lv_event_t *e)
{
    PhoneRadar *app = static_cast<PhoneRadar *>(lv_event_get_user_data(e));
    if (!app) return;
    if (app->_running) {
        app->stopScanning();
        if (app->_run_lbl) lv_label_set_text(app->_run_lbl, LV_SYMBOL_PLAY "  QUET");
    } else {
        app->_core.reset();
        app->startScanning();
        if (app->_run_lbl) lv_label_set_text(app->_run_lbl, LV_SYMBOL_STOP "  DUNG");
    }
}

bool PhoneRadar::run(void)
{
    lv_obj_t *screen = lv_scr_act();
    buildUi(screen);
    if (_tick == nullptr) _tick = lv_timer_create(uiTickCb, 80, this);
    return true;
}

bool PhoneRadar::back(void)
{
    notifyCoreClosed();
    return true;
}

bool PhoneRadar::close(void)
{
    stopScanning();
    if (_tick) { lv_timer_del(_tick); _tick = nullptr; }
    for (int i = 0; i < kBlip; ++i) _blip[i] = nullptr;
    _sweep = nullptr; _radar_area = nullptr; _list = nullptr;
    return true;
}
