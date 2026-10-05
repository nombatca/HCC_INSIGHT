#include "HCCRffeView.hpp"

#include <stdio.h>
#include <string.h>
#include <vector>

#include "HCCRffe.hpp"

namespace HCCRffeView {

using HCCRffe::RegResult;
using HCCRffe::Report;
using HCCRffe::UsidResult;

static const lv_color_t C_BG = lv_color_hex(0x101820);
static const lv_color_t C_PANEL = lv_color_hex(0x0B141C);
static const lv_color_t C_BORDER = lv_color_hex(0x304553);
static const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
static const lv_color_t C_MUTED = lv_color_hex(0xB8C5CF);
static const lv_color_t C_CYAN = lv_color_hex(0x00D9FF);
static const lv_color_t C_GREEN = lv_color_hex(0x3DDC84);
static const lv_color_t C_RED = lv_color_hex(0xFF5A5F);
static const lv_color_t C_ORANGE = lv_color_hex(0xFFB020);
static const lv_color_t C_BLUE = lv_color_hex(0x4C9AFF);
static const lv_color_t C_GREY = lv_color_hex(0x5B6B78);

enum RowState : uint8_t { ROW_NEUTRAL, ROW_PASS, ROW_FAIL, ROW_WARN };

static const int TABLE_COLS = 6;

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *model_dd = nullptr;
    lv_obj_t *data_dd = nullptr;
    lv_obj_t *data_info = nullptr;
    lv_obj_t *summary = nullptr;
    lv_obj_t *chips = nullptr;
    lv_obj_t *id_line = nullptr;
    lv_obj_t *table = nullptr;
    lv_obj_t *empty = nullptr;
    lv_obj_t *scan_btn = nullptr;
    lv_obj_t *status = nullptr;
    lv_timer_t *timer = nullptr;
    bool have_models = false;
    bool have_data = false;
    bool waiting_scan = false;
    uint32_t seen_serial = 0;
    char model[64] = "";
    char data[64] = "";
    std::vector<HCCRffe::RefEntry> ref;
    Report report;
    int sel = 0;
    std::vector<uint8_t> row_state;
};

static View s_v;

