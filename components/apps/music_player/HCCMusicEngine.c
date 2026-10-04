/*
 * HCCMusicEngine: xem HCCMusicEngine.h.
 *
 * Luồng dữ liệu: file (fread) -> esp_audio_simple_dec (tự tách khung, bỏ qua dữ liệu lỗi)
 *                -> PCM 16 bit (chuyển từ 24/32 bit nếu cần, tối đa 2 kênh)
 *                -> product_audio_i2s_write (chặn theo tốc độ phát, nên không cần tự canh giờ).
 * WAV PCM không qua bộ giải mã: đọc thẳng khối data để tua chính xác theo byte.
 * Vị trí phát = điểm tua gần nhất + số mẫu đã xuất / tần số lấy mẫu.
 */
#include "HCCMusicEngine.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include "hcc_cores.h"
#include "product_audio.h"

static const char *TAG = "HCCMusic";

#define ME_IN_SIZE        4608   // chia hết cho mọi block_align WAV 16/24/32 bit mono/stereo
#define ME_OUT_INIT_SIZE  8192
#define ME_PATH_MAX       256
#define ME_MAX_ERRORS     64     // lỗi giải mã liên tiếp không ra âm thanh -> bỏ bài

typedef enum {
    CMD_PLAY,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_SEEK,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t ms;
    uint32_t play_id;
    char path[ME_PATH_MAX];
} cmd_t;

static QueueHandle_t s_queue;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static hcc_me_status_t s_status;          // bảo vệ bởi s_lock
static uint32_t s_next_play_id;

// Chỉ task phát dùng các biến dưới đây.
static FILE *s_file;
static hcc_audio_meta_t s_meta;
static esp_audio_simple_dec_handle_t s_dec;
static bool s_native_wav;
static uint8_t *s_in;
static uint8_t *s_raw_ptr;
static uint32_t s_raw_len;
static bool s_eos;
static uint8_t *s_out;
static uint32_t s_out_cap;
static uint8_t *s_pcm16;
static uint32_t s_pcm16_cap;
static int64_t s_read_pos;
static uint32_t s_base_ms;
static uint64_t s_frames_out;
static uint32_t s_rate;
static uint8_t s_ch;
static uint8_t s_bits;
static uint32_t s_cfg_rate;
static uint8_t s_cfg_ch;
static int s_errors;
static uint32_t s_cur_play_id;
static bool s_playing;     // trạng thái thật của task (s_status.state có thể đổi trước từ luồng giao diện)
static bool s_paused;

