/*
 * XiaozhiAvatar: xem XiaozhiAvatar.hpp.
 * Toạ độ thiết kế: khung 300 x 360 (đầu ở giữa phía trên), được co giãn vừa vùng vẽ.
 */
#include "XiaozhiAvatar.hpp"

#include <math.h>
#include <algorithm>
#include <string.h>

#include "esp_random.h"

namespace {

constexpr float DESIGN_W = 300.0f;
constexpr float DESIGN_H = 360.0f;
constexpr float PI2 = 6.2831853f;

constexpr uint32_t C_HAIR = 0x4B2E2A;
constexpr uint32_t C_HAIR_HI = 0x7A5246;
constexpr uint32_t C_SKIN = 0xFFE3D3;
constexpr uint32_t C_SKIN_SHADE = 0xF2C6AE;
constexpr uint32_t C_SHIRT = 0x5B5FF0;
constexpr uint32_t C_SHIRT_DARK = 0x4246C8;
constexpr uint32_t C_WHITE = 0xFFFFFF;
constexpr uint32_t C_IRIS = 0x6B4BA1;
constexpr uint32_t C_PUPIL = 0x2A1A3A;
constexpr uint32_t C_LINE = 0x3A2430;
constexpr uint32_t C_MOUTH = 0x9E2B3C;
constexpr uint32_t C_LIP = 0xB0475A;
constexpr uint32_t C_TONGUE = 0xFF7A8A;
constexpr uint32_t C_BLUSH = 0xFF8FA3;
constexpr uint32_t C_BOW = 0xFF7EB6;
constexpr uint32_t C_BOW_DARK = 0xE0559A;
constexpr uint32_t C_TEAR = 0x7FC8FF;

float frand(float a, float b)
{
    return a + (b - a) * (float)(esp_random() % 10000) / 10000.0f;
}

// Bộ vẽ: đổi toạ độ thiết kế sang pixel.
struct Painter {
    lv_draw_ctx_t *ctx;
    float ox, oy, s;

    lv_coord_t X(float x) const { return (lv_coord_t)lroundf(ox + x * s); }
    lv_coord_t Y(float y) const { return (lv_coord_t)lroundf(oy + y * s); }
    lv_coord_t S(float v) const { return (lv_coord_t)lroundf(v * s); }

    void ellipse(float cx, float cy, float rx, float ry, uint32_t color, lv_opa_t opa = LV_OPA_COVER) const
    {
        if (rx <= 0.2f || ry <= 0.2f) {
            return;
        }
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(color);
        d.bg_opa = opa;
        d.radius = LV_RADIUS_CIRCLE;
        lv_area_t a = {X(cx - rx), Y(cy - ry), X(cx + rx), Y(cy + ry)};
        lv_draw_rect(ctx, &d, &a);
    }

    void rect(float x1, float y1, float x2, float y2, float radius, uint32_t color, lv_opa_t opa = LV_OPA_COVER) const
    {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(color);
        d.bg_opa = opa;
        d.radius = S(radius);
        lv_area_t a = {X(x1), Y(y1), X(x2), Y(y2)};
        lv_draw_rect(ctx, &d, &a);
    }

    // Góc độ của LVGL: 0 = bên phải, tăng theo chiều kim đồng hồ.
    void arc(float cx, float cy, float r, int start, int end, float width, uint32_t color,
             lv_opa_t opa = LV_OPA_COVER) const
    {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = lv_color_hex(color);
        d.width = (lv_coord_t)std::max(1L, lroundf(width * s));
        d.opa = opa;
        d.rounded = 1;
        lv_point_t c = {X(cx), Y(cy)};
        lv_draw_arc(ctx, &d, &c, (uint16_t)std::max(1, (int)S(r)), (uint16_t)((start + 360) % 360),
                    (uint16_t)((end + 360) % 360));
    }

    void line(float x1, float y1, float x2, float y2, float width, uint32_t color) const
    {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = lv_color_hex(color);
        d.width = (lv_coord_t)std::max(1L, lroundf(width * s));
        d.round_start = 1;
        d.round_end = 1;
        lv_point_t a = {X(x1), Y(y1)};
        lv_point_t b = {X(x2), Y(y2)};
        lv_draw_line(ctx, &d, &a, &b);
    }
};

}  // namespace

