#pragma once

#include <string>

#include "HCCPanicKBv1.hpp"

// Chuyển kết quả KB V1 (HCCKB::Analysis) thành chữ cho các ô màn PANIC (BƯỚC 3).
// Không phụ thuộc LVGL: chuỗi dùng cú pháp recolor "#RRGGBB chữ#" của LVGL cho nhãn mức bằng
// chứng; test được trên máy host.
namespace HCCKB {

struct KbView {
    std::string chain;       // CHUỖI PHÂN TÍCH: loại -> driver/sensor/mã -> subsystem -> phần cứng -> bus/rail
    std::string checks;      // ĐO KIỂM / KIỂM TRA
    std::string match;       // ĐỐI CHIẾU VERIFIED / KB: confidence, case khớp, các mục UNKNOWN
    std::string conclusion;  // KẾT LUẬN: theo case, hoặc nhóm + "linh kiện: UNKNOWN"
    Confidence confidence = Confidence::None;
    const CaseRecord *top_case = nullptr; // case tốt nhất (nullptr nếu không có)
};

// Kết quả cơ bản cho kỹ thuật viên (màn chính). KHÔNG chứa nhãn UNKNOWN / INFERRED, mailbox,
// bitmask, RTKit hay chuỗi suy luận: những thứ đó nằm trong renderDetail().
struct BasicView {
    std::string error;        // LỖI: loại panic + thông điệp ngắn
    std::string signature;    // SIGNATURE / HEX: mã, target, tag, driver/sensor chính, signature
    std::string hardware;     // PHẦN CỨNG NGHI NGỜ (kết luận, hiện to nhất)
    bool hardware_confirmed = false; // true: từ VERIFIED case khớp chính xác
    bool hardware_known = false;     // false: chưa xác định được khu vực nào
    Confidence confidence = Confidence::None;
    std::string confidence_text;     // "CAO" / "TRUNG BÌNH" / "THẤP" / "CHƯA ĐỦ DỮ KIỆN"
    std::string steps;        // CÁC BƯỚC KIỂM TRA (đánh số)
    std::string case_match;   // VERIFIED / REPAIR CASE
};
BasicView renderBasic(const PanicFacts &facts, const Analysis &a);

// Toàn bộ phân tích kỹ thuật (cho nút XEM PHÂN TÍCH CHI TIẾT): kết luận + chuỗi suy luận có
// nhãn mức bằng chứng, đối chiếu KB/case, UNKNOWN, và mọi fact của Parser v2 (mailbox, RTKit...).
std::string renderDetail(const PanicFacts &facts, const Analysis &a);

// Nhãn mức có màu: DOCUMENTED xanh dương, INFERRED vàng, VERIFIED xanh lá, UNKNOWN xám.
std::string levelTag(Level l);

// Màn CASE ĐÃ LƯU: một dòng tóm tắt cho danh sách, và nội dung đầy đủ (recolor LVGL) của một case.
std::string caseStatusText(const CaseRecord &c);
std::string caseListLine(const CaseRecord &c);
std::string describeCase(const CaseRecord &c);

KbView renderView(const PanicFacts &facts, const Analysis &a);

// Bỏ '#' khỏi chữ tự do để không phá cú pháp recolor.
std::string noRecolor(const std::string &s);

}  // namespace HCCKB