static void set_state(hcc_me_state_t st, const char *err)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = st;
    if (err != NULL) {
        snprintf(s_status.error, sizeof(s_status.error), "%s", err);
    } else if (st != HCC_ME_ERROR) {
        s_status.error[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

static void publish_position(void)
{
    uint32_t pos = s_base_ms;
    if (s_rate > 0) {
        pos += (uint32_t)(s_frames_out * 1000 / s_rate);
    }
    portENTER_CRITICAL(&s_lock);
    s_status.pos_ms = pos;
    s_status.dur_ms = s_meta.duration_ms;
    s_status.bitrate = s_meta.bitrate;
    portEXIT_CRITICAL(&s_lock);
}

static esp_audio_simple_dec_type_t dec_type_for(hcc_audio_fmt_t fmt)
{
    switch (fmt) {
    case HCC_AUDIO_FMT_MP3: return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    case HCC_AUDIO_FMT_AAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case HCC_AUDIO_FMT_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case HCC_AUDIO_FMT_WAV: return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    case HCC_AUDIO_FMT_M4A: return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case HCC_AUDIO_FMT_OGG: return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    default: return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

static bool open_decoder(void)
{
    if (s_dec != NULL) {
        esp_audio_simple_dec_close(s_dec);
        s_dec = NULL;
    }
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = dec_type_for(s_meta.fmt),
    };
    if (cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        return false;
    }
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &s_dec);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Mở bộ giải mã %s lỗi %d", hcc_audio_fmt_name(s_meta.fmt), ret);
        s_dec = NULL;
        return false;
    }
    return true;
}

static void silence_and_mute(void)
{
    // Đẩy ~40 ms im lặng để DMA I2S không lặp lại đoạn âm thanh cuối, rồi tắt tiếng.
    if (s_cfg_rate > 0 && s_pcm16 != NULL) {
        const uint32_t bytes = s_cfg_rate / 25 * s_cfg_ch * 2;
        uint32_t left = bytes;
        memset(s_pcm16, 0, s_pcm16_cap);
        while (left > 0) {
            const uint32_t n = left < s_pcm16_cap ? left : s_pcm16_cap;
            size_t w = 0;
            product_audio_i2s_write(s_pcm16, n, &w, 100);
            left -= n;
        }
    }
    product_audio_codec_mute_set(true);
}

static void close_track(void)
{
    s_playing = false;
    s_paused = false;
    if (s_dec != NULL) {
        esp_audio_simple_dec_close(s_dec);
        s_dec = NULL;
    }
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    s_raw_len = 0;
    s_eos = false;
}

// Đặt vị trí đọc trong file và xoá trạng thái giải mã (dùng khi bắt đầu bài và khi tua).
static bool position_at(uint32_t ms)
{
    int64_t off = s_meta.audio_start;
    uint32_t base = 0;
    if (ms > 0 && s_meta.seekable) {
        off = hcc_audio_meta_seek_offset(&s_meta, ms);
        base = ms;
    }
    if (fseek(s_file, (long)off, SEEK_SET) != 0) {
        return false;
    }
    s_read_pos = off;
    s_raw_len = 0;
    s_eos = false;
    s_base_ms = base;
    s_frames_out = 0;
    s_errors = 0;
    if (!s_native_wav && !open_decoder()) {
        return false;
    }
    return true;
}

static bool configure_output(uint32_t rate, uint8_t ch, uint8_t bits)
{
    if (rate == 0 || ch == 0 || (bits != 16 && bits != 24 && bits != 32)) {
        ESP_LOGE(TAG, "Định dạng PCM không hỗ trợ: %lu Hz, %u kênh, %u bit", (unsigned long)rate, ch, bits);
        return false;
    }
    s_rate = rate;
    s_ch = ch;
    s_bits = bits;
    const uint8_t out_ch = ch >= 2 ? 2 : 1;
    if (rate != s_cfg_rate || out_ch != s_cfg_ch) {
        esp_err_t err = product_audio_codec_set_fs(rate, 16, out_ch == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cấu hình codec %lu Hz lỗi %s", (unsigned long)rate, esp_err_to_name(err));
            return false;
        }
        s_cfg_rate = rate;
        s_cfg_ch = out_ch;
        product_audio_codec_volume_set(product_audio_codec_volume_get(), NULL);
    }
    product_audio_codec_mute_set(false);
    portENTER_CRITICAL(&s_lock);
    s_status.sample_rate = rate;
    s_status.channels = ch;
    s_status.bits = bits;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Phát %s: %lu Hz, %u kênh, %u bit", hcc_audio_fmt_name(s_meta.fmt), (unsigned long)rate, ch, bits);
    return true;
}

// Xuất PCM: chuyển về 16 bit, tối đa 2 kênh (lấy 2 kênh đầu nếu nhiều hơn).
static void write_pcm(const uint8_t *data, uint32_t len)
{
    const uint32_t bps = s_bits / 8;
    const uint32_t frame_bytes = bps * s_ch;
    if (frame_bytes == 0) {
        return;
    }
    const uint32_t frames = len / frame_bytes;
    const uint8_t out_ch = s_ch >= 2 ? 2 : 1;
    const uint8_t *src = data;
    uint32_t out_len = frames * out_ch * 2;
    if (s_bits != 16 || s_ch > 2) {
        if (out_len > s_pcm16_cap) {
            uint8_t *nb = heap_caps_realloc(s_pcm16, out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb == NULL) {
                return;
            }
            s_pcm16 = nb;
            s_pcm16_cap = out_len;
        }
        int16_t *dst = (int16_t *)s_pcm16;
        for (uint32_t f = 0; f < frames; ++f) {
            const uint8_t *fr = data + f * frame_bytes;
            for (uint8_t c = 0; c < out_ch; ++c) {
                const uint8_t *s = fr + c * bps;
                // Little-endian: 2 byte cao nhất của mẫu 24/32 bit là mẫu 16 bit.
                *dst++ = (int16_t)(s[bps - 2] | (s[bps - 1] << 8));
            }
        }
        src = s_pcm16;
    } else {
        out_len = frames * frame_bytes;
    }
    size_t written = 0;
    product_audio_i2s_write((void *)src, out_len, &written, portMAX_DELAY);
    s_frames_out += frames;
}

static void finish_track(void)
{
    publish_position();
    close_track();
    silence_and_mute();
    portENTER_CRITICAL(&s_lock);
    s_status.state = HCC_ME_IDLE;
    s_status.finished_id = s_cur_play_id;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Hết bài (id %lu)", (unsigned long)s_cur_play_id);
}

static void fail_track(const char *why)
{
    ESP_LOGE(TAG, "%s", why);
    close_track();
    silence_and_mute();
    portENTER_CRITICAL(&s_lock);
    s_status.state = HCC_ME_ERROR;
    snprintf(s_status.error, sizeof(s_status.error), "%s", why);
    s_status.finished_id = s_cur_play_id;  // giao diện sang bài kế tiếp
    portEXIT_CRITICAL(&s_lock);
}

static void start_track(const cmd_t *c)
{
    close_track();
    s_cur_play_id = c->play_id;
    s_rate = 0;
    // App khác (Xiaozhi, video) có thể đã đổi tần số codec: luôn cấu hình lại ở bài mới.
    s_cfg_rate = 0;
    s_cfg_ch = 0;
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, offsetof(hcc_me_status_t, play_id));
    s_status.play_id = c->play_id;
    s_status.error[0] = '\0';
    portEXIT_CRITICAL(&s_lock);

    if (!hcc_audio_meta_read(c->path, &s_meta) || s_meta.fmt == HCC_AUDIO_FMT_UNKNOWN) {
        fail_track("Không mở được file nhạc");
        return;
    }
    s_native_wav = s_meta.fmt == HCC_AUDIO_FMT_WAV && s_meta.seekable;
    portENTER_CRITICAL(&s_lock);
    s_status.fmt = s_meta.fmt;
    s_status.seekable = s_meta.seekable;
    s_status.dur_ms = s_meta.duration_ms;
    s_status.bitrate = s_meta.bitrate;
    portEXIT_CRITICAL(&s_lock);

    s_file = fopen(c->path, "rb");
    if (s_file == NULL) {
        fail_track("Không mở được file nhạc");
        return;
    }
    setvbuf(s_file, NULL, _IOFBF, 16 * 1024);
    if (!position_at(c->ms)) {
        fail_track("Không khởi tạo được bộ giải mã");
        return;
    }
    if (s_native_wav && !configure_output(s_meta.sample_rate, s_meta.channels, s_meta.bits)) {
        fail_track("WAV: định dạng không hỗ trợ");
        return;
    }
    // Bộ phát cũ (esp-audio-player, dùng cho video) có thể còn giữ I2S: dừng nó.
    audio_player_stop();
    s_playing = true;
    set_state(HCC_ME_PLAYING, NULL);
    publish_position();
}

