/*
 * XiaozhiAvatar: nhân vật cô gái hoạt hình cho app Xiaozhi AI, vẽ hoàn toàn bằng hình cơ bản của LVGL
 * (không dùng ảnh): thở, chớp mắt, đuôi tóc đung đưa, nhìn quanh khi rảnh, sóng âm khi nghe,
 * "..." khi đang nghĩ, miệng mấp máy theo mức âm khi nói, nét mặt theo cảm xúc từ máy chủ.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

class XiaozhiAvatar {
public:
    enum class Mode : uint8_t { Idle, Listening, Thinking, Speaking, Sad };

    // Tạo vùng vẽ trong parent (kích thước do người gọi đặt). Timer khung hình tạo ở đây.
    lv_obj_t *create(lv_obj_t *parent);
    void detach(void);   // gọi khi màn hình bị xoá

    void setMode(Mode mode) { _mode = mode; }
    void setEmotion(const char *emotion);
    void setSpeakLevel(int level_0_1000) { _speak_target = level_0_1000 / 1000.0f; }
    void setMicLevel(int level_0_1000) { _mic_target = level_0_1000 / 1000.0f; }

private:
    enum class Eyes : uint8_t { Normal, Happy, Closed, Wink, Sleepy, Wide };
    enum class Brow : uint8_t { Neutral, Angry, Sad, Raised };
    enum class Mouth : uint8_t { Smile, Flat, Sad, O, BigOpen, Tongue, Kiss };

    struct Face {
        Eyes eyes;
        Brow brow;
        Mouth mouth;
        uint8_t blush;
        bool tears;
    };

    static void onDraw(lv_event_t *e);
    static void onDelete(lv_event_t *e);
    static void onTick(lv_timer_t *t);
    void tick(void);
    void draw(lv_draw_ctx_t *ctx);

    lv_obj_t *_obj = nullptr;
    lv_timer_t *_timer = nullptr;
    Mode _mode = Mode::Idle;
    Face _face = {Eyes::Normal, Brow::Neutral, Mouth::Smile, 70, false};

    // trạng thái hoạt hình
    uint32_t _t0 = 0;
    float _t = 0;               // giây
    float _speak = 0;
    float _speak_target = 0;
    float _mic = 0;
    float _mic_target = 0;
    float _gx = 0, _gy = 0;     // hướng nhìn hiện tại
    float _tgx = 0, _tgy = 0;   // hướng nhìn đích
    float _next_gaze = 0;
    float _blink_start = -10;
    float _next_blink = 2;
};
