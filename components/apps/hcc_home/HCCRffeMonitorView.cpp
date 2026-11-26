#include "HCCRffeMonitorView.hpp"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>

#include "HCCLayout.hpp"
#include "HCCRffeDecode.hpp"
#include "HCCRffeMonitor.hpp"
#include "HCCSpmiMonitor.hpp"
#include "HCCSpmiSource.hpp"

namespace HCCRffeMonitorView {

namespace rf = HCCRffeDecode;
namespace rm = HCCRffeMonitor;
using HCCSpmiMonitor::Snapshot;
using HCCSpmiMonitor::Src;
using HCCSpmiMonitor::State;
using Txn = rf::Txn;

static const lv_color_t C_BG = lv_color_hex(0x101820);
static const lv_color_t C_PANEL = lv_color_hex(0x0B141C);
static const lv_color_t C_PANEL2 = lv_color_hex(0x13202B);
static const lv_color_t C_BORDER = lv_color_hex(0x304553);
static const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
static const lv_color_t C_MUTED = lv_color_hex(0xB8C5CF);
static const lv_color_t C_CYAN = lv_color_hex(0x00D9FF);
static const lv_color_t C_GREEN = lv_color_hex(0x3DDC84);
static const lv_color_t C_RED = lv_color_hex(0xFF5A5F);
static const lv_color_t C_ORANGE = lv_color_hex(0xFFB020);
static const lv_color_t C_BLUE = lv_color_hex(0x4C9AFF);

constexpr int PAGE = 50;
constexpr int COLS = 7;  // USID | IC | TIME | CMD | REGISTER | DATA | STATUS

struct V {
    lv_obj_t *root = nullptr;
    lv_obj_t *screen = nullptr;
    lv_event_cb_t back_cb = nullptr;
    void *back_user = nullptr;

    lv_obj_t *state_lbl = nullptr;
    lv_obj_t *info_lbl = nullptr;
    lv_obj_t *count_lbl = nullptr;
    lv_obj_t *msg = nullptr;
    lv_obj_t *stats = nullptr;
    lv_obj_t *rate_dd = nullptr;
    lv_obj_t *usid_dd = nullptr;
    lv_obj_t *ic_dd = nullptr;
    lv_obj_t *rd_btn = nullptr;
    lv_obj_t *wr_btn = nullptr;
    lv_obj_t *err_btn = nullptr;
    lv_obj_t *follow_btn = nullptr;
    lv_obj_t *page_lbl = nullptr;
    lv_obj_t *table = nullptr;
    lv_timer_t *timer = nullptr;

    rm::UsidMap map;
    std::vector<HCCRffe::RefEntry> ref;
    rm::Filter fil;
    std::vector<uint32_t> idx;  // chỉ số gói đã lọc
    uint32_t scanned = 0;       // số gói đã quét để dựng idx
    uint32_t page = 0;
    bool follow = true;

    uint32_t shown_page = UINT32_MAX;
    uint32_t shown_rows = UINT32_MAX;
    uint32_t shown_total = UINT32_MAX;
    uint32_t stats_seen = UINT32_MAX;
    int rate_sel = 2;  // 0=20 1=40 2=80 MHz
    char ic_options[16][24];
    int ic_count = 0;
};

static V v;

// ==================== widget tiện ích ====================

static lv_obj_t *label(lv_obj_t *p, const char *t, lv_color_t c, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_font(l, f, 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *p, const char *t, int w, int h, lv_color_t c, lv_event_cb_t cb, void *u)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, c, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, u);
    lv_obj_t *l = label(b, t, C_TEXT, &lv_font_be_vietnam_16);
    lv_obj_center(l);
    return b;
}

static void setToggle(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, on ? C_BLUE : C_BORDER, 0);
}

// ==================== lọc + bảng ====================

static bool match(const Txn &t) { return v.fil.pass(t, v.map); }

static void resetFilter(void)
{
    v.idx.clear();
    v.scanned = 0;
}

static void scanNew(void)
{
    const auto &pk = HCCSpmiMonitor::packets();
    const uint32_t n = pk.count();
    if (n < v.scanned) {  // bộ đệm đã bị clear
        resetFilter();
    }
    for (uint32_t i = v.scanned; i < n; ++i) {
        if (match(pk.at(i))) {
            v.idx.push_back(i);
        }
    }
    v.scanned = n;
}

