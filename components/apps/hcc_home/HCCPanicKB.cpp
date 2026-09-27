#include "HCCPanicKB.hpp"
#include "HCCPanicStorage.hpp"

#include <cctype>
#include <cstring>
#include <cstdio>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "PanicKB";

static const char *KB_VERIFIED = "kb/verified";
static const char *KB_CONFIRMED = "kb/repair_confirmed";
static const char *KB_AI = "kb/ai_inference";

namespace {

std::string toLower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string jstr(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return std::string();
}

// Keyword "tầm thường" (không đủ đặc trưng để tra cứu): quá ngắn, hoặc hex toàn số 0.
bool isTrivialKeyword(const std::string &kw)
{
    if (kw.size() < 4) return true;
    if (kw.rfind("0x", 0) == 0 || kw.rfind("0X", 0) == 0) {
        bool all_zero = true;
        for (size_t i = 2; i < kw.size(); ++i) {
            if (kw[i] != '0') { all_zero = false; break; }
        }
        if (all_zero) return true;
    }
    return false;
}

// Chuẩn hoá signature thành tên file an toàn.
std::string safeName(const std::string &sig)
{
    std::string s;
    for (char c : sig) {
        if (isalnum((unsigned char)c)) s.push_back((char)tolower((unsigned char)c));
        else s.push_back('_');
        if (s.size() >= 48) break;
    }
    if (s.empty()) s = "entry";
    return s;
}

}  // namespace

void HCCPanicKB::ensureLayout()
{
    HCCPanicStorage::ensureDir(KB_VERIFIED);
    HCCPanicStorage::ensureDir(KB_CONFIRMED);
    HCCPanicStorage::ensureDir(KB_AI);
}

bool HCCPanicKB::lookupInTier(const char *rel_dir, const char *tier_name,
                              const ParsedPanic &parsed, PanicResult &out)
{
    std::vector<std::string> files = HCCPanicStorage::listDir(rel_dir, ".json");
    const std::string p_model = toLower(parsed.model);
    const std::string p_type = toLower(parsed.panic_type);

    // Ngữ cảnh để so khớp keyword: signature + các dòng sensor + panic string.
    std::string haystack = toLower(parsed.signature);
    for (const auto &s : parsed.sensors) haystack += "\n" + toLower(s);
    haystack += "\n" + toLower(parsed.panic_string);

    int best_score = -1;
    for (const auto &fn : files) {
        std::string content;
        if (!HCCPanicStorage::readFile(std::string(rel_dir) + "/" + fn, content)) continue;
        cJSON *root = cJSON_Parse(content.c_str());
        if (!root) continue;

        const cJSON *match = cJSON_GetObjectItemCaseSensitive(root, "match");
        std::string m_model = match ? toLower(jstr(match, "model")) : std::string();
        std::string m_type = match ? toLower(jstr(match, "panic_type")) : std::string();
        std::string m_kw = match ? toLower(jstr(match, "keyword")) : std::string();

        // BẮT BUỘC có keyword ĐẶC TRƯNG và phải xuất hiện trong panic -> tránh khớp bừa.
        // Bỏ qua keyword tầm thường (0x0, quá ngắn) và keyword không có trong panic.
        if (m_kw.empty() || isTrivialKeyword(m_kw) || haystack.find(m_kw) == std::string::npos) {
            cJSON_Delete(root);
            continue;
        }
        int score = 1;  // keyword đã khớp

        bool ok = true;
        if (!m_model.empty()) {
            if (p_model.find(m_model) == std::string::npos) ok = false;
            else score++;
        }
        if (ok && !m_type.empty()) {
            if (p_type.find(m_type) == std::string::npos) ok = false;
            else score++;
        }

        std::string e_error = jstr(root, "error");
        std::string e_cause = jstr(root, "cause");
        std::string e_repair = jstr(root, "repair");
        // Entry rỗng nội dung (không có mô tả lỗi/sửa) -> bỏ qua, để rơi xuống AI.
        if (e_error.empty() && e_cause.empty() && e_repair.empty()) {
            ESP_LOGW(TAG, "KB skip empty entry [%s] file=%s keyword='%s'",
                     tier_name, fn.c_str(), m_kw.c_str());
            cJSON_Delete(root);
            continue;
        }

        if (ok && score > best_score) {
            best_score = score;
            out.error = e_error;
            out.cause = e_cause;
            out.repair = e_repair;
            out.confidence = jstr(root, "confidence");
            out.source_tier = tier_name;
            out.ok = true;
            ESP_LOGI(TAG, "KB match [%s] file=%s keyword='%s' score=%d",
                     tier_name, fn.c_str(), m_kw.c_str(), score);
        }
        cJSON_Delete(root);
    }
    return best_score >= 0;
}

PanicResult HCCPanicKB::lookup(const ParsedPanic &parsed)
{
    PanicResult r;
    // Chỉ tra KB do người dùng xác nhận: VERIFIED > REPAIR_CONFIRMED.
    // KHÔNG tra cache AI_INFERENCE (tránh khớp bừa từ suy luận AI cũ).
    if (lookupInTier(KB_VERIFIED, "VERIFIED", parsed, r)) return r;
    if (lookupInTier(KB_CONFIRMED, "REPAIR_CONFIRMED", parsed, r)) return r;
    r.ok = false;
    return r;
}

