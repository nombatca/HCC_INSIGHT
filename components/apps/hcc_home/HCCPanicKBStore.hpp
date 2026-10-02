#pragma once

#include <string>
#include <vector>

#include "HCCPanicKBv1.hpp"

// Nơi lưu Knowledge Base V1 trên board (BƯỚC 2; chưa nối vào giao diện / luồng ANALYZE).
//
//   firmware : kb/hcc_kb_default.json (nhúng bằng EMBED_TXTFILES) - KB mặc định
//   SD       : kb/v1/user/*.json      - KB người dùng (cùng định dạng; thêm/ghi đè theo id)
//              kb/v1/cases/*.json     - VERIFIED / REPAIR_CONFIRMED / DRAFT case (hcc.case.v1)
//              kb/verified/*.json, kb/repair_confirmed/*.json - case cũ (HCCPanicKB), chỉ đọc
//
// Case cũ được migrate trong bộ nhớ mỗi lần nạp (không xoá, không sửa file cũ). Nếu file panic
// gốc còn trong panic/raw thì tính signature V1 từ đó.
class HCCPanicKBStore {
public:
    struct LoadReport {
        int user_files = 0;
        int cases = 0;
        int legacy_cases = 0;
        std::vector<std::string> messages; // lỗi validate / cảnh báo migrate
    };

    static bool load(HCCKB::KnowledgeBase &kb, std::vector<HCCKB::CaseRecord> &cases, LoadReport &report);

    // Ghi case vào kb/v1/cases/<id>.json rồi đọc lại để xác nhận.
    static bool saveCase(const HCCKB::CaseRecord &c, std::string &error);

    // "Xoá" case: chuyển file (c.source_file) sang kb/v1/trash/ - không xoá hẳn, chép lại để khôi phục.
    // Chỉ nhận file trong kb/v1/cases, kb/verified, kb/repair_confirmed.
    static bool trashCase(const HCCKB::CaseRecord &c, std::string &moved_to, std::string &error);

    // Ghi bản migrate của mọi case cũ vào kb/v1/cases (giữ nguyên file cũ). Trả số case đã ghi.
    static int migrateLegacyToSd(std::vector<std::string> &messages);

    static const char *defaultKbJson();
};
