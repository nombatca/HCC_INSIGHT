#pragma once

#include <string>
#include <vector>

// Tiện ích lưu trữ trên thẻ SD dùng chung cho tính năng PANIC.
// Tự mount SD qua BSP nếu chưa mount. Mọi đường dẫn tương đối tính từ gốc thẻ.
class HCCPanicStorage {
public:
    // Đảm bảo thẻ SD đã mount. Trả về true nếu sẵn sàng.
    static bool ensureMounted();

    // Tạo (đệ quy) thư mục con dưới gốc thẻ, vd "panic/raw".
    static bool ensureDir(const std::string &rel_dir);

    // Ghi buffer ra file (rel_path tính từ gốc thẻ). Tự tạo thư mục cha.
    static bool writeFile(const std::string &rel_path, const char *data, size_t len);

    // Đọc toàn bộ file vào chuỗi. Trả về false nếu không đọc được.
    static bool readFile(const std::string &rel_path, std::string &out);

    // Liệt kê file trong thư mục (không đệ quy), lọc theo đuôi nếu ext != nullptr.
    static std::vector<std::string> listDir(const std::string &rel_dir, const char *ext);

    // Đường dẫn tuyệt đối "<mount>/<rel_path>".
    static std::string absPath(const std::string &rel_path);
};