static void fillTable(bool force)
{
    const uint32_t total = (uint32_t)v.idx.size();
    const uint32_t pages = total ? (total + PAGE - 1) / PAGE : 1;
    if (v.follow && pages > 0) {
        v.page = pages - 1;
    }
    if (v.page >= pages) {
        v.page = pages - 1;
    }
    const uint32_t first = v.page * PAGE;
    const uint32_t rows = total > first ? std::min<uint32_t>(PAGE, total - first) : 0;

    char pl[48];
    snprintf(pl, sizeof(pl), "Trang %u/%u  (%u gói)", (unsigned)(v.page + 1), (unsigned)pages, (unsigned)total);
    lv_label_set_text(v.page_lbl, pl);

    if (!force && v.shown_page == v.page && v.shown_rows == rows && v.shown_total == total) {
        return;
    }
    v.shown_page = v.page;
    v.shown_rows = rows;
    v.shown_total = total;

    lv_table_set_row_cnt(v.table, (rows ? rows : 1) + 1);  // +1 hàng tiêu đề
    const char *hdr[COLS] = {"USID", "IC", "TIME (ms)", "CMD", "REG", "DATA", "STATUS"};
    for (int c = 0; c < COLS; ++c) {
        lv_table_set_cell_value(v.table, 0, c, hdr[c]);
    }
    if (rows == 0) {
        for (int c = 0; c < COLS; ++c) {
            lv_table_set_cell_value(v.table, 1, c, c == 0 ? "—" : "");
        }
        return;
    }
    const uint32_t sample_hz = HCCSpmiMonitor::sampleHz();
    const auto &pk = HCCSpmiMonitor::packets();
    for (uint32_t r = 0; r < rows; ++r) {
        const Txn &t = pk.at(v.idx[first + r]);
        rm::Row row;
        rm::format(t, v.map, sample_hz, row);
        const uint16_t tr = (uint16_t)(r + 1);
        lv_table_set_cell_value(v.table, tr, 0, row.usid);
        lv_table_set_cell_value(v.table, tr, 1, row.ic);
        lv_table_set_cell_value(v.table, tr, 2, row.time);
        lv_table_set_cell_value(v.table, tr, 3, row.cmd);
        lv_table_set_cell_value(v.table, tr, 4, row.reg);
        lv_table_set_cell_value(v.table, tr, 5, row.data);
        lv_table_set_cell_value(v.table, tr, 6, row.status);
    }
}

static void onTableDraw(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_PART_BEGIN) {
        return;
    }
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->part != LV_PART_ITEMS) {
        return;
    }
    const uint32_t row = d->id / COLS;
    if (row == 0) {  // tiêu đề
        d->label_dsc->color = C_CYAN;
        return;
    }
    const uint32_t k = v.page * PAGE + (row - 1);
    if (k >= v.idx.size()) {
        return;
    }
    const Txn &t = HCCSpmiMonitor::packets().at(v.idx[k]);
    const rf::Kind kind = rf::kindOf(t.cmd);
    const rf::PktStatus st = rf::status(t);
    if (st != rf::ST_OK && st != rf::ST_RESERVED) {
        d->label_dsc->color = C_RED;
    } else if (rf::isWrite(kind)) {
        d->label_dsc->color = C_TEXT;
    } else if (rf::isRead(kind)) {
        d->label_dsc->color = C_GREEN;
    } else {
        d->label_dsc->color = C_MUTED;
    }
}

// ==================== thống kê theo IC ====================

static void updateStats(void)
{
    const auto &pk = HCCSpmiMonitor::packets();
    rm::IcCount cnt[16];
    const int k = rm::aggregate(pk.data(), pk.count(), v.map, cnt, 16);
    std::string s = "THỐNG KÊ THEO USID:  ";
    if (k == 0) {
        s += "(chưa có gói)";
    }
    char line[96];
    for (int i = 0; i < k; ++i) {
        snprintf(line, sizeof(line), "0x%X %s: %u (R%u/W%u", cnt[i].usid, cnt[i].ic[0] ? cnt[i].ic : "—",
                 (unsigned)cnt[i].total, (unsigned)cnt[i].reads, (unsigned)cnt[i].writes);
        s += line;
        if (cnt[i].errors) {
            snprintf(line, sizeof(line), "/E%u", (unsigned)cnt[i].errors);
            s += line;
        }
        s += ")   ";
    }
    lv_label_set_text(v.stats, s.c_str());
}

// ==================== timer ====================

