#include "HCCAndroidApps.hpp"

#include <algorithm>
#include <set>

const char *HCCAndroidApps::scanCommand()
{
    // Một lệnh gộp, output chia khối theo tiền tố ## để parseScan tách.
    // - PKGS: app bên thứ 3 + installer
    // - ACC : dịch vụ Trợ năng đang bật (danh sách component "pkg/cls:pkg2/cls")
    // - NL  : notification listener đang bật
    // - SMS : app SMS mặc định
    // - ADMIN: component device-admin đang kích hoạt (rút gọn còn "pkg/cls")
    return
        "echo '##PKGS'; pm list packages -3 -i 2>/dev/null; "
        "echo '##ACC'; settings get secure enabled_accessibility_services 2>/dev/null; "
        "echo '##NL'; settings get secure enabled_notification_listeners 2>/dev/null; "
        "echo '##SMS'; settings get secure sms_default_application 2>/dev/null; "
        "echo '##ADMIN'; dumpsys device_policy 2>/dev/null | grep -oE '[a-zA-Z0-9_.]+/[a-zA-Z0-9_.$]+' | sort -u";
}

std::string HCCAndroidApps::uninstallCommand(const std::string &pkg)
{
    return std::string("pm uninstall --user 0 ") + pkg + " 2>&1";
}

bool HCCAndroidApps::isOfficialStore(const std::string &inst)
{
    if (inst.empty() || inst == "null") return false;
    static const char *kStores[] = {
        "com.android.vending", "com.google.android.feedback",
        "com.sec.android.app.samsungapps", "com.amazon.venezia",
        "com.huawei.appmarket", "com.xiaomi.market", "com.xiaomi.mipicks",
        "com.heytap.market", "com.oppo.market", "com.bbk.appstore",
        "com.vivo.appstore", "com.qihoo.appstore",
    };
    for (const char *s : kStores) if (inst == s) return true;
    return false;
}

bool HCCAndroidApps::isBlocklisted(const std::string &pkg)
{
    // Danh sách gói xấu/adware đã biết (khớp đúng tên). MỞ RỘNG DẦN khi gặp thực tế.
    static const char *kBad[] = {
        "com.wifi.boost.cleaner",
        "com.super.cleaner.phonebooster",
        "com.battery.saver.booster.cleaner",
        "com.cool.security.antivirus",
        "com.phone.cleaner.booster.master",
    };
    for (const char *b : kBad) if (pkg == b) return true;
    // Mẫu tên hay gặp ở adware/PUP (khớp chuỗi con, thận trọng).
    static const char *kPat[] = {
        ".fakegps", ".sms.spy", ".hiddenspy", ".phonetracker.secret",
    };
    for (const char *p : kPat) if (pkg.find(p) != std::string::npos) return true;
    return false;
}

std::string HCCAndroidApps::flagsText(int flags)
{
    std::string s;
    auto add = [&](const char *t) { if (!s.empty()) s += ", "; s += t; };
    if (flags & F_BLOCKLIST) add("DS den");
    if (flags & F_ACCESS)    add("Tro nang");
    if (flags & F_DEVADMIN)  add("Admin");
    if (flags & F_NOTIFLIST) add("Thong bao");
    if (flags & F_DEFSMS)    add("SMS");
    if (flags & F_SIDELOAD)  add("Sideload");
    return s;
}

namespace {
// Lấy các tên gói (phần trước '/') từ 1 chuỗi danh sách component ngăn cách bởi ':' hoặc xuống dòng.
void collectPkgsFromComponents(const std::string &s, std::set<std::string> &out)
{
    size_t i = 0, n = s.size();
    while (i < n) {
        // tách theo ':' hoặc khoảng trắng / xuống dòng
        size_t j = i;
        while (j < n && s[j] != ':' && s[j] != '\n' && s[j] != '\r' && s[j] != ' ' && s[j] != '\t') j++;
        std::string tok = s.substr(i, j - i);
        size_t slash = tok.find('/');
        if (slash != std::string::npos && slash > 0) out.insert(tok.substr(0, slash));
        i = (j < n) ? j + 1 : j;
    }
}
std::string trimLine(const std::string &l)
{
    size_t a = 0, b = l.size();
    while (a < b && (l[a] == ' ' || l[a] == '\t')) a++;
    while (b > a && (l[b-1] == ' ' || l[b-1] == '\t' || l[b-1] == '\r')) b--;
    return l.substr(a, b - a);
}
} // namespace

