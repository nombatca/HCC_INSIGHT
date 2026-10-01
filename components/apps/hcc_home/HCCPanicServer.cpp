#include "HCCPanicServer.hpp"
#include "HCCPanicStorage.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "HCCPanicAI.hpp"

static const char *TAG = "PanicSrv";
static const char *RAW_DIR = "panic/raw";
static const char *MDNS_HOST = "hccpanic";  // -> hccpanic.local
static constexpr int MAX_UPLOAD = 2 * 1024 * 1024;  // trần upload (đủ cho ảnh panic)

static httpd_handle_t s_server = nullptr;
static uint16_t s_port = 80;
static bool s_mdns_started = false;

// Panic nhận từ iPhone giữ trong RAM (không ghi SD). Bảo vệ bằng mutex.
static SemaphoreHandle_t s_recv_mtx = nullptr;
static std::string s_recv_name;
static std::string s_recv_raw;
static bool s_recv_is_image = false;
static std::string s_recv_mime;
static volatile uint32_t s_recv_serial = 0;

static void setReceived(const std::string &name, const std::string &raw,
                        bool is_image, const std::string &mime)
{
    if (!s_recv_mtx) s_recv_mtx = xSemaphoreCreateMutex();
    if (s_recv_mtx) xSemaphoreTake(s_recv_mtx, portMAX_DELAY);
    s_recv_name = name;
    s_recv_raw = raw;
    s_recv_is_image = is_image;
    s_recv_mime = mime;
    s_recv_serial = s_recv_serial + 1;
    if (s_recv_mtx) xSemaphoreGive(s_recv_mtx);
}

uint32_t HCCPanicServer::receivedSerial()
{
    return s_recv_serial;
}

bool HCCPanicServer::getReceived(std::string &name, std::string &raw)
{
    if (!s_recv_mtx) return false;
    xSemaphoreTake(s_recv_mtx, portMAX_DELAY);
    bool has = !s_recv_raw.empty();
    if (has) { name = s_recv_name; raw = s_recv_raw; }
    xSemaphoreGive(s_recv_mtx);
    return has;
}

bool HCCPanicServer::receivedIsImage(std::string *mime)
{
    if (!s_recv_mtx) return false;
    xSemaphoreTake(s_recv_mtx, portMAX_DELAY);
    bool img = s_recv_is_image && !s_recv_raw.empty();
    if (img && mime) *mime = s_recv_mime;
    xSemaphoreGive(s_recv_mtx);
    return img;
}

// Nối ":port" chỉ khi khác 80 (để địa chỉ gọn còn http://host).
static void appendPort(char *out, size_t out_size)
{
    if (s_port != 80) {
        size_t len = strlen(out);
        snprintf(out + len, out_size - len, ":%u", s_port);
    }
}

// ESP32-P4 không có Wi-Fi trong chip (Wi-Fi qua C6/esp_hosted) nên mDNS không có sẵn
// interface Wi-Fi STA: phải tự đăng ký netif và bật/announce IPv4 mỗi khi có IP.
static esp_netif_t *s_mdns_netif = nullptr;

static void mdnsEnableOn(esp_netif_t *netif)
{
    if (netif == nullptr) return;
    if (s_mdns_netif != netif) {
        esp_err_t err = mdns_register_netif(netif);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE = đã đăng ký
            ESP_LOGW(TAG, "mdns_register_netif failed: %s", esp_err_to_name(err));
            return;
        }
        s_mdns_netif = netif;
    }
    esp_err_t err = mdns_netif_action(netif, (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
    ESP_LOGI(TAG, "mDNS on Wi-Fi: %s", esp_err_to_name(err));
}

static void onIpEvent(void *, esp_event_base_t, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        mdnsEnableOn(static_cast<ip_event_got_ip_t *>(data)->esp_netif);
    } else if (id == IP_EVENT_STA_LOST_IP && s_mdns_netif) {
        mdns_netif_action(s_mdns_netif, MDNS_EVENT_DISABLE_IP4);
    }
}