// Một bước phát: đọc thêm dữ liệu nếu cần, giải mã một khung, xuất ra loa.
static void step(void)
{
    if (s_raw_len == 0) {
        if (s_eos) {
            if (s_dec != NULL) {
                // Báo hết dữ liệu để bộ giải mã (vd FLAC) xả phần còn giữ.
                esp_audio_simple_dec_raw_t raw = {.buffer = s_in, .len = 0, .eos = true};
                esp_audio_simple_dec_out_t out = {.buffer = s_out, .len = s_out_cap};
                if (esp_audio_simple_dec_process(s_dec, &raw, &out) == ESP_AUDIO_ERR_OK && out.decoded_size > 0 &&
                        s_rate > 0) {
                    write_pcm(s_out, out.decoded_size);
                }
            }
            finish_track();
            return;
        }
        uint32_t want = ME_IN_SIZE;
        if (s_meta.audio_end > 0 && s_read_pos + want > s_meta.audio_end) {
            want = (uint32_t)(s_meta.audio_end - s_read_pos);
        }
        const size_t n = want > 0 ? fread(s_in, 1, want, s_file) : 0;
        s_read_pos += (int64_t)n;
        s_raw_ptr = s_in;
        s_raw_len = (uint32_t)n;
        s_eos = n < ME_IN_SIZE;
        if (n == 0) {
            return;  // vòng sau xử lý eos
        }
    }

    if (s_native_wav) {
        const uint32_t len = s_raw_len - (s_raw_len % s_meta.block_align);
        if (len > 0) {
            write_pcm(s_raw_ptr, len);
        }
        s_raw_len = 0;
        publish_position();
        return;
    }

    esp_audio_simple_dec_raw_t raw = {.buffer = s_raw_ptr, .len = s_raw_len, .eos = s_eos};
    esp_audio_simple_dec_out_t out = {.buffer = s_out, .len = s_out_cap};
    const esp_audio_err_t ret = esp_audio_simple_dec_process(s_dec, &raw, &out);
    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        uint8_t *nb = heap_caps_realloc(s_out, out.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            fail_track("Hết bộ nhớ khi giải mã");
            return;
        }
        s_out = nb;
        s_out_cap = out.needed_size;
        return;  // giải mã lại khung này với bộ đệm lớn hơn
    }
    uint32_t consumed = raw.consumed;
    const bool soft = ret == ESP_AUDIO_ERR_OK || ret == ESP_AUDIO_ERR_CONTINUE || ret == ESP_AUDIO_ERR_DATA_LACK;
    if (!soft || (consumed == 0 && out.decoded_size == 0)) {
        // Lỗi, hoặc bộ giải mã không tiến được: đếm, quá ngưỡng thì bỏ bài (tránh treo như bản cũ).
        if (++s_errors >= ME_MAX_ERRORS) {
            ESP_LOGE(TAG, "Giải mã lỗi %d (%d lần liên tiếp)", ret, s_errors);
            fail_track("Không giải mã được file này (định dạng/mã hoá không hỗ trợ)");
            return;
        }
        if (!soft && consumed == 0) {
            consumed = 1;  // bỏ 1 byte để bộ tách khung tìm khung kế tiếp
        }
    }
    if (consumed > s_raw_len) {
        consumed = s_raw_len;
    }
    s_raw_ptr += consumed;
    s_raw_len -= consumed;

    if (out.decoded_size > 0) {
        s_errors = 0;
        if (s_rate == 0) {
            // Khung đầu tiên: lấy định dạng PCM thật từ bộ giải mã rồi cấu hình codec.
            esp_audio_simple_dec_info_t info = {0};
            esp_audio_simple_dec_get_info(s_dec, &info);
            if (!configure_output(info.sample_rate, info.channel, info.bits_per_sample)) {
                fail_track("Định dạng âm thanh không hỗ trợ");
                return;
            }
            if (info.bitrate > 0) {
                hcc_audio_meta_update_bitrate(&s_meta, info.bitrate);  // MP3/AAC CBR không có khung Xing
            }
        }
        write_pcm(s_out, out.decoded_size);
        publish_position();
    }
}

