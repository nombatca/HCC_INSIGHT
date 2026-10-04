/*
 * App "Xiaozhi AI": xem XiaozhiApp.hpp / HCCXiaozhi.hpp.
 */
#include "XiaozhiApp.hpp"

#include <stdio.h>
#include <string.h>

#include <string>

#include "esp_log.h"

#include "HCCXiaozhi.hpp"

LV_IMG_DECLARE(img_app_xiaozhi);

namespace {

const char *TAG = "XiaozhiApp";

constexpr uint32_t COLOR_BG = 0x0F141A;
constexpr uint32_t COLOR_PANEL = 0x18212B;
constexpr uint32_t COLOR_ACCENT = 0x5B5FF0;
constexpr uint32_t COLOR_USER = 0x2478D0;
constexpr uint32_t COLOR_BOT = 0x2C3845;
constexpr uint32_t COLOR_TEXT = 0xE8EEF4;
constexpr uint32_t COLOR_MUTED = 0x8A97A3;
constexpr uint32_t COLOR_ERROR = 0xFF6B6B;
constexpr uint32_t COLOR_OK = 0x3DD66E;

struct EmotionView {
    const char *key;
    const char *face;   // kaomoji: font không có emoji
    const char *label;
    uint32_t color;
};

// Các cảm xúc máy chủ Xiaozhi gửi trong tin nhắn "llm".
const EmotionView EMOTIONS[] = {
    {"neutral", "(-_-)", "Bình thường", 0x5B5FF0},
    {"happy", "(^_^)", "Vui", 0x2EB872},
    {"laughing", "(^o^)", "Cười", 0x2EB872},
    {"funny", "(^v^)", "Hài hước", 0x2EB872},
    {"sad", "(T_T)", "Buồn", 0x4A6FA5},
    {"angry", "(>_<)", "Giận", 0xD64545},
    {"crying", "(;_;)", "Khóc", 0x4A6FA5},
    {"loving", "(*^3^)", "Yêu thích", 0xE0559A},
    {"embarrassed", "(^_^;)", "Ngại", 0xE08A55},
    {"surprised", "(o_O)", "Ngạc nhiên", 0xE0B455},
    {"shocked", "(O_O)", "Sốc", 0xE0B455},
    {"thinking", "(._.)?", "Đang nghĩ", 0x7A6FD6},
    {"winking", "(^_-)", "Nháy mắt", 0x2EB872},
    {"cool", "(B-)", "Ngầu", 0x2E9EB8},
    {"relaxed", "(~_~)", "Thư giãn", 0x2E9EB8},
    {"delicious", "(^p^)", "Ngon", 0xE08A55},
    {"kissy", "(*^3^)", "Hôn", 0xE0559A},
    {"confident", "(^_^)b", "Tự tin", 0x2EB872},
    {"sleepy", "(-_-)zz", "Buồn ngủ", 0x6B7A8A},
    {"silly", "(:P)", "Tinh nghịch", 0xE0B455},
    {"confused", "(@_@)", "Bối rối", 0x7A6FD6},
};

const EmotionView &emotionView(const std::string &key)
{
    for (const auto &e : EMOTIONS) {
        if (key == e.key) {
            return e;
        }
    }
    return EMOTIONS[0];
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

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, uint32_t bg, const lv_font_t *font, lv_event_cb_t cb,
                     void *user, lv_obj_t **label_out = nullptr)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = makeLabel(b, font, COLOR_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    if (label_out != nullptr) {
        *label_out = l;
    }
    return b;
}

}  // namespace

XiaozhiApp::XiaozhiApp():
    ESP_Brookesia_PhoneApp("Xiaozhi AI", &img_app_xiaozhi, true)
{
}

XiaozhiApp::~XiaozhiApp()
{
}

bool XiaozhiApp::init(void)
{
    return true;
}

bool XiaozhiApp::run(void)
{
    buildUi(lv_scr_act());
    lv_timer_create(onTimer, 60, this);  // nhanh để miệng nhân vật khớp tiếng
    HCCXiaozhi::start();
    refresh();
    return true;
}

bool XiaozhiApp::pause(void)
{
    // Xuống nền: kết thúc trò chuyện (không thu micro khi không nhìn thấy app).
    HCCXiaozhi::endSession();
    return true;
}

bool XiaozhiApp::back(void)
{
    if (_dialog != nullptr) {
        closeServerDialog();
        return true;
    }
    notifyCoreClosed();
    return true;
}

