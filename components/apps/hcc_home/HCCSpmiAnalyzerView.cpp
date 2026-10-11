#include "HCCSpmiAnalyzerView.hpp"

#include "HCCLayout.hpp"

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <vector>

#include "HCCSpmiAnalyzer.hpp"
#include "HCCSpmiCapFile.hpp"
#include "HCCSpmiLink.hpp"
#include "HCCSpmiMonitor.hpp"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hcc_cores.h"

namespace HCCSpmiAnalyzerView {

using HCCSpmiDecode::Txn;
using namespace HCCSpmiAnalyzer;

namespace {

const lv_color_t C_BG = lv_color_hex(0x101820);
const lv_color_t C_PANEL = lv_color_hex(0x16232E);
const lv_color_t C_PANEL2 = lv_color_hex(0x1C2C38);
const lv_color_t C_BORDER = lv_color_hex(0x2A3A48);
const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
const lv_color_t C_MUTED = lv_color_hex(0xB8C5CF);
const lv_color_t C_CYAN = lv_color_hex(0x35C2E0);
const lv_color_t C_YELLOW = lv_color_hex(0xF4D03F);
const lv_color_t C_OK = lv_color_hex(0x2ECC71);
const lv_color_t C_WARN = lv_color_hex(0xF0A04B);
const lv_color_t C_ERR = lv_color_hex(0xE74C3C);
const lv_color_t C_PURPLE = lv_color_hex(0x7A4FD8);
const lv_color_t C_BLUE = lv_color_hex(0x1FA9D6);

constexpr int PAGE = 50;
const char *MAP_PATH = BSP_SD_MOUNT_POINT "/spmi/pmic_map.csv";

// ---------- dữ liệu dùng chung với task nền ----------
struct Capture {
    std::vector<Txn> pk;
    uint32_t hz = 1;
    bool demo = false;
    bool lossy = false;  // capture có mất dữ liệu
    std::string name;
};

struct Job {
    volatile int state = 0;  // 0 chưa chạy, 1 đang chạy, 2 xong
    std::string src_a, src_b;  // "" = capture hiện tại
    Capture a, b;
    Result res;
    std::vector<RegRow> regs;
    PmicMap map;
    bool map_found = false;
    char err[200] = "";
};

Job s_job;

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *dd_a = nullptr, *dd_b = nullptr, *run_btn = nullptr;
    lv_obj_t *summary = nullptr, *tabs = nullptr, *table = nullptr, *page_lbl = nullptr, *msg = nullptr;
    lv_timer_t *timer = nullptr;
    int tab = 0;  // 0 trình tự, 1 chỉ khác biệt, 2 thanh ghi cuối, 3 rail
    uint32_t page = 0;
    std::vector<uint32_t> rows;  // chỉ số dòng của tab hiện tại
    bool waiting = false;
    int ncols = 0;
};

View v;
lv_event_cb_t s_back_cb = nullptr;
void *s_back_user = nullptr;
std::vector<std::string> s_files;  // tên file trong /sdcard/spmi (theo thứ tự dropdown, sau "Capture hiện tại")

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
    lv_obj_set_style_bg_color(b, C_BORDER, (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_DISABLED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = label(b, t, &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }
    return b;
}

// ---------- task nền ----------

bool loadCapture(const std::string &src, Capture &c, char *err, size_t es)
{
    c = Capture();
    if (src.empty()) {
        const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();
        const HCCSpmiMonitor::Snapshot sn = HCCSpmiMonitor::snapshot();
        if (sn.running) {
            snprintf(err, es, "SPMI MONITOR đang thu - dừng trước khi so sánh capture hiện tại");
            return false;
        }
        const uint32_t n = pk.count();
        if (n == 0) {
            snprintf(err, es, "Capture hiện tại trống");
            return false;
        }
        c.pk.assign(pk.data(), pk.data() + n);
        c.hz = sn.sample_hz ? sn.sample_hz : 1;
        c.demo = sn.demo;
        c.lossy = sn.lost_chunks || sn.dropped_pkts;
        c.name = "Capture hiện tại";
        return true;
    }
    const std::string path = std::string(BSP_SD_MOUNT_POINT "/spmi/") + src;
    const uint32_t cap = HCCSpmiMonitor::MAX_PACKETS;
    Txn *mem = (Txn *)malloc(sizeof(Txn) * cap);
    if (mem == nullptr) {
        snprintf(err, es, "Hết bộ nhớ để nạp %s", src.c_str());
        return false;
    }
    HCCSpmiBuffers::PacketBuf pb;
    pb.init(mem, cap);
    HCCSpmiCapFile::Info info;
    const size_t len = src.size();
    const bool csv = len > 4 && strcasecmp(src.c_str() + len - 4, ".csv") == 0;
    const bool ok = csv ? HCCSpmiCapFile::importCsv(path.c_str(), info, pb, err, es)
                        : HCCSpmiCapFile::load(path.c_str(), info, pb, nullptr, err, es);
    if (ok) {
        c.pk.assign(pb.data(), pb.data() + pb.count());
        c.hz = info.sample_hz ? info.sample_hz : 1;
        c.demo = (info.flags & HCCSpmiCapFile::CF_DEMO) != 0;
        c.lossy = (info.flags & (HCCSpmiCapFile::CF_OVERFLOW | HCCSpmiCapFile::CF_PKT_TRUNC)) || pb.dropped();
        c.name = src;
    }
    free(mem);
    return ok;
}

void jobTask(void *)
{
    Job &j = s_job;
    j.err[0] = '\0';
    j.res = Result();
    j.regs.clear();
    if (loadCapture(j.src_a, j.a, j.err, sizeof(j.err)) && loadCapture(j.src_b, j.b, j.err, sizeof(j.err))) {
        compare(j.a.pk.data(), (uint32_t)j.a.pk.size(), j.b.pk.data(), (uint32_t)j.b.pk.size(), j.res);
        j.regs = finalRegs(j.a.pk.data(), (uint32_t)j.a.pk.size(), j.b.pk.data(), (uint32_t)j.b.pk.size());
        j.map_found = loadMap(MAP_PATH, j.map);
    }
    j.state = 2;
    vTaskDelete(nullptr);
}

// ---------- hiển thị ----------

void fmtTime(char *b, size_t n, uint64_t t, uint32_t hz)
{
    const double us = (double)t * 1e6 / (double)(hz ? hz : 1);
    if (us < 1e6) {
        snprintf(b, n, "%.3f µs", us);
    } else {
        snprintf(b, n, "%.6f s", us / 1e6);
    }
}

void fmtData(char *b, size_t n, const Txn &t)
{
    if (t.kind == HCCSpmiDecode::K_REG0_WRITE) {
        snprintf(b, n, "%02X", t.cmd & 0x7F);
        return;
    }
    int k = 0;
    b[0] = '\0';
    for (int i = 0; i < t.len && i < 4; ++i) {
        k += snprintf(b + k, n - k, "%s%02X", i ? " " : "", t.data[i]);
    }
    if (t.len > 4) {
        snprintf(b + k, n - k, "..");
    }
    if (t.len == 0) {
        snprintf(b, n, "-");
    }
}

void setupColumns(int n, const int *w, const char *const *hdr)
{
    v.ncols = n;
    lv_table_set_col_cnt(v.table, n);
    lv_table_set_row_cnt(v.table, 1);
    for (int c = 0; c < n; ++c) {
        lv_table_set_col_width(v.table, c, w[c]);
        lv_table_set_cell_value(v.table, 0, c, hdr[c]);
    }
}

void buildRows(void)
{
    v.rows.clear();
    v.page = 0;
    if (s_job.state != 2 || s_job.err[0]) {
        return;
    }
    if (v.tab == 0 || v.tab == 1) {
        for (uint32_t i = 0; i < s_job.res.rows.size(); ++i) {
            if (v.tab == 0 || s_job.res.rows[i].type != D_SAME) {
                v.rows.push_back(i);
            }
        }
    } else if (v.tab == 2) {
        for (uint32_t i = 0; i < s_job.regs.size(); ++i) {
            v.rows.push_back(i);
        }
    } else {
        for (uint32_t i = 0; i < s_job.map.entries.size(); ++i) {
            v.rows.push_back(i);
        }
    }
}

void fillTable(void)
{
    static const int w_seq[9] = {64, 136, 136, 58, 104, 84, 130, 130, 166};
    static const char *h_seq[9] = {"#", "TIME A", "TIME B", "SID", "CMD", "REG", "DATA A", "DATA B", "KẾT QUẢ"};
    static const int w_reg[7] = {80, 120, 160, 160, 130, 130, 228};
    static const char *h_reg[7] = {"SID", "REG", "GHI CUỐI A", "GHI CUỐI B", "SỐ LẦN A", "SỐ LẦN B", "KẾT QUẢ"};
    static const int w_map[7] = {180, 60, 90, 130, 170, 170, 208};
    static const char *h_map[7] = {"NHÃN", "SID", "REG", "MASK / GIÁ TRỊ", "A", "B", "NGUỒN"};
    if (v.tab <= 1) {
        setupColumns(9, w_seq, h_seq);
    } else if (v.tab == 2) {
        setupColumns(7, w_reg, h_reg);
    } else {
        setupColumns(7, w_map, h_map);
    }
    const uint32_t total = (uint32_t)v.rows.size();
    const uint32_t pages = total ? (total + PAGE - 1) / PAGE : 1;
    if (v.page >= pages) {
        v.page = pages - 1;
    }
    const uint32_t first = v.page * PAGE;
    const uint32_t n = total > first ? std::min<uint32_t>(PAGE, total - first) : 0;
    char pl[48];
    snprintf(pl, sizeof(pl), total ? "%u-%u / %u" : "0 / 0", (unsigned)(first + 1), (unsigned)(first + n),
             (unsigned)total);
    lv_label_set_text(v.page_lbl, pl);
    lv_table_set_row_cnt(v.table, n + 1);
    const Job &j = s_job;
    char b[64];
    for (uint32_t r = 0; r < n; ++r) {
        const uint16_t row = (uint16_t)(r + 1);
        const uint32_t k = v.rows[first + r];
        if (v.tab <= 1) {
            const Row &x = j.res.rows[k];
            const Txn *ta = x.ia >= 0 ? &j.a.pk[x.ia] : nullptr;
            const Txn *tb = x.ib >= 0 ? &j.b.pk[x.ib] : nullptr;
            const Txn *t = ta ? ta : tb;
            snprintf(b, sizeof(b), "%u", (unsigned)(k + 1));
            lv_table_set_cell_value(v.table, row, 0, b);
            if (ta) {
                fmtTime(b, sizeof(b), ta->t_sample, j.a.hz);
            } else {
                snprintf(b, sizeof(b), "-");
            }
            lv_table_set_cell_value(v.table, row, 1, b);
            if (tb) {
                fmtTime(b, sizeof(b), tb->t_sample, j.b.hz);
            } else {
                snprintf(b, sizeof(b), "-");
            }
            lv_table_set_cell_value(v.table, row, 2, b);
            if (t->kind == HCCSpmiDecode::K_BAD) {
                lv_table_set_cell_value(v.table, row, 3, "?");
                lv_table_set_cell_value(v.table, row, 4, "LỖI LỆNH");
                lv_table_set_cell_value(v.table, row, 5, "-");
            } else {
                snprintf(b, sizeof(b), "0x%X", t->sid);
                lv_table_set_cell_value(v.table, row, 3, b);
                lv_table_set_cell_value(v.table, row, 4, HCCSpmiDecode::kindName(t->kind));
                if (HCCSpmiDecode::isRead(t->kind) || HCCSpmiDecode::isWrite(t->kind)) {
                    snprintf(b, sizeof(b), t->addr > 0xFF ? "0x%04X" : "0x%02X", t->addr);
                } else {
                    snprintf(b, sizeof(b), "%02X", t->cmd);
                }
                lv_table_set_cell_value(v.table, row, 5, b);
            }
            if (ta) {
                fmtData(b, sizeof(b), *ta);
            } else {
                snprintf(b, sizeof(b), "-");
            }
            lv_table_set_cell_value(v.table, row, 6, b);
            if (tb) {
                fmtData(b, sizeof(b), *tb);
            } else {
                snprintf(b, sizeof(b), "-");
            }
            lv_table_set_cell_value(v.table, row, 7, b);
            lv_table_set_cell_value(v.table, row, 8, diffName(x.type));
        } else if (v.tab == 2) {
            const RegRow &x = j.regs[k];
            snprintf(b, sizeof(b), "0x%X", x.sid);
            lv_table_set_cell_value(v.table, row, 0, b);
            snprintf(b, sizeof(b), x.addr > 0xFF ? "0x%04X" : "0x%02X", x.addr);
            lv_table_set_cell_value(v.table, row, 1, b);
            snprintf(b, sizeof(b), x.has_a ? "0x%02X" : "-", x.last_a);
            lv_table_set_cell_value(v.table, row, 2, b);
            snprintf(b, sizeof(b), x.has_b ? "0x%02X" : "-", x.last_b);
            lv_table_set_cell_value(v.table, row, 3, b);
            snprintf(b, sizeof(b), "%u", (unsigned)x.writes_a);
            lv_table_set_cell_value(v.table, row, 4, b);
            snprintf(b, sizeof(b), "%u", (unsigned)x.writes_b);
            lv_table_set_cell_value(v.table, row, 5, b);
            const char *res = !x.has_b ? "CHỈ CÓ Ở A" : !x.has_a ? "CHỈ CÓ Ở B" : x.last_a == x.last_b ? "GIỐNG" : "KHÁC";
            lv_table_set_cell_value(v.table, row, 6, res);
        } else {
            const MapEntry &e = j.map.entries[k];
            lv_table_set_cell_value(v.table, row, 0, e.label.c_str());
            snprintf(b, sizeof(b), "0x%X", e.sid);
            lv_table_set_cell_value(v.table, row, 1, b);
            snprintf(b, sizeof(b), "0x%02X", e.addr);
            lv_table_set_cell_value(v.table, row, 2, b);
            snprintf(b, sizeof(b), "%02X / %02X", e.mask, e.value);
            lv_table_set_cell_value(v.table, row, 3, b);
            for (int s = 0; s < 2; ++s) {
                const Capture &c = s ? j.b : j.a;
                uint8_t lv = 0;
                const MapState st = applyMap(e, c.pk.data(), (uint32_t)c.pk.size(), &lv);
                if (st == M_NO_WRITE) {
                    snprintf(b, sizeof(b), "%s", mapStateName(st));
                } else {
                    snprintf(b, sizeof(b), "%s (%02X)", st == M_MATCH ? "KHỚP" : "KHÁC", lv);
                }
                lv_table_set_cell_value(v.table, row, 4 + s, b);
            }
            lv_table_set_cell_value(v.table, row, 6, e.source.c_str());
        }
    }
    lv_obj_scroll_to_y(v.table, 0, LV_ANIM_OFF);
}

void showSummary(void)
{
    const Job &j = s_job;
    if (j.state != 2) {
        return;
    }
    if (j.err[0]) {
        lv_label_set_text(v.summary, j.err);
        lv_obj_set_style_text_color(v.summary, C_ERR, 0);
        return;
    }
    std::string s;
    char b[256];
    if (j.a.demo || j.b.demo) {
        s += "[DEMO - dữ liệu mô phỏng] ";
    }
    snprintf(b, sizeof(b), "A: %s - %u gói, %u lỗi%s   ·   B: %s - %u gói, %u lỗi%s\n", j.a.name.c_str(),
             (unsigned)j.a.pk.size(), (unsigned)j.res.err_a, j.a.lossy ? ", CÓ MẤT DỮ LIỆU" : "", j.b.name.c_str(),
             (unsigned)j.b.pk.size(), (unsigned)j.res.err_b, j.b.lossy ? ", CÓ MẤT DỮ LIỆU" : "");
    s += b;
    snprintf(b, sizeof(b), "Giống %u · Khác dữ liệu %u · Thiếu ở B %u · Thừa ở B %u · Lỗi giao tiếp %u%s",
             (unsigned)j.res.same, (unsigned)j.res.data, (unsigned)j.res.missing, (unsigned)j.res.extra,
             (unsigned)j.res.err, j.res.limited ? " (đã cắt bớt)" : "");
    s += b;
    if (j.res.first_diff >= 0) {
        const Row &r = j.res.rows[j.res.first_diff];
        char tb[32] = "-";
        if (r.ia >= 0) {
            fmtTime(tb, sizeof(tb), j.a.pk[r.ia].t_sample, j.a.hz);
        } else if (r.ib >= 0) {
            fmtTime(tb, sizeof(tb), j.b.pk[r.ib].t_sample, j.b.hz);
        }
        snprintf(b, sizeof(b), "\nKhác biệt đầu tiên: dòng %d (%s) tại %s", j.res.first_diff + 1, diffName(r.type), tb);
        s += b;
    } else {
        s += "\nKhông có khác biệt về trình tự lệnh.";
    }
    if (v.tab == 3) {
        if (!j.map_found) {
            s += "\nRAIL: chưa có mapping thanh ghi PMIC đã xác minh (/sdcard/spmi/pmic_map.csv) - không suy luận trạng "
                 "thái rail.";
        } else {
            snprintf(b, sizeof(b), "\nRAIL: %u dòng mapping hợp lệ, %u dòng bị loại (thiếu nguồn). Chỉ so lệnh ghi - "
                                   "KHÔNG xác nhận điện áp, cần đo thực tế.",
                     (unsigned)j.map.entries.size(), (unsigned)j.map.rejected);
            s += b;
        }
    } else {
        s += "\nHeuristic dóng hàng - xem lại các dòng khác biệt; không kết luận lỗi phần cứng chỉ từ lệnh SPMI.";
    }
    lv_label_set_text(v.summary, s.c_str());
    lv_obj_set_style_text_color(v.summary, (j.a.demo || j.b.demo) ? C_YELLOW : C_TEXT, 0);
}

void onTableDraw(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->part != LV_PART_ITEMS || v.ncols == 0) {
        return;
    }
    const uint32_t row = d->id / v.ncols;
    if (row == 0) {
        d->label_dsc->color = C_CYAN;
        d->rect_dsc->bg_color = C_PANEL2;
        d->rect_dsc->bg_opa = LV_OPA_COVER;
        return;
    }
    const uint32_t k = v.page * PAGE + row - 1;
    if (k >= v.rows.size() || s_job.state != 2) {
        return;
    }
    lv_color_t c = C_TEXT;
    if (v.tab <= 1) {
        switch (s_job.res.rows[v.rows[k]].type) {
        case D_SAME: c = C_MUTED; break;
        case D_DATA: c = C_YELLOW; break;
        case D_MISSING: c = C_WARN; break;
        case D_EXTRA: c = C_BLUE; break;
        case D_ERR: c = C_ERR; break;
        }
    } else if (v.tab == 2) {
        const RegRow &x = s_job.regs[v.rows[k]];
        c = (x.has_a && x.has_b && x.last_a == x.last_b) ? C_MUTED : C_YELLOW;
    }
    d->label_dsc->color = c;
}

void onTab(lv_event_t *e)
{
    v.tab = (int)lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    buildRows();
    fillTable();
    showSummary();
}

void onPrev(lv_event_t *)
{
    if (v.page > 0) {
        --v.page;
        fillTable();
    }
}

void onNext(lv_event_t *)
{
    if ((v.page + 1) * PAGE < v.rows.size()) {
        ++v.page;
        fillTable();
    }
}

std::string pick(lv_obj_t *dd)
{
    const uint16_t s = lv_dropdown_get_selected(dd);
    return (s == 0 || s > s_files.size()) ? std::string() : s_files[s - 1];
}

void onRun(lv_event_t *)
{
    if (s_job.state == 1) {
        return;
    }
    s_job.src_a = pick(v.dd_a);
    s_job.src_b = pick(v.dd_b);
    if (s_job.src_a == s_job.src_b) {
        lv_label_set_text(v.msg, "A và B đang là cùng một capture");
    }
    v.rows.clear();  // không đọc kết quả cũ trong lúc task nền ghi đè
    s_job.state = 1;
    fillTable();
    if (xTaskCreatePinnedToCore(jobTask, "hcc_spmi_cmp", 8192, nullptr, 4, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_job.state = 0;
        lv_label_set_text(v.msg, "Không tạo được task so sánh");
        return;
    }
    v.waiting = true;
    lv_obj_add_state(v.run_btn, LV_STATE_DISABLED);
    lv_label_set_text(v.summary, "Đang nạp và so sánh...");
    lv_obj_set_style_text_color(v.summary, C_CYAN, 0);
}

void onLink(lv_event_t *e)
{
    const HCCSpmiLink::Target t = (HCCSpmiLink::Target)(uintptr_t)lv_event_get_user_data(e);
    HCCSpmiLink::Ref r = {0, 0, false};
    if (s_job.state == 2 && s_job.res.first_diff >= 0) {
        const Row &x = s_job.res.rows[s_job.res.first_diff];
        const Txn &tx = x.ia >= 0 ? s_job.a.pk[x.ia] : s_job.b.pk[x.ib];
        r = {tx.sid, tx.addr, true};
    }
    HCCSpmiLink::open(t, r);
}

void onTimer(lv_timer_t *)
{
    if (v.waiting && s_job.state == 2) {
        v.waiting = false;
        lv_obj_clear_state(v.run_btn, LV_STATE_DISABLED);
        buildRows();
        fillTable();
        showSummary();
        lv_label_set_text(v.msg, s_job.err[0] ? "" : "Xong");
    }
}

void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
    }
    v = View();
}

