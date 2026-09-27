#include "Browser.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"

static const char *TAG = "Browser";
static constexpr size_t PAGE_BUFFER_SIZE = 65536;
static constexpr size_t MAX_BLOCKS = 600;
static const char *HOME_URL = "https://example.com";

namespace {

struct FetchContext {
    Browser *app;
    std::string url;
    char *body;
    size_t length;
    esp_err_t result;
    int status_code;
    std::string final_url;
    std::vector<Browser::Block> *blocks;
};

esp_err_t httpEventHandler(esp_http_client_event_t *event)
{
    auto *context = static_cast<FetchContext *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ERROR) {
        ESP_LOGW(TAG, "HTTP_EVENT_ERROR");
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data && context && context->body) {
        const size_t available = PAGE_BUFFER_SIZE - 1 - context->length;
        const size_t copy_size = (size_t)event->data_len < available ? (size_t)event->data_len : available;
        memcpy(context->body + context->length, event->data, copy_size);
        context->length += copy_size;
        context->body[context->length] = '\0';
    }
    return ESP_OK;
}

bool hasNetworkConnectivity()
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || !esp_netif_is_netif_up(netif)) {
        return false;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }
    return ip_info.ip.addr != 0;
}

// Thêm scheme https:// nếu người dùng gõ thiếu.
std::string normalizeUrl(const std::string &input)
{
    std::string s = input;
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    if (a == std::string::npos) {
        return std::string();
    }
    s = s.substr(a, b - a + 1);
    if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0) {
        return s;
    }
    return "https://" + s;
}

// Phân giải link tương đối theo URL trang hiện tại thành URL tuyệt đối.
std::string resolveUrl(const std::string &base, const std::string &href)
{
    if (href.empty() || href[0] == '#') {
        return base;
    }
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) {
        return href;
    }

    std::string scheme = "https";
    std::string host_path = base;
    size_t sp = base.find("://");
    if (sp != std::string::npos) {
        scheme = base.substr(0, sp);
        host_path = base.substr(sp + 3);
    }
    size_t slash = host_path.find('/');
    std::string host = (slash == std::string::npos) ? host_path : host_path.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : host_path.substr(slash);

    if (href.rfind("//", 0) == 0) {
        return scheme + ":" + href;
    }
    if (href[0] == '/') {
        return scheme + "://" + host + href;
    }
    // Tương đối theo thư mục hiện tại.
    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    size_t last = path.find_last_of('/');
    std::string dir = (last == std::string::npos) ? "/" : path.substr(0, last + 1);
    return scheme + "://" + host + dir + href;
}

