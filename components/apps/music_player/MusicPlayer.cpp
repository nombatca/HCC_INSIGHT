/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HCC: viết lại toàn bộ. Phát bằng HCCMusicEngine (esp_audio_codec: MP3/AAC/FLAC/M4A/OGG/WAV),
 * giao diện riêng: danh sách bài, thời gian thật / tổng thời lượng, tua, âm lượng, ngẫu nhiên, lặp.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>
#include <vector>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_random.h"
#include "product_audio.h"

#include "HCCMusicEngine.h"
#include "MusicPlayer.hpp"
#include "hcc_home/HCCUsbDisk.hpp"

#define MUSIC_DIR       BSP_SPIFFS_MOUNT_POINT "/music"   // nhạc nạp sẵn trong flash (build)
#define MUSIC_DIR_SD    BSP_SD_MOUNT_POINT "/music"       // nhạc chép vào thẻ SD
#define MUSIC_PATH_MAX  250                               // HCCMusicEngine giữ đường dẫn 256 byte
#define MUSIC_MAX_FILES 1000

LV_IMG_DECLARE(img_app_music_player);

static const char *TAG = "MusicPlayer";

namespace {

constexpr uint32_t COLOR_BG = 0x0F141A;
constexpr uint32_t COLOR_PANEL = 0x18212B;
constexpr uint32_t COLOR_ACCENT = 0x2478D0;
constexpr uint32_t COLOR_TEXT = 0xE8EEF4;
constexpr uint32_t COLOR_MUTED = 0x8A97A3;
constexpr uint32_t COLOR_ERROR = 0xFF6B6B;

// File nhạc phát được. Bỏ file ẩn (".DS_Store", "._Bai.mp3" do macOS tạo trên thẻ).
bool isPlayableAudio(const char *name)
{
    if (name == nullptr || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    return hcc_audio_fmt_from_name(name) != HCC_AUDIO_FMT_UNKNOWN;
}

// Quét thư mục, chỉ lấy file nhạc, sắp theo tên.
std::vector<std::string> scanMusicDir(const char *dir)
{
    std::vector<std::string> names;
    DIR *d = opendir(dir);
    if (d == nullptr) {
        return names;
    }
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
        if (names.size() >= MUSIC_MAX_FILES) {
            ESP_LOGW(TAG, "Quá %d bài, chỉ lấy %d bài đầu", MUSIC_MAX_FILES, MUSIC_MAX_FILES);
            break;
        }
    }
    closedir(d);
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    return names;
}

void formatTime(char *buf, size_t len, uint32_t ms)
{
    const uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60));
    } else {
        snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
}

lv_obj_t *makePanel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 16, 0);
    lv_obj_set_style_pad_all(p, 16, 0);
    return p;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *makeRoundButton(lv_obj_t *parent, int size, uint32_t bg, const char *symbol, const lv_font_t *font,
                          lv_event_cb_t cb, void *user, lv_obj_t **label_out = nullptr)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = makeLabel(b, font, COLOR_TEXT, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    if (label_out != nullptr) {
        *label_out = l;
    }
    return b;
}

lv_obj_t *makeSlider(lv_obj_t *parent)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2C3845), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 16);
    return s;
}

}  // namespace

MusicPlayer::MusicPlayer():
    ESP_Brookesia_PhoneApp("Music Player", &img_app_music_player, true) // auto_resize_visual_area
{
}

MusicPlayer::~MusicPlayer()
{
}

bool MusicPlayer::init(void)
{
    // Bộ phát cũ (esp-audio-player) vẫn được video player dùng cho nhạc nền: giữ khởi tạo như trước.
    if (product_audio_player_init() != ESP_OK) {
        ESP_LOGW(TAG, "product_audio_player_init lỗi");
    }
    if (hcc_music_engine_init() != ESP_OK) {
        ESP_LOGE(TAG, "Không khởi tạo được bộ phát nhạc");
        return false;
    }
    return true;
}