static void onTimer(lv_timer_t *)
{
    const Snapshot sn = HCCSpmiMonitor::snapshot();

    lv_label_set_text(v.state_lbl, HCCSpmiMonitor::stateName(sn.state));
    lv_color_t sc = C_MUTED;
    switch (sn.state) {
    case State::CAPTURING: sc = C_GREEN; break;
    case State::NO_SIGNAL: sc = C_ORANGE; break;
    case State::OVERFLOW: sc = C_RED; break;
    case State::HW_NOT_SUPPORTED:
    case State::ERROR: sc = C_RED; break;
    case State::STOPPED: sc = C_CYAN; break;
    default: sc = C_MUTED; break;
    }
    lv_obj_set_style_text_color(v.state_lbl, sc, 0);

    char info[128];
    snprintf(info, sizeof(info), "Lấy mẫu %u MHz   SCLK ~ %.2f MHz%s", (unsigned)(sn.sample_hz / 1000000),
             sn.sclk_mhz, sn.undersampled ? "  (CHẬM: SCLK quá nhanh so với mẫu)" : "");
    lv_label_set_text(v.info_lbl, info);

    char cnt[160];
    snprintf(cnt, sizeof(cnt), "Frame: %u   Lỗi: %u   Mất: %u gói / %u chunk   %.1f s",
             (unsigned)sn.frames, (unsigned)sn.errors, (unsigned)sn.dropped_pkts, (unsigned)sn.lost_chunks,
             sn.elapsed_ms / 1000.0f);
    lv_label_set_text(v.count_lbl, cnt);
    lv_obj_set_style_text_color(v.count_lbl, (sn.dropped_pkts || sn.lost_chunks) ? C_ORANGE : C_MUTED, 0);

    if (sn.hw_why[0] && sn.state == State::HW_NOT_SUPPORTED) {
        lv_label_set_text(v.msg, sn.hw_why);
    }

    const uint32_t pc = HCCSpmiMonitor::packets().count();
    scanNew();
    fillTable(false);
    if (pc != v.stats_seen) {
        v.stats_seen = pc;
        updateStats();
    }
}

// ==================== bộ lọc IC ====================

static void buildIcDropdown(void)
{
    std::string opt = "TẤT CẢ IC";
    v.ic_count = 0;
    for (int u = 0; u < 16 && v.ic_count < 16; ++u) {
        if (v.map.hasIc((uint8_t)u)) {
            const char *name = v.map.icOf((uint8_t)u);
            snprintf(v.ic_options[v.ic_count], sizeof(v.ic_options[0]), "%s", name);
            opt += "\n";
            opt += name;
            ++v.ic_count;
        }
    }
    lv_dropdown_set_options(v.ic_dd, opt.c_str());
}

// ==================== handler điều khiển ====================

static uint32_t rateHz(void)
{
    static const uint32_t r[3] = {20000000, 40000000, 80000000};
    return r[v.rate_sel < 0 || v.rate_sel > 2 ? 2 : v.rate_sel];
}

static void onRate(lv_event_t *e) { v.rate_sel = (int)lv_dropdown_get_selected(lv_event_get_target(e)); }

static void onStart(lv_event_t *)
{
    if (!HCCSpmiMonitor::hwConfirmed()) {
        lv_label_set_text(v.msg,
                          "Chưa xác nhận mạch thu. Bấm XÁC NHẬN MẠCH sau khi đã lắp mạch chuyển mức một chiều "
                          "(VCCA = VIO main). Monitor chỉ NGHE, không phát lệnh lên bus.");
        lv_obj_set_style_text_color(v.msg, C_ORANGE, 0);
        return;
    }
    HCCSpmiSource::Params p;
    p.sample_hz = rateHz();
    p.hw_confirmed = true;
    char err[320] = "";
    if (!HCCSpmiMonitor::start(Src::PARLIO, p, err, sizeof(err))) {
        lv_label_set_text(v.msg, err[0] ? err : "Không bắt đầu thu được");
        lv_obj_set_style_text_color(v.msg, C_RED, 0);
        return;
    }
    resetFilter();
    v.follow = true;
    lv_obj_add_state(v.follow_btn, LV_STATE_CHECKED);
    lv_label_set_text(v.msg, "Đang thu (chỉ nghe). SCLK GPIO2 / SDATA GPIO3.");
    lv_obj_set_style_text_color(v.msg, C_GREEN, 0);
}

static void onStop(lv_event_t *)
{
    HCCSpmiMonitor::stop();
    lv_label_set_text(v.msg, "Đã dừng thu.");
    lv_obj_set_style_text_color(v.msg, C_CYAN, 0);
}