// ==================== Tiện ích dựng widget ====================

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, lv_color_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *text, lv_color_t color, int w, int h, lv_event_cb_t cb,
                            void *user)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = makeLabel(b, text, C_TEXT, &lv_font_be_vietnam_18);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *makeDropdown(lv_obj_t *parent, int w, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_size(dd, w, 40);
    lv_obj_set_style_text_font(dd, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_bg_color(dd, C_PANEL, 0);
    lv_obj_set_style_text_color(dd, C_TEXT, 0);
    lv_obj_set_style_border_color(dd, C_BORDER, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_max_height(list, 300, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

static void setStatus(const char *text, lv_color_t color)
{
    if (s_v.status) {
        lv_label_set_text(s_v.status, text);
        lv_obj_set_style_text_color(s_v.status, color, 0);
    }
}

// ==================== Bảng kết quả ====================

static const char *usidSummary(const UsidResult &u, bool scanned, char *buf, size_t n, lv_color_t *color)
{
    if (!scanned) {
        *color = C_GREY;
        snprintf(buf, n, "%d thanh ghi", (int)u.regs.size());
    } else if (!u.responded) {
        *color = u.in_ref ? C_RED : C_GREY;
        snprintf(buf, n, "Không phản hồi");
    } else if (!u.in_ref) {
        *color = C_BLUE;
        snprintf(buf, n, "Đọc thô");
    } else if (u.fail > 0 || u.no_read > 0) {
        *color = C_RED;
        snprintf(buf, n, "FAIL %d / %d", u.fail + u.no_read, (int)u.regs.size());
    } else {
        *color = C_GREEN;
        snprintf(buf, n, "PASS %d / %d", u.pass, (int)u.regs.size());
    }
    return buf;
}

static void renderTable(void);

static void onChipClicked(lv_event_t *e)
{
    s_v.sel = (int)(intptr_t)lv_event_get_user_data(e);
    const uint32_t n = lv_obj_get_child_cnt(s_v.chips);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(s_v.chips, i);
        lv_obj_set_style_border_width(c, (int)i == s_v.sel ? 3 : 0, 0);
    }
    renderTable();
}

static void onTableDraw(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc == nullptr || dsc->part != LV_PART_ITEMS) {
        return;
    }
    const uint32_t row = dsc->id / TABLE_COLS;
    const uint32_t col = dsc->id % TABLE_COLS;
    if (row == 0) {
        if (dsc->rect_dsc) {
            dsc->rect_dsc->bg_color = lv_color_hex(0x1B2A36);
            dsc->rect_dsc->bg_opa = LV_OPA_COVER;
        }
        if (dsc->label_dsc) {
            dsc->label_dsc->color = C_CYAN;
        }
        return;
    }
    if (dsc->rect_dsc && (row % 2) == 0) {
        dsc->rect_dsc->bg_color = lv_color_hex(0x101C26);
        dsc->rect_dsc->bg_opa = LV_OPA_COVER;
    }
    if (dsc->label_dsc == nullptr || row - 1 >= s_v.row_state.size()) {
        return;
    }
    const uint8_t st = s_v.row_state[row - 1];
    if (col == 5 || (col == 2 && st == ROW_FAIL)) {
        dsc->label_dsc->color = st == ROW_PASS ? C_GREEN : st == ROW_FAIL ? C_RED : st == ROW_WARN ? C_ORANGE : C_MUTED;
    }
}

static void renderTable(void)
{
    if (s_v.table == nullptr) {
        return;
    }
    const Report &rep = s_v.report;
    if (rep.usids.empty() || s_v.sel >= (int)rep.usids.size()) {
        lv_obj_add_flag(s_v.table, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_v.id_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_v.empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_v.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_v.table, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_v.id_line, LV_OBJ_FLAG_HIDDEN);

    const UsidResult &u = rep.usids[s_v.sel];
    char line[160];
    if (!rep.scanned) {
        snprintf(line, sizeof(line), "USID 0x%X  -  dữ liệu tham chiếu, chưa scan", u.usid);
    } else if (!u.responded) {
        snprintf(line, sizeof(line), "USID 0x%X  -  không phản hồi", u.usid);
    } else {
        char ids[3][8];
        for (int i = 0; i < 3; ++i) {
            if (u.id_ok[i]) {
                snprintf(ids[i], sizeof(ids[i]), "0x%02X", u.id_regs[i]);
            } else {
                snprintf(ids[i], sizeof(ids[i]), "--");
            }
        }
        snprintf(line, sizeof(line), "USID 0x%X   |   0x1D = %s   0x1E = %s   0x1F = %s   (giá trị thô)", u.usid, ids[0],
                 ids[1], ids[2]);
    }
    lv_label_set_text(s_v.id_line, line);

    const uint16_t rows = (uint16_t)(u.regs.size() + 1);
    lv_table_set_row_cnt(s_v.table, rows);
    static const char *const HEAD[TABLE_COLS] = {"Thanh ghi", "Tên", "Đọc được", "Chuẩn", "Mask", "Kết quả"};
    for (int c = 0; c < TABLE_COLS; ++c) {
        lv_table_set_cell_value(s_v.table, 0, c, HEAD[c]);
    }
    s_v.row_state.assign(u.regs.size(), ROW_NEUTRAL);
    for (size_t i = 0; i < u.regs.size(); ++i) {
        const RegResult &r = u.regs[i];
        const uint16_t row = (uint16_t)(i + 1);
        lv_table_set_cell_value_fmt(s_v.table, row, 0, r.reg > 0x1F ? "0x%02X (ext)" : "0x%02X", r.reg);
        lv_table_set_cell_value(s_v.table, row, 1, r.name[0] ? r.name : "-");
        if (rep.scanned && r.read_ok) {
            lv_table_set_cell_value_fmt(s_v.table, row, 2, "0x%02X", r.value);
        } else {
            lv_table_set_cell_value(s_v.table, row, 2, "--");
        }
        if (r.has_ref) {
            lv_table_set_cell_value_fmt(s_v.table, row, 3, "0x%02X", r.expected);
            lv_table_set_cell_value_fmt(s_v.table, row, 4, "0x%02X", r.mask);
        } else {
            lv_table_set_cell_value(s_v.table, row, 3, "-");
            lv_table_set_cell_value(s_v.table, row, 4, "-");
        }
        const char *res;
        uint8_t st = ROW_NEUTRAL;
        if (!rep.scanned) {
            res = "Chưa scan";
        } else if (!u.responded) {
            res = "Không phản hồi";
            st = ROW_FAIL;
        } else if (!r.read_ok) {
            res = "Lỗi chẵn lẻ";
            st = ROW_WARN;
        } else if (!r.has_ref) {
            res = "Đọc thô";
        } else if ((r.value & r.mask) == (r.expected & r.mask)) {
            res = "PASS";
            st = ROW_PASS;
        } else {
            res = "FAIL";
            st = ROW_FAIL;
        }
        s_v.row_state[i] = st;
        lv_table_set_cell_value(s_v.table, row, 5, res);
    }
    lv_obj_scroll_to_y(s_v.table, 0, LV_ANIM_OFF);
    lv_obj_invalidate(s_v.table);
}

static void renderReport(void)
{
    if (s_v.chips == nullptr) {
        return;
    }
    const Report &rep = s_v.report;
    lv_obj_clean(s_v.chips);
    if (s_v.sel >= (int)rep.usids.size()) {
        s_v.sel = 0;
    }
    for (size_t i = 0; i < rep.usids.size(); ++i) {
        const UsidResult &u = rep.usids[i];
        lv_color_t color;
        char sub[32], text[64];
        usidSummary(u, rep.scanned, sub, sizeof(sub), &color);
        snprintf(text, sizeof(text), "USID 0x%X\n%s", u.usid, sub);
        lv_obj_t *chip = lv_btn_create(s_v.chips);
        lv_obj_set_size(chip, 150, 54);
        lv_obj_set_style_bg_color(chip, color, 0);
        lv_obj_set_style_radius(chip, 10, 0);
        lv_obj_set_style_shadow_width(chip, 0, 0);
        lv_obj_set_style_border_color(chip, C_TEXT, 0);
        lv_obj_set_style_border_width(chip, (int)i == s_v.sel ? 3 : 0, 0);
        lv_obj_add_event_cb(chip, onChipClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = makeLabel(chip, text, C_TEXT, &lv_font_be_vietnam_14);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }

    char sum[128];
    if (rep.scanned) {
        snprintf(sum, sizeof(sum), "%s", rep.message);
    } else if (!rep.usids.empty()) {
        snprintf(sum, sizeof(sum), "Dữ liệu: %d USID - bấm SCAN để đọc", (int)rep.usids.size());
    } else {
        sum[0] = '\0';
    }
    lv_label_set_text(s_v.summary, sum);

    if (!s_v.have_models) {
        lv_label_set_text(s_v.empty,
                          "Chưa có dữ liệu RFFE trên thẻ SD.\n\n"
                          "Tạo thư mục /rffe/<MODEL>/ trên thẻ và chép file .csv vào đó.\n"
                          "Mỗi dòng: usid,reg,expected,mask,tên   (vd: 0x5,0x00,0x38,0xFF,REG0)\n\n"
                          "Không có dữ liệu vẫn SCAN được: các USID tìm thấy sẽ đọc thô 0x00..0x1F.");
    } else if (rep.scanned) {
        lv_label_set_text(s_v.empty, "Không có USID nào phản hồi.");
    } else {
        lv_label_set_text(s_v.empty, "Chọn model và file dữ liệu, rồi bấm SCAN.");
    }
    renderTable();
}

// ==================== Dữ liệu ====================

static void loadSelectedData(void)
{
    s_v.ref.clear();
    char info[160] = "";
    if (s_v.have_data) {
        int skipped = 0;
        char err[96];
        if (HCCRffe::loadReference(s_v.model, s_v.data, s_v.ref, &skipped, err, sizeof(err))) {
            int usids = 0;
            uint16_t seen = 0;
            for (const auto &e : s_v.ref) {
                if (!(seen & (1u << e.usid))) {
                    seen |= 1u << e.usid;
                    ++usids;
                }
            }
            snprintf(info, sizeof(info), "%s/%s: %d thanh ghi, %d USID", s_v.model, s_v.data, (int)s_v.ref.size(),
                     usids);
            if (skipped) {
                size_t n = strlen(info);
                snprintf(info + n, sizeof(info) - n, "  (bỏ %d dòng sai)", skipped);
            }
            lv_obj_set_style_text_color(s_v.data_info, C_MUTED, 0);
        } else {
            snprintf(info, sizeof(info), "%s", err);
            lv_obj_set_style_text_color(s_v.data_info, C_ORANGE, 0);
        }
    } else if (s_v.have_models) {
        snprintf(info, sizeof(info), "Thư mục %s chưa có file .csv", s_v.model);
        lv_obj_set_style_text_color(s_v.data_info, C_ORANGE, 0);
    }
    lv_label_set_text(s_v.data_info, info);
    HCCRffe::buildPreview(s_v.ref, s_v.report);
    s_v.sel = 0;
    renderReport();
}

static void populateData(void)
{
    char list[1024];
    int n = s_v.have_models ? HCCRffe::listData(s_v.model, list, sizeof(list)) : -1;
    s_v.have_data = n > 0;
    lv_dropdown_set_options(s_v.data_dd, s_v.have_data ? list : "(trống)");
    s_v.data[0] = '\0';
    if (s_v.have_data) {
        lv_dropdown_get_selected_str(s_v.data_dd, s_v.data, sizeof(s_v.data));
    }
    loadSelectedData();
}

static void populateModels(void)
{
    char list[1024];
    const int n = HCCRffe::listModels(list, sizeof(list));
    s_v.have_models = n > 0;
    lv_dropdown_set_options(s_v.model_dd, s_v.have_models ? list : "(chưa có /rffe)");
    s_v.model[0] = '\0';
    if (s_v.have_models) {
        lv_dropdown_get_selected_str(s_v.model_dd, s_v.model, sizeof(s_v.model));
    }
    populateData();
}

static void onModelChanged(lv_event_t *)
{
    if (!s_v.have_models) {
        return;
    }
    lv_dropdown_get_selected_str(s_v.model_dd, s_v.model, sizeof(s_v.model));
    populateData();
}

static void onDataChanged(lv_event_t *)
{
    if (!s_v.have_data) {
        return;
    }
    lv_dropdown_get_selected_str(s_v.data_dd, s_v.data, sizeof(s_v.data));
    loadSelectedData();
}

static void onRefreshClicked(lv_event_t *)
{
    if (s_v.waiting_scan) {
        return;
    }
    populateModels();
    setStatus("Đã làm mới danh sách", C_CYAN);
}

// ==================== Scan ====================

static void onScanClicked(lv_event_t *)
{
    if (s_v.waiting_scan || HCCRffe::busy()) {
        return;
    }
    if (!HCCRffe::configured()) {
        setStatus("Chưa cấu hình chân SCLK / SDATA (menuconfig > HCC RFFE)", C_ORANGE);
        return;
    }
    if (!HCCRffe::startScan(s_v.ref)) {
        setStatus("Không khởi động được scan", C_RED);
        return;
    }
    s_v.waiting_scan = true;
    s_v.seen_serial = HCCRffe::reportSerial();
    lv_obj_add_state(s_v.scan_btn, LV_STATE_DISABLED);
    setStatus("Đang scan USID 0x1..0xF...", C_CYAN);
}

static void onTimer(lv_timer_t *)
{
    if (!s_v.waiting_scan || HCCRffe::busy() || HCCRffe::reportSerial() == s_v.seen_serial) {
        return;
    }
    s_v.waiting_scan = false;
    s_v.seen_serial = HCCRffe::reportSerial();
    s_v.report = HCCRffe::report();
    s_v.sel = 0;
    lv_obj_clear_state(s_v.scan_btn, LV_STATE_DISABLED);
    setStatus("Scan xong", C_GREEN);
    renderReport();
}

static void onRootDeleted(lv_event_t *)
{
    if (s_v.timer) {
        lv_timer_del(s_v.timer);
    }
    s_v = View();
}

// ==================== Dựng màn hình ====================

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user_data)
{
    if (s_v.root) {
        lv_obj_del(s_v.root);  // onRootDeleted dọn trạng thái
    }
    lv_obj_set_style_bg_color(screen, C_BG, 0);

    lv_obj_t *root = lv_obj_create(screen);
    s_v.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t *title = makeLabel(root, "RFFE", C_TEXT, &lv_font_be_vietnam_28);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 14);

    char pins[160];
    if (HCCRffe::configured()) {
        snprintf(pins, sizeof(pins), "SCLK GPIO %d   SDATA GPIO %d   |   bus 1,8 V qua mạch chuyển mức",
                 HCCRffe::sclkPin(), HCCRffe::sdataPin());
    } else {
        snprintf(pins, sizeof(pins), "Chưa cấu hình chân SCLK / SDATA  (menuconfig > HCC RFFE)");
    }
    lv_obj_t *pin_label =
        makeLabel(root, pins, HCCRffe::configured() ? C_MUTED : C_ORANGE, &lv_font_be_vietnam_16);
    lv_obj_align(pin_label, LV_ALIGN_TOP_LEFT, 120, 24);

    /* SELECT MODEL / RFFE DATA */
    lv_obj_t *l1 = makeLabel(root, "SELECT MODEL", C_MUTED, &lv_font_be_vietnam_16);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 28, 68);
    s_v.model_dd = makeDropdown(root, 300, onModelChanged);
    lv_obj_align(s_v.model_dd, LV_ALIGN_TOP_LEFT, 28, 92);

    lv_obj_t *l2 = makeLabel(root, "RFFE DATA", C_MUTED, &lv_font_be_vietnam_16);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 348, 68);
    s_v.data_dd = makeDropdown(root, 300, onDataChanged);
    lv_obj_align(s_v.data_dd, LV_ALIGN_TOP_LEFT, 348, 92);

    lv_obj_t *refresh = makeButton(root, "LÀM MỚI", C_BORDER, 130, 40, onRefreshClicked, nullptr);
    lv_obj_align(refresh, LV_ALIGN_TOP_LEFT, 668, 92);

    s_v.data_info = makeLabel(root, "", C_MUTED, &lv_font_be_vietnam_14);
    lv_obj_align(s_v.data_info, LV_ALIGN_TOP_LEFT, 28, 138);
    lv_obj_set_width(s_v.data_info, 960);
    lv_label_set_long_mode(s_v.data_info, LV_LABEL_LONG_DOT);

    /* Khung kết quả (theo USID) */
    lv_obj_t *box = lv_obj_create(root);
    lv_obj_set_size(box, 968, 352);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 28, 162);
    lv_obj_set_style_bg_color(box, C_PANEL, 0);
    lv_obj_set_style_border_color(box, C_BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box_title = makeLabel(box, "KẾT QUẢ SCAN THEO USID", C_CYAN, &lv_font_be_vietnam_16);
    lv_obj_align(box_title, LV_ALIGN_TOP_LEFT, 0, 0);
    s_v.summary = makeLabel(box, "", C_MUTED, &lv_font_be_vietnam_14);
    lv_obj_align(s_v.summary, LV_ALIGN_TOP_RIGHT, 0, 2);

    s_v.chips = lv_obj_create(box);
    lv_obj_remove_style_all(s_v.chips);
    lv_obj_set_size(s_v.chips, 942, 62);
    lv_obj_align(s_v.chips, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_flex_flow(s_v.chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_v.chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_v.chips, 10, 0);
    lv_obj_set_scroll_dir(s_v.chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_v.chips, LV_SCROLLBAR_MODE_OFF);

    s_v.id_line = makeLabel(box, "", C_MUTED, &lv_font_be_vietnam_14);
    lv_obj_align(s_v.id_line, LV_ALIGN_TOP_LEFT, 0, 94);

    s_v.table = lv_table_create(box);
    lv_table_set_col_cnt(s_v.table, TABLE_COLS);
    static const lv_coord_t COL_W[TABLE_COLS] = {130, 250, 120, 110, 100, 222};
    for (int c = 0; c < TABLE_COLS; ++c) {
        lv_table_set_col_width(s_v.table, c, COL_W[c]);
    }
    lv_obj_set_size(s_v.table, 942, 210);
    lv_obj_align(s_v.table, LV_ALIGN_TOP_LEFT, 0, 118);
    lv_obj_set_style_bg_color(s_v.table, C_PANEL, 0);
    lv_obj_set_style_border_width(s_v.table, 0, 0);
    lv_obj_set_style_pad_all(s_v.table, 0, 0);
    lv_obj_set_style_bg_color(s_v.table, C_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_v.table, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_v.table, &lv_font_be_vietnam_16, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_v.table, C_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_v.table, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_side(s_v.table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(s_v.table, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(s_v.table, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(s_v.table, 10, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(s_v.table, 6, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_v.table, onTableDraw, LV_EVENT_DRAW_PART_BEGIN, nullptr);

    s_v.empty = makeLabel(box, "", C_MUTED, &lv_font_be_vietnam_16);
    lv_obj_set_width(s_v.empty, 900);
    lv_label_set_long_mode(s_v.empty, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_v.empty, LV_ALIGN_TOP_LEFT, 8, 110);

    /* SCAN / BACK */
    lv_obj_t *back = makeButton(root, "BACK", C_BORDER, 140, 48, back_cb, back_user_data);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 28, -22);
    s_v.scan_btn = makeButton(root, "SCAN", lv_color_hex(0x1E88E5), 180, 48, onScanClicked, nullptr);
    lv_obj_align(s_v.scan_btn, LV_ALIGN_BOTTOM_LEFT, 184, -22);
    lv_obj_set_style_bg_color(s_v.scan_btn, C_GREY, LV_STATE_DISABLED);

    s_v.status = makeLabel(root, "Sẵn sàng", C_CYAN, &lv_font_be_vietnam_16);
    lv_obj_align(s_v.status, LV_ALIGN_BOTTOM_LEFT, 388, -36);
    lv_obj_set_width(s_v.status, 610);
    lv_label_set_long_mode(s_v.status, LV_LABEL_LONG_DOT);

    populateModels();

    // Lần scan trước còn chạy (rời màn rồi quay lại): chờ nó xong rồi hiện kết quả.
    s_v.seen_serial = HCCRffe::reportSerial();
    if (HCCRffe::busy()) {
        s_v.waiting_scan = true;
        lv_obj_add_state(s_v.scan_btn, LV_STATE_DISABLED);
        setStatus("Đang scan...", C_CYAN);
    }
    s_v.timer = lv_timer_create(onTimer, 150, nullptr);
}

}  // namespace HCCRffeView
