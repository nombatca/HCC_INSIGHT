#include "HCCPanicAI.hpp"
#include "HCCPanicStorage.hpp"

#include <cstring>
#include <cstdlib>
#include <cctype>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "PanicAI";

static const char *ANTHROPIC_URL = "https://api.anthropic.com/v1/messages";
static const char *OPENAI_URL = "https://api.openai.com/v1/chat/completions";
static const char *DEFAULT_ANTHROPIC_MODEL = "claude-opus-5";
static const char *DEFAULT_OPENAI_MODEL = "gpt-4o";
static const char *ANTHROPIC_VERSION = "2023-06-01";
// GPT: tiếng Việt có dấu tốn nhiều token -> 1024 dễ cắt mất [KIEMTRA]/[DOTINCAY].
static constexpr int OPENAI_MAX_TOKENS = 3000;
static constexpr int ANTHROPIC_MAX_TOKENS = 1024;

static int maxTokensFor(const std::string &provider)
{
    return provider == "anthropic" ? ANTHROPIC_MAX_TOKENS : OPENAI_MAX_TOKENS;
}

// System prompt: chẩn đoán sửa chữa phần cứng iPhone, xuất theo khối có tag ASCII.
static const char *SYSTEM_PROMPT =
    "Ban la ky su sua chua BO MACH (main) iPhone o cap do phan cung. Nhiem vu: phan tich log "
    "panic iPhone theo huong CHAN DOAN SUA CHUA PHAN CUNG, KHONG tra loi chung chung kieu "
    "'kernel panic / loi CPU / loi phan mem / khoi dong lai may'.\n"
    "\n"
    "QUY TAC:\n"
    "1. Doc va phan tich DAY DU panicString.\n"
    "2. Trich xuat CHINH XAC cac signature/driver/service xuat hien trong panic, vi du: "
    "atc0-dpphy, atc-dpphy0, usb-drd, S.sensor[x], missing sensor, watchdog, userspace watchdog, "
    "AOP, SMC, ANS, NAND, baseband, dpphy, IOService...\n"
    "3. Voi moi signature: noi ro thuoc SUBSYSTEM nao va chuc nang lien quan.\n"
    "4. Tu signature -> khoanh vung PHAN CUNG can kiem tra: cum flex, IC, nguon, bus, duong tin "
    "hieu, connector hoac vung mainboard lien quan.\n"
    "5. Dua ra THU TU kiem tra thuc te cho tho sua main.\n"
    "6. TUYET DOI khong chi thay 'timeout/CPU/kernel panic' roi ket luan chung chung.\n"
    "7. Khong khang dinh mot linh kien DA HONG neu panic chua du bang chung. Phan biet ro:\n"
    "   FACT = thong tin CO TRUC TIEP trong panic; INFERENCE = suy luan ky thuat.\n"
    "8. KHONG tu doan ten thuong mai iPhone tu Product ID (chi dung ma model neu duoc cung cap).\n"
    "9. Ngan gon, de doc tren man hinh nho; dung gach dau dong '-' khi liet ke.\n"
    "10. QUAN TRONG: Toan bo NOI DUNG tra loi PHAI bang TIENG VIET CO DAU day du "
    "(dung dau thanh: sac/huyen/hoi/nga/nang, cac chu a a e o o u d...). "
    "Vi du phai viet 'cap sac', 'nguon', 'kiem tra', 'phan cung' thanh 'cáp sạc', 'nguồn', "
    "'kiểm tra', 'phần cứng'. KHONG tra loi tieng Viet khong dau.\n"
    "(Cac tag [LOI]/[SIGNATURE]... giu nguyen chu HOA khong dau; chi phan noi dung ben trong moi can co dau.)\n"
    "\n"
    "TRA LOI DUNG cac khoi sau, moi tag tren 1 dong rieng, KHONG them gi ngoai cac khoi:\n"
    "[LOI]\n<loai panic + subsystem chinh>\n"
    "[SIGNATURE]\n<liet ke cac signature quan trong>\n"
    "[PHANTICH]\n<giai thich panicString va tung subsystem lien quan; danh dau FACT/INFERENCE>\n"
    "[NGUYENNHAN]\n<phan cung/duong tin hieu nghi ngo, sap theo thu tu kha nang cao->thap>\n"
    "[KIEMTRA]\n<cac buoc kiem tra/sua thuc te theo thu tu>\n"
    "[DOTINCAY]\n<HIGH|MEDIUM|LOW + ly do ngan>\n"
    "\n"
    "KIEN THUC SUBSYSTEM (map signature -> phan cung):\n"
    "- dpphy / atc-dpphy / atc0-dpphy = USB-C DisplayPort PHY (cong USB-C + xuat hinh).\n"
    "- atc / atc0 = Apple Type-C controller (USB-C).\n"
    "- usb-drd = USB dual-role (duong data USB-C).\n"
    "  -> nhom atc/dpphy/usb-drd nghi: cum cap sac USB-C (long/dut/am), IC sac (Tristar/Tigris), IC/PHY DisplayPort.\n"
    "- ans / ANS2 = NAND storage controller (o cung/eMMC).\n"
    "- aop = Always-On Processor; smc = System Management Controller (nguon/sac).\n"
    "- pmu / pmic = quan ly nguon; baseband = song/modem.\n"
    "- S.sensor[x] / missing sensor = duong sensor/flex sensor (nguon rail sensor).\n"
    "- watchdog / userspace watchdog / busy timeout = treo bus/khong phan hoi -> nghi phan cung tren bus do.\n"
    "\n"
    "VI DU MAU (bam sat cach nay; noi dung CO DAU):\n"
    "Input: panic-full ... busy timeout[0] 60s ... 'atc0-dpphy' 'atc-dpphy0' 'usb-drd' @IOService.cpp\n"
    "[LOI]\npanic-full: busy timeout 60s treo registry - nhóm USB-C DisplayPort PHY\n"
    "[SIGNATURE]\natc0-dpphy, atc-dpphy0, usb-drd\n"
    "[PHANTICH]\nFACT: các entry giữ registry busy là atc0-dpphy/atc-dpphy0/usb-drd. "
    "INFERENCE: cụm USB-C PHY/DisplayPort không phản hồi -> treo bus 60s -> hệ ép panic bảo vệ. "
    "Đây là hướng phần cứng cổng USB-C, KHÔNG phải lỗi CPU/phần mềm.\n"
    "[NGUYENNHAN]\n- Cụm cáp sạc USB-C lỏng/đứt/ẩm (khả năng cao)\n- IC sạc (Tristar/Tigris)\n- IC/PHY DisplayPort trên main\n"
    "[KIEMTRA]\n- Restore để loại trừ phần mềm\n- Đo/vệ sinh chân cáp USB-C\n- Thay cụm cáp sạc USB-C\n- Kiểm IC sạc/PHY nếu còn lỗi\n"
    "[DOTINCAY]\nMEDIUM - signature rõ nhưng chưa đo thực tế";