void appendUtf8(std::string &out, uint32_t codepoint)
{
    if (codepoint <= 0x7F) {
        out.push_back((char)codepoint);
    } else if (codepoint <= 0x7FF) {
        out.push_back((char)(0xC0 | (codepoint >> 6)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back((char)(0xE0 | (codepoint >> 12)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (codepoint >> 18)));
        out.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    }
}

// Trích giá trị thuộc tính (vd href) trong chuỗi thuộc tính của thẻ.
std::string extractAttr(const char *attrs, size_t len, const char *name)
{
    std::string hay(attrs, len);
    std::string low = hay;
    for (auto &c : low) c = (char)tolower((unsigned char)c);
    std::string key = name;
    size_t pos = low.find(key);
    while (pos != std::string::npos) {
        size_t eq = pos + key.size();
        while (eq < low.size() && (low[eq] == ' ' || low[eq] == '\t')) eq++;
        if (eq < low.size() && low[eq] == '=') {
            eq++;
            while (eq < low.size() && (low[eq] == ' ' || low[eq] == '\t')) eq++;
            char quote = 0;
            if (eq < hay.size() && (hay[eq] == '"' || hay[eq] == '\'')) {
                quote = hay[eq];
                eq++;
            }
            size_t start = eq;
            while (eq < hay.size()) {
                if (quote && hay[eq] == quote) break;
                if (!quote && (hay[eq] == ' ' || hay[eq] == '\t' || hay[eq] == '>')) break;
                eq++;
            }
            return hay.substr(start, eq - start);
        }
        pos = low.find(key, pos + 1);
    }
    return std::string();
}

} // namespace

std::vector<Browser::Block> Browser::parseHtml(const char *html, size_t len, const std::string &base)
{
    std::vector<Block> blocks;
    std::string cur;
    bool cur_space = true;
    int heading_level = 0;
    bool in_link = false;
    std::string link_text;
    std::string link_href;
    bool link_space = true;

    auto appendCh = [](std::string &buf, bool &last_space, char c) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_space) {
                buf.push_back(' ');
                last_space = true;
            }
        } else {
            buf.push_back(c);
            last_space = false;
        }
    };
    auto trim = [](const std::string &s) -> std::string {
        size_t a = s.find_first_not_of(' ');
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(' ');
        return s.substr(a, b - a + 1);
    };
    auto flushText = [&]() {
        std::string t = trim(cur);
        cur.clear();
        cur_space = true;
        if (!t.empty() && blocks.size() < MAX_BLOCKS) {
            Block blk;
            blk.type = (heading_level > 0) ? Block::Type::Heading : Block::Type::Text;
            blk.level = heading_level;
            blk.text = t;
            blocks.push_back(std::move(blk));
        }
    };

    static const char *skip_tags[] = {"script", "style", "noscript", "head"};
    static const char *block_tags[] = {
        "p", "div", "br", "li", "tr", "ul", "ol", "table", "hr",
        "article", "section", "header", "footer", "nav", "aside", "main", "blockquote"
    };

    size_t i = 0;
    while (i < len && blocks.size() < MAX_BLOCKS) {
        char c = html[i];
        if (c == '<') {
            size_t tag_start = i + 1;
            bool closing = (tag_start < len && html[tag_start] == '/');
            size_t name_start = closing ? tag_start + 1 : tag_start;
            size_t name_end = name_start;
            while (name_end < len && isalnum((unsigned char)html[name_end])) name_end++;
            size_t name_len = name_end - name_start;

            size_t gt = i;
            while (gt < len && html[gt] != '>') gt++;
            if (gt >= len) break;

            auto nameIs = [&](const char *t) {
                return name_len == strlen(t) && strncasecmp(html + name_start, t, name_len) == 0;
            };

            // Bỏ hẳn khối script/style/head.
            bool skip = false;
            for (auto t : skip_tags) {
                if (nameIs(t)) { skip = true; break; }
            }
            if (skip && !closing) {
                char close_tag[16];
                snprintf(close_tag, sizeof(close_tag), "</%.*s", (int)name_len, html + name_start);
                size_t close_len = strlen(close_tag);
                size_t pos = len;
                for (size_t j = gt + 1; j + close_len <= len; ++j) {
                    if (strncasecmp(html + j, close_tag, close_len) == 0) { pos = j; break; }
                }
                if (pos == len) { i = len; }
                else {
                    size_t cg = pos;
                    while (cg < len && html[cg] != '>') cg++;
                    i = (cg < len) ? cg + 1 : len;
                }
                continue;
            }

            // Tiêu đề h1..h6
            if (name_len == 2 && (html[name_start] == 'h' || html[name_start] == 'H') &&
                html[name_start + 1] >= '1' && html[name_start + 1] <= '6') {
                flushText();
                heading_level = closing ? 0 : (html[name_start + 1] - '0');
                i = gt + 1;
                continue;
            }

            // Liên kết
            if (nameIs("a")) {
                if (!closing) {
                    flushText();
                    in_link = true;
                    link_text.clear();
                    link_space = true;
                    link_href = extractAttr(html + name_end, gt - name_end, "href");
                } else if (in_link) {
                    std::string t = trim(link_text);
                    if (!t.empty() && !link_href.empty() && blocks.size() < MAX_BLOCKS) {
                        Block blk;
                        blk.type = Block::Type::Link;
                        blk.level = 0;
                        blk.text = t;
                        blk.href = resolveUrl(base, link_href);
                        blocks.push_back(std::move(blk));
                    }
                    in_link = false;
                }
                i = gt + 1;
                continue;
            }

            // Thẻ khối -> ngắt đoạn
            for (auto t : block_tags) {
                if (nameIs(t)) { flushText(); break; }
            }
            i = gt + 1;
            continue;
        }

        if (c == '&') {
            size_t semi = i + 1;
            size_t limit = (i + 12 < len) ? i + 12 : len;
            while (semi < limit && html[semi] != ';') semi++;
            if (semi < len && html[semi] == ';') {
                std::string entity(html + i + 1, semi - i - 1);
                uint32_t cp = 0;
                bool handled = true;
                if (entity == "amp") cp = '&';
                else if (entity == "lt") cp = '<';
                else if (entity == "gt") cp = '>';
                else if (entity == "quot") cp = '"';
                else if (entity == "apos") cp = '\'';
                else if (entity == "nbsp") cp = ' ';
                else if (!entity.empty() && entity[0] == '#') {
                    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
                        cp = (uint32_t)strtoul(entity.c_str() + 2, nullptr, 16);
                    else
                        cp = (uint32_t)strtoul(entity.c_str() + 1, nullptr, 10);
                } else handled = false;

                if (handled) {
                    std::string &buf = in_link ? link_text : cur;
                    bool &sp = in_link ? link_space : cur_space;
                    if (cp == ' ') {
                        if (!sp) { buf.push_back(' '); sp = true; }
                    } else {
                        appendUtf8(buf, cp);
                        sp = false;
                    }
                    i = semi + 1;
                    continue;
                }
            }
            appendCh(in_link ? link_text : cur, in_link ? link_space : cur_space, '&');
            i++;
            continue;
        }

        appendCh(in_link ? link_text : cur, in_link ? link_space : cur_space, c);
        i++;
    }

    flushText();
    return blocks;
}

