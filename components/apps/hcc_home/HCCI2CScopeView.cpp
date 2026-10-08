#include "HCCI2CScopeView.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace HCCI2CScopeView {

using HCCI2CScope::Event;

namespace {

const lv_color_t C_BG = lv_color_hex(0x101820);
const lv_color_t C_PANEL = lv_color_hex(0x0A1016);
const lv_color_t C_GRID = lv_color_hex(0x1E2A35);
const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
const lv_color_t C_MUTED = lv_color_hex(0xB8C5CF);
const lv_color_t C_SCL = lv_color_hex(0xF5C542);
const lv_color_t C_SDA = lv_color_hex(0x35C2E0);
const lv_color_t C_OK = lv_color_hex(0x3DDC84);
const lv_color_t C_BAD = lv_color_hex(0xFF5A5A);
const lv_color_t C_BYTE = lv_color_hex(0x2B4A66);
const lv_color_t C_ORANGE = lv_color_hex(0xF0A04B);
const lv_color_t C_BTN = lv_color_hex(0x2A3A48);

constexpr int MAX_W = 7600;  // LVGL 8: toạ độ tối đa 8191 -> chỉ vẽ một cửa sổ mẫu quanh vùng xem
constexpr float MAX_PPS = 16.0f;
constexpr int Y_BAND = 6;     // dải giải mã
constexpr int BAND_H = 30;
constexpr int Y_SCL_HI = 52, Y_SCL_LO = 102;
constexpr int Y_SDA_HI = 136, Y_SDA_LO = 186;
constexpr int Y_RULER = 200;
const float PERIODS[] = {0.5f, 1.0f, 2.0f, 5.0f, 10.0f};

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *mode_dd = nullptr;
    lv_obj_t *period_dd = nullptr;
    lv_obj_t *addr_label = nullptr;
    lv_obj_t *cap_btn = nullptr;
    lv_obj_t *cont_sw = nullptr;
    lv_obj_t *panel = nullptr;    // khung cuộn ngang
    lv_obj_t *content = nullptr;  // nội dung rộng theo thu phóng
    lv_obj_t *info = nullptr;
    lv_obj_t *status = nullptr;
    lv_timer_t *timer = nullptr;
    HCCI2CScope::BusGetter get_bus = nullptr;
    void *bus_user = nullptr;

    std::vector<uint8_t> s;  // bản chụp đang hiển thị
    float period_us = 0.5f;
    int trigger = -1;
    std::vector<Event> ev;
    float pps = 1.0f;  // điểm ảnh / mẫu
    int win0 = 0;      // cửa sổ mẫu đang vẽ [win0, win1)
    int win1 = 0;
    bool have = false;
    uint32_t seen = 0;
    uint32_t last_start_ms = 0;
    std::vector<lv_point_t> pts_scl, pts_sda;
};

View v;
uint8_t s_addr = 0x18;  // nhớ giữa các lần mở
int s_mode = 0;
int s_period = 0;

lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *button(lv_obj_t *parent, const char *t, int w, int h, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, t, &lv_font_be_vietnam_18, C_TEXT);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }
    return b;
}

void setStatus(const char *t, lv_color_t c)
{
    if (v.status) {
        lv_label_set_text(v.status, t);
        lv_obj_set_style_text_color(v.status, c, 0);
    }
}

int panelInnerW(void)
{
    return lv_obj_get_content_width(v.panel);
}

float maxPps(void)
{
    return MAX_PPS;
}

inline lv_coord_t xOf(int i)
{
    return (lv_coord_t)lroundf((i - v.win0) * v.pps);
}

void tracePoints(std::vector<lv_point_t> &pts, uint8_t bit, int y_hi, int y_lo)
{
    pts.clear();
    if (v.s.empty()) {
        return;
    }
    int lvl = (v.s[v.win0] & bit) ? 1 : 0;
    pts.push_back({0, (lv_coord_t)(lvl ? y_hi : y_lo)});
    for (int i = v.win0 + 1; i < v.win1; ++i) {
        const int n = (v.s[i] & bit) ? 1 : 0;
        if (n != lvl) {
            const lv_coord_t x = xOf(i);
            pts.push_back({x, (lv_coord_t)(lvl ? y_hi : y_lo)});
            pts.push_back({x, (lv_coord_t)(n ? y_hi : y_lo)});
            lvl = n;
        }
    }
    pts.push_back({xOf(v.win1), (lv_coord_t)(lvl ? y_hi : y_lo)});
}

