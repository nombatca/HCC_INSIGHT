/*
 * HCCXiaozhi: xem HCCXiaozhi.hpp.
 *
 * Task:
 *   xz_ctrl  (nhân WORK): máy trạng thái, HTTP OTA / kích hoạt, mở/đóng WebSocket.
 *   xz_mic   (nhân WORK): đọc micro ES7210 (16 kHz, 2 kênh, lấy kênh trái) -> Opus -> WebSocket.
 *   xz_play  (nhân WORK): hàng đợi khung Opus từ máy chủ -> giải mã 16 kHz mono -> loa (2 kênh).
 *   Task của esp_websocket_client: nhận dữ liệu, ghép khung, đẩy sự kiện cho xz_ctrl.
 * Codec để cố định 16 kHz / 16 bit / 2 kênh trong lúc trò chuyện (giống mặc định của product_audio);
 * Opus của máy chủ (thường 24 kHz) được giải mã thẳng ra 16 kHz nên không cần đổi tần số.
 */
#include "HCCXiaozhi.hpp"

#include <string.h>
#include <sys/time.h>

#include <algorithm>
#include <mutex>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#include "hcc_cores.h"
#include "music_player/HCCMusicEngine.h"
#include "product_audio.h"

namespace HCCXiaozhi {
namespace {

const char *TAG = "Xiaozhi";

constexpr const char *DEFAULT_OTA_URL = "https://api.tenclass.net/xiaozhi/ota/";
constexpr const char *NVS_NS = "hcc_xz";
constexpr const char *LANGUAGE = "vi-VN";
constexpr const char *BOARD_TYPE = "hcc-insight-esp32p4";
constexpr int SAMPLE_RATE = 16000;
constexpr int FRAME_MS = 60;
constexpr int FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;  // 960
constexpr int HELLO_TIMEOUT_MS = 10000;
constexpr float MIC_GAIN_DB = 33.0f;  // ES7210: 0..37.5 dB
constexpr int MAX_CHAT = 40;
constexpr int PLAY_QUEUE_LEN = 96;   // ~5.8 s âm thanh 60 ms/khung
constexpr int MAX_WS_MESSAGE = 64 * 1024;

enum class Evt : uint8_t {
    Start,
    Stop,
    Talk,
    End,
    ResetActivation,
    WsConnected,
    WsHello,
    WsClosed,
    TtsStart,
    TtsStop,
};

struct OpusPacket {
    uint16_t len;
    uint8_t data[];
};

// ---- trạng thái dùng chung (bảo vệ bởi s_mutex) ----
std::mutex s_mutex;
State s_state = State::Off;
std::string s_code;
std::string s_activation_msg;
std::string s_emotion = "neutral";
std::string s_error;
std::vector<ChatLine> s_chat;
uint32_t s_chat_version = 0;

// ---- chỉ task xz_ctrl dùng ----
QueueHandle_t s_evt_queue;
esp_websocket_client_handle_t s_ws;
std::string s_session_id;
std::string s_ws_url;
std::string s_ws_token;
std::string s_challenge;
bool s_have_challenge = false;
int64_t s_next_poll_us = 0;
int64_t s_hello_deadline_us = 0;
bool s_pending_listen = false;

// ---- âm thanh ----
QueueHandle_t s_play_queue;
TaskHandle_t s_mic_task;
volatile bool s_mic_on = false;
volatile bool s_play_flush = false;
volatile bool s_play_busy = false;
volatile bool s_accept_audio = false;  // nhận khung Opus từ máy chủ (không nhận khi đang thu)
// Cho hoạt hình nhân vật: mức âm (0..1000) của tiếng trả lời / micro, và "đang nghĩ" (đã nhận stt, chờ tts).
volatile int s_speak_level = 0;
volatile int s_mic_level = 0;
volatile bool s_thinking = false;

// ---- ghép khung WebSocket (task websocket) ----
std::string s_rx_buf;
uint8_t s_rx_opcode = 0;

bool s_inited = false;

void setState(State st)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_state != st) {
        ESP_LOGI(TAG, "Trạng thái: %s -> %s", stateText(s_state), stateText(st));
    }
    s_state = st;
}

State getState(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_state;
}