Browser::Browser():
    ESP_Brookesia_PhoneApp("Browser", nullptr, true),
    _addr_input(nullptr),
    _keyboard(nullptr),
    _content(nullptr),
    _status(nullptr),
    _btn_back(nullptr),
    _btn_forward(nullptr),
    _history_pos(-1),
    _fetch_task(nullptr),
    _loading(false)
{
}

Browser::~Browser()
{
}

bool Browser::init(void)
{
    return true;
}

static lv_obj_t *makeToolButton(lv_obj_t *parent, const char *symbol, lv_coord_t w,
                                lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 56);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

bool Browser::run(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);

    /* Thanh công cụ kiểu Safari: Back / Forward / địa chỉ / Reload / Home / Go */
    _btn_back = makeToolButton(screen, LV_SYMBOL_LEFT, 60, onBackClicked, this);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 16, 18);

    _btn_forward = makeToolButton(screen, LV_SYMBOL_RIGHT, 60, onForwardClicked, this);
    lv_obj_align(_btn_forward, LV_ALIGN_TOP_LEFT, 84, 18);

    _addr_input = lv_textarea_create(screen);
    lv_obj_set_size(_addr_input, 560, 56);
    lv_obj_align(_addr_input, LV_ALIGN_TOP_LEFT, 152, 18);
    lv_textarea_set_one_line(_addr_input, true);
    lv_textarea_set_placeholder_text(_addr_input, "Tìm hoặc nhập trang web");
    lv_obj_set_style_text_font(_addr_input, &lv_font_be_vietnam_22, 0);
    lv_obj_set_style_radius(_addr_input, 12, 0);
    lv_obj_add_event_cb(_addr_input, onAddressFocused, LV_EVENT_FOCUSED, this);

    lv_obj_t *btn_reload = makeToolButton(screen, LV_SYMBOL_REFRESH, 60, onReloadClicked, this);
    lv_obj_align(btn_reload, LV_ALIGN_TOP_LEFT, 724, 18);

    lv_obj_t *btn_home = makeToolButton(screen, LV_SYMBOL_HOME, 60, onHomeClicked, this);
    lv_obj_align(btn_home, LV_ALIGN_TOP_LEFT, 792, 18);

    lv_obj_t *btn_go = makeToolButton(screen, "Go", 90, onGoClicked, this);
    lv_obj_align(btn_go, LV_ALIGN_TOP_LEFT, 860, 18);

    _status = lv_label_create(screen);
    lv_label_set_text(_status, "Sẵn sàng");
    lv_obj_align(_status, LV_ALIGN_TOP_LEFT, 18, 84);
    lv_obj_set_style_text_color(_status, lv_color_hex(0x8BD3DD), 0);
    lv_obj_set_style_text_font(_status, &lv_font_be_vietnam_16, 0);

    /* Vùng nội dung cuộn dọc chứa các khối văn bản / liên kết */
    _content = lv_obj_create(screen);
    lv_obj_set_size(_content, 964, 372);
    lv_obj_align(_content, LV_ALIGN_TOP_LEFT, 28, 112);
    lv_obj_set_style_bg_color(_content, lv_color_hex(0xF4F7F8), 0);
    lv_obj_set_style_radius(_content, 8, 0);
    lv_obj_set_style_pad_all(_content, 14, 0);
    lv_obj_set_style_pad_row(_content, 8, 0);
    lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_content, LV_DIR_VER);

    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_text(hint, "Nhập địa chỉ rồi bấm Go. Bấm vào liên kết màu xanh để chuyển trang; dùng nút mũi tên để quay lại / tiến tới.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x172026), 0);
    lv_obj_set_style_text_font(hint, &lv_font_be_vietnam_18, 0);

    _keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(_keyboard, 1024, 250);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_keyboard, _addr_input);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, onKeyboardEvent, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_keyboard, onKeyboardEvent, LV_EVENT_CANCEL, this);

    updateNavButtons();
    return true;
}