lv_obj_t *makeLine(std::vector<lv_point_t> &pts, lv_color_t c)
{
    lv_obj_t *l = lv_line_create(v.content);
    lv_line_set_points(l, pts.data(), (uint16_t)pts.size());
    lv_obj_set_style_line_width(l, 2, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_pos(l, 0, 0);
    return l;
}

void bandBox(int x0, int x1, const char *text, lv_color_t bg, lv_color_t fg)
{
    int w = x1 - x0;
    if (w < 4) {
        w = 4;
    }
    lv_obj_t *b = lv_obj_create(v.content);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x0, Y_BAND);
    lv_obj_set_size(b, w, BAND_H);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (text && w >= (int)strlen(text) * 9 + 4) {
        lv_obj_t *l = label(b, text, &lv_font_be_vietnam_16, fg);
        lv_obj_center(l);
    }
}

// Nhãn chữ nhỏ tại x (S / P / A / N): khung hẹp, chữ luôn hiện.
void bandMark(int x, const char *text, lv_color_t c)
{
    lv_obj_t *l = label(v.content, text, &lv_font_be_vietnam_16, c);
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, x - lv_obj_get_width(l) / 2, Y_BAND + 4);
    lv_obj_t *tick = lv_obj_create(v.content);
    lv_obj_remove_style_all(tick);
    lv_obj_set_pos(tick, x, Y_BAND + BAND_H);
    lv_obj_set_size(tick, 1, Y_SDA_LO - Y_BAND - BAND_H);
    lv_obj_set_style_bg_color(tick, c, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_40, 0);
}