void setError(const std::string &err)
{
    ESP_LOGE(TAG, "%s", err.c_str());
    std::lock_guard<std::mutex> lk(s_mutex);
    s_error = err;
    s_state = State::Error;
}

void addChat(bool user, const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lk(s_mutex);
    s_chat.push_back({user, text});
    if ((int)s_chat.size() > MAX_CHAT) {
        s_chat.erase(s_chat.begin());
    }
    s_chat_version++;
}

void post(Evt e)
{
    if (s_evt_queue != nullptr) {
        xQueueSend(s_evt_queue, &e, pdMS_TO_TICKS(100));
    }
}

/* -------------------------------- NVS -------------------------------- */

std::string nvsGet(const char *key)
{
    nvs_handle_t h;
    std::string out;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return out;
    }
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0) {
        out.resize(len);
        if (nvs_get_str(h, key, &out[0], &len) == ESP_OK) {
            out.resize(len - 1);
        } else {
            out.clear();
        }
    }
    nvs_close(h);
    return out;
}

void nvsSet(const char *key, const std::string &value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (value.empty()) {
        nvs_erase_key(h, key);
    } else {
        nvs_set_str(h, key, value.c_str());
    }
    nvs_commit(h);
    nvs_close(h);
}

std::string clientId(void)
{
    static std::string uuid;
    if (!uuid.empty()) {
        return uuid;
    }
    uuid = nvsGet("uuid");
    if (uuid.size() == 36) {
        return uuid;
    }
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;  // UUID v4
    b[8] = (b[8] & 0x3F) | 0x80;
    char s[40];
    snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
             b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    uuid = s;
    nvsSet("uuid", uuid);
    return uuid;
}

std::string macAddress(void)
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK || (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
        esp_efuse_mac_get_default(mac);
    }
    char s[20];
    snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return s;
}

std::string currentOtaUrl(void)
{
    std::string url = nvsGet("ota_url");
    return url.empty() ? DEFAULT_OTA_URL : url;
}

/* -------------------------------- HTTP (OTA / kích hoạt) -------------------------------- */

std::string userAgent(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return std::string(BOARD_TYPE) + "/" + app->version;
}

