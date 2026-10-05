/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE  // fopencookie()

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "bsp/esp-bsp.h"
#include "product_audio.h"

static const char *TAG = "product_audio_board";

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;

static bool _is_audio_init = false;
static bool _is_player_init = false;
static int _vloume_intensity = CODEC_DEFAULT_VOLUME;

static audio_player_cb_t audio_idle_callback = NULL;
static void *audio_idle_cb_user_data = NULL;
static char audio_file_path[128];

static esp_err_t audio_file_path_length(const char *file_path, size_t *length)
{
    if (file_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t path_length = strnlen(file_path, sizeof(audio_file_path));
    if (path_length >= sizeof(audio_file_path)) {
        ESP_LOGE(TAG, "Audio file path is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    if (length != NULL) {
        *length = path_length;
    }
    return ESP_OK;
}

static esp_err_t set_audio_file_path(const char *file_path)
{
    size_t path_length = 0;
    esp_err_t ret = audio_file_path_length(file_path, &path_length);
    if (ret != ESP_OK) {
        return ret;
    }

    memcpy(audio_file_path, file_path, path_length + 1);
    return ESP_OK;
}

/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    // Volume saved when muting and restored when unmuting. Restoring volume is necessary
    // as es8311_set_voice_mute(true) results in voice volume (REG32) being set to zero.

    product_audio_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);

    // restore the voice volume upon unmuting
    if (setting == AUDIO_PLAYER_UNMUTE) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity), TAG, "Set Codec volume failed");
    }

    return ESP_OK;
}

static void audio_callback(audio_player_cb_ctx_t *ctx)
{
    if (audio_idle_callback) {
        ctx->user_ctx = audio_idle_cb_user_data;
        audio_idle_callback(ctx);
    }
}

esp_err_t product_audio_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_read(record_dev_handle, audio_buffer, len);
    if (bytes_read != NULL) {
        *bytes_read = (ret == ESP_OK) ? len : 0;
    }
    return ret;
}

esp_err_t product_audio_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_write(play_dev_handle, audio_buffer, len);
    if (bytes_written != NULL) {
        *bytes_written = (ret == ESP_OK) ? len : 0;
    }
    return ret;
}

esp_err_t product_audio_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_close(record_dev_handle);
        ret |= esp_codec_dev_set_in_gain(record_dev_handle, CODEC_DEFAULT_ADC_VOLUME);
    }

    if (play_dev_handle) {
        ret |= esp_codec_dev_open(play_dev_handle, &fs);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_open(record_dev_handle, &fs);
    }
    return ret;
}

esp_err_t product_audio_codec_volume_set(int volume, int *volume_set)
{
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, volume), TAG, "Set Codec volume failed");
    _vloume_intensity = volume;

    ESP_LOGI(TAG, "Setting volume: %d", volume);

    return ESP_OK;
}

// HCC: đặt độ nhạy micro (ES7210) sau khi thiết bị thu đã mở. product_audio_codec_set_fs() đặt gain lúc thiết bị
// đang đóng (giữa close và open) nên có thể không có tác dụng.
esp_err_t product_audio_codec_set_in_gain(float db)
{
    if (record_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_codec_dev_set_in_gain(record_dev_handle, db);
    ESP_LOGI(TAG, "Mic gain %.1f dB -> %d", db, ret);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

int product_audio_codec_volume_get(void)
{
    return _vloume_intensity;
}

esp_err_t product_audio_codec_mute_set(bool enable)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_set_out_mute(play_dev_handle, enable);
    return ret;
}

esp_err_t product_audio_codec_dev_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }

    if (record_dev_handle) {
        ret = esp_codec_dev_close(record_dev_handle);
    }
    return ret;
}

esp_err_t product_audio_codec_dev_resume(void)
{
    return product_audio_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

esp_err_t product_audio_codec_init()
{
    if (_is_audio_init) {
        return ESP_OK;
    }

    play_dev_handle = bsp_audio_codec_speaker_init();
    assert((play_dev_handle) && "play_dev_handle not initialized");

    record_dev_handle = bsp_audio_codec_microphone_init();
    assert((record_dev_handle) && "record_dev_handle not initialized");

    product_audio_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);

    _is_audio_init = true;

    return ESP_OK;
}

