/*
 * Music: danh sách bài từ thẻ SD (thư mục music hoặc gốc thẻ) hoặc nhạc nạp sẵn trong flash,
 * phát bằng HCCMusicEngine (esp_audio_codec). Giao diện riêng thay cho lv_demo_music của LVGL.
 */
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "lvgl.h"
#include "esp_brookesia.hpp"

class MusicPlayer: public ESP_Brookesia_PhoneApp {
public:
    MusicPlayer();
    ~MusicPlayer();

    bool run(void);
    bool back(void);
    bool close(void);

    bool init(void) override;
    bool pause(void) override;

private:
    enum class Repeat : uint8_t { Off, All, One };

    void buildUi(lv_obj_t *screen);
    void detachUi(void);
    void playIndex(int index, uint32_t start_ms = 0);
    int nextIndex(bool user_pressed);
    int prevIndex(void);
    void refresh(void);
    void refreshList(void);
    void refreshModeButtons(void);
    std::string trackTitle(int index) const;

    static void onTimer(lv_timer_t *t);
    static void onListClicked(lv_event_t *e);
    static void onPlayClicked(lv_event_t *e);
    static void onPrevClicked(lv_event_t *e);
    static void onNextClicked(lv_event_t *e);
    static void onShuffleClicked(lv_event_t *e);
    static void onRepeatClicked(lv_event_t *e);
    static void onSeekEvent(lv_event_t *e);
    static void onVolumeEvent(lv_event_t *e);
    static void onRootDeleted(lv_event_t *e);

    // Danh sách bài
    std::string _dir;
    std::string _source;
    std::vector<std::string> _files;

    // Trạng thái phát
    int _current = -1;
    uint32_t _play_id = 0;
    uint32_t _handled_finish = 0;
    int _error_streak = 0;
    bool _shuffle = false;
    Repeat _repeat = Repeat::All;
    bool _seeking = false;
    bool _user_paused = false;

    // Giao diện (nullptr khi app đóng)
    lv_obj_t *_root = nullptr;
    lv_obj_t *_list = nullptr;
    lv_obj_t *_list_title = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_info = nullptr;
    lv_obj_t *_status = nullptr;
    lv_obj_t *_seek = nullptr;
    lv_obj_t *_pos = nullptr;
    lv_obj_t *_dur = nullptr;
    lv_obj_t *_btn_play = nullptr;
    lv_obj_t *_btn_play_label = nullptr;
    lv_obj_t *_btn_shuffle = nullptr;
    lv_obj_t *_btn_repeat = nullptr;
    lv_obj_t *_btn_repeat_label = nullptr;
    lv_obj_t *_volume = nullptr;
    lv_obj_t *_volume_label = nullptr;
    lv_timer_t *_timer = nullptr;
    int _list_highlight = -1;
};