lv_obj_t *dropdown(lv_obj_t *parent, const std::string &opts, int x, int y, int w)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts.c_str());
    lv_obj_set_pos(dd, x, y);
    lv_obj_set_size(dd, w, 42);
    lv_obj_set_style_text_font(dd, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_bg_color(dd, C_PANEL2, 0);
    lv_obj_set_style_text_color(dd, C_TEXT, 0);
    lv_obj_set_style_border_color(dd, C_BORDER, 0);
    lv_obj_set_style_pad_ver(dd, 10, 0);
    return dd;
}

}  // namespace

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user)
{
    if (v.root) {
        lv_obj_del(v.root);
    }
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
    lv_coord_t H = HCCLayout::usableHeight(root, 503);
    if (H < 420 || H > 800) {
        H = 527;
    }

    lv_obj_t *title = label(root, "PMIC ANALYZER", &lv_font_be_vietnam_24, C_TEXT);
    lv_obj_set_pos(title, 12, 6);
    lv_obj_t *note = label(root, "So sánh lệnh SPMI main tốt / main lỗi. Không kết luận điện áp rail từ lệnh SPMI.",
                           &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(note, 214, 13);

    // Danh sách nguồn.
    s_files.clear();
    DIR *d = opendir(BSP_SD_MOUNT_POINT "/spmi");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr && s_files.size() < 200) {
            const char *n = de->d_name;
            const size_t len = strlen(n);
            if ((len > 5 && strcasecmp(n + len - 5, ".spmc") == 0) || (len > 4 && strcasecmp(n + len - 4, ".csv") == 0)) {
                s_files.push_back(n);
            }
        }
        closedir(d);
    }
    std::sort(s_files.begin(), s_files.end(), std::greater<std::string>());
    std::string opts = "Capture hiện tại";
    for (const std::string &f : s_files) {
        opts += "\n" + f;
    }
    lv_obj_t *la = label(root, "A (main tốt)", &lv_font_be_vietnam_16, C_OK);
    lv_obj_set_pos(la, 12, 52);
    v.dd_a = dropdown(root, opts, 118, 42, 320);
    lv_obj_t *lb = label(root, "B (main lỗi)", &lv_font_be_vietnam_16, C_ERR);
    lv_obj_set_pos(lb, 452, 52);
    v.dd_b = dropdown(root, opts, 556, 42, 320);
    if (!s_files.empty()) {
        lv_dropdown_set_selected(v.dd_a, 1);
    }
    v.run_btn = button(root, "SO SÁNH", 128, 42, C_BLUE, onRun, nullptr);
    lv_obj_set_pos(v.run_btn, 886, 42);

    v.summary = label(root, "Chọn capture A và B rồi bấm SO SÁNH.", &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_set_pos(v.summary, 12, 92);
    lv_obj_set_width(v.summary, 1000);
    lv_label_set_long_mode(v.summary, LV_LABEL_LONG_WRAP);

    const int ty = 200;
    static const char *tab_map[] = {"TRÌNH TỰ", "CHỈ KHÁC BIỆT", "THANH GHI CUỐI", "RAIL (MAPPING)", ""};
    v.tabs = lv_btnmatrix_create(root);
    lv_btnmatrix_set_map(v.tabs, tab_map);
    lv_btnmatrix_set_btn_ctrl_all(v.tabs, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(v.tabs, true);
    lv_btnmatrix_set_btn_ctrl(v.tabs, 0, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_pos(v.tabs, 8, ty);
    lv_obj_set_size(v.tabs, 700, 40);
    lv_obj_set_style_pad_all(v.tabs, 2, 0);
    lv_obj_set_style_pad_gap(v.tabs, 4, 0);
    lv_obj_set_style_bg_color(v.tabs, C_PANEL, 0);
    lv_obj_set_style_border_width(v.tabs, 0, 0);
    lv_obj_set_style_text_font(v.tabs, &lv_font_be_vietnam_16, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(v.tabs, C_PANEL2, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.tabs, C_TEXT, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(v.tabs, C_BLUE, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_add_event_cb(v.tabs, onTab, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *pv = button(root, "<", 44, 40, C_PANEL2, onPrev, nullptr);
    lv_obj_set_pos(pv, 718, ty);
    v.page_lbl = label(root, "0 / 0", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_width(v.page_lbl, 196);
    lv_obj_set_style_text_align(v.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v.page_lbl, 766, ty + 10);
    lv_obj_t *nx = button(root, ">", 44, 40, C_PANEL2, onNext, nullptr);
    lv_obj_set_pos(nx, 968, ty);

    const int by = H - 50;
    v.table = lv_table_create(root);
    lv_obj_set_pos(v.table, 8, ty + 46);
    lv_obj_set_size(v.table, 1008, by - ty - 52);
    lv_obj_set_style_bg_color(v.table, C_BG, 0);
    lv_obj_set_style_border_color(v.table, C_BORDER, 0);
    lv_obj_set_style_border_width(v.table, 1, 0);
    lv_obj_set_style_pad_all(v.table, 0, 0);
    lv_obj_set_style_text_font(v.table, &lv_font_be_vietnam_14, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(v.table, C_PANEL, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.table, C_TEXT, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_border_color(v.table, C_BORDER, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_border_width(v.table, 1, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(v.table, 3, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(v.table, 6, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_add_event_cb(v.table, onTableDraw, LV_EVENT_DRAW_PART_BEGIN, nullptr);

    lv_obj_t *back = button(root, LV_SYMBOL_LEFT " BACK", 130, 44, C_BORDER, back_cb, back_user);
    lv_obj_set_pos(back, 8, by);
    for (int k = 0; k < 2; ++k) {
        const HCCSpmiLink::Target t = (HCCSpmiLink::Target)k;
        const bool ok = HCCSpmiLink::available(t);
        char b[48];
        snprintf(b, sizeof(b), "%s%s", HCCSpmiLink::targetName(t), ok ? "" : " (chưa nối)");
        lv_obj_t *lk = button(root, b, 230, 44, C_PURPLE, onLink, (void *)(uintptr_t)k);
        lv_obj_set_pos(lk, 146 + k * 238, by);
        if (!ok) {
            lv_obj_add_state(lk, LV_STATE_DISABLED);
        }
    }
    v.msg = label(root, "", &lv_font_be_vietnam_16, C_CYAN);
    lv_obj_set_pos(v.msg, 630, by + 12);
    lv_obj_set_width(v.msg, 384);
    lv_label_set_long_mode(v.msg, LV_LABEL_LONG_DOT);

    fillTable();
    if (s_job.state == 2) {
        buildRows();
        fillTable();
        showSummary();
    } else if (s_job.state == 1) {
        v.waiting = true;
        lv_obj_add_state(v.run_btn, LV_STATE_DISABLED);
    }
    v.timer = lv_timer_create(onTimer, 150, nullptr);
}

}  // namespace HCCSpmiAnalyzerView
