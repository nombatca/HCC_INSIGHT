#include "HCCAndroidApps.hpp"

#include <algorithm>

const char *HCCAndroidApps::listCommand()
{
    // -3: chỉ app bên thứ 3 (người dùng cài). -i: kèm installer. Lọc rỗng cho gọn.
    return "pm list packages -3 -i 2>/dev/null";
}

std::string HCCAndroidApps::uninstallCommand(const std::string &pkg)
{
    return std::string("pm uninstall --user 0 ") + pkg + " 2>&1";
}

bool HCCAndroidApps::isOfficialStore(const std::string &inst)
{
    if (inst.empty() || inst == "null") return false;
    static const char *kStores[] = {
        "com.android.vending",            // Google Play
        "com.google.android.feedback",    // Play (cũ)
        "com.sec.android.app.samsungapps",// Samsung Galaxy Store
        "com.amazon.venezia",             // Amazon Appstore
        "com.huawei.appmarket",           // Huawei AppGallery
        "com.xiaomi.market",              // Xiaomi GetApps
        "com.xiaomi.mipicks",
        "com.heytap.market",              // Oppo/Realme
        "com.oppo.market",
        "com.bbk.appstore",               // Vivo
        "com.vivo.appstore",
        "com.qihoo.appstore",
    };
    for (const char *s : kStores) {
        if (inst == s) return true;
    }
    return false;
}

std::vector<HCCAndroidApps::AppEntry> HCCAndroidApps::parseList(const std::string &output)
{
    std::vector<AppEntry> out;
    size_t i = 0, n = output.size();
    while (i < n) {
        size_t eol = output.find('\n', i);
        if (eol == std::string::npos) eol = n;
        std::string line = output.substr(i, eol - i);
        i = eol + 1;
        // Bỏ \r cuối (CRLF).
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        // Tìm "package:"
        size_t p = line.find("package:");
        if (p == std::string::npos) continue;
        p += 8;
        // pkg = tới khoảng trắng đầu tiên.
        size_t sp = line.find_first_of(" \t", p);
        std::string pkg = (sp == std::string::npos) ? line.substr(p) : line.substr(p, sp - p);
        if (pkg.empty()) continue;
        // installer=
        std::string inst;
        size_t ip = line.find("installer=");
        if (ip != std::string::npos) {
            ip += 10;
            size_t ie = line.find_first_of(" \t", ip);
            inst = (ie == std::string::npos) ? line.substr(ip) : line.substr(ip, ie - ip);
        }
        AppEntry e;
        e.pkg = pkg;
        e.installer = inst;
        e.suspicious = !isOfficialStore(inst);
        out.push_back(e);
    }
    // Sắp: nghi ngờ trước, rồi theo tên gói.
    std::sort(out.begin(), out.end(), [](const AppEntry &a, const AppEntry &b) {
        if (a.suspicious != b.suspicious) return a.suspicious && !b.suspicious;
        return a.pkg < b.pkg;
    });
    return out;
}
