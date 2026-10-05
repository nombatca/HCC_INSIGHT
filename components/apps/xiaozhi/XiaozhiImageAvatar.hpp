/*
 * XiaozhiImageAvatar: avatar nhân vật từ bộ ảnh PNG trên thẻ SD, thư mục /sdcard/xiaozhi/avatar/:
 *   base.png (bắt buộc); tùy chọn: blink1.png (mắt khép hờ), blink2.png (mắt nhắm), talk1..talk4.png (miệng hé
 *   dần -> mở to), smile.png, sad.png, surprised.png, smile_talk1.png, smile_talk2.png (vừa cười vừa nói).
 *   Bộ 7 ảnh cũ (blink.png, talk1.png, talk2.png...) vẫn dùng được.
 *
 * - Mỗi khung ~577 KB PSRAM (370x520 RGB565+alpha), đủ 12 khung ~7 MB.
 * - Nạp ở nền (task trên nhân WORK), giải mã + co giãn "phủ kín" về đúng kích thước vùng avatar,
 *   lưu cache trong PSRAM (RGB565 + alpha). Cache giữ lại sau khi đóng app; lần mở sau chỉ nạp lại khi
 *   kích thước / thời gian sửa của file đổi, hoặc vùng avatar đổi kích thước.
 * - Hoạt hình chỉ đổi nguồn ảnh (lv_img_set_src) giữa các khung đã có trong RAM, không đọc thẻ SD.
 * - Thiếu ảnh tùy chọn / ảnh lỗi: bỏ hiệu ứng đó. Thiếu base.png: ready() = false, app giữ avatar hoạt hình.
 * - Ảnh tùy chọn phải cùng kích thước gốc với base.png (để các khung chồng khít), khác thì bỏ qua.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

class XiaozhiImageAvatar {
public:
    enum class Mode : uint8_t { Idle, Listening, Thinking, Speaking, Sad };

    // Tạo vùng hiển thị (ẩn cho tới khi ảnh sẵn sàng) và bắt đầu nạp ảnh ở nền với kích thước w x h.
    lv_obj_t *create(lv_obj_t *parent, int w, int h);
    void detach(void);

    // true khi base.png đã nạp xong và vùng ảnh đang hiển thị.
    bool ready(void) const { return _shown; }
    // true khi việc nạp đã kết thúc mà không có base.png dùng được (dùng avatar hoạt hình).
    bool unavailable(void) const;

    void setMode(Mode mode) { _mode = mode; }
    void setEmotion(const char *emotion);
    void setSpeakLevel(int level_0_1000) { _speak_target = level_0_1000 / 1000.0f; }
    void setMicLevel(int level_0_1000) { _mic_target = level_0_1000 / 1000.0f; }

    // Gọi định kỳ từ luồng LVGL (timer riêng của lớp đã làm việc này).
    void tick(void);

private:
    static void onTick(lv_timer_t *t);
    static void onDelete(lv_event_t *e);
    int pickFrame(void);

    lv_obj_t *_box = nullptr;
    lv_obj_t *_img = nullptr;
    lv_timer_t *_timer = nullptr;
    bool _shown = false;
    int _w = 0;
    int _h = 0;

    Mode _mode = Mode::Idle;
    int _emotion_frame = -1;  // chỉ số khung cảm xúc (-1: base)
    float _speak = 0;
    float _speak_target = 0;
    float _mic = 0;
    float _mic_target = 0;

    int _cur_frame = -1;
    int _mouth = 0;             // 0 ngậm .. 4 mở to
    uint32_t _mouth_since = 0;  // ms
    uint32_t _t0 = 0;
    uint32_t _next_blink = 0;
    uint32_t _blink_until = 0;
    uint32_t _blink_start = 0;
    uint32_t _last_zoom_ms = 0;
    uint16_t _zoom = 256;
    lv_opa_t _glow = 0;
};
