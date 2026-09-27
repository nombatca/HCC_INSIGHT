#pragma once

#include <string>
#include <vector>

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

class Browser : public ESP_Brookesia_PhoneApp {
public:
    Browser();
    ~Browser() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    // Một khối nội dung đã phân tích từ HTML: đoạn văn, tiêu đề, hoặc liên kết bấm được.
    struct Block {
        enum class Type { Text, Heading, Link } type;
        std::string text;
        std::string href;  // chỉ dùng cho Link
        int level;         // cấp tiêu đề (1..6) cho Heading
    };

private:
    // UI events
    static void onGoClicked(lv_event_t *event);
    static void onBackClicked(lv_event_t *event);
    static void onForwardClicked(lv_event_t *event);
    static void onReloadClicked(lv_event_t *event);
    static void onHomeClicked(lv_event_t *event);
    static void onKeyboardEvent(lv_event_t *event);
    static void onAddressFocused(lv_event_t *event);
    static void onLinkClicked(lv_event_t *event);
    static void onLinkDeleted(lv_event_t *event);

    // Fetch pipeline
    static void fetchTask(void *arg);
    static void applyFetchResult(void *arg);
    static std::vector<Block> parseHtml(const char *html, size_t len, const std::string &base);

    // Navigation
    void navigate(const char *url, bool push_history);
    void goToHistory(int new_pos);
    void reload();
    void updateNavButtons();

    // Rendering
    void renderBlocks(const std::vector<Block> &blocks);
    void clearContent();
    void setStatus(const char *text);
    void setAddress(const char *url);

    lv_obj_t *_addr_input;
    lv_obj_t *_keyboard;
    lv_obj_t *_content;   // vùng cuộn chứa các khối nội dung
    lv_obj_t *_status;
    lv_obj_t *_btn_back;
    lv_obj_t *_btn_forward;

    std::vector<std::string> _history;
    int _history_pos;          // vị trí hiện tại trong _history (-1 nếu rỗng)
    std::string _current_url;   // URL trang đang hiển thị (để phân giải link tương đối)

    TaskHandle_t _fetch_task;
    volatile bool _loading;
};