namespace {

esp_err_t aiEventHandler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->user_data) {
        auto *buf = static_cast<std::string *>(evt->user_data);
        buf->append(static_cast<const char *>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

}  // namespace

bool HCCPanicAI::loadConfig(std::string &api_key, std::string &provider,
                            std::string &model, std::string &url)
{
    api_key.clear();
    provider.clear();
    model.clear();
    url.clear();

    std::string content;
    if (!HCCPanicStorage::readFile("panic/ai_config.json", content)) {
        ESP_LOGW(TAG, "ai_config.json not found");
        return false;
    }
    cJSON *root = cJSON_Parse(content.c_str());
    if (!root) {
        ESP_LOGW(TAG, "ai_config.json parse error");
        return false;
    }
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(root, "provider");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "model");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "base_url");
    if (cJSON_IsString(k) && k->valuestring) api_key = k->valuestring;
    if (cJSON_IsString(pv) && pv->valuestring) {
        provider = pv->valuestring;
        for (auto &c : provider) c = (char)tolower((unsigned char)c);
    }
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0]) model = m->valuestring;
    if (cJSON_IsString(u) && u->valuestring && u->valuestring[0]) url = u->valuestring;
    cJSON_Delete(root);

    // Tự đoán provider theo tiền tố key nếu chưa khai báo.
    if (provider.empty()) {
        provider = (api_key.rfind("sk-ant", 0) == 0) ? "anthropic" : "openai";
    }
    // Mặc định model + url theo provider; sửa model lệch provider (config cũ).
    if (provider == "anthropic") {
        if (model.empty() || model.rfind("gpt", 0) == 0) model = DEFAULT_ANTHROPIC_MODEL;
        if (url.empty()) url = ANTHROPIC_URL;
    } else {
        provider = "openai";
        if (model.empty() || model.rfind("claude", 0) == 0) model = DEFAULT_OPENAI_MODEL;
        if (url.empty()) url = OPENAI_URL;
    }
    return !api_key.empty();
}

