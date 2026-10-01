#pragma once

#include <string>
#include <vector>

#include "HCCPanicFacts.hpp"

// Knowledge Base V1 cho phân tích panic iPhone (BƯỚC 2).
//
// Mỗi mapping mang một mức bằng chứng:
//   DOCUMENTED - có nguồn kỹ thuật đáng tin (mã nguồn / tài liệu Apple, chuẩn kỹ thuật).
//   INFERRED   - suy luận kỹ thuật (phải ghi lý do; nguồn cộng đồng chỉ ở mức này).
//   VERIFIED   - ca sửa thực tế của HCC (CHỈ đến từ case, không bao giờ từ rule).
//   UNKNOWN    - chưa đủ bằng chứng.
// Validator chặn: DOCUMENTED không có nguồn tin cậy, INFERRED không có lý do, VERIFIED trong rule.
//
// Không phụ thuộc ESP-IDF / LVGL / SD: nhận JSON dạng chuỗi (HCCPanicKBStore lo phần SD/flash).

namespace HCCKB {

enum class Level : uint8_t { Unknown, Inferred, Documented, Verified };
const char *levelName(Level l);

enum class Confidence : uint8_t { None, Low, Medium, High };
const char *confidenceName(Confidence c);

// Một giá trị có mức bằng chứng + nguồn.
struct Field {
    std::string value;              // "UNKNOWN" nếu chưa biết
    Level level = Level::Unknown;
    std::vector<std::string> sources; // id trong KB.sources
    std::string reasoning;          // bắt buộc với INFERRED
};

struct Source {
    std::string id;
    std::string type;  // apple_source | apple_doc | standard | community | research | hcc_observation
    std::string title;
    std::string ref;   // URL / đường dẫn
    std::string note;
};

struct Group {
    std::string id;    // SMC_SENSOR, USB_PHY, BASEBAND, ANS_NAND, POWER_PMU, WATCHDOG, AOP, SEP, KERNEL_SOFTWARE
    std::string name;
    std::vector<std::string> kinds; // tên PanicKind (panicKindName)
    std::vector<std::string> tags;  // tag từ vựng của driver (PanicDriverFact.tags)
};

struct Check {
    std::string step;
    Level level = Level::Unknown;
    std::vector<std::string> sources;
    std::string reasoning;
};

// Kiến thức theo loại panic (cơ chế).
struct KindRule {
    std::string id, kind, group;
    Field mechanism;      // panic này nghĩa là gì
    Field hardware_area;  // khu vực phần cứng (thường UNKNOWN nếu chỉ biết cơ chế)
    std::vector<Check> checks;
};

// Kiến thức theo tên driver / service.
struct DriverRule {
    std::string id, group;
    std::vector<std::string> exact, prefix, contains; // so khớp tên (không phân biệt hoa thường)
    Field subsystem, hardware_area, bus_rail, components;
    int requires_facts = 2; // số fact độc lập tối thiểu để nêu components (không kết luận từ 1 keyword)
};

struct SensorRule {
    std::string id, name;              // tên sensor (không phân biệt hoa thường)
    std::vector<std::string> models;   // "iPhone10,3"...; rỗng = mọi model
    Field meaning, location;
};

struct BitMeaning {
    int bit = -1;
    Field meaning;
};

struct CodeRule {
    std::string id, label;             // nhãn mã, vd "S.sensor[1]", "panicFlags"
    std::vector<std::string> models;
    Field meaning;
    std::vector<BitMeaning> bits;
};

// Mục đã biết là CHƯA biết (để báo UNKNOWN rõ ràng, có lý do).
struct UnknownItem {
    std::string id;
    std::string what;   // driver | sensor | code | assert_text | mailbox_ascii | kernel_target | firmware_target
    std::string value;  // so khớp không phân biệt hoa thường; "*" = mọi giá trị
    std::string reason;
    std::string seen_in;
};

struct KnowledgeBase {
    int kb_version = 0;
    std::vector<Source> sources;
    std::vector<Group> groups;
    std::vector<KindRule> kind_rules;
    std::vector<DriverRule> driver_rules;
    std::vector<SensorRule> sensor_rules;
    std::vector<CodeRule> code_rules;
    std::vector<UnknownItem> unknowns;
    std::vector<std::string> errors;   // lỗi validate (rule lỗi bị bỏ, không nạp)
};

// ---- VERIFIED CASE ----
struct Component {
    std::string name;    // vd "cụm cáp sạc", "IC sạc"
    std::string ref;     // ký hiệu trên main (vd U3300) hoặc mã linh kiện
    std::string action;  // replaced | repaired | reworked | cleaned | other
};

struct Measurement {
    std::string point;   // điểm đo / net
    std::string type;    // voltage | diode | resistance | current | other
    std::string before, after, unit, note;
};

struct RepairResult {
    bool panic_resolved = false;
    int observed_days = -1; // số ngày theo dõi sau sửa; -1 = không ghi
    std::string note;
};

struct CaseRecord {
    std::string id;
    std::string status;   // VERIFIED | REPAIR_CONFIRMED | DRAFT
    std::string model, ios, panic_type, primary_kind;
    std::string signature, signature_generic;
    std::vector<std::string> drivers, sensors, codes;
    std::string hardware_area;
    std::vector<Component> components;
    std::vector<std::string> bus_rail;
    std::vector<Measurement> measurements;
    std::string repair_action;
    RepairResult result;
    std::string notes, date, technician;
    std::string evidence_panic_file, evidence_excerpt;
    std::vector<std::string> evidence_attachments;
    // Case cũ (HCCPanicKB trước V1) đã migrate.
    std::string legacy_dir, legacy_file, legacy_keyword, legacy_signature;
    std::vector<std::string> warnings; // validate / migrate
};

// ---- Kết quả đối chiếu ----
struct GroupHit {
    std::string group;
    Level level = Level::Unknown;
    std::string via;      // "kind:busy_timeout", "driver:atc0-dpphy", "sensor:PRS0"
    std::string evidence; // trích log
};

struct KindHit {
    std::string kind;
    const KindRule *rule = nullptr;
};

struct DriverHit {
    std::string name, role, group;
    const DriverRule *rule = nullptr;  // nullptr: chưa có rule -> UNKNOWN
    bool components_allowed = false;   // đủ requires_facts để nêu components
    std::string evidence;
};

struct SensorHit {
    std::string name, context;
    const SensorRule *rule = nullptr;
    std::string evidence;
};

struct CodeHit {
    std::string label, hex;
    std::vector<int> bits;
    const CodeRule *rule = nullptr;
    std::string evidence;
};

struct CaseMatch {
    const CaseRecord *rec = nullptr;
    std::string match_type; // exact | same_signature_other_model | partial_same_model | legacy_keyword
    Confidence confidence = Confidence::None;
    std::string reason;
};

struct Analysis {
    std::vector<CaseMatch> cases;     // đã sắp: case tốt nhất trước
    std::vector<GroupHit> groups;
    std::vector<KindHit> kinds;
    std::vector<DriverHit> drivers;
    std::vector<SensorHit> sensors;
    std::vector<CodeHit> codes;
    std::vector<std::string> unknowns; // các mục UNKNOWN (có lý do)
    Confidence confidence = Confidence::None;
    std::string confidence_reason;
};

// ---- API ----
// Nạp KB từ JSON (mặc định trong firmware, hoặc file người dùng trên SD). Rule không hợp lệ
// bị bỏ và ghi vào kb.errors. Gọi nhiều lần để gộp: KB người dùng thêm/ghi đè theo id.
bool loadKnowledgeBase(const std::string &json, const char *origin, KnowledgeBase &kb);

// Case V1 <-> JSON. parseCase kiểm tra: VERIFIED thiếu kết quả sửa -> hạ REPAIR_CONFIRMED + warning.
bool parseCase(const std::string &json, CaseRecord &out);
std::string caseToJson(const CaseRecord &c);

// Case từ panic đã parse (điền model, iOS, signature, driver, sensor, code); kỹ thuật viên
// điền phần sửa chữa. Mặc định status DRAFT.
CaseRecord caseFromFacts(const PanicFacts &f);

// Migrate case cũ (kb/verified, kb/repair_confirmed của HCCPanicKB). Không xoá gì.
// raw_panic: nội dung file panic gốc nếu còn -> tính signature V1 để khớp chính xác.
bool migrateLegacyCase(const std::string &legacy_json, const char *legacy_dir, const std::string &raw_panic,
                       CaseRecord &out);

Analysis analyze(const PanicFacts &facts, const KnowledgeBase &kb, const std::vector<CaseRecord> &cases);

// Văn bản đọc được (log / test).
std::string describe(const Analysis &a);

}  // namespace HCCKB