static void handle(const cmd_t *c)
{
    switch (c->type) {
    case CMD_PLAY:
        start_track(c);
        break;
    case CMD_PAUSE:
        if (s_playing) {
            s_playing = false;
            s_paused = true;
            silence_and_mute();
            set_state(HCC_ME_PAUSED, NULL);
        }
        break;
    case CMD_RESUME:
        if (s_paused && s_file != NULL) {
            s_paused = false;
            s_playing = true;
            product_audio_codec_mute_set(false);
            set_state(HCC_ME_PLAYING, NULL);
        }
        break;
    case CMD_STOP:
        if (s_file != NULL) {
            close_track();
            silence_and_mute();
        }
        set_state(HCC_ME_IDLE, NULL);
        break;
    case CMD_SEEK:
        if (s_file != NULL && s_meta.seekable) {
            uint32_t ms = c->ms;
            if (s_meta.duration_ms > 0 && ms >= s_meta.duration_ms) {
                ms = s_meta.duration_ms > 1000 ? s_meta.duration_ms - 1000 : 0;
            }
            if (!position_at(ms)) {
                fail_track("Tua lỗi");
                break;
            }
            publish_position();
        }
        break;
    }
}

static void engine_task(void *arg)
{
    (void)arg;
    cmd_t c;
    for (;;) {
        const TickType_t wait = s_playing ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_queue, &c, wait) == pdTRUE) {
            handle(&c);
            continue;  // xử lý hết lệnh đang chờ trước khi phát tiếp
        }
        if (s_playing && s_file != NULL) {
            step();
        }
    }
}

