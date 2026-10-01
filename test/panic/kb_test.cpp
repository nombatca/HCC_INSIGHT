// Test host cho Knowledge Base V1 (HCCPanicKBv1): validator, matcher, case, migrate.
//   kb_test <root test/panic> [--dump <file.ips>]
#include "HCCPanicKBv1.hpp"
#include "HCCPanicKBView.hpp"
#include "HCCPanicParser.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace HCCKB;

static int g_fail = 0, g_checks = 0;

static void check(bool ok, const std::string &what)
{
    g_checks++;
    if (!ok) {
        g_fail++;
        printf("  FAIL %s\n", what.c_str());
    }
}

static std::string readAll(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool has(const std::string &text, const std::string &line)
{
    return ("\n" + text).find("\n" + line) != std::string::npos;
}

static PanicFacts factsOf(const std::string &path)
{
    return HCCPanicParser::parse(readAll(path)).facts;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <test/panic dir> [--dump file.ips]\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1];
    const std::string kbPath = root + "/../../components/apps/hcc_home/kb/hcc_kb_default.json";

    KnowledgeBase kb;
    check(loadKnowledgeBase(readAll(kbPath), "firmware", kb), "nạp KB mặc định");
    for (const auto &e : kb.errors) printf("  KB error: %s\n", e.c_str());
    check(kb.errors.empty(), "KB mặc định không có lỗi validate");

    if (argc == 4 && std::string(argv[2]) == "--dump") {
        PanicFacts f = factsOf(argv[3]);
        fputs(describe(analyze(f, kb, {})).c_str(), stdout);
        return 0;
    }

    // ---- 1. KB mặc định: đủ 9 nhóm, mọi DOCUMENTED có nguồn tin cậy (validator đã chặn) ----
    printf("[1] KB mặc định\n");
    const char *groups[] = {"SMC_SENSOR", "USB_PHY", "BASEBAND", "ANS_NAND", "POWER_PMU",
                            "WATCHDOG",   "AOP",     "SEP",      "KERNEL_SOFTWARE"};
    for (const char *g : groups) {
        bool found = false;
        for (const auto &x : kb.groups) found = found || x.id == g;
        check(found, std::string("có nhóm ") + g);
    }
    for (const auto &r : kb.driver_rules) {
        check(r.components.level == Level::Unknown, "V1 không nêu linh kiện cụ thể: " + r.id);
        check(r.bus_rail.level != Level::Documented || !r.bus_rail.sources.empty(), "bus_rail có nguồn: " + r.id);
    }
    for (const auto &r : kb.sensor_rules) {
        check(r.location.level != Level::Documented, "vị trí sensor không DOCUMENTED khi chỉ có nguồn cộng đồng: " + r.id);
    }
    printf("    %u nguồn, %u kind rule, %u driver rule, %u sensor rule, %u code rule, %u unknown\n",
           (unsigned)kb.sources.size(), (unsigned)kb.kind_rules.size(), (unsigned)kb.driver_rules.size(),
           (unsigned)kb.sensor_rules.size(), (unsigned)kb.code_rules.size(), (unsigned)kb.unknowns.size());

    // ---- 2. Validator chặn mapping sai mức bằng chứng ----
    printf("[2] Validator\n");
    {
        KnowledgeBase bad;
        loadKnowledgeBase(readAll(root + "/kb/bad_kb.json"), "bad", bad);
        auto rejected = [&](const char *id) {
            for (const auto &r : bad.kind_rules) {
                if (r.id == id) return false;
            }
            for (const auto &r : bad.driver_rules) {
                if (r.id == id) return false;
            }
            return true;
        };
        check(rejected("bad.documented_community"), "DOCUMENTED với nguồn cộng đồng bị từ chối");
        check(rejected("bad.documented_nosource"), "DOCUMENTED không nguồn bị từ chối");
        check(rejected("bad.inferred_noreason"), "INFERRED không lý do bị từ chối");
        check(rejected("bad.verified_in_rule"), "VERIFIED trong rule bị từ chối");
        check(rejected("bad.missing_source"), "nguồn không tồn tại bị từ chối");
        check(rejected("bad.one_keyword"), "requires_facts < 2 bị từ chối");
        check(!rejected("good.documented"), "DOCUMENTED với apple_doc được nhận");
        for (const auto &e : bad.errors) printf("    (đúng) %s\n", e.c_str());
    }

    // ---- 3. Hai panic THẬT: phân tích, không có case nào, không gán linh kiện ----
    printf("[3] Panic thật\n");
    {
        PanicFacts f = factsOf(root + "/samples/real/busy_timeout_smc_charger_iphone13_4.ips");
        Analysis a = analyze(f, kb, {});
        std::string d = describe(a);
        printf("---- iPhone13,4 busy timeout ----\n%s", d.c_str());
        check(a.cases.empty(), "13,4: không có case khớp (chưa có ca sửa)");
        check(has(d, "group: WATCHDOG [DOCUMENTED] via kind:busy_timeout"), "13,4: WATCHDOG DOCUMENTED từ busy timeout");
        check(has(d, "group: SMC_SENSOR [INFERRED] via tag:smc(smc-charger)"), "13,4: SMC_SENSOR chỉ INFERRED theo tên");
        check(has(d, "driver: smc-charger group=SMC_SENSOR rule=-"), "13,4: smc-charger không có rule");
        check(has(d, "driver: portmicro group=- rule=-"), "13,4: portmicro không có rule");
        check(has(d, "unknown: driver smc-charger: UNKNOWN - tên driver thấy trong panic thật"), "13,4: smc-charger UNKNOWN có lý do");
        check(has(d, "unknown: driver lapOverUsbHid: UNKNOWN"), "13,4: lapOverUsbHid UNKNOWN");
        check(has(d, "unknown: kernel_target T8101: UNKNOWN"), "13,4: T8101 UNKNOWN");
        check(a.confidence == Confidence::None, "13,4: confidence NONE (không có ca sửa, không có mapping phần cứng)");
        check(d.find("components: ") == std::string::npos || d.find("components: (chưa đủ") != std::string::npos,
              "13,4: không nêu linh kiện");
    }
    {
        PanicFacts f = factsOf(root + "/samples/real/smc_bsc_failure_fragment.ips");
        Analysis a = analyze(f, kb, {});
        std::string d = describe(a);
        printf("---- SMC BSC fragment ----\n%s", d.c_str());
        check(a.cases.empty(), "SMC: không có case");
        check(has(d, "group: SMC_SENSOR [INFERRED] via kind:smc"), "SMC: nhóm SMC_SENSOR INFERRED");
        check(has(d, "unknown: code S.sensor[1]=1572864 (0x180000, bit 19,20): UNKNOWN - ý nghĩa phần tử S.sensor array"),
              "SMC: S.sensor[1] bit 19,20 UNKNOWN");
        check(has(d, "unknown: assert_text SMC BSC failure: UNKNOWN - chưa có tài liệu nghĩa 'BSC'"), "SMC: BSC UNKNOWN");
        check(has(d, "unknown: mailbox_ascii TG0B: UNKNOWN"), "SMC: TG0B UNKNOWN");
        check(has(d, "unknown: mailbox_ascii B0S5: UNKNOWN"), "SMC: B0S5 UNKNOWN");
        check(has(d, "unknown: model: UNKNOWN"), "SMC: model UNKNOWN");
        check(a.confidence == Confidence::Low, "SMC: confidence LOW (chỉ INFERRED, không có case)");
    }

    // ---- 4. Case: exact / khác model / partial / REPAIR_CONFIRMED / DRAFT ----
    printf("[4] Case matcher\n");
    PanicFacts usb = factsOf(root + "/samples/busy_timeout_usbc.ips");
    PanicFacts usb_var = factsOf(root + "/samples/busy_timeout_usbc_variant.ips");
    CaseRecord verified;
    check(parseCase(readAll(root + "/kb/example_verified_case.json"), verified), "đọc case mẫu");
    check(verified.status == "VERIFIED" && verified.warnings.empty(), "case mẫu hợp lệ VERIFIED");
    check(verified.signature == usb.signature, "signature case mẫu = signature mẫu busy_timeout_usbc");

    CaseRecord other = verified;  // cùng signature, model khác
    other.id = "test_other_model";
    other.model = "iPhone15,5";
    other.signature = "v1|iPhone15,5" + verified.signature.substr(verified.signature.find('|', 3));
    CaseRecord confirmed = verified;  // REPAIR_CONFIRMED cùng model + signature
    confirmed.id = "test_confirmed";
    confirmed.status = "REPAIR_CONFIRMED";
    CaseRecord draft = verified;
    draft.id = "test_draft";
    draft.status = "DRAFT";
    std::vector<CaseRecord> cases = {other, confirmed, draft, verified};

    {
        Analysis a = analyze(usb, kb, cases);
        check(!a.cases.empty() && a.cases[0].rec->id == verified.id && a.cases[0].match_type == "exact" &&
                  a.cases[0].confidence == Confidence::High,
              "VERIFIED cùng model + signature đứng đầu, HIGH");
        bool saw_conf = false, saw_other = false, saw_draft = false;
        for (const auto &m : a.cases) {
            if (m.rec->id == "test_confirmed") saw_conf = m.confidence == Confidence::Medium && m.match_type == "exact";
            if (m.rec->id == "test_other_model")
                saw_other = m.confidence == Confidence::Medium && m.match_type == "same_signature_other_model";
            if (m.rec->id == "test_draft") saw_draft = true;
        }
        check(saw_conf, "REPAIR_CONFIRMED exact -> MEDIUM");
        check(saw_other, "VERIFIED khác model -> hạ còn MEDIUM");
        check(!saw_draft, "DRAFT không dùng để khớp");
        check(a.confidence == Confidence::High, "confidence tổng HIGH khi có VERIFIED exact");
        printf("%s", describe(a).c_str());
    }
    {
        Analysis a = analyze(usb_var, kb, cases);
        check(!a.cases.empty() && a.cases[0].rec->id == verified.id && a.cases[0].match_type == "exact",
              "biến thể cùng lỗi (khác cpu/caller/giây/thứ tự) vẫn khớp exact");
    }
    {
        // Chỉ còn case model khác: confidence tổng bị hạ.
        Analysis a = analyze(usb, kb, {other});
        check(a.confidence == Confidence::Medium, "chỉ có case khác model -> MEDIUM");
    }
    {
        // Partial: cùng model + loại, signature khác (thiếu 1 driver).
        CaseRecord part = verified;
        part.id = "test_partial";
        part.signature = "v1|iPhone15,4|210|busy_timeout|k=busy_timeout,usb_phy|d=atc0-dpphy,usb-drd0|s=-|c=-|w=-|x=-|a=-";
        part.signature_generic = "x";
        part.drivers = {"atc0-dpphy", "usb-drd0"};
        Analysis a = analyze(usb, kb, {part});
        check(!a.cases.empty() && a.cases[0].match_type == "partial_same_model" && a.cases[0].confidence == Confidence::Low,
              "partial cùng model -> LOW");
    }
    {
        // Không có product (fragment): không bao giờ khớp exact.
        PanicFacts frag = factsOf(root + "/samples/real/smc_bsc_failure_fragment.ips");
        CaseRecord c = caseFromFacts(frag);
        c.status = "VERIFIED";
        c.model = "";
        Analysis a = analyze(frag, kb, {c});
        bool exact = false;
        for (const auto &m : a.cases) exact = exact || m.match_type == "exact";
        check(!exact, "panic không có model không khớp exact");
    }

    // ---- 5. Validate case: VERIFIED thiếu kết quả bị hạ, không mất dữ liệu ----
    printf("[5] Case schema\n");
    {
        CaseRecord c = verified;
        c.result.panic_resolved = false;
        CaseRecord back;
        check(parseCase(caseToJson(c), back), "round-trip JSON");
        check(back.status == "REPAIR_CONFIRMED" && !back.warnings.empty(), "VERIFIED chưa hết lỗi -> REPAIR_CONFIRMED + cảnh báo");
        check(back.measurements.size() == 2 && back.measurements[1].after == "0.45", "giữ measurements before/after");
        check(back.components.size() == 1 && back.components[0].action == "replaced", "giữ components");
        CaseRecord rt;
        parseCase(caseToJson(verified), rt);
        check(caseToJson(rt) == caseToJson(verified), "round-trip giữ nguyên toàn bộ trường");
    }

    // ---- 6. Migrate case cũ (HCCPanicKB) ----
    printf("[6] Migrate case cũ\n");
    {
        const std::string legacy = readAll(root + "/kb/legacy_repair_confirmed_smc.json");
        PanicFacts smc = factsOf(root + "/samples/smc_panic_assert.ips");
        CaseRecord noraw, withraw;
        check(migrateLegacyCase(legacy, "kb/repair_confirmed", "", noraw), "migrate không có file panic gốc");
        check(noraw.status == "REPAIR_CONFIRMED" && noraw.legacy_keyword == "0x41" && noraw.signature.empty(),
              "giữ status + keyword cũ, chưa có signature");
        check(noraw.hardware_area.find("TEST") == 0 && noraw.repair_action.find("TEST") == 0 &&
                  noraw.notes.find("CAUSE (cũ): S.sensor[3] / 0x41") != std::string::npos,
              "không mất dữ liệu cũ (hardware, repair, cause)");
        Analysis a1 = analyze(smc, kb, {noraw});
        check(!a1.cases.empty() && a1.cases[0].match_type == "legacy_keyword" && a1.cases[0].confidence == Confidence::Low,
              "case cũ không có panic gốc -> legacy_keyword, LOW");

        check(migrateLegacyCase(legacy, "kb/repair_confirmed", readAll(root + "/samples/smc_panic_assert.ips"), withraw),
              "migrate có file panic gốc");
        check(withraw.signature == smc.signature, "tính lại signature V1 từ panic gốc");
        Analysis a2 = analyze(smc, kb, {withraw});
        check(!a2.cases.empty() && a2.cases[0].match_type == "exact" && a2.cases[0].confidence == Confidence::Medium,
              "case cũ có panic gốc -> exact, MEDIUM (REPAIR_CONFIRMED)");

        CaseRecord v;
        migrateLegacyCase(readAll(root + "/kb/legacy_verified_no_repair.json"), "kb/verified", "", v);
        check(v.status == "REPAIR_CONFIRMED" && !v.warnings.empty(), "VERIFIED cũ thiếu cách sửa -> hạ + cảnh báo");
    }

    // ---- 7. Không panic thật nào bị biến thành VERIFIED ----
    printf("[7] Panic thật không thành VERIFIED\n");
    {
        PanicFacts f = factsOf(root + "/samples/real/busy_timeout_smc_charger_iphone13_4.ips");
        CaseRecord c = caseFromFacts(f);
        check(c.status == "DRAFT", "case tạo từ panic mặc định DRAFT");
        check(c.model == "iPhone13,4" && c.signature == f.signature && c.drivers.size() == 6, "DRAFT điền sẵn từ facts");
    }

    // ---- 8. Chữ hiển thị trên màn PANIC (BƯỚC 3) ----
    printf("[8] KbView\n");
    {
        PanicFacts f = factsOf(root + "/samples/real/busy_timeout_smc_charger_iphone13_4.ips");
        KbView v = renderView(f, analyze(f, kb, {}));
        printf("---- chain ----\n%s\n---- conclusion ----\n%s\n", v.chain.c_str(), v.conclusion.c_str());
        check(v.chain.find("- Driver smc-charger (giữ busy) -> nhóm SMC_SENSOR #F2C94C [INFERRED]# (theo tên)") !=
                  std::string::npos,
              "view 13,4: smc-charger chỉ INFERRED theo tên");
        check(v.chain.find("chức năng / phần cứng: #8A97A3 [UNKNOWN]#") != std::string::npos, "view 13,4: chức năng UNKNOWN");
        check(v.conclusion.find("Linh kiện cụ thể: #8A97A3 [UNKNOWN]#") != std::string::npos, "view 13,4: linh kiện UNKNOWN");
        check(v.top_case == nullptr, "view 13,4: không có case");
    }
    {
        PanicFacts f = factsOf(root + "/samples/real/smc_bsc_failure_fragment.ips");
        KbView v = renderView(f, analyze(f, kb, {}));
        check(v.chain.find("- Mã S.sensor[1] = 1572864 = 0x180000 (bit 19,20) -> #8A97A3 [UNKNOWN]#") != std::string::npos,
              "view SMC: S.sensor[1] dec -> hex -> bit -> UNKNOWN");
        check(v.chain.find("ASSERT: SMC BSC failure @ target/d47/target.cpp:194 #8A97A3 [UNKNOWN]#") != std::string::npos,
              "view SMC: ASSERT UNKNOWN");
    }
    {
        KbView v = renderView(usb, analyze(usb, kb, cases));
        check(v.top_case && v.top_case->id == verified.id, "view USB: case VERIFIED đứng đầu");
        check(v.conclusion.find("Theo case VERIFIED #3DD66E [VERIFIED]# (exact)") == 0, "view USB: kết luận theo VERIFIED");
        check(v.conclusion.find("VÍ DỤ: cụm cáp chân sạc") != std::string::npos, "view USB: linh kiện từ case");
        check(v.checks.find("VÍ DỤ: chân VBUS (voltage): trước 0 / sau 5.0 V") != std::string::npos, "view USB: phép đo trước/sau");
        const std::vector<CaseRecord> only_other = {other};  // Analysis giữ con trỏ vào vector này
        Analysis other_only = analyze(usb, kb, only_other);
        KbView v2 = renderView(usb, other_only);
        check(v2.conclusion.find("Lưu ý: không phải khớp chính xác") != std::string::npos, "view: case khác model có cảnh báo");
    }
    {
        // '#' trong chữ tự do không phá recolor.
        check(noRecolor("a#b") == "a b", "noRecolor bỏ '#'");
    }

    printf("\nKB: %d kiểm tra, %d lỗi\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
