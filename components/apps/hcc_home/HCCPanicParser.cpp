#include "HCCPanicParser.hpp"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "PanicParse";

namespace {

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string toLower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Tìm đệ quy giá trị chuỗi đầu tiên cho một key trong cây cJSON.
// Với item số, trả về dạng số. Trả "" nếu không thấy.
std::string findKey(const cJSON *node, const char *key)
{
    if (!node) return std::string();
    for (const cJSON *c = node->child; c; c = c->next) {
        if (c->string && strcasecmp(c->string, key) == 0) {
            if (cJSON_IsString(c) && c->valuestring) return c->valuestring;
            if (cJSON_IsNumber(c)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", c->valuedouble);
                return buf;
            }
        }
        if (cJSON_IsObject(c) || cJSON_IsArray(c)) {
            std::string r = findKey(c, key);
            if (!r.empty()) return r;
        }
    }
    return std::string();
}

std::string firstNonEmpty(std::initializer_list<std::string> vals)
{
    for (const auto &v : vals) {
        if (!v.empty()) return v;
    }
    return std::string();
}

void pushUnique(std::vector<std::string> &v, const std::string &s, size_t cap)
{
    if (s.empty() || v.size() >= cap) return;
    if (std::find(v.begin(), v.end(), s) != v.end()) return;
    v.push_back(s);
}

}  // namespace

ParsedPanic HCCPanicParser::parse(const std::string &raw)
{
    std::string t = trim(raw);
    if (!t.empty() && (t[0] == '{' || t[0] == '[')) {
        ParsedPanic p = parseJson(raw);
        if (p.valid) return p;
    }
    return parseText(raw);
}

ParsedPanic HCCPanicParser::parseJson(const std::string &raw)
{
    ParsedPanic out;

    // .ips thường có 2 dòng JSON: header + payload. Thử cả hai.
    cJSON *header = nullptr;
    cJSON *payload = nullptr;

    size_t nl = raw.find('\n');
    if (nl != std::string::npos) {
        std::string line1 = raw.substr(0, nl);
        std::string rest = raw.substr(nl + 1);
        header = cJSON_Parse(line1.c_str());
        payload = cJSON_Parse(rest.c_str());
    }
    if (!payload) {
        // Thử parse toàn bộ như một JSON.
        payload = cJSON_Parse(raw.c_str());
    }

    if (!header && !payload) {
        ESP_LOGW(TAG, "JSON parse failed, fallback to text");
        return out;  // valid=false
    }

    auto lookup = [&](const char *key) -> std::string {
        std::string v = findKey(payload, key);
        if (v.empty() && header) v = findKey(header, key);
        return v;
    };

    out.model = firstNonEmpty({lookup("product"), lookup("modelCode"),
                               lookup("hardware_model"), lookup("model")});
    out.os_version = firstNonEmpty({lookup("os_version"), lookup("build"),
                                    lookup("kernel"), lookup("osVersion")});
    out.panic_type = firstNonEmpty({lookup("bug_type"), lookup("panicType"),
                                    lookup("bugType")});
    out.panic_string = firstNonEmpty({lookup("panicString"), lookup("panic_string"),
                                      lookup("description")});
    out.source = lookup("source");
    out.index = lookup("index");

    if (out.panic_string.empty()) {
        // Không có panicString: dùng toàn bộ payload rút gọn làm ngữ cảnh.
        char *printed = payload ? cJSON_PrintUnformatted(payload) : nullptr;
        if (printed) {
            out.panic_string = printed;
            cJSON_free(printed);
        }
    }

    extractFromPanicString(out, out.panic_string);
    out.raw_excerpt = out.panic_string.substr(0, 400);
    buildSignature(out);
    out.valid = true;

    if (header) cJSON_Delete(header);
    if (payload) cJSON_Delete(payload);
    return out;
}

ParsedPanic HCCPanicParser::parseText(const std::string &raw)
{
    ParsedPanic out;
    // Text thô: quét theo dòng tìm các trường quen thuộc.
    std::string lower = toLower(raw);

    auto grabAfter = [&](const char *label) -> std::string {
        size_t p = lower.find(label);
        if (p == std::string::npos) return std::string();
        p = raw.find_first_of(":=", p);
        if (p == std::string::npos) return std::string();
        size_t e = raw.find('\n', p);
        std::string v = raw.substr(p + 1, (e == std::string::npos ? raw.size() : e) - p - 1);
        return trim(v);
    };

    out.model = firstNonEmpty({grabAfter("product"), grabAfter("model")});
    out.os_version = firstNonEmpty({grabAfter("os version"), grabAfter("build"),
                                    grabAfter("kernel version")});
    out.panic_type = firstNonEmpty({grabAfter("panic type"), grabAfter("bug_type")});
    out.source = grabAfter("source");
    out.index = grabAfter("index");
    out.panic_string = raw;

    extractFromPanicString(out, raw);
    out.raw_excerpt = trim(raw).substr(0, 400);
    buildSignature(out);
    out.valid = true;
    return out;
}

