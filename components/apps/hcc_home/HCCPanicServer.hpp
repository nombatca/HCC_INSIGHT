#pragma once

#include <stdint.h>
#include <string>

// HTTP server nhận RAW panic log qua Wi-Fi (LAN, không auth) và LƯU ra SD.
//   GET  /        -> form web để dán text hoặc chọn file .ips
//   POST /panic   -> lưu body vào /panic/raw/<timestamp>.ips, trả JSON {ok,file}
// Không tự phân tích; kỹ thuật viên bấm ANALYZE trên máy.
class HCCPanicServer {
public:
    static bool start(uint16_t port = 80);
    static void stop();
    static bool isRunning();

    // Lấy URL theo IP (vd "http://192.168.1.20:8080"). Trả false nếu chưa có IP.
    static bool deviceUrl(char *out, size_t out_size);

    // URL theo tên mDNS: "http://hccpanic.local:<port>".
    static void hostUrl(char *out, size_t out_size);

    // Thư mục lưu raw panic (tương đối gốc SD) - giữ cho file cũ còn trên thẻ.
    static const char *rawDir();

    // Panic nhận từ iPhone được GIỮ TRONG RAM (không ghi SD). Lấy bản mới nhất.
    // Trả false nếu chưa nhận panic nào. name/raw được copy an toàn (có khoá).
    static bool getReceived(std::string &name, std::string &raw);

    // True nếu panic RAM hiện tại là ảnh; *mime nhận kiểu ảnh (vd image/jpeg).
    static bool receivedIsImage(std::string *mime = nullptr);

    // Tăng 1 mỗi lần nhận panic mới (0 = chưa nhận). Màn PANIC so sánh để tự làm mới danh sách.
    static uint32_t receivedSerial();
};
