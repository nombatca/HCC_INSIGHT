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

// Nhãn mức có màu: DOCUMENTED xanh dương, INFERRED vàng, VERIFIED xanh lá, UNKNOWN xám.
std::string levelTag(Level l);

KbView renderView(const PanicFacts &facts, const Analysis &a);

// Bỏ '#' khỏi chữ tự do để không phá cú pháp recolor.
std::string noRecolor(const std::string &s);

}  // namespace HCCKB
