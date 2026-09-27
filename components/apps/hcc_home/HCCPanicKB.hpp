#pragma once

#include <string>
#include "HCCPanicTypes.hpp"

// Knowledge Base trên thẻ SD, 3 tầng ưu tiên:
//   kb/verified          (VERIFIED)        - cao nhất
//   kb/repair_confirmed  (REPAIR_CONFIRMED)
//   kb/ai_inference      (AI_INFERENCE)     - cache suy luận AI
// Tra cứu luôn ưu tiên VERIFIED > REPAIR_CONFIRMED > AI_INFERENCE.
// Kết quả AI chỉ được ghi vào ai_inference; KHÔNG bao giờ tự lên VERIFIED.
class HCCPanicKB {
public:
    // Tạo cây thư mục KB nếu chưa có.
    static void ensureLayout();

    // Tra KB theo panic đã parse. Trả PanicResult.ok=true nếu có entry khớp,
    // kèm source_tier tương ứng. Không khớp -> ok=false.
    static PanicResult lookup(const ParsedPanic &parsed);

    // Lưu kết quả AI vào tầng ai_inference (tier cố định AI_INFERENCE).
    static bool saveAiResult(const ParsedPanic &parsed, const PanicResult &result);

    // Lưu CASE sửa chữa. verified=false -> REPAIR_CONFIRMED; verified=true -> VERIFIED
    // (và xoá bản REPAIR_CONFIRMED cùng id nếu có). Sau khi ghi sẽ ĐỌC LẠI để xác nhận.
    // Trả về false nếu ghi/đọc lại thất bại.
    static bool saveCase(const RepairCase &c, bool verified);

private:
    static bool lookupInTier(const char *rel_dir, const char *tier_name,
                             const ParsedPanic &parsed, PanicResult &out);
};
