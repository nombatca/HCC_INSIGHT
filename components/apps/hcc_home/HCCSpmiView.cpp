#include "HCCSpmiView.hpp"

#include <stdio.h>

#include "HCCRffe.hpp"
#include "HCCSpmiLogView.hpp"

namespace HCCSpmiView {

namespace {

const lv_color_t C_BG = lv_color_hex(0x101820);
const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
const lv_color_t C_MUTED = lv_color_hex(0xB8C5CF);
const lv_color_t C_IDLE = lv_color_hex(0x2A3A48);
const lv_color_t C_OK = lv_color_hex(0x1E8E4E);
const lv_color_t C_WARN = lv_color_hex(0xF0A04B);
const lv_color_t C_CYAN = lv_color_hex(0x35C2E0);
const lv_color_t C_BTN = lv_color_hex(0x2A3A48);

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *tiles = nullptr;
    lv_obj_t *summary = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *scan_btn = nullptr;
    lv_timer_t *timer = nullptr;
    uint32_t seen = 0;
    bool waiting = false;
};

View v;
lv_obj_t *s_screen = nullptr;  // để quay lại từ BẮT DỮ LIỆU SPMI
lv_event_cb_t s_back_cb = nullptr;
void *s_back_user = nullptr;

lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *button(lv_obj_t *parent, const char *t, int w, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 52);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, t, &lv_font_be_vietnam_20, C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void setStatus(const char *t, lv_color_t c)
{
    lv_label_set_text(v.status, t);
    lv_obj_set_style_text_color(v.status, c, 0);
}

void render(void)
{
    lv_obj_clean(v.tiles);
    const HCCRffe::SpmiReport &rep = HCCRffe::spmiReport();
    for (int sid = 0; sid < 16; ++sid) {
        const HCCRffe::SpmiSid *r = (rep.scanned && sid < (int)rep.sids.size()) ? &rep.sids[sid] : nullptr;
        const bool ok = r && r->responded;
        lv_obj_t *t = lv_obj_create(v.tiles);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, 228, 92);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, ok ? C_OK : C_IDLE, 0);
        lv_obj_set_style_pad_all(t, 10, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        char b[64];
        snprintf(b, sizeof(b), "SID %d (0x%X)", sid, sid);
        lv_obj_t *h = label(t, b, &lv_font_be_vietnam_20, C_TEXT);
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 0);
        if (ok) {
            snprintf(b, sizeof(b), "0x00 = 0x%02X", r->r0);
            lv_obj_t *l1 = label(t, b, &lv_font_be_vietnam_16, C_TEXT);
            lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 30);
            if (r->long_ok) {
                snprintf(b, sizeof(b), "0x0100: %02X %02X %02X %02X %02X %02X", r->ext[0], r->ext[1], r->ext[2],
                         r->ext[3], r->ext[4], r->ext[5]);
            } else {
                snprintf(b, sizeof(b), "0x0100: không đọc được");
            }
            lv_obj_t *l2 = label(t, b, &lv_font_be_vietnam_14, C_TEXT);
            lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 52);
        } else {
            lv_obj_t *l1 = label(t, rep.scanned ? "Không thấy" : "Chưa quét", &lv_font_be_vietnam_16, C_MUTED);
            lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 34);
        }
    }
    if (!rep.scanned) {
        lv_label_set_text(v.summary, "");
        return;
    }
    int found = 0;
    for (const HCCRffe::SpmiSid &s : rep.sids) {
        found += s.responded ? 1 : 0;
    }
    char sum[64];
    snprintf(sum, sizeof(sum), "Thấy %d / 16 SID", found);
    lv_label_set_text(v.summary, sum);
}

void onScan(lv_event_t *)
{
    if (!HCCRffe::configured()) {
        setStatus(HCCRffe::configError(), C_WARN);
        return;
    }
    v.seen = HCCRffe::spmiSerial();  // ghi trước khi bắt đầu: kết quả cũ không bị coi là mới
    if (!HCCRffe::startSpmiScan()) {
        setStatus("Đang bận (đang quét RFFE / SPMI)", C_WARN);
        return;
    }
    v.waiting = true;
    lv_obj_add_state(v.scan_btn, LV_STATE_DISABLED);
    setStatus("Đang quét SPMI...", C_CYAN);
}

