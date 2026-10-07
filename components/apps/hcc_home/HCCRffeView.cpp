#include "HCCRffeView.hpp"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>

#include "HCCRffe.hpp"
#include "HCCRffeImage.hpp"
#include "bsp/touch.h"

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

static const int TABLE_COLS = 6;  // bảng thanh ghi (dữ liệu CSV thư mục)

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
    lv_obj_t *box_title = nullptr;
    lv_obj_t *tiles = nullptr;  // các ô DEVICE / PART (dữ liệu JC)
    // Màn xem ảnh testpoint (trên lv_layer_top, không phải con của root)
    lv_obj_t *viewer = nullptr;
    lv_obj_t *v_scroll = nullptr;
    lv_obj_t *v_img = nullptr;
    lv_obj_t *v_title = nullptr;
    lv_obj_t *v_msg = nullptr;
    lv_timer_t *v_timer = nullptr;  // zoom hai ngón
    std::vector<std::string> imgs;
    int img_idx = 0;
    float z = 1.0f;        // kích thước hiển thị / "vừa màn hình"
    float dl = 1.0f;       // độ phân giải ảnh đang giải mã / "vừa màn hình"
    int fit_w = 0, fit_h = 0, src_w = 0, src_h = 0;
    bool img_waiting = false;
    bool reload = false;   // lần giải mã đang chờ là đổi độ phân giải (không phải ảnh mới)
    float load_dl = 1.0f;
    float keep_cx = 0, keep_cy = 0;
    int pan_x = 0, pan_y = 0;
    bool pinching = false;
    int pinch_idle = 0;
    float p_d0 = 1, p_z0 = 1, p_cx = 0, p_cy = 0;
    uint32_t img_serial = 0;
    char img_info[32] = "";
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
    } else if (u.usid == 0) {
        *color = C_GREY;
        snprintf(buf, n, "Không quét");  // USID 0 là broadcast
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
    const uint32_t cols = TABLE_COLS;
    const uint32_t row = dsc->id / cols;
    const uint32_t col = dsc->id % cols;
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
    if (col == cols - 1 || (cols == TABLE_COLS && col == 2 && st == ROW_FAIL)) {
        dsc->label_dsc->color = st == ROW_PASS ? C_GREEN : st == ROW_FAIL ? C_RED : st == ROW_WARN ? C_ORANGE : C_MUTED;
    }
}

// Đổi bố cục: JC = các ô DEVICE / PART chiếm cả khung (ẩn hàng thẻ USID và bảng thanh ghi).
static void setTableMode(bool mapping)
{
    if (mapping) {
        lv_obj_add_flag(s_v.chips, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_v.id_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_v.table, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_v.tiles, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_v.chips, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_v.tiles, LV_OBJ_FLAG_HIDDEN);
    }
}

// Mỗi linh kiện của model / đường RFFE đang chọn là một ô mang tên, theo thứ tự trong bảng JC:
// xám = chưa scan, xanh = tìm thấy, đỏ = không thấy / sai, cam = lỗi đọc. USID chỉ dùng nội bộ để tìm kết quả.
enum TileState : uint8_t { TILE_IDLE, TILE_OK, TILE_FAIL, TILE_WARN, TILE_SKIP, TILE_EXTRA };

static void addTile(const char *name, const char *sub, TileState st)
{
    static const uint32_t BG[] = {0x2A3A47, 0x1F8A4C, 0xB3261E, 0xC77700, 0x1A242D, 0x8A5A00};
    lv_obj_t *t = lv_obj_create(s_v.tiles);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 178, 64);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(BG[st]), 0);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_set_style_border_width(t, st == TILE_SKIP ? 1 : 0, 0);
    lv_obj_set_style_border_color(t, C_BORDER, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *n = makeLabel(t, name, st == TILE_SKIP ? C_MUTED : C_TEXT, &lv_font_be_vietnam_18);
    lv_obj_set_width(n, 166);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 9);
    lv_obj_t *l = makeLabel(t, sub, (st == TILE_IDLE || st == TILE_SKIP) ? C_MUTED : C_TEXT, &lv_font_be_vietnam_14);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -7);
}

