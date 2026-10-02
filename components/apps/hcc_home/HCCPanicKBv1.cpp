#include "HCCPanicKBv1.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "HCCPanicParser.hpp"
#include "cJSON.h"

namespace HCCKB {

namespace {

std::string lower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string jstr(const cJSON *o, const char *k)
{
    const cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr;
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : std::string();
}

int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr;
    return cJSON_IsNumber(it) ? it->valueint : def;
}

bool jbool(const cJSON *o, const char *k, bool def)
{
    const cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    return def;
}

std::vector<std::string> jstrs(const cJSON *o, const char *k)
{
    std::vector<std::string> v;
    const cJSON *arr = o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr;
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr)
    {
        if (cJSON_IsString(it) && it->valuestring) v.push_back(it->valuestring);
    }
    return v;
}

bool parseLevel(const std::string &s, Level &out)
{
    if (s == "DOCUMENTED") out = Level::Documented;
    else if (s == "INFERRED") out = Level::Inferred;
    else if (s == "VERIFIED") out = Level::Verified;
    else if (s == "UNKNOWN" || s.empty()) out = Level::Unknown;
    else return false;
    return true;
}

bool trustedSourceType(const std::string &t)
{
    return t == "apple_source" || t == "apple_doc" || t == "standard";
}

const Source *findSource(const KnowledgeBase &kb, const std::string &id)
{
    for (const auto &s : kb.sources) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

// Kiểm tra quy tắc bằng chứng của một field. Trả false + ghi lỗi nếu vi phạm.
bool validateLevel(Level level, const std::vector<std::string> &sources, const std::string &reasoning,
                   const KnowledgeBase &kb, const std::string &ctx, std::vector<std::string> &errors)
{
    for (const auto &id : sources) {
        if (!findSource(kb, id)) {
            errors.push_back(ctx + ": nguồn '" + id + "' không có trong sources");
            return false;
        }
    }
    switch (level) {
        case Level::Verified:
            errors.push_back(ctx + ": VERIFIED chỉ đến từ case sửa thực tế, không dùng trong rule");
            return false;
        case Level::Documented:
            if (sources.empty()) {
                errors.push_back(ctx + ": DOCUMENTED phải có nguồn");
                return false;
            }
            for (const auto &id : sources) {
                if (!trustedSourceType(findSource(kb, id)->type)) {
                    errors.push_back(ctx + ": DOCUMENTED chỉ dùng nguồn apple_source/apple_doc/standard ('" + id +
                                     "' là " + findSource(kb, id)->type + ")");
                    return false;
                }
            }
            return true;
        case Level::Inferred:
            if (reasoning.empty()) {
                errors.push_back(ctx + ": INFERRED phải ghi lý do (reasoning)");
                return false;
            }
            return true;
        case Level::Unknown:
            return true;
    }
    return true;
}

bool parseField(const cJSON *o, const char *key, Field &f, const KnowledgeBase &kb, const std::string &ctx,
                std::vector<std::string> &errors)
{
    const cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, key) : nullptr;
    if (!it) {
        f = Field();
        f.value = "UNKNOWN";
        return true;
    }
    f.value = jstr(it, "value");
    if (f.value.empty()) f.value = "UNKNOWN";
    f.sources = jstrs(it, "sources");
    f.reasoning = jstr(it, "reasoning");
    if (!parseLevel(jstr(it, "level"), f.level)) {
        errors.push_back(ctx + "." + key + ": level không hợp lệ '" + jstr(it, "level") + "'");
        return false;
    }
    if (f.level != Level::Unknown && f.value == "UNKNOWN") {
        errors.push_back(ctx + "." + key + ": value UNKNOWN nhưng level " + levelName(f.level));
        return false;
    }
    return validateLevel(f.level, f.sources, f.reasoning, kb, ctx + "." + key, errors);
}

template <typename T>
void upsert(std::vector<T> &v, const T &item)
{
    for (auto &x : v) {
        if (x.id == item.id) {
            x = item;
            return;
        }
    }
    v.push_back(item);
}

bool containsToken(const std::string &hay, const std::string &kw)
{
    if (kw.empty()) return false;
    for (size_t p = hay.find(kw); p != std::string::npos; p = hay.find(kw, p + 1)) {
        const size_t e = p + kw.size();
        const bool l = p == 0 || !isalnum((unsigned char)hay[p - 1]) || !isalnum((unsigned char)kw[0]);
        const bool r = e >= hay.size() || !isalnum((unsigned char)hay[e]) || !isalnum((unsigned char)kw.back());
        if (l && r) return true;
    }
    return false;
}