std::string HCCPanicAI::buildUserContent(const ParsedPanic &parsed)
{
    std::string s;
    s += "Model: " + parsed.model + "\n";
    s += "OS: " + parsed.os_version + "\n";
    s += "Panic type: " + parsed.panic_type + "\n";
    if (!parsed.source.empty()) s += "Source: " + parsed.source + "\n";
    if (!parsed.index.empty()) s += "Index: " + parsed.index + "\n";
    if (!parsed.error_codes.empty()) {
        s += "Ma loi: ";
        for (size_t i = 0; i < parsed.error_codes.size(); ++i) {
            if (i) s += ", ";
            s += parsed.error_codes[i];
        }
        s += "\n";
    }
    if (!parsed.sensors.empty()) {
        s += "Dong quan trong:\n";
        for (const auto &line : parsed.sensors) s += "- " + line + "\n";
    }
    std::string ps = parsed.panic_string.substr(0, 6000);
    s += "panicString:\n" + ps;
    return s;
}

PanicResult HCCPanicAI::parseAiText(const std::string &text)
{
    // Các tag khối do prompt quy định.
    static const char *TAGS[] = {"[LOI]", "[SIGNATURE]", "[PHANTICH]",
                                  "[NGUYENNHAN]", "[KIEMTRA]", "[DOTINCAY]"};
    auto trimq = [](std::string s) -> std::string {
        size_t a = s.find_first_not_of(" \t\r\n:");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    auto block = [&](const char *tag) -> std::string {
        size_t p = text.find(tag);
        if (p == std::string::npos) return std::string();
        p += strlen(tag);
        size_t nxt = text.size();
        for (auto t : TAGS) {
            size_t q = text.find(t, p);
            if (q != std::string::npos && q < nxt) nxt = q;
        }
        return trimq(text.substr(p, nxt - p));
    };

    std::string loi = block("[LOI]");
    std::string sig = block("[SIGNATURE]");
    std::string ana = block("[PHANTICH]");
    std::string ngn = block("[NGUYENNHAN]");
    std::string kt = block("[KIEMTRA]");
    std::string conf = block("[DOTINCAY]");

    PanicResult r;
    r.source_tier = "AI_INFERENCE";

    // Không thấy tag nào -> coi như thất bại (giữ text để chẩn đoán).
    if (loi.empty() && sig.empty() && ana.empty() && ngn.empty() && kt.empty()) {
        r.ok = false;
        r.message = text.substr(0, 200);
        return r;
    }

    r.error = loi.empty() ? "-" : loi;
    // Gom SIGNATURE + PHÂN TÍCH + NGUYÊN NHÂN vào ô "NGUYÊN NHÂN" của UI (giữ nguyên UI 4 ô).
    std::string cause;
    if (!sig.empty()) cause += "SIGNATURE:\n" + sig + "\n\n";
    if (!ana.empty()) cause += "PHÂN TÍCH:\n" + ana + "\n\n";
    if (!ngn.empty()) cause += "NGUYÊN NHÂN KHẢ NĂNG:\n" + ngn;
    r.cause = cause.empty() ? "-" : cause;
    r.repair = kt.empty() ? "-" : kt;
    r.confidence = conf.empty() ? "-" : conf;
    r.ok = true;
    return r;
}

bool HCCPanicAI::saveApiKey(const std::string &key)
{
    cJSON *root = nullptr;
    std::string content;
    if (HCCPanicStorage::readFile("panic/ai_config.json", content)) {
        root = cJSON_Parse(content.c_str());
    }
    if (!root) root = cJSON_CreateObject();

    cJSON_DeleteItemFromObject(root, "api_key");
    cJSON_AddStringToObject(root, "api_key", key.c_str());

    const bool is_anthropic = (key.rfind("sk-ant", 0) == 0);
    // Provider luôn đặt theo key vừa nhập (đè cấu hình cũ cho khớp).
    cJSON_DeleteItemFromObject(root, "provider");
    cJSON_AddStringToObject(root, "provider", is_anthropic ? "anthropic" : "openai");
    // Bỏ model lệch nhà cung cấp (để dùng mặc định đúng), giữ model hợp lệ.
    const cJSON *m = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(m) && m->valuestring) {
        std::string mv = m->valuestring;
        bool wrong = (is_anthropic && mv.rfind("gpt", 0) == 0) ||
                     (!is_anthropic && mv.rfind("claude", 0) == 0);
        if (wrong) cJSON_DeleteItemFromObject(root, "model");
    }

    char *printed = cJSON_Print(root);
    bool ok = false;
    if (printed) {
        ok = HCCPanicStorage::writeFile("panic/ai_config.json", printed, strlen(printed));
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return ok;
}

bool HCCPanicAI::hasApiKey()
{
    std::string api_key, provider, model, url;
    return loadConfig(api_key, provider, model, url);
}

PanicResult HCCPanicAI::analyze(const ParsedPanic &parsed)
{
    PanicResult r;
    r.source_tier = "AI_INFERENCE";

    std::string api_key, provider, model, url;
    if (!loadConfig(api_key, provider, model, url)) {
        r.ok = false;
        r.message = "Chua cau hinh AI (thieu panic/ai_config.json hoac api_key)";
        return r;
    }

    std::string user_content = buildUserContent(parsed);

    // Dựng body JSON theo provider.
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model.c_str());
    cJSON_AddNumberToObject(body, "max_tokens", maxTokensFor(provider));
    if (provider == "anthropic") {
        cJSON_AddStringToObject(body, "system", SYSTEM_PROMPT);
        cJSON *msgs = cJSON_AddArrayToObject(body, "messages");
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON_AddStringToObject(um, "content", user_content.c_str());
        cJSON_AddItemToArray(msgs, um);
    } else {  // openai
        cJSON_AddNumberToObject(body, "temperature", 0.2);
        cJSON *msgs = cJSON_AddArrayToObject(body, "messages");
        cJSON *sm = cJSON_CreateObject();
        cJSON_AddStringToObject(sm, "role", "system");
        cJSON_AddStringToObject(sm, "content", SYSTEM_PROMPT);
        cJSON_AddItemToArray(msgs, sm);
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON_AddStringToObject(um, "content", user_content.c_str());
        cJSON_AddItemToArray(msgs, um);
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) {
        r.ok = false;
        r.message = "Loi tao request";
        return r;
    }
    std::string body_json = body_str;
    cJSON_free(body_str);

    return sendAndParse(provider, url, api_key, body_json);
}