lv_obj_t *XiaozhiAvatar::create(lv_obj_t *parent)
{
    _obj = lv_obj_create(parent);
    lv_obj_remove_style_all(_obj);
    lv_obj_clear_flag(_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_obj, onDraw, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(_obj, onDelete, LV_EVENT_DELETE, this);
    _t0 = lv_tick_get();
    _next_blink = frand(1.5f, 4.0f);
    _timer = lv_timer_create(onTick, 40, this);  // ~25 khung/giây
    return _obj;
}

void XiaozhiAvatar::detach(void)
{
    _obj = nullptr;
    _timer = nullptr;  // timer do Brookesia thu dọn cùng app
}

void XiaozhiAvatar::onDelete(lv_event_t *e)
{
    auto *self = static_cast<XiaozhiAvatar *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_obj = nullptr;
    }
}

void XiaozhiAvatar::onTick(lv_timer_t *t)
{
    auto *self = static_cast<XiaozhiAvatar *>(t->user_data);
    if (self != nullptr && self->_obj != nullptr) {
        self->tick();
    }
}

void XiaozhiAvatar::onDraw(lv_event_t *e)
{
    auto *self = static_cast<XiaozhiAvatar *>(lv_event_get_user_data(e));
    if (self != nullptr && self->_obj != nullptr) {
        self->draw(lv_event_get_draw_ctx(e));
    }
}

void XiaozhiAvatar::setEmotion(const char *emotion)
{
    struct Map {
        const char *key;
        Face face;
    };
    static const Map MAP[] = {
        {"neutral", {Eyes::Normal, Brow::Neutral, Mouth::Smile, 70, false}},
        {"happy", {Eyes::Normal, Brow::Neutral, Mouth::Smile, 130, false}},
        {"laughing", {Eyes::Happy, Brow::Raised, Mouth::BigOpen, 150, false}},
        {"funny", {Eyes::Happy, Brow::Neutral, Mouth::Tongue, 130, false}},
        {"sad", {Eyes::Normal, Brow::Sad, Mouth::Sad, 40, false}},
        {"angry", {Eyes::Normal, Brow::Angry, Mouth::Flat, 110, false}},
        {"crying", {Eyes::Closed, Brow::Sad, Mouth::Sad, 60, true}},
        {"loving", {Eyes::Happy, Brow::Neutral, Mouth::Smile, 230, false}},
        {"embarrassed", {Eyes::Normal, Brow::Sad, Mouth::Flat, 255, false}},
        {"surprised", {Eyes::Wide, Brow::Raised, Mouth::O, 80, false}},
        {"shocked", {Eyes::Wide, Brow::Raised, Mouth::O, 30, false}},
        {"thinking", {Eyes::Normal, Brow::Neutral, Mouth::Flat, 70, false}},
        {"winking", {Eyes::Wink, Brow::Neutral, Mouth::Smile, 130, false}},
        {"cool", {Eyes::Sleepy, Brow::Neutral, Mouth::Smile, 60, false}},
        {"relaxed", {Eyes::Closed, Brow::Neutral, Mouth::Smile, 100, false}},
        {"delicious", {Eyes::Happy, Brow::Neutral, Mouth::Tongue, 140, false}},
        {"kissy", {Eyes::Closed, Brow::Neutral, Mouth::Kiss, 210, false}},
        {"confident", {Eyes::Normal, Brow::Raised, Mouth::Smile, 100, false}},
        {"sleepy", {Eyes::Sleepy, Brow::Neutral, Mouth::Flat, 60, false}},
        {"silly", {Eyes::Wink, Brow::Neutral, Mouth::Tongue, 120, false}},
        {"confused", {Eyes::Normal, Brow::Sad, Mouth::Flat, 60, false}},
    };
    _face = MAP[0].face;
    if (emotion == nullptr) {
        return;
    }
    for (const auto &m : MAP) {
        if (strcmp(m.key, emotion) == 0) {
            _face = m.face;
            return;
        }
    }
}