// So khớp mẫu có '*' (khớp chuỗi bất kỳ), vd "s.sensor[*]".
bool glob(const std::string &pat, const std::string &s)
{
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && pat[p] == s[i]) {
            p++;
            i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

std::string safeId(const std::string &s)
{
    std::string o;
    for (char c : s) {
        o += isalnum((unsigned char)c) ? (char)tolower((unsigned char)c) : '_';
        if (o.size() >= 60) break;
    }
    return o.empty() ? std::string("case") : o;
}

bool modelMatches(const std::vector<std::string> &models, const std::string &product)
{
    if (models.empty()) return true;
    for (const auto &m : models) {
        if (m == product) return true;
    }
    return false;
}

int confRank(Confidence c)
{
    return (int)c;
}

int matchRank(const std::string &t)
{
    if (t == "exact") return 5;
    if (t == "signature_no_discriminator") return 0;
    if (t == "same_signature_other_model") return 3;
    if (t == "legacy_keyword") return 2;
    return 1;
}

void addCaseFromFacts(CaseRecord &c, const PanicFacts &f)
{
    c.model = f.product;
    c.ios = f.os_version.empty() ? f.build : f.os_version;
    c.panic_type = f.bug_type;
    c.primary_kind = panicKindName(f.primary_kind);
    c.signature = f.signature;
    c.signature_generic = f.signature_generic;
    c.drivers.clear();
    c.sensors.clear();
    c.codes.clear();
    for (const auto &d : f.drivers) {
        if (d.role != "watchdog_daemon") c.drivers.push_back(d.name);
    }
    for (const auto &s : f.sensors) c.sensors.push_back(s.name);
    for (const auto &k : f.codes) {
        if (k.in_message && k.value != 0) c.codes.push_back(k.label + "=" + k.hex);
    }
    c.smc_target = f.smc_target;
    c.assert_tags = f.assert_tags;
}

void validateCase(CaseRecord &c)
{
    if (c.status != "VERIFIED" && c.status != "REPAIR_CONFIRMED" && c.status != "DRAFT") {
        c.warnings.push_back("status '" + c.status + "' không hợp lệ -> DRAFT");
        c.status = "DRAFT";
    }
    if (c.status == "VERIFIED") {
        std::vector<std::string> missing;
        if (c.signature.empty() && c.legacy_keyword.empty() && c.legacy_signature.empty())
            missing.push_back("signature");
        if (c.repair_action.empty()) missing.push_back("repair_action");
        if (!c.result.panic_resolved) missing.push_back("result.panic_resolved=true");
        if (c.model.empty()) missing.push_back("model");
        if (!missing.empty()) {
            std::string m;
            for (const auto &x : missing) m += (m.empty() ? "" : ", ") + x;
            c.warnings.push_back("VERIFIED thiếu " + m + " -> hạ REPAIR_CONFIRMED (giữ nguyên dữ liệu)");
            c.status = "REPAIR_CONFIRMED";
        }
    }
}

}  // namespace

const char *levelName(Level l)
{
    switch (l) {
        case Level::Documented: return "DOCUMENTED";
        case Level::Inferred: return "INFERRED";
        case Level::Verified: return "VERIFIED";
        case Level::Unknown: break;
    }
    return "UNKNOWN";
}

const char *confidenceName(Confidence c)
{
    switch (c) {
        case Confidence::High: return "HIGH";
        case Confidence::Medium: return "MEDIUM";
        case Confidence::Low: return "LOW";
        case Confidence::None: break;
    }
    return "NONE";
}