static void renderMapping(void)
{
    const Report &rep = s_v.report;
    lv_obj_add_flag(s_v.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_v.tiles);

    int ok = 0, bad = 0, skip = 0, extra = 0;
    for (const HCCRffe::RefEntry &e : s_v.ref) {
        const UsidResult *u = nullptr;
        for (const UsidResult &x : rep.usids) {
            if (x.usid == e.usid) {
                u = &x;
                break;
            }
        }
        const RegResult *r = nullptr;
        if (u) {
            for (const RegResult &x : u->regs) {
                if (x.reg == e.reg && strcmp(x.name, e.name) == 0) {
                    r = &x;
                    break;
                }
            }
        }
        if (!rep.scanned) {
            addTile(e.name, "Chưa scan", TILE_IDLE);
        } else if (e.usid == 0) {
            addTile(e.name, "Không quét được", TILE_SKIP);  // USID 0 trong bảng = broadcast
            ++skip;
        } else if (u == nullptr || !u->responded) {
            addTile(e.name, "Không thấy", TILE_FAIL);
            ++bad;
        } else if (r == nullptr || !r->read_ok) {
            addTile(e.name, "Lỗi đọc", TILE_WARN);
            ++bad;
        } else if ((r->value & r->mask) == (r->expected & r->mask)) {
            addTile(e.name, "OK", TILE_OK);
            ++ok;
        } else {
            addTile(e.name, "Sai", TILE_FAIL);
            ++bad;
        }
    }
    // Thiết bị trả lời nhưng không có trong bảng của model / đường này.
    for (const UsidResult &u : rep.usids) {
        if (u.in_ref || !u.responded) {
            continue;
        }
        char sub[32];
        snprintf(sub, sizeof(sub), "USID 0x%X", u.usid);
        addTile("Thiết bị lạ", sub, TILE_EXTRA);
        ++extra;
    }

    char sum[160];
    if (rep.scanned) {
        int n = snprintf(sum, sizeof(sum), "Thấy %d / %d   không thấy %d", ok, (int)s_v.ref.size(), bad);
        if (skip) {
            n += snprintf(sum + n, sizeof(sum) - n, "   không quét được %d", skip);
        }
        if (extra) {
            snprintf(sum + n, sizeof(sum) - n, "   lạ %d", extra);
        }
    } else {
        snprintf(sum, sizeof(sum), "%d linh kiện - bấm SCAN", (int)s_v.ref.size());
    }
    lv_label_set_text(s_v.summary, sum);
    lv_obj_scroll_to_y(s_v.tiles, 0, LV_ANIM_OFF);
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
    char ic[40] = "";
    if (u.ic[0]) {
        snprintf(ic, sizeof(ic), "  IC %s", u.ic);
    }
    char line[200];
    if (!rep.scanned) {
        snprintf(line, sizeof(line), "USID 0x%X%s  -  dữ liệu tham chiếu, chưa scan", u.usid, ic);
    } else if (u.usid == 0) {
        snprintf(line, sizeof(line), "USID 0x0%s  -  broadcast, không quét được", ic);
    } else if (!u.responded) {
        snprintf(line, sizeof(line), "USID 0x%X%s  -  không phản hồi", u.usid, ic);
    } else {
        char ids[3][8];
        for (int i = 0; i < 3; ++i) {
            if (u.id_ok[i]) {
                snprintf(ids[i], sizeof(ids[i]), "0x%02X", u.id_regs[i]);
            } else {
                snprintf(ids[i], sizeof(ids[i]), "--");
            }
        }
        snprintf(line, sizeof(line), "USID 0x%X%s   |   0x1D = %s   0x1E = %s   0x1F = %s   (giá trị thô)", u.usid, ic,
                 ids[0], ids[1], ids[2]);
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
        } else if (u.usid == 0) {
            res = "Không quét (USID 0)";
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
    const bool mapping = s_v.have_data && HCCRffe::isJcModel(s_v.model) && !s_v.ref.empty();
    setTableMode(mapping);
    if (mapping) {
        char t[160];
        snprintf(t, sizeof(t), "%s / %s", s_v.model, s_v.data);
        lv_label_set_text(s_v.box_title, t);
        renderMapping();
        return;
    }
    lv_label_set_text(s_v.box_title, "KẾT QUẢ SCAN THEO USID");
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

    char sum[160];
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
                          "Chép file JC_BUS2MM4_iPhone_RFFE_USID.csv vào /rffe/ trên thẻ,\n"
                          "hoặc tạo thư mục /rffe/<MODEL>/ và chép file .csv vào đó.\n"
                          "Mỗi dòng: usid,reg,expected,mask,tên,ic   (vd: 0x5,0x00,0x38,0xFF,REG0,<IC>)\n\n"
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
    if (!s_v.waiting_scan) {
        setStatus("Sẵn sàng", C_CYAN);  // kết quả cũ không còn thuộc model / đường đang chọn
    }
    char info[224] = "";
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
            if (HCCRffe::isJcModel(s_v.model)) {
                snprintf(info, sizeof(info), "%s / %s: %d linh kiện  (JC BUS2MM4 - %s)", s_v.model, s_v.data,
                         (int)s_v.ref.size(), HCCRffe::jcSource());
            } else {
                snprintf(info, sizeof(info), "%s/%s: %d thanh ghi, %d USID", s_v.model, s_v.data,
                         (int)s_v.ref.size(), usids);
            }
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
    static char list[2048];
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
    static char list[2048];  // file JC có ~48 model
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
        setStatus(HCCRffe::configError(), C_ORANGE);
        return;
    }
    // Ghi số thứ tự trước khi chạy: task scan (nhân khác) có thể xong trước khi startScan() trả về.
    s_v.seen_serial = HCCRffe::reportSerial();
    if (!HCCRffe::startScan(s_v.ref)) {
        setStatus("Không khởi động được scan", C_RED);
        return;
    }
    s_v.waiting_scan = true;
    lv_obj_add_state(s_v.scan_btn, LV_STATE_DISABLED);
    setStatus("Đang scan USID 0x1..0xF...", C_CYAN);
}