static void onClear(lv_event_t *)
{
    if (HCCSpmiMonitor::running()) {
        lv_label_set_text(v.msg, "Dừng thu trước khi XÓA.");
        lv_obj_set_style_text_color(v.msg, C_ORANGE, 0);
        return;
    }
    HCCSpmiMonitor::clear();
    resetFilter();
    v.page = 0;
    fillTable(true);
    updateStats();
    lv_label_set_text(v.msg, "Đã xóa dữ liệu.");
    lv_obj_set_style_text_color(v.msg, C_MUTED, 0);
}

static void onSave(lv_event_t *)
{
    if (!HCCSpmiMonitor::startSave()) {
        lv_label_set_text(v.msg, "Không lưu được (đang thu / đang lưu / không có dữ liệu).");
        lv_obj_set_style_text_color(v.msg, C_ORANGE, 0);
        return;
    }
    lv_label_set_text(v.msg, "Đang lưu ra /sdcard/spmi ...");
    lv_obj_set_style_text_color(v.msg, C_CYAN, 0);
}

static void onHwConfirm(lv_event_t *)
{
    const bool now = !HCCSpmiMonitor::hwConfirmed();
    HCCSpmiMonitor::setHwConfirmed(now);
    lv_label_set_text(v.msg, now ? "Đã xác nhận mạch thu. Bấm START để thu."
                                 : "Đã thu hồi xác nhận mạch thu.");
    lv_obj_set_style_text_color(v.msg, now ? C_GREEN : C_MUTED, 0);
}

static void onFilterUsid(lv_event_t *e)
{
    const uint16_t s = lv_dropdown_get_selected(lv_event_get_target(e));
    v.fil.usid = s == 0 ? -1 : (int)s - 1;
    resetFilter();
    v.page = 0;
    fillTable(true);
}

static void onFilterIc(lv_event_t *e)
{
    const uint16_t s = lv_dropdown_get_selected(lv_event_get_target(e));
    if (s == 0 || (int)s - 1 >= v.ic_count) {
        v.fil.ic[0] = '\0';
    } else {
        snprintf(v.fil.ic, sizeof(v.fil.ic), "%s", v.ic_options[s - 1]);
    }
    resetFilter();
    v.page = 0;
    fillTable(true);
}

static void onToggleRead(lv_event_t *)
{
    v.fil.reads = !v.fil.reads;
    setToggle(v.rd_btn, v.fil.reads);
    resetFilter();
    v.page = 0;
    fillTable(true);
}

static void onToggleWrite(lv_event_t *)
{
    v.fil.writes = !v.fil.writes;
    setToggle(v.wr_btn, v.fil.writes);
    resetFilter();
    v.page = 0;
    fillTable(true);
}

static void onToggleErr(lv_event_t *)
{
    v.fil.errors_only = !v.fil.errors_only;
    setToggle(v.err_btn, v.fil.errors_only);
    resetFilter();
    v.page = 0;
    fillTable(true);
}

static void onPrev(lv_event_t *)
{
    if (v.page > 0) {
        --v.page;
        v.follow = false;
        lv_obj_clear_state(v.follow_btn, LV_STATE_CHECKED);
        fillTable(true);
    }
}

static void onNext(lv_event_t *)
{
    const uint32_t pages = v.idx.empty() ? 1 : ((uint32_t)v.idx.size() + PAGE - 1) / PAGE;
    if (v.page + 1 < pages) {
        ++v.page;
        v.follow = false;
        lv_obj_clear_state(v.follow_btn, LV_STATE_CHECKED);
        fillTable(true);
    }
}

static void onFollow(lv_event_t *e)
{
    v.follow = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    fillTable(true);
}

// BACK: dừng thu để trả chân SCLK / SDATA cho RFFE Scan. back_cb (gắn thêm trên cùng nút)
// sẽ lv_obj_clean(screen) + dựng lại màn RFFE Scan sau đó.
static void onBackStop(lv_event_t *) { HCCSpmiMonitor::stop(); }

static void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
        v.timer = nullptr;
    }
    v.root = nullptr;
}

