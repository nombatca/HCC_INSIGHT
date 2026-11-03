#pragma once

#include <string>
#include <vector>

// Quét ứng dụng Android (qua ADB) để tìm app rác/spam/malware và gỡ.
//
// scanCommand() chạy 1 lệnh shell gộp: liệt kê app bên thứ 3 (+installer) và các "tín hiệu
// toàn cục" mà malware/scam hay lạm dụng: dịch vụ Trợ năng đang bật, Notification listener
// đang bật, Device-admin đang kích hoạt, app SMS mặc định. parseScan() đối chiếu từng app
// với các tập này + blocklist để CHẤM ĐIỂM RỦI RO (không cần dumpsys từng app, rất nhẹ).
//
// level: 0 = an toàn, 1 = nghi ngờ, 2 = NGUY HIỂM. Đây là HEURISTIC hành vi (bắt tốt app
// lừa đảo ngân hàng kiểu chiếm Trợ năng + đọc SMS/thông báo), KHÔNG phải antivirus đầy đủ.
//
// Gỡ: "pm uninstall --user 0 <pkg>" (không cần root).
class HCCAndroidApps {
public:
    enum Flag {
        F_SIDELOAD    = 1 << 0,  // cài ngoài cửa hàng chính thống
        F_ACCESS      = 1 << 1,  // dịch vụ Trợ năng đang bật (chiếm thao tác)
        F_DEVADMIN    = 1 << 2,  // device-admin đang kích hoạt (khó gỡ)
        F_NOTIFLIST   = 1 << 3,  // đọc mọi thông báo (trộm OTP)
        F_DEFSMS      = 1 << 4,  // là app SMS mặc định (chặn/đọc OTP)
        F_BLOCKLIST   = 1 << 5,  // trùng danh sách gói xấu đã biết
    };

    struct AppEntry {
        std::string pkg;
        std::string installer;
        int flags = 0;
        int risk = 0;            // điểm rủi ro
        int level = 0;           // 0 an toàn, 1 nghi ngờ, 2 nguy hiểm
        bool suspicious = false; // tương thích cũ: level >= 1
    };

    // 1 lệnh gộp (nhiều dòng), xuất các mục có tiền tố ##PKGS/##ACC/##NL/##SMS/##ADMIN.
    static const char *scanCommand();
    // Phân tích output của scanCommand -> danh sách đã chấm điểm (nguy hiểm lên đầu).
    static std::vector<AppEntry> parseScan(const std::string &output);

    static std::string uninstallCommand(const std::string &pkg);
    static bool isOfficialStore(const std::string &installer);
    static bool isBlocklisted(const std::string &pkg);

    // Mô tả ngắn các cờ rủi ro của 1 app (vd "Trợ năng, SMS, Sideload").
    static std::string flagsText(int flags);
};