PanicResult HCCPanicAI::sendAndParse(const std::string &provider, const std::string &url,
                                     const std::string &api_key, const std::string &body_json)
{
    PanicResult r;
    r.source_tier = "AI_INFERENCE";

    std::string response;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = aiEventHandler;
    config.user_data = &response;
    config.timeout_ms = 90000;  // trả lời dài (3000 token) có thể > 40 s
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        r.ok = false;
        r.message = "Khong tao duoc HTTP client";
        return r;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "content-type", "application/json");
    if (provider == "anthropic") {
        esp_http_client_set_header(client, "x-api-key", api_key.c_str());
        esp_http_client_set_header(client, "anthropic-version", ANTHROPIC_VERSION);
    } else {
        std::string bearer = "Bearer " + api_key;
        esp_http_client_set_header(client, "Authorization", bearer.c_str());
    }
    esp_http_client_set_post_field(client, body_json.c_str(), body_json.size());

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    // In gọn body (bỏ xuống dòng) để không bị cắt trên serial.
    std::string body_log = response.substr(0, 300);
    for (auto &ch : body_log) { if (ch == '\n' || ch == '\r') ch = ' '; }
    ESP_LOGI(TAG, "AI(%s) err=%s status=%d req_bytes=%u resp_bytes=%u body=%s",
             provider.c_str(), esp_err_to_name(err), status,
             (unsigned)body_json.size(), (unsigned)response.size(), body_log.c_str());

    if (status >= 400) {
        r.ok = false;
        if (status == 401) r.message = "API key sai hoac thieu (HTTP 401)";
        else if (status == 429) r.message = "AI qua tai/het quota (HTTP 429)";
        else r.message = "AI tra ve HTTP " + std::to_string(status);
        if (!response.empty()) r.message += " - " + response.substr(0, 120);
        return r;
    }
    if (status == 0 && err != ESP_OK) {
        r.ok = false;
        r.message = std::string("Loi ket noi AI: ") + esp_err_to_name(err);
        return r;
    }
    if (status < 200 || status >= 300) {
        r.ok = false;
        r.message = "AI tra ve HTTP " + std::to_string(status);
        return r;
    }

    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) {
        r.ok = false;
        r.message = "Loi parse phan hoi AI";
        return r;
    }
    std::string text;
    bool truncated = false;  // GPT dừng vì hết max_tokens (finish_reason == "length")
    if (provider == "anthropic") {
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (cJSON_IsArray(content)) {
            const cJSON *item;
            cJSON_ArrayForEach(item, content) {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
                if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0) {
                    const cJSON *t = cJSON_GetObjectItemCaseSensitive(item, "text");
                    if (cJSON_IsString(t) && t->valuestring) { text = t->valuestring; break; }
                }
            }
        }
    } else {
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        if (cJSON_IsArray(choices)) {
            const cJSON *first = cJSON_GetArrayItem(choices, 0);
            const cJSON *msg = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : nullptr;
            const cJSON *c = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : nullptr;
            if (cJSON_IsString(c) && c->valuestring) text = c->valuestring;
            const cJSON *fr = first ? cJSON_GetObjectItemCaseSensitive(first, "finish_reason") : nullptr;
            truncated = cJSON_IsString(fr) && fr->valuestring && strcmp(fr->valuestring, "length") == 0;
        }
    }
    cJSON_Delete(root);

    if (text.empty()) {
        r.ok = false;
        r.message = truncated ? "Phan hoi AI bi cat (het max_tokens)" : "Phan hoi AI rong";
        return r;
    }
    r = parseAiText(text);
    if (truncated) {
        ESP_LOGW(TAG, "AI response truncated (finish_reason=length)");
        if (!r.ok) {
            r.message = "Phan hoi AI bi cat (het max_tokens) - " + r.message;
        } else {
            // Vẫn hiện phần đã nhận nhưng báo rõ có thể thiếu các khối cuối.
            const char *warn = "(Phản hồi AI bị cắt - có thể thiếu nội dung)";
            r.confidence = (r.confidence == "-") ? warn : r.confidence + "\n" + warn;
        }
    }
    return r;
}