void HCCPanicParser::extractFromPanicString(ParsedPanic &out, const std::string &panic)
{
    if (panic.empty()) return;

    static const char *keywords[] = {
        "panic", "watchdog", "wdt", "assert", "fault", "failure", "error",
        "sensor", "gpu", "pmu", "nand", "i2c", "baseband", "wifi", "sleep",
        "thermal", "voltage", "current", "hang", "timeout", "reset"
    };

    // Duyệt từng dòng, thu các dòng chứa từ khoá (tối đa 12 dòng).
    size_t start = 0;
    while (start < panic.size()) {
        size_t nl = panic.find('\n', start);
        std::string line = panic.substr(start, (nl == std::string::npos ? panic.size() : nl) - start);
        std::string low = toLower(line);
        for (auto kw : keywords) {
            if (low.find(kw) != std::string::npos) {
                std::string t = trim(line);
                if (t.size() > 3) pushUnique(out.sensors, t.substr(0, 120), 12);
                break;
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    // Trích mã lỗi: token hex 0x... và số thập phân đứng sau từ khoá code/error/status.
    for (size_t i = 0; i + 1 < panic.size(); ++i) {
        if (panic[i] == '0' && (panic[i + 1] == 'x' || panic[i + 1] == 'X')) {
            size_t j = i + 2;
            while (j < panic.size() && isxdigit((unsigned char)panic[j])) j++;
            // Bỏ địa chỉ bộ nhớ (con trỏ kernel 64-bit, vd 0xfffffff0...): không phải mã lỗi,
            // lại hay đứng đầu danh sách -> bị điền sẵn vào CODE HEX và thành keyword KB sai.
            if (j > i + 2 && j - (i + 2) <= 10) {
                std::string hex = panic.substr(i, j - i);
                unsigned long dec = strtoul(hex.c_str() + 2, nullptr, 16);
                if (dec != 0) {  // 0x0 không mang thông tin và KB coi là keyword tầm thường
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s (dec %lu)", hex.c_str(), dec);
                    pushUnique(out.error_codes, buf, 8);
                }
            }
            i = j;
        }
    }

    std::string low = toLower(panic);
    static const char *code_labels[] = {"code", "error", "status", "result", "ret"};
    for (auto label : code_labels) {
        size_t p = 0;
        while ((p = low.find(label, p)) != std::string::npos) {
            size_t q = p + strlen(label);
            while (q < panic.size() && !isdigit((unsigned char)panic[q]) &&
                   (panic[q] == ' ' || panic[q] == ':' || panic[q] == '=' || panic[q] == '\t')) {
                q++;
            }
            if (q < panic.size() && isdigit((unsigned char)panic[q])) {
                size_t r = q;
                while (r < panic.size() && isdigit((unsigned char)panic[r])) r++;
                if (r - q >= 3) {  // bỏ số quá nhỏ
                    std::string dec = panic.substr(q, r - q);
                    unsigned long v = strtoul(dec.c_str(), nullptr, 10);
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s (0x%lX)", dec.c_str(), v);
                    pushUnique(out.error_codes, buf, 8);
                }
            }
            p = q + 1;
        }
    }
}

// Tìm tok trong low (đã hạ thường) mà ký tự ngay trước KHÔNG phải chữ/số,
// để "atc" không khớp trong "watchdog". Trả npos nếu không thấy.
static size_t findPrefixToken(const std::string &low, const char *tok, size_t from = 0)
{
    size_t p = from;
    while ((p = low.find(tok, p)) != std::string::npos) {
        if (p == 0 || !isalnum((unsigned char)low[p - 1])) return p;
        p++;
    }
    return std::string::npos;
}

void HCCPanicParser::buildSignature(ParsedPanic &out)
{
    // Chữ ký: model | panic_type | tối đa 3 subsystem đặc trưng tìm thấy trong panic
    // (vd "s.sensor[1],dpphy"). Ổn định hơn dòng đầu tiên (thường chỉ là "panic(cpu ...").
    // Sắp từ cụ thể -> chung chung.
    static const char *kTokens[] = {
        "missing sensor", "dpphy", "usb-drd", "atc", "ans", "nand", "aop", "smc", "pmu",
        "baseband", "i2c", "gpu", "dcp", "sep", "thermal", "userspace watchdog", "watchdog",
        "busy timeout",
    };
    const std::string low = toLower(out.panic_string);
    std::vector<std::string> found;

    // S.sensor[n]: giữ nguyên chỉ số vì mỗi index là một đường sensor khác nhau.
    size_t p = findPrefixToken(low, "s.sensor[");
    if (p != std::string::npos) {
        size_t e = low.find(']', p);
        if (e != std::string::npos && e - p <= 14) found.push_back(low.substr(p, e - p + 1));
    }
    for (const char *tok : kTokens) {
        if (found.size() >= 3) break;
        if (strcmp(tok, "watchdog") == 0 &&
            std::find(found.begin(), found.end(), "userspace watchdog") != found.end()) {
            continue;
        }
        if (findPrefixToken(low, tok) != std::string::npos) found.push_back(tok);
    }

    std::string key;
    for (size_t i = 0; i < found.size(); ++i) {
        if (i) key += ",";
        key += found[i];
    }
    if (key.empty()) {
        // Không có subsystem quen thuộc: dùng ~40 ký tự đầu của dòng khoá như trước.
        key = out.sensors.empty() ? std::string() : toLower(out.sensors[0]);
        if (key.size() > 40) key = key.substr(0, 40);
    }
    out.signature = toLower(out.model) + "|" + toLower(out.panic_type) + "|" + key;
}