void onTimer(lv_timer_t *)
{
    if (!v.waiting || HCCRffe::busy() || HCCRffe::spmiSerial() == v.seen) {
        return;
    }
    v.waiting = false;
    v.seen = HCCRffe::spmiSerial();
    lv_obj_clear_state(v.scan_btn, LV_STATE_DISABLED);
    const HCCRffe::SpmiReport &rep = HCCRffe::spmiReport();
    int found = 0;
    for (const HCCRffe::SpmiSid &s : rep.sids) {
        found += s.responded ? 1 : 0;
    }
    setStatus(rep.message, found > 0 ? C_CYAN : C_WARN);
    render();
}

void onCaptureBack(lv_event_t *)
{
    lv_obj_t *scr = s_screen;
    lv_obj_clean(scr);
    show(scr, s_back_cb, s_back_user);
}

void onCapture(lv_event_t *)
{
    if (HCCRffe::busy() && !v.waiting) {
        setStatus("Đang bận (đang quét RFFE / SPMI)", C_WARN);
        return;
    }
    lv_obj_t *scr = s_screen;
    lv_obj_clean(scr);
    HCCSpmiLogView::show(scr, onCaptureBack, nullptr);
}

void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
    }
    v = View();
}

}  // namespace

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user)
{
    if (v.root) {
        lv_obj_del(v.root);
    }
    s_screen = screen;
    s_back_cb = back_cb;
    s_back_user = back_user;
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_t *root = lv_obj_create(screen);
    v.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    lv_coord_t H = lv_obj_get_height(root);
    if (H < 360 || H > 800) {
        H = 520;
    }

    lv_obj_t *title = label(root, "SPMI", &lv_font_be_vietnam_28, C_TEXT);
    lv_obj_set_pos(title, 28, 14);
    char pins[128];
    if (HCCRffe::configured()) {
        snprintf(pins, sizeof(pins), "SCLK GPIO %d   SDATA GPIO %d (dùng chung với RFFE, tự đảo nếu nối ngược)",
                 HCCRffe::sclkPin(), HCCRffe::sdataPin());
    } else {
        snprintf(pins, sizeof(pins), "%s", HCCRffe::configError());
    }
    lv_obj_t *pl = label(root, pins, &lv_font_be_vietnam_16, HCCRffe::configured() ? C_MUTED : C_WARN);
    lv_obj_set_pos(pl, 120, 24);
    v.summary = label(root, "", &lv_font_be_vietnam_18, C_CYAN);
    lv_obj_set_pos(v.summary, 28, 58);

    const int ty = 88;
    v.tiles = lv_obj_create(root);
    lv_obj_remove_style_all(v.tiles);
    lv_obj_set_pos(v.tiles, 24, ty);
    lv_obj_set_size(v.tiles, 976, H - ty - 84);
    lv_obj_set_flex_flow(v.tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(v.tiles, 4, 0);
    lv_obj_set_style_pad_gap(v.tiles, 10, 0);
    lv_obj_set_scroll_dir(v.tiles, LV_DIR_VER);

    const int yb = H - 70;
    lv_obj_t *back = button(root, "BACK", 160, C_BTN, back_cb, back_user);
    lv_obj_set_pos(back, 28, yb);
    v.scan_btn = button(root, "SCAN", 200, lv_color_hex(0x1FA9D6), onScan, nullptr);
    lv_obj_set_pos(v.scan_btn, 206, yb);
    lv_obj_t *sn = button(root, "NGHE BUS", 200, lv_color_hex(0x7A4FD8), onCapture, nullptr);
    lv_obj_set_pos(sn, 424, yb);
    v.status = label(root, "Sẵn sàng", &lv_font_be_vietnam_16, C_CYAN);
    lv_obj_set_width(v.status, 360);
    lv_label_set_long_mode(v.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(v.status, 642, yb + 4);

    render();
    v.timer = lv_timer_create(onTimer, 100, nullptr);
}

}  // namespace HCCSpmiView
