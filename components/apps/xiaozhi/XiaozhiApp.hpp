/*
 * App "Xiaozhi AI": giao diện cho HCCXiaozhi (trợ lý giọng nói xiaozhi.me).
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "XiaozhiAvatar.hpp"

class XiaozhiApp: public ESP_Brookesia_PhoneApp {
public:
    XiaozhiApp();
    ~XiaozhiApp();

    bool run(void);
    bool back(void);
    bool close(void);

    bool init(void) override;
    bool pause(void) override;

private:
    void buildUi(lv_obj_t *screen);
    void detachUi(void);
    void refresh(void);
    void rebuildChat(void);
    void openServerDialog(void);
    void closeServerDialog(void);

    static void onTimer(lv_timer_t *t);
    static void onTalkClicked(lv_event_t *e);
    static void onEndClicked(lv_event_t *e);
    static void onClearClicked(lv_event_t *e);
    static void onServerClicked(lv_event_t *e);
    static void onReactivateClicked(lv_event_t *e);
    static void onVolumeChanged(lv_event_t *e);
    static void onDialogSave(lv_event_t *e);
    static void onDialogDefault(lv_event_t *e);
    static void onDialogCancel(lv_event_t *e);
    static void onRootDeleted(lv_event_t *e);

    lv_obj_t *_root = nullptr;
    XiaozhiAvatar _avatar;
    lv_obj_t *_emotion_label = nullptr;
    lv_obj_t *_status = nullptr;
    lv_obj_t *_btn_talk = nullptr;
    lv_obj_t *_btn_talk_label = nullptr;
    lv_obj_t *_btn_end = nullptr;
    lv_obj_t *_volume_label = nullptr;
    lv_obj_t *_chat = nullptr;
    lv_obj_t *_chat_panel = nullptr;
    lv_obj_t *_act_panel = nullptr;
    lv_obj_t *_act_code = nullptr;
    lv_obj_t *_act_msg = nullptr;
    lv_obj_t *_device_label = nullptr;
    lv_obj_t *_dialog = nullptr;
    lv_obj_t *_dialog_ta = nullptr;
    uint32_t _chat_version = UINT32_MAX;
    int _last_state = -1;
    char _last_emotion[24] = "";
};