// Thẻ SD có nhạc (thư mục music, không có thì gốc thẻ) thì dùng thẻ, không thì nhạc trong flash.
// Quét lại mỗi lần mở app để nhận bài vừa chép vào thẻ.
bool MusicPlayer::run(void)
{
    _files.clear();
    _dir.clear();
    _current = -1;
    _play_id = 0;
    _handled_finish = 0;
    _error_streak = 0;
    _user_paused = false;

    if (HCCUsbDisk::state() != HCCUsbDisk::State::Off) {
        // Thẻ đang giao cho máy tính (USB DISK): không được mount song song.
        ESP_LOGW(TAG, "USB DISK đang bật - thẻ SD đang ở máy tính, dùng nhạc trong flash");
    } else {
        const esp_err_t mount = bsp_sdcard_mount();
        if (mount == ESP_OK || mount == ESP_ERR_INVALID_STATE) {  // INVALID_STATE: đã mount nơi khác
            for (const char *dir : {MUSIC_DIR_SD, BSP_SD_MOUNT_POINT}) {
                _files = scanMusicDir(dir);
                if (!_files.empty()) {
                    _dir = dir;
                    _source = "Thẻ SD";
                    break;
                }
                ESP_LOGI(TAG, "Không có file nhạc trong %s", dir);
            }
        } else {
            ESP_LOGI(TAG, "Không có thẻ SD (%s) - dùng nhạc trong flash", esp_err_to_name(mount));
        }
    }
    if (_files.empty()) {
        _files = scanMusicDir(MUSIC_DIR);
        _dir = MUSIC_DIR;
        _source = "Bộ nhớ trong";
    }
    ESP_LOGI(TAG, "Nhạc: %s (%u bài)", _dir.c_str(), (unsigned)_files.size());

    buildUi(lv_scr_act());
    _timer = lv_timer_create(onTimer, 200, this);
    if (!_files.empty()) {
        _current = 0;
        refreshList();
    }
    refresh();
    return true;
}

bool MusicPlayer::pause(void)
{
    // App xuống nền (về màn hình chính): tạm dừng nhạc như bản cũ.
    hcc_music_engine_pause();
    return true;
}

bool MusicPlayer::back(void)
{
    notifyCoreClosed();
    return true;
}

bool MusicPlayer::close(void)
{
    hcc_music_engine_stop();
    // Brookesia tự xoá màn hình và timer tạo trong run(); chỉ bỏ con trỏ.
    detachUi();
    return true;
}

void MusicPlayer::detachUi(void)
{
    _root = nullptr;
    _list = nullptr;
    _list_title = nullptr;
    _title = nullptr;
    _info = nullptr;
    _status = nullptr;
    _seek = nullptr;
    _pos = nullptr;
    _dur = nullptr;
    _btn_play = nullptr;
    _btn_play_label = nullptr;
    _btn_shuffle = nullptr;
    _btn_repeat = nullptr;
    _btn_repeat_label = nullptr;
    _volume = nullptr;
    _volume_label = nullptr;
    _timer = nullptr;
    _list_highlight = -1;
}

void MusicPlayer::onRootDeleted(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app != nullptr) {
        app->detachUi();
    }
}

std::string MusicPlayer::trackTitle(int index) const
{
    if (index < 0 || index >= (int)_files.size()) {
        return "";
    }
    std::string name = _files[index];
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        name.erase(dot);
    }
    return name;
}

