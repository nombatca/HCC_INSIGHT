#pragma once

#include <string>
#include <vector>

// Kết quả parse RAW panic log thành cấu trúc chuẩn hoá.
struct ParsedPanic {
    std::string model;          // product / model code (vd iPhone14,2)
    std::string os_version;     // build / os version
    std::string panic_type;     // bug_type hoặc loại panic
    std::string panic_string;   // đoạn panic string đã cắt gọn
    std::string source;         // trường "source" nếu có
    std::string index;          // trường "index" nếu có
    std::vector<std::string> error_codes;  // mã lỗi: "123 (0x7B)"
    std::vector<std::string> sensors;      // sensor / error token
    std::string signature;      // chữ ký để tra Knowledge Base
    std::string raw_excerpt;    // trích đoạn ngắn để hiển thị
    bool valid = false;
};

// Một CASE sửa chữa thực tế do kỹ thuật viên nhập, liên kết với file panic.
struct RepairCase {
    std::string panic_file;    // file raw panic liên kết
    std::string model;         // MODEL (tự lấy, sửa được)
    std::string panic_type;    // loại panic
    std::string signature;     // PANIC/SIGNATURE
    std::string code_hex;      // CODE HEX (tự lấy/chuyển nếu có)
    std::string source;        // SOURCE, vd S.sensor[1]
    std::string hardware;      // HARDWARE / LỖI THỰC TẾ (người dùng nhập)
    std::string repair_action; // REPAIR ACTION
    std::string note;          // ghi chú tuỳ chọn
    std::string created_at;    // thời điểm lưu
};

// Kết quả phân tích cuối cùng (từ KB hoặc AI).
struct PanicResult {
    std::string error;        // LỖI
    std::string cause;        // NGUYÊN NHÂN
    std::string repair;       // KIỂM TRA / SỬA CHỮA
    std::string confidence;   // ĐỘ TIN CẬY
    std::string source_tier;  // VERIFIED | REPAIR_CONFIRMED | AI_INFERENCE
    bool ok = false;
    std::string message;      // thông báo lỗi khi ok == false
};
