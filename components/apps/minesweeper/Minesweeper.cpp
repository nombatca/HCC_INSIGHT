#include "Minesweeper.hpp"

#include <cstdio>
#include <cstring>

#include "esp_random.h"
#include "vn_fonts.h"

#define TAG "Minesweeper"

LV_IMG_DECLARE(img_app_minesweeper);

namespace {
constexpr int CELL = 50;              // kích thước 1 ô (px)
constexpr int BOARD_TOP = 96;         // y bắt đầu lưới (chừa thanh trạng thái + header)
// Màu số 1..8 kiểu Minesweeper cổ điển.
const uint32_t kNumColor[9] = {
    0x000000, 0x4A90E2, 0x3DD66E, 0xFF6B6B, 0x9B6DFF,
    0xF2994A, 0x00D9FF, 0xB8C5CF, 0xFFFFFF};
inline int encode(int r, int c) { return r * Minesweeper::COLS + c; }
}  // namespace

Minesweeper::Minesweeper()
    : ESP_Brookesia_PhoneApp("Do Min", &img_app_minesweeper, true)
{
    memset(_cell, 0, sizeof(_cell));
}

Minesweeper::~Minesweeper() {}

bool Minesweeper::init(void) { return true; }

bool Minesweeper::run(void)
{
    buildUi(lv_scr_act());
    newGame();
    if (_timer == nullptr) _timer = lv_timer_create(tickCb, 1000, this);
    return true;
}

bool Minesweeper::back(void)
{
    notifyCoreClosed();
    return true;
}

bool Minesweeper::close(void)
{
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    return true;
}

void Minesweeper::buildUi(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0B1420), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // ---- Thanh tiêu đề ----
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_WARNING " DÒ MÌN");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_be_vietnam_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 22);

    _mines_label = lv_label_create(parent);
    lv_obj_set_style_text_color(_mines_label, lv_color_hex(0xFF8A8A), 0);
    lv_obj_set_style_text_font(_mines_label, &lv_font_be_vietnam_20, 0);
    lv_obj_align(_mines_label, LV_ALIGN_TOP_LEFT, 300, 22);

    _time_label = lv_label_create(parent);
    lv_obj_set_style_text_color(_time_label, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(_time_label, &lv_font_be_vietnam_20, 0);
    lv_obj_align(_time_label, LV_ALIGN_TOP_LEFT, 470, 22);

    _status_label = lv_label_create(parent);
    lv_label_set_long_mode(_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_status_label, 1000);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(_status_label, &lv_font_be_vietnam_16, 0);
    lv_obj_align(_status_label, LV_ALIGN_TOP_LEFT, 24, 62);
    lv_label_set_text(_status_label, "Chạm: mở ô   -   Giữ: cắm cờ");

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 150, 56);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -24, 16);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2478D0), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, newGameCb, LV_EVENT_CLICKED, this);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "VÁN MỚI");
    lv_obj_set_style_text_font(bl, &lv_font_be_vietnam_16, 0);
    lv_obj_center(bl);

    // ---- Lưới ô ----
    const int x0 = (1024 - COLS * CELL) / 2;
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            // Dùng lv_obj thường (không phải lv_btn) để theme không tô xanh đè màu.
            lv_obj_t *cell = lv_obj_create(parent);
            lv_obj_set_size(cell, CELL - 4, CELL - 4);
            lv_obj_set_pos(cell, x0 + c * CELL, BOARD_TOP + r * CELL);
            lv_obj_set_style_radius(cell, 6, 0);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x24405A), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(cell, (void *)(intptr_t)encode(r, c));
            lv_obj_add_event_cb(cell, cellClickedCb, LV_EVENT_SHORT_CLICKED, this);
            lv_obj_add_event_cb(cell, cellLongCb, LV_EVENT_LONG_PRESSED, this);
            lv_obj_t *lab = lv_label_create(cell);
            lv_label_set_text(lab, "");
            lv_obj_set_style_text_font(lab, &lv_font_be_vietnam_20, 0);
            lv_obj_center(lab);
            _cell[r][c] = cell;
        }
    }
}

void Minesweeper::newGame(void)
{
    memset(_mine, 0, sizeof(_mine));
    memset(_revealed, 0, sizeof(_revealed));
    memset(_flagged, 0, sizeof(_flagged));
    memset(_adj, 0, sizeof(_adj));
    _mines_placed = false;
    _alive = true;
    _won = false;
    _revealed_count = 0;
    _flags = 0;
    _seconds = 0;
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) refreshCell(r, c);
    refreshHeader();
    if (_status_label) lv_label_set_text(_status_label, "Chạm: mở ô   -   Giữ: cắm cờ");
}

