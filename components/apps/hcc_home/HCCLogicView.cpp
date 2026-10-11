#include "HCCLogicView.hpp"

#include "HCCLayout.hpp"

#include <algorithm>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <vector>

#include "HCCLogic.hpp"
#include "HCCLogicCore.hpp"
#include "HCCLogicDecode.hpp"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hcc_cores.h"

namespace HCCLogicView {

using HCCLogic::State;
using HCCLogicCore::Trigger;
using HCCLogicDecode::Annot;

namespace {

const lv_color_t C_BG = lv_color_hex(0x101820);
const lv_color_t C_PANEL = lv_color_hex(0x16232E);
const lv_color_t C_PANEL2 = lv_color_hex(0x1C2C38);
const lv_color_t C_BORDER = lv_color_hex(0x2A3A48);
const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
const lv_color_t C_MUTED = lv_color_hex(0xB8C5CF);
const lv_color_t C_DIM = lv_color_hex(0x5D7080);
const lv_color_t C_CYAN = lv_color_hex(0x35C2E0);
const lv_color_t C_YELLOW = lv_color_hex(0xF4D03F);
const lv_color_t C_OK = lv_color_hex(0x2ECC71);
const lv_color_t C_WARN = lv_color_hex(0xF0A04B);
const lv_color_t C_ERR = lv_color_hex(0xE74C3C);
const lv_color_t C_PURPLE = lv_color_hex(0x7A4FD8);
const lv_color_t C_BLUE = lv_color_hex(0x1FA9D6);
const lv_color_t C_MAGENTA = lv_color_hex(0xE056C8);

// Màu kênh kiểu máy logic analyzer (theo mã màu điện trở).
const uint32_t CH_COLOR[8] = {0x9E9E9E, 0xC0703A, 0xE74C3C, 0xF39C12, 0xF4D03F, 0x2ECC71, 0x3498DB, 0xB07CE0};

const uint32_t RATES[] = {100000, 200000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000, 40000000, 80000000};
const char *RATE_OPTS = "100 kHz\n200 kHz\n500 kHz\n1 MHz\n2 MHz\n5 MHz\n10 MHz\n20 MHz\n40 MHz\n80 MHz";
const uint32_t DEPTHS[] = {256 * 1024, 1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024};
const char *DEPTH_OPTS = "256K mẫu\n1M mẫu\n2M mẫu\n4M mẫu\n8M mẫu";
const uint8_t PRES[] = {0, 10, 25, 50, 75, 90};
const char *PRE_OPTS = "0 %\n10 %\n25 %\n50 %\n75 %\n90 %";
const uint32_t BAUDS[] = {0, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 1500000};
const char *BAUD_OPTS = "AUTO\n9600\n19200\n38400\n57600\n115200\n230400\n460800\n921600\n1000000\n1500000";
constexpr int LABEL_W = 78;
constexpr int NDEC = 2;
constexpr int DEC_H = 24;
constexpr int AXIS_H = 20;

// ---------- cài đặt (giữ qua các lần mở màn) ----------
struct Settings {
    int rate = 6;   // 10 MHz
    int depth = 2;  // 2M
    int pre = 1;    // 10 %
    bool demo = false;
    Trigger trig;
    HCCLogicDecode::Config dec[NDEC];
    int baud_sel[NDEC] = {0, 0};
};
Settings s_set;

// ---------- giải mã nền ----------
struct DecodeJob {
    volatile int state = 0;  // 0 không có, 1 đang chạy, 2 xong
    HCCLogicDecode::Config cfg[NDEC];
    HCCLogicDecode::Result res[NDEC];
    uint32_t samples = 0, hz = 0;
};
DecodeJob s_job;

void decodeTask(void *)
{
    const uint8_t *d = HCCLogic::data();
    for (int k = 0; k < NDEC; ++k) {
        HCCLogicDecode::decode(s_job.cfg[k], d, s_job.samples, s_job.hz, s_job.res[k]);
    }
    s_job.state = 2;
    vTaskDelete(nullptr);
}

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *badge = nullptr, *badge_lbl = nullptr, *demo_tag = nullptr, *info = nullptr, *msg = nullptr;
    lv_obj_t *wave = nullptr, *meas = nullptr, *cur = nullptr;
    lv_obj_t *run_btn = nullptr, *stop_btn = nullptr;
    lv_obj_t *modal = nullptr;
    lv_obj_t *pat_btn[8] = {};
    lv_obj_t *dec_dd[NDEC][6] = {};
    lv_obj_t *dec_role[NDEC] = {};
    lv_timer_t *timer = nullptr;
    lv_coord_t H = 503;
    uint32_t seen_serial = 0;
    bool saving = false;
    // khung nhìn
    double t0 = 0, spp = 1;
    uint32_t n = 0, hz = 1, trig = 0xFFFFFFFF;
    bool demo = false;
    int sel_ch = 0;
    double cur_a = -1, cur_b = -1;
    bool next_b = false;
    lv_coord_t drag = 0;
    bool decode_shown = false;
    char names[8][8];
};

View v;
lv_event_cb_t s_back_cb = nullptr;
void *s_back_user = nullptr;
uint16_t s_cols[1024];
std::vector<std::string> s_files;

lv_obj_t *label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *button(lv_obj_t *p, const char *t, int w, int h, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(p);
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

lv_obj_t *dropdown(lv_obj_t *p, const char *opts, int sel, int x, int y, int w)
{
    lv_obj_t *dd = lv_dropdown_create(p);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, (uint16_t)sel);
    lv_obj_set_pos(dd, x, y);
    lv_obj_set_size(dd, w, 40);
    lv_obj_set_style_text_font(dd, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_bg_color(dd, C_PANEL2, 0);
    lv_obj_set_style_text_color(dd, C_TEXT, 0);
    lv_obj_set_style_border_color(dd, C_BORDER, 0);
    lv_obj_set_style_pad_ver(dd, 9, 0);
    return dd;
}

void setEnabled(lv_obj_t *o, bool en)
{
    if (en) {
        lv_obj_clear_state(o, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(o, LV_STATE_DISABLED);
    }
}

void fmtTime(char *b, size_t n, double s)
{
    const double a = fabs(s);
    if (a < 1e-6) {
        snprintf(b, n, "%.1f ns", s * 1e9);
    } else if (a < 1e-3) {
        snprintf(b, n, "%.3f µs", s * 1e6);
    } else if (a < 1.0) {
        snprintf(b, n, "%.3f ms", s * 1e3);
    } else {
        snprintf(b, n, "%.4f s", s);
    }
}

void fmtFreq(char *b, size_t n, double f)
{
    if (f >= 1e6) {
        snprintf(b, n, "%.4f MHz", f / 1e6);
    } else if (f >= 1e3) {
        snprintf(b, n, "%.3f kHz", f / 1e3);
    } else {
        snprintf(b, n, "%.2f Hz", f);
    }
}

void fmtRate(char *b, size_t n, uint32_t hz)
{
    if (hz >= 1000000) {
        snprintf(b, n, "%u MHz", (unsigned)(hz / 1000000));
    } else {
        snprintf(b, n, "%u kHz", (unsigned)(hz / 1000));
    }
}

// ---------- tên kênh ----------
void updateNames(void)
{
    char from_file[8][8];
    const bool f = HCCLogic::fileNames(from_file);
    for (int k = 0; k < 8; ++k) {
        snprintf(v.names[k], 8, "%s", f && from_file[k][0] ? from_file[k] : "");
    }
    static const char *ROLE[5][4] = {{"", "", "", ""},
                                     {"SCL", "SDA", "", ""},
                                     {"RX", "", "", ""},
                                     {"CLK", "MOSI", "MISO", "CS"},
                                     {"SCLK", "SDATA", "", ""}};
    for (int s = 0; s < NDEC; ++s) {
        const HCCLogicDecode::Config &c = s_set.dec[s];
        for (int r = 0; r < 4; ++r) {
            if (c.proto != HCCLogicDecode::P_NONE && c.ch[r] >= 0 && ROLE[c.proto][r][0]) {
                snprintf(v.names[c.ch[r]], 8, "%s", ROLE[c.proto][r]);
            }
        }
    }
}

// ---------- khung nhìn ----------
int waveCols(void)
{
    return std::max(16, std::min(1000, (int)lv_obj_get_width(v.wave) - LABEL_W - 6));
}

double maxSpp(void)
{
    return std::max(1.0, (double)v.n / waveCols());
}

void clampView(void)
{
    v.spp = std::max(1.0 / 32, std::min(maxSpp(), v.spp));
    const double span = waveCols() * v.spp;
    v.t0 = std::max(-span * 0.05, std::min((double)v.n - span * 0.95, v.t0));
}

void fitAll(void)
{
    v.spp = maxSpp();
    v.t0 = 0;
    clampView();
}

int rowH(void)
{
    const int h = lv_obj_get_height(v.wave) - AXIS_H - (v.decode_shown ? NDEC * DEC_H : 0) - 4;
    return std::max(16, h / 8);
}

void updateMeasure(void);
void updateCursorText(void);

// ---------- vẽ ----------
void drawWave(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    const int x0 = a.x1 + LABEL_W;
    const int ncols = waveCols();
    const int rh = rowH();
    const uint8_t *d = HCCLogic::data();
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_be_vietnam_14;

    // tên kênh
    for (int k = 0; k < 8; ++k) {
        const int y = a.y1 + 2 + k * rh;
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = k == v.sel_ch ? lv_color_hex(0x1B4A66) : (k & 1 ? C_PANEL : C_PANEL2);
        rd.radius = 0;
        lv_area_t ra = {(lv_coord_t)a.x1, (lv_coord_t)y, (lv_coord_t)(x0 - 3), (lv_coord_t)(y + rh - 2)};
        lv_draw_rect(ctx, &rd, &ra);
        lv_draw_rect_dsc_t cd;
        lv_draw_rect_dsc_init(&cd);
        cd.bg_color = lv_color_hex(CH_COLOR[k]);
        lv_area_t ca = {(lv_coord_t)a.x1, (lv_coord_t)y, (lv_coord_t)(a.x1 + 4), (lv_coord_t)(y + rh - 2)};
        lv_draw_rect(ctx, &cd, &ca);
        char b[24];
        snprintf(b, sizeof(b), "D%d %s", k, v.names[k]);
        ld.color = C_TEXT;
        lv_area_t ta = {(lv_coord_t)(a.x1 + 8), (lv_coord_t)(y + 1), (lv_coord_t)(x0 - 4), (lv_coord_t)(y + 17)};
        lv_draw_label(ctx, &ld, &ta, b, nullptr);
        if (rh >= 30) {
            const int p = HCCLogic::pin(k);
            if (p >= 0) {
                snprintf(b, sizeof(b), "GPIO%d", p);
            } else {
                snprintf(b, sizeof(b), "-");
            }
            ld.color = C_DIM;
            ld.font = &lv_font_be_vietnam_12;
            lv_area_t pa = {(lv_coord_t)(a.x1 + 8), (lv_coord_t)(y + 17), (lv_coord_t)(x0 - 4), (lv_coord_t)(y + 30)};
            lv_draw_label(ctx, &ld, &pa, b, nullptr);
            ld.font = &lv_font_be_vietnam_14;
        }
    }
    const int ybot = a.y1 + 2 + 8 * rh + (v.decode_shown ? NDEC * DEC_H : 0);

    // lưới mỗi 100 điểm ảnh
    lv_draw_line_dsc_t gl;
    lv_draw_line_dsc_init(&gl);
    gl.color = C_BORDER;
    gl.width = 1;
    for (int x = 0; x <= ncols; x += 100) {
        lv_point_t p1 = {(lv_coord_t)(x0 + x), (lv_coord_t)(a.y1 + 2)};
        lv_point_t p2 = {(lv_coord_t)(x0 + x), (lv_coord_t)ybot};
        lv_draw_line(ctx, &gl, &p1, &p2);
    }

    if (v.n == 0 || d == nullptr) {
        ld.color = C_MUTED;
        ld.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t ta = {(lv_coord_t)x0, (lv_coord_t)(a.y1 + 4 * rh - 10), (lv_coord_t)(x0 + ncols),
                        (lv_coord_t)(a.y1 + 4 * rh + 10)};
        lv_draw_label(ctx, &ld, &ta, "Chưa có dữ liệu - bấm RUN (CÀI ĐẶT > nguồn DEMO để thử không cần que đo)",
                      nullptr);
        return;
    }

    HCCLogicCore::columns(d, v.n, &HCCLogic::mipmap(), v.t0, v.spp, ncols, s_cols);
    for (int k = 0; k < 8; ++k) {
        const int y = a.y1 + 2 + k * rh;
        const int yh = y + 4, yl = y + rh - 6;
        lv_draw_line_dsc_t dl;
        lv_draw_line_dsc_init(&dl);
        dl.color = lv_color_hex(CH_COLOR[k]);
        dl.width = 2;
        lv_draw_rect_dsc_t blk;
        lv_draw_rect_dsc_init(&blk);
        blk.bg_color = lv_color_hex(CH_COLOR[k]);
        blk.bg_opa = LV_OPA_50;
        blk.radius = 0;
        const uint16_t lo = (uint16_t)(1u << k), hi = (uint16_t)(1u << (8 + k));
        auto lvl = [&](int c) {
            const uint16_t m = s_cols[c];
            if (m == 0) {
                return 3;
            }
            if ((m & lo) && (m & hi)) {
                return 2;
            }
            return (m & hi) ? 1 : 0;
        };
        int c = 0, prev = -1;
        while (c < ncols) {
            const int l = lvl(c);
            int e2 = c + 1;
            while (e2 < ncols && lvl(e2) == l) {
                ++e2;
            }
            if (l == 2) {  // nhiều cạnh trong một cột: vẽ dải đặc
                lv_area_t ra = {(lv_coord_t)(x0 + c), (lv_coord_t)yh, (lv_coord_t)(x0 + e2 - 1), (lv_coord_t)yl};
                lv_draw_rect(ctx, &blk, &ra);
            } else if (l != 3) {
                const int yy = l ? yh : yl;
                if ((prev == 0 || prev == 1) && prev != l) {
                    lv_point_t p1 = {(lv_coord_t)(x0 + c), (lv_coord_t)yh};
                    lv_point_t p2 = {(lv_coord_t)(x0 + c), (lv_coord_t)yl};
                    lv_draw_line(ctx, &dl, &p1, &p2);
                }
                lv_point_t p1 = {(lv_coord_t)(x0 + c), (lv_coord_t)yy};
                lv_point_t p2 = {(lv_coord_t)(x0 + e2), (lv_coord_t)yy};
                lv_draw_line(ctx, &dl, &p1, &p2);
            }
            prev = l;
            c = e2;
        }
    }

    // chú thích giải mã
    if (v.decode_shown && s_job.state == 2) {
        const double tend = v.t0 + ncols * v.spp;
        for (int s = 0; s < NDEC; ++s) {
            const int y = a.y1 + 2 + 8 * rh + s * DEC_H;
            const std::vector<Annot> &an = s_job.res[s].ann;
            if (s_job.cfg[s].proto == HCCLogicDecode::P_NONE) {
                continue;
            }
            char b[40];
            snprintf(b, sizeof(b), "%s", HCCLogicDecode::protoName(s_job.cfg[s].proto));
            ld.color = C_CYAN;
            ld.align = LV_TEXT_ALIGN_LEFT;
            lv_area_t na = {(lv_coord_t)(a.x1 + 8), (lv_coord_t)(y + 3), (lv_coord_t)(x0 - 4), (lv_coord_t)(y + 20)};
            lv_draw_label(ctx, &ld, &na, b, nullptr);
            size_t i = HCCLogicDecode::firstVisible(an, v.t0 < 0 ? 0 : (uint32_t)v.t0);
            int drawn = 0;
            for (; i < an.size() && drawn < 400; ++i) {
                const Annot &x = an[i];
                if (x.t0 > tend) {
                    break;
                }
                int xa = x0 + (int)(((double)x.t0 - v.t0) / v.spp);
                int xb = x0 + (int)(((double)x.t1 - v.t0) / v.spp);
                xa = std::max(xa, x0);
                xb = std::min(xb, x0 + ncols);
                if (xb < xa) {
                    continue;
                }
                ++drawn;
                lv_color_t col = C_BLUE;
                switch (x.kind) {
                case HCCLogicDecode::A_START:
                case HCCLogicDecode::A_STOP: col = C_OK; break;
                case HCCLogicDecode::A_ADDR: col = C_PURPLE; break;
                case HCCLogicDecode::A_NACK: col = C_WARN; break;
                case HCCLogicDecode::A_ERR: col = C_ERR; break;
                default: break;
                }
                lv_draw_rect_dsc_t rd;
                lv_draw_rect_dsc_init(&rd);
                rd.bg_color = col;
                rd.radius = 4;
                const bool point = x.kind == HCCLogicDecode::A_START || x.kind == HCCLogicDecode::A_STOP;
                if (point) {
                    xa -= 8;
                    xb = xa + 16;
                }
                lv_area_t ra = {(lv_coord_t)xa, (lv_coord_t)(y + 2), (lv_coord_t)std::max(xb, xa + 2),
                                (lv_coord_t)(y + DEC_H - 3)};
                lv_draw_rect(ctx, &rd, &ra);
                const int tw = (int)strlen(x.text) * 8;
                if (xb - xa >= tw || (point && xb - xa >= 12)) {
                    ld.color = C_TEXT;
                    ld.align = LV_TEXT_ALIGN_CENTER;
                    lv_area_t ta = {(lv_coord_t)xa, (lv_coord_t)(y + 3), (lv_coord_t)xb, (lv_coord_t)(y + 20)};
                    lv_draw_label(ctx, &ld, &ta, x.text, nullptr);
                }
            }
        }
    }

    // điểm kích + con trỏ
    auto vline = [&](double t, lv_color_t col, const char *tag) {
        const double xf = (t - v.t0) / v.spp;
        if (xf < 0 || xf > ncols) {
            return;
        }
        lv_draw_line_dsc_t m;
        lv_draw_line_dsc_init(&m);
        m.color = col;
        m.width = 2;
        m.dash_width = 4;
        m.dash_gap = 3;
        lv_point_t p1 = {(lv_coord_t)(x0 + xf), (lv_coord_t)(a.y1 + 2)};
        lv_point_t p2 = {(lv_coord_t)(x0 + xf), (lv_coord_t)ybot};
        lv_draw_line(ctx, &m, &p1, &p2);
        ld.color = col;
        ld.align = LV_TEXT_ALIGN_LEFT;
        lv_area_t ta = {(lv_coord_t)(x0 + xf + 3), (lv_coord_t)(a.y1 + 2), (lv_coord_t)(x0 + xf + 30),
                        (lv_coord_t)(a.y1 + 18)};
        lv_draw_label(ctx, &ld, &ta, tag, nullptr);
    };
    if (v.trig != 0xFFFFFFFF) {
        vline((double)v.trig, C_WARN, "T");
    }
    if (v.cur_a >= 0) {
        vline(v.cur_a, C_CYAN, "A");
    }
    if (v.cur_b >= 0) {
        vline(v.cur_b, C_MAGENTA, "B");
    }

    // trục thời gian (so với điểm kích nếu có)
    const double ref = v.trig != 0xFFFFFFFF ? (double)v.trig : 0;
    ld.color = C_MUTED;
    ld.align = LV_TEXT_ALIGN_LEFT;
    for (int x = 0; x + 60 <= ncols; x += 200) {
        char b[24];
        fmtTime(b, sizeof(b), (v.t0 + x * v.spp - ref) / v.hz);
        lv_area_t ta = {(lv_coord_t)(x0 + x + 2), (lv_coord_t)(ybot + 2), (lv_coord_t)(x0 + x + 196),
                        (lv_coord_t)(ybot + 18)};
        lv_draw_label(ctx, &ld, &ta, b, nullptr);
    }
    char tb[64];
    {
        // dạng gọn: 3 chữ số có nghĩa
        const double sd = 100.0 * v.spp / v.hz;
        const char *u = "s";
        double val = sd;
        if (sd < 1e-6) {
            val = sd * 1e9;
            u = "ns";
        } else if (sd < 1e-3) {
            val = sd * 1e6;
            u = "µs";
        } else if (sd < 1) {
            val = sd * 1e3;
            u = "ms";
        }
        snprintf(tb, sizeof(tb), "%.3g %s/vạch", val, u);
    }
    ld.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t ta = {(lv_coord_t)(a.x1 + 4), (lv_coord_t)(ybot + 2), (lv_coord_t)(x0 - 2), (lv_coord_t)(ybot + 18)};
    ld.font = &lv_font_be_vietnam_12;
    lv_draw_label(ctx, &ld, &ta, tb, nullptr);
}

void zoom(double f)
{
    const double c = v.t0 + waveCols() * v.spp / 2;
    v.spp *= f;
    clampView();
    v.t0 = c - waveCols() * v.spp / 2;
    clampView();
    lv_obj_invalidate(v.wave);
    updateMeasure();
}

void onZoomIn(lv_event_t *)
{
    zoom(0.5);
}
void onZoomOut(lv_event_t *)
{
    zoom(2.0);
}
void onFit(lv_event_t *)
{
    fitAll();
    lv_obj_invalidate(v.wave);
    updateMeasure();
}

void onWaveEvent(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_get_act();
    if (in == nullptr) {
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        v.drag = 0;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_point_t vec;
        lv_indev_get_vect(in, &vec);
        v.drag = (lv_coord_t)(v.drag + abs(vec.x));
        if (vec.x != 0 && v.n) {
            v.t0 -= vec.x * v.spp;
            clampView();
            lv_obj_invalidate(v.wave);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (v.drag >= 10) {
            updateMeasure();
            return;
        }
        // chạm: tên kênh -> chọn kênh đo; vùng sóng -> con trỏ A / B
        lv_point_t p;
        lv_indev_get_point(in, &p);
        lv_area_t a;
        lv_obj_get_coords(v.wave, &a);
        const int rh = rowH();
        if (p.x < a.x1 + LABEL_W) {
            const int k = (p.y - a.y1 - 2) / rh;
            if (k >= 0 && k < 8) {
                v.sel_ch = k;
                lv_obj_invalidate(v.wave);
                updateMeasure();
            }
            return;
        }
        if (v.n == 0) {
            return;
        }
        const double t = v.t0 + (p.x - a.x1 - LABEL_W) * v.spp;
        if (v.next_b) {
            v.cur_b = t;
        } else {
            v.cur_a = t;
        }
        v.next_b = !v.next_b;
        lv_obj_invalidate(v.wave);
        updateCursorText();
    }
}

void updateMeasure(void)
{
    if (v.n == 0 || HCCLogic::data() == nullptr) {
        lv_label_set_text(v.meas, "Chạm tên kênh để đo tần số / duty; chạm vùng sóng đặt con trỏ A / B");
        return;
    }
    const size_t from = (size_t)std::max(0.0, v.t0);
    const size_t to = (size_t)std::min((double)v.n, v.t0 + waveCols() * v.spp);
    if (to <= from + 1) {
        return;
    }
    const HCCLogicCore::Measure m = HCCLogicCore::measure(HCCLogic::data(), from, to, v.sel_ch, v.hz);
    char b[220];
    if (m.valid) {
        char f[24], p[24], h[24], l[24];
        fmtFreq(f, sizeof(f), m.freq_hz);
        fmtTime(p, sizeof(p), m.period_s);
        fmtTime(h, sizeof(h), m.high_min_s);
        fmtTime(l, sizeof(l), m.low_min_s);
        snprintf(b, sizeof(b), "D%d %s (phần đang nhìn): f = %s · T = %s · duty %.1f %% · xung 1 ngắn nhất %s · xung 0 ngắn nhất %s",
                 v.sel_ch, v.names[v.sel_ch], f, p, m.duty * 100, h, l);
    } else if (m.level >= 0) {
        snprintf(b, sizeof(b), "D%d %s (phần đang nhìn): luôn mức %d", v.sel_ch, v.names[v.sel_ch], m.level);
    } else {
        snprintf(b, sizeof(b), "D%d %s (phần đang nhìn): %u cạnh lên, %u cạnh xuống - chưa đủ để đo tần số",
                 v.sel_ch, v.names[v.sel_ch], (unsigned)m.rises, (unsigned)m.falls);
    }
    lv_label_set_text(v.meas, b);
}

void updateCursorText(void)
{
    char b[200], ta[24], tb[24], dt[24], f[24];
    const double ref = v.trig != 0xFFFFFFFF ? (double)v.trig : 0;
    if (v.cur_a < 0) {
        lv_label_set_text(v.cur, "");
        return;
    }
    fmtTime(ta, sizeof(ta), (v.cur_a - ref) / v.hz);
    if (v.cur_b < 0) {
        snprintf(b, sizeof(b), "A = %s   (chạm lần nữa đặt B)", ta);
    } else {
        fmtTime(tb, sizeof(tb), (v.cur_b - ref) / v.hz);
        const double d = fabs(v.cur_b - v.cur_a) / v.hz;
        fmtTime(dt, sizeof(dt), d);
        if (d > 0) {
            fmtFreq(f, sizeof(f), 1.0 / d);
        } else {
            snprintf(f, sizeof(f), "-");
        }
        snprintf(b, sizeof(b), "A = %s   B = %s   B-A = %s   1/(B-A) = %s", ta, tb, dt, f);
    }
    lv_label_set_text(v.cur, b);
}

// ---------- hộp thoại ----------
void closeModal(void)
{
    if (v.modal) {
        lv_obj_del(v.modal);
        v.modal = nullptr;
    }
}

void onCloseModal(lv_event_t *)
{
    closeModal();
}

lv_obj_t *modalCard(int w, int h, const char *title)
{
    closeModal();
    v.modal = lv_obj_create(v.root);
    lv_obj_remove_style_all(v.modal);
    lv_obj_set_size(v.modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(v.modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(v.modal, LV_OPA_60, 0);
    lv_obj_add_flag(v.modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(v.modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *c = lv_obj_create(v.modal);
    lv_obj_set_size(c, w, h);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, std::max(4, (v.H - h) / 2));
    lv_obj_set_style_bg_color(c, C_PANEL, 0);
    lv_obj_set_style_border_color(c, C_CYAN, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = label(c, title, &lv_font_be_vietnam_22, C_CYAN);
    lv_obj_set_pos(t, 0, 0);
    return c;
}

std::string trigText(void)
{
    const Trigger &t = s_set.trig;
    char b[64];
    switch (t.type) {
    case HCCLogicCore::T_NONE: return "kích: NGAY";
    case HCCLogicCore::T_RISE: snprintf(b, sizeof(b), "kích: D%d LÊN", t.ch); break;
    case HCCLogicCore::T_FALL: snprintf(b, sizeof(b), "kích: D%d XUỐNG", t.ch); break;
    case HCCLogicCore::T_EDGE: snprintf(b, sizeof(b), "kích: D%d CẠNH", t.ch); break;
    case HCCLogicCore::T_PATTERN: {
        std::string s = "kích: MẪU ";
        for (int k = 7; k >= 0; --k) {
            s += (t.mask >> k) & 1 ? ((t.value >> k) & 1 ? '1' : '0') : 'X';
        }
        return s;
    }
    }
    return b;
}

// Trigger
void onTrigType(lv_event_t *e)
{
    s_set.trig.type = (HCCLogicCore::TrigType)lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
}

void onTrigCh(lv_event_t *e)
{
    s_set.trig.ch = (uint8_t)lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
}

void patLabel(int k)
{
    const Trigger &t = s_set.trig;
    char b[8];
    snprintf(b, sizeof(b), "D%d\n%c", k, (t.mask >> k) & 1 ? ((t.value >> k) & 1 ? '1' : '0') : 'X');
    lv_label_set_text(lv_obj_get_child(v.pat_btn[k], 0), b);
}

void onPat(lv_event_t *e)
{
    const int k = (int)(uintptr_t)lv_event_get_user_data(e);
    Trigger &t = s_set.trig;
    const uint8_t b = (uint8_t)(1u << k);
    // X -> 0 -> 1 -> X
    if (!(t.mask & b)) {
        t.mask |= b;
        t.value &= (uint8_t)~b;
    } else if (!(t.value & b)) {
        t.value |= b;
    } else {
        t.mask &= (uint8_t)~b;
        t.value &= (uint8_t)~b;
    }
    patLabel(k);
}

lv_obj_t *btnmatrix(lv_obj_t *p, const char **map, int sel, int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *bm = lv_btnmatrix_create(p);
    lv_btnmatrix_set_map(bm, map);
    lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(bm, true);
    lv_btnmatrix_set_btn_ctrl(bm, (uint16_t)sel, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_pos(bm, x, y);
    lv_obj_set_size(bm, w, h);
    lv_obj_set_style_pad_all(bm, 2, 0);
    lv_obj_set_style_pad_gap(bm, 4, 0);
    lv_obj_set_style_bg_color(bm, C_BG, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_text_font(bm, &lv_font_be_vietnam_16, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, C_PANEL2, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, C_TEXT, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, C_BLUE, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return bm;
}

void onTrigOk(lv_event_t *)
{
    closeModal();
}

void onTrigger(lv_event_t *)
{
    lv_obj_t *c = modalCard(820, 360, "TRIGGER - ĐIỀU KIỆN KÍCH");
    static const char *types[] = {"NGAY", "CẠNH LÊN", "CẠNH XUỐNG", "CẠNH BẤT KỲ", "MẪU", ""};
    lv_obj_t *l = label(c, "Kiểu", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(l, 0, 44);
    btnmatrix(c, types, s_set.trig.type, 90, 36, 690, 42, onTrigType);
    static const char *chs[] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", ""};
    l = label(c, "Kênh", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(l, 0, 100);
    btnmatrix(c, chs, s_set.trig.ch, 90, 92, 690, 42, onTrigCh);
    l = label(c, "Mẫu (chạm: X -> 0 -> 1)", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(l, 0, 150);
    for (int k = 7; k >= 0; --k) {
        v.pat_btn[k] = button(c, "", 80, 54, C_PANEL2, onPat, (void *)(uintptr_t)k);
        lv_obj_set_pos(v.pat_btn[k], (7 - k) * 88, 176);
        lv_obj_set_style_text_align(lv_obj_get_child(v.pat_btn[k], 0), LV_TEXT_ALIGN_CENTER, 0);
        patLabel(k);
    }
    l = label(c, "MẪU: kích khi các kênh khớp mẫu (X = bỏ qua). Mức vào 0 / 3,3 V.", &lv_font_be_vietnam_14, C_DIM);
    lv_obj_set_pos(l, 0, 238);
    lv_obj_t *ok = button(c, "XONG", 130, 44, C_BLUE, onTrigOk, nullptr);
    lv_obj_set_pos(ok, 660, 270);
}

// Cài đặt
void onSetRate(lv_event_t *e)
{
    s_set.rate = lv_dropdown_get_selected(lv_event_get_target(e));
}
void onSetDepth(lv_event_t *e)
{
    s_set.depth = lv_dropdown_get_selected(lv_event_get_target(e));
}
void onSetPre(lv_event_t *e)
{
    s_set.pre = lv_dropdown_get_selected(lv_event_get_target(e));
}
void onSetSrc(lv_event_t *e)
{
    s_set.demo = lv_btnmatrix_get_selected_btn(lv_event_get_target(e)) == 1;
}

void onSettings(lv_event_t *)
{
    lv_obj_t *c = modalCard(820, 380, "CÀI ĐẶT THU");
    const char *names[] = {"Tần số lấy mẫu", "Độ sâu (bộ nhớ)", "Trước điểm kích", "Nguồn"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = label(c, names[i], &lv_font_be_vietnam_16, C_MUTED);
        lv_obj_set_pos(l, 0, 50 + i * 56);
    }
    lv_obj_add_event_cb(dropdown(c, RATE_OPTS, s_set.rate, 190, 40, 200), onSetRate, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(dropdown(c, DEPTH_OPTS, s_set.depth, 190, 96, 200), onSetDepth, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(dropdown(c, PRE_OPTS, s_set.pre, 190, 152, 200), onSetPre, LV_EVENT_VALUE_CHANGED, nullptr);
    static const char *src[] = {"THẬT (PARLIO)", "DEMO (mô phỏng)", ""};
    btnmatrix(c, src, s_set.demo ? 1 : 0, 190, 202, 420, 44, onSetSrc);
    std::string pins = "Chân: ";
    for (int k = 0; k < 8; ++k) {
        char b[20];
        const int p = HCCLogic::pin(k);
        snprintf(b, sizeof(b), "D%d=%s%d  ", k, p >= 0 ? "GPIO" : "", p);
        pins += b;
    }
    pins += "\nĐổi chân: menuconfig > HCC LOGIC ANALYZER. Vào 0 / 3,3 V (1,8 V cần chuyển mức). Nối GND chung.\n"
            "40-80 MHz có thể tràn (mất dữ liệu) - với I2C / UART / SPI thường 1-20 MHz là đủ.";
    lv_obj_t *l = label(c, pins.c_str(), &lv_font_be_vietnam_14, C_DIM);
    lv_obj_set_width(l, 780);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, 0, 256);
    lv_obj_t *ok = button(c, "XONG", 130, 44, C_BLUE, onTrigOk, nullptr);
    lv_obj_set_pos(ok, 660, 40);
}

// Giải mã
void startDecode(void)
{
    const HCCLogic::Status st = HCCLogic::status();
    if (st.running || st.samples == 0 || s_job.state == 1) {
        return;
    }
    bool any = false;
    for (int k = 0; k < NDEC; ++k) {
        s_job.cfg[k] = s_set.dec[k];
        any |= s_set.dec[k].proto != HCCLogicDecode::P_NONE;
    }
    v.decode_shown = any;
    if (!any) {
        s_job.state = 0;
        lv_obj_invalidate(v.wave);
        return;
    }
    s_job.samples = st.samples;
    s_job.hz = st.sample_hz;
    s_job.state = 1;
    lv_label_set_text(v.msg, "Đang giải mã...");
    if (xTaskCreatePinnedToCore(decodeTask, "hcc_logic_dec", 6144, nullptr, 4, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_job.state = 0;
    }
}

const char *ROLE_TEXT[5] = {"", "kênh: SCL, SDA", "kênh: RX | baud", "kênh: CLK, MOSI, MISO, CS | mode",
                            "kênh: SCLK, SDATA"};

void refreshDecRow(int s)
{
    const HCCLogicDecode::Config &c = s_set.dec[s];
    static const int nch[5] = {0, 2, 1, 4, 2};
    for (int j = 0; j < 4; ++j) {
        lv_obj_t *dd = v.dec_dd[s][1 + j];
        if (j < nch[c.proto]) {
            lv_obj_clear_flag(dd, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dd, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_t *param = v.dec_dd[s][5];
    if (c.proto == HCCLogicDecode::P_UART) {
        lv_dropdown_set_options(param, BAUD_OPTS);
        lv_dropdown_set_selected(param, (uint16_t)s_set.baud_sel[s]);
        lv_obj_clear_flag(param, LV_OBJ_FLAG_HIDDEN);
    } else if (c.proto == HCCLogicDecode::P_SPI) {
        lv_dropdown_set_options(param, "MODE 0\nMODE 1\nMODE 2\nMODE 3");
        lv_dropdown_set_selected(param, c.spi_mode);
        lv_obj_clear_flag(param, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(param, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(v.dec_role[s], ROLE_TEXT[c.proto]);
}

void onDecDd(lv_event_t *e)
{
    const int id = (int)(uintptr_t)lv_event_get_user_data(e);
    const int s = id / 10, f = id % 10;
    const int sel = lv_dropdown_get_selected(lv_event_get_target(e));
    HCCLogicDecode::Config &c = s_set.dec[s];
    if (f == 0) {
        c.proto = (HCCLogicDecode::Proto)sel;
        refreshDecRow(s);
    } else if (f <= 4) {
        c.ch[f - 1] = (int8_t)(sel == 8 ? -1 : sel);
    } else if (c.proto == HCCLogicDecode::P_UART) {
        s_set.baud_sel[s] = sel;
        c.baud = BAUDS[sel];
    } else if (c.proto == HCCLogicDecode::P_SPI) {
        c.spi_mode = (uint8_t)sel;
    }
}

void onDecApply(lv_event_t *)
{
    closeModal();
    updateNames();
    startDecode();
    lv_obj_invalidate(v.wave);
}

void onDecode(lv_event_t *)
{
    lv_obj_t *c = modalCard(900, 330, "GIẢI MÃ GIAO THỨC");
    const char *CH_OPTS = "D0\nD1\nD2\nD3\nD4\nD5\nD6\nD7\n-";
    for (int s = 0; s < NDEC; ++s) {
        const int y = 46 + s * 100;
        char b[16];
        snprintf(b, sizeof(b), "Bộ %d", s + 1);
        lv_obj_t *l = label(c, b, &lv_font_be_vietnam_16, C_MUTED);
        lv_obj_set_pos(l, 0, y + 10);
        HCCLogicDecode::Config &cfg = s_set.dec[s];
        v.dec_dd[s][0] = dropdown(c, "TẮT\nI2C\nUART\nSPI\nSPMI", cfg.proto, 60, y, 130);
        for (int j = 0; j < 4; ++j) {
            v.dec_dd[s][1 + j] = dropdown(c, CH_OPTS, cfg.ch[j] < 0 ? 8 : cfg.ch[j], 200 + j * 100, y, 92);
        }
        v.dec_dd[s][5] = dropdown(c, BAUD_OPTS, 0, 610, y, 150);
        for (int f = 0; f < 6; ++f) {
            lv_obj_add_event_cb(v.dec_dd[s][f], onDecDd, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)(s * 10 + f));
        }
        v.dec_role[s] = label(c, "", &lv_font_be_vietnam_14, C_DIM);
        lv_obj_set_pos(v.dec_role[s], 200, y + 46);
        refreshDecRow(s);
    }
    lv_obj_t *l = label(c, "Kết quả hiện dưới các kênh; tên kênh đổi theo vai (SCL, SDA, RX...).", &lv_font_be_vietnam_14,
                        C_DIM);
    lv_obj_set_pos(l, 0, 254);
    lv_obj_t *ok = button(c, "ÁP DỤNG", 150, 44, C_BLUE, onDecApply, nullptr);
    lv_obj_set_pos(ok, 720, 244);
}

// File
void onSave(lv_event_t *)
{
    closeModal();
    if (!HCCLogic::startSave(v.names)) {
        lv_label_set_text(v.msg, "Không lưu được (đang thu / chưa có dữ liệu / đang lưu)");
        return;
    }
    v.saving = true;
    lv_label_set_text(v.msg, "Đang lưu thẻ SD...");
}

void onOpenFile(lv_event_t *e)
{
    const uint32_t k = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (k >= s_files.size()) {
        return;
    }
    char path[160], err[200] = "";
    snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/logic/%s", s_files[k].c_str());
    closeModal();
    if (!HCCLogic::startLoad(path, err, sizeof(err))) {
        lv_label_set_text(v.msg, err);
    }
}

void onFile(lv_event_t *)
{
    s_files.clear();
    DIR *d = opendir(BSP_SD_MOUNT_POINT "/logic");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr && s_files.size() < 200) {
            const size_t n = strlen(de->d_name);
            if (n > 4 && strcasecmp(de->d_name + n - 4, ".lab") == 0) {
                s_files.push_back(de->d_name);
            }
        }
        closedir(d);
    }
    std::sort(s_files.begin(), s_files.end(), std::greater<std::string>());
    const int h = std::min<int>(420, v.H - 20);
    lv_obj_t *c = modalCard(640, h, "FILE (/sdcard/logic)");
    lv_obj_t *sv = button(c, "LƯU CAPTURE (.lab + .vcd)", 300, 44, C_BLUE, onSave, nullptr);
    lv_obj_set_pos(sv, 0, 40);
    setEnabled(sv, HCCLogic::status().samples > 0 && !HCCLogic::status().running);
    lv_obj_t *list = lv_list_create(c);
    lv_obj_set_pos(list, 0, 94);
    lv_obj_set_size(list, 610, h - 28 - 94 - 56);
    lv_obj_set_style_bg_color(list, C_BG, 0);
    lv_obj_set_style_border_color(list, C_BORDER, 0);
    if (s_files.empty()) {
        lv_obj_t *l = label(list, "Chưa có file .lab", &lv_font_be_vietnam_16, C_MUTED);
        lv_obj_set_style_pad_all(l, 10, 0);
    }
    for (size_t i = 0; i < s_files.size(); ++i) {
        lv_obj_t *b = lv_list_add_btn(list, nullptr, s_files[i].c_str());
        lv_obj_set_style_bg_color(b, C_PANEL2, 0);
        lv_obj_set_style_text_color(b, strstr(s_files[i].c_str(), "DEMO") ? C_YELLOW : C_TEXT, 0);
        lv_obj_set_style_text_font(b, &lv_font_be_vietnam_18, 0);
        lv_obj_add_event_cb(b, onOpenFile, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    lv_obj_t *cb = button(c, "ĐÓNG", 120, 44, C_BORDER, onCloseModal, nullptr);
    lv_obj_set_pos(cb, 490, h - 28 - 48);
}

// ---------- chạy ----------
HCCLogic::Config makeConfig(void)
{
    HCCLogic::Config c;
    c.sample_hz = RATES[s_set.rate];
    c.depth = DEPTHS[s_set.depth];
    c.pre_pct = PRES[s_set.pre];
    c.trig = s_set.trig;
    return c;
}

void onRun(lv_event_t *)
{
    char err[200] = "";
    const HCCLogic::Config c = makeConfig();
    const bool ok = s_set.demo ? HCCLogic::startDemo(c, err, sizeof(err)) : HCCLogic::start(c, err, sizeof(err));
    if (!ok) {
        lv_label_set_text(v.msg, err);
        return;
    }
    s_job.state = 0;
    v.cur_a = v.cur_b = -1;
    v.next_b = false;
    lv_label_set_text(v.cur, "");
}

void onStop(lv_event_t *)
{
    HCCLogic::stop();
}

void onBack(lv_event_t *e)
{
    HCCLogic::stop();
    if (s_back_cb) {
        lv_event_t ev = *e;
        ev.user_data = s_back_user;
        s_back_cb(&ev);
    }
}

void setBadge(State st, bool demo)
{
    lv_color_t c = C_BORDER;
    switch (st) {
    case State::IDLE: c = C_BORDER; break;
    case State::ARMED: c = lv_color_hex(0x8A6D1F); break;
    case State::CAPTURING: c = C_OK; break;
    case State::DONE: c = lv_color_hex(0x34607A); break;
    case State::OVERFLOW: c = C_WARN; break;
    case State::ERROR:
    case State::NO_PINS: c = C_ERR; break;
    }
    lv_obj_set_style_bg_color(v.badge, c, 0);
    lv_label_set_text(v.badge_lbl, HCCLogic::stateName(st));
    if (demo) {
        lv_obj_clear_flag(v.demo_tag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v.demo_tag, LV_OBJ_FLAG_HIDDEN);
    }
}

void onTimer(lv_timer_t *)
{
    const HCCLogic::Status st = HCCLogic::status();
    setBadge(st.state, st.demo);
    char r[16], info[160];
    fmtRate(r, sizeof(r), st.samples ? st.sample_hz : RATES[s_set.rate]);
    if (st.running) {
        snprintf(info, sizeof(info), "%s · %s · đã nhận %u mẫu · %.1f s", r, trigText().c_str(), (unsigned)st.written,
                 st.elapsed_ms / 1000.0);
    } else {
        char dur[24];
        fmtTime(dur, sizeof(dur), st.sample_hz ? (double)st.samples / st.sample_hz : 0);
        snprintf(info, sizeof(info), "%s · %u mẫu (%s) · %s%s", r, (unsigned)st.samples, dur, trigText().c_str(),
                 s_set.demo ? " · nguồn DEMO" : "");
    }
    lv_label_set_text(v.info, info);
    setEnabled(v.run_btn, !st.running);
    setEnabled(v.stop_btn, st.running);

    if (!st.running && st.serial != v.seen_serial) {
        v.seen_serial = st.serial;
        // vừa thu xong
        v.n = st.samples;
        v.hz = st.sample_hz ? st.sample_hz : 1;
        v.trig = st.trig;
        v.demo = st.demo;
        lv_label_set_text(v.msg, st.msg);
        lv_obj_set_style_text_color(v.msg, st.state == State::OVERFLOW || st.state == State::ERROR ? C_WARN : C_CYAN, 0);
        updateNames();
        fitAll();
        if (v.trig != 0xFFFFFFFF && v.n > 0) {
            // quanh điểm kích
            v.spp = std::max(1.0 / 32, std::min(maxSpp(), (double)v.n / 20 / waveCols()));
            v.t0 = (double)v.trig - waveCols() * v.spp * 0.2;
            clampView();
        }
        startDecode();
        lv_obj_invalidate(v.wave);
        updateMeasure();
    } else if (st.running) {
        lv_label_set_text(v.msg, st.msg);
    }

    if (v.saving && !HCCLogic::saving()) {
        v.saving = false;
        char m[200];
        const bool ok = HCCLogic::saveResult(m, sizeof(m));
        lv_label_set_text(v.msg, m);
        lv_obj_set_style_text_color(v.msg, ok ? C_CYAN : C_ERR, 0);
    }
}

// Giải mã xong: vẽ lại + báo số lỗi (timer riêng nhẹ).
void onDecodePoll(lv_timer_t *)
{
    static int last = 0;
    if (s_job.state == 2 && last != 2) {
        uint32_t ann = 0, err = 0;
        std::string extra;
        for (int k = 0; k < NDEC; ++k) {
            ann += (uint32_t)s_job.res[k].ann.size();
            err += s_job.res[k].errors;
            if (s_job.cfg[k].proto == HCCLogicDecode::P_UART && s_job.res[k].baud_used) {
                char b[48];
                snprintf(b, sizeof(b), " · UART %u baud%s", (unsigned)s_job.res[k].baud_used,
                         s_job.cfg[k].baud ? "" : " (tự đoán)");
                extra += b;
            }
        }
        char m[200];
        snprintf(m, sizeof(m), "Giải mã xong: %u chú thích, %u lỗi%s%s", (unsigned)ann, (unsigned)err, extra.c_str(),
                 v.demo ? " - DEMO" : "");
        lv_label_set_text(v.msg, m);
        lv_obj_invalidate(v.wave);
    }
    last = s_job.state;
}

lv_timer_t *s_dec_timer = nullptr;

void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
    }
    if (s_dec_timer) {
        lv_timer_del(s_dec_timer);
        s_dec_timer = nullptr;
    }
    v = View();
    s_files.clear();
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
    const lv_coord_t H = HCCLayout::usableHeight(root, 503);
    v.H = H;

    lv_obj_t *title = label(root, "LOGIC ANALYZER", &lv_font_be_vietnam_24, C_TEXT);
    lv_obj_set_pos(title, 12, 6);
    v.badge = lv_obj_create(root);
    lv_obj_remove_style_all(v.badge);
    lv_obj_set_size(v.badge, 170, 30);
    lv_obj_set_pos(v.badge, 240, 6);
    lv_obj_set_style_radius(v.badge, 15, 0);
    lv_obj_set_style_bg_opa(v.badge, LV_OPA_COVER, 0);
    v.badge_lbl = label(v.badge, "READY", &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_center(v.badge_lbl);
    v.demo_tag = lv_obj_create(root);
    lv_obj_remove_style_all(v.demo_tag);
    lv_obj_set_size(v.demo_tag, 70, 30);
    lv_obj_set_pos(v.demo_tag, 416, 6);
    lv_obj_set_style_radius(v.demo_tag, 15, 0);
    lv_obj_set_style_bg_opa(v.demo_tag, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(v.demo_tag, C_YELLOW, 0);
    lv_obj_center(label(v.demo_tag, "DEMO", &lv_font_be_vietnam_16, C_BG));
    lv_obj_add_flag(v.demo_tag, LV_OBJ_FLAG_HIDDEN);
    v.info = label(root, "", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(v.info, 494, 11);
    lv_obj_set_width(v.info, 522);
    lv_label_set_long_mode(v.info, LV_LABEL_LONG_DOT);
    v.msg = label(root, "", &lv_font_be_vietnam_14, C_CYAN);
    lv_obj_set_pos(v.msg, 14, 40);
    lv_obj_set_width(v.msg, 1000);
    lv_label_set_long_mode(v.msg, LV_LABEL_LONG_DOT);
    if (const char *pe = HCCLogic::pinError()) {
        lv_label_set_text(v.msg, pe);
        lv_obj_set_style_text_color(v.msg, C_WARN, 0);
    } else {
        lv_label_set_text(v.msg, "Chỉ nghe: các chân là đầu vào, không kéo lên / xuống. Mức 0 / 3,3 V, nối GND chung.");
    }

    const int by = H - 50;
    const int wy = 60, wh = by - 52 - wy;
    v.wave = lv_obj_create(root);
    lv_obj_remove_style_all(v.wave);
    lv_obj_set_pos(v.wave, 8, wy);
    lv_obj_set_size(v.wave, 1008, wh);
    lv_obj_set_style_bg_color(v.wave, lv_color_hex(0x0A1117), 0);
    lv_obj_set_style_bg_opa(v.wave, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(v.wave, C_BORDER, 0);
    lv_obj_set_style_border_width(v.wave, 1, 0);
    lv_obj_add_flag(v.wave, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(v.wave, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(v.wave, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v.wave, drawWave, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(v.wave, onWaveEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(v.wave, onWaveEvent, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(v.wave, onWaveEvent, LV_EVENT_RELEASED, nullptr);

    v.meas = label(root, "", &lv_font_be_vietnam_14, C_TEXT);
    lv_obj_set_pos(v.meas, 12, wy + wh + 4);
    lv_obj_set_width(v.meas, 1000);
    lv_label_set_long_mode(v.meas, LV_LABEL_LONG_DOT);
    v.cur = label(root, "", &lv_font_be_vietnam_14, C_CYAN);
    lv_obj_set_pos(v.cur, 12, wy + wh + 26);
    lv_obj_set_width(v.cur, 1000);
    lv_label_set_long_mode(v.cur, LV_LABEL_LONG_DOT);

    int x = 8;
    auto place = [&](lv_obj_t *b, int w) {
        lv_obj_set_pos(b, x, by);
        x += w + 6;
    };
    place(button(root, LV_SYMBOL_LEFT " BACK", 96, 44, C_BORDER, onBack, nullptr), 96);
    v.run_btn = button(root, "RUN", 96, 44, C_OK, onRun, nullptr);
    place(v.run_btn, 96);
    v.stop_btn = button(root, "STOP", 84, 44, C_ERR, onStop, nullptr);
    place(v.stop_btn, 84);
    place(button(root, "-", 52, 44, C_PANEL2, onZoomOut, nullptr), 52);
    place(button(root, "+", 52, 44, C_PANEL2, onZoomIn, nullptr), 52);
    place(button(root, "VỪA", 70, 44, C_PANEL2, onFit, nullptr), 70);
    place(button(root, "TRIGGER", 112, 44, lv_color_hex(0xB9770E), onTrigger, nullptr), 112);
    place(button(root, "CÀI ĐẶT", 112, 44, C_BLUE, onSettings, nullptr), 112);
    place(button(root, "GIẢI MÃ", 112, 44, C_PURPLE, onDecode, nullptr), 112);
    place(button(root, "FILE", 1016 - x, 44, lv_color_hex(0x16A085), onFile, nullptr), 1016 - x);

    // Dữ liệu cũ (nếu có) hiện lại khi mở màn.
    lv_obj_update_layout(root);
    const HCCLogic::Status st = HCCLogic::status();
    v.seen_serial = st.serial;
    if (!st.running && st.samples > 0) {
        v.n = st.samples;
        v.hz = st.sample_hz ? st.sample_hz : 1;
        v.trig = st.trig;
        v.demo = st.demo;
        fitAll();
        v.decode_shown = s_job.state == 2;
    }
    updateNames();
    updateMeasure();
    onTimer(nullptr);
    v.timer = lv_timer_create(onTimer, 100, nullptr);
    s_dec_timer = lv_timer_create(onDecodePoll, 150, nullptr);
}

}  // namespace HCCLogicView