void MusicPlayer::buildUi(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    _root = lv_obj_create(screen);
    lv_obj_remove_style_all(_root);
    lv_obj_set_size(_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(_root, 16, 0);
    lv_obj_set_style_pad_gap(_root, 16, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_root, onRootDeleted, LV_EVENT_DELETE, this);

    /* ----- Danh sách bài ----- */
    lv_obj_t *left = makePanel(_root);
    lv_obj_set_size(left, LV_PCT(40), LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(left, 10, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    char buf[96];
    snprintf(buf, sizeof(buf), LV_SYMBOL_LIST "  %u bài  ·  %s", (unsigned)_files.size(), _source.c_str());
    _list_title = makeLabel(left, &lv_font_be_vietnam_18, COLOR_MUTED, buf);

    _list = lv_list_create(left);
    lv_obj_set_width(_list, LV_PCT(100));
    lv_obj_set_flex_grow(_list, 1);
    lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_pad_row(_list, 4, 0);
    for (size_t i = 0; i < _files.size(); ++i) {
        lv_obj_t *btn = lv_list_add_btn(_list, LV_SYMBOL_AUDIO, trackTitle((int)i).c_str());
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(btn, &lv_font_be_vietnam_18, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_t *label = lv_obj_get_child(btn, 1);
        if (label != nullptr) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        }
        lv_obj_add_event_cb(btn, onListClicked, LV_EVENT_CLICKED, this);
    }
    if (_files.empty()) {
        lv_obj_t *empty = makeLabel(_list, &lv_font_be_vietnam_18, COLOR_MUTED,
                                    "Chưa có bài nào.\n\nChép file .mp3 / .m4a / .flac / .wav / .aac / .ogg vào "
                                    "thư mục music trên thẻ SD (dùng USB DISK trong HCC INSIGHT).");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    }

    /* ----- Đang phát ----- */
    lv_obj_t *right = makePanel(_root);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(right, 28, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *art = lv_obj_create(right);
    lv_obj_set_size(art, 120, 120);
    lv_obj_set_style_radius(art, 24, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(makeLabel(art, &lv_font_be_vietnam_48, COLOR_TEXT, LV_SYMBOL_AUDIO));

    _title = makeLabel(right, &lv_font_be_vietnam_28, COLOR_TEXT, "");
    lv_obj_set_width(_title, LV_PCT(100));
    lv_label_set_long_mode(_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(_title, LV_TEXT_ALIGN_CENTER, 0);

    _info = makeLabel(right, &lv_font_be_vietnam_16, COLOR_MUTED, "");
    lv_obj_set_width(_info, LV_PCT(100));
    lv_obj_set_style_text_align(_info, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *seek_box = lv_obj_create(right);
    lv_obj_remove_style_all(seek_box);
    lv_obj_set_size(seek_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seek_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(seek_box, 10, 0);
    lv_obj_set_style_pad_top(seek_box, 8, 0);
    _seek = makeSlider(seek_box);
    lv_obj_set_width(_seek, LV_PCT(100));
    lv_slider_set_range(_seek, 0, 1000);
    lv_obj_add_event_cb(_seek, onSeekEvent, LV_EVENT_ALL, this);
    lv_obj_t *times = lv_obj_create(seek_box);
    lv_obj_remove_style_all(times);
    lv_obj_set_size(times, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(times, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _pos = makeLabel(times, &lv_font_be_vietnam_16, COLOR_MUTED, "0:00");
    _dur = makeLabel(times, &lv_font_be_vietnam_16, COLOR_MUTED, "--:--");

    lv_obj_t *ctrl = lv_obj_create(right);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_size(ctrl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _btn_shuffle = makeRoundButton(ctrl, 56, COLOR_PANEL, LV_SYMBOL_SHUFFLE, &lv_font_be_vietnam_24, onShuffleClicked, this);
    makeRoundButton(ctrl, 68, 0x2C3845, LV_SYMBOL_PREV, &lv_font_be_vietnam_28, onPrevClicked, this);
    _btn_play = makeRoundButton(ctrl, 92, COLOR_ACCENT, LV_SYMBOL_PLAY, &lv_font_be_vietnam_40, onPlayClicked, this,
                                &_btn_play_label);
    makeRoundButton(ctrl, 68, 0x2C3845, LV_SYMBOL_NEXT, &lv_font_be_vietnam_28, onNextClicked, this);
    _btn_repeat = makeRoundButton(ctrl, 56, COLOR_PANEL, LV_SYMBOL_LOOP, &lv_font_be_vietnam_24, onRepeatClicked, this,
                                  &_btn_repeat_label);

    lv_obj_t *vol = lv_obj_create(right);
    lv_obj_remove_style_all(vol);
    lv_obj_set_size(vol, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(vol, 16, 0);
    makeLabel(vol, &lv_font_be_vietnam_24, COLOR_MUTED, LV_SYMBOL_VOLUME_MID);
    _volume = makeSlider(vol);
    lv_obj_set_flex_grow(_volume, 1);
    lv_slider_set_range(_volume, 0, 100);
    lv_slider_set_value(_volume, hcc_music_engine_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(_volume, onVolumeEvent, LV_EVENT_VALUE_CHANGED, this);
    snprintf(buf, sizeof(buf), "%d", hcc_music_engine_get_volume());
    _volume_label = makeLabel(vol, &lv_font_be_vietnam_18, COLOR_MUTED, buf);
    lv_obj_set_width(_volume_label, 44);

    _status = makeLabel(right, &lv_font_be_vietnam_16, COLOR_ERROR, "");
    lv_obj_set_width(_status, LV_PCT(100));
    lv_obj_set_style_text_align(_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_status, LV_LABEL_LONG_DOT);

    refreshModeButtons();
}

void MusicPlayer::refreshModeButtons(void)
{
    if (_btn_shuffle == nullptr || _btn_repeat == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(_btn_shuffle, lv_color_hex(_shuffle ? COLOR_ACCENT : COLOR_PANEL), 0);
    lv_obj_set_style_bg_color(_btn_repeat, lv_color_hex(_repeat == Repeat::Off ? COLOR_PANEL : COLOR_ACCENT), 0);
    lv_label_set_text(_btn_repeat_label, _repeat == Repeat::One ? LV_SYMBOL_LOOP "1" : LV_SYMBOL_LOOP);
}

void MusicPlayer::refreshList(void)
{
    if (_list == nullptr) {
        return;
    }
    if (_list_highlight >= 0 && _list_highlight < (int)lv_obj_get_child_cnt(_list)) {
        lv_obj_clear_state(lv_obj_get_child(_list, _list_highlight), LV_STATE_CHECKED);
    }
    _list_highlight = -1;
    if (_current >= 0 && _current < (int)lv_obj_get_child_cnt(_list)) {
        lv_obj_t *btn = lv_obj_get_child(_list, _current);
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        lv_obj_scroll_to_view(btn, LV_ANIM_ON);
        _list_highlight = _current;
    }
    if (_title != nullptr) {
        lv_label_set_text(_title, _current >= 0 ? trackTitle(_current).c_str() : "Chưa có bài nào");
    }
}

void MusicPlayer::playIndex(int index, uint32_t start_ms)
{
    if (index < 0 || index >= (int)_files.size()) {
        return;
    }
    _current = index;
    _user_paused = false;
    const std::string path = _dir + "/" + _files[index];
    if (hcc_music_engine_play(path.c_str(), start_ms, &_play_id) != ESP_OK) {
        ESP_LOGE(TAG, "Không phát được %s", path.c_str());
    }
    refreshList();
}

int MusicPlayer::nextIndex(bool user_pressed)
{
    const int n = (int)_files.size();
    if (n == 0) {
        return -1;
    }
    if (_shuffle && n > 1) {
        int i = (int)(esp_random() % (uint32_t)(n - 1));
        return i >= _current ? i + 1 : i;  // khác bài hiện tại
    }
    int i = _current + 1;
    if (i >= n) {
        if (_repeat == Repeat::Off && !user_pressed) {
            return -1;  // hết danh sách
        }
        i = 0;
    }
    return i;
}

int MusicPlayer::prevIndex(void)
{
    const int n = (int)_files.size();
    if (n == 0) {
        return -1;
    }
    if (_shuffle && n > 1) {
        return nextIndex(true);
    }
    return _current <= 0 ? n - 1 : _current - 1;
}

void MusicPlayer::refresh(void)
{
    if (_root == nullptr) {
        return;
    }
    hcc_me_status_t st;
    hcc_music_engine_get_status(&st);

    // Phát xong (hoặc lỗi) bài hiện tại -> bài kế tiếp theo chế độ lặp / ngẫu nhiên.
    if (_play_id != 0 && st.play_id == _play_id && st.finished_id == _play_id && _handled_finish != _play_id) {
        _handled_finish = _play_id;
        const bool failed = st.state == HCC_ME_ERROR;
        _error_streak = failed ? _error_streak + 1 : 0;
        if (_error_streak >= (int)_files.size()) {
            ESP_LOGW(TAG, "Không phát được bài nào trong danh sách");
        } else {
            const int next = (_repeat == Repeat::One && !failed) ? _current : nextIndex(false);
            if (next >= 0) {
                playIndex(next);
                return;
            }
        }
    }

    const bool playing = st.state == HCC_ME_PLAYING && st.play_id == _play_id && _play_id != 0;
    lv_label_set_text(_btn_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    char a[24];
    char b[24];
    const bool ours = st.play_id == _play_id && _play_id != 0;
    const uint32_t pos = ours ? st.pos_ms : 0;
    const uint32_t dur = ours ? st.dur_ms : 0;
    if (!_seeking) {
        formatTime(a, sizeof(a), pos);
        lv_label_set_text(_pos, a);
        lv_slider_set_value(_seek, dur > 0 ? (int32_t)((uint64_t)std::min(pos, dur) * 1000 / dur) : 0, LV_ANIM_OFF);
    }
    if (dur > 0) {
        formatTime(b, sizeof(b), dur);
        lv_label_set_text(_dur, b);
    } else {
        lv_label_set_text(_dur, "--:--");
    }
    if (ours && st.seekable && dur > 0) {
        lv_obj_clear_state(_seek, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(_seek, LV_STATE_DISABLED);
    }

    char info[128];
    if (ours && st.fmt != HCC_AUDIO_FMT_UNKNOWN) {
        int n = snprintf(info, sizeof(info), "%s", hcc_audio_fmt_name(st.fmt));
        if (st.sample_rate > 0) {
            n += snprintf(info + n, sizeof(info) - n, "  ·  %lu.%lu kHz", (unsigned long)(st.sample_rate / 1000),
                          (unsigned long)(st.sample_rate % 1000 / 100));
        }
        if (st.bitrate > 0) {
            n += snprintf(info + n, sizeof(info) - n, "  ·  %lu kbps", (unsigned long)(st.bitrate / 1000));
        }
        if (st.channels == 1) {
            n += snprintf(info + n, sizeof(info) - n, "  ·  mono");
        }
        if (!st.seekable) {
            snprintf(info + n, sizeof(info) - n, "  ·  không tua được");
        }
    } else {
        snprintf(info, sizeof(info), "%s", _files.empty() ? "" : "Bấm " LV_SYMBOL_PLAY " để phát");
    }
    lv_label_set_text(_info, info);

    if (ours && st.state == HCC_ME_ERROR) {
        lv_label_set_text(_status, st.error);
    } else if (_error_streak >= (int)_files.size() && !_files.empty()) {
        lv_label_set_text(_status, "Không phát được bài nào trong danh sách");
    } else {
        lv_label_set_text(_status, "");
    }
}

void MusicPlayer::onTimer(lv_timer_t *t)
{
    auto *app = static_cast<MusicPlayer *>(t->user_data);
    if (app != nullptr && app->_root != nullptr) {
        app->refresh();
    }
}

void MusicPlayer::onListClicked(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_target(e);
    if (app == nullptr || btn == nullptr) {
        return;
    }
    app->_error_streak = 0;
    app->playIndex((int)lv_obj_get_index(btn));
    app->refresh();
}

void MusicPlayer::onPlayClicked(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app == nullptr || app->_files.empty()) {
        return;
    }
    hcc_me_status_t st;
    hcc_music_engine_get_status(&st);
    const bool ours = st.play_id == app->_play_id && app->_play_id != 0;
    if (ours && st.state == HCC_ME_PLAYING) {
        app->_user_paused = true;
        hcc_music_engine_pause();
    } else if (ours && st.state == HCC_ME_PAUSED) {
        app->_user_paused = false;
        hcc_music_engine_resume();
    } else {
        app->_error_streak = 0;
        app->playIndex(app->_current < 0 ? 0 : app->_current);
    }
    app->refresh();
}

void MusicPlayer::onPrevClicked(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app == nullptr || app->_files.empty()) {
        return;
    }
    hcc_me_status_t st;
    hcc_music_engine_get_status(&st);
    app->_error_streak = 0;
    // Đã nghe quá 3 giây: về đầu bài (như điện thoại); không thì bài trước.
    if (st.play_id == app->_play_id && app->_play_id != 0 && st.pos_ms > 3000 && st.state != HCC_ME_ERROR) {
        if (st.seekable) {
            hcc_music_engine_seek(0);
            if (st.state == HCC_ME_PAUSED) {
                hcc_music_engine_resume();
            }
        } else {
            app->playIndex(app->_current);
        }
        return;
    }
    app->playIndex(app->prevIndex());
    app->refresh();
}

void MusicPlayer::onNextClicked(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app == nullptr || app->_files.empty()) {
        return;
    }
    app->_error_streak = 0;
    app->playIndex(app->nextIndex(true));
    app->refresh();
}

void MusicPlayer::onShuffleClicked(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app == nullptr) {
        return;
    }
    app->_shuffle = !app->_shuffle;
    app->refreshModeButtons();
}

void MusicPlayer::onRepeatClicked(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app == nullptr) {
        return;
    }
    // Lặp cả danh sách -> lặp 1 bài -> không lặp.
    app->_repeat = app->_repeat == Repeat::All ? Repeat::One : (app->_repeat == Repeat::One ? Repeat::Off : Repeat::All);
    app->refreshModeButtons();
}

void MusicPlayer::onSeekEvent(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app == nullptr || app->_seek == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    hcc_me_status_t st;
    hcc_music_engine_get_status(&st);
    const uint32_t dur = st.dur_ms;
    if (code == LV_EVENT_PRESSED) {
        app->_seeking = true;
    } else if (code == LV_EVENT_VALUE_CHANGED && app->_seeking && dur > 0) {
        char a[24];
        formatTime(a, sizeof(a), (uint32_t)((uint64_t)lv_slider_get_value(app->_seek) * dur / 1000));
        lv_label_set_text(app->_pos, a);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (app->_seeking && dur > 0 && st.seekable && st.play_id == app->_play_id) {
            hcc_music_engine_seek((uint32_t)((uint64_t)lv_slider_get_value(app->_seek) * dur / 1000));
        }
        app->_seeking = false;
    }
}

void MusicPlayer::onVolumeEvent(lv_event_t *e)
{
    auto *app = static_cast<MusicPlayer *>(lv_event_get_user_data(e));
    if (app == nullptr || app->_volume == nullptr) {
        return;
    }
    const int v = lv_slider_get_value(app->_volume);
    hcc_music_engine_set_volume(v);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(app->_volume_label, buf);
}