bool Browser::back(void)
{
    if (_fetch_task) {
        vTaskDelete(_fetch_task);
        _fetch_task = nullptr;
    }
    notifyCoreClosed();
    return true;
}

bool Browser::close(void)
{
    if (_fetch_task) {
        vTaskDelete(_fetch_task);
        _fetch_task = nullptr;
    }
    _addr_input = nullptr;
    _keyboard = nullptr;
    _content = nullptr;
    _status = nullptr;
    _btn_back = nullptr;
    _btn_forward = nullptr;
    return true;
}

void Browser::setStatus(const char *text)
{
    if (_status) {
        lv_label_set_text(_status, text);
    }
}

void Browser::setAddress(const char *url)
{
    if (_addr_input && url) {
        lv_textarea_set_text(_addr_input, url);
    }
}

void Browser::clearContent()
{
    if (_content) {
        lv_obj_clean(_content);
    }
}

void Browser::updateNavButtons()
{
    if (_btn_back) {
        if (_history_pos > 0) lv_obj_clear_state(_btn_back, LV_STATE_DISABLED);
        else lv_obj_add_state(_btn_back, LV_STATE_DISABLED);
    }
    if (_btn_forward) {
        if (_history_pos >= 0 && _history_pos + 1 < (int)_history.size())
            lv_obj_clear_state(_btn_forward, LV_STATE_DISABLED);
        else
            lv_obj_add_state(_btn_forward, LV_STATE_DISABLED);
    }
}

