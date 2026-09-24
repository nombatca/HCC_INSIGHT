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

static const char *TAG = "Browser";
static constexpr size_t PAGE_BUFFER_SIZE = 32768;

namespace {

struct FetchContext {
    Browser *app;
    char url[256];
    char *body;
    size_t length;
    esp_err_t result;
    int status_code;
};

esp_err_t httpEventHandler(esp_http_client_event_t *event)
{
    auto *context = static_cast<FetchContext *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ERROR) {
        ESP_LOGW(TAG, "HTTP_EVENT_ERROR while fetching %s", context ? context->url : "?");
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data && context->body) {
        const size_t available = PAGE_BUFFER_SIZE - 1 - context->length;
        const size_t copy_size = event->data_len < available ? event->data_len : available;
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

// Strips <script>/<style>, removes remaining tags, decodes entities, and turns block tags into newlines
// so an HTML page reads like plain article text instead of raw markup/JSON.
std::string htmlToReadableText(const char *html, size_t html_len)
{
    static const char *skip_tags[] = {"style", "noscript"};
    static const char *block_tags[] = {
        "p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6",
        "tr", "article", "section", "header", "footer", "ul", "ol", "table", "hr"
    };

    std::string out;
    out.reserve(html_len / 2 + 64);
    size_t i = 0;
    bool pending_newline = false;
    bool last_was_space = true;

    auto flushPendingNewline = [&]() {
        if (pending_newline) {
            if (!out.empty() && out.back() != '\n') {
                out.push_back('\n');
            }
            pending_newline = false;
            last_was_space = true;
        }
    };

    while (i < html_len) {
        char c = html[i];
        if (c == '<') {
            size_t tag_start = i + 1;
            bool closing = (tag_start < html_len && html[tag_start] == '/');
            size_t name_start = closing ? tag_start + 1 : tag_start;
            size_t name_end = name_start;
            while (name_end < html_len && isalnum((unsigned char)html[name_end])) {
                name_end++;
            }
            size_t name_len = name_end - name_start;

            size_t gt = i;
            while (gt < html_len && html[gt] != '>') {
                gt++;
            }
            if (gt >= html_len) {
                break;
            }

            bool skip = false;
            for (auto tag : skip_tags) {
                if (name_len == strlen(tag) && strncasecmp(html + name_start, tag, name_len) == 0) {
                    skip = true;
                    break;
                }
            }
            // Only drop <script> blocks that run code; keep JSON payload scripts, since
            // pages often embed the real article text in a JSON blob rather than in tags.
            bool is_script_tag = (name_len == 6 && strncasecmp(html + name_start, "script", name_len) == 0);
            if (!skip && !closing && is_script_tag) {
                std::string tag_attrs(html + name_start, gt - name_start);
                for (auto &ch : tag_attrs) {
                    ch = (char)tolower((unsigned char)ch);
                }
                skip = (tag_attrs.find("json") == std::string::npos);
            }
            if (skip && !closing) {
                char close_tag[16];
                snprintf(close_tag, sizeof(close_tag), "</%.*s", (int)name_len, html + name_start);
                size_t close_len = strlen(close_tag);
                size_t pos = html_len;
                for (size_t j = gt + 1; j + close_len <= html_len; ++j) {
                    if (strncasecmp(html + j, close_tag, close_len) == 0) {
                        pos = j;
                        break;
                    }
                }
                if (pos == html_len) {
                    i = html_len;
                } else {
                    size_t close_gt = pos;
                    while (close_gt < html_len && html[close_gt] != '>') {
                        close_gt++;
                    }
                    i = (close_gt < html_len) ? close_gt + 1 : html_len;
                }
                continue;
            }

            bool is_block = false;
            for (auto tag : block_tags) {
                if (name_len == strlen(tag) && strncasecmp(html + name_start, tag, name_len) == 0) {
                    is_block = true;
                    break;
                }
            }
            if (is_block) {
                pending_newline = true;
            }
            i = gt + 1;
            continue;
        }

        if (c == '&') {
            size_t semi = i + 1;
            size_t limit = (i + 10 < html_len) ? i + 10 : html_len;
            while (semi < limit && html[semi] != ';') {
                semi++;
            }
            if (semi < html_len && html[semi] == ';') {
                std::string entity(html + i + 1, semi - i - 1);
                uint32_t codepoint = 0;
                bool handled = true;
                if (entity == "amp") {
                    codepoint = '&';
                } else if (entity == "lt") {
                    codepoint = '<';
                } else if (entity == "gt") {
                    codepoint = '>';
                } else if (entity == "quot") {
                    codepoint = '"';
                } else if (entity == "apos") {
                    codepoint = '\'';
                } else if (entity == "nbsp") {
                    codepoint = ' ';
                } else if (!entity.empty() && entity[0] == '#') {
                    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
                        codepoint = (uint32_t)strtoul(entity.c_str() + 2, nullptr, 16);
                    } else {
                        codepoint = (uint32_t)strtoul(entity.c_str() + 1, nullptr, 10);
                    }
                } else {
                    handled = false;
                }

                if (handled) {
                    flushPendingNewline();
                    if (codepoint == ' ') {
                        if (!last_was_space) {
                            out.push_back(' ');
                            last_was_space = true;
                        }
                    } else {
                        appendUtf8(out, codepoint);
                        last_was_space = false;
                    }
                    i = semi + 1;
                    continue;
                }
            }
            out.push_back('&');
            last_was_space = false;
            i++;
            continue;
        }

        unsigned char uc = (unsigned char)c;
        if (uc == '\r' || uc == '\n' || uc == '\t' || uc == ' ') {
            if (!last_was_space) {
                flushPendingNewline();
                if (!pending_newline) {
                    out.push_back(' ');
                }
                last_was_space = true;
            }
            i++;
            continue;
        }

        flushPendingNewline();
        out.push_back(c);
        last_was_space = false;
        i++;
    }

    return out;
}

} // namespace