std::string systemInfoJson(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", LANGUAGE);
    cJSON_AddStringToObject(root, "mac_address", macAddress().c_str());
    cJSON_AddStringToObject(root, "uuid", clientId().c_str());
    cJSON_AddStringToObject(root, "chip_model_name", "esp32p4");
    char heap[16];
    snprintf(heap, sizeof(heap), "%u", (unsigned)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "minimum_free_heap_size", heap);
    cJSON *ci = cJSON_AddObjectToObject(root, "chip_info");
    cJSON_AddNumberToObject(ci, "model", chip.model);
    cJSON_AddNumberToObject(ci, "cores", chip.cores);
    cJSON_AddNumberToObject(ci, "revision", chip.revision);
    cJSON_AddNumberToObject(ci, "features", chip.features);
    cJSON *ap = cJSON_AddObjectToObject(root, "application");
    cJSON_AddStringToObject(ap, "name", app->project_name);
    cJSON_AddStringToObject(ap, "version", app->version);
    cJSON_AddStringToObject(ap, "idf_version", app->idf_ver);
    cJSON *board = cJSON_AddObjectToObject(root, "board");
    cJSON_AddStringToObject(board, "type", BOARD_TYPE);
    cJSON_AddStringToObject(board, "name", "HCC INSIGHT");
    char *s = cJSON_PrintUnformatted(root);
    std::string out = s != nullptr ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

// POST JSON, trả mã HTTP (<0 nếu lỗi mạng), nội dung trả về vào *resp.
int httpPost(const std::string &url, const std::string &body, std::string *resp)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == nullptr) {
        return -1;
    }
    const std::string mac = macAddress();
    const std::string cid = clientId();
    const std::string ua = userAgent();
    esp_http_client_set_header(c, "Activation-Version", "1");
    esp_http_client_set_header(c, "Device-Id", mac.c_str());
    esp_http_client_set_header(c, "Client-Id", cid.c_str());
    esp_http_client_set_header(c, "User-Agent", ua.c_str());
    esp_http_client_set_header(c, "Accept-Language", LANGUAGE);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    int status = -1;
    if (esp_http_client_open(c, (int)body.size()) == ESP_OK) {
        if (esp_http_client_write(c, body.data(), (int)body.size()) == (int)body.size() &&
                esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            char buf[512];
            for (;;) {
                const int n = esp_http_client_read(c, buf, sizeof(buf));
                if (n <= 0 || resp->size() > 32 * 1024) {
                    break;
                }
                resp->append(buf, n);
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

// Hỏi máy chủ OTA. true nếu có cấu hình WebSocket dùng được (đã kích hoạt).
bool checkServer(void)
{
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_code.clear();
        s_activation_msg.clear();
    }
    s_have_challenge = false;
    std::string resp;
    const std::string url = currentOtaUrl();
    ESP_LOGI(TAG, "Kiểm tra máy chủ: %s (Device-Id %s)", url.c_str(), macAddress().c_str());
    const int status = httpPost(url, systemInfoJson(), &resp);
    if (status != 200) {
        ESP_LOGE(TAG, "OTA HTTP %d: %.200s", status, resp.c_str());
        if (!s_ws_url.empty()) {
            ESP_LOGW(TAG, "Dùng cấu hình WebSocket đã lưu");
            return true;
        }
        setError(status < 0 ? "Không kết nối được máy chủ Xiaozhi (kiểm tra Wi-Fi)"
                            : "Máy chủ Xiaozhi trả lỗi HTTP " + std::to_string(status));
        return false;
    }
    cJSON *root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        setError("Máy chủ trả dữ liệu không hợp lệ");
        return false;
    }
    bool need_activation = false;
    cJSON *act = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(act)) {
        cJSON *code = cJSON_GetObjectItem(act, "code");
        cJSON *msg = cJSON_GetObjectItem(act, "message");
        cJSON *ch = cJSON_GetObjectItem(act, "challenge");
        std::lock_guard<std::mutex> lk(s_mutex);
        if (cJSON_IsString(code)) {
            s_code = code->valuestring;
            need_activation = true;
        }
        if (cJSON_IsString(msg)) {
            s_activation_msg = msg->valuestring;
        }
        if (cJSON_IsString(ch)) {
            s_challenge = ch->valuestring;
            s_have_challenge = true;
        }
    }
    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        cJSON *u = cJSON_GetObjectItem(ws, "url");
        cJSON *t = cJSON_GetObjectItem(ws, "token");
        if (cJSON_IsString(u)) {
            s_ws_url = u->valuestring;
            nvsSet("ws_url", s_ws_url);
        }
        if (cJSON_IsString(t)) {
            s_ws_token = t->valuestring;
            nvsSet("ws_token", s_ws_token);
        }
    } else if (!need_activation) {
        ESP_LOGW(TAG, "Máy chủ không trả cấu hình websocket (có thể chỉ hỗ trợ MQTT)");
    }
    cJSON_Delete(root);
    if (need_activation) {
        ESP_LOGW(TAG, "Cần kích hoạt, mã: %s", activationCode().c_str());
        return false;
    }
    if (s_ws_url.empty()) {
        setError("Máy chủ không cung cấp địa chỉ WebSocket");
        return false;
    }
    return true;
}

// Báo máy chủ đã nhập mã chưa. 200 = xong, 202 = chưa.
int pollActivation(void)
{
    std::string url = currentOtaUrl();
    url += (url.back() == '/') ? "activate" : "/activate";
    std::string resp;
    const int status = httpPost(url, "{}", &resp);
    ESP_LOGI(TAG, "Kích hoạt: HTTP %d", status);
    return status;
}

/* -------------------------------- WebSocket -------------------------------- */

void sendText(const std::string &text)
{
    if (s_ws != nullptr && esp_websocket_client_is_connected(s_ws)) {
        esp_websocket_client_send_text(s_ws, text.c_str(), (int)text.size(), pdMS_TO_TICKS(1000));
    }
}

void sendListen(const char *state_str)
{
    std::string msg = "{\"session_id\":\"" + s_session_id + "\",\"type\":\"listen\",\"state\":\"" + state_str + "\"";
    if (strcmp(state_str, "start") == 0) {
        msg += ",\"mode\":\"auto\"";
    }
    msg += "}";
    sendText(msg);
}