void Browser::navigate(const char *url, bool push_history)
{
    if (!url || !url[0] || _loading) {
        return;
    }
    std::string full = normalizeUrl(url);
    if (full.empty()) {
        setStatus("Địa chỉ trống");
        return;
    }

    if (push_history) {
        // Cắt bỏ các mục "tiến tới" nếu đang ở giữa lịch sử.
        if (_history_pos >= 0 && _history_pos + 1 < (int)_history.size()) {
            _history.resize(_history_pos + 1);
        }
        _history.push_back(full);
        _history_pos = (int)_history.size() - 1;
    }

    _current_url = full;
    setAddress(full.c_str());
    updateNavButtons();

    _loading = true;
    setStatus("Đang tải...");

    auto *context = new FetchContext();
    context->app = this;
    context->url = full;
    // Cấp phát vùng đệm trang từ PSRAM (dung lượng lớn); dự phòng RAM nội bộ.
    context->body = (char *)heap_caps_malloc(PAGE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!context->body) {
        context->body = (char *)malloc(PAGE_BUFFER_SIZE);
    }
    if (context->body) {
        context->body[0] = '\0';
    }
    context->length = 0;
    context->blocks = nullptr;
    _fetch_task = nullptr;
    xTaskCreate(fetchTask, "browser_fetch", 8192, context, 4, &_fetch_task);
}

void Browser::goToHistory(int new_pos)
{
    if (new_pos < 0 || new_pos >= (int)_history.size() || _loading) {
        return;
    }
    _history_pos = new_pos;
    navigate(_history[new_pos].c_str(), false);
}

void Browser::reload()
{
    if (!_current_url.empty()) {
        navigate(_current_url.c_str(), false);
    }
}

void Browser::renderBlocks(const std::vector<Block> &blocks)
{
    clearContent();
    if (!_content) {
        return;
    }
    if (blocks.empty()) {
        lv_obj_t *lbl = lv_label_create(_content);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_label_set_text(lbl, "(Trang không có nội dung văn bản để hiển thị)");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x556066), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_be_vietnam_18, 0);
        return;
    }

    for (const auto &b : blocks) {
        lv_obj_t *lbl = lv_label_create(_content);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_label_set_text(lbl, b.text.c_str());

        if (b.type == Block::Type::Link) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x2E86FF), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_be_vietnam_18, 0);
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(lbl, lv_color_hex(0xE3EEFB), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, LV_STATE_PRESSED);
            char *href = strdup(b.href.c_str());
            lv_obj_set_user_data(lbl, href);
            lv_obj_add_event_cb(lbl, onLinkClicked, LV_EVENT_CLICKED, this);
            lv_obj_add_event_cb(lbl, onLinkDeleted, LV_EVENT_DELETE, this);
        } else {
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x172026), 0);
            const lv_font_t *font = &lv_font_be_vietnam_18;
            if (b.type == Block::Type::Heading) {
                font = (b.level <= 2) ? &lv_font_be_vietnam_28 : &lv_font_be_vietnam_22;
            }
            lv_obj_set_style_text_font(lbl, font, 0);
        }
    }
    lv_obj_scroll_to_y(_content, 0, LV_ANIM_OFF);
}

void Browser::onGoClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (!app || !app->_addr_input) {
        return;
    }
    if (app->_keyboard) {
        lv_obj_add_flag(app->_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    const char *url = lv_textarea_get_text(app->_addr_input);
    app->navigate(url, true);
}

void Browser::onBackClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (app) app->goToHistory(app->_history_pos - 1);
}

void Browser::onForwardClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (app) app->goToHistory(app->_history_pos + 1);
}

void Browser::onReloadClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (app) app->reload();
}

void Browser::onHomeClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (app) app->navigate(HOME_URL, true);
}