bool loadKnowledgeBase(const std::string &json, const char *origin, KnowledgeBase &kb)
{
    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) {
        kb.errors.push_back(std::string(origin) + ": JSON không hợp lệ");
        return false;
    }
    const int ver = jint(root, "kb_version", 0);
    if (ver != 1) {
        kb.errors.push_back(std::string(origin) + ": kb_version phải là 1");
        cJSON_Delete(root);
        return false;
    }
    kb.kb_version = 1;
    const cJSON *it = nullptr;

    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "sources"))
    {
        Source s;
        s.id = jstr(it, "id");
        s.type = jstr(it, "type");
        s.title = jstr(it, "title");
        s.ref = jstr(it, "ref");
        s.note = jstr(it, "note");
        static const char *types[] = {"apple_source", "apple_doc", "standard", "community", "research",
                                      "hcc_observation"};
        bool ok = false;
        for (const char *t : types) ok = ok || s.type == t;
        if (s.id.empty() || !ok) {
            kb.errors.push_back(std::string(origin) + ": source '" + s.id + "' thiếu id hoặc type không hợp lệ");
            continue;
        }
        upsert(kb.sources, s);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "groups"))
    {
        Group g;
        g.id = jstr(it, "id");
        g.name = jstr(it, "name");
        g.kinds = jstrs(it, "kinds");
        g.tags = jstrs(it, "tags");
        if (!g.id.empty()) upsert(kb.groups, g);
    }
    auto checks = [&](const cJSON *o, const std::string &ctx, std::vector<Check> &out) {
        bool ok = true;
        const cJSON *c = nullptr;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(o, "checks"))
        {
            Check k;
            k.step = jstr(c, "step");
            k.sources = jstrs(c, "sources");
            k.reasoning = jstr(c, "reasoning");
            if (!parseLevel(jstr(c, "level"), k.level)) {
                kb.errors.push_back(ctx + ".checks: level không hợp lệ");
                ok = false;
                continue;
            }
            ok = validateLevel(k.level, k.sources, k.reasoning, kb, ctx + ".checks[" + k.step + "]", kb.errors) && ok;
            out.push_back(k);
        }
        return ok;
    };
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "kind_rules"))
    {
        KindRule r;
        r.id = jstr(it, "id");
        r.kind = jstr(it, "kind");
        r.group = jstr(it, "group");
        const std::string ctx = std::string(origin) + ":kind_rules." + r.id;
        bool ok = !r.id.empty() && !r.kind.empty();
        ok = parseField(it, "mechanism", r.mechanism, kb, ctx, kb.errors) && ok;
        ok = parseField(it, "hardware_area", r.hardware_area, kb, ctx, kb.errors) && ok;
        ok = checks(it, ctx, r.checks) && ok;
        if (ok) upsert(kb.kind_rules, r);
        else kb.errors.push_back(ctx + ": rule bị bỏ");
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "driver_rules"))
    {
        DriverRule r;
        r.id = jstr(it, "id");
        r.group = jstr(it, "group");
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "match");
        for (auto &s : jstrs(m, "exact")) r.exact.push_back(lower(s));
        for (auto &s : jstrs(m, "prefix")) r.prefix.push_back(lower(s));
        for (auto &s : jstrs(m, "contains")) r.contains.push_back(lower(s));
        r.requires_facts = jint(it, "requires_facts", 2);
        const std::string ctx = std::string(origin) + ":driver_rules." + r.id;
        bool ok = !r.id.empty() && (!r.exact.empty() || !r.prefix.empty() || !r.contains.empty());
        ok = parseField(it, "subsystem", r.subsystem, kb, ctx, kb.errors) && ok;
        ok = parseField(it, "hardware_area", r.hardware_area, kb, ctx, kb.errors) && ok;
        ok = parseField(it, "bus_rail", r.bus_rail, kb, ctx, kb.errors) && ok;
        ok = parseField(it, "components", r.components, kb, ctx, kb.errors) && ok;
        if (r.requires_facts < 2) {
            kb.errors.push_back(ctx + ": requires_facts < 2 (không kết luận linh kiện từ 1 keyword)");
            ok = false;
        }
        if (ok) upsert(kb.driver_rules, r);
        else kb.errors.push_back(ctx + ": rule bị bỏ");
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "sensor_rules"))
    {
        SensorRule r;
        r.id = jstr(it, "id");
        r.name = lower(jstr(it, "name"));
        r.models = jstrs(it, "models");
        const std::string ctx = std::string(origin) + ":sensor_rules." + r.id;
        bool ok = !r.id.empty() && !r.name.empty();
        ok = parseField(it, "meaning", r.meaning, kb, ctx, kb.errors) && ok;
        ok = parseField(it, "location", r.location, kb, ctx, kb.errors) && ok;
        if (ok) upsert(kb.sensor_rules, r);
        else kb.errors.push_back(ctx + ": rule bị bỏ");
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "code_rules"))
    {
        CodeRule r;
        r.id = jstr(it, "id");
        r.label = lower(jstr(it, "label"));
        r.models = jstrs(it, "models");
        const std::string ctx = std::string(origin) + ":code_rules." + r.id;
        bool ok = !r.id.empty() && !r.label.empty();
        ok = parseField(it, "meaning", r.meaning, kb, ctx, kb.errors) && ok;
        const cJSON *b = nullptr;
        cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(it, "bits"))
        {
            BitMeaning bm;
            bm.bit = jint(b, "bit", -1);
            ok = parseField(b, "meaning", bm.meaning, kb, ctx + ".bit" + std::to_string(bm.bit), kb.errors) && ok;
            if (bm.bit < 0 || bm.bit > 63) ok = false;
            r.bits.push_back(bm);
        }
        if (ok) upsert(kb.code_rules, r);
        else kb.errors.push_back(ctx + ": rule bị bỏ");
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "unknowns"))
    {
        UnknownItem u;
        u.id = jstr(it, "id");
        u.what = jstr(it, "what");
        u.value = lower(jstr(it, "value"));
        u.reason = jstr(it, "reason");
        u.seen_in = jstr(it, "seen_in");
        if (!u.id.empty() && !u.what.empty()) upsert(kb.unknowns, u);
    }
    cJSON_Delete(root);
    return true;
}

// ---------------- Case JSON ----------------

bool parseCase(const std::string &json, CaseRecord &c)
{
    cJSON *r = cJSON_Parse(json.c_str());
    if (!r) return false;
    if (jstr(r, "schema") != "hcc.case.v1") {
        cJSON_Delete(r);
        return false;
    }
    c = CaseRecord();
    c.id = jstr(r, "id");
    c.status = jstr(r, "status");
    c.model = jstr(r, "model");
    c.ios = jstr(r, "ios");
    c.panic_type = jstr(r, "panic_type");
    c.primary_kind = jstr(r, "primary_kind");
    c.signature = jstr(r, "signature");
    c.signature_generic = jstr(r, "signature_generic");
    c.drivers = jstrs(r, "drivers");
    c.sensors = jstrs(r, "sensors");
    c.codes = jstrs(r, "codes");
    c.smc_target = jstr(r, "smc_target");
    c.assert_tags = jstrs(r, "assert_tags");
    c.hardware_area = jstr(r, "hardware_area");
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(r, "components"))
    {
        c.components.push_back({jstr(it, "name"), jstr(it, "ref"), jstr(it, "action")});
    }
    c.bus_rail = jstrs(r, "bus_rail");
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(r, "measurements"))
    {
        Measurement m;
        m.point = jstr(it, "point");
        m.type = jstr(it, "type");
        m.before = jstr(it, "before");
        m.after = jstr(it, "after");
        m.unit = jstr(it, "unit");
        m.note = jstr(it, "note");
        c.measurements.push_back(m);
    }
    c.repair_action = jstr(r, "repair_action");
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(r, "result");
    c.result.panic_resolved = jbool(res, "panic_resolved", false);
    c.result.observed_days = jint(res, "observed_days", -1);
    c.result.note = jstr(res, "note");
    c.notes = jstr(r, "notes");
    c.date = jstr(r, "date");
    c.technician = jstr(r, "technician");
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(r, "evidence");
    c.evidence_panic_file = jstr(ev, "panic_file");
    c.evidence_excerpt = jstr(ev, "panic_excerpt");
    c.evidence_attachments = jstrs(ev, "attachments");
    const cJSON *lg = cJSON_GetObjectItemCaseSensitive(r, "legacy");
    c.legacy_dir = jstr(lg, "dir");
    c.legacy_file = jstr(lg, "file");
    c.legacy_keyword = jstr(lg, "keyword");
    c.legacy_signature = jstr(lg, "signature");
    cJSON_Delete(r);
    if (c.id.empty()) c.id = safeId(c.signature.empty() ? c.evidence_panic_file : c.signature);
    validateCase(c);
    return true;
}

