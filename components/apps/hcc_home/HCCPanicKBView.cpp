#include "HCCPanicKBView.hpp"

#include <algorithm>
#include <cstdio>

namespace HCCKB {

namespace {

std::string fieldText(const Field &f)
{
    return noRecolor(f.value) + " " + levelTag(f.level);
}

const char *confColor(Confidence c)
{
    switch (c) {
        case Confidence::High: return "#3DD66E ";
        case Confidence::Medium: return "#F2C94C ";
        case Confidence::Low: return "#FF9F43 ";
        case Confidence::None: break;
    }
    return "#8A97A3 ";
}

std::string bitsText(const std::vector<int> &bits)
{
    if (bits.empty()) return "-";
    std::string s;
    for (size_t i = 0; i < bits.size() && i < 12; ++i) s += (i ? "," : "") + std::to_string(bits[i]);
    if (bits.size() > 12) s += ",...";
    return s;
}

}  // namespace

std::string noRecolor(const std::string &s)
{
    std::string o = s;
    for (auto &c : o) {
        if (c == '#') c = ' ';
    }
    return o;
}

std::string levelTag(Level l)
{
    switch (l) {
        case Level::Documented: return "#4DA3FF [DOCUMENTED]#";
        case Level::Inferred: return "#F2C94C [INFERRED]#";
        case Level::Verified: return "#3DD66E [VERIFIED]#";
        case Level::Unknown: break;
    }
    return "#8A97A3 [UNKNOWN]#";
}

KbView renderView(const PanicFacts &f, const Analysis &a)
{
    KbView v;
    v.confidence = a.confidence;
    v.top_case = a.cases.empty() ? nullptr : a.cases[0].rec;

    // ---- CHUỖI PHÂN TÍCH ----
    std::string &c = v.chain;
    for (const auto &k : a.kinds) {
        c += "Loại: " + k.kind;
        if (k.rule) {
            c += " " + levelTag(k.rule->mechanism.level) + "\n  " + noRecolor(k.rule->mechanism.value) + "\n";
            c += "  Phần cứng: " + fieldText(k.rule->hardware_area) + "\n";
        } else {
            c += " " + levelTag(Level::Unknown) + "\n";
        }
    }
    if (!f.assert_text.empty() || !f.assert_location.empty()) {
        c += "ASSERT: " + noRecolor(f.assert_text) + " @ " + noRecolor(f.assert_location) + " " +
             levelTag(Level::Unknown) + "\n";
    }
    size_t n = 0;
    for (const auto &d : a.drivers) {
        if (n++ >= 8) {
            c += "- ... (+" + std::to_string(a.drivers.size() - 8) + " driver)\n";
            break;
        }
        c += "- Driver " + noRecolor(d.name);
        if (d.role == "busy_entry") c += " (giữ busy)";
        c += " -> nhóm " + (d.group.empty() ? std::string("?") : d.group);
        if (d.rule) {
            c += "\n    subsystem: " + fieldText(d.rule->subsystem);
            c += "\n    phần cứng: " + fieldText(d.rule->hardware_area);
            c += "\n    bus/rail: " + fieldText(d.rule->bus_rail);
            if (d.components_allowed) c += "\n    linh kiện: " + fieldText(d.rule->components);
            c += "\n";
        } else {
            c += (d.group.empty() ? std::string(" ") : std::string(" ") + levelTag(Level::Inferred) + " (theo tên)") +
                 "\n    chức năng / phần cứng: " + levelTag(Level::Unknown) + "\n";
        }
    }
    for (const auto &s : a.sensors) {
        c += "- Sensor " + noRecolor(s.name);
        if (s.rule) {
            c += ": " + fieldText(s.rule->meaning) + "\n    vị trí: " + fieldText(s.rule->location) + "\n";
        } else {
            c += ": " + levelTag(Level::Unknown) + "\n";
        }
    }
    for (const auto &k : a.codes) {
        // Tìm giá trị gốc (dec) của mã để hiện dec -> hex -> bit.
        std::string raw = k.hex;
        for (const auto &fc : f.codes) {
            if (fc.label == k.label && fc.hex == k.hex) raw = fc.raw;
        }
        c += "- Mã " + noRecolor(k.label) + " = " + noRecolor(raw) + " = " + k.hex + " (bit " + bitsText(k.bits) + ")";
        c += k.rule ? " " + fieldText(k.rule->meaning) + "\n" : " -> " + levelTag(Level::Unknown) + "\n";
    }
    if (c.empty()) c = "Không tách được dữ kiện nào " + levelTag(Level::Unknown);

    // ---- ĐO KIỂM / KIỂM TRA ----
    if (v.top_case && !v.top_case->measurements.empty()) {
        v.checks += "Theo case " + noRecolor(v.top_case->id) + " " + levelTag(Level::Verified) + ":\n";
        for (const auto &m : v.top_case->measurements) {
            v.checks += "- " + noRecolor(m.point) + " (" + noRecolor(m.type) + "): trước " + noRecolor(m.before) +
                        " / sau " + noRecolor(m.after) + " " + noRecolor(m.unit) + "\n";
        }
    }
    for (const auto &k : a.kinds) {
        if (!k.rule) continue;
        for (const auto &ch : k.rule->checks) v.checks += "- " + noRecolor(ch.step) + " " + levelTag(ch.level) + "\n";
    }
    if (v.checks.empty()) v.checks = "Chưa có bước kiểm tra trong KB " + levelTag(Level::Unknown);

    // ---- ĐỐI CHIẾU VERIFIED / KB ----
    v.match = std::string("Confidence: ") + confColor(a.confidence) + confidenceName(a.confidence) + "#\n" +
              noRecolor(a.confidence_reason) + "\n";
    if (a.cases.empty()) {
        v.match += "Case khớp: không có\n";
    } else {
        for (size_t i = 0; i < a.cases.size() && i < 4; ++i) {
            const auto &m = a.cases[i];
            v.match += "- " + noRecolor(m.rec->status) + " " + noRecolor(m.rec->id) + " (" + m.match_type + ", " +
                       confColor(m.confidence) + confidenceName(m.confidence) + "#)\n";
        }
    }
    if (!a.unknowns.empty()) {
        v.match += "UNKNOWN (" + std::to_string(a.unknowns.size()) + "):\n";
        for (size_t i = 0; i < a.unknowns.size() && i < 8; ++i) v.match += "- " + noRecolor(a.unknowns[i]) + "\n";
        if (a.unknowns.size() > 8) v.match += "- ... (+" + std::to_string(a.unknowns.size() - 8) + ")\n";
    }

    // ---- KẾT LUẬN ----
    if (v.top_case) {
        const CaseRecord &r = *v.top_case;
        const Level lv = (r.status == "VERIFIED") ? Level::Verified : Level::Inferred;
        v.conclusion = "Theo case " + noRecolor(r.status) + " " + levelTag(lv) + " (" + a.cases[0].match_type + "):\n";
        v.conclusion += "Khu vực: " + noRecolor(r.hardware_area.empty() ? "-" : r.hardware_area) + "\n";
        if (!r.components.empty()) {
            v.conclusion += "Linh kiện:";
            for (const auto &cp : r.components) {
                v.conclusion += " " + noRecolor(cp.name) + (cp.ref.empty() ? "" : " (" + noRecolor(cp.ref) + ")") +
                                (cp.action.empty() ? "" : " [" + noRecolor(cp.action) + "]") + ";";
            }
            v.conclusion += "\n";
        }
        if (!r.bus_rail.empty()) {
            v.conclusion += "Bus/rail:";
            for (const auto &b : r.bus_rail) v.conclusion += " " + noRecolor(b) + ";";
            v.conclusion += "\n";
        }
        v.conclusion += "Cách sửa: " + noRecolor(r.repair_action.empty() ? "-" : r.repair_action) + "\n";
        v.conclusion += std::string("Kết quả: ") + (r.result.panic_resolved ? "hết panic" : "chưa xác nhận hết panic");
        if (r.result.observed_days >= 0) v.conclusion += " (theo dõi " + std::to_string(r.result.observed_days) + " ngày)";
        v.conclusion += "\n";
        if (a.cases[0].match_type != "exact") {
            v.conclusion += "#FF9F43 Lưu ý: không phải khớp chính xác cùng model + signature - cần đo kiểm xác nhận.#\n";
        }
    } else {
        v.conclusion = "Nhóm:";
        for (const auto &g : a.groups) v.conclusion += " " + g.group + " " + levelTag(g.level) + ";";
        if (a.groups.empty()) v.conclusion += " " + levelTag(Level::Unknown);
        v.conclusion += "\nLinh kiện cụ thể: " + levelTag(Level::Unknown) +
                        " - chưa có VERIFIED case / bằng chứng. Đo kiểm theo ô bên cạnh rồi lưu case bằng "
                        "XÁC NHẬN ĐÃ SỬA.";
    }
    return v;
}

namespace {

std::string firstLine(const std::string &s, size_t cap)
{
    std::string l = s.substr(0, s.find('\n'));
    if (l.size() > cap) l = l.substr(0, cap) + "...";
    return l;
}

void addUnique(std::vector<std::string> &v, const std::string &x)
{
    if (!x.empty() && std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

std::string joinList(const std::vector<std::string> &v, size_t cap, const char *sep = ", ")
{
    std::string o;
    for (size_t i = 0; i < v.size() && i < cap; ++i) o += (i ? sep : "") + v[i];
    if (v.size() > cap) o += sep + std::string("(+") + std::to_string(v.size() - cap) + ")";
    return o;
}

const char *matchText(const std::string &t)
{
    if (t == "exact") return "khớp chính xác (model + signature)";
    if (t == "same_signature_other_model") return "cùng lỗi, khác model - cần xác nhận";
    if (t == "partial_same_model") return "giống một phần - cần xác nhận";
    if (t == "legacy_keyword") return "case cũ (theo keyword) - cần xác nhận";
    return "chỉ trùng model / loại panic - không dùng làm kết luận";
}

}  // namespace

BasicView renderBasic(const PanicFacts &f, const Analysis &a)
{
    BasicView b;
    b.confidence = a.confidence;
    switch (a.confidence) {
        case Confidence::High: b.confidence_text = "CAO"; break;
        case Confidence::Medium: b.confidence_text = "TRUNG BÌNH"; break;
        case Confidence::Low: b.confidence_text = "THẤP"; break;
        case Confidence::None: b.confidence_text = "CHƯA ĐỦ DỮ KIỆN"; break;
    }

    std::vector<std::string> busy, missing;
    long busy_secs = -1, wd_secs = -1;
    std::string daemon;
    for (const auto &d : f.drivers) {
        if (d.role == "busy_entry") busy.push_back(d.name);
        if (d.role == "watchdog_daemon" && daemon.empty()) daemon = d.name;
    }
    for (const auto &s : f.sensors) {
        if (s.context == "missing_sensor") missing.push_back(s.name);
    }
    for (const auto &t : f.timeouts) {
        if (t.what == "busy_timeout" || t.what == "iokit_daemon_stall") busy_secs = t.seconds;
        else wd_secs = t.seconds;
    }

    // ---- LỖI ----
    switch (f.primary_kind) {
        case PanicKind::SmcPanic:
            b.error = "SMC PANIC" + (f.assert_text.empty() ? std::string() : " - ASSERT: " + f.assert_text);
            break;
        case PanicKind::BusyTimeout:
        case PanicKind::IOKitDaemonStall:
            b.error = "Busy timeout" + (busy_secs >= 0 ? " " + std::to_string(busy_secs) + "s" : std::string()) +
                      " - " + std::to_string(busy.size()) + " driver giữ busy";
            break;
        case PanicKind::UserspaceWatchdog:
            b.error = "Watchdog: " + (daemon.empty() ? std::string("dịch vụ hệ thống") : daemon) + " không phản hồi" +
                      (wd_secs >= 0 ? " " + std::to_string(wd_secs) + "s" : std::string()) +
                      (missing.empty() ? std::string() : " - thiếu sensor: " + joinList(missing, 6, " "));
            break;
        case PanicKind::WatchdogTimeout:
            b.error = "Watchdog timeout" + (wd_secs >= 0 ? " " + std::to_string(wd_secs) + "s" : std::string());
            break;
        case PanicKind::KernelException:
            b.error = "Kernel exception: " + firstLine(f.message, 60) +
                      (f.esr_ec_name.empty() ? std::string() : " (" + f.esr_ec_name + ")");
            break;
        default:
            b.error = f.message.empty() ? std::string("Không đọc được thông điệp panic") : firstLine(f.message, 120);
            break;
    }

    // ---- SIGNATURE / HEX ----
    {
        std::vector<std::string> lines;
        std::vector<std::string> hex;
        for (const auto &c : f.codes) {
            if (c.in_message && c.value != 0) hex.push_back(c.label + " = " + c.hex);
        }
        if (!hex.empty()) lines.push_back("HEX: " + joinList(hex, 4));
        if (!f.smc_target.empty()) lines.push_back("Target: " + f.smc_target);
        if (!f.assert_tags.empty()) lines.push_back("Tag: " + joinList(f.assert_tags, 8, " "));
        if (!busy.empty()) lines.push_back("Driver: " + joinList(busy, 4));
        if (!missing.empty()) lines.push_back("Sensor thiếu: " + joinList(missing, 6, " "));
        // Màn chính: thay "UNKNOWN" (thiếu model / bug_type) bằng "?"; signature gốc có trong chi tiết.
        std::string sig = f.signature;
        for (size_t p = sig.find("UNKNOWN"); p != std::string::npos; p = sig.find("UNKNOWN")) sig.replace(p, 7, "?");
        lines.push_back("Signature: " + sig);
        b.signature = joinList(lines, 10, "\n");
    }

    // ---- PHẦN CỨNG NGHI NGỜ ----
    const CaseMatch *top = a.cases.empty() ? nullptr : &a.cases[0];
    if (top && top->confidence >= Confidence::Medium) {
        const CaseRecord &r = *top->rec;
        std::vector<std::string> comps;
        for (const auto &c : r.components) addUnique(comps, c.name + (c.ref.empty() ? "" : " (" + c.ref + ")"));
        b.hardware = r.hardware_area.empty() ? std::string("(case chưa ghi khu vực)") : r.hardware_area;
        if (!comps.empty()) b.hardware += "\nLinh kiện: " + joinList(comps, 3);
        b.hardware_confirmed = r.status == "VERIFIED" && top->match_type == "exact";
        b.hardware_known = true;
        if (!b.hardware_confirmed) b.hardware += "\n(theo case " + r.status + " - " + matchText(top->match_type) + ")";
    } else {
        std::vector<std::string> areas;
        for (const auto &d : a.drivers) {
            if (d.rule && d.rule->hardware_area.level != Level::Unknown) addUnique(areas, d.rule->hardware_area.value);
        }
        for (const auto &s : a.sensors) {
            if (s.rule && s.rule->location.level != Level::Unknown)
                addUnique(areas, "Sensor " + s.name + ": " + s.rule->location.value);
        }
        for (const auto &k : a.kinds) {
            if (k.rule && k.rule->group != "WATCHDOG" && k.rule->hardware_area.level != Level::Unknown)
                addUnique(areas, k.rule->hardware_area.value);
        }
        std::vector<std::string> unruled;
        for (const auto &d : a.drivers) {
            if (!d.rule && d.role == "busy_entry") unruled.push_back(d.name);
        }
        if (!unruled.empty()) addUnique(areas, "Khối do driver " + joinList(unruled, 4) + " điều khiển");
        if (f.primary_kind == PanicKind::SmcPanic) {
            std::vector<std::string> idx;
            for (const auto &s : f.sensors) {
                if (s.context != "missing_sensor") idx.push_back(s.name);
            }
            if (!idx.empty()) addUnique(areas, "SMC: " + joinList(idx, 4));
        }
        if (!missing.empty()) {
            bool have = false;
            for (const auto &s : a.sensors) have = have || (s.rule && s.rule->location.level != Level::Unknown);
            if (!have) addUnique(areas, "Sensor bị thiếu: " + joinList(missing, 6, " "));
        }
        if (areas.empty()) {
            b.hardware = "Chưa xác định - đo kiểm theo các bước bên dưới";
        } else {
            b.hardware = joinList(areas, 3, "\n") + "\n(chưa xác nhận - cần đo kiểm)";
            b.hardware_known = true;
        }
    }

    // ---- CÁC BƯỚC KIỂM TRA ----
    {
        std::vector<std::string> steps;
        if (top && top->confidence >= Confidence::Medium) {
            for (const auto &m : top->rec->measurements) {
                steps.push_back("Đo " + m.point + ": trước " + m.before + " -> sau " + m.after + " " + m.unit +
                                " (theo case)");
            }
            if (!top->rec->repair_action.empty()) steps.push_back("Cách sửa đã xác nhận: " + top->rec->repair_action);
        }
        for (const auto &k : a.kinds) {
            if (!k.rule) continue;
            for (const auto &c : k.rule->checks) addUnique(steps, c.step);
        }
        if (steps.empty()) steps.push_back("Chưa có bước kiểm tra trong KB - đo kiểm khu vực nghi ngờ rồi lưu case");
        for (size_t i = 0; i < steps.size() && i < 6; ++i) {
            b.steps += (i ? "\n" : "") + std::to_string(i + 1) + ". " + steps[i];
        }
    }

    // ---- VERIFIED / REPAIR CASE ----
    if (top) {
        b.case_match = top->rec->status + " " + top->rec->id + "\n" + matchText(top->match_type);
        if (a.cases.size() > 1) b.case_match += "\n(+" + std::to_string(a.cases.size() - 1) + " case khác - xem chi tiết)";
    } else {
        b.case_match = "Chưa có VERIFIED / REPAIR case khớp";
    }
    for (std::string *t : {&b.error, &b.signature, &b.hardware, &b.steps, &b.case_match}) *t = noRecolor(*t);
    return b;
}

std::string renderDetail(const PanicFacts &f, const Analysis &a)
{
    const KbView v = renderView(f, a);
    std::string d;
    d += "#00D9FF == KẾT LUẬN ==#\n" + v.conclusion + "\n\n";
    d += "#00D9FF == CHUỖI PHÂN TÍCH (kèm mức bằng chứng) ==#\n" + v.chain + "\n\n";
    d += "#00D9FF == KIỂM TRA ==#\n" + v.checks + "\n\n";
    d += "#00D9FF == ĐỐI CHIẾU KB / CASE / UNKNOWN ==#\n" + v.match + "\n\n";
    if (a.cases.size() > 0) {
        d += "#00D9FF == CASE KHỚP ==#\n";
        for (const auto &m : a.cases) {
            d += "- " + noRecolor(m.rec->status + " " + m.rec->id + " (" + m.match_type + ", " +
                                  confidenceName(m.confidence) + "): " + m.reason) + "\n";
        }
        d += "\n";
    }
    d += "#00D9FF == DỮ KIỆN PARSER (facts, mailbox, RTKit, bitmask...) ==#\n" + noRecolor(HCCPanicFacts::describe(f));
    return d;
}

}  // namespace HCCKB