void XiaozhiAvatar::tick(void)
{
    _t = (lv_tick_get() - _t0) / 1000.0f;
    // Miệng: mở nhanh, khép chậm hơn một chút cho tự nhiên.
    const float k_speak = _speak_target > _speak ? 0.6f : 0.35f;
    _speak += (_speak_target - _speak) * k_speak;
    _mic += (_mic_target - _mic) * 0.4f;

    // Hướng nhìn
    if (_mode == Mode::Thinking) {
        _tgx = 3.0f;
        _tgy = -3.0f;
    } else if (_mode == Mode::Listening || _mode == Mode::Speaking) {
        if (_t > _next_gaze) {
            _tgx = frand(-1.5f, 1.5f);
            _tgy = frand(-1.0f, 1.5f);
            _next_gaze = _t + frand(0.8f, 2.0f);
        }
    } else if (_t > _next_gaze) {
        _tgx = frand(-3.5f, 3.5f);
        _tgy = frand(-2.5f, 2.5f);
        _next_gaze = _t + frand(1.5f, 3.5f);
    }
    _gx += (_tgx - _gx) * 0.25f;
    _gy += (_tgy - _gy) * 0.25f;

    // Chớp mắt ngẫu nhiên
    if (_t > _next_blink) {
        _blink_start = _t;
        _next_blink = _t + frand(2.2f, 5.5f);
        if (frand(0, 1) < 0.15f) {
            _next_blink = _t + 0.35f;  // thỉnh thoảng chớp 2 lần
        }
    }
    lv_obj_invalidate(_obj);
}

