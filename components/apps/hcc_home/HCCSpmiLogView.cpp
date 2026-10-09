#include "HCCSpmiLogView.hpp"

#include <stdio.h>
#include <string.h>
#include <vector>

#include "HCCRffe.hpp"
#include "HCCSpmiLog.hpp"

namespace HCCSpmiLogView {

using HCCSpmiDecode::Txn;

namespace {

const lv_color_t C_BG = lv_color_hex(0x101820);
const lv_color_t C_PANEL = lv_color_hex(0x0A1016);
const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
const lv_color_t C_MUTED = lv_color_hex(0xB8C5CF);
const lv_color_t C_CYAN = lv_color_hex(0x35C2E0);
const lv_color_t C_OK = lv_color_hex(0x3DDC84);
const lv_color_t C_WARN = lv_color_hex(0xF0A04B);
const lv_color_t C_BAD = lv_color_hex(0xFF5A5A);
const lv_color_t C_BTN = lv_color_hex(0x2A3A48);

const int SECONDS[] = {10, 30, 60, 90};
const uint32_t RATES[] = {80000000, 40000000, 20000000};
constexpr int PAGE = 100;  // số dòng mỗi trang

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *start_btn = nullptr;
    lv_obj_t *start_lbl = nullptr;
    lv_obj_t *bar = nullptr;
    lv_obj_t *info = nullptr;
    lv_obj_t *table = nullptr;
    lv_obj_t *tab_txn = nullptr;
    lv_obj_t *tab_stat = nullptr;
    lv_obj_t *page_lbl = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *save_btn = nullptr;
    lv_timer_t *timer = nullptr;
    bool stats_tab = false;
    int page = 0;           // trang giao dịch; -1 = trang cuối (theo dõi khi đang bắt)
    uint32_t shown = 0;     // số giao dịch lúc vẽ bảng gần nhất
    bool was_running = false;
    bool was_saving = false;
};

View v;
int s_sec = 1;
int s_rate = 0;

lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *button(lv_obj_t *parent, const char *t, int w, lv_color_t bg, lv_event_cb_t cb, void *ud,
                 lv_obj_t **lbl = nullptr)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 48);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, t, &lv_font_be_vietnam_18, C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lbl) {
        *lbl = l;
    }
    return b;
}

lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int sel, int w, lv_event_cb_t cb)
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
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

void setStatus(const char *t, lv_color_t c)
{
    lv_label_set_text(v.status, t);
    lv_obj_set_style_text_color(v.status, c, 0);
}