void Browser::onAddressFocused(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (app && app->_keyboard) {
        lv_keyboard_set_textarea(app->_keyboard, app->_addr_input);
        lv_obj_clear_flag(app->_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void Browser::onKeyboardEvent(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (!app) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        onGoClicked(event);
    } else if (code == LV_EVENT_CANCEL && app->_keyboard) {
        lv_obj_add_flag(app->_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void Browser::onLinkClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    lv_obj_t *target = lv_event_get_target(event);
    if (!app || !target) {
        return;
    }
    const char *href = static_cast<const char *>(lv_obj_get_user_data(target));
    if (href && href[0]) {
        app->navigate(href, true);
    }
}

void Browser::onLinkDeleted(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    if (target) {
        void *href = lv_obj_get_user_data(target);
        if (href) {
            free(href);
            lv_obj_set_user_data(target, nullptr);
        }
    }
}

void Browser::fetchTask(void *arg)
{
    auto *context = static_cast<FetchContext *>(arg);
    Browser *app = context->app;

    if (!hasNetworkConnectivity()) {
        context->result = ESP_ERR_INVALID_STATE;
        context->status_code = 0;
        ESP_LOGW(TAG, "No IP yet; skipping request");
        lv_async_call(applyFetchResult, context);
        app->_fetch_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!context->body) {
        context->result = ESP_ERR_NO_MEM;
        context->status_code = 0;
        ESP_LOGE(TAG, "Page buffer alloc failed");
        lv_async_call(applyFetchResult, context);
        app->_fetch_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_config_t config = {};
    config.url = context->url.c_str();
    config.event_handler = httpEventHandler;
    config.user_data = context;
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        // Gửi header giống trình duyệt thật, nếu không nhiều server/CDN trả 400/403.
        esp_http_client_set_header(client, "User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36");
        esp_http_client_set_header(client, "Accept",
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
        esp_http_client_set_header(client, "Accept-Language", "vi,en;q=0.9");
        // Không hỗ trợ giải nén: yêu cầu server trả nội dung nguyên bản.
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_http_client_set_header(client, "Connection", "close");
    }
    esp_err_t result = client ? esp_http_client_perform(client) : ESP_FAIL;
    int status_code = client ? esp_http_client_get_status_code(client) : 0;
    if (client) {
        char final_url[256] = {0};
        if (esp_http_client_get_url(client, final_url, sizeof(final_url)) == ESP_OK && final_url[0]) {
            context->final_url = final_url;
        }
        esp_http_client_cleanup(client);
    }

    context->result = result;
    context->status_code = status_code;

    ESP_LOGI(TAG, "fetch url=%s -> err=%s status=%d bytes=%u",
             context->url.c_str(), esp_err_to_name(result), status_code,
             (unsigned)context->length);

    if (result == ESP_OK && status_code >= 200 && status_code < 400) {
        const std::string &base = context->final_url.empty() ? context->url : context->final_url;
        context->blocks = new std::vector<Block>(parseHtml(context->body, context->length, base));
        ESP_LOGI(TAG, "parsed blocks=%d head=[%.140s]",
                 (int)context->blocks->size(), context->body);
    } else {
        ESP_LOGW(TAG, "HTTP failed: %s status=%d", esp_err_to_name(result), status_code);
    }

    lv_async_call(applyFetchResult, context);
    app->_fetch_task = nullptr;
    vTaskDelete(nullptr);
}

void Browser::applyFetchResult(void *arg)
{
    auto *context = static_cast<FetchContext *>(arg);
    Browser *app = context->app;

    app->_loading = false;

    if (context->result == ESP_OK && context->status_code >= 200 && context->status_code < 400) {
        // Cập nhật URL cuối cùng sau redirect để phân giải link đúng.
        if (!context->final_url.empty()) {
            app->_current_url = context->final_url;
            app->setAddress(context->final_url.c_str());
            if (app->_history_pos >= 0 && app->_history_pos < (int)app->_history.size()) {
                app->_history[app->_history_pos] = context->final_url;
            }
        }
        char st[48];
        snprintf(st, sizeof(st), "Xong (%d)", context->status_code);
        app->setStatus(st);
        if (context->blocks) {
            app->renderBlocks(*context->blocks);
        }
    } else {
        app->setStatus("Tải thất bại");
        app->clearContent();
        if (app->_content) {
            lv_obj_t *lbl = lv_label_create(app->_content);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl, lv_pct(100));
            char msg[160];
            snprintf(msg, sizeof(msg), "Không tải được trang.\n%s\nHTTP: %d",
                     esp_err_to_name(context->result), context->status_code);
            lv_label_set_text(lbl, msg);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xB03030), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_be_vietnam_18, 0);
        }
    }
    app->updateNavButtons();

    delete context->blocks;
    free(context->body);
    delete context;
}