void XiaozhiAvatar::draw(lv_draw_ctx_t *ctx)
{
    lv_area_t a;
    lv_obj_get_coords(_obj, &a);
    const float w = (float)lv_area_get_width(&a);
    const float h = (float)lv_area_get_height(&a);
    Painter p;
    p.ctx = ctx;
    p.s = fminf(w / DESIGN_W, h / DESIGN_H);
    p.ox = a.x1 + (w - DESIGN_W * p.s) / 2.0f;
    p.oy = a.y1 + (h - DESIGN_H * p.s) / 2.0f;

    const float t = _t;
    const bool speaking = _mode == Mode::Speaking;
    const bool listening = _mode == Mode::Listening;
    const float breath = sinf(PI2 * t / 3.2f) * 3.0f;
    float hx = (_mode == Mode::Idle || _mode == Mode::Sad) ? sinf(PI2 * t / 5.0f) * 4.0f : sinf(PI2 * t / 7.0f) * 1.5f;
    float hy = breath;
    if (listening) {
        hy += 3.0f;  // hơi nghiêng về phía người nói
    }
    if (speaking) {
        hy += _speak * 3.0f;
    }
    const float sway = sinf(PI2 * t / 2.6f) * 5.0f + hx * 0.5f;

    // Mức mở mắt khi chớp (0.16 s)
    float open = 1.0f;
    const float bt = t - _blink_start;
    if (bt >= 0 && bt < 0.16f) {
        open = fabsf(bt - 0.08f) / 0.08f;
    }

    Face f = _face;
    if (_mode == Mode::Sad) {
        f = {Eyes::Normal, Brow::Sad, Mouth::Sad, 40, false};
    }

    /* ---- hào quang theo trạng thái ---- */
    uint32_t aura = 0x5B5FF0;
    float aura_r = 128.0f + sinf(PI2 * t / 3.2f) * 4.0f;
    if (listening) {
        aura = 0x3DD66E;
        aura_r += _mic * 18.0f;
    } else if (speaking) {
        aura = 0xE08A55;
        aura_r += _speak * 14.0f;
    } else if (_mode == Mode::Thinking) {
        aura = 0x7A6FD6;
    }
    p.ellipse(150, 175, aura_r, aura_r, aura, 34);
    p.ellipse(150, 175, aura_r * 0.82f, aura_r * 0.82f, aura, 26);

    const float bx = 0;
    const float by = breath * 0.6f;

    /* ---- đuôi tóc 2 bên (sau lưng) ---- */
    p.ellipse(60 + hx + sway, 215 + hy, 24, 72, C_HAIR);
    p.ellipse(240 + hx - sway, 215 + hy, 24, 72, C_HAIR);
    p.ellipse(62 + hx + sway, 280 + hy, 16, 22, C_HAIR);
    p.ellipse(238 + hx - sway, 280 + hy, 16, 22, C_HAIR);
    // dây buộc tóc
    p.ellipse(68 + hx + sway * 0.4f, 150 + hy, 12, 9, C_BOW);
    p.ellipse(232 + hx - sway * 0.4f, 150 + hy, 12, 9, C_BOW);

    /* ---- tóc sau ---- */
    p.ellipse(150 + hx, 150 + hy, 90, 102, C_HAIR);

    /* ---- thân ---- */
    p.rect(138 + bx, 222 + by, 162 + bx, 268 + by, 6, C_SKIN_SHADE);         // cổ
    p.rect(74 + bx, 258 + by, 226 + bx, 420 + by, 56, C_SHIRT);              // áo
    p.rect(74 + bx, 330 + by, 226 + bx, 420 + by, 20, C_SHIRT_DARK, 120);    // bóng áo
    p.ellipse(150 + bx, 262 + by, 30, 12, C_WHITE);                           // cổ áo
    p.ellipse(150 + bx, 258 + by, 16, 9, C_SKIN_SHADE);
    p.ellipse(150 + bx, 282 + by, 7, 7, C_BOW_DARK);                          // nơ cổ áo
    p.ellipse(138 + bx, 282 + by, 9, 6, C_BOW);
    p.ellipse(162 + bx, 282 + by, 9, 6, C_BOW);

    /* ---- mặt ---- */
    const float fx = 150 + hx;
    const float fy = 162 + hy;
    p.ellipse(fx, fy, 70, 78, C_SKIN);

    /* ---- tóc mái + tóc mai ---- */
    p.ellipse(fx, fy - 64, 82, 40, C_HAIR);
    p.ellipse(fx - 50, fy - 46, 28, 32, C_HAIR);
    p.ellipse(fx + 50, fy - 46, 28, 32, C_HAIR);
    p.ellipse(fx - 14, fy - 48, 30, 26, C_HAIR);
    p.ellipse(fx + 22, fy - 50, 26, 24, C_HAIR);
    p.ellipse(fx - 68, fy + 14, 14, 52, C_HAIR);
    p.ellipse(fx + 68, fy + 14, 14, 52, C_HAIR);
    p.ellipse(fx + 30, fy - 70, 16, 5, C_HAIR_HI, 150);  // ánh tóc

    /* ---- nơ trên đầu ---- */
    p.ellipse(fx - 58, fy - 92, 15, 11, C_BOW);
    p.ellipse(fx - 32, fy - 92, 15, 11, C_BOW);
    p.ellipse(fx - 45, fy - 92, 7, 7, C_BOW_DARK);

    /* ---- má hồng ---- */
    if (f.blush > 0) {
        p.ellipse(fx - 42, fy + 26, 14, 7, C_BLUSH, f.blush);
        p.ellipse(fx + 42, fy + 26, 14, 7, C_BLUSH, f.blush);
    }

    /* ---- mắt ---- */
    const float ey = fy - 2;
    const float exs[2] = {fx - 28, fx + 28};
    for (int i = 0; i < 2; ++i) {
        const float ex = exs[i];
        Eyes kind = f.eyes;
        if (kind == Eyes::Wink) {
            kind = i == 1 ? Eyes::Closed : Eyes::Normal;
        }
        float o = open;
        float scale = 1.0f;
        if (kind == Eyes::Sleepy) {
            o = fminf(open, 0.5f);
        } else if (kind == Eyes::Wide) {
            scale = 1.15f;
        }
        if (kind == Eyes::Happy) {
            p.arc(ex, ey + 6, 12, 200, 340, 5, C_LINE);  // ^ ^
            continue;
        }
        if (kind == Eyes::Closed || o < 0.22f) {
            p.arc(ex, ey - 6, 12, 25, 155, 4, C_LINE);   // mắt nhắm
            continue;
        }
        const float rx = 15 * scale;
        const float ry = 18 * scale * o;
        p.ellipse(ex, ey, rx, ry, C_WHITE);
        const float gx = _gx;
        const float gy = _gy * o;
        p.ellipse(ex + gx, ey + gy + 1, 11 * scale, fminf(13 * scale, ry - 1), C_IRIS);
        p.ellipse(ex + gx, ey + gy + 2, 5.5f * scale, fminf(6.5f * scale, ry * 0.5f), C_PUPIL);
        if (o > 0.5f) {
            p.ellipse(ex + gx - 4, ey + gy - 5, 3.5f, 3.5f, C_WHITE);
            p.ellipse(ex + gx + 4, ey + gy + 5, 1.6f, 1.6f, C_WHITE);
        }
        p.arc(ex, ey + 8 * scale, 19 * scale, 210, 330, 3.5f, C_LINE);  // mi trên
        if (kind == Eyes::Sleepy) {
            p.rect(ex - 17, ey - 20, ex + 17, ey - 2, 4, C_SKIN);
            p.line(ex - 15, ey - 2, ex + 15, ey - 2, 3, C_LINE);
        }
    }

    /* ---- nước mắt ---- */
    if (f.tears) {
        const float drop = fmodf(t * 30.0f, 30.0f);
        p.ellipse(fx - 30, ey + 12 + drop, 4, 6, C_TEAR, 220);
        p.ellipse(fx + 30, ey + 12 + fmodf(drop + 15, 30.0f), 4, 6, C_TEAR, 220);
    }

    /* ---- lông mày ---- */
    for (int i = 0; i < 2; ++i) {
        const float ex = exs[i];
        const float dir = i == 0 ? -1.0f : 1.0f;  // ra ngoài
        float inner = -27;
        float outer = -29;
        switch (f.brow) {
        case Brow::Angry: inner = -19; outer = -31; break;
        case Brow::Sad: inner = -33; outer = -24; break;
        case Brow::Raised: inner = -36; outer = -35; break;
        default: break;
        }
        if (listening) {
            inner -= 2;
            outer -= 2;
        }
        p.line(ex - dir * 4, ey + inner, ex + dir * 14, ey + outer, 4, C_HAIR);
    }

    /* ---- mũi ---- */
    p.line(fx + 1, fy + 18, fx - 1, fy + 22, 2, 0xE8A890);

    /* ---- miệng ---- */
    const float mx = fx;
    const float my = fy + 40;
    if (speaking && _speak > 0.03f) {
        const float ry = 3.0f + _speak * 12.0f;
        const float rx = 11.0f + _speak * 4.0f;
        p.ellipse(mx, my, rx, ry, C_MOUTH);
        if (ry > 6) {
            p.ellipse(mx, my + ry * 0.45f, rx * 0.55f, ry * 0.4f, C_TONGUE);
        }
    } else {
        switch (f.mouth) {
        case Mouth::Smile:
            p.arc(mx, my - 11, 14, 35, 145, 4, C_LIP);
            break;
        case Mouth::Flat:
            p.line(mx - 9, my, mx + 9, my, 4, C_LIP);
            break;
        case Mouth::Sad:
            p.arc(mx, my + 12, 13, 215, 325, 4, C_LIP);
            break;
        case Mouth::O:
            p.ellipse(mx, my, 8, 10, C_MOUTH);
            break;
        case Mouth::BigOpen:
            p.ellipse(mx, my, 16, 11, C_MOUTH);
            p.ellipse(mx, my + 5, 9, 5, C_TONGUE);
            break;
        case Mouth::Tongue:
            p.arc(mx, my - 11, 14, 35, 145, 4, C_LIP);
            p.ellipse(mx + 4, my + 6, 6, 6, C_TONGUE);
            break;
        case Mouth::Kiss:
            p.ellipse(mx, my, 5, 6, C_LIP);
            break;
        }
    }

    /* ---- trang trí theo trạng thái ---- */
    if (listening) {
        // sóng âm hai bên, to theo giọng người nói
        for (int k = 0; k < 3; ++k) {
            const float ph = fmodf(t * 1.6f + k * 0.33f, 1.0f);
            const lv_opa_t opa = (lv_opa_t)(60 + 160 * (1.0f - ph) * (0.35f + _mic));
            const float r = 16 + k * 12 + _mic * 8;
            p.arc(fx + 92, fy, r, 315, 45, 4, 0x3DD66E, opa);
            p.arc(fx - 92, fy, r, 135, 225, 4, 0x3DD66E, opa);
        }
    } else if (_mode == Mode::Thinking) {
        // "..." phía trên đầu, sáng lần lượt
        for (int k = 0; k < 3; ++k) {
            const float ph = fmodf(t * 2.0f - k * 0.3f, 1.0f);
            const lv_opa_t opa = (lv_opa_t)(80 + 175 * (ph < 0.5f ? ph * 2 : (1 - ph) * 2));
            p.ellipse(fx + 62 + k * 20, fy - 108 - k * 10, 6 + k * 2, 6 + k * 2, C_WHITE, opa);
        }
    } else if (speaking) {
        // nốt nhạc nhỏ bay lên khi nói to
        const float ph = fmodf(t * 0.8f, 1.0f);
        const lv_opa_t opa = (lv_opa_t)(200 * (1.0f - ph) * fminf(1.0f, _speak * 3));
        if (opa > 10) {
            p.ellipse(fx + 95, fy - 40 - ph * 50, 7, 6, 0xE08A55, opa);
            p.line(fx + 101, fy - 40 - ph * 50, fx + 101, fy - 62 - ph * 50, 3, 0xE08A55);
        }
    }
}