std::vector<HCCAndroidApps::AppEntry> HCCAndroidApps::parseScan(const std::string &output)
{
    // Tách output theo section ##.
    std::vector<std::pair<std::string, std::string>> raw;  // (pkg, installer)
    std::set<std::string> accSet, nlSet, adminSet;
    std::string defSms;
    int section = 0;  // 0 none, 1 PKGS, 2 ACC, 3 NL, 4 SMS, 5 ADMIN
    std::string accBuf, nlBuf;

    size_t i = 0, n = output.size();
    while (i < n) {
        size_t eol = output.find('\n', i);
        if (eol == std::string::npos) eol = n;
        std::string line = trimLine(output.substr(i, eol - i));
        i = eol + 1;
        if (line.empty()) continue;
        if (line == "##PKGS") { section = 1; continue; }
        if (line == "##ACC")  { section = 2; continue; }
        if (line == "##NL")   { section = 3; continue; }
        if (line == "##SMS")  { section = 4; continue; }
        if (line == "##ADMIN"){ section = 5; continue; }
        switch (section) {
            case 1: {
                size_t p = line.find("package:");
                if (p == std::string::npos) break;
                p += 8;
                size_t sp = line.find_first_of(" \t", p);
                std::string pkg = (sp == std::string::npos) ? line.substr(p) : line.substr(p, sp - p);
                std::string inst;
                size_t ip = line.find("installer=");
                if (ip != std::string::npos) {
                    ip += 10;
                    size_t ie = line.find_first_of(" \t", ip);
                    inst = (ie == std::string::npos) ? line.substr(ip) : line.substr(ip, ie - ip);
                }
                if (!pkg.empty()) raw.push_back({pkg, inst});
                break;
            }
            case 2: if (line != "null") accBuf += line + ":"; break;
            case 3: if (line != "null") nlBuf += line + ":"; break;
            case 4: if (line != "null") defSms = line; break;
            case 5: collectPkgsFromComponents(line, adminSet); break;
        }
    }
    collectPkgsFromComponents(accBuf, accSet);
    collectPkgsFromComponents(nlBuf, nlSet);

    std::vector<AppEntry> out;
    out.reserve(raw.size());
    for (auto &r : raw) {
        AppEntry e;
        e.pkg = r.first;
        e.installer = r.second;
        int flags = 0, risk = 0;
        if (!isOfficialStore(e.installer)) { flags |= F_SIDELOAD; risk += 1; }
        if (accSet.count(e.pkg))   { flags |= F_ACCESS;    risk += 3; }
        if (adminSet.count(e.pkg)) { flags |= F_DEVADMIN;  risk += 3; }
        if (nlSet.count(e.pkg))    { flags |= F_NOTIFLIST; risk += 2; }
        if (!defSms.empty() && e.pkg == defSms && (flags & F_SIDELOAD)) { flags |= F_DEFSMS; risk += 2; }
        if (isBlocklisted(e.pkg))  { flags |= F_BLOCKLIST; risk += 10; }
        e.flags = flags;
        e.risk = risk;
        e.level = (risk >= 5 || (flags & F_BLOCKLIST)) ? 2 : (risk >= 1 ? 1 : 0);
        e.suspicious = e.level >= 1;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const AppEntry &a, const AppEntry &b) {
        if (a.risk != b.risk) return a.risk > b.risk;
        return a.pkg < b.pkg;
    });
    return out;
}