void sendAbort(void)
{
    sendText("{\"session_id\":\"" + s_session_id + "\",\"type\":\"abort\"}");
}

void flushPlayback(void)
{
    s_play_flush = true;
    OpusPacket *p = nullptr;
    while (s_play_queue != nullptr && xQueueReceive(s_play_queue, &p, 0) == pdTRUE) {
        heap_caps_free(p);
    }
}

void handleJson(const char *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        return;
    }
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char *t = type->valuestring;
    if (strcmp(t, "hello") == 0) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid)) {
            s_session_id = sid->valuestring;  // đọc ở xz_ctrl sau sự kiện WsHello
        }
        post(Evt::WsHello);
    } else if (strcmp(t, "stt") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            ESP_LOGI(TAG, ">> %s", text->valuestring);
            addChat(true, text->valuestring);
            s_thinking = true;
        }
    } else if (strcmp(t, "llm") == 0) {
        cJSON *emo = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emo)) {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_emotion = emo->valuestring;
        }
    } else if (strcmp(t, "tts") == 0) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(st)) {
            if (strcmp(st->valuestring, "start") == 0) {
                s_accept_audio = true;
                s_thinking = false;
                post(Evt::TtsStart);
            } else if (strcmp(st->valuestring, "stop") == 0) {
                post(Evt::TtsStop);
            } else if (strcmp(st->valuestring, "sentence_start") == 0) {
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    addChat(false, text->valuestring);
                }
            }
        }
    } else if (strcmp(t, "alert") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(msg)) {
            addChat(false, msg->valuestring);
        }
    } else {
        ESP_LOGI(TAG, "Tin nhắn khác: %.*s", (int)std::min<size_t>(len, 300), data);
    }
    cJSON_Delete(root);
}

void handleBinary(const uint8_t *data, size_t len)
{
    if (!s_accept_audio || len == 0 || len > 4000 || s_play_queue == nullptr) {
        return;
    }
    auto *p = static_cast<OpusPacket *>(heap_caps_malloc(sizeof(OpusPacket) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        return;
    }
    p->len = (uint16_t)len;
    memcpy(p->data, data, len);
    if (xQueueSend(s_play_queue, &p, pdMS_TO_TICKS(200)) != pdTRUE) {
        heap_caps_free(p);
    }
}

void wsEvent(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    auto *d = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        post(Evt::WsConnected);
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (d->op_code == 0x08) {  // close
            break;
        }
        if (d->op_code != 0x01 && d->op_code != 0x02 && d->op_code != 0x00) {
            break;  // ping/pong
        }
        if (d->payload_offset == 0) {
            if (d->op_code != 0x00) {
                s_rx_opcode = d->op_code;
            }
            s_rx_buf.clear();
            if (d->payload_len > MAX_WS_MESSAGE) {
                ESP_LOGW(TAG, "Bỏ tin nhắn quá lớn (%d byte)", d->payload_len);
                s_rx_opcode = 0;
                break;
            }
            s_rx_buf.reserve(d->payload_len);
        }
        if (s_rx_opcode == 0) {
            break;
        }
        s_rx_buf.append(d->data_ptr, d->data_len);
        if (d->payload_offset + d->data_len >= d->payload_len && d->fin) {
            if (s_rx_opcode == 0x01) {
                handleJson(s_rx_buf.data(), s_rx_buf.size());
            } else if (s_rx_opcode == 0x02) {
                handleBinary(reinterpret_cast<const uint8_t *>(s_rx_buf.data()), s_rx_buf.size());
            }
            s_rx_buf.clear();
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        post(Evt::WsClosed);
        break;
    default:
        break;
    }
}

void closeWs(void)
{
    s_mic_on = false;
    s_thinking = false;
    s_mic_level = 0;
    s_accept_audio = false;
    s_pending_listen = false;
    if (s_ws != nullptr) {
        if (esp_websocket_client_is_connected(s_ws)) {
            esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
        }
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
    }
    flushPlayback();
    s_session_id.clear();
}

bool openWs(void)
{
    closeWs();
    std::string token = s_ws_token;
    if (!token.empty() && token.find(' ') == std::string::npos) {
        token = "Bearer " + token;
    }
    static std::string headers;  // client giữ con trỏ
    headers.clear();
    if (!token.empty()) {
        headers += "Authorization: " + token + "\r\n";
    }
    headers += "Protocol-Version: 1\r\n";
    headers += "Device-Id: " + macAddress() + "\r\n";
    headers += "Client-Id: " + clientId() + "\r\n";

    esp_websocket_client_config_t cfg = {};
    cfg.uri = s_ws_url.c_str();
    cfg.headers = headers.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.task_stack = 6144;
    cfg.task_prio = 6;
    cfg.network_timeout_ms = 10000;
    cfg.disable_auto_reconnect = true;
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == nullptr) {
        setError("Không tạo được kết nối WebSocket");
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, wsEvent, nullptr);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
        setError("Không kết nối được máy chủ trò chuyện");
        return false;
    }
    s_hello_deadline_us = esp_timer_get_time() + (int64_t)HELLO_TIMEOUT_MS * 1000;
    ESP_LOGI(TAG, "Kết nối %s", s_ws_url.c_str());
    return true;
}