PanicResult HCCPanicAI::analyzeImage(const std::string &img_bytes, const std::string &mime)
{
    PanicResult r;
    r.source_tier = "AI_INFERENCE";

    std::string api_key, provider, model, url;
    if (!loadConfig(api_key, provider, model, url)) {
        r.ok = false;
        r.message = "Chua cau hinh AI (thieu api_key)";
        return r;
    }
    if (img_bytes.empty()) {
        r.ok = false;
        r.message = "Anh rong";
        return r;
    }

    // Base64 ảnh.
    size_t need = 4 * ((img_bytes.size() + 2) / 3) + 1;
    std::string b64;
    b64.resize(need);
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)&b64[0], need, &olen,
                              (const unsigned char *)img_bytes.data(), img_bytes.size()) != 0) {
        r.ok = false;
        r.message = "Loi ma hoa anh";
        return r;
    }
    b64.resize(olen);

    const char *img_prompt = "Doc log panic trong anh (chup man hinh iPhone) va phan tich theo dung dinh dang.";

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model.c_str());
    cJSON_AddNumberToObject(body, "max_tokens", maxTokensFor(provider));

    if (provider == "anthropic") {
        cJSON_AddStringToObject(body, "system", SYSTEM_PROMPT);
        cJSON *msgs = cJSON_AddArrayToObject(body, "messages");
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON *content = cJSON_AddArrayToObject(um, "content");
        cJSON *img = cJSON_CreateObject();
        cJSON_AddStringToObject(img, "type", "image");
        cJSON *src = cJSON_AddObjectToObject(img, "source");
        cJSON_AddStringToObject(src, "type", "base64");
        cJSON_AddStringToObject(src, "media_type", mime.c_str());
        cJSON_AddStringToObject(src, "data", b64.c_str());
        cJSON_AddItemToArray(content, img);
        cJSON *tx = cJSON_CreateObject();
        cJSON_AddStringToObject(tx, "type", "text");
        cJSON_AddStringToObject(tx, "text", img_prompt);
        cJSON_AddItemToArray(content, tx);
        cJSON_AddItemToArray(msgs, um);
    } else {  // openai vision
        cJSON *msgs = cJSON_AddArrayToObject(body, "messages");
        cJSON *sm = cJSON_CreateObject();
        cJSON_AddStringToObject(sm, "role", "system");
        cJSON_AddStringToObject(sm, "content", SYSTEM_PROMPT);
        cJSON_AddItemToArray(msgs, sm);
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON *content = cJSON_AddArrayToObject(um, "content");
        cJSON *tx = cJSON_CreateObject();
        cJSON_AddStringToObject(tx, "type", "text");
        cJSON_AddStringToObject(tx, "text", img_prompt);
        cJSON_AddItemToArray(content, tx);
        cJSON *iu = cJSON_CreateObject();
        cJSON_AddStringToObject(iu, "type", "image_url");
        cJSON *urlobj = cJSON_AddObjectToObject(iu, "image_url");
        std::string data_uri = "data:" + mime + ";base64," + b64;
        cJSON_AddStringToObject(urlobj, "url", data_uri.c_str());
        cJSON_AddItemToArray(content, iu);
        cJSON_AddItemToArray(msgs, um);
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) {
        r.ok = false;
        r.message = "Loi tao request anh";
        return r;
    }
    std::string body_json = body_str;
    cJSON_free(body_str);

    return sendAndParse(provider, url, api_key, body_json);
}