esp_err_t hcc_music_engine_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    s_in = heap_caps_malloc(ME_IN_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out = heap_caps_malloc(ME_OUT_INIT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pcm16 = heap_caps_malloc(ME_OUT_INIT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_queue = xQueueCreate(4, sizeof(cmd_t));
    if (s_in == NULL || s_out == NULL || s_pcm16 == NULL || s_queue == NULL) {
        ESP_LOGE(TAG, "Không đủ bộ nhớ cho bộ phát");
        return ESP_ERR_NO_MEM;
    }
    s_out_cap = ME_OUT_INIT_SIZE;
    s_pcm16_cap = ME_OUT_INIT_SIZE;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    // Bộ giải mã AAC/FLAC dùng khá nhiều stack.
    if (xTaskCreatePinnedToCore(engine_task, "hcc_music", 12 * 1024, NULL, 6, NULL, HCC_CORE_WORK) != pdPASS) {
        ESP_LOGE(TAG, "Không tạo được task phát nhạc");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void send(cmd_t *c)
{
    if (s_queue == NULL) {
        return;
    }
    // Lệnh mới quan trọng hơn: nếu hàng đợi đầy (người dùng bấm liên tục) thì chờ ngắn.
    xQueueSend(s_queue, c, pdMS_TO_TICKS(200));
}

esp_err_t hcc_music_engine_play(const char *path, uint32_t start_ms, uint32_t *play_id)
{
    if (s_queue == NULL || path == NULL || strlen(path) >= ME_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_t c = {.type = CMD_PLAY, .ms = start_ms};
    snprintf(c.path, sizeof(c.path), "%s", path);
    portENTER_CRITICAL(&s_lock);
    c.play_id = ++s_next_play_id;
    // Giao diện thấy ngay bài mới, không lẫn trạng thái "hết bài" của bài cũ.
    s_status.play_id = c.play_id;
    s_status.state = HCC_ME_PLAYING;
    s_status.pos_ms = start_ms;
    portEXIT_CRITICAL(&s_lock);
    if (play_id != NULL) {
        *play_id = c.play_id;
    }
    send(&c);
    return ESP_OK;
}

void hcc_music_engine_pause(void)
{
    cmd_t c = {.type = CMD_PAUSE};
    send(&c);
}

void hcc_music_engine_resume(void)
{
    cmd_t c = {.type = CMD_RESUME};
    send(&c);
}

void hcc_music_engine_stop(void)
{
    cmd_t c = {.type = CMD_STOP};
    send(&c);
}

void hcc_music_engine_seek(uint32_t ms)
{
    cmd_t c = {.type = CMD_SEEK, .ms = ms};
    send(&c);
}

void hcc_music_engine_get_status(hcc_me_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

int hcc_music_engine_get_volume(void)
{
    return product_audio_codec_volume_get();
}

void hcc_music_engine_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    product_audio_codec_volume_set(volume, NULL);
}