void sendHello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON *ap = cJSON_AddObjectToObject(root, "audio_params");
    cJSON_AddStringToObject(ap, "format", "opus");
    cJSON_AddNumberToObject(ap, "sample_rate", SAMPLE_RATE);
    cJSON_AddNumberToObject(ap, "channels", 1);
    cJSON_AddNumberToObject(ap, "frame_duration", FRAME_MS);
    char *s = cJSON_PrintUnformatted(root);
    if (s != nullptr) {
        sendText(s);
        cJSON_free(s);
    }
    cJSON_Delete(root);
}

void prepareAudio(void)
{
    // Dừng các bộ phát khác đang dùng I2S, đặt codec 16 kHz / 16 bit / 2 kênh.
    hcc_music_engine_stop();
    audio_player_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    product_audio_codec_set_fs(SAMPLE_RATE, 16, I2S_SLOT_MODE_STEREO);
    product_audio_codec_volume_set(product_audio_codec_volume_get(), nullptr);
    product_audio_codec_mute_set(false);
    // Đo trên board: với gain mặc định đỉnh micro chỉ vài trăm (trên 32767) -> máy chủ không nhận ra lời nói.
    product_audio_codec_set_in_gain(MIC_GAIN_DB);
}

void startListening(void)
{
    s_pending_listen = false;
    s_thinking = false;
    s_accept_audio = false;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_emotion = "neutral";
    }
    sendListen("start");
    s_mic_on = true;
    if (s_mic_task != nullptr) {
        xTaskNotifyGive(s_mic_task);
    }
    setState(State::Listening);
}

/* -------------------------------- điều khiển -------------------------------- */

void beginCheck(void)
{
    setState(State::Checking);
    if (checkServer()) {
        setState(State::Ready);
    } else if (!activationCode().empty()) {
        setState(State::Activating);
        s_next_poll_us = esp_timer_get_time() + 3 * 1000 * 1000;
    }
    // else: setError đã đặt trạng thái Error
}

void handle(Evt e)
{
    const State st = getState();
    switch (e) {
    case Evt::Start:
        s_ws_url = nvsGet("ws_url");
        s_ws_token = nvsGet("ws_token");
        beginCheck();
        break;
    case Evt::Stop:
        closeWs();
        setState(State::Off);
        break;
    case Evt::ResetActivation:
        closeWs();
        s_ws_url.clear();
        s_ws_token.clear();
        nvsSet("ws_url", "");
        nvsSet("ws_token", "");
        beginCheck();
        break;
    case Evt::Talk:
        if (st == State::Ready) {
            setState(State::Connecting);
            prepareAudio();
            openWs();
        } else if (st == State::Error) {
            beginCheck();
        } else if (st == State::Listening) {
            closeWs();
            setState(State::Ready);
        } else if (st == State::Speaking) {
            // Ngắt lời: bỏ phần còn lại và nghe tiếp ngay.
            sendAbort();
            s_accept_audio = false;
            flushPlayback();
            startListening();
        }
        break;
    case Evt::End:
        if (st == State::Connecting || st == State::Listening || st == State::Speaking) {
            closeWs();
            setState(State::Ready);
        }
        break;
    case Evt::WsConnected:
        if (st == State::Connecting) {
            sendHello();
        }
        break;
    case Evt::WsHello:
        if (st == State::Connecting) {
            ESP_LOGI(TAG, "Phiên trò chuyện: %s", s_session_id.c_str());
            startListening();
        }
        break;
    case Evt::WsClosed:
        if (st == State::Connecting) {
            closeWs();
            setError("Không kết nối được máy chủ trò chuyện (token hết hạn? thử Kích hoạt lại)");
        } else if (st == State::Listening || st == State::Speaking) {
            closeWs();
            setState(State::Ready);
        }
        break;
    case Evt::TtsStart:
        if (st == State::Listening || st == State::Speaking) {
            s_mic_on = false;
            s_pending_listen = false;
            setState(State::Speaking);
        }
        break;
    case Evt::TtsStop:
        if (st == State::Speaking) {
            s_pending_listen = true;  // chờ phát hết hàng đợi rồi mới nghe tiếp
        }
        break;
    }
}