esp_err_t product_audio_player_init(void)
{
    if (_is_player_init) {
        return ESP_OK;
    }

    audio_player_config_t config = { .mute_fn = audio_mute_function,
                                     .write_fn = product_audio_i2s_write,
                                     .clk_set_fn = product_audio_codec_set_fs,
                                     .priority = 5
                                   };
    ESP_RETURN_ON_ERROR(audio_player_new(config), TAG, "audio_player_init failed");
    audio_player_callback_register(audio_callback, NULL);

    _is_player_init = true;

    return ESP_OK;
}

esp_err_t product_audio_player_del(void)
{
    _is_player_init = false;

    ESP_RETURN_ON_ERROR(audio_player_delete(), TAG, "audio_player_delete failed");

    return ESP_OK;
}

esp_err_t product_audio_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    file_iterator_instance_t *file_iterator = file_iterator_new(path);
    ESP_RETURN_ON_FALSE(file_iterator, ESP_FAIL, TAG, "file_iterator_new failed, %s", path);

    *ret_instance = file_iterator;

    return ESP_OK;
}

/*
 * HCC: esp-audio-player 1.1.0 không bỏ qua thẻ ID3v2 ở đầu file MP3. File có ảnh bìa (JPEG chứa
 * byte 0xFF 0xE0/0xE1 giống sync word MP3) làm MP3Decode() trả ERR_MP3_INVALID_FRAMEHEADER (-6)
 * mà không tiến con trỏ đọc, decode_mp3() lại tìm đúng chỗ đó -> lặp "mp3: status error -6" mãi,
 * không ra tiếng. Ở đây mở file qua fopencookie() để người chơi nhạc thấy file bắt đầu SAU thẻ
 * ID3v2 (is_mp3() vẫn nhận ra frame MP3 0xFF 0xFB/0xF3/0xF2 ở đầu).
 */
#ifdef __LARGE64_FILES
typedef _off64_t audio_cookie_off_t;
#else
typedef off_t audio_cookie_off_t;
#endif

typedef struct {
    int fd;
    off_t base;  // số byte bỏ qua ở đầu file (thẻ ID3v2)
} audio_skip_cookie_t;

static ssize_t audio_skip_read(void *c, char *buf, size_t n)
{
    return read(((audio_skip_cookie_t *)c)->fd, buf, n);
}

static int audio_skip_seek(void *c, audio_cookie_off_t *off, int whence)
{
    audio_skip_cookie_t *ck = (audio_skip_cookie_t *)c;
    off_t target = (off_t)*off;
    if (whence == SEEK_SET) {
        target += ck->base;
    }
    off_t pos = lseek(ck->fd, target, whence);
    if (pos < 0) {
        return -1;
    }
    if (pos < ck->base) {  // không cho lùi vào trong thẻ ID3
        pos = lseek(ck->fd, ck->base, SEEK_SET);
        if (pos < 0) {
            return -1;
        }
    }
    *off = (audio_cookie_off_t)(pos - ck->base);
    return 0;
}

static int audio_skip_close(void *c)
{
    audio_skip_cookie_t *ck = (audio_skip_cookie_t *)c;
    int ret = close(ck->fd);
    free(ck);
    return ret;
}

// Kích thước thẻ ID3v2 ở đầu file (gồm header 10 byte và footer nếu có), 0 nếu không có thẻ.
static off_t audio_id3v2_size(int fd)
{
    uint8_t h[10];
    if (read(fd, h, sizeof(h)) != (ssize_t)sizeof(h) || memcmp(h, "ID3", 3) != 0 ||
            h[3] == 0xFF || h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80) != 0) {
        return 0;
    }
    off_t size = ((off_t)h[6] << 21) | ((off_t)h[7] << 14) | ((off_t)h[8] << 7) | (off_t)h[9];
    size += 10;
    if (h[5] & 0x10) {  // có footer
        size += 10;
    }
    return size;
}