std::string caseToJson(const CaseRecord &c)
{
    cJSON *r = cJSON_CreateObject();
    auto s = [](cJSON *o, const char *k, const std::string &v) { cJSON_AddStringToObject(o, k, v.c_str()); };
    auto arr = [](cJSON *o, const char *k, const std::vector<std::string> &v) {
        cJSON *a = cJSON_AddArrayToObject(o, k);
        for (const auto &x : v) cJSON_AddItemToArray(a, cJSON_CreateString(x.c_str()));
    };
    s(r, "schema", "hcc.case.v1");
    s(r, "id", c.id);
    s(r, "status", c.status);
    s(r, "model", c.model);
    s(r, "ios", c.ios);
    s(r, "panic_type", c.panic_type);
    s(r, "primary_kind", c.primary_kind);
    s(r, "signature", c.signature);
    s(r, "signature_generic", c.signature_generic);
    arr(r, "drivers", c.drivers);
    arr(r, "sensors", c.sensors);
    arr(r, "codes", c.codes);
    s(r, "smc_target", c.smc_target);
    arr(r, "assert_tags", c.assert_tags);
    s(r, "hardware_area", c.hardware_area);
    cJSON *comps = cJSON_AddArrayToObject(r, "components");
    for (const auto &x : c.components) {
        cJSON *o = cJSON_CreateObject();
        s(o, "name", x.name);
        s(o, "ref", x.ref);
        s(o, "action", x.action);
        cJSON_AddItemToArray(comps, o);
    }
    arr(r, "bus_rail", c.bus_rail);
    cJSON *ms = cJSON_AddArrayToObject(r, "measurements");
    for (const auto &m : c.measurements) {
        cJSON *o = cJSON_CreateObject();
        s(o, "point", m.point);
        s(o, "type", m.type);
        s(o, "before", m.before);
        s(o, "after", m.after);
        s(o, "unit", m.unit);
        s(o, "note", m.note);
        cJSON_AddItemToArray(ms, o);
    }
    s(r, "repair_action", c.repair_action);
    cJSON *res = cJSON_AddObjectToObject(r, "result");
    cJSON_AddBoolToObject(res, "panic_resolved", c.result.panic_resolved);
    cJSON_AddNumberToObject(res, "observed_days", c.result.observed_days);
    s(res, "note", c.result.note);
    s(r, "notes", c.notes);
    s(r, "date", c.date);
    s(r, "technician", c.technician);
    cJSON *ev = cJSON_AddObjectToObject(r, "evidence");
    s(ev, "panic_file", c.evidence_panic_file);
    s(ev, "panic_excerpt", c.evidence_excerpt);
    arr(ev, "attachments", c.evidence_attachments);
    if (!c.legacy_dir.empty() || !c.legacy_keyword.empty() || !c.legacy_signature.empty()) {
        cJSON *lg = cJSON_AddObjectToObject(r, "legacy");
        s(lg, "dir", c.legacy_dir);
        s(lg, "file", c.legacy_file);
        s(lg, "keyword", c.legacy_keyword);
        s(lg, "signature", c.legacy_signature);
    }
    char *p = cJSON_Print(r);
    std::string out = p ? p : "";
    if (p) cJSON_free(p);
    cJSON_Delete(r);
    return out;
}

CaseRecord caseFromFacts(const PanicFacts &f)
{
    CaseRecord c;
    c.status = "DRAFT";
    addCaseFromFacts(c, f);
    c.id = safeId(f.signature);
    if (!f.kind_evidence.empty()) c.evidence_excerpt = f.kind_evidence[0].excerpt;
    return c;
}