bool XiaozhiApp::close(void)
{
    HCCXiaozhi::stop();
    detachUi();
    return true;
}

void XiaozhiApp::detachUi(void)
{
    _root = nullptr;
    _avatar.detach();
    _emotion_label = nullptr;
    _status = nullptr;
    _btn_talk = nullptr;
    _btn_talk_label = nullptr;
    _btn_end = nullptr;
    _volume_label = nullptr;
    _chat = nullptr;
    _chat_panel = nullptr;
    _act_panel = nullptr;
    _act_code = nullptr;
    _act_msg = nullptr;
    _device_label = nullptr;
    _dialog = nullptr;
    _dialog_ta = nullptr;
    _chat_version = UINT32_MAX;
    _last_state = -1;
    _last_emotion[0] = '\0';
}

void XiaozhiApp::onRootDeleted(lv_event_t *e)
{
    auto *app = static_cast<XiaozhiApp *>(lv_event_get_user_data(e));
    if (app != nullptr) {
        app->detachUi();
    }
}

void XiaozhiApp::buildUi(lv_obj_t *screen)
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

    /* ----- Bên trái: nhân vật hoạt hình + trạng thái ----- */
    lv_obj_t *left = makePanel(_root);
    lv_obj_set_size(left, LV_PCT(38), LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(left, 6, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *avatar = _avatar.create(left);
    lv_obj_set_width(avatar, LV_PCT(100));
    lv_obj_set_flex_grow(avatar, 1);

    _emotion_label = makeLabel(left, &lv_font_be_vietnam_16, COLOR_MUTED, "");
    _status = makeLabel(left, &lv_font_be_vietnam_24, COLOR_TEXT, "");
    lv_obj_set_width(_status, LV_PCT(100));
    lv_obj_set_style_text_align(_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_status, LV_LABEL_LONG_WRAP);

    /* ----- Bên phải: trò chuyện / kích hoạt + điều khiển ----- */
    lv_obj_t *right = makePanel(_root);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(right, 10, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    _chat_panel = lv_obj_create(right);
    lv_obj_remove_style_all(_chat_panel);
    lv_obj_set_width(_chat_panel, LV_PCT(100));
    lv_obj_set_flex_grow(_chat_panel, 1);
    lv_obj_set_flex_flow(_chat_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(_chat_panel, 10, 0);
    lv_obj_clear_flag(_chat_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *head = lv_obj_create(_chat_panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    makeLabel(head, &lv_font_be_vietnam_22, COLOR_TEXT, "Xiaozhi AI");
    lv_obj_t *clr = makeButton(head, LV_SYMBOL_TRASH " Xoá", 0x2C3845, &lv_font_be_vietnam_16, onClearClicked, this);
    lv_obj_set_height(clr, 40);

    _chat = lv_obj_create(_chat_panel);
    lv_obj_remove_style_all(_chat);
    lv_obj_set_width(_chat, LV_PCT(100));
    lv_obj_set_flex_grow(_chat, 1);
    lv_obj_set_flex_flow(_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(_chat, 10, 0);
    lv_obj_set_scroll_dir(_chat, LV_DIR_VER);
    lv_obj_add_flag(_chat, LV_OBJ_FLAG_SCROLLABLE);

    _act_panel = lv_obj_create(right);
    lv_obj_remove_style_all(_act_panel);
    lv_obj_set_width(_act_panel, LV_PCT(100));
    lv_obj_set_flex_grow(_act_panel, 1);
    lv_obj_set_flex_flow(_act_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_act_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(_act_panel, 14, 0);
    makeLabel(_act_panel, &lv_font_be_vietnam_24, COLOR_TEXT, "Kích hoạt thiết bị");
    _act_code = makeLabel(_act_panel, &lv_font_be_vietnam_48, COLOR_OK, "------");
    lv_obj_set_style_text_letter_space(_act_code, 12, 0);
    lv_obj_t *guide = makeLabel(_act_panel, &lv_font_be_vietnam_18, COLOR_MUTED,
                                "1. Mở xiaozhi.me trên điện thoại / máy tính, đăng nhập.\n"
                                "2. Bảng điều khiển -> Thêm thiết bị -> nhập mã 6 số ở trên.\n"
                                "3. Trong cấu hình agent: chọn ngôn ngữ / giọng tiếng Việt.\n"
                                "Board tự kiểm tra mỗi 3 giây, xong sẽ chuyển sang Sẵn sàng.");
    lv_obj_set_width(guide, LV_PCT(95));
    lv_label_set_long_mode(guide, LV_LABEL_LONG_WRAP);
    _act_msg = makeLabel(_act_panel, &lv_font_be_vietnam_16, COLOR_MUTED, "");
    lv_obj_set_width(_act_msg, LV_PCT(95));
    lv_label_set_long_mode(_act_msg, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(_act_panel, LV_OBJ_FLAG_HIDDEN);

    // Hàng nút chính
    lv_obj_t *row = lv_obj_create(right);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 10, 0);
    _btn_talk = makeButton(row, LV_SYMBOL_AUDIO "  Bắt đầu nói chuyện", COLOR_ACCENT, &lv_font_be_vietnam_22,
                           onTalkClicked, this, &_btn_talk_label);
    lv_obj_set_height(_btn_talk, 64);
    lv_obj_set_flex_grow(_btn_talk, 1);
    _btn_end = makeButton(row, LV_SYMBOL_CLOSE "  Kết thúc", 0x2C3845, &lv_font_be_vietnam_18, onEndClicked, this);
    lv_obj_set_size(_btn_end, 150, 64);

    // Âm lượng + cài đặt
    lv_obj_t *tools = lv_obj_create(right);
    lv_obj_remove_style_all(tools);
    lv_obj_set_size(tools, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tools, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(tools, 10, 0);
    makeLabel(tools, &lv_font_be_vietnam_22, COLOR_MUTED, LV_SYMBOL_VOLUME_MID);
    lv_obj_t *slider = lv_slider_create(tools);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, HCCXiaozhi::volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2C3845), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 14);
    lv_obj_add_event_cb(slider, onVolumeChanged, LV_EVENT_VALUE_CHANGED, this);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", HCCXiaozhi::volume());
    _volume_label = makeLabel(tools, &lv_font_be_vietnam_18, COLOR_MUTED, buf);
    lv_obj_set_width(_volume_label, 40);
    lv_obj_t *b1 = makeButton(tools, LV_SYMBOL_SETTINGS " Máy chủ", 0x2C3845, &lv_font_be_vietnam_16, onServerClicked, this);
    lv_obj_set_height(b1, 44);
    lv_obj_t *b2 = makeButton(tools, LV_SYMBOL_REFRESH " Kích hoạt lại", 0x2C3845, &lv_font_be_vietnam_16,
                              onReactivateClicked, this);
    lv_obj_set_height(b2, 44);

    std::string dev = "Mã thiết bị: " + HCCXiaozhi::deviceId();
    _device_label = makeLabel(right, &lv_font_be_vietnam_14, COLOR_MUTED, dev.c_str());
}

void XiaozhiApp::rebuildChat(void)
{
    if (_chat == nullptr) {
        return;
    }
    lv_obj_clean(_chat);
    const auto lines = HCCXiaozhi::chat();
    if (lines.empty()) {
        lv_obj_t *hint = makeLabel(_chat, &lv_font_be_vietnam_18, COLOR_MUTED,
                                   "Bấm \"Bắt đầu nói chuyện\" rồi nói. Trợ lý tự nhận biết khi bạn nói xong, "
                                   "trả lời bằng giọng nói, rồi nghe tiếp.\nBấm lúc trợ lý đang nói để ngắt lời.");
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        return;
    }
    lv_obj_t *last = nullptr;
    for (const auto &line : lines) {
        lv_obj_t *row = lv_obj_create(_chat);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, line.user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_t *bubble = lv_obj_create(row);
        lv_obj_remove_style_all(bubble);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(line.user ? COLOR_USER : COLOR_BOT), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bubble, 14, 0);
        lv_obj_set_style_pad_hor(bubble, 14, 0);
        lv_obj_set_style_pad_ver(bubble, 10, 0);
        lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(bubble, LV_PCT(80), 0);
        lv_obj_t *l = makeLabel(bubble, &lv_font_be_vietnam_18, COLOR_TEXT, line.text.c_str());
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_max_width(l, 480, 0);
        lv_obj_set_width(l, LV_SIZE_CONTENT);
        last = row;
    }
    if (last != nullptr) {
        lv_obj_update_layout(_chat);
        lv_obj_scroll_to_view(last, LV_ANIM_OFF);
    }
}

void XiaozhiApp::refresh(void)
{
    if (_root == nullptr) {
        return;
    }
    using HCCXiaozhi::State;
    const State st = HCCXiaozhi::state();

    // Trạng thái + nút chính
    std::string status = HCCXiaozhi::stateText(st);
    uint32_t status_color = COLOR_TEXT;
    const char *talk = LV_SYMBOL_AUDIO "  Bắt đầu nói chuyện";
    uint32_t talk_color = COLOR_ACCENT;
    bool talk_enabled = true;
    switch (st) {
    case State::Off:
    case State::Checking:
    case State::Connecting:
        talk_enabled = false;
        talk = "Vui lòng chờ...";
        break;
    case State::Activating:
        talk_enabled = false;
        talk = "Chờ kích hoạt...";
        break;
    case State::Ready:
        break;
    case State::Listening:
        talk = LV_SYMBOL_STOP "  Đang nghe - bấm để dừng";
        talk_color = 0xC0392B;
        status_color = COLOR_OK;
        break;
    case State::Speaking:
        talk = LV_SYMBOL_PAUSE "  Ngắt lời";
        talk_color = 0xE08A55;
        break;
    case State::Error:
        talk = LV_SYMBOL_REFRESH "  Thử lại";
        status = HCCXiaozhi::lastError();
        status_color = COLOR_ERROR;
        break;
    }
    lv_label_set_text(_status, status.c_str());
    lv_obj_set_style_text_color(_status, lv_color_hex(status_color), 0);
    lv_label_set_text(_btn_talk_label, talk);
    lv_obj_set_style_bg_color(_btn_talk, lv_color_hex(talk_color), 0);
    if (talk_enabled) {
        lv_obj_clear_state(_btn_talk, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(_btn_talk, LV_STATE_DISABLED);
    }
    const bool in_session = st == State::Connecting || st == State::Listening || st == State::Speaking;
    if (in_session) {
        lv_obj_clear_flag(_btn_end, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_btn_end, LV_OBJ_FLAG_HIDDEN);
    }

    // Nhân vật: chế độ, cảm xúc, mức âm
    const std::string emo = HCCXiaozhi::emotion();
    XiaozhiAvatar::Mode mode = XiaozhiAvatar::Mode::Idle;
    if (st == State::Listening) {
        mode = HCCXiaozhi::thinking() ? XiaozhiAvatar::Mode::Thinking : XiaozhiAvatar::Mode::Listening;
    } else if (st == State::Speaking) {
        mode = XiaozhiAvatar::Mode::Speaking;
    } else if (st == State::Connecting || st == State::Checking) {
        mode = XiaozhiAvatar::Mode::Thinking;
    } else if (st == State::Error) {
        mode = XiaozhiAvatar::Mode::Sad;
    }
    _avatar.setMode(mode);
    _avatar.setSpeakLevel(HCCXiaozhi::speakLevel());
    _avatar.setMicLevel(HCCXiaozhi::micLevel());
    if ((int)st != _last_state || emo != _last_emotion) {
        const EmotionView &ev = emotionView(emo);
        _avatar.setEmotion(in_session ? emo.c_str() : "neutral");
        lv_label_set_text(_emotion_label, in_session ? ev.label : "");
        snprintf(_last_emotion, sizeof(_last_emotion), "%s", emo.c_str());
        _last_state = (int)st;
    }

    // Kích hoạt / trò chuyện
    if (st == State::Activating) {
        lv_obj_add_flag(_chat_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_act_panel, LV_OBJ_FLAG_HIDDEN);
        const std::string code = HCCXiaozhi::activationCode();
        lv_label_set_text(_act_code, code.empty() ? "------" : code.c_str());
        lv_label_set_text(_act_msg, HCCXiaozhi::activationMessage().c_str());
    } else {
        lv_obj_add_flag(_act_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_chat_panel, LV_OBJ_FLAG_HIDDEN);
        const uint32_t ver = HCCXiaozhi::chatVersion();
        if (ver != _chat_version) {
            _chat_version = ver;
            rebuildChat();
        }
    }
}

void XiaozhiApp::onTimer(lv_timer_t *t)
{
    auto *app = static_cast<XiaozhiApp *>(t->user_data);
    if (app != nullptr && app->_root != nullptr) {
        app->refresh();
    }
}

void XiaozhiApp::onTalkClicked(lv_event_t *e)
{
    HCCXiaozhi::talk();
}

void XiaozhiApp::onEndClicked(lv_event_t *e)
{
    HCCXiaozhi::endSession();
}

void XiaozhiApp::onClearClicked(lv_event_t *e)
{
    HCCXiaozhi::clearChat();
}

void XiaozhiApp::onReactivateClicked(lv_event_t *e)
{
    HCCXiaozhi::resetActivation();
}

void XiaozhiApp::onVolumeChanged(lv_event_t *e)
{
    auto *app = static_cast<XiaozhiApp *>(lv_event_get_user_data(e));
    lv_obj_t *slider = lv_event_get_target(e);
    const int v = lv_slider_get_value(slider);
    HCCXiaozhi::setVolume(v);
    if (app != nullptr && app->_volume_label != nullptr) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", v);
        lv_label_set_text(app->_volume_label, buf);
    }
}

/* ----- Hộp thoại máy chủ (cho máy chủ tự dựng, vd xiaozhi-esp32-server) ----- */

void XiaozhiApp::onServerClicked(lv_event_t *e)
{
    auto *app = static_cast<XiaozhiApp *>(lv_event_get_user_data(e));
    if (app != nullptr) {
        app->openServerDialog();
    }
}

void XiaozhiApp::openServerDialog(void)
{
    if (_dialog != nullptr || _root == nullptr) {
        return;
    }
    _dialog = lv_obj_create(lv_obj_get_parent(_root));
    lv_obj_set_size(_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_dialog, LV_OPA_80, 0);
    lv_obj_set_style_border_width(_dialog, 0, 0);
    lv_obj_set_style_radius(_dialog, 0, 0);
    lv_obj_clear_flag(_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = makePanel(_dialog);
    lv_obj_set_size(box, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(box, 10, 0);
    makeLabel(box, &lv_font_be_vietnam_20, COLOR_TEXT, "Địa chỉ OTA của máy chủ Xiaozhi");
    lv_obj_t *hint = makeLabel(box, &lv_font_be_vietnam_14, COLOR_MUTED,
                               "Mặc định: máy chủ chính thức xiaozhi.me. Máy chủ tự dựng (xiaozhi-esp32-server): "
                               "nhập địa chỉ OTA, vd http://192.168.1.10:8003/xiaozhi/ota/");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    _dialog_ta = lv_textarea_create(box);
    lv_obj_set_width(_dialog_ta, LV_PCT(100));
    lv_textarea_set_one_line(_dialog_ta, true);
    lv_obj_set_style_text_font(_dialog_ta, &lv_font_be_vietnam_18, 0);
    lv_textarea_set_text(_dialog_ta, HCCXiaozhi::otaUrl().c_str());
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 10, 0);
    lv_obj_t *b;
    b = makeButton(row, "Lưu", COLOR_ACCENT, &lv_font_be_vietnam_18, onDialogSave, this);
    lv_obj_set_flex_grow(b, 1);
    b = makeButton(row, "Mặc định", 0x2C3845, &lv_font_be_vietnam_18, onDialogDefault, this);
    lv_obj_set_flex_grow(b, 1);
    b = makeButton(row, "Huỷ", 0x2C3845, &lv_font_be_vietnam_18, onDialogCancel, this);
    lv_obj_set_flex_grow(b, 1);

    lv_obj_t *kb = lv_keyboard_create(_dialog);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(45));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, _dialog_ta);
}

void XiaozhiApp::closeServerDialog(void)
{
    if (_dialog != nullptr) {
        lv_obj_del(_dialog);
        _dialog = nullptr;
        _dialog_ta = nullptr;
    }
}

void XiaozhiApp::onDialogSave(lv_event_t *e)
{
    auto *app = static_cast<XiaozhiApp *>(lv_event_get_user_data(e));
    if (app == nullptr || app->_dialog_ta == nullptr) {
        return;
    }
    std::string url = lv_textarea_get_text(app->_dialog_ta);
    while (!url.empty() && (url.back() == ' ' || url.back() == '\n')) {
        url.pop_back();
    }
    ESP_LOGI(TAG, "Đổi máy chủ OTA: %s", url.c_str());
    HCCXiaozhi::setOtaUrl(url);
    app->closeServerDialog();
}

void XiaozhiApp::onDialogDefault(lv_event_t *e)
{
    auto *app = static_cast<XiaozhiApp *>(lv_event_get_user_data(e));
    if (app == nullptr) {
        return;
    }
    HCCXiaozhi::setOtaUrl("");
    app->closeServerDialog();
}

void XiaozhiApp::onDialogCancel(lv_event_t *e)
{
    auto *app = static_cast<XiaozhiApp *>(lv_event_get_user_data(e));
    if (app != nullptr) {
        app->closeServerDialog();
    }
}