bool HCCPanicKB::saveCase(const RepairCase &c, bool verified)
{
    ensureLayout();
    const char *tier_dir = verified ? KB_VERIFIED : KB_CONFIRMED;
    const char *tier_name = verified ? "VERIFIED" : "REPAIR_CONFIRMED";

    // id ổn định theo file panic (một case / panic).
    std::string base = c.panic_file.empty() ? c.signature : c.panic_file;
    std::string id = safeName(base);

    // keyword để tra cứu panic sau: ưu tiên mã hex (0x...), rồi source, rồi signature.
    std::string keyword;
    if (!c.code_hex.empty()) {
        // lấy token đầu (vd "0x800000" từ "0x800000 (dec ...)")
        size_t sp = c.code_hex.find_first_of(" \t");
        keyword = (sp == std::string::npos) ? c.code_hex : c.code_hex.substr(0, sp);
    }
    if (keyword.empty()) keyword = c.source;
    // keyword rỗng => match theo model+panic_type (lookup coi keyword rỗng là "bất kỳ").

    std::string cause = c.source;
    if (!c.code_hex.empty()) cause += (cause.empty() ? "" : " / ") + c.code_hex;
    if (cause.empty()) cause = c.note;

    std::string confidence = verified ? "Cao - da kiem chung (VERIFIED)"
                                      : "Cao - da sua thuc te (REPAIR_CONFIRMED)";

    cJSON *root = cJSON_CreateObject();
    cJSON *match = cJSON_CreateObject();
    cJSON_AddStringToObject(match, "model", c.model.c_str());
    cJSON_AddStringToObject(match, "panic_type", c.panic_type.c_str());
    cJSON_AddStringToObject(match, "keyword", keyword.c_str());
    cJSON_AddItemToObject(root, "match", match);

    cJSON_AddStringToObject(root, "error", c.hardware.c_str());
    cJSON_AddStringToObject(root, "cause", cause.c_str());
    cJSON_AddStringToObject(root, "repair", c.repair_action.c_str());
    cJSON_AddStringToObject(root, "confidence", confidence.c_str());
    cJSON_AddStringToObject(root, "tier", tier_name);

    cJSON *cs = cJSON_CreateObject();
    cJSON_AddStringToObject(cs, "panic_file", c.panic_file.c_str());
    cJSON_AddStringToObject(cs, "model", c.model.c_str());
    cJSON_AddStringToObject(cs, "panic_type", c.panic_type.c_str());
    cJSON_AddStringToObject(cs, "signature", c.signature.c_str());
    cJSON_AddStringToObject(cs, "code_hex", c.code_hex.c_str());
    cJSON_AddStringToObject(cs, "source", c.source.c_str());
    cJSON_AddStringToObject(cs, "hardware", c.hardware.c_str());
    cJSON_AddStringToObject(cs, "repair_action", c.repair_action.c_str());
    cJSON_AddStringToObject(cs, "note", c.note.c_str());
    cJSON_AddStringToObject(cs, "created_at", c.created_at.c_str());
    cJSON_AddStringToObject(cs, "status", tier_name);
    cJSON_AddItemToObject(root, "case", cs);

    char *printed = cJSON_Print(root);
    bool ok = false;
    std::string rel = std::string(tier_dir) + "/" + id + ".json";
    if (printed) {
        ok = HCCPanicStorage::writeFile(rel, printed, strlen(printed));
        cJSON_free(printed);
    }
    cJSON_Delete(root);

    if (!ok) {
        ESP_LOGW(TAG, "saveCase write failed: %s", rel.c_str());
        return false;
    }

    // ĐỌC LẠI để xác nhận đã lưu đúng (yêu cầu 6).
    std::string verify;
    if (!HCCPanicStorage::readFile(rel, verify)) {
        ESP_LOGW(TAG, "saveCase readback failed: %s", rel.c_str());
        return false;
    }
    cJSON *rb = cJSON_Parse(verify.c_str());
    bool confirmed = false;
    if (rb) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(rb, "tier");
        confirmed = (cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, tier_name) == 0);
        cJSON_Delete(rb);
    }
    if (!confirmed) {
        ESP_LOGW(TAG, "saveCase readback mismatch: %s", rel.c_str());
        return false;
    }

    // Khi VERIFY: bỏ bản REPAIR_CONFIRMED cùng id (đã lên VERIFIED).
    if (verified) {
        std::string rc = std::string(KB_CONFIRMED) + "/" + id + ".json";
        remove(HCCPanicStorage::absPath(rc).c_str());
    }

    ESP_LOGI(TAG, "saveCase %s -> %s (keyword=%s)", tier_name, rel.c_str(), keyword.c_str());
    return true;
}

bool HCCPanicKB::saveAiResult(const ParsedPanic &parsed, const PanicResult &result)
{
    ensureLayout();
    cJSON *root = cJSON_CreateObject();
    cJSON *match = cJSON_CreateObject();
    cJSON_AddStringToObject(match, "model", parsed.model.c_str());
    cJSON_AddStringToObject(match, "panic_type", parsed.panic_type.c_str());
    // keyword = từ khoá chính (dòng sensor đầu tiên, cắt ngắn) để lần sau tra lại.
    std::string kw = parsed.sensors.empty() ? std::string() : parsed.sensors[0];
    if (kw.size() > 40) kw = kw.substr(0, 40);
    cJSON_AddStringToObject(match, "keyword", kw.c_str());
    cJSON_AddItemToObject(root, "match", match);

    cJSON_AddStringToObject(root, "error", result.error.c_str());
    cJSON_AddStringToObject(root, "cause", result.cause.c_str());
    cJSON_AddStringToObject(root, "repair", result.repair.c_str());
    cJSON_AddStringToObject(root, "confidence", result.confidence.c_str());
    cJSON_AddStringToObject(root, "tier", "AI_INFERENCE");

    char *printed = cJSON_Print(root);
    bool ok = false;
    if (printed) {
        std::string rel = std::string(KB_AI) + "/" + safeName(parsed.signature) + ".json";
        ok = HCCPanicStorage::writeFile(rel, printed, strlen(printed));
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    if (!ok) ESP_LOGW(TAG, "save AI result failed");
    return ok;
}