void fmtTime(char *b, size_t n, uint32_t ms)
{
    snprintf(b, n, "%02u:%02u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60));
}

void setupTable(int cols, const int *widths, const char *const *heads)
{
    lv_table_set_col_cnt(v.table, cols);
    for (int c = 0; c < cols; ++c) {
        lv_table_set_col_width(v.table, c, widths[c]);
        lv_table_set_cell_value(v.table, 0, c, heads[c]);
    }
}

void renderTxns(void)
{
    const uint32_t n = HCCSpmiLog::count();
    const uint32_t pages = n == 0 ? 1 : (n + PAGE - 1) / PAGE;
    int page = v.page < 0 ? (int)pages - 1 : v.page;
    if (page >= (int)pages) {
        page = (int)pages - 1;
    }
    const uint32_t first = (uint32_t)page * PAGE;
    const uint32_t last = first + PAGE < n ? first + PAGE : n;
    static const int W[] = {78, 118, 56, 66, 108, 92, 260, 168};
    static const char *const H[] = {"#", "t (ms)", "SID", "Lệnh", "Loại", "Địa chỉ", "Dữ liệu", "Kiểm tra"};
    lv_table_set_row_cnt(v.table, 1);  // xoá dòng cũ trước khi đổi số cột
    setupTable(8, W, H);
    lv_table_set_row_cnt(v.table, 1 + (last - first));
    const double ms_per = 1e3 / (double)(HCCSpmiLog::sampleHz() ? HCCSpmiLog::sampleHz() : 1);
    char b[64];
    for (uint32_t i = first; i < last; ++i) {
        const Txn *t = HCCSpmiLog::txn(i);
        if (t == nullptr) {
            break;
        }
        const uint16_t r = (uint16_t)(1 + i - first);
        snprintf(b, sizeof(b), "%u", (unsigned)(i + 1));
        lv_table_set_cell_value(v.table, r, 0, b);
        snprintf(b, sizeof(b), "%.3f", t->t_sample * ms_per);
        lv_table_set_cell_value(v.table, r, 1, b);
        snprintf(b, sizeof(b), "%X", t->sid);
        lv_table_set_cell_value(v.table, r, 2, b);
        snprintf(b, sizeof(b), "%02X", t->cmd);
        lv_table_set_cell_value(v.table, r, 3, b);
        lv_table_set_cell_value(v.table, r, 4, HCCSpmiDecode::kindName(t->kind));
        const bool has_addr = t->kind != HCCSpmiDecode::K_REG0_WRITE && t->kind < HCCSpmiDecode::K_RESET;
        if (has_addr) {
            snprintf(b, sizeof(b), t->addr > 0xFF ? "%04X" : "%02X", t->addr);
        } else {
            b[0] = '\0';
        }
        lv_table_set_cell_value(v.table, r, 5, b);
        int k = 0;
        b[0] = '\0';
        for (int j = 0; j < t->len && j < 8; ++j) {
            k += snprintf(b + k, sizeof(b) - k, "%s%02X", j ? " " : "", t->data[j]);
        }
        lv_table_set_cell_value(v.table, r, 6, b);
        if (t->kind == HCCSpmiDecode::K_BAD) {
            snprintf(b, sizeof(b), "lỗi khung lệnh");
        } else if (t->parity_err) {
            snprintf(b, sizeof(b), "sai parity (%u)", t->parity_err);
        } else if (t->short_bits) {
            snprintf(b, sizeof(b), "thiếu bit (%u)", t->nbits);
        } else {
            snprintf(b, sizeof(b), "OK");
        }
        lv_table_set_cell_value(v.table, r, 7, b);
    }
    snprintf(b, sizeof(b), "Trang %d / %u", page + 1, (unsigned)pages);
    lv_label_set_text(v.page_lbl, b);
    v.shown = n;
}

void renderStats(void)
{
    std::vector<HCCSpmiLog::Stat> st = HCCSpmiLog::stats();
    const uint32_t n = (uint32_t)st.size();
    const uint32_t pages = n == 0 ? 1 : (n + PAGE - 1) / PAGE;
    int page = v.page < 0 ? 0 : v.page;
    if (page >= (int)pages) {
        page = (int)pages - 1;
    }
    const uint32_t first = (uint32_t)page * PAGE;
    const uint32_t last = first + PAGE < n ? first + PAGE : n;
    static const int W[] = {60, 120, 100, 100, 150, 150, 150, 116};
    static const char *const H[] = {"SID", "Địa chỉ", "Số ghi", "Số đọc", "Ghi đầu", "Ghi cuối", "Đọc cuối", "Kiểu"};
    lv_table_set_row_cnt(v.table, 1);
    setupTable(8, W, H);
    lv_table_set_row_cnt(v.table, 1 + (last - first));
    char b[32];
    for (uint32_t i = first; i < last; ++i) {
        const HCCSpmiLog::Stat &s = st[i];
        const uint16_t r = (uint16_t)(1 + i - first);
        snprintf(b, sizeof(b), "%X", s.sid);
        lv_table_set_cell_value(v.table, r, 0, b);
        snprintf(b, sizeof(b), s.ext ? "%04X" : "%02X", s.addr);
        lv_table_set_cell_value(v.table, r, 1, b);
        snprintf(b, sizeof(b), "%u", (unsigned)s.writes);
        lv_table_set_cell_value(v.table, r, 2, b);
        snprintf(b, sizeof(b), "%u", (unsigned)s.reads);
        lv_table_set_cell_value(v.table, r, 3, b);
        snprintf(b, sizeof(b), s.has_w ? "%02X" : "-", s.first_w);
        lv_table_set_cell_value(v.table, r, 4, b);
        snprintf(b, sizeof(b), s.has_w ? "%02X" : "-", s.last_w);
        lv_table_set_cell_value(v.table, r, 5, b);
        snprintf(b, sizeof(b), s.has_r ? "%02X" : "-", s.last_r);
        lv_table_set_cell_value(v.table, r, 6, b);
        lv_table_set_cell_value(v.table, r, 7, s.ext ? "Extended" : "Thường");
    }
    snprintf(b, sizeof(b), "Trang %d / %u", page + 1, (unsigned)pages);
    lv_label_set_text(v.page_lbl, b);
}

void render(void)
{
    if (v.stats_tab) {
        renderStats();
    } else {
        renderTxns();
    }
    lv_obj_set_style_bg_color(v.tab_txn, v.stats_tab ? C_BTN : lv_color_hex(0x1FA9D6), 0);
    lv_obj_set_style_bg_color(v.tab_stat, v.stats_tab ? lv_color_hex(0x1FA9D6) : C_BTN, 0);
}

void updateInfo(const HCCSpmiLog::Status &st)
{
    char a[16], b2[16], info[256];
    fmtTime(a, sizeof(a), st.elapsed_ms);
    fmtTime(b2, sizeof(b2), st.total_ms);
    int n = snprintf(info, sizeof(info), "%s / %s   |   %u giao dịch", a, b2, (unsigned)st.txns);
    if (st.sclk_mhz > 0) {
        n += snprintf(info + n, sizeof(info) - n, "   |   SCLK ~%.2f MHz", st.sclk_mhz);
    }
    n += snprintf(info + n, sizeof(info) - n, "   |   sai parity %u   lỗi khung %u   thiếu bit %u",
                  (unsigned)st.parity_errs, (unsigned)st.bad_frames, (unsigned)st.short_frames);
    if (st.overruns) {
        n += snprintf(info + n, sizeof(info) - n, "   |   mất dữ liệu %u lần", (unsigned)st.overruns);
    }
    if (st.dropped_txns) {
        snprintf(info + n, sizeof(info) - n, "   |   đầy bộ nhớ, bỏ %u", (unsigned)st.dropped_txns);
    }
    lv_label_set_text(v.info, info);
    lv_bar_set_value(v.bar, st.total_ms ? (int32_t)(st.elapsed_ms * 100 / st.total_ms) : 0, LV_ANIM_OFF);
}

void onTimer(lv_timer_t *)
{
    const HCCSpmiLog::Status st = HCCSpmiLog::status();
    if (st.running) {
        updateInfo(st);
        if (!st.lines_found) {
            setStatus("Đang nghe... chưa thấy tín hiệu - cấp nguồn / bật main", C_WARN);
        } else {
            char b[96];
            snprintf(b, sizeof(b), "Đang nghe - SCLK ở GPIO%d", st.swapped ? HCCRffe::sdataPin() : HCCRffe::sclkPin());
            setStatus(b, C_CYAN);
        }
        // Đang bắt: theo dõi trang cuối mỗi ~1 s.
        static uint32_t tick = 0;
        if (!v.stats_tab && v.page < 0 && ++tick % 3 == 0 && HCCSpmiLog::count() != v.shown) {
            renderTxns();
        }
    } else if (v.was_running) {
        updateInfo(st);
        lv_label_set_text(v.start_lbl, "BẮT ĐẦU");
        lv_obj_set_style_bg_color(v.start_btn, lv_color_hex(0x1E8E4E), 0);
        setStatus(st.msg, st.lines_found ? C_OK : C_WARN);
        render();
    }
    v.was_running = st.running;
    if (v.was_saving && !HCCSpmiLog::saving()) {
        char m[160];
        const bool ok = HCCSpmiLog::saveResult(m, sizeof(m));
        setStatus(m, ok ? C_OK : C_BAD);
        lv_obj_clear_state(v.save_btn, LV_STATE_DISABLED);
    }
    v.was_saving = HCCSpmiLog::saving();
}

void onStart(lv_event_t *)
{
    if (HCCSpmiLog::status().running) {
        HCCSpmiLog::stop();
        setStatus("Đang dừng...", C_WARN);
        return;
    }
    HCCSpmiLog::Config cfg;
    cfg.seconds = SECONDS[s_sec];
    cfg.sample_hz = RATES[s_rate];
    char err[128];
    if (!HCCSpmiLog::start(cfg, err, sizeof(err))) {
        setStatus(err, C_BAD);
        return;
    }
    v.page = -1;
    v.stats_tab = false;
    v.was_running = true;
    lv_label_set_text(v.start_lbl, "DỪNG");
    lv_obj_set_style_bg_color(v.start_btn, lv_color_hex(0xC0392B), 0);
    setStatus("Đang nghe... cấp nguồn / bật main ngay bây giờ", C_CYAN);
    render();
}

void onTab(lv_event_t *e)
{
    v.stats_tab = (intptr_t)lv_event_get_user_data(e) != 0;
    v.page = v.stats_tab ? 0 : (HCCSpmiLog::status().running ? -1 : 0);
    render();
}

void onPage(lv_event_t *e)
{
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (v.page < 0) {
        const uint32_t n = HCCSpmiLog::count();
        v.page = n == 0 ? 0 : (int)((n - 1) / PAGE);
    }
    v.page += d;
    if (v.page < 0) {
        v.page = 0;
    }
    render();  // render tự kẹp trang cuối
    lv_obj_scroll_to_y(v.table, 0, LV_ANIM_OFF);
}

void onSave(lv_event_t *)
{
    if (HCCSpmiLog::count() == 0) {
        setStatus("Chưa có giao dịch để lưu", C_WARN);
        return;
    }
    if (!HCCSpmiLog::startSave()) {
        setStatus("Không lưu được lúc này (đang bắt / đang lưu)", C_WARN);
        return;
    }
    v.was_saving = true;
    lv_obj_add_state(v.save_btn, LV_STATE_DISABLED);
    setStatus("Đang lưu CSV ra thẻ SD...", C_CYAN);
}

void onSec(lv_event_t *e)
{
    s_sec = lv_dropdown_get_selected(lv_event_get_target(e));
}

void onRate(lv_event_t *e)
{
    s_rate = lv_dropdown_get_selected(lv_event_get_target(e));
}

void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
    }
    v = View();  // việc bắt (nếu đang chạy) vẫn tiếp tục ở nền; mở lại màn sẽ thấy kết quả
}

}  // namespace

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user)
{
    if (v.root) {
        lv_obj_del(v.root);
    }
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_t *root = lv_obj_create(screen);
    v.root = root;
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

    lv_obj_t *t = label(root, "BẮT DỮ LIỆU SPMI", &lv_font_be_vietnam_28, C_TEXT);
    lv_obj_set_pos(t, 28, 10);
    char pins[128];
    snprintf(pins, sizeof(pins), "Chỉ nghe, không gửi lên bus  |  GPIO%d + GPIO%d (tự nhận SCLK / SDATA)  |  cần chuyển mức",
             HCCRffe::sclkPin(), HCCRffe::sdataPin());
    lv_obj_t *pl = label(root, pins, &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(pl, 330, 22);

    const int y1 = 52;
    lv_obj_t *l1 = label(root, "Thời gian", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(l1, 28, y1 + 14);
    lv_obj_t *dd1 = dropdown(root, "10 giây\n30 giây\n60 giây\n90 giây", s_sec, 130, onSec);
    lv_obj_set_pos(dd1, 104, y1);
    lv_obj_t *l2 = label(root, "Lấy mẫu", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(l2, 250, y1 + 14);
    lv_obj_t *dd2 = dropdown(root, "80 MHz\n40 MHz\n20 MHz", s_rate, 130, onRate);
    lv_obj_set_pos(dd2, 322, y1);
    v.start_btn = button(root, "BẮT ĐẦU", 170, lv_color_hex(0x1E8E4E), onStart, nullptr, &v.start_lbl);
    lv_obj_set_pos(v.start_btn, 470, y1);
    v.bar = lv_bar_create(root);
    lv_obj_set_size(v.bar, W - 28 - 660, 14);
    lv_obj_set_pos(v.bar, 660, y1 + 17);
    lv_obj_set_style_bg_color(v.bar, C_BTN, 0);
    lv_obj_set_style_bg_color(v.bar, C_OK, LV_PART_INDICATOR);

    v.info = label(root, "", &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_width(v.info, W - 56);
    lv_label_set_long_mode(v.info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v.info, 28, y1 + 56);

    const int y2 = y1 + 82;
    v.tab_txn = button(root, "GIAO DỊCH", 150, C_BTN, onTab, (void *)0);
    lv_obj_set_pos(v.tab_txn, 28, y2);
    v.tab_stat = button(root, "THỐNG KÊ", 150, C_BTN, onTab, (void *)1);
    lv_obj_set_pos(v.tab_stat, 186, y2);
    lv_obj_t *pp = button(root, "<", 52, C_BTN, onPage, (void *)(intptr_t)-1);
    lv_obj_set_pos(pp, W - 28 - 52 - 8 - 52 - 120, y2);
    v.page_lbl = label(root, "", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_width(v.page_lbl, 116);
    lv_obj_set_style_text_align(v.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v.page_lbl, W - 28 - 52 - 120 - 2, y2 + 14);
    lv_obj_t *pn = button(root, ">", 52, C_BTN, onPage, (void *)(intptr_t)1);
    lv_obj_set_pos(pn, W - 28 - 52, y2);

    const int ty = y2 + 56;
    v.table = lv_table_create(root);
    lv_obj_set_pos(v.table, 28, ty);
    lv_obj_set_size(v.table, W - 56, H - ty - 74);
    lv_obj_set_style_bg_color(v.table, C_PANEL, 0);
    lv_obj_set_style_bg_color(v.table, C_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.table, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(v.table, &lv_font_be_vietnam_14, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(v.table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(v.table, 6, LV_PART_ITEMS);
    lv_obj_set_style_border_color(v.table, lv_color_hex(0x1E2A35), LV_PART_ITEMS);
    lv_obj_set_style_border_width(v.table, 0, 0);

    const int yb = H - 64;
    lv_obj_t *back = button(root, "BACK", 150, C_BTN, back_cb, back_user);
    lv_obj_set_pos(back, 28, yb);
    v.save_btn = button(root, "LƯU CSV", 150, lv_color_hex(0x7A4FD8), onSave, nullptr);
    lv_obj_set_pos(v.save_btn, 194, yb);
    v.status = label(root, "Bấm BẮT ĐẦU rồi cấp nguồn / bật main trong thời gian đã chọn.", &lv_font_be_vietnam_16,
                     C_CYAN);
    lv_obj_set_width(v.status, W - 380);
    lv_label_set_long_mode(v.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(v.status, 362, yb + 4);

    const HCCSpmiLog::Status st = HCCSpmiLog::status();
    v.was_running = st.running;
    v.page = st.running ? -1 : 0;
    if (st.running) {
        lv_label_set_text(v.start_lbl, "DỪNG");
        lv_obj_set_style_bg_color(v.start_btn, lv_color_hex(0xC0392B), 0);
    }
    if (st.running || st.done) {
        updateInfo(st);
        if (st.done && !st.running) {
            setStatus(st.msg, st.lines_found ? C_OK : C_WARN);
        }
    }
    render();
    v.timer = lv_timer_create(onTimer, 330, nullptr);
}

}  // namespace HCCSpmiLogView
