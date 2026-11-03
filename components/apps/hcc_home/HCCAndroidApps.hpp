#pragma once

#include <string>
#include <vector>

// Quét ứng dụng bên thứ 3 trên Android (qua ADB) để tìm app rác/spam và gỡ.
//
// listCommand() chạy 1 lệnh shell ADB: liệt kê app do người dùng cài (-3) kèm "installer"
// (nguồn cài). parseList() tách thành danh sách. App cài KHÔNG qua cửa hàng chính thống
// (sideload / installer=null / package installer) bị đánh dấu NGHI NGỜ — đó là tín hiệu
// app rác/APK lạ hay gặp. Việc gỡ do kỹ thuật viên quyết định (không tự động xoá).
//
// Gỡ bằng "pm uninstall --user 0 <pkg>": xoá cho user hiện tại, chạy được KHÔNG cần root,
// áp dụng cả app thường lẫn bloatware (không xoá vĩnh viễn phân vùng hệ thống).
class HCCAndroidApps {
public:
    struct AppEntry {
        std::string pkg;        // tên gói, vd com.facebook.katana
        std::string installer;  // nguồn cài, vd com.android.vending / null
        bool suspicious = false;// true nếu KHÔNG cài từ cửa hàng chính thống
    };

    // Lệnh liệt kê: "pm list packages -3 -i". Mỗi dòng: "package:<pkg>  installer=<src>".
    static const char *listCommand();

    // Tách output thành danh sách (sắp: nghi ngờ lên trước, rồi theo tên gói).
    static std::vector<AppEntry> parseList(const std::string &output);

    // Lệnh gỡ 1 app cho user 0 (không cần root).
    static std::string uninstallCommand(const std::string &pkg);

    // installer có phải cửa hàng ứng dụng chính thống không (Play, Galaxy Store, AppGallery...).
    static bool isOfficialStore(const std::string &installer);
};