// Khởi tạo mDNS để truy cập qua http://hccpanic.local:<port>
static void startMdns(uint16_t port)
{
    if (s_mdns_started) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed");
        return;
    }
    mdns_hostname_set(MDNS_HOST);
    mdns_instance_name_set("HCC INSIGHT Panic");
    mdns_service_add(nullptr, "_http", "_tcp", port, nullptr, 0);
    s_mdns_started = true;
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onIpEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, onIpEvent, nullptr);
    // Wi-Fi thường đã có IP trước khi mở màn PANIC: bật mDNS ngay trên netif đó.
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        mdnsEnableOn(sta);
    }
    ESP_LOGI(TAG, "mDNS started: http://%s.local:%u", MDNS_HOST, port);
}

const char *HCCPanicServer::rawDir() { return RAW_DIR; }

// Trang form đơn giản: dán text hoặc chọn file .ips rồi POST raw sang /panic.
static const char *FORM_HTML =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HCC INSIGHT - Panic Upload</title></head><body style='font-family:sans-serif;max-width:640px;margin:24px auto;padding:0 12px'>"
    "<h2>HCC INSIGHT - Gui Panic Log</h2>"
    "<p>Chon file .ips hoac dan noi dung, roi bam Gui. File se duoc luu vao may.</p>"
    "<input type='file' id='f' accept='.ips,.txt,.log'><br><br>"
    "<textarea id='t' rows='10' style='width:100%' placeholder='Hoac dan raw panic o day...'></textarea><br><br>"
    "<button onclick='send()' style='padding:10px 18px;font-size:16px'>Gui text/.ips</button>"
    "<pre id='r'></pre>"
    "<hr>"
    "<h3>Gui ANH panic</h3>"
    "<p>Chup/chon anh chup man hinh panic. AI se doc anh de phan tich.</p>"
    "<input type='file' id='im' accept='image/*'><br><br>"
    "<button onclick='sendimg()' style='padding:10px 18px;font-size:16px'>Gui anh</button>"
    "<pre id='ir'></pre>"
    "<hr>"
    "<h3>API key AI</h3>"
    "<p>Nhap API key de bat AI Analyze (OpenAI sk-... hoac Claude sk-ant-...). Key duoc luu vao may.</p>"
    "<input type='password' id='k' style='width:100%;padding:8px' placeholder='sk-... (OpenAI) hoac sk-ant-... (Claude)'><br><br>"
    "<button onclick='savekey()' style='padding:10px 18px;font-size:16px'>Luu API key</button>"
    "<pre id='kr'></pre>"
    "<script>"
    "async function send(){var f=document.getElementById('f').files[0];var body;var name='';"
    "if(f){body=await f.text();name='?name='+encodeURIComponent(f.name);}else{body=document.getElementById('t').value;}"
    "if(!body){document.getElementById('r').textContent='Chua co noi dung';return;}"
    "var res=await fetch('/panic'+name,{method:'POST',body:body});"
    "document.getElementById('r').textContent=await res.text();}"
    "async function savekey(){var k=document.getElementById('k').value.trim();"
    "if(!k){document.getElementById('kr').textContent='Chua nhap key';return;}"
    "var res=await fetch('/apikey',{method:'POST',body:k});"
    "document.getElementById('kr').textContent=await res.text();"
    "document.getElementById('k').value='';}"
    "function toJpeg(file){return new Promise(function(resolve,reject){"
    "var img=new Image();img.onload=function(){var m=1600;var w=img.width,h=img.height;"
    "if(w>m||h>m){if(w>=h){h=Math.round(h*m/w);w=m;}else{w=Math.round(w*m/h);h=m;}}"
    "var c=document.createElement('canvas');c.width=w;c.height=h;"
    "c.getContext('2d').drawImage(img,0,0,w,h);"
    "c.toBlob(function(b){URL.revokeObjectURL(img.src);b?resolve(b):reject('encode');},'image/jpeg',0.85);};"
    "img.onerror=function(){reject('decode');};img.src=URL.createObjectURL(file);});}"
    "async function sendimg(){var f=document.getElementById('im').files[0];"
    "if(!f){document.getElementById('ir').textContent='Chua chon anh';return;}"
    "document.getElementById('ir').textContent='Dang chuyen JPEG...';"
    "var blob;try{blob=await toJpeg(f);}catch(e){document.getElementById('ir').textContent='Loi doc anh: '+e;return;}"
    "var name=(f.name||'panic').replace(/\\.[^.]+$/,'')+'.jpg';"
    "var q='?img=1&mime=image/jpeg&name='+encodeURIComponent(name);"
    "document.getElementById('ir').textContent='Dang gui ('+Math.round(blob.size/1024)+' KB)...';"
    "var res=await fetch('/panic'+q,{method:'POST',body:blob});"
    "document.getElementById('ir').textContent=await res.text();}"
    "</script></body></html>";

