/*
 * HCCMusicEngine: bộ phát nhạc cho app Music, dùng bộ giải mã esp_audio_codec của Espressif
 * (MP3, AAC, FLAC, M4A, OGG/Opus; WAV PCM đọc thẳng). Xuất âm qua product_audio (esp_codec_dev).
 *
 * Chạy trên một task riêng (nhân HCC_CORE_WORK). Mọi hàm điều khiển chỉ gửi lệnh vào hàng đợi,
 * gọi được từ luồng LVGL. Trạng thái đọc qua hcc_music_engine_get_status().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "HCCAudioMeta.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HCC_ME_IDLE = 0,
    HCC_ME_PLAYING,
    HCC_ME_PAUSED,
    HCC_ME_ERROR,
} hcc_me_state_t;

typedef struct {
    hcc_me_state_t state;
    uint32_t pos_ms;
    uint32_t dur_ms;          // 0 = chưa biết
    bool seekable;
    hcc_audio_fmt_t fmt;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    uint32_t bitrate;
    uint32_t play_id;         // tăng mỗi lần hcc_music_engine_play()
    uint32_t finished_id;     // = play_id của bài vừa phát HẾT (để giao diện tự sang bài)
    char error[96];
} hcc_me_status_t;

esp_err_t hcc_music_engine_init(void);

// Phát file từ thời điểm start_ms (bỏ qua nếu định dạng không tua được). Trả về play_id qua *play_id.
esp_err_t hcc_music_engine_play(const char *path, uint32_t start_ms, uint32_t *play_id);
void hcc_music_engine_pause(void);
void hcc_music_engine_resume(void);
void hcc_music_engine_stop(void);
void hcc_music_engine_seek(uint32_t ms);

void hcc_music_engine_get_status(hcc_me_status_t *status);

int hcc_music_engine_get_volume(void);
void hcc_music_engine_set_volume(int volume);

#ifdef __cplusplus
}
#endif
