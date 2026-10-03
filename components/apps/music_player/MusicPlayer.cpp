/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>
#include <vector>

#include "esp_check.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "product_audio.h"

#include "gui_music/lv_demo_music.h"
#include "gui_music/lv_demo_music_main.h"
#include "MusicPlayer.hpp"
#include "hcc_home/HCCUsbDisk.hpp"

#define MUSIC_DIR       BSP_SPIFFS_MOUNT_POINT "/music"   // nhạc nạp sẵn trong flash (build)
#define MUSIC_DIR_SD    BSP_SD_MOUNT_POINT "/music"       // nhạc chép vào thẻ SD
// product_audio giữ đường dẫn file trong buffer 128 byte (kể cả '\0').
#define MUSIC_PATH_MAX  127

using namespace std;

LV_IMG_DECLARE(img_app_music_player);

static const char *TAG = "MusicPlayer";

// File nhạc phát được: .mp3 / .wav. Bỏ file ẩn (".DS_Store", "._Bai.mp3" do macOS tạo trên thẻ).
static bool isPlayableAudio(const char *name)
{
    if (name == nullptr || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    const char *dot = strrchr(name, '.');
    return dot != nullptr && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0);
}

// Quét thư mục, chỉ lấy file nhạc, sắp theo tên. nullptr nếu không mở được / không có bài nào.
// Dựng file_iterator_instance_t trực tiếp (struct công khai) vì file_iterator_new() liệt kê mọi
// entry, kể cả file ẩn và file không phải nhạc.
static file_iterator_instance_t *scanMusicDir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == nullptr) {
        return nullptr;
    }
    std::vector<std::string> names;
    const size_t dir_len = strlen(dir);
    for (struct dirent *e = readdir(d); e != nullptr; e = readdir(d)) {
        if (e->d_type == DT_DIR || !isPlayableAudio(e->d_name)) {
            continue;
        }
        if (dir_len + 1 + strlen(e->d_name) > MUSIC_PATH_MAX) {
            ESP_LOGW(TAG, "Bỏ qua (tên quá dài): %s", e->d_name);
            continue;
        }
        names.emplace_back(e->d_name);
    }
    closedir(d);
    if (names.empty()) {
        return nullptr;
    }
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });

    auto *it = static_cast<file_iterator_instance_t *>(calloc(1, sizeof(file_iterator_instance_t)));
    char **list = static_cast<char **>(calloc(names.size(), sizeof(char *)));
    char *path = strdup(dir);
    if (it == nullptr || list == nullptr || path == nullptr) {
        free(it);
        free(list);
        free(path);
        return nullptr;
    }
    for (size_t i = 0; i < names.size(); ++i) {
        list[i] = strdup(names[i].c_str());
        if (list[i] == nullptr) {
            for (size_t j = 0; j < i; ++j) free(list[j]);
            free(list);
            free(path);
            free(it);
            return nullptr;
        }
    }
    it->count = names.size();
    it->index = 0;
    it->list = list;
    it->directory_path = path;
    return it;
}

static void freeMusicList(file_iterator_instance_t *it)
{
    if (it == nullptr) {
        return;
    }
    for (size_t i = 0; i < it->count; ++i) free(it->list[i]);
    free(it->list);
    free(const_cast<char *>(it->directory_path));
    free(it);
}

// Thẻ SD có thư mục music chứa nhạc thì dùng thẻ (không có thì thử gốc thẻ), không thì nhạc trong flash.
// Quét lại mỗi lần mở app để nhận bài vừa chép vào thẻ.
static file_iterator_instance_t *loadMusicList(const char **source)
{
    if (HCCUsbDisk::state() != HCCUsbDisk::State::Off) {
        // Thẻ đang giao cho máy tính (USB DISK): không được mount song song.
        ESP_LOGW(TAG, "USB DISK đang bật - thẻ SD đang ở máy tính, dùng nhạc trong flash");
    } else {
        const esp_err_t mount = bsp_sdcard_mount();
        if (mount == ESP_OK || mount == ESP_ERR_INVALID_STATE) {  // INVALID_STATE: đã mount nơi khác
            for (const char *dir : {MUSIC_DIR_SD, BSP_SD_MOUNT_POINT}) {
                file_iterator_instance_t *sd = scanMusicDir(dir);
                if (sd != nullptr) {
                    *source = "SD card";
                    ESP_LOGI(TAG, "Nhạc từ thẻ SD: %s (%u bài)", dir, (unsigned)sd->count);
                    return sd;
                }
                ESP_LOGI(TAG, "Không có .mp3/.wav trong %s", dir);
            }
        } else {
            ESP_LOGI(TAG, "Không có thẻ SD (%s) - dùng nhạc trong flash", esp_err_to_name(mount));
        }
    }
    file_iterator_instance_t *flash = scanMusicDir(MUSIC_DIR);
    *source = "Internal";
    if (flash != nullptr) {
        ESP_LOGI(TAG, "Nhạc trong flash: %s (%u bài)", MUSIC_DIR, (unsigned)flash->count);
    } else {
        ESP_LOGE(TAG, "Không tìm thấy nhạc ở %s", MUSIC_DIR);
    }
    return flash;
}

MusicPlayer::MusicPlayer():
    ESP_Brookesia_PhoneApp("Music Player", &img_app_music_player, true), // auto_resize_visual_area
    _file_iterator(NULL)
{
}

MusicPlayer::~MusicPlayer()
{
    freeMusicList(_file_iterator);
}

bool MusicPlayer::run(void)
{
    // Đổi danh sách khi app đang đóng (close() đã dừng nhạc); file đang phát giữ đường dẫn riêng.
    freeMusicList(_file_iterator);
    const char *source = "Internal";
    _file_iterator = loadMusicList(&source);
    if (_file_iterator == nullptr) {
        ESP_LOGE(TAG, "Không có bài nhạc nào để phát");
        return false;
    }
    _lv_demo_music_set_source_label(source);
    lv_demo_music(lv_scr_act(), _file_iterator);

    return true;
}

bool MusicPlayer::pause(void)
{
    _lv_demo_music_exit_pause();

    return true;
}

bool MusicPlayer::back(void)
{
    notifyCoreClosed();

    return true;
}

bool MusicPlayer::close(void)
{
    if (audio_player_pause() != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_pause failed");
        return false;
    }

    return true;
}

bool MusicPlayer::init(void)
{
    if (product_audio_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "Play init with SPIFFS failed");
        return false;
    }

    // Danh sách bài được quét khi mở app (run()), để nhận nhạc mới chép vào thẻ SD.
    return true;
}