void tick(void)
{
    const State st = getState();
    const int64_t now = esp_timer_get_time();
    if (st == State::Activating && now >= s_next_poll_us) {
        int status = 202;
        if (s_have_challenge) {
            status = pollActivation();
        }
        if (status == 200 || !s_have_challenge) {
            beginCheck();  // hỏi lại để lấy WebSocket (hoặc mã mới)
        }
        if (getState() == State::Activating) {
            s_next_poll_us = esp_timer_get_time() + 3 * 1000 * 1000;
        }
    } else if (st == State::Connecting && now > s_hello_deadline_us) {
        closeWs();
        setError("Máy chủ không phản hồi (hết 10 giây chờ)");
    } else if (st == State::Speaking && s_pending_listen && !s_play_busy &&
               uxQueueMessagesWaiting(s_play_queue) == 0) {
        startListening();
    }
}

void ctrlTask(void *arg)
{
    (void)arg;
    Evt e;
    for (;;) {
        if (xQueueReceive(s_evt_queue, &e, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle(e);
        }
        tick();
    }
}

/* -------------------------------- âm thanh -------------------------------- */

void micTask(void *arg)
{
    (void)arg;
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = SAMPLE_RATE;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    // Giống xiaozhi-esp32 (audio_service.h): complexity 0. Complexity >= 2 bật bộ lượng tử hoá
    // "delayed decision" của SILK (NSQ_del_dec) dùng rất nhiều stack -> tràn stack 24 KB (đã crash).
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    cfg.complexity = 0;
    cfg.enable_dtx = true;
    cfg.enable_vbr = true;
    void *enc = nullptr;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &enc) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Không mở được bộ mã hoá Opus");
        vTaskDelete(nullptr);
        return;
    }
    int in_size = 0;
    int out_size = 0;
    esp_opus_enc_get_frame_size(enc, &in_size, &out_size);
    if (in_size != FRAME_SAMPLES * 2) {
        ESP_LOGW(TAG, "Opus in_size %d (mong đợi %d)", in_size, FRAME_SAMPLES * 2);
    }
    const size_t stereo_bytes = FRAME_SAMPLES * 2 * sizeof(int16_t);
    auto *stereo = static_cast<int16_t *>(heap_caps_malloc(stereo_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto *mono = static_cast<int16_t *>(heap_caps_malloc(FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL));
    auto *out = static_cast<uint8_t *>(heap_caps_malloc(out_size > 0 ? out_size : 1500, MALLOC_CAP_INTERNAL));
    if (stereo == nullptr || mono == nullptr || out == nullptr) {
        ESP_LOGE(TAG, "Thiếu bộ nhớ cho micro");
        vTaskDelete(nullptr);
        return;
    }
    bool was_on = false;
    // Chẩn đoán: mức âm 2 kênh + số khung đã gửi, in mỗi ~1 giây khi đang nghe.
    int64_t sum_l = 0;
    int64_t sum_r = 0;
    int peak_l = 0;
    int peak_r = 0;
    int frames = 0;
    int sent = 0;
    int sent_bytes = 0;
    for (;;) {
        if (!s_mic_on) {
            was_on = false;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (!was_on) {
            esp_opus_enc_reset(enc);
            was_on = true;
        }
        size_t got = 0;
        if (product_audio_i2s_read(stereo, stereo_bytes, &got, 200) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // Chưa rõ micro của board nối vào kênh nào của ES7210: trộn 2 kênh (kênh im lặng chỉ làm nhỏ 6 dB).
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
            const int l = stereo[i * 2];
            const int r = stereo[i * 2 + 1];
            mono[i] = (int16_t)((l + r) / 2);
            sum_l += l < 0 ? -l : l;
            sum_r += r < 0 ? -r : r;
            peak_l = std::max(peak_l, l < 0 ? -l : l);
            peak_r = std::max(peak_r, r < 0 ? -r : r);
        }
        {
            int64_t a = 0;
            for (int i = 0; i < FRAME_SAMPLES; ++i) {
                a += mono[i] < 0 ? -mono[i] : mono[i];
            }
            s_mic_level = (int)std::min<int64_t>(1000, a / FRAME_SAMPLES * 1000 / 1500);
        }
        if (++frames >= 16) {
            ESP_LOGI(TAG, "Micro: trái tb %d đỉnh %d | phải tb %d đỉnh %d | đã gửi %d khung (%d byte)",
                     (int)(sum_l / (frames * FRAME_SAMPLES)), peak_l, (int)(sum_r / (frames * FRAME_SAMPLES)), peak_r,
                     sent, sent_bytes);
            sum_l = sum_r = 0;
            peak_l = peak_r = 0;
            frames = sent = sent_bytes = 0;
        }
        esp_audio_enc_in_frame_t in = {.buffer = reinterpret_cast<uint8_t *>(mono), .len = FRAME_SAMPLES * 2};
        esp_audio_enc_out_frame_t o = {.buffer = out, .len = (uint32_t)(out_size > 0 ? out_size : 1500)};
        if (esp_opus_enc_process(enc, &in, &o) != ESP_AUDIO_ERR_OK || o.encoded_bytes == 0) {
            continue;
        }
        if (s_mic_on && s_ws != nullptr && esp_websocket_client_is_connected(s_ws)) {
            if (esp_websocket_client_send_bin(s_ws, reinterpret_cast<const char *>(out), (int)o.encoded_bytes,
                                              pdMS_TO_TICKS(200)) > 0) {
                sent++;
                sent_bytes += (int)o.encoded_bytes;
            }
        }
    }
}

void playTask(void *arg)
{
    (void)arg;
    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate = SAMPLE_RATE;  // giải mã thẳng ra 16 kHz dù máy chủ mã hoá 24 kHz
    cfg.channel = 1;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    void *dec = nullptr;
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Không mở được bộ giải mã Opus");
        vTaskDelete(nullptr);
        return;
    }
    constexpr int MAX_SAMPLES = SAMPLE_RATE * 120 / 1000;  // khung dài nhất 120 ms
    uint32_t pcm_cap = MAX_SAMPLES * sizeof(int16_t);
    auto *pcm = static_cast<int16_t *>(heap_caps_malloc(pcm_cap, MALLOC_CAP_INTERNAL));
    auto *stereo = static_cast<int16_t *>(heap_caps_malloc(MAX_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL));
    if (pcm == nullptr || stereo == nullptr) {
        ESP_LOGE(TAG, "Thiếu bộ nhớ cho loa");
        vTaskDelete(nullptr);
        return;
    }
    bool wrote = false;
    for (;;) {
        OpusPacket *p = nullptr;
        if (xQueueReceive(s_play_queue, &p, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (wrote) {
                // Hết dữ liệu: đẩy im lặng để DMA I2S không lặp lại đoạn cuối.
                memset(stereo, 0, MAX_SAMPLES * 2 * sizeof(int16_t));
                size_t w = 0;
                product_audio_i2s_write(stereo, MAX_SAMPLES * 2 * sizeof(int16_t), &w, 200);
                wrote = false;
            }
            s_play_busy = false;
            s_speak_level = 0;
            continue;
        }
        s_play_busy = true;
        if (s_play_flush) {
            s_play_flush = false;
            esp_opus_dec_reset(dec);
        }
        esp_audio_dec_in_raw_t raw = {.buffer = p->data, .len = p->len};
        esp_audio_dec_out_frame_t out = {.buffer = reinterpret_cast<uint8_t *>(pcm), .len = pcm_cap};
        esp_audio_dec_info_t info = {};
        const esp_audio_err_t ret = esp_opus_dec_decode(dec, &raw, &out, &info);
        heap_caps_free(p);
        if (ret != ESP_AUDIO_ERR_OK || out.decoded_size == 0 || !s_accept_audio) {
            continue;
        }
        const int n = (int)(out.decoded_size / sizeof(int16_t));
        int64_t a = 0;
        for (int i = 0; i < n && i < MAX_SAMPLES; ++i) {
            stereo[i * 2] = pcm[i];
            stereo[i * 2 + 1] = pcm[i];
            a += pcm[i] < 0 ? -pcm[i] : pcm[i];
        }
        // Mức âm của khung sắp phát (write chặn đúng thời lượng khung) -> miệng nhân vật khớp tiếng.
        s_speak_level = n > 0 ? (int)std::min<int64_t>(1000, a / n * 1000 / 4000) : 0;
        size_t w = 0;
        product_audio_i2s_write(stereo, (size_t)std::min(n, MAX_SAMPLES) * 2 * sizeof(int16_t), &w, portMAX_DELAY);
        wrote = true;
    }
}

void ensureInit(void)
{
    if (s_inited) {
        return;
    }
    s_evt_queue = xQueueCreate(16, sizeof(Evt));
    s_play_queue = xQueueCreate(PLAY_QUEUE_LEN, sizeof(OpusPacket *));
    xTaskCreatePinnedToCore(ctrlTask, "xz_ctrl", 8192, nullptr, 5, nullptr, HCC_CORE_WORK);
    // Opus (mã hoá / giải mã) dùng khá nhiều stack.
    xTaskCreatePinnedToCore(micTask, "xz_mic", 24 * 1024, nullptr, 6, &s_mic_task, HCC_CORE_WORK);
    xTaskCreatePinnedToCore(playTask, "xz_play", 16 * 1024, nullptr, 6, nullptr, HCC_CORE_WORK);
    s_inited = true;
}

}  // namespace

/* -------------------------------- API -------------------------------- */

void start(void)
{
    ensureInit();
    setState(State::Checking);
    post(Evt::Start);
}

void stop(void)
{
    if (s_inited) {
        post(Evt::Stop);
    }
}

void talk(void)
{
    post(Evt::Talk);
}

void endSession(void)
{
    post(Evt::End);
}

void resetActivation(void)
{
    post(Evt::ResetActivation);
}

std::string otaUrl(void)
{
    return currentOtaUrl();
}

void setOtaUrl(const std::string &url)
{
    nvsSet("ota_url", url == DEFAULT_OTA_URL ? std::string() : url);
    resetActivation();
}

State state(void)
{
    return getState();
}

const char *stateText(State st)
{
    switch (st) {
    case State::Off: return "Đã tắt";
    case State::Checking: return "Đang kết nối máy chủ...";
    case State::Activating: return "Chờ kích hoạt";
    case State::Ready: return "Sẵn sàng";
    case State::Connecting: return "Đang kết nối...";
    case State::Listening: return "Đang nghe...";
    case State::Speaking: return "Đang trả lời...";
    case State::Error: return "Lỗi";
    }
    return "";
}

std::string activationCode(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_code;
}

std::string activationMessage(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_activation_msg;
}

std::string emotion(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_emotion;
}

std::string lastError(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_error;
}

std::string deviceId(void)
{
    return macAddress();
}

uint32_t chatVersion(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_chat_version;
}

std::vector<ChatLine> chat(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_chat;
}

void clearChat(void)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    s_chat.clear();
    s_chat_version++;
}

int speakLevel(void)
{
    return s_speak_level;
}

int micLevel(void)
{
    return s_mic_on ? s_mic_level : 0;
}

bool thinking(void)
{
    return s_thinking;
}

int volume(void)
{
    return product_audio_codec_volume_get();
}

void setVolume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    product_audio_codec_volume_set(v, nullptr);
}

}  // namespace HCCXiaozhi
