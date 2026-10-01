#include "HCCPanicKBView.hpp"

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

}  // namespace HCCKB