bool migrateLegacyCase(const std::string &legacy_json, const char *legacy_dir, const std::string &raw_panic,
                       CaseRecord &c)
{
    cJSON *r = cJSON_Parse(legacy_json.c_str());
    if (!r) return false;
    const cJSON *match = cJSON_GetObjectItemCaseSensitive(r, "match");
    const cJSON *cs = cJSON_GetObjectItemCaseSensitive(r, "case");
    c = CaseRecord();
    const std::string tier = jstr(r, "tier");
    c.legacy_dir = legacy_dir ? legacy_dir : "";
    c.legacy_keyword = jstr(match, "keyword");
    c.legacy_signature = jstr(cs, "signature");
    c.model = jstr(cs, "model").empty() ? jstr(match, "model") : jstr(cs, "model");
    c.panic_type = jstr(cs, "panic_type").empty() ? jstr(match, "panic_type") : jstr(cs, "panic_type");
    c.hardware_area = jstr(cs, "hardware").empty() ? jstr(r, "error") : jstr(cs, "hardware");
    c.repair_action = jstr(cs, "repair_action").empty() ? jstr(r, "repair") : jstr(cs, "repair_action");
    c.notes = jstr(cs, "note");
    c.date = jstr(cs, "created_at");
    c.evidence_panic_file = jstr(cs, "panic_file");
    c.legacy_file = c.evidence_panic_file;
    const std::string code = jstr(cs, "code_hex");
    if (!code.empty()) c.codes.push_back(code);
    const std::string src = jstr(cs, "source");
    if (!src.empty()) {
        if (lower(src).find("sensor") != std::string::npos) c.sensors.push_back(src);
        else c.notes += (c.notes.empty() ? "" : "\n") + std::string("SOURCE (cũ): ") + src;
    }
    // Case cũ đã tách cause/confidence -> giữ trong notes để không mất dữ liệu.
    const std::string cause = jstr(r, "cause");
    if (!cause.empty()) c.notes += (c.notes.empty() ? "" : "\n") + std::string("CAUSE (cũ): ") + cause;
    cJSON_Delete(r);

    if (tier == "VERIFIED") {
        // "XÁC NHẬN ĐÃ SỬA" của bản cũ = kỹ thuật viên xác nhận máy đã hết lỗi.
        c.status = "VERIFIED";
        c.result.panic_resolved = true;
        c.result.note = "migrate từ kb/verified (HCC cũ): kỹ thuật viên đã xác nhận đã sửa";
    } else {
        c.status = "REPAIR_CONFIRMED";
        c.result.note = "migrate từ kb/repair_confirmed (HCC cũ): chưa xác nhận hết lỗi";
    }
    if (!raw_panic.empty()) {
        ParsedPanic p = HCCPanicParser::parse(raw_panic);
        if (p.facts.valid) {
            const std::string model = c.model;
            addCaseFromFacts(c, p.facts);
            if (c.model.empty()) c.model = model;
            if (!code.empty() && std::find(c.codes.begin(), c.codes.end(), code) == c.codes.end())
                c.codes.push_back(code);
            c.warnings.push_back("signature V1 tính lại từ file panic gốc");
        }
    } else {
        c.warnings.push_back("không có file panic gốc: chỉ khớp theo keyword cũ (độ tin cậy thấp hơn)");
    }
    c.id = "legacy_" + safeId(c.evidence_panic_file.empty() ? c.legacy_signature : c.evidence_panic_file);
    validateCase(c);
    return true;
}

// ---------------- Matcher ----------------