// ==================== dựng màn ====================

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user, const std::vector<HCCRffe::RefEntry> &ref)
{
    if (v.root) {
        lv_obj_del(v.root);
    }
    v = V{};
    v.screen = screen;
    v.back_cb = back_cb;
    v.back_user = back_user;
    v.ref = ref;
    v.map.build(v.ref);

    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_t *root = lv_obj_create(screen);
    v.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    lv_coord_t root_h = HCCLayout::usableHeight(root, 503);
    if (root_h < 360 || root_h > 600) {
        root_h = 520;
    }

    // Thanh trên.
    lv_obj_t *title = label(root, "RFFE MONITOR", C_TEXT, &lv_font_be_vietnam_28);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 10);
    v.state_lbl = label(root, "IDLE", C_MUTED, &lv_font_be_vietnam_20);
    lv_obj_align(v.state_lbl, LV_ALIGN_TOP_LEFT, 300, 16);
    v.info_lbl = label(root, "", C_MUTED, &lv_font_be_vietnam_16);
    lv_obj_align(v.info_lbl, LV_ALIGN_TOP_LEFT, 500, 10);
    v.count_lbl = label(root, "", C_MUTED, &lv_font_be_vietnam_16);
    lv_obj_align(v.count_lbl, LV_ALIGN_TOP_LEFT, 500, 30);

    // Hàng điều khiển.
    lv_obj_t *rate = lv_dropdown_create(root);
    v.rate_dd = rate;
    lv_dropdown_set_options(rate, "20 MHz\n40 MHz\n80 MHz");
    lv_dropdown_set_selected(rate, 2);
    lv_obj_set_size(rate, 120, 40);
    lv_obj_set_style_text_font(rate, &lv_font_be_vietnam_16, 0);
    lv_obj_align(rate, LV_ALIGN_TOP_LEFT, 28, 58);
    lv_obj_add_event_cb(rate, onRate, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *b_start = button(root, "START", 110, 40, C_GREEN, onStart, nullptr);
    lv_obj_align(b_start, LV_ALIGN_TOP_LEFT, 158, 58);
    lv_obj_t *b_stop = button(root, "STOP", 100, 40, C_BORDER, onStop, nullptr);
    lv_obj_align(b_stop, LV_ALIGN_TOP_LEFT, 274, 58);
    lv_obj_t *b_clear = button(root, "CLEAR", 100, 40, C_BORDER, onClear, nullptr);
    lv_obj_align(b_clear, LV_ALIGN_TOP_LEFT, 380, 58);
    lv_obj_t *b_save = button(root, "SAVE SD", 120, 40, C_BLUE, onSave, nullptr);
    lv_obj_align(b_save, LV_ALIGN_TOP_LEFT, 486, 58);
    lv_obj_t *b_hw = button(root, "XÁC NHẬN MẠCH", 190, 40, C_ORANGE, onHwConfirm, nullptr);
    lv_obj_align(b_hw, LV_ALIGN_TOP_LEFT, 612, 58);
    lv_obj_t *b_back = button(root, "BACK", 110, 40, C_BORDER, onBackStop, nullptr);
    if (back_cb) {
        lv_obj_add_event_cb(b_back, back_cb, LV_EVENT_CLICKED, back_user);
    }
    lv_obj_align(b_back, LV_ALIGN_TOP_RIGHT, -28, 58);

    // Hàng bộ lọc.
    lv_obj_t *fl = label(root, "LỌC:", C_MUTED, &lv_font_be_vietnam_16);
    lv_obj_align(fl, LV_ALIGN_TOP_LEFT, 28, 112);
    v.usid_dd = lv_dropdown_create(root);
    lv_dropdown_set_options(v.usid_dd,
                            "USID: TẤT CẢ\n0x0\n0x1\n0x2\n0x3\n0x4\n0x5\n0x6\n0x7\n0x8\n0x9\n0xA\n0xB\n0xC\n0xD\n0xE\n0xF");
    lv_obj_set_size(v.usid_dd, 150, 40);
    lv_obj_set_style_text_font(v.usid_dd, &lv_font_be_vietnam_16, 0);
    lv_obj_align(v.usid_dd, LV_ALIGN_TOP_LEFT, 80, 106);
    lv_obj_add_event_cb(v.usid_dd, onFilterUsid, LV_EVENT_VALUE_CHANGED, nullptr);

    v.ic_dd = lv_dropdown_create(root);
    lv_obj_set_size(v.ic_dd, 200, 40);
    lv_obj_set_style_text_font(v.ic_dd, &lv_font_be_vietnam_16, 0);
    lv_obj_align(v.ic_dd, LV_ALIGN_TOP_LEFT, 242, 106);
    lv_obj_add_event_cb(v.ic_dd, onFilterIc, LV_EVENT_VALUE_CHANGED, nullptr);
    buildIcDropdown();

    v.rd_btn = button(root, "READ", 90, 40, C_BLUE, onToggleRead, nullptr);
    lv_obj_align(v.rd_btn, LV_ALIGN_TOP_LEFT, 456, 106);
    v.wr_btn = button(root, "WRITE", 90, 40, C_BLUE, onToggleWrite, nullptr);
    lv_obj_align(v.wr_btn, LV_ALIGN_TOP_LEFT, 552, 106);
    v.err_btn = button(root, "ERROR", 90, 40, C_BORDER, onToggleErr, nullptr);
    lv_obj_align(v.err_btn, LV_ALIGN_TOP_LEFT, 648, 106);

    lv_obj_t *prev = button(root, "<", 50, 40, C_BORDER, onPrev, nullptr);
    lv_obj_align(prev, LV_ALIGN_TOP_RIGHT, -256, 106);
    lv_obj_t *next = button(root, ">", 50, 40, C_BORDER, onNext, nullptr);
    lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -200, 106);
    v.follow_btn = lv_btn_create(root);
    lv_obj_set_size(v.follow_btn, 110, 40);
    lv_obj_add_flag(v.follow_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_state(v.follow_btn, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(v.follow_btn, C_BORDER, 0);
    lv_obj_set_style_bg_color(v.follow_btn, C_BLUE, LV_STATE_CHECKED);
    lv_obj_set_style_radius(v.follow_btn, 10, 0);
    lv_obj_set_style_border_width(v.follow_btn, 0, 0);
    lv_obj_set_style_shadow_width(v.follow_btn, 0, 0);
    lv_obj_add_event_cb(v.follow_btn, onFollow, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *fl2 = label(v.follow_btn, "CUỐI", C_TEXT, &lv_font_be_vietnam_16);
    lv_obj_center(fl2);
    lv_obj_align(v.follow_btn, LV_ALIGN_TOP_RIGHT, -84, 106);

    v.page_lbl = label(root, "Trang 1/1", C_MUTED, &lv_font_be_vietnam_14);
    lv_obj_align(v.page_lbl, LV_ALIGN_TOP_RIGHT, -28, 150);

    // Bảng gói.
    const int table_y = 176;
    const int table_h = root_h - table_y - 92;
    v.table = lv_table_create(root);
    lv_obj_set_pos(v.table, 28, table_y);
    lv_obj_set_size(v.table, 968, table_h);
    lv_obj_set_style_bg_color(v.table, C_PANEL, 0);
    lv_obj_set_style_border_color(v.table, C_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(v.table, 1, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(v.table, C_PANEL2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.table, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(v.table, &lv_font_be_vietnam_16, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(v.table, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(v.table, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(v.table, 8, LV_PART_ITEMS);
    lv_table_set_col_cnt(v.table, COLS);
    lv_table_set_col_width(v.table, 0, 80);
    lv_table_set_col_width(v.table, 1, 170);
    lv_table_set_col_width(v.table, 2, 130);
    lv_table_set_col_width(v.table, 3, 110);
    lv_table_set_col_width(v.table, 4, 110);
    lv_table_set_col_width(v.table, 5, 150);
    lv_table_set_col_width(v.table, 6, 118);
    lv_obj_add_event_cb(v.table, onTableDraw, LV_EVENT_DRAW_PART_BEGIN, nullptr);

    // Thống kê + thông báo.
    v.stats = label(root, "THỐNG KÊ THEO USID:", C_CYAN, &lv_font_be_vietnam_14);
    lv_obj_set_width(v.stats, 968);
    lv_label_set_long_mode(v.stats, LV_LABEL_LONG_DOT);
    lv_obj_align(v.stats, LV_ALIGN_BOTTOM_LEFT, 28, -40);

    v.msg = label(root, "Lắp mạch chuyển mức → XÁC NHẬN MẠCH → START. Monitor chỉ NGHE, không phát lệnh.", C_MUTED,
                  &lv_font_be_vietnam_16);
    lv_obj_set_width(v.msg, 968);
    lv_label_set_long_mode(v.msg, LV_LABEL_LONG_DOT);
    lv_obj_align(v.msg, LV_ALIGN_BOTTOM_LEFT, 28, -14);

    setToggle(v.rd_btn, true);
    setToggle(v.wr_btn, true);
    setToggle(v.err_btn, false);

    // Đang thu dở (quay lại màn): tiếp tục hiển thị.
    if (HCCSpmiMonitor::running()) {
        scanNew();
    }
    fillTable(true);
    updateStats();
    v.timer = lv_timer_create(onTimer, 100, nullptr);
}

}  // namespace HCCRffeMonitorView