// ==================== Ảnh testpoint (toàn màn hình) ====================
// Ảnh trên thẻ: /rffe/img/<MODEL>/*.jpg|png (xem HCCRffeImage.hpp). Lớp phủ đặt trên lv_layer_top() để
// phủ cả màn hình (che thanh trạng thái); các nút điều khiển nổi trên ảnh.
//
// Zoom hai ngón: LVGL 8 chỉ nhận một điểm chạm nên đọc đa điểm qua bsp_touch_get_points() (timer 30 ms).
//  - z  : kích thước hiển thị / kích thước "vừa màn hình" (1 .. 4)
//  - dl : độ phân giải của ảnh đang giải mã / "vừa màn hình". Khi chụm tay: đổi lv_img zoom ngay
//         (= z / dl). Khi thả tay: giải mã lại ở độ phân giải gần z (không vượt ảnh gốc) cho nét.
// Một ngón: kéo để di chuyển. Vị trí ảnh tự quản lý (pan_x / pan_y = phần ảnh đã trượt ra ngoài màn bên
// trái / trên); không dùng cuộn của LVGL vì lv_img zoom không làm đổi kích thước đối tượng.

static const int SCR_W = 1024, SCR_H = 600;
static const float Z_MAX = 4.0f;
static const size_t HIRES_MAX_BYTES = 12 * 1024 * 1024;
static const int LV_IMG_MAX_DIM = 2047;  // lv_img_header_t w / h: 11 bit

static lv_obj_t *makeOverlayButton(lv_obj_t *parent, const char *text, int w, lv_event_cb_t cb)
{
    lv_obj_t *b = makeButton(parent, text, lv_color_hex(0x000000), w, 48, cb, nullptr);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    return b;
}