Browser::Browser():
    ESP_Brookesia_PhoneApp("Browser", nullptr, true),
    _url_input(nullptr),
    _keyboard(nullptr),
    _page(nullptr),
    _status(nullptr),
    _fetch_task(nullptr)
{
}

Browser::~Browser()
{
}

bool Browser::init(void)
{
    return true;
}

bool Browser::run(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);

    _url_input = lv_textarea_create(screen);
    lv_obj_set_size(_url_input, 690, 62);
    lv_obj_align(_url_input, LV_ALIGN_TOP_LEFT, 28, 24);
    lv_textarea_set_one_line(_url_input, true);
    lv_textarea_set_placeholder_text(_url_input, "http://example.com");
    lv_textarea_set_text(_url_input, "http://example.com");
    lv_obj_set_style_text_font(_url_input, &lv_font_be_vietnam_22, 0);

    lv_obj_t *open_button = lv_btn_create(screen);
    lv_obj_set_size(open_button, 120, 62);
    lv_obj_align(open_button, LV_ALIGN_TOP_RIGHT, -170, 24);
    lv_obj_t *open_label = lv_label_create(open_button);
    lv_label_set_text(open_label, "Open");
    lv_obj_center(open_label);
    lv_obj_add_event_cb(open_button, onOpenClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *clear_button = lv_btn_create(screen);
    lv_obj_set_size(clear_button, 120, 62);
    lv_obj_align(clear_button, LV_ALIGN_TOP_RIGHT, -28, 24);
    lv_obj_t *clear_label = lv_label_create(clear_button);
    lv_label_set_text(clear_label, "Clear");
    lv_obj_center(clear_label);
    lv_obj_add_event_cb(clear_button, onClearClicked, LV_EVENT_CLICKED, this);

    _status = lv_label_create(screen);
    lv_label_set_text(_status, "Ready");
    lv_obj_align(_status, LV_ALIGN_TOP_LEFT, 30, 105);
    lv_obj_set_style_text_color(_status, lv_color_hex(0x8BD3DD), 0);
    lv_obj_set_style_text_font(_status, &lv_font_be_vietnam_18, 0);

    _page = lv_textarea_create(screen);
    lv_obj_set_size(_page, 964, 390);
    lv_obj_align(_page, LV_ALIGN_TOP_LEFT, 28, 140);
    lv_textarea_set_text(_page, "Enter a URL and press Open.\n\nThis lightweight browser displays page text over HTTP/HTTPS.");
    lv_textarea_set_cursor_click_pos(_page, false);
    lv_obj_set_style_text_font(_page, &lv_font_be_vietnam_18, 0);
    lv_obj_set_style_bg_color(_page, lv_color_hex(0xF4F7F8), 0);
    lv_obj_set_style_text_color(_page, lv_color_hex(0x172026), 0);

    _keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(_keyboard, 964, 250);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_keyboard, _url_input);
    lv_obj_add_event_cb(_keyboard, onKeyboardEvent, LV_EVENT_READY, this);

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
    return true;
}

