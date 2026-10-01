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
// Key ở cấp hiện tại được ưu tiên trước key lồng bên trong (vd "product" của payload
// chứ không phải "product" nằm trong một object con).
std::string findKey(const cJSON *node, const char *key)
{
    if (!node) return std::string();
    for (const cJSON *c = node->child; c; c = c->next) {
        if (c->string && strcasecmp(c->string, key) == 0) {
            if (cJSON_IsString(c) && c->valuestring && c->valuestring[0]) return c->valuestring;
            if (cJSON_IsNumber(c)) {
                char buf[32];
                // Số nguyên (vd panicFlags) in đủ chữ số; "%g" làm tròn số lớn.
                if (c->valuedouble == (double)(long long)c->valuedouble) {
                    snprintf(buf, sizeof(buf), "%lld", (long long)c->valuedouble);
                } else {
                    snprintf(buf, sizeof(buf), "%g", c->valuedouble);
                }
                return buf;
            }
        }
    }
    for (const cJSON *c = node->child; c; c = c->next) {
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

// Mã máy Apple dạng "iPhone14,2" / "iPad13,1" đầu tiên trong chuỗi; "" nếu không có.
// ("iPhone OS 17.5" không khớp vì sau tên phải là số,số.)
std::string findProductType(const std::string &s)
{
    static const char *prefixes[] = {"iPhone", "iPad", "iPod"};
    size_t best = std::string::npos;
    std::string found;
    for (const char *pre : prefixes) {
        const size_t plen = strlen(pre);
        for (size_t p = s.find(pre); p != std::string::npos; p = s.find(pre, p + 1)) {
            size_t i = p + plen;
            size_t a = i;
            while (i < s.size() && isdigit((unsigned char)s[i])) i++;
            if (i == a || i >= s.size() || s[i] != ',') continue;
            size_t b = ++i;
            while (i < s.size() && isdigit((unsigned char)s[i])) i++;
            if (i == b) continue;
            if (p < best) {
                best = p;
                found = s.substr(p, i - p);
            }
            break;
        }
    }
    return found;
}

// Giá trị của "key": "..." trong text (JSON bị cắt / chữ AI chép từ ảnh); "" nếu không có.
std::string quotedValue(const std::string &s, const char *key)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return std::string();
    p = s.find_first_not_of(" \t", p + pat.size());
    if (p == std::string::npos || s[p] != ':') return std::string();
    p = s.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos || s[p] != '"') return std::string();
    size_t e = s.find('"', p + 1);
    if (e == std::string::npos) return std::string();
    return s.substr(p + 1, e - p - 1);
}

// Chuỗi JSON "key": "..." có escape (\n, \", \uXXXX...) -> text thật. Dùng khi payload
// .ips không parse được bằng cJSON (bị cắt, quá lớn...). Không có key -> "".
std::string escapedValue(const std::string &s, const char *key)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return std::string();
    p = s.find_first_not_of(" \t\r\n", p + pat.size());
    if (p == std::string::npos || s[p] != ':') return std::string();
    p = s.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || s[p] != '"') return std::string();
    std::string out;
    for (size_t i = p + 1; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') break;
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }
        char e = s[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': break;
            case 'u':
                if (i + 4 < s.size()) {
                    unsigned v = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (v < 0x80) {
                        out += (char)v;
                    } else if (v < 0x800) {
                        out += (char)(0xC0 | (v >> 6));
                        out += (char)(0x80 | (v & 0x3F));
                    } else {
                        out += (char)(0xE0 | (v >> 12));
                        out += (char)(0x80 | ((v >> 6) & 0x3F));
                        out += (char)(0x80 | (v & 0x3F));
                    }
                }
                break;
            default: out += e; break;
        }
    }
    return out;
}

// Bỏ ngoặc kép / dấu phẩy thừa quanh giá trị lấy theo dòng.
std::string cleanValue(std::string v)
{
    v = trim(v);
    while (!v.empty() && (v.back() == ',' || v.back() == '"' || v.back() == '}')) v.pop_back();
    while (!v.empty() && v.front() == '"') v.erase(0, 1);
    return trim(v);
}

// Model chuẩn: ưu tiên mã iPhoneN,M trong giá trị đọc được, rồi trong toàn bộ log.
std::string normalizeModel(const std::string &value, const std::string &raw)
{
    std::string m = findProductType(value);
    if (m.empty()) m = findProductType(raw);
    if (m.empty()) m = cleanValue(value);
    return m;
}