void Minesweeper::placeMines(int first_r, int first_c)
{
    int placed = 0;
    while (placed < MINES) {
        int r = esp_random() % ROWS;
        int c = esp_random() % COLS;
        if (_mine[r][c]) continue;
        if (r == first_r && c == first_c) continue;  // ô chạm đầu luôn an toàn
        _mine[r][c] = true;
        ++placed;
    }
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            if (_mine[r][c]) { _adj[r][c] = -1; continue; }
            int n = 0;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc) {
                    int nr = r + dr, nc = c + dc;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                    if (_mine[nr][nc]) ++n;
                }
            _adj[r][c] = (int8_t)n;
        }
    }
    _mines_placed = true;
}

void Minesweeper::revealCell(int r, int c)
{
    if (!_alive || _won) return;
    if (_revealed[r][c] || _flagged[r][c]) return;
    if (!_mines_placed) placeMines(r, c);

    if (_mine[r][c]) {
        _revealed[r][c] = true;
        _alive = false;
        revealAllMines();
        refreshCell(r, c);
        endGame(false);
        return;
    }
    floodReveal(r, c);
    refreshHeader();
    if (_revealed_count >= ROWS * COLS - MINES) { _won = true; endGame(true); }
}

void Minesweeper::floodReveal(int r, int c)
{
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    if (_revealed[r][c] || _flagged[r][c] || _mine[r][c]) return;
    _revealed[r][c] = true;
    ++_revealed_count;
    refreshCell(r, c);
    if (_adj[r][c] != 0) return;  // ô có số: dừng lan
    for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc)
            if (dr || dc) floodReveal(r + dr, c + dc);
}

void Minesweeper::toggleFlag(int r, int c)
{
    if (!_alive || _won || _revealed[r][c]) return;
    _flagged[r][c] = !_flagged[r][c];
    _flags += _flagged[r][c] ? 1 : -1;
    refreshCell(r, c);
    refreshHeader();
}

void Minesweeper::revealAllMines(void)
{
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c)
            if (_mine[r][c]) { _revealed[r][c] = true; refreshCell(r, c); }
}

void Minesweeper::refreshCell(int r, int c)
{
    lv_obj_t *cell = _cell[r][c];
    if (!cell) return;
    lv_obj_t *lab = lv_obj_get_child(cell, 0);

    if (!_revealed[r][c]) {
        if (_flagged[r][c]) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(0xF2994A), 0);
            if (lab) { lv_label_set_text(lab, "!"); lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), 0); }
        } else {
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x24405A), 0);
            if (lab) lv_label_set_text(lab, "");
        }
        return;
    }

    // Đã mở.
    if (_mine[r][c]) {
        lv_obj_set_style_bg_color(cell, lv_color_hex(0xE05C5C), 0);
        if (lab) { lv_label_set_text(lab, "*"); lv_obj_set_style_text_color(lab, lv_color_hex(0x16202B), 0); }
        return;
    }
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x16202B), 0);
    if (lab) {
        if (_adj[r][c] > 0) {
            char t[2] = {(char)('0' + _adj[r][c]), 0};
            lv_label_set_text(lab, t);
            lv_obj_set_style_text_color(lab, lv_color_hex(kNumColor[_adj[r][c]]), 0);
        } else {
            lv_label_set_text(lab, "");
        }
    }
}

void Minesweeper::refreshHeader(void)
{
    if (_mines_label) {
        char t[48];
        snprintf(t, sizeof(t), LV_SYMBOL_WARNING " Mìn: %d", MINES - _flags);
        lv_label_set_text(_mines_label, t);
    }
    if (_time_label) {
        char t[32];
        snprintf(t, sizeof(t), "Giây: %u", (unsigned)_seconds);
        lv_label_set_text(_time_label, t);
    }
}

void Minesweeper::endGame(bool win)
{
    if (_status_label) {
        lv_label_set_text(_status_label,
                          win ? "THẮNG! Đã mở hết ô an toàn. Bấm VÁN MỚI để chơi lại."
                              : "THUA! Trúng mìn. Bấm VÁN MỚI để chơi lại.");
        lv_obj_set_style_text_color(_status_label,
                                    lv_color_hex(win ? 0x3DD66E : 0xFF6B6B), 0);
    }
}

// ---- callbacks ----
void Minesweeper::cellClickedCb(lv_event_t *e)
{
    Minesweeper *app = static_cast<Minesweeper *>(lv_event_get_user_data(e));
    lv_obj_t *cell = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(cell);
    app->revealCell(idx / COLS, idx % COLS);
}

void Minesweeper::cellLongCb(lv_event_t *e)
{
    Minesweeper *app = static_cast<Minesweeper *>(lv_event_get_user_data(e));
    lv_obj_t *cell = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(cell);
    app->toggleFlag(idx / COLS, idx % COLS);
}

void Minesweeper::newGameCb(lv_event_t *e)
{
    Minesweeper *app = static_cast<Minesweeper *>(lv_event_get_user_data(e));
    app->newGame();
}

void Minesweeper::tickCb(lv_timer_t *t)
{
    Minesweeper *app = static_cast<Minesweeper *>(t->user_data);
    if (app->_alive && !app->_won && app->_mines_placed) {
        ++app->_seconds;
        app->refreshHeader();
    }
}