namespace {

const DriverRule *findDriverRule(const KnowledgeBase &kb, const std::string &name)
{
    const std::string n = lower(name);
    const DriverRule *best = nullptr;
    size_t best_score = 0;
    for (const auto &r : kb.driver_rules) {
        for (const auto &e : r.exact) {
            if (n == e && 1000 > best_score) {
                best = &r;
                best_score = 1000;
            }
        }
        for (const auto &p : r.prefix) {
            if (n.compare(0, p.size(), p) == 0 && 500 + p.size() > best_score) {
                best = &r;
                best_score = 500 + p.size();
            }
        }
        for (const auto &c : r.contains) {
            if (n.find(c) != std::string::npos && c.size() > best_score) {
                best = &r;
                best_score = c.size();
            }
        }
    }
    return best;
}

const UnknownItem *findUnknown(const KnowledgeBase &kb, const char *what, const std::string &value)
{
    const std::string v = lower(value);
    for (const auto &u : kb.unknowns) {
        if (u.what == what && glob(u.value, v)) return &u;
    }
    return nullptr;
}

const Group *groupById(const KnowledgeBase &kb, const std::string &id)
{
    for (const auto &g : kb.groups) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

void addGroup(Analysis &a, const std::string &group, Level level, const std::string &via, const std::string &ev)
{
    if (group.empty()) return;
    for (auto &g : a.groups) {
        if (g.group == group) {
            if ((int)level > (int)g.level) {
                g.level = level;
                g.via = via;
                g.evidence = ev;
            }
            return;
        }
    }
    a.groups.push_back({group, level, via, ev});
}

void addUnknown(Analysis &a, const KnowledgeBase &kb, const char *what, const std::string &value,
                const std::string &fallback, const std::string &display = std::string())
{
    const UnknownItem *u = findUnknown(kb, what, value);
    std::string line = std::string(what) + " " + (display.empty() ? value : display) + ": UNKNOWN - " +
                       (u ? u->reason : fallback);
    if (std::find(a.unknowns.begin(), a.unknowns.end(), line) == a.unknowns.end()) a.unknowns.push_back(line);
}

double overlap(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    std::vector<std::string> x, y;
    for (auto s : a) x.push_back(lower(s));
    for (auto s : b) y.push_back(lower(s));
    std::sort(x.begin(), x.end());
    x.erase(std::unique(x.begin(), x.end()), x.end());
    std::sort(y.begin(), y.end());
    y.erase(std::unique(y.begin(), y.end()), y.end());
    if (x.empty() && y.empty()) return 0;
    std::vector<std::string> inter;
    std::set_intersection(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(inter));
    const size_t uni = x.size() + y.size() - inter.size();
    return uni ? (double)inter.size() / (double)uni : 0;
}

}  // namespace

Analysis analyze(const PanicFacts &f, const KnowledgeBase &kb, const std::vector<CaseRecord> &cases)
{
    Analysis a;

    // 1) Loại panic -> rule cơ chế + nhóm.
    for (size_t i = 0; i < f.kinds.size(); ++i) {
        const std::string kn = panicKindName(f.kinds[i]);
        KindHit kh;
        kh.kind = kn;
        for (const auto &r : kb.kind_rules) {
            if (r.kind == kn) kh.rule = &r;
        }
        a.kinds.push_back(kh);
        const std::string ev = i < f.kind_evidence.size() ? f.kind_evidence[i].excerpt : std::string();
        if (kh.rule && !kh.rule->group.empty()) {
            addGroup(a, kh.rule->group, kh.rule->mechanism.level, "kind:" + kn, ev);
        } else {
            for (const auto &g : kb.groups) {
                if (std::find(g.kinds.begin(), g.kinds.end(), kn) != g.kinds.end())
                    addGroup(a, g.id, Level::Inferred, "kind:" + kn, ev);
            }
            if (!kh.rule) addUnknown(a, kb, "kind", kn, "chưa có rule cho loại panic này");
        }
    }

    // 2) Driver / service.
    for (const auto &d : f.drivers) {
        if (d.role == "watchdog_daemon") continue;  // daemon là chủ thể timeout, không phải phần cứng
        DriverHit h;
        h.name = d.name;
        h.role = d.role;
        h.evidence = d.evidence.excerpt;
        h.rule = findDriverRule(kb, d.name);
        if (h.rule) {
            h.group = h.rule->group;
            addGroup(a, h.group, h.rule->subsystem.level, "driver:" + d.name, h.evidence);
            // Số fact độc lập ủng hộ cùng nhóm (không kết luận linh kiện từ 1 keyword).
            int facts = 1;
            for (const auto &k : a.kinds) {
                if (k.rule && k.rule->group == h.group) facts++;
            }
            for (const auto &o : f.drivers) {
                const DriverRule *orr = (o.name == d.name) ? nullptr : findDriverRule(kb, o.name);
                if (orr && orr->group == h.group) {
                    facts++;
                    break;
                }
            }
            h.components_allowed = facts >= h.rule->requires_facts;
        } else {
            // Không có rule: nhóm chỉ theo TÊN (tag từ vựng) -> INFERRED, chức năng UNKNOWN.
            for (const auto &t : d.tags) {
                for (const auto &g : kb.groups) {
                    if (std::find(g.tags.begin(), g.tags.end(), t) != g.tags.end()) {
                        h.group = g.id;
                        addGroup(a, g.id, Level::Inferred, "tag:" + t + "(" + d.name + ")", h.evidence);
                    }
                }
            }
            addUnknown(a, kb, "driver", d.name, "chưa có tài liệu / case xác nhận chức năng của driver này");
        }
        a.drivers.push_back(h);
    }

    // 3) Sensor.
    for (const auto &s : f.sensors) {
        SensorHit h;
        h.name = s.name;
        h.context = s.context;
        h.evidence = s.evidence.excerpt;
        for (const auto &r : kb.sensor_rules) {
            if (r.name == lower(s.name) && modelMatches(r.models, f.product)) h.rule = &r;
        }
        if (h.rule) {
            const Group *g = groupById(kb, "SMC_SENSOR");
            if (g) addGroup(a, g->id, Level::Inferred, "sensor:" + s.name, h.evidence);
        } else {
            addUnknown(a, kb, "sensor", s.name,
                       f.product.empty() ? "không có model để tra" : "chưa có mapping cho model " + f.product);
        }
        if (s.context == "missing_sensor") {
            addGroup(a, "SMC_SENSOR", Level::Inferred, "missing_sensor:" + s.name, h.evidence);
        }
        a.sensors.push_back(h);
    }

    // 4) Mã (chỉ mã trong thông điệp; panicFlags giữ trong facts).
    for (const auto &c : f.codes) {
        if (!c.in_message) continue;
        CodeHit h;
        h.label = c.label;
        h.hex = c.hex;
        h.bits = c.bits;
        h.evidence = c.evidence.excerpt;
        const std::string l = lower(c.label);
        for (const auto &r : kb.code_rules) {
            if (glob(r.label, l) && modelMatches(r.models, f.product)) h.rule = &r;
        }
        if (!h.rule) {
            std::string bits;
            for (int b : c.bits) bits += (bits.empty() ? "" : ",") + std::to_string(b);
            addUnknown(a, kb, "code", c.label, "chưa có bảng ý nghĩa mã/bit đã kiểm chứng",
                       c.label + "=" + c.raw + " (" + c.hex + ", bit " + (bits.empty() ? "-" : bits) + ")");
        }
        a.codes.push_back(h);
    }

    // 5) Các fact khác có trong danh sách UNKNOWN đã biết.
    if (!f.assert_text.empty()) addUnknown(a, kb, "assert_text", f.assert_text, "chưa có tài liệu ý nghĩa ASSERT này");
    {
        std::vector<std::string> seen;
        for (const auto &m : f.mailbox) {
            if (m.ascii4.empty() || std::find(seen.begin(), seen.end(), m.ascii4) != seen.end()) continue;
            seen.push_back(m.ascii4);
            addUnknown(a, kb, "mailbox_ascii", m.ascii4, "ý nghĩa khoá chưa có tài liệu");
        }
    }
    if (!f.kernel_target.empty()) addUnknown(a, kb, "kernel_target", f.kernel_target, "chưa đối chiếu sang model máy");
    if (f.product.empty()) a.unknowns.push_back("model: UNKNOWN - log không có product; không khớp case theo model");

    // 6) VERIFIED / REPAIR_CONFIRMED case.
    // Dữ kiện phân biệt: chỉ những gì nói lên LỖI CỤ THỂ (không tính model, loại panic, tên chung
    // như "SMC" được nhắc trong thông điệp, hay target). Cùng model + cùng loại mà không có dữ kiện
    // phân biệt thì không bao giờ là exact.
    std::vector<std::string> mine;
    for (const auto &d : f.drivers) {
        if (d.role == "busy_entry") mine.push_back(d.name);
    }
    for (const auto &s : f.sensors) mine.push_back(s.name);
    for (const auto &c : f.codes) {
        if (c.in_message && c.value != 0) mine.push_back(c.label + "=" + c.hex);
    }
    for (const auto &t : f.assert_tags) mine.push_back("tag:" + t);
    const bool is_smc = std::find(f.kinds.begin(), f.kinds.end(), PanicKind::SmcPanic) != f.kinds.end();
    bool discriminating = !mine.empty() || !f.assert_text.empty();
    if (is_smc) {
        // SMC: phải có sensor index / HEX / ASSERT tag (model + target + ASSERT text chưa đủ).
        bool smc_key = !f.assert_tags.empty();
        for (const auto &s : f.sensors) smc_key = smc_key || s.context == "sensor_array_nonzero" || s.context == "indexed_sensor";
        for (const auto &c : f.codes) smc_key = smc_key || (c.in_message && c.value != 0);
        discriminating = smc_key;
    }
    std::string hay = lower(f.message);
    for (const auto &c : f.codes) hay += " " + lower(c.raw) + " " + lower(c.hex);
    for (const auto &s : f.sensors) hay += " " + lower(s.name);

    for (const auto &c : cases) {
        if (c.status == "DRAFT") continue;
        const bool verified = c.status == "VERIFIED";
        CaseMatch m;
        m.rec = &c;
        if (!c.signature.empty()) {
            if ((c.signature == f.signature || c.signature_generic == f.signature_generic) && !discriminating) {
                // Signature trùng nhưng chỉ có model / loại panic -> không đủ để coi là cùng lỗi.
                m.match_type = "signature_no_discriminator";
                m.confidence = Confidence::Low;
                m.reason = std::string("chỉ trùng model / loại panic, không có dữ kiện phân biệt") +
                           (is_smc ? " (SMC cần sensor index / HEX / ASSERT tag)" : "");
            } else if (!f.product.empty() && c.model == f.product && c.signature == f.signature) {
                m.match_type = "exact";
                m.confidence = verified ? Confidence::High : Confidence::Medium;
                m.reason = "cùng model " + c.model + " + cùng normalized signature";
                if (is_smc) {
                    std::string key = c.model + (f.smc_target.empty() ? "" : "+" + f.smc_target);
                    for (const auto &s : f.sensors) key += "+" + s.name;
                    for (const auto &k : f.codes) {
                        if (k.in_message && k.value != 0) key += "+" + k.hex;
                    }
                    for (const auto &t : f.assert_tags) key += "+" + t;
                    m.reason += " (SMC: " + key + ")";
                }
            } else if (c.signature_generic == f.signature_generic) {
                m.match_type = "same_signature_other_model";
                m.confidence = verified ? Confidence::Medium : Confidence::Low;
                m.reason = "cùng signature nhưng model khác (" + (c.model.empty() ? std::string("?") : c.model) +
                           " vs " + (f.product.empty() ? std::string("UNKNOWN") : f.product) + "): hạ confidence";
            } else if (!f.product.empty() && c.model == f.product && c.primary_kind == panicKindName(f.primary_kind) &&
                       !mine.empty() && (!is_smc || c.smc_target == f.smc_target)) {
                std::vector<std::string> theirs = c.drivers;
                theirs.insert(theirs.end(), c.sensors.begin(), c.sensors.end());
                theirs.insert(theirs.end(), c.codes.begin(), c.codes.end());
                for (const auto &t : c.assert_tags) theirs.push_back("tag:" + t);
                const double o = overlap(mine, theirs);
                if (o >= 0.5) {
                    char b[64];
                    snprintf(b, sizeof(b), "%.0f%%", o * 100);
                    m.match_type = "partial_same_model";
                    m.confidence = Confidence::Low;
                    m.reason = "cùng model + loại panic, trùng " + std::string(b) + " driver/sensor/mã";
                }
            }
            if (m.match_type.empty() && is_smc && !f.assert_tags.empty() && !c.assert_tags.empty()) {
                // SMC khác board / HEX nhưng trùng tag ASSERT: chỉ để tham khảo (LOW), không bao giờ
                // dùng làm kết luận phần cứng - ý nghĩa tag chưa có tài liệu.
                std::vector<std::string> common;
                for (const auto &t : f.assert_tags) {
                    if (std::find(c.assert_tags.begin(), c.assert_tags.end(), t) != c.assert_tags.end()) common.push_back(t);
                }
                if (!common.empty()) {
                    std::string tags;
                    for (const auto &t : common) tags += (tags.empty() ? "" : " ") + t;
                    m.match_type = "shared_assert_tag";
                    m.confidence = Confidence::Low;
                    m.reason = "cùng tag ASSERT " + tags + " (case: " + (c.model.empty() ? std::string("?") : c.model) +
                               (c.smc_target.empty() ? std::string() : " board " + c.smc_target) + ") - chỉ tham khảo";
                }
            }
        } else if (!c.legacy_keyword.empty() || !c.legacy_signature.empty()) {
            // Case cũ chưa có file panic gốc: khớp như HCCPanicKB cũ (model + bug_type + keyword).
            const bool model_ok = c.model.empty() || (!f.product.empty() && lower(f.product).find(lower(c.model)) != std::string::npos);
            const bool type_ok = c.panic_type.empty() || lower(f.bug_type).find(lower(c.panic_type)) != std::string::npos;
            const bool kw_ok = !c.legacy_keyword.empty() && containsToken(hay, lower(c.legacy_keyword));
            if (model_ok && type_ok && kw_ok && !c.model.empty()) {
                m.match_type = "legacy_keyword";
                m.confidence = verified ? Confidence::Medium : Confidence::Low;
                m.reason = "case cũ: khớp model + bug_type + keyword '" + c.legacy_keyword + "' (chưa có signature V1)";
            }
        }
        if (!m.match_type.empty()) a.cases.push_back(m);
    }
    std::sort(a.cases.begin(), a.cases.end(), [](const CaseMatch &x, const CaseMatch &y) {
        if (confRank(x.confidence) != confRank(y.confidence)) return confRank(x.confidence) > confRank(y.confidence);
        if (matchRank(x.match_type) != matchRank(y.match_type)) return matchRank(x.match_type) > matchRank(y.match_type);
        return x.rec->status == "VERIFIED" && y.rec->status != "VERIFIED";
    });

    // 7) Confidence tổng.
    if (!a.cases.empty()) {
        a.confidence = a.cases[0].confidence;
        a.confidence_reason = a.cases[0].rec->status + " case " + a.cases[0].rec->id + ": " + a.cases[0].reason;
    } else {
        bool hw = false;
        for (const auto &d : a.drivers) hw = hw || (d.rule && d.rule->hardware_area.level != Level::Unknown);
        // Rule nhóm WATCHDOG chỉ nói chung "phần cứng do driver điều khiển" -> không tính.
        for (const auto &k : a.kinds) {
            hw = hw || (k.rule && k.rule->group != "WATCHDOG" && k.rule->hardware_area.level != Level::Unknown);
        }
        for (const auto &s : a.sensors) hw = hw || (s.rule && s.rule->location.level != Level::Unknown);
        if (hw) {
            a.confidence = Confidence::Low;
            a.confidence_reason = "chưa có case sửa thực tế; khu vực phần cứng chỉ ở mức INFERRED/DOCUMENTED từ KB";
        } else {
            a.confidence = Confidence::None;
            a.confidence_reason = "chỉ xác định được cơ chế panic; khu vực phần cứng UNKNOWN, chưa có case khớp";
        }
    }
    return a;
}

std::string describe(const Analysis &a)
{
    std::string s;
    auto field = [&](const char *name, const Field &f) {
        s += std::string("    ") + name + ": " + f.value + " [" + levelName(f.level) + "]";
        if (!f.sources.empty()) {
            s += " src=";
            for (size_t i = 0; i < f.sources.size(); ++i) s += (i ? "," : "") + f.sources[i];
        }
        s += "\n";
    };
    s += "confidence: " + std::string(confidenceName(a.confidence)) + " - " + a.confidence_reason + "\n";
    for (const auto &m : a.cases) {
        s += "case: " + m.rec->id + " " + m.rec->status + " " + m.match_type + " " + confidenceName(m.confidence) +
             " - " + m.reason + "\n";
    }
    for (const auto &g : a.groups) {
        s += "group: " + g.group + " [" + levelName(g.level) + "] via " + g.via + "\n";
    }
    for (const auto &k : a.kinds) {
        s += "kind: " + k.kind + (k.rule ? " rule=" + k.rule->id : std::string(" rule=-")) + "\n";
        if (k.rule) {
            field("mechanism", k.rule->mechanism);
            field("hardware_area", k.rule->hardware_area);
        }
    }
    for (const auto &d : a.drivers) {
        s += "driver: " + d.name + " group=" + (d.group.empty() ? std::string("-") : d.group) +
             (d.rule ? " rule=" + d.rule->id : std::string(" rule=-")) + "\n";
        if (d.rule) {
            field("subsystem", d.rule->subsystem);
            field("hardware_area", d.rule->hardware_area);
            field("bus_rail", d.rule->bus_rail);
            if (d.components_allowed) field("components", d.rule->components);
            else s += "    components: (chưa đủ fact độc lập để nêu)\n";
        }
    }
    for (const auto &h : a.sensors) {
        s += "sensor: " + h.name + (h.rule ? " rule=" + h.rule->id : std::string(" rule=-")) + "\n";
        if (h.rule) {
            field("meaning", h.rule->meaning);
            field("location", h.rule->location);
        }
    }
    for (const auto &c : a.codes) {
        s += "code: " + c.label + "=" + c.hex + (c.rule ? " rule=" + c.rule->id : std::string(" rule=-")) + "\n";
    }
    for (const auto &u : a.unknowns) s += "unknown: " + u + "\n";
    return s;
}

}  // namespace HCCKB