// Gắn Panic Parser v2 vào kết quả cũ. error_codes (CODE HEX gợi ý + dữ liệu cho AI) lấy từ
// fact: không còn địa chỉ kernel, địa chỉ caller, thanh ghi hay trạng thái (x,y) của IOService.
void attachFacts(ParsedPanic &out, const HCCPanicFacts::FileFields &ff)
{
    out.facts = HCCPanicFacts::extract(out.panic_string, ff);
    out.error_codes.clear();
    for (const auto &c : out.facts.codes) {
        if (!c.in_message || c.value == 0 || out.error_codes.size() >= 8) continue;
        std::string s = (c.base == 16) ? c.raw + " (dec " + c.dec + ")" : c.raw + " (" + c.hex + ")";
        if (std::find(out.error_codes.begin(), out.error_codes.end(), s) == out.error_codes.end())
            out.error_codes.push_back(s);
    }
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
        if (!payload) {
            const char *ep = cJSON_GetErrorPtr();
            ESP_LOGW(TAG, "payload JSON parse failed (%u bytes, at offset %d)", (unsigned)rest.size(),
                     ep ? (int)(ep - rest.c_str()) : -1);
        }
    }
    if (!payload && !header) {
        // Thử parse toàn bộ như một JSON (bắt buộc hết chuỗi: không lấy nhầm dòng header).
        payload = cJSON_ParseWithOpts(raw.c_str(), nullptr, true);
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

    // Trường file cho PanicFacts, mỗi trường ghi rõ lấy từ header hay payload.
    HCCPanicFacts::FileFields ff;
    ff.format = payload && payload != header ? "ips_json" : "ips_text";
    auto field = [&](const char *key, std::string &dst) {
        std::string v = findKey(header, key);
        const char *where = "header.";
        if (v.empty()) {
            v = findKey(payload, key);
            where = "payload.";
        }
        if (v.empty()) {
            v = escapedValue(raw, key);
            where = "text.";
        }
        if (v.empty()) return;
        dst = v;
        PanicEvidence e;
        e.field = std::string(where) + key;
        e.excerpt = v.substr(0, 240);
        ff.evidence.push_back(e);
    };
    field("bug_type", ff.bug_type);
    field("os_version", ff.os_version);
    field("timestamp", ff.timestamp);
    field("incident_id", ff.incident_id);
    field("build", ff.build);
    field("socId", ff.soc_id);
    field("panicFlags", ff.panic_flags);

    out.model = normalizeModel(firstNonEmpty({lookup("product"), lookup("modelCode"),
                                              lookup("hardware_model"), lookup("model")}), raw);
    out.os_version = firstNonEmpty({lookup("os_version"), lookup("build"),
                                    lookup("kernel"), lookup("osVersion")});
    out.panic_type = firstNonEmpty({lookup("bug_type"), lookup("panicType"),
                                    lookup("bugType")});
    out.panic_string = firstNonEmpty({lookup("panicString"), lookup("panic_string"),
                                      lookup("description")});
    if (out.panic_string.empty()) {
        // Payload không parse được: lấy panicString thẳng từ text.
        out.panic_string = firstNonEmpty({escapedValue(raw, "panicString"), escapedValue(raw, "panic_string")});
    }
    if (out.os_version.empty()) out.os_version = escapedValue(raw, "build");
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
    if (out.panic_string.empty()) {
        // Payload hỏng và không có panicString: phần sau dòng header là ngữ cảnh tốt nhất.
        size_t nl2 = raw.find('\n');
        out.panic_string = (nl2 != std::string::npos) ? raw.substr(nl2 + 1) : raw;
        if (trim(out.panic_string).empty()) out.panic_string = raw;
    }

    extractFromPanicString(out, out.panic_string);
    out.raw_excerpt = out.panic_string.substr(0, 400);
    buildSignature(out);
    out.valid = true;

    ff.product = findProductType(out.model);
    if (!ff.product.empty()) {
        PanicEvidence e;
        e.field = lookup("product").empty() ? "text.product" : "product";
        e.excerpt = ff.product;
        ff.evidence.push_back(e);
    }
    attachFacts(out, ff);

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

    out.model = normalizeModel(firstNonEmpty({quotedValue(raw, "product"), grabAfter("product"),
                                              grabAfter("model")}), raw);
    out.os_version = firstNonEmpty({quotedValue(raw, "os_version"), quotedValue(raw, "build"),
                                    cleanValue(grabAfter("os version")), cleanValue(grabAfter("build")),
                                    cleanValue(grabAfter("kernel version"))});
    out.panic_type = firstNonEmpty({quotedValue(raw, "bug_type"), cleanValue(grabAfter("panic type")),
                                    cleanValue(grabAfter("bug_type"))});
    out.source = grabAfter("source");
    out.index = grabAfter("index");
    // Text có "panicString":"..." (JSON không parse được): dùng đúng đoạn panic đó.
    out.panic_string = escapedValue(raw, "panicString");
    if (out.panic_string.empty()) out.panic_string = raw;

    extractFromPanicString(out, out.panic_string);
    out.raw_excerpt = trim(raw).substr(0, 400);
    buildSignature(out);
    out.valid = true;

    HCCPanicFacts::FileFields ff;
    ff.format = "text";
    auto textField = [&](const char *name, const std::string &v, std::string &dst) {
        if (v.empty()) return;
        dst = v;
        PanicEvidence e;
        e.field = std::string("text.") + name;
        e.excerpt = v.substr(0, 240);
        ff.evidence.push_back(e);
    };
    textField("bug_type", out.panic_type, ff.bug_type);
    textField("os_version", out.os_version, ff.os_version);
    textField("product", findProductType(out.model), ff.product);
    textField("panicFlags", quotedValue(raw, "panicFlags"), ff.panic_flags);
    attachFacts(out, ff);
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