// Từ vị trí 'from', tìm header frame MP3 mà is_mp3() chấp nhận (0xFF 0xFB / 0xF3 / 0xF2) với
// bitrate và tần số lấy mẫu hợp lệ; bỏ qua byte đệm giữa thẻ ID3 và frame đầu. -1 nếu không thấy.
static off_t audio_find_first_frame(int fd, off_t from)
{
    uint8_t buf[512];
    off_t pos = from;
    while (pos < from + 65536) {
        if (lseek(fd, pos, SEEK_SET) != pos) {
            return -1;
        }
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 4) {
            return -1;
        }
        for (ssize_t i = 0; i + 3 < n; ++i) {
            if (buf[i] == 0xFF && (buf[i + 1] == 0xFB || buf[i + 1] == 0xF3 || buf[i + 1] == 0xF2) &&
                    (buf[i + 2] & 0xF0) != 0xF0 && (buf[i + 2] & 0xF0) != 0x00 && (buf[i + 2] & 0x0C) != 0x0C) {
                return pos + i;
            }
        }
        pos += n - 3;
    }
    return -1;
}

static FILE *audio_open_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    off_t skip = audio_id3v2_size(fd);
    if (skip > 0) {
        off_t first = audio_find_first_frame(fd, skip);
        if (first > 0) {
            skip = first;
        }
    }
    off_t end = lseek(fd, 0, SEEK_END);
    if (skip <= 0 || end < 0 || skip >= end) {
        close(fd);
        return fopen(path, "rb");
    }
    audio_skip_cookie_t *ck = calloc(1, sizeof(*ck));
    if (ck == NULL || lseek(fd, skip, SEEK_SET) != skip) {
        free(ck);
        close(fd);
        return fopen(path, "rb");
    }
    ck->fd = fd;
    ck->base = skip;
    cookie_io_functions_t io = {
        .read = audio_skip_read,
        .write = NULL,
        .seek = audio_skip_seek,
        .close = audio_skip_close,
    };
    FILE *fp = fopencookie(ck, "rb", io);
    if (fp == NULL) {
        audio_skip_close(ck);
        return fopen(path, "rb");
    }
    ESP_LOGI(TAG, "Bỏ qua thẻ ID3v2 %ld byte (ảnh bìa/thông tin bài)", (long)skip);
    return fp;
}

esp_err_t product_audio_player_play_index(file_iterator_instance_t *instance, int index)
{
    ESP_RETURN_ON_FALSE(instance, ESP_FAIL, TAG, "instance is NULL");

    ESP_LOGI(TAG, "play_index(%d)", index);
    char filename[128] = {0};
    int retval = file_iterator_get_full_path_from_index(instance, index, filename, sizeof(filename));
    ESP_RETURN_ON_FALSE(retval != 0, ESP_FAIL, TAG, "file_iterator_get_full_path_from_index failed");
    ESP_RETURN_ON_ERROR(audio_file_path_length(filename, NULL), TAG, "invalid audio file path");

    ESP_LOGI(TAG, "opening file '%s'", filename);
    FILE *fp = audio_open_file(filename);
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", filename);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");
    ESP_RETURN_ON_ERROR(set_audio_file_path(filename), TAG, "invalid audio file path");

    return ESP_OK;
}

esp_err_t product_audio_player_play_file(const char *file_path)
{
    ESP_RETURN_ON_ERROR(audio_file_path_length(file_path, NULL), TAG, "invalid audio file path");
    ESP_LOGI(TAG, "opening file '%s'", file_path);
    FILE *fp = audio_open_file(file_path);
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", file_path);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    ESP_RETURN_ON_ERROR(set_audio_file_path(file_path), TAG, "invalid audio file path");

    return ESP_OK;
}

void product_audio_player_register_callback(audio_player_cb_t cb, void *user_data)
{
    audio_idle_callback = cb;
    audio_idle_cb_user_data = user_data;
}

bool product_audio_player_is_playing_by_path(const char *file_path)
{
    if (audio_file_path_length(file_path, NULL) != ESP_OK) {
        return false;
    }
    return (strcmp(audio_file_path, file_path) == 0);
}

bool product_audio_player_is_playing_by_index(file_iterator_instance_t *instance, int index)
{
    return (index == file_iterator_get_index(instance));
}
