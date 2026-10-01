#include "HCCPanicKBStore.hpp"

#include <algorithm>

#include "HCCPanicStorage.hpp"
#include "esp_log.h"

static const char *TAG = "PanicKBv1";

static const char *DIR_USER = "kb/v1/user";
static const char *DIR_CASES = "kb/v1/cases";
static const char *DIR_LEGACY_VERIFIED = "kb/verified";
static const char *DIR_LEGACY_CONFIRMED = "kb/repair_confirmed";
static const char *DIR_RAW = "panic/raw";

// kb/hcc_kb_default.json nhúng bởi EMBED_TXTFILES (components/apps/CMakeLists.txt), có '\0' cuối.
extern const char hcc_kb_default_start[] asm("_binary_hcc_kb_default_json_start");

const char *HCCPanicKBStore::defaultKbJson()
{
    return hcc_kb_default_start;
}

static void loadLegacyDir(const char *dir, std::vector<HCCKB::CaseRecord> &out, std::vector<std::string> &msgs)
{
    for (const auto &fn : HCCPanicStorage::listDir(dir, ".json")) {
        std::string json;
        if (!HCCPanicStorage::readFile(std::string(dir) + "/" + fn, json)) continue;
        // File panic gốc (nếu còn) để tính signature V1.
        std::string raw;
        HCCKB::CaseRecord probe;
        if (HCCKB::migrateLegacyCase(json, dir, std::string(), probe) && !probe.evidence_panic_file.empty()) {
            HCCPanicStorage::readFile(std::string(DIR_RAW) + "/" + probe.evidence_panic_file, raw);
        }
        HCCKB::CaseRecord c;
        if (!HCCKB::migrateLegacyCase(json, dir, raw, c)) {
            msgs.push_back(std::string(dir) + "/" + fn + ": không đọc được (bỏ qua, file giữ nguyên)");
            continue;
        }
        c.legacy_file = fn;
        for (const auto &w : c.warnings) msgs.push_back(c.id + ": " + w);
        out.push_back(c);
    }
}

bool HCCPanicKBStore::load(HCCKB::KnowledgeBase &kb, std::vector<HCCKB::CaseRecord> &cases, LoadReport &report)
{
    kb = HCCKB::KnowledgeBase();
    cases.clear();
    report = LoadReport();
    if (!HCCKB::loadKnowledgeBase(defaultKbJson(), "firmware", kb)) {
        ESP_LOGE(TAG, "KB mặc định lỗi");
    }
    HCCPanicStorage::ensureMounted();
    for (const auto &fn : HCCPanicStorage::listDir(DIR_USER, ".json")) {
        std::string json;
        if (HCCPanicStorage::readFile(std::string(DIR_USER) + "/" + fn, json) &&
            HCCKB::loadKnowledgeBase(json, ("sd:" + fn).c_str(), kb)) {
            report.user_files++;
        }
    }
    for (const auto &fn : HCCPanicStorage::listDir(DIR_CASES, ".json")) {
        std::string json;
        HCCKB::CaseRecord c;
        if (!HCCPanicStorage::readFile(std::string(DIR_CASES) + "/" + fn, json) || !HCCKB::parseCase(json, c)) {
            report.messages.push_back(std::string(DIR_CASES) + "/" + fn + ": case không hợp lệ (bỏ qua)");
            continue;
        }
        for (const auto &w : c.warnings) report.messages.push_back(c.id + ": " + w);
        cases.push_back(c);
    }
    report.cases = (int)cases.size();
    // Case cũ: chỉ thêm nếu chưa có bản V1 cùng id (đã migrate ra SD).
    std::vector<HCCKB::CaseRecord> legacy;
    loadLegacyDir(DIR_LEGACY_VERIFIED, legacy, report.messages);
    loadLegacyDir(DIR_LEGACY_CONFIRMED, legacy, report.messages);
    for (auto &c : legacy) {
        bool dup = false;
        for (const auto &x : cases) dup = dup || x.id == c.id;
        if (!dup) {
            cases.push_back(c);
            report.legacy_cases++;
        }
    }
    for (const auto &e : kb.errors) report.messages.push_back(e);
    ESP_LOGI(TAG, "KB v1: %u kind, %u driver, %u sensor rule; %d user file; %d case + %d case cũ; %u thông báo",
             (unsigned)kb.kind_rules.size(), (unsigned)kb.driver_rules.size(), (unsigned)kb.sensor_rules.size(),
             report.user_files, report.cases, report.legacy_cases, (unsigned)report.messages.size());
    return kb.kb_version == 1;
}

bool HCCPanicKBStore::saveCase(const HCCKB::CaseRecord &c, std::string &error)
{
    if (c.id.empty()) {
        error = "case thiếu id";
        return false;
    }
    const std::string rel = std::string(DIR_CASES) + "/" + c.id + ".json";
    const std::string json = HCCKB::caseToJson(c);
    if (!HCCPanicStorage::writeFile(rel, json.c_str(), json.size())) {
        error = "ghi thất bại: " + rel;
        return false;
    }
    std::string back;
    HCCKB::CaseRecord rb;
    if (!HCCPanicStorage::readFile(rel, back) || !HCCKB::parseCase(back, rb) || rb.id != c.id ||
        rb.status != c.status) {
        error = "đọc lại không khớp: " + rel + (rb.status != c.status ? " (status " + rb.status + ")" : "");
        return false;
    }
    return true;
}

int HCCPanicKBStore::migrateLegacyToSd(std::vector<std::string> &messages)
{
    std::vector<HCCKB::CaseRecord> legacy;
    loadLegacyDir(DIR_LEGACY_VERIFIED, legacy, messages);
    loadLegacyDir(DIR_LEGACY_CONFIRMED, legacy, messages);
    int n = 0;
    for (const auto &c : legacy) {
        std::string err;
        if (saveCase(c, err)) n++;
        else messages.push_back(c.id + ": " + err);
    }
    return n;
}
