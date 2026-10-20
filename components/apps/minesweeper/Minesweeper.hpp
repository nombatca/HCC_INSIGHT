#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

// Game Dò Mìn (Minesweeper) cho HCC INSIGHT - app Brookesia, màn 1024x600.
//   Chạm ô: mở ô. Giữ ô (long press): cắm / gỡ cờ.
//   Mở trúng mìn: thua. Mở hết ô không mìn: thắng.
class Minesweeper : public ESP_Brookesia_PhoneApp {
public:
    Minesweeper();
    ~Minesweeper();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    static constexpr int COLS = 9;
    static constexpr int ROWS = 9;
    static constexpr int MINES = 10;

private:
    void buildUi(lv_obj_t *parent);
    void newGame(void);
    void placeMines(int first_r, int first_c);
    void revealCell(int r, int c);
    void floodReveal(int r, int c);
    void toggleFlag(int r, int c);
    void revealAllMines(void);
    void refreshCell(int r, int c);
    void refreshHeader(void);
    void endGame(bool win);

    static void cellClickedCb(lv_event_t *e);
    static void cellLongCb(lv_event_t *e);
    static void newGameCb(lv_event_t *e);
    static void tickCb(lv_timer_t *t);

    // Trạng thái bàn chơi.
    bool _mine[ROWS][COLS];
    bool _revealed[ROWS][COLS];
    bool _flagged[ROWS][COLS];
    int8_t _adj[ROWS][COLS];
    lv_obj_t *_cell[ROWS][COLS];

    bool _mines_placed = false;   // mìn chỉ gieo sau cú chạm đầu (ô đầu luôn an toàn)
    bool _alive = true;
    bool _won = false;
    int _revealed_count = 0;
    int _flags = 0;
    uint32_t _seconds = 0;

    lv_obj_t *_mines_label = nullptr;
    lv_obj_t *_time_label = nullptr;
    lv_obj_t *_status_label = nullptr;
    lv_obj_t *_face_label = nullptr;
    lv_timer_t *_timer = nullptr;
};