static esp_err_t rootGetHandler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t panicPostHandler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > MAX_UPLOAD) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"kich thuoc khong hop le\"}");
        return ESP_OK;
    }

    char *buf = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(total + 1);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"het bo nho\"}");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nhan du lieu that bai\"}");
            return ESP_OK;
        }
        received += r;
    }
    buf[received] = '\0';

    // Tên file theo thời gian (giờ VN đã đồng bộ) + tên client nếu có.
    char name_q[64] = {0};
    char client_name[48] = {0};
    char img_q[8] = {0};
    char mime_q[48] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        char q[256];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            if (httpd_query_key_value(q, "name", name_q, sizeof(name_q)) == ESP_OK) {
                for (int i = 0; name_q[i] && i < 40; ++i) {
                    char c = name_q[i];
                    client_name[i] = (c >= 32 && c < 127 && c != '/' && c != '\\') ? c : '_';
                }
            }
            httpd_query_key_value(q, "img", img_q, sizeof(img_q));
            httpd_query_key_value(q, "mime", mime_q, sizeof(mime_q));
        }
    }
    const bool is_image = (img_q[0] == '1');
    std::string mime = mime_q[0] ? mime_q : "image/jpeg";

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);

    char name[80];
    if (client_name[0]) {
        snprintf(name, sizeof(name), "%s_%s", ts, client_name);
    } else {
        snprintf(name, sizeof(name), "%s.ips", ts);
    }

    // Giữ trong RAM (không ghi SD).
    setReceived(std::string(name), std::string(buf, received), is_image, mime);
    free(buf);

    ESP_LOGI(TAG, "received panic (RAM) name=%s bytes=%d img=%d mime=%s",
             name, received, is_image ? 1 : 0, mime.c_str());
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\",\"bytes\":%d,\"img\":%d,\"stored\":\"ram\"}",
             name, received, is_image ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t apikeyPostHandler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"key khong hop le\"}");
        return ESP_OK;
    }
    char key[520] = {0};
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, key + received, total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nhan that bai\"}");
            return ESP_OK;
        }
        received += r;
    }
    key[received] = '\0';

    bool ok = HCCPanicAI::saveApiKey(std::string(key));
    if (ok) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Da luu API key\"}");
    }
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"luu that bai (the SD?)\"}");
    return ESP_OK;
}

bool HCCPanicServer::start(uint16_t port)
{
    if (s_server) return true;
    HCCPanicStorage::ensureDir(RAW_DIR);

    s_port = port;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = rootGetHandler;
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t panic = {};
    panic.uri = "/panic";
    panic.method = HTTP_POST;
    panic.handler = panicPostHandler;
    httpd_register_uri_handler(s_server, &panic);

    httpd_uri_t apikey = {};
    apikey.uri = "/apikey";
    apikey.method = HTTP_POST;
    apikey.handler = apikeyPostHandler;
    httpd_register_uri_handler(s_server, &apikey);

    startMdns(port);
    ESP_LOGI(TAG, "server started on :%u", port);
    return true;
}

void HCCPanicServer::stop()
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
}

bool HCCPanicServer::isRunning()
{
    return s_server != nullptr;
}

bool HCCPanicServer::deviceUrl(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    snprintf(out, out_size, "http://" IPSTR, IP2STR(&ip.ip));
    appendPort(out, out_size);
    return true;
}

void HCCPanicServer::hostUrl(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "http://%s.local", MDNS_HOST);
    appendPort(out, out_size);
}
