#include "HCCSpmiMonitorView.hpp"

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <vector>

#include "HCCRffe.hpp"
#include "HCCSpmiAnalyzerView.hpp"
#include "HCCSpmiLink.hpp"
#include "HCCSpmiMonitor.hpp"
#include "bsp/esp-bsp.h"

namespace HCCSpmiMonitorView {

using HCCSpmiDecode::Txn;
using HCCSpmiMonitor::Snapshot;
using HCCSpmiMonitor::Src;
using HCCSpmiMonitor::State;

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

constexpr int PAGE = 50;
constexpr int WAVE_LEFT = 62;  // chỗ cho nhãn SCLK / SDATA
constexpr double SPP_MIN = 1.0 / 16;
constexpr double SPP_MAX = 4096;

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *badge = nullptr, *badge_lbl = nullptr, *demo_tag = nullptr, *info = nullptr, *msg = nullptr;
    lv_obj_t *wave = nullptr;
    lv_obj_t *sid_dd = nullptr, *type_bm = nullptr, *page_lbl = nullptr, *follow_btn = nullptr;
    lv_obj_t *table = nullptr, *hdr = nullptr;
    lv_obj_t *src_dd = nullptr, *rate_dd = nullptr;
    lv_obj_t *start_btn = nullptr, *stop_btn = nullptr, *clear_btn = nullptr, *save_btn = nullptr, *open_btn = nullptr;
    lv_obj_t *modal = nullptr;
    lv_timer_t *timer = nullptr;
    lv_coord_t H = 527;

    int f_sid = -1;   // -1 = tất cả
    int f_type = 0;   // 0 tất cả, 1 READ, 2 WRITE, 3 LỖI
    std::vector<uint32_t> idx;
    uint32_t scanned = 0;
    uint32_t page = 0;
    bool follow = true;
    uint32_t shown_page = UINT32_MAX, shown_rows = UINT32_MAX, shown_total = UINT32_MAX;
    int32_t sel = -1;  // gói đang chọn (chỉ số trong PacketBuf)

    double t0 = 0;     // mẫu ở mép trái vùng sóng
    double spp = 0.5;  // mẫu / điểm ảnh
    uint32_t hz = 80000000;
    bool swapped = false;
    bool demo = false;
    uint32_t wave_seen = UINT32_MAX;
    bool saving = false;
    State last_state = State::IDLE;
};

View v;
lv_obj_t *s_screen = nullptr;
lv_event_cb_t s_back_cb = nullptr;
void *s_back_user = nullptr;
Src s_src_sel = Src::PARLIO;
int s_rate_sel = 2;  // 0: 20 MHz, 1: 40 MHz, 2: 80 MHz
uint8_t s_cols[1024];

// ==================== tiện ích ====================

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