void Browser::setStatus(const char *text)
{
    if (_status) {
        lv_label_set_text(_status, text);
    }
}

void Browser::setPageText(const char *text)
{
    if (_page) {
        lv_textarea_set_text(_page, text ? text : "No page content");
    }
}

void Browser::onOpenClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (!app || app->_fetch_task) {
        return;
    }
    const char *url = lv_textarea_get_text(app->_url_input);
    if (!url || !url[0]) {
        app->setStatus("URL is empty");
        return;
    }
    app->fetchUrl(url);
}

void Browser::onClearClicked(lv_event_t *event)
{
    auto *app = static_cast<Browser *>(lv_event_get_user_data(event));
    if (!app) {
        return;
    }
    lv_textarea_set_text(app->_page, "");
    app->setStatus("Ready");
}

void Browser::onKeyboardEvent(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        onOpenClicked(event);
    }
}

void Browser::fetchUrl(const char *url)
{
    auto *context = new FetchContext{};
    context->app = this;
    strncpy(context->url, url, sizeof(context->url) - 1);
    context->body = new char[PAGE_BUFFER_SIZE]{};
    _fetch_task = nullptr;
    xTaskCreate(fetchTask, "browser_fetch", 8192, context, 4, &_fetch_task);
}

void Browser::fetchTask(void *arg)
{
    auto *context = static_cast<FetchContext *>(arg);
    Browser *app = context->app;

    if (!hasNetworkConnectivity()) {
        app->setStatus("Load failed");
        context->result = ESP_ERR_INVALID_STATE;
        context->status_code = 0;
        ESP_LOGW(TAG, "HTTP request skipped: Wi-Fi has no IP address yet");
        lv_async_call(applyFetchResult, context);
        app->_fetch_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    app->setStatus("Loading...");

    esp_http_client_config_t config = {};
    config.url = context->url;
    config.event_handler = httpEventHandler;
    config.user_data = context;
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = client ? esp_http_client_perform(client) : ESP_FAIL;
    int status_code = client ? esp_http_client_get_status_code(client) : 0;
    if (client) {
        esp_http_client_cleanup(client);
    }

    context->result = result;
    context->status_code = status_code;
    if (!(result == ESP_OK && status_code >= 200 && status_code < 400)) {
        ESP_LOGW(TAG, "HTTP request failed: %s status=%d url=%s", esp_err_to_name(result), status_code, context->url);
    }

    lv_async_call(applyFetchResult, context);
    app->_fetch_task = nullptr;
    vTaskDelete(nullptr);
}

void Browser::applyFetchResult(void *arg)
{
    auto *context = static_cast<FetchContext *>(arg);
    Browser *app = context->app;
    if (context->result == ESP_OK && context->status_code >= 200 && context->status_code < 400) {
        app->setStatus("Loaded");
        std::string readable = htmlToReadableText(context->body, context->length);
        app->setPageText(readable.c_str());
    } else {
        char error_text[128];
        snprintf(error_text, sizeof(error_text), "Request failed\n%s\nHTTP status: %d",
                 esp_err_to_name(context->result), context->status_code);
        app->setStatus("Load failed");
        app->setPageText(error_text);
    }
    delete[] context->body;
    delete context;
}