void render(int keep_center_sample)
{
    lv_obj_clean(v.content);  // xoá line trước khi đổi mảng điểm
    v.pts_scl.clear();
    v.pts_sda.clear();
    if (v.s.empty()) {
        lv_obj_set_width(v.content, panelInnerW());
        return;
    }
    if (v.pps > maxPps()) {
        v.pps = maxPps();
    }
    // Cửa sổ mẫu quanh tâm xem, rộng tối đa MAX_W điểm ảnh.
    const int n = (int)v.s.size();
    int span = (int)(MAX_W / v.pps);
    if (span > n) {
        span = n;
    }
    const int center = keep_center_sample >= 0 ? keep_center_sample : (v.win0 + v.win1) / 2;
    v.win0 = center - span / 2;
    if (v.win0 < 0) {
        v.win0 = 0;
    }
    if (v.win0 + span > n) {
        v.win0 = n - span;
    }
    v.win1 = v.win0 + span;
    const int width = (int)lroundf(span * v.pps);
    lv_obj_set_size(v.content, width < panelInnerW() ? panelInnerW() : width, Y_RULER + 26);

    // Thước thời gian: vạch ~ mỗi >= 90 px.
    static const float STEPS[] = {0.5f, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    const float px_per_us = v.pps / v.period_us;
    float step = STEPS[sizeof(STEPS) / sizeof(STEPS[0]) - 1];
    for (float st : STEPS) {
        if (st * px_per_us >= 90) {
            step = st;
            break;
        }
    }
    const float t_end = v.win1 * v.period_us;
    for (float t = ceilf(v.win0 * v.period_us / step) * step; t <= t_end; t += step) {
        const int x = (int)lroundf((t / v.period_us - v.win0) * v.pps);
        lv_obj_t *g = lv_obj_create(v.content);
        lv_obj_remove_style_all(g);
        lv_obj_set_pos(g, x, Y_SCL_HI - 10);
        lv_obj_set_size(g, 1, Y_RULER - Y_SCL_HI + 10);
        lv_obj_set_style_bg_color(g, C_GRID, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        char b[24];
        if (step >= 1000) {
            snprintf(b, sizeof(b), "%g ms", t / 1000.0f);
        } else {
            snprintf(b, sizeof(b), "%g µs", t);
        }
        lv_obj_t *l = label(v.content, b, &lv_font_be_vietnam_14, C_MUTED);
        lv_obj_set_pos(l, x + 3, Y_RULER + 2);
    }
    // Điểm kích.
    if (v.trigger >= v.win0 && v.trigger < v.win1) {
        lv_obj_t *tg = lv_obj_create(v.content);
        lv_obj_remove_style_all(tg);
        lv_obj_set_pos(tg, xOf(v.trigger), Y_BAND + BAND_H);
        lv_obj_set_size(tg, 2, Y_RULER - Y_BAND - BAND_H);
        lv_obj_set_style_bg_color(tg, C_ORANGE, 0);
        lv_obj_set_style_bg_opa(tg, LV_OPA_60, 0);
    }

    tracePoints(v.pts_scl, HCCI2CScope::BIT_SCL, Y_SCL_HI, Y_SCL_LO);
    tracePoints(v.pts_sda, HCCI2CScope::BIT_SDA, Y_SDA_HI, Y_SDA_LO);
    makeLine(v.pts_scl, C_SCL);
    makeLine(v.pts_sda, C_SDA);

    for (const Event &e : v.ev) {
        if (e.i1 < v.win0 || e.i0 >= v.win1) {
            continue;
        }
        const int x0 = xOf(e.i0 < v.win0 ? v.win0 : e.i0);
        const int x1 = xOf(e.i1 >= v.win1 ? v.win1 - 1 : e.i1);
        char b[24];
        switch (e.type) {
        case Event::Start:
            bandMark(x0, "S", C_OK);
            break;
        case Event::Restart:
            bandMark(x0, "Sr", C_OK);
            break;
        case Event::Stop:
            bandMark(x0, "P", C_BAD);
            break;
        case Event::Ack:
            bandMark(x0, "A", C_OK);
            break;
        case Event::Nack:
            bandMark(x0, "N", C_BAD);
            break;
        case Event::Byte:
            if (e.addr) {
                snprintf(b, sizeof(b), "0x%02X %s", e.value >> 1, (e.value & 1) ? "R" : "W");
            } else {
                snprintf(b, sizeof(b), "0x%02X", e.value);
            }
            bandBox(x0, x1, b, C_BYTE, C_TEXT);
            break;
        }
    }
    int sx = (int)lroundf((center - v.win0) * v.pps) - panelInnerW() / 2;
    if (sx < 0) {
        sx = 0;
    }
    lv_obj_update_layout(v.panel);
    lv_obj_scroll_to_x(v.panel, sx, LV_ANIM_OFF);
}

int centerSample(void)
{
    if (v.pps <= 0) {
        return 0;
    }
    return v.win0 + (int)((lv_obj_get_scroll_x(v.panel) + panelInnerW() / 2) / v.pps);
}

// Kéo tới gần mép cửa sổ đang vẽ: vẽ lại cửa sổ mới quanh vị trí xem.
void onPanelScrollEnd(lv_event_t *)
{
    if (!v.have) {
        return;
    }
    const int sx = lv_obj_get_scroll_x(v.panel);
    const int w = lv_obj_get_width(v.content);
    const int pw = panelInnerW();
    const bool near_left = sx < pw / 2 && v.win0 > 0;
    const bool near_right = sx + pw > w - pw / 2 && v.win1 < (int)v.s.size();
    if (near_left || near_right) {
        render(centerSample());
    }
}

// Vừa khung với vùng có tín hiệu (cạnh đầu - cạnh cuối, thêm lề).
void fitActivity(void)
{
    int a = -1, b = -1;
    for (size_t i = 1; i < v.s.size(); ++i) {
        if (v.s[i] != v.s[i - 1]) {
            if (a < 0) {
                a = (int)i;
            }
            b = (int)i;
        }
    }
    if (a < 0) {
        a = 0;
        b = (int)v.s.size();
    }
    int span = b - a;
    const int margin = span / 10 + 10;
    a -= margin;
    b += margin;
    if (a < 0) {
        a = 0;
    }
    if (b > (int)v.s.size()) {
        b = (int)v.s.size();
    }
    span = b - a;
    v.pps = span > 0 ? (float)panelInnerW() / span : 1.0f;
    render((a + b) / 2);
}

void startCapture(void)
{
    if (HCCI2CScope::busy()) {
        return;
    }
    HCCI2CScope::Request r;
    r.mode = s_mode == 0 ? HCCI2CScope::Mode::Probe : HCCI2CScope::Mode::Passive;
    r.addr = s_addr;
    r.period_us = PERIODS[s_period];
    v.seen = HCCI2CScope::serial();  // trước start: kết quả cũ không bị coi là mới
    if (HCCI2CScope::start(r, v.get_bus, v.bus_user)) {
        v.last_start_ms = lv_tick_get();
        setStatus(r.mode == HCCI2CScope::Mode::Probe ? "Đang chụp..." : "Đang nghe bus (tối đa 3 s)...", C_ORANGE);
    }
}

void applyResult(void)
{
    const HCCI2CScope::Capture &c = HCCI2CScope::result();
    const bool first = !v.have;
    const int center = v.have ? centerSample() : -1;
    v.s = c.s;
    v.period_us = c.period_us > 0 ? c.period_us : PERIODS[s_period];
    v.trigger = c.trigger;
    v.ev = HCCI2CScope::decode(v.s.data(), (int)v.s.size());
    v.have = !v.s.empty();
    const float f = HCCI2CScope::sclFrequency(v.s.data(), (int)v.s.size(), v.period_us);
    char info[256];
    int n = snprintf(info, sizeof(info), "%s", c.msg);
    if (f > 0) {
        n += snprintf(info + n, sizeof(info) - n, "   |   SCL ~%.0f kHz", f / 1000.0f);
    }
    snprintf(info + n, sizeof(info) - n, "   |   %g µs/mẫu, %d mẫu = %.2f ms", v.period_us, (int)v.s.size(),
             v.s.size() * v.period_us / 1000.0f);
    lv_label_set_text(v.info, info);
    const bool ok = c.trigger >= 0;
    setStatus(ok ? "Xong" : "Không có sóng", ok ? C_OK : C_ORANGE);
    if (first || center < 0) {
        fitActivity();
    } else {
        render(center);
    }
}

void onTimer(lv_timer_t *)
{
    if (v.root == nullptr) {
        return;
    }
    if (!HCCI2CScope::busy() && HCCI2CScope::serial() != v.seen) {
        v.seen = HCCI2CScope::serial();
        applyResult();
    }
    if (v.cont_sw && lv_obj_has_state(v.cont_sw, LV_STATE_CHECKED) && !HCCI2CScope::busy() &&
        lv_tick_elaps(v.last_start_ms) > 600) {
        startCapture();
    }
}

void updateAddr(void)
{
    char b[16];
    snprintf(b, sizeof(b), "0x%02X", s_addr);
    lv_label_set_text(v.addr_label, b);
}

void onAddr(lv_event_t *e)
{
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    int a = s_addr + d;
    if (a < 0x08) {
        a = 0x77;
    } else if (a > 0x77) {
        a = 0x08;
    }
    s_addr = (uint8_t)a;
    updateAddr();
}

void onCapture(lv_event_t *)
{
    startCapture();
}

void onZoom(lv_event_t *e)
{
    if (!v.have) {
        return;
    }
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    const int center = centerSample();
    float p = d > 0 ? v.pps * 2 : v.pps / 2;
    const float min_pps = (float)panelInnerW() / v.s.size();
    if (p < min_pps) {
        p = min_pps;
    }
    if (p > maxPps()) {
        p = maxPps();
    }
    v.pps = p;
    render(center);
}

void onFit(lv_event_t *)
{
    if (v.have) {
        fitActivity();
    }
}

void onMode(lv_event_t *e)
{
    s_mode = lv_dropdown_get_selected(lv_event_get_target(e));
    lv_obj_t *ag = (lv_obj_t *)lv_event_get_user_data(e);
    if (s_mode == 0) {
        lv_obj_clear_state(ag, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(ag, LV_STATE_DISABLED);
    }
}

void onPeriod(lv_event_t *e)
{
    s_period = lv_dropdown_get_selected(lv_event_get_target(e));
}

void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
    }
    v = View();
}

lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int sel, int w, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_size(dd, w, 48);
    lv_obj_set_style_text_font(dd, &lv_font_be_vietnam_18, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &lv_font_be_vietnam_20, 0);
    lv_obj_set_style_bg_color(dd, C_BTN, 0);
    lv_obj_set_style_text_color(dd, C_TEXT, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, ud);
    return dd;
}

}  // namespace

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user, HCCI2CScope::BusGetter get_bus, void *bus_user)
{
    if (v.root) {
        lv_obj_del(v.root);
    }
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_t *root = lv_obj_create(screen);
    v.root = root;
    v.get_bus = get_bus;
    v.bus_user = bus_user;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    lv_coord_t W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    if (W < 600 || W > 1280) {
        W = 1024;
    }
    if (H < 360 || H > 800) {
        H = 520;
    }

    lv_obj_t *t = label(root, "SÓNG I2C", &lv_font_be_vietnam_28, C_TEXT);
    lv_obj_set_pos(t, 28, 12);
    char pins[96];
    snprintf(pins, sizeof(pins), "SCL GPIO%d", HCCI2CScope::sclPin());
    lv_obj_t *ls = label(root, pins, &lv_font_be_vietnam_18, C_SCL);
    lv_obj_set_pos(ls, 210, 20);
    snprintf(pins, sizeof(pins), "SDA GPIO%d", HCCI2CScope::sdaPin());
    lv_obj_t *ld = label(root, pins, &lv_font_be_vietnam_18, C_SDA);
    lv_obj_set_pos(ld, 330, 20);
    lv_obj_t *lv = label(root, "bus 1,8 V cần mạch chuyển mức", &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(lv, 460, 24);

    // Hàng điều khiển.
    const int y1 = 58;
    lv_obj_t *addr_grp = lv_obj_create(root);
    lv_obj_remove_style_all(addr_grp);
    lv_obj_set_size(addr_grp, 196, 48);
    v.mode_dd = dropdown(root, "Probe địa chỉ\nNghe bus ngoài", s_mode, 210, onMode, addr_grp);
    lv_obj_set_pos(v.mode_dd, 28, y1);
    lv_obj_set_pos(addr_grp, 250, y1);
    lv_obj_t *bm = button(addr_grp, "-", 52, 48, C_BTN, onAddr, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(bm, onAddr, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
    lv_obj_set_pos(bm, 0, 0);
    v.addr_label = label(addr_grp, "", &lv_font_be_vietnam_22, C_TEXT);
    lv_obj_set_width(v.addr_label, 84);
    lv_obj_set_style_text_align(v.addr_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v.addr_label, 56, 10);
    lv_obj_t *bp = button(addr_grp, "+", 52, 48, C_BTN, onAddr, (void *)(intptr_t)1);
    lv_obj_add_event_cb(bp, onAddr, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);
    lv_obj_set_pos(bp, 144, 0);
    updateAddr();
    if (s_mode != 0) {
        lv_obj_add_state(addr_grp, LV_STATE_DISABLED);
    }
    v.period_dd = dropdown(root, "0,5 µs/mẫu\n1 µs/mẫu\n2 µs/mẫu\n5 µs/mẫu\n10 µs/mẫu", s_period, 160, onPeriod, nullptr);
    lv_obj_set_pos(v.period_dd, 462, y1);
    v.cap_btn = button(root, "CHỤP", 130, 48, lv_color_hex(0x1FA9D6), onCapture, nullptr);
    lv_obj_set_pos(v.cap_btn, 638, y1);
    v.cont_sw = lv_switch_create(root);
    lv_obj_set_size(v.cont_sw, 60, 32);
    lv_obj_set_pos(v.cont_sw, 790, y1 + 8);
    lv_obj_t *lc = label(root, "Liên tục", &lv_font_be_vietnam_18, C_MUTED);
    lv_obj_set_pos(lc, 860, y1 + 12);

    // Khung sóng (cuộn ngang).
    const int py = 118;
    const int ph = Y_RULER + 26 + 16;
    lv_obj_t *names = lv_obj_create(root);
    lv_obj_remove_style_all(names);
    lv_obj_set_pos(names, 28, py);
    lv_obj_set_size(names, 52, ph);
    lv_obj_t *n1 = label(names, "SCL", &lv_font_be_vietnam_18, C_SCL);
    lv_obj_set_pos(n1, 0, 8 + (Y_SCL_HI + Y_SCL_LO) / 2 - 12);
    lv_obj_t *n2 = label(names, "SDA", &lv_font_be_vietnam_18, C_SDA);
    lv_obj_set_pos(n2, 0, 8 + (Y_SDA_HI + Y_SDA_LO) / 2 - 12);
    lv_obj_t *n3 = label(names, "I2C", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(n3, 0, 8 + Y_BAND + 6);

    v.panel = lv_obj_create(root);
    lv_obj_remove_style_all(v.panel);
    lv_obj_set_pos(v.panel, 84, py);
    lv_obj_set_size(v.panel, W - 84 - 28, ph);
    lv_obj_set_style_bg_color(v.panel, C_PANEL, 0);
    lv_obj_set_style_bg_opa(v.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(v.panel, 10, 0);
    lv_obj_set_style_pad_all(v.panel, 8, 0);
    lv_obj_set_scroll_dir(v.panel, LV_DIR_HOR);
    lv_obj_add_flag(v.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(v.panel, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_event_cb(v.panel, onPanelScrollEnd, LV_EVENT_SCROLL_END, nullptr);
    v.content = lv_obj_create(v.panel);
    lv_obj_remove_style_all(v.content);
    lv_obj_set_pos(v.content, 0, 0);
    lv_obj_set_size(v.content, W - 84 - 28 - 16, Y_RULER + 26);
    lv_obj_clear_flag(v.content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v.content, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_t *hint = label(v.content,
                           "Bấm CHỤP để xem sóng. Probe: máy gửi địa chỉ đã chọn trên bus và chụp lại; "
                           "Nghe bus ngoài: chờ mạch khác hoạt động.",
                           &lv_font_be_vietnam_18, C_MUTED);
    lv_obj_set_width(hint, W - 84 - 28 - 40);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(hint, 8, 80);

    v.info = label(root, "", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_width(v.info, W - 56);
    lv_label_set_long_mode(v.info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v.info, 28, py + ph + 8);

    // Hàng dưới.
    const int yb = H - 70;
    lv_obj_t *back = button(root, "BACK", 160, 52, C_BTN, back_cb, back_user);
    lv_obj_set_pos(back, 28, yb);
    lv_obj_t *zo = button(root, "-", 64, 52, C_BTN, onZoom, (void *)(intptr_t)-1);
    lv_obj_set_pos(zo, 210, yb);
    lv_obj_t *zi = button(root, "+", 64, 52, C_BTN, onZoom, (void *)(intptr_t)1);
    lv_obj_set_pos(zi, 282, yb);
    lv_obj_t *fit = button(root, "VỪA", 100, 52, C_BTN, onFit, nullptr);
    lv_obj_set_pos(fit, 354, yb);
    lv_obj_t *zl = label(root, "Thu phóng / kéo ngang để xem", &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(zl, 470, yb + 18);
    v.status = label(root, "Sẵn sàng", &lv_font_be_vietnam_18, C_MUTED);
    lv_obj_set_pos(v.status, 760, yb + 14);

    v.seen = HCCI2CScope::serial();
    v.timer = lv_timer_create(onTimer, 100, nullptr);
}

}  // namespace HCCI2CScopeView