static void viewerSetMessage(const char *msg)
{
    if (s_v.v_msg == nullptr) {
        return;
    }
    if (msg && msg[0]) {
        lv_label_set_text(s_v.v_msg, msg);
        lv_obj_clear_flag(s_v.v_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_v.v_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

static void viewerUpdateTitle(void)
{
    if (s_v.v_title == nullptr) {
        return;
    }
    char t[220];
    if (s_v.imgs.empty()) {
        snprintf(t, sizeof(t), "%s  -  ẢNH TESTPOINT", s_v.model);
    } else {
        const std::string &p = s_v.imgs[s_v.img_idx];
        const size_t slash = p.rfind('/');
        snprintf(t, sizeof(t), "%s   %d / %d   %s%s   %d%%", s_v.model, s_v.img_idx + 1, (int)s_v.imgs.size(),
                 p.c_str() + (slash == std::string::npos ? 0 : slash + 1), s_v.img_info, (int)(s_v.z * 100 + 0.5f));
    }
    lv_label_set_text(s_v.v_title, t);
}

// Gỡ ảnh khỏi lv_img trước khi bộ đệm bị giải phóng.
static void viewerDetachImage(void)
{
    if (s_v.v_img) {
        const void *src = lv_img_get_src(s_v.v_img);
        if (src) {
            lv_img_cache_invalidate_src(src);
        }
        lv_obj_add_flag(s_v.v_img, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(s_v.v_img, nullptr);
    }
}

// Góc trên trái của ảnh trên màn hình: ảnh nhỏ hơn màn thì đặt giữa, lớn hơn thì trượt theo pan.
static void viewerImageOrigin(int *ox, int *oy)
{
    const int disp_w = (int)(s_v.fit_w * s_v.z + 0.5f), disp_h = (int)(s_v.fit_h * s_v.z + 0.5f);
    *ox = disp_w < SCR_W ? (SCR_W - disp_w) / 2 : -s_v.pan_x;
    *oy = disp_h < SCR_H ? (SCR_H - disp_h) / 2 : -s_v.pan_y;
}

// Giới hạn pan trong ảnh rồi đặt vị trí lv_img.
static void viewerPlaceImage(void)
{
    const int disp_w = (int)(s_v.fit_w * s_v.z + 0.5f), disp_h = (int)(s_v.fit_h * s_v.z + 0.5f);
    const int max_x = disp_w > SCR_W ? disp_w - SCR_W : 0, max_y = disp_h > SCR_H ? disp_h - SCR_H : 0;
    s_v.pan_x = s_v.pan_x < 0 ? 0 : (s_v.pan_x > max_x ? max_x : s_v.pan_x);
    s_v.pan_y = s_v.pan_y < 0 ? 0 : (s_v.pan_y > max_y ? max_y : s_v.pan_y);
    int ox, oy;
    viewerImageOrigin(&ox, &oy);
    lv_obj_set_pos(s_v.v_img, ox, oy);
}

// Điểm trên ảnh (đơn vị ảnh "vừa màn hình") đang nằm dưới điểm (ax, ay) của màn hình.
static void viewerContentAt(int ax, int ay, float *cx, float *cy)
{
    const float z = s_v.z > 0 ? s_v.z : 1.0f;
    int ox, oy;
    viewerImageOrigin(&ox, &oy);
    *cx = (ax - ox) / z;
    *cy = (ay - oy) / z;
}

// Đặt mức zoom z, giữ điểm ảnh (cx, cy) nằm dưới điểm màn hình (ax, ay).
static void viewerApplyZoom(float z, int ax, int ay, float cx, float cy)
{
    if (s_v.v_img == nullptr || s_v.fit_w <= 0) {
        return;
    }
    if (z < 1.0f) {
        z = 1.0f;
    }
    if (z > Z_MAX) {
        z = Z_MAX;
    }
    s_v.z = z;
    int lz = (int)(256.0f * z / s_v.dl + 0.5f);
    if (lz < 16) {
        lz = 16;
    }
    lv_img_set_zoom(s_v.v_img, (uint16_t)lz);
    // Điểm ảnh (cx, cy) ở toạ độ cx * z trong ảnh đã phóng -> pan để nó nằm dưới (ax, ay).
    s_v.pan_x = (int)(cx * z - ax + 0.5f);
    s_v.pan_y = (int)(cy * z - ay + 0.5f);
    viewerPlaceImage();
    viewerUpdateTitle();
}

static bool viewerIsPng(void)
{
    const std::string &p = s_v.imgs[s_v.img_idx];
    const size_t dot = p.rfind('.');
    return dot != std::string::npos && strcasecmp(p.c_str() + dot, ".png") == 0;
}

static void viewerStartLoad(float dl, bool reload)
{
    s_v.img_serial = HCCRffeImage::serial();
    s_v.load_dl = dl;
    s_v.reload = reload;
    const int w = (int)(s_v.fit_w > 0 ? s_v.fit_w * dl : SCR_W);
    const int h = (int)(s_v.fit_h > 0 ? s_v.fit_h * dl : SCR_H);
    if (HCCRffeImage::startLoad(s_v.imgs[s_v.img_idx].c_str(), reload ? w : SCR_W, reload ? h : SCR_H)) {
        s_v.img_waiting = true;
    }
}

// Sau khi thả tay: giải mã lại ở độ phân giải gần mức zoom (không vượt ảnh gốc, giới hạn bộ nhớ).
static void viewerMaybeReload(void)
{
    if (s_v.imgs.empty() || s_v.fit_w <= 0 || s_v.img_waiting || HCCRffeImage::busy()) {
        return;
    }
    float dl_max = std::min((float)s_v.src_w / s_v.fit_w, (float)s_v.src_h / s_v.fit_h);
    const float bpp = viewerIsPng() ? 3.0f : 2.0f;
    const float mem_max = sqrtf(HIRES_MAX_BYTES / (bpp * s_v.fit_w * s_v.fit_h));
    // LVGL 8: lv_img_header_t chỉ có 11 bit cho rộng / cao -> ảnh tối đa 2047 px mỗi chiều.
    const float lv_max = std::min((float)LV_IMG_MAX_DIM / s_v.fit_w, (float)LV_IMG_MAX_DIM / s_v.fit_h);
    dl_max = std::min(std::min(std::min(dl_max, mem_max), lv_max), Z_MAX);
    if (dl_max < 1.0f) {
        dl_max = 1.0f;
    }
    const float target = std::min(std::max(s_v.z, 1.0f), dl_max);
    if (fabsf(target / s_v.dl - 1.0f) < 0.2f) {
        return;
    }
    viewerContentAt(SCR_W / 2, SCR_H / 2, &s_v.keep_cx, &s_v.keep_cy);
    viewerStartLoad(target, true);
}

static void viewerLoadCurrent(void)
{
    if (s_v.imgs.empty() || HCCRffeImage::busy()) {
        return;
    }
    viewerDetachImage();
    s_v.img_info[0] = '\0';
    s_v.z = 1.0f;
    s_v.dl = 1.0f;
    s_v.fit_w = s_v.fit_h = 0;
    viewerUpdateTitle();
    viewerStartLoad(1.0f, false);
    viewerSetMessage(s_v.img_waiting ? "Đang tải ảnh..." : "Không giải mã được ảnh");
}

// Gọi từ timer: chuyển sang ảnh vừa giải mã xong.
static void viewerPoll(void)
{
    if (!s_v.img_waiting || HCCRffeImage::busy() || HCCRffeImage::serial() == s_v.img_serial) {
        return;
    }
    s_v.img_waiting = false;
    if (s_v.viewer == nullptr) {
        HCCRffeImage::release();  // màn xem ảnh đã đóng trong lúc giải mã
        return;
    }
    const lv_img_dsc_t *dsc = nullptr;
    int sw = 0, sh = 0;
    char err[128];
    if (!HCCRffeImage::result(&dsc, &sw, &sh, err, sizeof(err))) {
        if (!s_v.reload) {
            viewerSetMessage(err);
        }  // lỗi khi giải mã lại (thiếu bộ nhớ...): giữ ảnh đang hiện
        return;
    }
    const int new_w = dsc->header.w, new_h = dsc->header.h;
    // Đổi nguồn: bỏ cache ảnh cũ, commit (giải phóng ảnh cũ), gán ảnh mới (địa chỉ dsc thay đổi).
    const void *old = lv_img_get_src(s_v.v_img);
    if (old) {
        lv_img_cache_invalidate_src(old);
    }
    HCCRffeImage::commit();
    const lv_img_dsc_t *cur = HCCRffeImage::current();
    lv_img_cache_invalidate_src(cur);
    lv_img_set_src(s_v.v_img, cur);
    lv_img_set_pivot(s_v.v_img, 0, 0);  // phóng từ góc trên trái; vị trí do viewerPlaceImage đặt
    lv_obj_clear_flag(s_v.v_img, LV_OBJ_FLAG_HIDDEN);
    viewerSetMessage(nullptr);
    if (s_v.reload) {
        s_v.dl = (float)new_w / s_v.fit_w;
        viewerApplyZoom(s_v.z, SCR_W / 2, SCR_H / 2, s_v.keep_cx, s_v.keep_cy);
    } else {
        s_v.fit_w = new_w;
        s_v.fit_h = new_h;
        s_v.src_w = sw;
        s_v.src_h = sh;
        s_v.dl = 1.0f;
        snprintf(s_v.img_info, sizeof(s_v.img_info), "   (%dx%d)", sw, sh);
        viewerApplyZoom(1.0f, SCR_W / 2, SCR_H / 2, new_w / 2.0f, new_h / 2.0f);
    }
}

// Timer 30 ms: zoom hai ngón.
static void viewerPinchTick(lv_timer_t *)
{
    if (s_v.viewer == nullptr || s_v.fit_w <= 0) {
        return;
    }
    int16_t x[2], y[2];
    const int n = bsp_touch_get_points(x, y, 2);
    if (n >= 2) {
        const float dx = x[1] - x[0], dy = y[1] - y[0];
        const float d = sqrtf(dx * dx + dy * dy);
        const int ax = (x[0] + x[1]) / 2, ay = (y[0] + y[1]) / 2;
        if (d < 20.0f) {
            return;
        }
        if (!s_v.pinching) {
            s_v.pinching = true;
            s_v.p_d0 = d;
            s_v.p_z0 = s_v.z;
            viewerContentAt(ax, ay, &s_v.p_cx, &s_v.p_cy);
        } else {
            viewerApplyZoom(s_v.p_z0 * d / s_v.p_d0, ax, ay, s_v.p_cx, s_v.p_cy);
        }
        s_v.pinch_idle = 0;
    } else if (s_v.pinching && ++s_v.pinch_idle >= 3) {
        s_v.pinching = false;
        viewerMaybeReload();
    }
}

static void viewerClose(void)
{
    if (s_v.viewer == nullptr) {
        return;
    }
    if (s_v.v_timer) {
        lv_timer_del(s_v.v_timer);
        s_v.v_timer = nullptr;
    }
    viewerDetachImage();
    lv_obj_del(s_v.viewer);
    s_v.viewer = nullptr;
    s_v.v_img = nullptr;
    s_v.v_scroll = nullptr;
    s_v.v_title = nullptr;
    s_v.v_msg = nullptr;
    s_v.pinching = false;
    HCCRffeImage::release();  // nếu còn đang giải mã, viewerPoll sẽ giải phóng khi xong
}

static void onViewerClose(lv_event_t *)
{
    viewerClose();
}

static void onViewerPrev(lv_event_t *)
{
    if (s_v.imgs.size() > 1 && !HCCRffeImage::busy()) {
        s_v.img_idx = (s_v.img_idx + (int)s_v.imgs.size() - 1) % (int)s_v.imgs.size();
        viewerLoadCurrent();
    }
}

static void onViewerNext(lv_event_t *)
{
    if (s_v.imgs.size() > 1 && !HCCRffeImage::busy()) {
        s_v.img_idx = (s_v.img_idx + 1) % (int)s_v.imgs.size();
        viewerLoadCurrent();
    }
}

// VỪA MÀN: về toàn ảnh (và bỏ bản độ phân giải cao).
static void onViewerFit(lv_event_t *)
{
    if (s_v.fit_w <= 0) {
        return;
    }
    viewerApplyZoom(1.0f, SCR_W / 2, SCR_H / 2, s_v.fit_w / 2.0f, s_v.fit_h / 2.0f);
    viewerMaybeReload();
}

// Kéo một ngón (không áp dụng khi đang chụm hai ngón).
static void onViewerDrag(lv_event_t *e)
{
    if (s_v.pinching || s_v.fit_w <= 0) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) {
        return;
    }
    (void)e;
    s_v.pan_x -= v.x;
    s_v.pan_y -= v.y;
    viewerPlaceImage();
}

static void onImageClicked(lv_event_t *)
{
    if (s_v.viewer) {
        return;
    }
    s_v.imgs.clear();
    s_v.img_idx = 0;
    std::string folder;
    char err[160] = "";
    const bool have = s_v.have_models &&
                      HCCRffeImage::list(s_v.model, s_v.have_data ? s_v.data : "", s_v.imgs, folder, err, sizeof(err));

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_v.viewer = ov;
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, SCR_W, SCR_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);  // chặn chạm xuống màn RFFE bên dưới
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    s_v.v_scroll = lv_obj_create(ov);
    lv_obj_remove_style_all(s_v.v_scroll);
    lv_obj_set_size(s_v.v_scroll, SCR_W, SCR_H);
    lv_obj_clear_flag(s_v.v_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_v.v_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_v.v_scroll, onViewerDrag, LV_EVENT_PRESSING, nullptr);
    s_v.v_img = lv_img_create(s_v.v_scroll);
    lv_obj_add_flag(s_v.v_img, LV_OBJ_FLAG_HIDDEN);

    s_v.v_msg = makeLabel(ov, "", C_MUTED, &lv_font_be_vietnam_20);
    lv_obj_set_width(s_v.v_msg, 900);
    lv_label_set_long_mode(s_v.v_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_v.v_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_v.v_msg);

    s_v.v_title = makeLabel(ov, "", C_TEXT, &lv_font_be_vietnam_16);
    lv_obj_set_style_bg_color(s_v.v_title, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_v.v_title, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_v.v_title, 8, 0);
    lv_obj_set_style_radius(s_v.v_title, 8, 0);
    lv_obj_set_width(s_v.v_title, 1000);
    lv_label_set_long_mode(s_v.v_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_v.v_title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *b_close = makeOverlayButton(ov, "ĐÓNG", 120, onViewerClose);
    lv_obj_align(b_close, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_t *b_fit = makeOverlayButton(ov, "VỪA MÀN", 140, onViewerFit);
    lv_obj_align(b_fit, LV_ALIGN_BOTTOM_RIGHT, -144, -12);
    lv_obj_t *b_next = makeOverlayButton(ov, ">", 70, onViewerNext);
    lv_obj_align(b_next, LV_ALIGN_BOTTOM_RIGHT, -296, -12);
    lv_obj_t *b_prev = makeOverlayButton(ov, "<", 70, onViewerPrev);
    lv_obj_align(b_prev, LV_ALIGN_BOTTOM_RIGHT, -378, -12);

    s_v.v_timer = lv_timer_create(viewerPinchTick, 30, nullptr);

    viewerUpdateTitle();
    if (!have) {
        viewerSetMessage(err[0] ? err : "Chưa có ảnh cho model này");
        return;
    }
    viewerLoadCurrent();
}

static void onTimer(lv_timer_t *)
{
    viewerPoll();
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
    viewerClose();  // lớp phủ ảnh nằm trên lv_layer_top, phải xóa riêng
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
    // Vùng app thật nhỏ hơn màn hình (thanh trạng thái Brookesia): khung kết quả co theo chiều cao này
    // để không đè lên nút BACK / SCAN.
    lv_obj_update_layout(root);
    lv_coord_t root_h = lv_obj_get_height(root);
    if (root_h < 360 || root_h > 600) {
        root_h = 520;
    }
    const lv_coord_t box_h = root_h - 162 - 84;  // 84 = nút 48 + lề dưới 22 + khoảng cách 14
    const lv_coord_t inner_h = box_h - 24;        // trừ pad 12 trên + dưới

    lv_obj_t *title = makeLabel(root, "RFFE", C_TEXT, &lv_font_be_vietnam_28);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 14);

    char pins[160];
    if (HCCRffe::configured()) {
        snprintf(pins, sizeof(pins), "SCLK GPIO %d   SDATA GPIO %d (tự đảo nếu nối ngược)   |   bus 1,8 V qua chuyển mức",
                 HCCRffe::sclkPin(), HCCRffe::sdataPin());
    } else {
        snprintf(pins, sizeof(pins), "%s", HCCRffe::configError());
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
    lv_obj_set_size(box, 968, box_h);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 28, 162);
    lv_obj_set_style_bg_color(box, C_PANEL, 0);
    lv_obj_set_style_border_color(box, C_BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    s_v.box_title = makeLabel(box, "KẾT QUẢ SCAN THEO USID", C_CYAN, &lv_font_be_vietnam_16);
    lv_obj_align(s_v.box_title, LV_ALIGN_TOP_LEFT, 0, 0);
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
    lv_obj_set_size(s_v.table, 942, inner_h - 118);
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

    s_v.tiles = lv_obj_create(box);
    lv_obj_remove_style_all(s_v.tiles);
    lv_obj_set_size(s_v.tiles, 944, inner_h - 30);
    lv_obj_align(s_v.tiles, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_flex_flow(s_v.tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_v.tiles, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(s_v.tiles, 10, 0);
    lv_obj_set_style_pad_row(s_v.tiles, 10, 0);
    lv_obj_set_scroll_dir(s_v.tiles, LV_DIR_VER);
    lv_obj_add_flag(s_v.tiles, LV_OBJ_FLAG_HIDDEN);

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

    lv_obj_t *img_btn = makeButton(root, "ẢNH TESTPOINT", lv_color_hex(0x6A4BC4), 210, 48, onImageClicked, nullptr);
    lv_obj_align(img_btn, LV_ALIGN_BOTTOM_LEFT, 380, -22);

    s_v.status = makeLabel(root, "Sẵn sàng", C_CYAN, &lv_font_be_vietnam_16);
    lv_obj_align(s_v.status, LV_ALIGN_BOTTOM_LEFT, 608, -36);
    lv_obj_set_width(s_v.status, 400);
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