void setEnabled(lv_obj_t *o, bool en)
{
    if (en) {
        lv_obj_clear_state(o, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(o, LV_STATE_DISABLED);
    }
}

double usOf(uint64_t t)
{
    return (double)t * 1e6 / (double)(v.hz ? v.hz : 1);
}

void fmtTime(char *b, size_t n, double us)
{
    if (us < 1000.0) {
        snprintf(b, n, "%.3f µs", us);
    } else if (us < 1e6) {
        snprintf(b, n, "%.4f ms", us / 1000.0);
    } else {
        snprintf(b, n, "%.6f s", us / 1e6);
    }
}

void fmtReg(char *b, size_t n, const Txn &t)
{
    using namespace HCCSpmiDecode;
    switch (t.kind) {
    case K_REG_WRITE:
    case K_REG_READ:
    case K_EXT_WRITE:
    case K_EXT_READ: snprintf(b, n, "0x%02X", t.addr); break;
    case K_EXTL_WRITE:
    case K_EXTL_READ: snprintf(b, n, "0x%04X", t.addr); break;
    case K_REG0_WRITE: snprintf(b, n, "0x00"); break;
    default: snprintf(b, n, "-"); break;
    }
}

void fmtData(char *b, size_t n, const Txn &t)
{
    int k = 0;
    b[0] = '\0';
    for (int j = 0; j < t.len && j < 8 && k < (int)n - 4; ++j) {
        k += snprintf(b + k, n - k, "%s%02X", j ? " " : "", t.data[j]);
    }
    if (t.len == 0) {
        snprintf(b, n, "-");
    }
}

bool isErr(const Txn &t)
{
    const HCCSpmiDecode::PktStatus s = HCCSpmiDecode::status(t);
    return s != HCCSpmiDecode::ST_OK && s != HCCSpmiDecode::ST_UNKNOWN_CMD;
}

bool match(const Txn &t)
{
    if (v.f_sid >= 0 && (t.kind == HCCSpmiDecode::K_BAD || t.sid != v.f_sid)) {
        return false;
    }
    switch (v.f_type) {
    case 1: return HCCSpmiDecode::isRead(t.kind);
    case 2: return HCCSpmiDecode::isWrite(t.kind);
    case 3: return isErr(t) || (t.flags & HCCSpmiDecode::F_LOST_BEFORE);
    default: return true;
    }
}

void resetFilter(void)
{
    v.idx.clear();
    v.scanned = 0;
    v.page = 0;
    v.shown_page = UINT32_MAX;
}

// ==================== dạng sóng ====================

int waveCols(void)
{
    const lv_coord_t w = lv_obj_get_width(v.wave);
    return std::max(16, std::min(1000, (int)w - WAVE_LEFT - 8));
}

// Bit lấy mẫu tại cạnh xuống SCLK của gói t (đọc lại từ sóng thô), tô màu theo vai trò trong khung:
// trắng = bit dữ liệu, xanh / đỏ = parity đúng / sai, xám = bus park / bit dư. Trả false nếu bit quá sát nhau.
bool drawBits(lv_draw_ctx_t *ctx, int x0, int ncols, int ybit, const Txn &t)
{
    static uint8_t col[4096];
    const HCCSpmiBuffers::RawStore &raw = HCCSpmiMonitor::raw();
    const int n = (int)std::min<uint32_t>(t.dur + 8, sizeof(col));
    raw.columns((double)t.t_sample, 1.0, n, col);
    const uint8_t clk_hi = v.swapped ? HCCSpmiBuffers::COL_L1_HIGH : HCCSpmiBuffers::COL_L0_HIGH;
    const uint8_t dat_hi = v.swapped ? HCCSpmiBuffers::COL_L0_HIGH : HCCSpmiBuffers::COL_L1_HIGH;
    // Vai trò từng bit theo bảng lệnh: 'd' dữ liệu, 'p' parity (kèm số khung), 'b' bus park.
    char role[160];
    uint8_t frame[160];
    int nr = 0, f = 0;
    auto add = [&](char r, int cnt) {
        for (int i = 0; i < cnt && nr < (int)sizeof(role); ++i) {
            role[nr] = r;
            frame[nr++] = (uint8_t)f;
        }
    };
    add('d', 12);
    add('p', 1);
    ++f;
    if (t.kind != HCCSpmiDecode::K_BAD && t.kind != HCCSpmiDecode::K_OTHER && t.kind != HCCSpmiDecode::K_REG0_WRITE) {
        for (int i = 0; i < HCCSpmiDecode::addrFrames(t.kind); ++i, ++f) {
            add('d', 8);
            add('p', 1);
        }
        if (HCCSpmiDecode::isRead(t.kind)) {
            add('b', 1);
        }
        int nd = t.len;
        if (t.short_bits || nd == 0) {
            nd = HCCSpmiDecode::isRead(t.kind) || HCCSpmiDecode::isWrite(t.kind) ? 8 : 0;  // đến hết bit thu được
        }
        for (int i = 0; i < nd; ++i, ++f) {
            add('d', 8);
            add('p', 1);
        }
    }
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_be_vietnam_14;
    ld.align = LV_TEXT_ALIGN_CENTER;
    int prev = -1, k = 0;
    int64_t last_edge = -1;
    for (int s2 = 0; s2 < n; ++s2) {
        if (col[s2] & HCCSpmiBuffers::COL_NODATA) {
            break;
        }
        const int c = (col[s2] & clk_hi) ? 1 : 0;
        if (prev == 1 && c == 0 && s2 > 0) {
            if (last_edge >= 0 && (s2 - last_edge) / v.spp < 10) {
                return false;  // chưa đủ phóng to để ghi từng bit
            }
            last_edge = s2;
            const int bit = (col[s2 - 1] & dat_hi) ? 1 : 0;
            const double xf = ((double)t.t_sample + s2 - v.t0) / v.spp;
            if (k >= (int)t.nbits) {
                break;
            }
            if (xf >= 0 && xf <= ncols) {
                const char r = k < nr ? role[k] : 'b';
                if (r == 'p') {
                    ld.color = ((t.frame_err >> frame[k]) & 1) ? C_ERR : C_OK;
                } else if (r == 'b') {
                    ld.color = C_DIM;
                } else {
                    ld.color = C_TEXT;
                }
                const char txt[2] = {(char)('0' + bit), 0};
                const int x = x0 + (int)xf;
                lv_area_t ta = {(lv_coord_t)(x - 6), (lv_coord_t)ybit, (lv_coord_t)(x + 6), (lv_coord_t)(ybit + 16)};
                lv_draw_label(ctx, &ld, &ta, txt, nullptr);
            }
            ++k;
        }
        prev = c;
    }
    return true;
}

void drawWave(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int x0 = a.x1 + WAVE_LEFT;
    const int ncols = waveCols();
    const HCCSpmiBuffers::RawStore &raw = HCCSpmiMonitor::raw();
    const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();

    // Lưới dọc mỗi 100 điểm ảnh.
    lv_draw_line_dsc_t gl;
    lv_draw_line_dsc_init(&gl);
    gl.color = C_BORDER;
    gl.width = 1;
    for (int x = 0; x <= ncols; x += 100) {
        lv_point_t p1 = {(lv_coord_t)(x0 + x), (lv_coord_t)(a.y1 + 20)};
        lv_point_t p2 = {(lv_coord_t)(x0 + x), (lv_coord_t)(a.y1 + 76)};
        lv_draw_line(ctx, &gl, &p1, &p2);
    }

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_be_vietnam_14;
    ld.color = C_MUTED;

    if (!raw.enabled() || raw.segCount() == 0) {
        lv_area_t ta = {(lv_coord_t)x0, (lv_coord_t)(a.y1 + 40), (lv_coord_t)(x0 + ncols), (lv_coord_t)(a.y1 + 60)};
        ld.align = LV_TEXT_ALIGN_CENTER;
        lv_draw_label(ctx, &ld, &ta,
                      pk.count() ? "Không có sóng thô (file CSV / hết bộ nhớ) - chỉ có bảng gói"
                                 : "Chưa có dữ liệu - bấm START hoặc OPEN",
                      nullptr);
        return;
    }

    // Vùng gói đang chọn.
    if (v.sel >= 0 && (uint32_t)v.sel < pk.count()) {
        const Txn &t = pk.at(v.sel);
        const double xa = ((double)t.t_sample - v.t0) / v.spp;
        const double xb = ((double)t.t_sample + t.dur + 2 - v.t0) / v.spp;
        if (xb >= 0 && xa <= ncols) {
            lv_draw_rect_dsc_t rd;
            lv_draw_rect_dsc_init(&rd);
            rd.bg_color = C_BLUE;
            rd.bg_opa = LV_OPA_20;
            rd.radius = 0;
            lv_area_t ra = {(lv_coord_t)(x0 + std::max(0.0, xa)), (lv_coord_t)(a.y1 + 20),
                            (lv_coord_t)(x0 + std::min((double)ncols, xb)), (lv_coord_t)(a.y1 + 76)};
            lv_draw_rect(ctx, &rd, &ra);
        }
    }

    raw.columns(v.t0, v.spp, ncols, s_cols);
    const int ys[2][2] = {{a.y1 + 24, a.y1 + 44}, {a.y1 + 54, a.y1 + 74}};  // {cao, thấp} của SCLK, SDATA
    for (int line = 0; line < 2; ++line) {
        // line 0 = SCLK, 1 = SDATA; dây vật lý tuỳ vai đã nhận.
        const int phys = v.swapped ? 1 - line : line;
        const uint8_t lo = phys ? HCCSpmiBuffers::COL_L1_LOW : HCCSpmiBuffers::COL_L0_LOW;
        const uint8_t hi = phys ? HCCSpmiBuffers::COL_L1_HIGH : HCCSpmiBuffers::COL_L0_HIGH;
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = line == 0 ? C_CYAN : C_YELLOW;
        d.width = 2;
        lv_draw_line_dsc_t nd;
        lv_draw_line_dsc_init(&nd);
        nd.color = C_DIM;
        nd.width = 1;
        nd.dash_width = 3;
        nd.dash_gap = 3;
        const int yh = ys[line][0], yl = ys[line][1];
        int c = 0;
        int prev = -1;  // mức cột trước: 0 / 1 / 2 (cả hai) / 3 (không dữ liệu)
        while (c < ncols) {
            const uint8_t m = s_cols[c];
            int lvl;
            if (m & HCCSpmiBuffers::COL_NODATA) {
                lvl = 3;
            } else if ((m & lo) && (m & hi)) {
                lvl = 2;
            } else {
                lvl = (m & hi) ? 1 : 0;
            }
            if (lvl == 2) {
                lv_point_t p1 = {(lv_coord_t)(x0 + c), (lv_coord_t)yh};
                lv_point_t p2 = {(lv_coord_t)(x0 + c), (lv_coord_t)yl};
                lv_draw_line(ctx, &d, &p1, &p2);
                prev = 2;
                ++c;
                continue;
            }
            int e2 = c + 1;
            while (e2 < ncols) {
                const uint8_t m2 = s_cols[e2];
                int l2;
                if (m2 & HCCSpmiBuffers::COL_NODATA) {
                    l2 = 3;
                } else if ((m2 & lo) && (m2 & hi)) {
                    l2 = 2;
                } else {
                    l2 = (m2 & hi) ? 1 : 0;
                }
                if (l2 != lvl) {
                    break;
                }
                ++e2;
            }
            if (lvl == 3) {
                lv_point_t p1 = {(lv_coord_t)(x0 + c), (lv_coord_t)((yh + yl) / 2)};
                lv_point_t p2 = {(lv_coord_t)(x0 + e2 - 1), (lv_coord_t)((yh + yl) / 2)};
                lv_draw_line(ctx, &nd, &p1, &p2);
            } else {
                const int y = lvl ? yh : yl;
                if (prev == 0 || prev == 1) {
                    if (prev != lvl) {  // cạnh giữa hai đoạn mức
                        lv_point_t p1 = {(lv_coord_t)(x0 + c), (lv_coord_t)yh};
                        lv_point_t p2 = {(lv_coord_t)(x0 + c), (lv_coord_t)yl};
                        lv_draw_line(ctx, &d, &p1, &p2);
                    }
                }
                lv_point_t p1 = {(lv_coord_t)(x0 + c), (lv_coord_t)y};
                lv_point_t p2 = {(lv_coord_t)(x0 + e2), (lv_coord_t)y};
                lv_draw_line(ctx, &d, &p1, &p2);
            }
            prev = lvl;
            c = e2;
        }
    }

    // Dấu gói (SSC) + nhãn.
    const double tend = v.t0 + ncols * v.spp;
    uint32_t i = pk.lowerBound(v.t0 < 0 ? 0 : (uint64_t)v.t0);
    if (i > 0) {
        --i;
    }
    int last_lbl = -1000;
    int drawn = 0;
    lv_draw_line_dsc_t md;
    lv_draw_line_dsc_init(&md);
    md.width = 1;
    md.dash_width = 2;
    md.dash_gap = 2;
    int bits_drawn = 0;
    for (; i < pk.count() && drawn < 200; ++i) {
        const Txn &t = pk.at(i);
        if ((double)t.t_sample > tend) {
            break;
        }
        const double xs = ((double)t.t_sample - v.t0) / v.spp;
        const double xe = ((double)t.t_sample + t.dur - v.t0) / v.spp;
        if (xe >= 0 && xs <= ncols && bits_drawn < 6 && v.spp <= 2) {
            bits_drawn += drawBits(ctx, x0, ncols, a.y1 + 78, t) ? 1 : 6;
        }
        if (xs < 0 || xs > ncols) {
            continue;
        }
        ++drawn;
        const int x = x0 + (int)xs;
        md.color = isErr(t) ? C_ERR : C_OK;
        lv_point_t p1 = {(lv_coord_t)x, (lv_coord_t)(a.y1 + 18)};
        lv_point_t p2 = {(lv_coord_t)x, (lv_coord_t)(a.y1 + 76)};
        lv_draw_line(ctx, &md, &p1, &p2);
        if (x - last_lbl > 230) {
            char b[48];
            if (t.kind == HCCSpmiDecode::K_BAD) {
                snprintf(b, sizeof(b), "#%u LỖI LỆNH", (unsigned)(i + 1));
            } else {
                char r[12], d[28];
                fmtReg(r, sizeof(r), t);
                fmtData(d, sizeof(d), t);
                snprintf(b, sizeof(b), "#%u S%X %s %s=%s", (unsigned)(i + 1), t.sid, HCCSpmiDecode::kindName(t.kind),
                         r, d);
            }
            ld.color = isErr(t) ? C_ERR : C_TEXT;
            ld.align = LV_TEXT_ALIGN_LEFT;
            lv_area_t ta = {(lv_coord_t)(x + 3), (lv_coord_t)(a.y1 + 2), (lv_coord_t)(x + 226),
                            (lv_coord_t)(a.y1 + 18)};
            lv_draw_label(ctx, &ld, &ta, b, nullptr);
            last_lbl = x;
        }
    }

    // Trục thời gian.
    char b1[32], b2[32], tb[96];
    fmtTime(b1, sizeof(b1), usOf(v.t0 < 0 ? 0 : (uint64_t)v.t0));
    fmtTime(b2, sizeof(b2), usOf(1) * v.spp * 100);
    snprintf(tb, sizeof(tb), "t = %s     %s / vạch", b1, b2);
    ld.color = C_MUTED;
    ld.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t ta = {(lv_coord_t)x0, (lv_coord_t)(a.y1 + 100), (lv_coord_t)(x0 + ncols), (lv_coord_t)(a.y1 + 118)};
    lv_draw_label(ctx, &ld, &ta, tb, nullptr);
    if (raw.truncated()) {
        ld.color = C_WARN;
        ld.align = LV_TEXT_ALIGN_RIGHT;
        lv_draw_label(ctx, &ld, &ta, "bộ nhớ sóng đầy - phần sau chỉ có gói", nullptr);
    }
}

void onWavePress(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_get_act();
    if (in == nullptr) {
        return;
    }
    lv_point_t vec;
    lv_indev_get_vect(in, &vec);
    if (vec.x == 0) {
        return;
    }
    v.t0 -= vec.x * v.spp;
    if (v.t0 < -50 * v.spp) {
        v.t0 = -50 * v.spp;
    }
    if (v.follow) {
        v.follow = false;
        lv_obj_clear_state(v.follow_btn, LV_STATE_CHECKED);
    }
    lv_obj_invalidate(lv_event_get_target(e));
}

void zoomTo(double spp, double center_t)
{
    spp = std::max(SPP_MIN, std::min(SPP_MAX, spp));
    const int n = waveCols();
    v.spp = spp;
    v.t0 = center_t - n * spp / 2;
    lv_obj_invalidate(v.wave);
}

void onZoomIn(lv_event_t *)
{
    zoomTo(v.spp / 2, v.t0 + waveCols() * v.spp / 2);
}

void onZoomOut(lv_event_t *)
{
    zoomTo(v.spp * 2, v.t0 + waveCols() * v.spp / 2);
}

void showPacketOnWave(uint32_t i, bool fit)
{
    const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();
    if (i >= pk.count()) {
        return;
    }
    const Txn &t = pk.at(i);
    const int n = waveCols();
    if (fit) {
        const double span = (double)t.dur + 40;
        v.spp = std::max(SPP_MIN, std::min(SPP_MAX, span / (n * 0.8)));
    }
    v.t0 = (double)t.t_sample - n * v.spp * 0.1;
    lv_obj_invalidate(v.wave);
}

void onFit(lv_event_t *)
{
    const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();
    if (v.sel >= 0) {
        showPacketOnWave((uint32_t)v.sel, true);
    } else if (!v.idx.empty()) {
        const uint32_t k = std::min<uint32_t>(v.page * PAGE, (uint32_t)v.idx.size() - 1);
        showPacketOnWave(v.idx[k], true);
    } else if (pk.count()) {
        showPacketOnWave(0, true);
    }
}

void onHome(lv_event_t *)
{
    const uint64_t t = HCCSpmiMonitor::raw().nextActivity(0);
    v.t0 = t == UINT64_MAX ? 0 : (double)t - 20 * v.spp;
    v.follow = false;
    lv_obj_clear_state(v.follow_btn, LV_STATE_CHECKED);
    lv_obj_invalidate(v.wave);
}

// ==================== bảng gói ====================

void fillTable(bool force)
{
    const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();
    const uint32_t total = (uint32_t)v.idx.size();
    const uint32_t pages = total ? (total + PAGE - 1) / PAGE : 1;
    if (v.follow) {
        v.page = pages - 1;
    }
    if (v.page >= pages) {
        v.page = pages - 1;
    }
    const uint32_t first = v.page * PAGE;
    const uint32_t rows = total > first ? std::min<uint32_t>(PAGE, total - first) : 0;
    char pl[48];
    if (total) {
        snprintf(pl, sizeof(pl), "%u-%u / %u", (unsigned)(first + 1), (unsigned)(first + rows), (unsigned)total);
    } else {
        snprintf(pl, sizeof(pl), "0 / 0");
    }
    lv_label_set_text(v.page_lbl, pl);
    if (!force && v.shown_page == v.page && v.shown_rows == rows && v.shown_total == pk.count()) {
        return;
    }
    const bool same_page = v.shown_page == v.page && !force;
    const uint32_t from = same_page && v.shown_rows != UINT32_MAX ? v.shown_rows : 0;
    v.shown_page = v.page;
    v.shown_rows = rows;
    v.shown_total = pk.count();
    lv_table_set_row_cnt(v.table, rows ? rows : 1);
    if (rows == 0) {
        for (int c = 0; c < 6; ++c) {
            lv_table_set_cell_value(v.table, 0, c, "");
        }
    }
    for (uint32_t r = from; r < rows; ++r) {
        const Txn &t = pk.at(v.idx[first + r]);
        char b[48];
        const uint16_t row = (uint16_t)r;
        fmtTime(b, sizeof(b), usOf(t.t_sample));
        lv_table_set_cell_value(v.table, row, 0, b);
        if (t.kind == HCCSpmiDecode::K_BAD) {
            lv_table_set_cell_value(v.table, row, 1, "?");
        } else {
            snprintf(b, sizeof(b), "0x%X", t.sid);
            lv_table_set_cell_value(v.table, row, 1, b);
        }
        snprintf(b, sizeof(b), "%s  %02X", HCCSpmiDecode::kindName(t.kind), t.cmd);
        lv_table_set_cell_value(v.table, row, 2, b);
        fmtReg(b, sizeof(b), t);
        lv_table_set_cell_value(v.table, row, 3, b);
        fmtData(b, sizeof(b), t);
        lv_table_set_cell_value(v.table, row, 4, b);
        snprintf(b, sizeof(b), "%s%s", (t.flags & HCCSpmiDecode::F_LOST_BEFORE) ? "! " : "",
                 HCCSpmiDecode::statusName(HCCSpmiDecode::status(t)));
        lv_table_set_cell_value(v.table, row, 5, b);
    }
    if (v.follow && rows > 0) {
        lv_obj_scroll_to_y(v.table, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void scanNew(void)
{
    const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();
    const uint32_t n = pk.count();
    if (n < v.scanned) {
        resetFilter();
    }
    for (uint32_t i = v.scanned; i < n; ++i) {
        if (match(pk.at(i))) {
            v.idx.push_back(i);
        }
    }
    v.scanned = n;
}

void openDetail(uint32_t i);

void onTableEvent(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *t = lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        uint16_t row = 0, col = 0;
        lv_table_get_selected_cell(t, &row, &col);
        if (row == LV_TABLE_CELL_NONE) {
            return;
        }
        const uint32_t k = v.page * PAGE + row;
        if (k >= v.idx.size()) {
            return;
        }
        v.sel = (int32_t)v.idx[k];
        if (v.follow) {  // đang xem gói cụ thể: thôi bám gói mới
            v.follow = false;
            lv_obj_clear_state(v.follow_btn, LV_STATE_CHECKED);
        }
        lv_obj_invalidate(t);
        showPacketOnWave((uint32_t)v.sel, false);
        openDetail((uint32_t)v.sel);
        return;
    }
    if (code == LV_EVENT_DRAW_PART_BEGIN) {
        lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
        if (d->part != LV_PART_ITEMS) {
            return;
        }
        const uint32_t row = d->id / 6;
        const uint32_t k = v.page * PAGE + row;
        if (k >= v.idx.size()) {
            return;
        }
        const Txn &x = HCCSpmiMonitor::packets().at(v.idx[k]);
        if ((int32_t)v.idx[k] == v.sel) {
            d->rect_dsc->bg_color = lv_color_hex(0x1B4A66);
            d->rect_dsc->bg_opa = LV_OPA_COVER;
        }
        if (isErr(x)) {
            d->label_dsc->color = C_ERR;
        } else if ((x.flags & HCCSpmiDecode::F_LOST_BEFORE) || x.kind == HCCSpmiDecode::K_OTHER) {
            d->label_dsc->color = C_WARN;
        } else if (HCCSpmiDecode::isWrite(x.kind)) {
            d->label_dsc->color = C_TEXT;
        } else {
            d->label_dsc->color = C_MUTED;
        }
    }
}

void onFilterSid(lv_event_t *e)
{
    const uint16_t s = lv_dropdown_get_selected(lv_event_get_target(e));
    v.f_sid = s == 0 ? -1 : (int)s - 1;
    resetFilter();
    scanNew();
    fillTable(true);
}

void onFilterType(lv_event_t *e)
{
    v.f_type = (int)lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    resetFilter();
    scanNew();
    fillTable(true);
}

void onPrev(lv_event_t *)
{
    if (v.page > 0) {
        --v.page;
        v.follow = false;
        lv_obj_clear_state(v.follow_btn, LV_STATE_CHECKED);
        fillTable(true);
        lv_obj_scroll_to_y(v.table, 0, LV_ANIM_OFF);
    }
}

void onNext(lv_event_t *)
{
    const uint32_t pages = v.idx.empty() ? 1 : ((uint32_t)v.idx.size() + PAGE - 1) / PAGE;
    if (v.page + 1 < pages) {
        ++v.page;
        fillTable(true);
        lv_obj_scroll_to_y(v.table, 0, LV_ANIM_OFF);
    }
}

void onFollow(lv_event_t *e)
{
    v.follow = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    fillTable(true);
}

// ==================== hộp thoại ====================

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
    lv_obj_center(c);
    lv_obj_set_style_bg_color(c, C_PANEL, 0);
    lv_obj_set_style_border_color(c, C_CYAN, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_t *t = label(c, title, &lv_font_be_vietnam_22, C_CYAN);
    lv_obj_set_pos(t, 0, 0);
    return c;
}

void onDetailWave(lv_event_t *)
{
    closeModal();
    if (v.sel >= 0) {
        showPacketOnWave((uint32_t)v.sel, true);
    }
}

void onLink(lv_event_t *e)
{
    const HCCSpmiLink::Target tg = (HCCSpmiLink::Target)(uintptr_t)lv_event_get_user_data(e);
    const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();
    if (v.sel < 0 || (uint32_t)v.sel >= pk.count()) {
        return;
    }
    const Txn &t = pk.at(v.sel);
    HCCSpmiLink::Ref r = {t.sid, t.addr, HCCSpmiDecode::isRead(t.kind) || HCCSpmiDecode::isWrite(t.kind)};
    closeModal();
    HCCSpmiLink::open(tg, r);
}

void openDetail(uint32_t i)
{
    const HCCSpmiBuffers::PacketBuf &pk = HCCSpmiMonitor::packets();
    if (i >= pk.count()) {
        return;
    }
    const Txn &t = pk.at(i);
    char title[48];
    snprintf(title, sizeof(title), "GÓI #%u%s", (unsigned)(i + 1), v.demo ? "   (DEMO - mô phỏng)" : "");
    const int h = std::min<int>(430, v.H - 30);
    lv_obj_t *c = modalCard(780, h, title);
    std::string s;
    char b[200], tb[32], tb2[32];
    fmtTime(tb, sizeof(tb), usOf(t.t_sample));
    fmtTime(tb2, sizeof(tb2), usOf(t.dur));
    snprintf(b, sizeof(b), "Thời gian: %s  (mẫu %llu)     Độ dài: %s\n", tb, (unsigned long long)t.t_sample, tb2);
    s += b;
    if (t.kind == HCCSpmiDecode::K_BAD) {
        snprintf(b, sizeof(b), "Khung lệnh: SID? lệnh 0x%02X (12 bit đầu nếu có) - KHUNG LỆNH HỎNG\n", t.cmd);
        s += b;
    } else {
        snprintf(b, sizeof(b), "SID: 0x%X (%u)     Lệnh: 0x%02X = %s (%s)\n", t.sid, t.sid, t.cmd,
                 HCCSpmiDecode::kindName(t.kind), HCCSpmiDecode::kindLongName(t.kind));
        s += b;
        char r[12], d[28];
        fmtReg(r, sizeof(r), t);
        fmtData(d, sizeof(d), t);
        snprintf(b, sizeof(b), "Thanh ghi: %s     Dữ liệu (%u byte): %s\n", r, (unsigned)t.len, d);
        s += b;
    }
    // Danh sách khung + parity.
    s += "Khung (parity lẻ): CMD ";
    s += (t.frame_err & 1) ? "SAI" : "đúng";
    const int na = HCCSpmiDecode::addrFrames(t.kind);
    int idx = 1;
    for (int k = 0; k < na; ++k, ++idx) {
        snprintf(b, sizeof(b), " | ĐC%d %s", k, (t.frame_err >> idx) & 1 ? "SAI" : "đúng");
        s += b;
    }
    for (int k = 0; k < t.len && t.kind != HCCSpmiDecode::K_REG0_WRITE; ++k, ++idx) {
        snprintf(b, sizeof(b), " | D%d %s", k, (t.frame_err >> idx) & 1 ? "SAI" : "đúng");
        s += b;
    }
    s += "\n";
    snprintf(b, sizeof(b), "Số bit thu: %u     Bit dư sau khung: %u (0x%02X)     Thiếu bit: %s\n", (unsigned)t.nbits,
             (unsigned)t.extra_bits, t.extra, t.short_bits ? "CÓ" : "không");
    s += b;
    snprintf(b, sizeof(b), "Trạng thái: %s%s\n", HCCSpmiDecode::statusName(HCCSpmiDecode::status(t)),
             (t.flags & HCCSpmiDecode::F_LOST_BEFORE) ? "   - ngay trước gói có dữ liệu bị mất (tràn bộ đệm)" : "");
    s += b;
    s += "Chỉ là giá trị thô trên bus: không gán tên IC / đường nguồn.";
    lv_obj_t *l = label(c, s.c_str(), &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_set_width(l, 740);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, 0, 40);
    const int by = h - 32 - 52;
    lv_obj_t *wb = button(c, "XEM SÓNG", 150, 46, C_BLUE, onDetailWave, nullptr);
    lv_obj_set_pos(wb, 0, by);
    setEnabled(wb, HCCSpmiMonitor::raw().segCount() > 0);
    for (int k = 0; k < 2; ++k) {
        const HCCSpmiLink::Target tg = (HCCSpmiLink::Target)k;
        const bool ok = HCCSpmiLink::available(tg);
        snprintf(b, sizeof(b), "%s%s", HCCSpmiLink::targetName(tg), ok ? "" : " (chưa nối)");
        lv_obj_t *lb = button(c, b, 200, 46, C_PURPLE, onLink, (void *)(uintptr_t)k);
        lv_obj_set_pos(lb, 162 + k * 212, by);
        setEnabled(lb, ok);
    }
    lv_obj_t *cb = button(c, "ĐÓNG", 110, 46, C_BORDER, onCloseModal, nullptr);
    lv_obj_set_pos(cb, 638, by);
}

// --- Xác nhận phần cứng (PARLIO) ---

void onHwConfirm(lv_event_t *)
{
    HCCSpmiMonitor::setHwConfirmed(true);
    closeModal();
    lv_label_set_text(v.msg, "Đã xác nhận mạch thu. Bấm START để thu (chỉ nghe, không phát lệnh).");
}

void onHwRevoke(lv_event_t *)
{
    HCCSpmiMonitor::setHwConfirmed(false);
    closeModal();
    lv_label_set_text(v.msg, "Đã thu hồi xác nhận mạch thu PARLIO.");
}

void openHwPanel(const char *why)
{
    const int h = std::min<int>(450, v.H - 20);
    lv_obj_t *c = modalCard(860, h, "HARDWARE NOT SUPPORTED");
    lv_obj_set_style_border_color(c, C_ERR, 0);
    std::string s = why;
    s += "\n\nYêu cầu tối thiểu cho chế độ PARLIO (xem docs/SPMI_MONITOR.md):\n"
         "- Mạch chuyển mức một chiều (main -> P4), trở kháng vào cao, VCCA lấy từ VIO 1,2 / 1,8 V của main, "
         "VCCB 3,3 V từ board; không có điện trở kéo phía bus.\n"
         "- Dây đo ngắn, có GND chung; điện trở nối tiếp 47-100 ohm phía P4.\n"
         "- SCLK <= 1/4 tần số lấy mẫu (80 MHz -> SCLK <= 20 MHz; nên <= 13 MHz). Nhanh hơn: dùng bộ thu FPGA.\n"
         "- Không dùng mạch BSS138 của RFFE: kéo lên làm thay đổi bus và quá chậm cho SPMI.";
    lv_obj_t *l = label(c, s.c_str(), &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_set_width(l, 820);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, 0, 38);
    const int by = h - 32 - 52;
    if (s_src_sel == Src::PARLIO && HCCRffe::configured()) {
        lv_obj_t *ok = button(c, "ĐÃ LẮP MẠCH ĐÚNG - CHO PHÉP", 330, 46, C_WARN, onHwConfirm, nullptr);
        lv_obj_set_pos(ok, 0, by);
    }
    if (HCCSpmiMonitor::hwConfirmed()) {
        lv_obj_t *rv = button(c, "THU HỒI XÁC NHẬN", 220, 46, C_BORDER, onHwRevoke, nullptr);
        lv_obj_set_pos(rv, 342, by);
    }
    lv_obj_t *cb = button(c, "ĐÓNG", 120, 46, C_BORDER, onCloseModal, nullptr);
    lv_obj_set_pos(cb, 708, by);
}

// --- Chọn file phân tích lại ---

std::vector<std::string> s_files;

void onFilePick(lv_event_t *e)
{
    const uint32_t k = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (k >= s_files.size()) {
        return;
    }
    HCCSpmiSource::Params p;
    snprintf(p.file, sizeof(p.file), BSP_SD_MOUNT_POINT "/spmi/%s", s_files[k].c_str());
    closeModal();
    char err[200] = "";
    if (!HCCSpmiMonitor::start(Src::FILE, p, err, sizeof(err))) {
        lv_label_set_text(v.msg, err);
        return;
    }
    v.sel = -1;
    v.follow = false;
    lv_obj_clear_state(v.follow_btn, LV_STATE_CHECKED);
    resetFilter();
    v.wave_seen = UINT32_MAX;
    v.t0 = 0;
    lv_label_set_text(v.msg, "Đang phân tích file...");
}

void openFilePanel(void)
{
    s_files.clear();
    DIR *d = opendir(BSP_SD_MOUNT_POINT "/spmi");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr && s_files.size() < 300) {
            const char *n = de->d_name;
            const size_t len = strlen(n);
            if ((len > 5 && strcasecmp(n + len - 5, ".spmc") == 0) || (len > 4 && strcasecmp(n + len - 4, ".csv") == 0)) {
                s_files.push_back(n);
            }
        }
        closedir(d);
    }
    std::sort(s_files.begin(), s_files.end(), std::greater<std::string>());
    const int h = std::min<int>(460, v.H - 20);
    lv_obj_t *c = modalCard(640, h, "PHÂN TÍCH LẠI FILE (/sdcard/spmi)");
    lv_obj_t *list = lv_list_create(c);
    lv_obj_set_pos(list, 0, 38);
    lv_obj_set_size(list, 606, h - 32 - 38 - 62);
    lv_obj_set_style_bg_color(list, C_BG, 0);
    lv_obj_set_style_border_color(list, C_BORDER, 0);
    if (s_files.empty()) {
        lv_obj_t *l = label(list, "Chưa có file .spmc / .csv trong /sdcard/spmi", &lv_font_be_vietnam_16, C_MUTED);
        lv_obj_set_style_pad_all(l, 12, 0);
    }
    for (size_t i = 0; i < s_files.size(); ++i) {
        lv_obj_t *b = lv_list_add_btn(list, nullptr, s_files[i].c_str());
        lv_obj_set_style_bg_color(b, C_PANEL2, 0);
        lv_obj_set_style_text_color(b, strstr(s_files[i].c_str(), "DEMO") ? C_YELLOW : C_TEXT, 0);
        lv_obj_set_style_text_font(b, &lv_font_be_vietnam_18, 0);
        lv_obj_add_event_cb(b, onFilePick, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    lv_obj_t *cb = button(c, "ĐÓNG", 120, 46, C_BORDER, onCloseModal, nullptr);
    lv_obj_set_pos(cb, 486, h - 32 - 50);
}

// ==================== nút chính ====================

uint32_t rateHz(void)
{
    static const uint32_t r[3] = {20000000, 40000000, 80000000};
    return r[s_rate_sel < 0 || s_rate_sel > 2 ? 2 : s_rate_sel];
}

void onStart(lv_event_t *)
{
    HCCSpmiSource::Params p;
    p.sample_hz = rateHz();
    char err[320] = "";
    if (!HCCSpmiMonitor::start(s_src_sel, p, err, sizeof(err))) {
        const Snapshot sn = HCCSpmiMonitor::snapshot();
        if (sn.state == State::HW_NOT_SUPPORTED) {
            openHwPanel(err);
        } else {
            lv_label_set_text(v.msg, err);
        }
        return;
    }
    v.sel = -1;
    v.follow = true;
    lv_obj_add_state(v.follow_btn, LV_STATE_CHECKED);
    resetFilter();
    v.t0 = 0;
    v.wave_seen = UINT32_MAX;
    lv_label_set_text(v.msg, s_src_sel == Src::DEMO ? "DEMO: dữ liệu mô phỏng, không phải kết quả đo"
                                                    : "Đang nghe bus (chỉ nhận, không phát lệnh)...");
}

void onStop(lv_event_t *)
{
    HCCSpmiMonitor::stop();
}

void onClear(lv_event_t *)
{
    if (HCCSpmiMonitor::running()) {
        return;
    }
    HCCSpmiMonitor::clear();
    v.sel = -1;
    resetFilter();
    fillTable(true);
    lv_obj_invalidate(v.wave);
    lv_label_set_text(v.msg, "Đã xoá dữ liệu");
}

void onSave(lv_event_t *)
{
    if (!HCCSpmiMonitor::startSave()) {
        lv_label_set_text(v.msg, "Không lưu được (đang thu / chưa có gói / đang lưu)");
        return;
    }
    v.saving = true;
    lv_label_set_text(v.msg, "Đang lưu thẻ SD...");
}

void onOpen(lv_event_t *)
{
    if (HCCSpmiMonitor::running()) {
        lv_label_set_text(v.msg, "Dừng thu trước khi mở file");
        return;
    }
    openFilePanel();
}

void onAnalyzeBack(lv_event_t *)
{
    lv_obj_t *scr = s_screen;
    lv_obj_clean(scr);
    show(scr, s_back_cb, s_back_user);
}

void onAnalyze(lv_event_t *)
{
    lv_obj_t *scr = s_screen;
    lv_obj_clean(scr);
    HCCSpmiAnalyzerView::show(scr, onAnalyzeBack, nullptr);
}

void onBack(lv_event_t *e)
{
    HCCSpmiMonitor::stop();  // về SCANNER: trả chân SCLK / SDATA
    if (s_back_cb) {
        lv_event_t ev = *e;
        ev.user_data = s_back_user;
        s_back_cb(&ev);
    }
}

void onSrc(lv_event_t *e)
{
    s_src_sel = (Src)lv_dropdown_get_selected(lv_event_get_target(e));
}

void onRate(lv_event_t *e)
{
    s_rate_sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));
}

// ==================== cập nhật định kỳ ====================

void setBadge(State st, bool demo)
{
    lv_color_t c = C_BORDER;
    switch (st) {
    case State::IDLE: c = C_BORDER; break;
    case State::CAPTURING: c = C_OK; break;
    case State::NO_SIGNAL: c = lv_color_hex(0x8A6D1F); break;
    case State::STOPPED: c = lv_color_hex(0x34607A); break;
    case State::OVERFLOW: c = C_WARN; break;
    case State::HW_NOT_SUPPORTED: c = C_ERR; break;
    case State::REPLAY: c = C_PURPLE; break;
    case State::ERROR: c = C_ERR; break;
    }
    lv_obj_set_style_bg_color(v.badge, c, 0);
    lv_label_set_text(v.badge_lbl, HCCSpmiMonitor::stateName(st));
    if (demo) {
        lv_obj_clear_flag(v.demo_tag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v.demo_tag, LV_OBJ_FLAG_HIDDEN);
    }
}

void onTimer(lv_timer_t *)
{
    const Snapshot sn = HCCSpmiMonitor::snapshot();
    v.hz = sn.sample_hz ? sn.sample_hz : v.hz;
    v.swapped = sn.swapped;
    v.demo = sn.demo;
    setBadge(sn.state, sn.demo);

    char info[200];
    char sclk[24] = "-";
    if (sn.sclk_mhz > 0) {
        snprintf(sclk, sizeof(sclk), "%.2f MHz%s", sn.sclk_mhz, sn.undersampled ? "(!)" : "");
    }
    if (sn.replay || sn.src == Src::FILE) {
        snprintf(info, sizeof(info), "%s · SCLK %s · FRAME %u · LỖI %u · MẤT %u", sn.file[0] ? sn.file : "FILE", sclk,
                 (unsigned)sn.frames, (unsigned)sn.errors, (unsigned)(sn.lost_chunks + sn.dropped_pkts));
    } else {
        snprintf(info, sizeof(info), "%s %u MHz · SCLK %s · FRAME %u · LỖI %u · MẤT %u",
                 sn.source[0] ? sn.source : "PARLIO", (unsigned)(sn.sample_hz / 1000000), sclk, (unsigned)sn.frames,
                 (unsigned)sn.errors, (unsigned)(sn.lost_chunks + sn.dropped_pkts));
    }
    lv_label_set_text(v.info, info);

    if (sn.state != v.last_state) {
        if (sn.msg[0]) {
            lv_label_set_text(v.msg, sn.msg);
        }
        v.last_state = sn.state;
    } else if (sn.running && sn.undersampled) {
        lv_label_set_text(v.msg, "Cảnh báo: SCLK > 1/4 tần số lấy mẫu - giải mã không tin cậy, tăng tần số lấy mẫu");
    }

    const bool run = sn.running;
    setEnabled(v.start_btn, !run && !HCCSpmiMonitor::saving());
    setEnabled(v.stop_btn, run);
    setEnabled(v.clear_btn, !run);
    setEnabled(v.save_btn, !run && sn.frames > 0 && !HCCSpmiMonitor::saving());
    setEnabled(v.open_btn, !run);
    setEnabled(v.src_dd, !run);
    setEnabled(v.rate_dd, !run);

    if (v.saving && !HCCSpmiMonitor::saving()) {
        v.saving = false;
        char m[200];
        const bool ok = HCCSpmiMonitor::saveResult(m, sizeof(m));
        lv_label_set_text(v.msg, m);
        lv_obj_set_style_text_color(v.msg, ok ? C_CYAN : C_ERR, 0);
    }

    scanNew();
    fillTable(false);

    const uint32_t n = HCCSpmiMonitor::packets().count();
    if (n != v.wave_seen) {
        v.wave_seen = n;
        if (v.follow && n > 0) {
            showPacketOnWave(n - 1, false);
        } else {
            lv_obj_invalidate(v.wave);
        }
    }
}

void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
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
    if (H < 420 || H > 800) {
        H = 527;
    }
    v.H = H;

    // Thanh trên.
    lv_obj_t *title = label(root, "SPMI MONITOR", &lv_font_be_vietnam_24, C_TEXT);
    lv_obj_set_pos(title, 12, 6);
    v.badge = lv_obj_create(root);
    lv_obj_remove_style_all(v.badge);
    lv_obj_set_size(v.badge, 232, 30);
    lv_obj_set_pos(v.badge, 196, 6);
    lv_obj_set_style_radius(v.badge, 15, 0);
    lv_obj_set_style_bg_opa(v.badge, LV_OPA_COVER, 0);
    v.badge_lbl = label(v.badge, "IDLE", &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_center(v.badge_lbl);
    v.demo_tag = lv_obj_create(root);
    lv_obj_remove_style_all(v.demo_tag);
    lv_obj_set_size(v.demo_tag, 70, 30);
    lv_obj_set_pos(v.demo_tag, 434, 6);
    lv_obj_set_style_radius(v.demo_tag, 15, 0);
    lv_obj_set_style_bg_opa(v.demo_tag, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(v.demo_tag, C_YELLOW, 0);
    lv_obj_t *dl = label(v.demo_tag, "DEMO", &lv_font_be_vietnam_16, lv_color_hex(0x101820));
    lv_obj_center(dl);
    lv_obj_add_flag(v.demo_tag, LV_OBJ_FLAG_HIDDEN);
    v.info = label(root, "", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_pos(v.info, 512, 11);
    lv_obj_set_width(v.info, 504);
    lv_label_set_long_mode(v.info, LV_LABEL_LONG_DOT);
    v.msg = label(root, "Chỉ nghe thụ động: SPMI MONITOR không phát lệnh và không điều khiển bus.",
                  &lv_font_be_vietnam_14, C_CYAN);
    lv_obj_set_pos(v.msg, 14, 40);
    lv_obj_set_width(v.msg, 1000);
    lv_label_set_long_mode(v.msg, LV_LABEL_LONG_DOT);

    // Dạng sóng.
    const int wy = 62, wh = 122;
    v.wave = lv_obj_create(root);
    lv_obj_remove_style_all(v.wave);
    lv_obj_set_pos(v.wave, 8, wy);
    lv_obj_set_size(v.wave, 1008, wh);
    lv_obj_set_style_bg_color(v.wave, lv_color_hex(0x0A1117), 0);
    lv_obj_set_style_bg_opa(v.wave, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(v.wave, C_BORDER, 0);
    lv_obj_set_style_border_width(v.wave, 1, 0);
    lv_obj_set_style_radius(v.wave, 6, 0);
    lv_obj_add_flag(v.wave, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(v.wave, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(v.wave, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v.wave, drawWave, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(v.wave, onWavePress, LV_EVENT_PRESSING, nullptr);
    lv_obj_t *l1 = label(v.wave, "SCLK", &lv_font_be_vietnam_16, C_CYAN);
    lv_obj_set_pos(l1, 8, 28);
    lv_obj_t *l2 = label(v.wave, "SDATA", &lv_font_be_vietnam_16, C_YELLOW);
    lv_obj_set_pos(l2, 6, 60);

    // Hàng lọc / trang / phóng to.
    const int fy = wy + wh + 6, fh = 38;
    v.sid_dd = lv_dropdown_create(root);
    std::string opts = "SID: TẤT CẢ";
    for (int s = 0; s < 16; ++s) {
        char b[16];
        snprintf(b, sizeof(b), "\nSID 0x%X", s);
        opts += b;
    }
    lv_dropdown_set_options(v.sid_dd, opts.c_str());
    lv_obj_set_pos(v.sid_dd, 8, fy);
    lv_obj_set_size(v.sid_dd, 140, fh);
    lv_obj_set_style_text_font(v.sid_dd, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_bg_color(v.sid_dd, C_PANEL2, 0);
    lv_obj_set_style_text_color(v.sid_dd, C_TEXT, 0);
    lv_obj_set_style_border_color(v.sid_dd, C_BORDER, 0);
    lv_obj_set_style_pad_ver(v.sid_dd, 8, 0);
    lv_obj_add_event_cb(v.sid_dd, onFilterSid, LV_EVENT_VALUE_CHANGED, nullptr);

    static const char *type_map[] = {"TẤT CẢ", "READ", "WRITE", "LỖI", ""};
    v.type_bm = lv_btnmatrix_create(root);
    lv_btnmatrix_set_map(v.type_bm, type_map);
    lv_btnmatrix_set_btn_ctrl_all(v.type_bm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(v.type_bm, true);
    lv_btnmatrix_set_btn_ctrl(v.type_bm, 0, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_pos(v.type_bm, 154, fy);
    lv_obj_set_size(v.type_bm, 330, fh);
    lv_obj_set_style_pad_all(v.type_bm, 2, 0);
    lv_obj_set_style_pad_gap(v.type_bm, 4, 0);
    lv_obj_set_style_bg_color(v.type_bm, C_PANEL, 0);
    lv_obj_set_style_border_width(v.type_bm, 0, 0);
    lv_obj_set_style_text_font(v.type_bm, &lv_font_be_vietnam_16, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(v.type_bm, C_PANEL2, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.type_bm, C_TEXT, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(v.type_bm, C_BLUE,
                              (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_add_event_cb(v.type_bm, onFilterType, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *pv = button(root, "<", 40, fh, C_PANEL2, onPrev, nullptr);
    lv_obj_set_pos(pv, 492, fy);
    v.page_lbl = label(root, "0 / 0", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_width(v.page_lbl, 150);
    lv_obj_set_style_text_align(v.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v.page_lbl, 534, fy + 9);
    lv_obj_t *nx = button(root, ">", 40, fh, C_PANEL2, onNext, nullptr);
    lv_obj_set_pos(nx, 686, fy);
    v.follow_btn = button(root, "CUỐI", 64, fh, C_PANEL2, onFollow, nullptr);
    lv_obj_add_flag(v.follow_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(v.follow_btn, C_BLUE, (lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_add_state(v.follow_btn, LV_STATE_CHECKED);
    lv_obj_set_pos(v.follow_btn, 732, fy);
    lv_obj_t *zo = button(root, "-", 44, fh, C_PANEL2, onZoomOut, nullptr);
    lv_obj_set_pos(zo, 806, fy);
    lv_obj_t *zi = button(root, "+", 44, fh, C_PANEL2, onZoomIn, nullptr);
    lv_obj_set_pos(zi, 854, fy);
    lv_obj_t *ft = button(root, "GÓI", 56, fh, C_PANEL2, onFit, nullptr);
    lv_obj_set_pos(ft, 902, fy);
    lv_obj_t *hm = button(root, "ĐẦU", 54, fh, C_PANEL2, onHome, nullptr);
    lv_obj_set_pos(hm, 962, fy);

    // Bảng gói.
    const int ty = fy + fh + 6;
    const int by = H - 50;
    static const int cw[6] = {170, 74, 190, 124, 250, 176};
    static const char *hdr[6] = {"TIME", "SID", "CMD", "REGISTER", "DATA", "STATUS"};
    const int hh = 30;  // tiêu đề cố định, không cuộn theo bảng
    v.hdr = lv_table_create(root);
    v.table = lv_table_create(root);
    lv_obj_set_pos(v.hdr, 8, ty);
    lv_obj_set_size(v.hdr, 1008, hh);
    lv_obj_set_pos(v.table, 8, ty + hh);
    lv_obj_set_size(v.table, 1008, by - ty - hh - 6);
    for (lv_obj_t *tb : {v.hdr, v.table}) {
        lv_table_set_col_cnt(tb, 6);
        for (int c = 0; c < 6; ++c) {
            lv_table_set_col_width(tb, c, cw[c]);
        }
        lv_obj_set_style_bg_color(tb, C_BG, 0);
        lv_obj_set_style_border_color(tb, C_BORDER, 0);
        lv_obj_set_style_border_width(tb, 1, 0);
        lv_obj_set_style_pad_all(tb, 0, 0);
        lv_obj_set_style_text_font(tb, &lv_font_be_vietnam_16, (lv_style_selector_t)LV_PART_ITEMS);
        lv_obj_set_style_bg_color(tb, C_PANEL, (lv_style_selector_t)LV_PART_ITEMS);
        lv_obj_set_style_text_color(tb, C_TEXT, (lv_style_selector_t)LV_PART_ITEMS);
        lv_obj_set_style_border_color(tb, C_BORDER, (lv_style_selector_t)LV_PART_ITEMS);
        lv_obj_set_style_border_width(tb, 1, (lv_style_selector_t)LV_PART_ITEMS);
        lv_obj_set_style_pad_ver(tb, 3, (lv_style_selector_t)LV_PART_ITEMS);
        lv_obj_set_style_pad_hor(tb, 8, (lv_style_selector_t)LV_PART_ITEMS);
    }
    for (int c = 0; c < 6; ++c) {
        lv_table_set_cell_value(v.hdr, 0, c, hdr[c]);
    }
    lv_obj_set_style_bg_color(v.hdr, C_PANEL2, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.hdr, C_CYAN, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(v.hdr, 4, (lv_style_selector_t)LV_PART_ITEMS);
    lv_obj_clear_flag(v.hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(v.hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v.table, onTableEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(v.table, onTableEvent, LV_EVENT_DRAW_PART_BEGIN, nullptr);

    // Hàng nút dưới.
    int x = 8;
    lv_obj_t *back = button(root, "SCANNER", 104, 44, C_BORDER, onBack, nullptr);
    lv_obj_set_pos(back, x, by);
    x += 112;
    v.src_dd = lv_dropdown_create(root);
    lv_dropdown_set_options(v.src_dd, "PARLIO\nDEMO\nFPGA/STM32");
    lv_dropdown_set_selected(v.src_dd, (uint16_t)s_src_sel == (uint16_t)Src::FILE ? 0 : (uint16_t)s_src_sel);
    lv_obj_set_pos(v.src_dd, x, by);
    lv_obj_set_size(v.src_dd, 150, 44);
    x += 158;
    v.rate_dd = lv_dropdown_create(root);
    lv_dropdown_set_options(v.rate_dd, "20 MHz\n40 MHz\n80 MHz");
    lv_dropdown_set_selected(v.rate_dd, (uint16_t)s_rate_sel);
    lv_obj_set_pos(v.rate_dd, x, by);
    lv_obj_set_size(v.rate_dd, 112, 44);
    x += 120;
    for (lv_obj_t *dd : {v.src_dd, v.rate_dd}) {
        lv_obj_set_style_text_font(dd, &lv_font_be_vietnam_16, 0);
        lv_obj_set_style_bg_color(dd, C_PANEL2, 0);
        lv_obj_set_style_text_color(dd, C_TEXT, 0);
        lv_obj_set_style_border_color(dd, C_BORDER, 0);
        lv_obj_set_style_pad_ver(dd, 12, 0);
    }
    lv_obj_add_event_cb(v.src_dd, onSrc, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(v.rate_dd, onRate, LV_EVENT_VALUE_CHANGED, nullptr);
    v.start_btn = button(root, "START", 98, 44, C_OK, onStart, nullptr);
    lv_obj_set_pos(v.start_btn, x, by);
    x += 104;
    v.stop_btn = button(root, "STOP", 88, 44, C_ERR, onStop, nullptr);
    lv_obj_set_pos(v.stop_btn, x, by);
    x += 94;
    v.clear_btn = button(root, "CLEAR", 88, 44, C_BORDER, onClear, nullptr);
    lv_obj_set_pos(v.clear_btn, x, by);
    x += 94;
    v.save_btn = button(root, "SAVE SD", 104, 44, C_BLUE, onSave, nullptr);
    lv_obj_set_pos(v.save_btn, x, by);
    x += 110;
    v.open_btn = button(root, "OPEN", 80, 44, C_PURPLE, onOpen, nullptr);
    lv_obj_set_pos(v.open_btn, x, by);
    x += 86;
    lv_obj_t *an = button(root, "ANALYZE", 1016 - x, 44, lv_color_hex(0x16A085), onAnalyze, nullptr);
    lv_obj_set_pos(an, x, by);

    v.last_state = State::IDLE;
    const HCCSpmiMonitor::Snapshot sn = HCCSpmiMonitor::snapshot();
    if (sn.msg[0]) {
        lv_label_set_text(v.msg, sn.msg);
    }
    v.follow = true;
    onTimer(nullptr);
    fillTable(true);
    v.timer = lv_timer_create(onTimer, 100, nullptr);
}

}  // namespace HCCSpmiMonitorView
