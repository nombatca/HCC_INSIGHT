#include "HCCAndroidDiagnostics.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using Status = HCCAndroidDiagnostics::Status;
using Item = HCCAndroidDiagnostics::Item;
using Group = HCCAndroidDiagnostics::Group;

// Sections are introduced by "##X" lines; a section is complete once the next marker arrives
// ("##E" ends the last one). Output is filtered on the phone to stay well under the 12 KB
// the ADB client keeps: some ROMs print very long dumpsys battery / telephony.registry.
constexpr char kCommand[] =
    "echo '##S';dumpsys sensorservice|grep -o 'android\\.sensor\\.[a-z_]*'|sort -u;"
    "echo '##F';pm list features|grep -E 'sensor|fingerprint|biometrics|telephony|camera';"
    "echo '##P';dumpsys fingerprint 2>/dev/null|grep -ciE 'sensorid|sensor_id|sensorprops|prints';"
    "echo '##A';dumpsys face 2>/dev/null|grep -ciE 'sensorid|sensor_id|sensorprops';"
    "echo '##R';for p in gsm.version.baseband gsm.sim.state gsm.sim.operator.alpha gsm.sim.operator.numeric "
    "persist.radio.multisim.config ro.boot.baseband;do echo \"$p=$(getprop $p)\";done;"
    "echo \"airplane=$(settings get global airplane_mode_on)\";echo \"mdata=$(settings get global mobile_data)\";"
    "echo '##T';dumpsys telephony.registry|grep -E '^ *(Phone Id=|mServiceState=|mSignalStrength=|mCellInfo=)'"
    "|head -12|cut -c1-600;"
    "echo '##L';x=$(logcat -b radio -d -t 3000 2>/dev/null|grep -iE "
    "'RADIO_NOT_AVAILABLE|modem.{0,20}(crash|reset|restart)|subsystem restart');"
    "echo \"n=$(echo \"$x\"|grep -c .)\";echo \"$x\"|tail -2|cut -c1-160;"
    "echo '##I';service call iphonesubinfo 1 s16 com.android.shell 2>&1|cut -c1-80;getprop|grep -i imei|cut -c1-80;"
    "echo '##B';dumpsys battery|grep -iE '^ *((present|level|health|temperature|voltage|status|technology|"
    "charge counter)|(ac|usb|wireless|dock) powered|max charging (current|voltage)):|asoc|bsoh|cycle|first.?use'"
    "|cut -c1-100|head -30;d=/sys/class/power_supply/battery;"
    "for f in cycle_count charge_full charge_full_design current_now;do echo \"$f=$(cat $d/$f 2>/dev/null)\";done;"
    "echo '##C';dumpsys batterystats|grep -m4 -iE ' capacity: [0-9]|learned battery capacity|estimated battery "
    "capacity'|cut -c1-120;"
    "echo '##H';for z in /sys/class/thermal/thermal_zone*;do echo \"$(cat $z/type 2>/dev/null)=$(cat $z/temp "
    "2>/dev/null)\";done|grep -v '=$'|head -60|cut -c1-48;"
    "echo '##K';dumpsys media.camera 2>&1|grep -iE -A1 'number of camera devices|== camera hal device|"
    "camera [0-9]+ static information|^ *facing:|has a flash unit|pixelarraysize \\('"
    "|grep -vE '^--$'|cut -c1-110|head -80;"
    "echo '##E'";

struct Parsed {
    bool complete = false;
    std::set<char> done; // sections followed by another marker, i.e. not cut off
    std::set<std::string> sensors;  // "accelerometer", "proximity", ...
    std::set<std::string> features; // "android.hardware.fingerprint", ...
    int fingerprint_lines = -1;
    int face_lines = -1;
    std::map<std::string, std::string> values; // radio "key=value" and battery "key: value"
    std::vector<std::string> telephony_lines;   // dumpsys telephony.registry (filtered)
    int radio_errors = -1;                      // modem error lines in the radio log; -1 = not read
    std::vector<std::string> radio_log;         // last matching lines
    std::vector<std::string> imei_lines;        // service call / getprop output
    std::vector<std::string> battery_stats;     // dumpsys batterystats capacity lines
    std::vector<std::string> thermal;           // "zone_type=temp" lines
    std::vector<std::string> camera_lines;      // dumpsys media.camera (filtered)
};

std::string trim(const std::string &text)
{
    const size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

Parsed parseSections(const char *output)
{
    Parsed parsed;
    char section = 0;
    const char *line = output != nullptr ? output : "";
    while (*line != '\0') {
        const char *newline = strchr(line, '\n');
        const std::string text = trim(newline != nullptr ? std::string(line, newline - line) : std::string(line));
        line = newline != nullptr ? newline + 1 : line + strlen(line);

        if (text.size() == 3 && text[0] == '#' && text[1] == '#') {
            if (section != 0) {
                parsed.done.insert(section);
            }
            section = text[2];
            parsed.complete = section == 'E';
            continue;
        }
        if (text.empty()) {
            continue;
        }
        switch (section) {
        case 'S':
            if (text.rfind("android.sensor.", 0) == 0) {
                parsed.sensors.insert(text.substr(strlen("android.sensor.")));
            }
            break;
        case 'F':
            parsed.features.insert(text.rfind("feature:", 0) == 0 ? text.substr(strlen("feature:")) : text);
            break;
        case 'P':
            parsed.fingerprint_lines = atoi(text.c_str());
            break;
        case 'A':
            parsed.face_lines = atoi(text.c_str());
            break;
        case 'T':
            parsed.telephony_lines.push_back(text);
            break;
        case 'L':
            if (text.rfind("n=", 0) == 0) {
                parsed.radio_errors = atoi(text.c_str() + 2);
            } else {
                parsed.radio_log.push_back(text);
            }
            break;
        case 'I':
            parsed.imei_lines.push_back(text);
            break;
        case 'C':
            parsed.battery_stats.push_back(text);
            break;
        case 'H':
            parsed.thermal.push_back(text);
            break;
        case 'K':
            parsed.camera_lines.push_back(text);
            break;
        case 'R':
        case 'B': {
            // "key=value" (our echoes) or "key: value" (dumpsys battery).
            const size_t eq = text.find('=');
            const size_t colon = text.find(": ");
            if (eq != std::string::npos && (colon == std::string::npos || eq < colon)) {
                parsed.values[trim(text.substr(0, eq))] = trim(text.substr(eq + 1));
            } else if (colon != std::string::npos) {
                parsed.values[trim(text.substr(0, colon))] = trim(text.substr(colon + 2));
            }
            break;
        }
        default:
            break;
        }
    }
    return parsed;
}

std::string value(const Parsed &parsed, const char *key)
{
    const auto it = parsed.values.find(key);
    return it != parsed.values.end() ? it->second : "";
}

bool isNumber(const std::string &text)
{
    return !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
}

Item makeItem(const char *name, Status status, const char *source, const std::string &evidence, uint32_t ts)
{
    Item item;
    item.name = name;
    item.status = status;
    item.source = source;
    item.evidence = evidence;
    item.ts = ts;
    return item;
}

Group finish(const char *name, std::vector<Item> items)
{
    Group group;
    group.name = name;
    group.items = std::move(items);
    group.status = HCCiPhoneDiagnostics::rollup(group.items);
    return group;
}

Group notTested(const char *name, uint32_t ts)
{
    return finish(name, {makeItem("Chưa có dữ liệu", Status::NotTested, "ADB shell",
                                  "Lệnh chẩn đoán chưa chạy xong (máy rút ra hoặc ADB mất kết nối).", ts)});
}

Group checkSensors(const Parsed &parsed, uint32_t ts)
{
    static const struct {
        const char *label;
        const char *type;    // android.sensor.<type>
        const char *feature; // android.hardware.sensor.<feature>
    } kSensors[] = {
        {"Accelerometer", "accelerometer", "android.hardware.sensor.accelerometer"},
        {"Gyroscope", "gyroscope", "android.hardware.sensor.gyroscope"},
        {"Proximity", "proximity", "android.hardware.sensor.proximity"},
        {"Ambient Light", "light", "android.hardware.sensor.light"},
        {"Compass", "magnetic_field", "android.hardware.sensor.compass"},
        {"Barometer", "pressure", "android.hardware.sensor.barometer"},
    };

    std::vector<Item> items;
    for (const auto &sensor : kSensors) {
        const bool present = parsed.sensors.count(sensor.type) > 0;
        const bool declared = parsed.features.count(sensor.feature) > 0;
        if (present) {
            items.push_back(makeItem(sensor.label, Status::Pass, "dumpsys sensorservice",
                                     std::string("HAL đã đăng ký android.sensor.") + sensor.type +
                                         " (IC phản hồi khi khởi động; chưa đo giá trị sống).",
                                     ts));
        } else if (declared) {
            items.push_back(makeItem(sensor.label, Status::Fail, "dumpsys sensorservice + pm list features",
                                     std::string("Máy khai báo ") + sensor.feature +
                                         " nhưng HAL không có cảm biến này: nghi hỏng IC, lỏng/đứt cáp "
                                         "hoặc mất nguồn cảm biến.",
                                     ts));
        } else {
            items.push_back(makeItem(sensor.label, Status::Unsupported, "pm list features",
                                     "Máy không khai báo cảm biến này.", ts));
        }
    }

    std::string all;
    for (const auto &type : parsed.sensors) {
        all += (all.empty() ? "" : ", ") + type;
    }
    items.push_back(makeItem("Sensor HAL", parsed.sensors.empty() ? Status::Fail : Status::Pass,
                             "dumpsys sensorservice",
                             parsed.sensors.empty()
                                 ? std::string("HAL không trả về cảm biến nào (sensor hub không khởi động?).")
                                 : std::to_string(parsed.sensors.size()) + " loại: " + all,
                             ts));
    return finish("SENSORS", std::move(items));
}

Item biometricItem(const char *name, const char *feature, const char *service, int lines, const Parsed &parsed,
                   uint32_t ts)
{
    if (parsed.features.count(feature) == 0) {
        return makeItem(name, Status::Unsupported, "pm list features",
                        std::string("Máy không khai báo ") + feature + ".", ts);
    }
    if (lines > 0) {
        return makeItem(name, Status::Pass, service,
                        "Dịch vụ thấy cảm biến (" + std::to_string(lines) +
                            " dòng khớp). Thử đăng ký vân tay/khuôn mặt để xác nhận chức năng.",
                        ts);
    }
    return makeItem(name, Status::Check, service,
                    std::string("Máy khai báo ") + feature +
                        " nhưng dịch vụ không báo cảm biến: nghi hỏng/lỏng cáp cảm biến. Kiểm tra lại trong "
                        "Cài đặt > Bảo mật.",
                    ts);
}

Group checkBiometrics(const Parsed &parsed, uint32_t ts)
{
    return finish("VÂN TAY / FACE",
                  {biometricItem("Fingerprint", "android.hardware.fingerprint", "dumpsys fingerprint",
                                 parsed.fingerprint_lines, parsed, ts),
                   biometricItem("Face (biometric)", "android.hardware.biometrics.face", "dumpsys face",
                                 parsed.face_lines, parsed, ts)});
}

// ---- Radio helpers (BASEBAND / SIM) ----

std::vector<std::string> splitSlots(const std::string &text)
{
    // Dual-SIM getprops are "slot1,slot2".
    std::vector<std::string> slots;
    size_t start = 0;
    while (true) {
        const size_t comma = text.find(',', start);
        slots.push_back(trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return slots;
}

std::string slotValue(const Parsed &parsed, const char *key, size_t slot)
{
    const std::vector<std::string> slots = splitSlots(value(parsed, key));
    return slot < slots.size() ? slots[slot] : "";
}

// Signed integer that follows `key` (e.g. "rsrp=" or "ssRsrp = "), searched from `from`.
bool intAfter(const std::string &line, size_t from, size_t window, const char *key, int *out)
{
    const size_t pos = line.find(key, from);
    if (pos == std::string::npos || pos > from + window) {
        return false;
    }
    size_t i = pos + strlen(key);
    while (i < line.size() && (line[i] == ' ' || line[i] == '=')) {
        ++i;
    }
    char *end = nullptr;
    const long parsed = strtol(line.c_str() + i, &end, 10);
    if (end == line.c_str() + i) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

// Text after `key` up to the next ',', ' ' or '}' (e.g. "mOperatorAlphaLong=Viettel,").
std::string wordAfter(const std::string &line, size_t from, const char *key)
{
    const size_t pos = line.find(key, from);
    if (pos == std::string::npos) {
        return "";
    }
    const size_t begin = pos + strlen(key);
    const size_t end = line.find_first_of(",}", begin);
    return trim(line.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
}

bool luhnValid(const std::string &digits)
{
    int sum = 0;
    for (size_t i = 0; i < digits.size(); ++i) {
        int d = digits[digits.size() - 1 - i] - '0';
        if (i % 2 == 1) {
            d *= 2;
            if (d > 9) {
                d -= 9;
            }
        }
        sum += d;
    }
    return sum % 10 == 0;
}

std::string digitsOnly(const std::string &text)
{
    std::string digits;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            digits += c;
        }
    }
    return digits;
}

// IMEIs from `service call iphonesubinfo` (UTF-16 parcel dump, text between quotes) and from
// vendor getprops such as "[ro.ril.oem.imei1]: [86...]".
std::vector<std::string> imeiCandidates(const std::vector<std::string> &lines)
{
    std::vector<std::string> candidates;
    std::string parcel;
    for (const std::string &line : lines) {
        if (line.find("0x") != std::string::npos && line.find('\'') != std::string::npos) {
            const size_t open = line.find('\'');
            const size_t close = line.rfind('\'');
            if (close > open) {
                parcel += line.substr(open + 1, close - open - 1);
            }
        } else if (line[0] == '[') {
            const size_t value_open = line.find("]: [");
            if (value_open != std::string::npos) {
                const std::string digits = digitsOnly(line.substr(value_open + 4));
                if (digits.size() == 15) {
                    candidates.push_back(digits);
                }
            }
        }
    }
    // A permission error comes back as text (letters) instead of 15 digits.
    const std::string parcel_digits = digitsOnly(parcel);
    if (parcel_digits.size() == 15) {
        candidates.insert(candidates.begin(), parcel_digits);
    }
    std::vector<std::string> unique;
    for (const std::string &candidate : candidates) {
        bool seen = false;
        for (const std::string &u : unique) {
            seen = seen || u == candidate;
        }
        if (!seen) {
            unique.push_back(candidate);
        }
    }
    return unique;
}

struct PhoneRadio {
    std::string service;  // mServiceState=...
    std::string signal;   // mSignalStrength=...
    std::string cellinfo; // mCellInfo=...
};

std::vector<PhoneRadio> phonesFromRegistry(const std::vector<std::string> &lines)
{
    std::vector<PhoneRadio> phones;
    size_t current = 0;
    for (const std::string &line : lines) {
        const size_t id = line.find("Phone Id=");
        if (id != std::string::npos) {
            current = static_cast<size_t>(atoi(line.c_str() + id + strlen("Phone Id=")));
            continue;
        }
        if (current > 3) {
            continue;
        }
        if (phones.size() <= current) {
            phones.resize(current + 1);
        }
        PhoneRadio &phone = phones[current];
        std::string *slot = line.rfind("mServiceState=", 0) == 0     ? &phone.service
                            : line.rfind("mSignalStrength=", 0) == 0 ? &phone.signal
                            : line.rfind("mCellInfo=", 0) == 0       ? &phone.cellinfo
                                                                     : nullptr;
        if (slot != nullptr && slot->empty()) {
            *slot = line;
        }
    }
    return phones;
}

Item serviceItem(const char *name, const PhoneRadio &phone, bool airplane, const std::string &mobile_data,
                 uint32_t ts)
{
    const char *source = "dumpsys telephony.registry (ServiceState)";
    if (phone.service.empty()) {
        return makeItem(name, Status::Unsupported, source, "Không đọc được ServiceState.", ts);
    }
    int voice = -1;
    int data = -1;
    const std::string &s = phone.service;
    const size_t body = strlen("mServiceState=");
    if (body < s.size() && s[body] >= '0' && s[body] <= '9') {
        // Older Android: "mServiceState=0 0 voice home data home ..."
        voice = atoi(s.c_str() + body);
        const size_t space = s.find(' ', body);
        data = space != std::string::npos ? atoi(s.c_str() + space + 1) : -1;
    } else {
        intAfter(s, 0, s.size(), "mVoiceRegState=", &voice);
        intAfter(s, 0, s.size(), "mDataRegState=", &data);
    }
    const bool emergency = s.find("mIsEmergencyOnly=true") != std::string::npos ||
                           s.find("EmergOnly") != std::string::npos || voice == 2;
    std::string op = wordAfter(s, 0, "mOperatorAlphaLong=");
    std::string rat = wordAfter(s, 0, "accessNetworkTechnology=");
    if (rat.empty()) {
        rat = wordAfter(s, 0, "getRilDataRadioTechnology=");
    }
    std::string evidence = "voice=" + std::to_string(voice) + ", data=" + std::to_string(data) +
                           ", nhà mạng: " + (op.empty() || op == "null" ? "-" : op) +
                           ", công nghệ: " + (rat.empty() ? "-" : rat) +
                           ", dữ liệu di động: " + (mobile_data == "1" ? "bật" : mobile_data == "0" ? "tắt" : "-");

    if (airplane) {
        return makeItem(name, Status::NotTested, source, "Đang bật chế độ máy bay. " + evidence, ts);
    }
    if (voice == 0 || data == 0) {
        return makeItem(name, Status::Pass, source, "Đã đăng ký mạng. " + evidence, ts);
    }
    if (voice == 3 && data == 3) {
        return makeItem(name, Status::Fail, source,
                        "Radio TẮT dù không bật chế độ máy bay: modem không lên (nguồn modem, IC modem, "
                        "firmware modem). " + evidence,
                        ts);
    }
    if (emergency) {
        return makeItem(name, Status::Check, source,
                        "Chỉ gọi khẩn cấp: SIM không đăng ký được mạng. Nghi IMEI hỏng/bị khoá hoặc lỗi RF. " +
                            evidence,
                        ts);
    }
    return makeItem(name, Status::Check, source,
                    "Ngoài vùng phủ sóng: kiểm tra ăng-ten, cáp đồng trục, IC RF/PA (so với máy khác cùng chỗ). " +
                        evidence,
                    ts);
}

Item signalItem(const char *name, const PhoneRadio &phone, bool airplane, uint32_t ts)
{
    const char *source = "dumpsys telephony.registry (SignalStrength)";
    const std::string &s = phone.signal;
    struct Reading {
        const char *anchor;
        const char *key;
        const char *label;
        int min;
        int max;
        int weak; // below this -> weak
    } kReadings[] = {
        {"CellSignalStrengthLte", "rsrp", "LTE RSRP", -140, -43, -110},
        {"CellSignalStrengthNr", "ssRsrp", "5G SS-RSRP", -156, -31, -110},
        {"CellSignalStrengthWcdma", "rscp", "WCDMA RSCP", -120, -24, -105},
        {"CellSignalStrengthGsm", "rssi", "GSM RSSI", -113, -51, -95},
    };

    std::string evidence;
    bool have = false;
    bool weak = false;
    for (const auto &reading : kReadings) {
        const size_t anchor = s.find(reading.anchor);
        int dbm = 0;
        if (anchor != std::string::npos && intAfter(s, anchor, 200, reading.key, &dbm) &&
            dbm >= reading.min && dbm <= reading.max) {
            if (!have) {
                weak = dbm < reading.weak; // judged on the first (most modern) technology present
            }
            have = true;
            evidence += std::string(evidence.empty() ? "" : ", ") + reading.label + " " + std::to_string(dbm) + " dBm";
            if (strcmp(reading.anchor, "CellSignalStrengthLte") == 0) {
                int rsrq = 0;
                int sinr = 0;
                if (intAfter(s, anchor, 200, "rsrq", &rsrq) && rsrq > -40 && rsrq < 0) {
                    evidence += ", RSRQ " + std::to_string(rsrq) + " dB";
                }
                if (intAfter(s, anchor, 200, "rssnr", &sinr) && sinr > -30 && sinr < 60) {
                    evidence += ", SINR " + std::to_string(sinr) + " dB";
                }
            }
        }
    }
    if (airplane) {
        return makeItem(name, Status::NotTested, source, "Đang bật chế độ máy bay.", ts);
    }
    if (!have) {
        return makeItem(name, Status::Unsupported, source,
                        s.empty() ? "Không đọc được SignalStrength." : "Không có giá trị sóng hợp lệ (chưa có mạng).",
                        ts);
    }
    return makeItem(name, weak ? Status::Check : Status::Pass, source,
                    evidence + (weak ? "  -> sóng yếu: so với máy khác cùng chỗ; nếu máy khác sóng tốt thì nghi "
                                       "ăng-ten, cáp đồng trục, IC RF/PA."
                                     : "  -> sóng tốt."),
                    ts);
}

Item bandItem(const char *name, const PhoneRadio &phone, uint32_t ts)
{
    const char *source = "dumpsys telephony.registry (CellInfo)";
    const std::string &s = phone.cellinfo;
    const size_t registered = s.find("mRegistered=YES");
    if (registered == std::string::npos) {
        return makeItem(name, Status::Unsupported, source, "Không có cell đang đăng ký.", ts);
    }
    const size_t type_pos = s.rfind("CellInfo", registered);
    std::string type = "?";
    if (type_pos != std::string::npos) {
        const size_t end = s.find_first_of(":{ ", type_pos);
        type = s.substr(type_pos + strlen("CellInfo"), end == std::string::npos ? 0 : end - type_pos - strlen("CellInfo"));
    }
    std::string evidence = "Công nghệ: " + type;
    int number = 0;
    static const char *const kChannels[] = {"mEarfcn", "mNrArfcn", "mUarfcn", "mArfcn"};
    for (const char *key : kChannels) {
        if (intAfter(s, registered, 600, key, &number) && number >= 0 && number < 3000000) {
            evidence += std::string(", ") + (key + 1) + " " + std::to_string(number);
            break;
        }
    }
    const size_t bands = s.find("mBands", registered);
    if (bands != std::string::npos && bands < registered + 600) {
        const size_t open = s.find('[', bands);
        const size_t close = s.find(']', bands);
        if (open != std::string::npos && close != std::string::npos && close > open && close - open < 40) {
            evidence += ", band " + s.substr(open + 1, close - open - 1);
        }
    }
    if (intAfter(s, registered, 600, "mPci", &number) && number >= 0 && number < 1100) {
        evidence += ", PCI " + std::to_string(number);
    }
    return makeItem(name, Status::Pass, source, evidence, ts);
}

Group checkBaseband(const Parsed &parsed, uint32_t ts)
{
    std::vector<Item> items;
    if (parsed.features.count("android.hardware.telephony") == 0) {
        items.push_back(makeItem("Telephony", Status::Unsupported, "pm list features",
                                 "Máy không có chức năng di động (tablet Wi-Fi).", ts));
        return finish("BASEBAND / SIM", std::move(items));
    }

    // Airplane mode first: with the radio off by choice, network results are not failures.
    const std::string airplane_value = value(parsed, "airplane");
    const bool airplane = airplane_value == "1";
    items.push_back(makeItem("Chế độ máy bay",
                             airplane ? Status::Check : (airplane_value == "0" ? Status::Pass : Status::Unsupported),
                             "settings get global airplane_mode_on",
                             airplane ? "ĐANG BẬT: tắt đi rồi chạy lại để kiểm tra mạng/sóng." : "Tắt.", ts));

    // Modem present on the board, and how many SIM slots the firmware expects.
    const std::string boot_baseband = value(parsed, "ro.boot.baseband");
    const std::string multisim = value(parsed, "persist.radio.multisim.config");
    size_t slot_count = multisim == "dsds" || multisim == "dsda" ? 2 : multisim == "tsts" ? 3 : 1;
    const std::string modem_evidence = "ro.boot.baseband=" + (boot_baseband.empty() ? std::string("-") : boot_baseband) +
                                       ", cấu hình SIM: " + (multisim.empty() ? std::string("-") : multisim) + " (" +
                                       std::to_string(slot_count) + " khe)";
    items.push_back(makeItem("Loại modem / khe SIM", boot_baseband == "apq" ? Status::Fail : Status::Pass,
                             "getprop ro.boot.baseband / persist.radio.multisim.config",
                             boot_baseband == "apq" ? modem_evidence + "  -> board chạy KHÔNG có modem (APQ)."
                                                    : modem_evidence,
                             ts));

    const std::string baseband = value(parsed, "gsm.version.baseband");
    const bool no_baseband = digitsOnly(baseband).empty() && trim(splitSlots(baseband)[0]).empty();
    items.push_back(makeItem("Baseband", no_baseband || baseband == "unknown" ? Status::Fail : Status::Pass,
                             "getprop gsm.version.baseband",
                             no_baseband || baseband == "unknown"
                                 ? "Không đọc được phiên bản baseband (No baseband): kiểm tra IC modem, nguồn modem, "
                                   "phân vùng EFS/modem."
                                 : baseband,
                             ts));

    // IMEI: blocked for ADB on Android 10+ unless the vendor exposes it.
    const std::vector<std::string> imeis = imeiCandidates(parsed.imei_lines);
    const char *imei_source = "service call iphonesubinfo / getprop *imei*";
    if (imeis.empty()) {
        items.push_back(makeItem("IMEI", Status::Unsupported, imei_source,
                                 "Không đọc được IMEI qua ADB (Android 10+ chặn). Kiểm tra bằng *#06#.", ts));
    }
    for (size_t i = 0; i < imeis.size(); ++i) {
        const std::string label = imeis.size() > 1 ? "IMEI " + std::to_string(i + 1) : "IMEI";
        const bool null_imei = imeis[i].find_first_not_of('0') == std::string::npos;
        const bool valid = !null_imei && luhnValid(imeis[i]);
        items.push_back(makeItem(label.c_str(), valid ? Status::Pass : Status::Fail, imei_source,
                                 imeis[i] + (null_imei ? "  -> IMEI NULL: hỏng EFS/NV, cần phục hồi."
                                             : valid  ? "  (checksum hợp lệ)"
                                                      : "  -> IMEI sai checksum: bị sửa/hỏng EFS."),
                                 ts));
    }

    // Per slot: SIM, then network, signal and band from the matching telephony phone.
    const std::vector<PhoneRadio> phones = phonesFromRegistry(parsed.telephony_lines);
    const std::vector<std::string> sims = splitSlots(value(parsed, "gsm.sim.state"));
    slot_count = std::max(slot_count, std::max(sims.size(), phones.size()));
    bool any_ready = false;
    for (const std::string &sim : sims) {
        any_ready = any_ready || sim.find("READY") != std::string::npos || sim.find("LOADED") != std::string::npos;
    }
    const std::string mobile_data = value(parsed, "mdata");
    for (size_t slot = 0; slot < slot_count && slot < 3; ++slot) {
        const std::string n = std::to_string(slot + 1);
        const std::string sim = slot < sims.size() ? sims[slot] : "";
        const bool ready = sim.find("READY") != std::string::npos || sim.find("LOADED") != std::string::npos;
        const std::string numeric = slotValue(parsed, "gsm.sim.operator.numeric", slot);
        const std::string sim_operator = slotValue(parsed, "gsm.sim.operator.alpha", slot);
        std::string evidence = (sim.empty() ? std::string("(trống)") : sim);
        if (ready) {
            evidence += ", MCC/MNC " + (numeric.empty() ? std::string("-") : numeric) + ", SIM của " +
                        (sim_operator.empty() ? std::string("-") : sim_operator);
        }
        Status sim_status = Status::Pass;
        if (!ready) {
            // One working slot makes an empty second slot normal.
            sim_status = any_ready ? Status::Unsupported : Status::Check;
            evidence += any_ready ? "  -> khe trống" : "  -> không có SIM hoặc không đọc được SIM (khay/chân SIM, IC SIM).";
        }
        items.push_back(makeItem(("SIM " + n).c_str(), sim_status, "getprop gsm.sim.state / gsm.sim.operator.*",
                                 evidence, ts));
        if (!ready) {
            continue;
        }
        const PhoneRadio phone = slot < phones.size() ? phones[slot] : PhoneRadio{};
        items.push_back(serviceItem(("Mạng SIM " + n).c_str(), phone, airplane, mobile_data, ts));
        items.push_back(signalItem(("Sóng SIM " + n).c_str(), phone, airplane, ts));
        items.push_back(bandItem(("Băng tần SIM " + n).c_str(), phone, ts));
    }

    // Recent modem errors in the radio log (a restarting modem shows up here).
    const char *log_source = "logcat -b radio (RADIO_NOT_AVAILABLE, modem crash/reset)";
    if (parsed.radio_errors < 0) {
        items.push_back(makeItem("Log modem", Status::Unsupported, log_source, "Không đọc được log radio.", ts));
    } else if (parsed.radio_errors == 0) {
        items.push_back(makeItem("Log modem", Status::Pass, log_source,
                                 "Không thấy lỗi modem trong log radio gần đây.", ts));
    } else {
        std::string evidence = std::to_string(parsed.radio_errors) +
                               " dòng lỗi (bật/tắt chế độ máy bay cũng sinh RADIO_NOT_AVAILABLE).";
        for (const std::string &line : parsed.radio_log) {
            evidence += "\n    " + line;
        }
        items.push_back(makeItem("Log modem", Status::Check, log_source, evidence, ts));
    }
    return finish("BASEBAND / SIM", std::move(items));
}

std::string lower(std::string text)
{
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// Value of the first key whose lower-case name contains `needle` (vendor keys vary in case).
std::string valueLike(const Parsed &parsed, const char *needle)
{
    for (const auto &entry : parsed.values) {
        if (!entry.second.empty() && lower(entry.first).find(needle) != std::string::npos) {
            return entry.second;
        }
    }
    return "";
}

bool isSignedNumber(const std::string &text)
{
    return isNumber(text[0] == '-' ? text.substr(1) : text);
}

// First integer in `line` after `key` (case-insensitive), e.g. "Min learned battery capacity: 4100 mAh".
bool intAfterLike(const std::string &line, const char *key, long long *out)
{
    const std::string low = lower(line);
    const size_t pos = low.find(key);
    if (pos == std::string::npos) {
        return false;
    }
    const size_t digit = line.find_first_of("0123456789", pos + strlen(key));
    if (digit == std::string::npos) {
        return false;
    }
    *out = atoll(line.c_str() + digit);
    return true;
}

std::string fmt1(double value)
{
    char text[32];
    snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

Item capacityItem(const Parsed &parsed, uint32_t ts)
{
    // Candidate sources, best first; every one found is listed as evidence.
    std::string evidence;
    auto note = [&evidence](const std::string &text) { evidence += (evidence.empty() ? "" : "; ") + text; };
    int percent = -1;
    bool rough = false;
    const char *source = "";

    // Samsung: ASOC is its own state-of-health percentage.
    const std::string asoc = valueLike(parsed, "asoc");
    if (isNumber(asoc) && atoi(asoc.c_str()) > 0 && atoi(asoc.c_str()) <= 120) {
        percent = atoi(asoc.c_str());
        source = "dumpsys battery (Samsung ASOC)";
        note("Samsung ASOC " + asoc + "%");
    }
    const std::string bsoh = valueLike(parsed, "bsoh");
    if (!bsoh.empty()) {
        note("BSOH " + bsoh);
    }
    const std::string first_use = valueLike(parsed, "first");
    if (!first_use.empty()) {
        note("dùng lần đầu " + first_use);
    }

    // Fuel gauge full-charge capacity vs design (uAh).
    const std::string full = value(parsed, "charge_full");
    const std::string design = value(parsed, "charge_full_design");
    long long design_mah = isNumber(design) ? atoll(design.c_str()) / 1000 : 0;
    if (isNumber(full) && design_mah > 0) {
        const long long full_mah = atoll(full.c_str()) / 1000;
        const int p = static_cast<int>(full_mah * 100 / design_mah);
        note("charge_full " + std::to_string(full_mah) + " / " + std::to_string(design_mah) + " mAh = " +
             std::to_string(p) + "%");
        if (percent < 0) {
            percent = p;
            source = "/sys/class/power_supply/battery (charge_full)";
        }
    }

    // batterystats: design "Capacity" and the learned / estimated capacity (mAh).
    long long stats_design = 0;
    long long learned_min = 0;
    long long learned_max = 0;
    long long estimated = 0;
    for (const std::string &line : parsed.battery_stats) {
        long long v = 0;
        if (intAfterLike(line, "min learned battery capacity", &v)) {
            learned_min = v;
        } else if (intAfterLike(line, "max learned battery capacity", &v)) {
            learned_max = v;
        } else if (intAfterLike(line, "estimated battery capacity", &v)) {
            estimated = v;
        } else if (intAfterLike(line, "capacity:", &v)) {
            stats_design = v;
        }
    }
    auto to_mah = [](long long v) { return v > 20000 ? v / 1000 : v; }; // some ROMs print uAh
    learned_min = to_mah(learned_min);
    learned_max = to_mah(learned_max);
    estimated = to_mah(estimated);
    if (design_mah <= 0) {
        design_mah = stats_design;
    }
    const long long learned = learned_min > 0 && learned_max > 0 ? (learned_min + learned_max) / 2
                              : learned_max > 0                  ? learned_max
                                                                 : learned_min;
    if (stats_design > 0) {
        note("thiết kế " + std::to_string(stats_design) + " mAh (batterystats)");
    }
    if (learned > 0 && design_mah > 0) {
        const int p = static_cast<int>(learned * 100 / design_mah);
        note("học được " + std::to_string(learned_min) + "-" + std::to_string(learned_max) + " mAh = " +
             std::to_string(p) + "%");
        if (percent < 0) {
            percent = p;
            source = "dumpsys batterystats (learned capacity)";
        }
    } else if (estimated > 0) {
        note("ước tính " + std::to_string(estimated) + " mAh");
    }

    // Last resort: charge counter scaled by the level (rough; only with enough charge in).
    const std::string counter = valueLike(parsed, "charge counter");
    const int level = atoi(value(parsed, "level").c_str());
    if (isNumber(counter) && atoll(counter.c_str()) > 0 && level >= 30) {
        const long long full_est = atoll(counter.c_str()) / 1000 * 100 / level;
        note("charge counter " + std::to_string(atoll(counter.c_str()) / 1000) + " mAh ở " + std::to_string(level) +
             "% -> đầy ~" + std::to_string(full_est) + " mAh");
        if (percent < 0 && design_mah > 0) {
            // The counter drifts; above 100% only means "no measurable wear".
            percent = static_cast<int>(std::min<long long>(100, full_est * 100 / design_mah));
            source = "dumpsys battery (charge counter, ước tính thô)";
            rough = true;
        }
    }

    if (percent < 0 || percent > 130) {
        return makeItem("Sức khoẻ pin (dung lượng)", Status::Unsupported, "dumpsys battery / batterystats / sysfs",
                        evidence.empty() ? "Máy không cho đọc dung lượng pin qua ADB." : evidence, ts);
    }
    Status status = percent >= 80 ? Status::Pass : (percent >= 60 && !rough ? Status::Check : Status::Fail);
    if (rough && status == Status::Fail) {
        status = Status::Check; // a rough estimate never fails the battery on its own
    }
    std::string verdict = percent >= 80 ? "  -> pin còn tốt" : percent >= 60 ? "  -> pin chai, cân nhắc thay"
                                                                              : "  -> pin chai nặng, nên thay";
    return makeItem("Sức khoẻ pin (dung lượng)", status, source,
                    std::to_string(percent) + "%" + (rough ? " (ước tính thô)" : "") + verdict + ". " + evidence, ts);
}

Group checkBattery(const Parsed &parsed, uint32_t ts)
{
    std::vector<Item> items;
    const char *source = "dumpsys battery";

    const std::string present = value(parsed, "present");
    if (present == "false") {
        items.push_back(makeItem("Pin", Status::Fail, source, "present: false -> máy không nhận pin.", ts));
    }

    const std::string health = value(parsed, "health");
    static const char *const kHealth[] = {"?", "Unknown", "Good", "Overheat", "Dead", "Over voltage",
                                          "Unspecified failure", "Cold"};
    const int health_code = atoi(health.c_str());
    const char *health_name = health_code >= 1 && health_code <= 7 ? kHealth[health_code] : "?";
    const std::string technology = value(parsed, "technology");
    items.push_back(makeItem("Health", health_code == 2 ? Status::Pass : (health_code <= 1 ? Status::Check
                                                                                            : Status::Fail),
                             source,
                             std::string(health_name) + " (health=" + (health.empty() ? "-" : health) +
                                 "), level " + (value(parsed, "level").empty() ? "-" : value(parsed, "level")) + "%" +
                                 (technology.empty() ? "" : ", " + technology),
                             ts));

    items.push_back(capacityItem(parsed, ts));

    // Cycle count: sysfs, else a vendor/Android 14 "cycle" key in dumpsys battery.
    std::string cycles = value(parsed, "cycle_count");
    const char *cycle_source = "/sys/class/power_supply/battery/cycle_count";
    if (!isNumber(cycles)) {
        cycles = valueLike(parsed, "cycle");
        cycle_source = "dumpsys battery";
    }
    if (isNumber(cycles)) {
        const int count = atoi(cycles.c_str());
        items.push_back(makeItem("Chu kỳ sạc", count > 800 ? Status::Check : Status::Pass, cycle_source,
                                 std::to_string(count) + (count > 800 ? " -> nhiều chu kỳ" : ""), ts));
    } else {
        items.push_back(makeItem("Chu kỳ sạc", Status::Unsupported, cycle_source,
                                 "Máy không cho đọc số chu kỳ sạc qua ADB.", ts));
    }

    // Charging while plugged into the board (which powers VBUS).
    const int status_code = atoi(value(parsed, "status").c_str());
    static const char *const kStatus[] = {"?", "Unknown", "Đang sạc", "Đang xả", "Không sạc", "Đầy"};
    const char *status_name = status_code >= 1 && status_code <= 5 ? kStatus[status_code] : "?";
    std::string sources;
    for (const char *name : {"AC powered", "USB powered", "Wireless powered", "Dock powered"}) {
        if (value(parsed, name) == "true") {
            sources += (sources.empty() ? "" : "+") + std::string(name).substr(0, std::string(name).find(' '));
        }
    }
    std::string charge_evidence = std::string(status_name) + ", nguồn: " + (sources.empty() ? "không" : sources);
    const std::string max_current = value(parsed, "Max charging current");
    const std::string max_voltage = value(parsed, "Max charging voltage");
    if (isNumber(max_current) && atoll(max_current.c_str()) > 0) {
        charge_evidence += ", sạc cho phép tối đa " + fmt1(atoll(max_current.c_str()) / 1e6) + " A";
        if (isNumber(max_voltage) && atoll(max_voltage.c_str()) > 0) {
            charge_evidence += " / " + fmt1(atoll(max_voltage.c_str()) / 1e6) + " V";
        }
    }
    const int level = atoi(value(parsed, "level").c_str());
    Status charge_status = Status::Pass;
    if (sources.empty()) {
        charge_status = Status::Check;
        charge_evidence += "  -> máy không nhận nguồn qua cổng USB: nghi cổng sạc/IC sạc (hoặc board không cấp VBUS).";
    } else if (status_code == 4 && level < 95) {
        charge_status = Status::Check;
        charge_evidence += "  -> có nguồn nhưng KHÔNG sạc: nghi IC sạc, pin, hoặc máy đang bật giới hạn sạc bảo vệ pin.";
    }
    items.push_back(makeItem("Sạc", status_code == 0 ? Status::Unsupported : charge_status, source, charge_evidence,
                             ts));

    // Instantaneous current; units (uA or mA) and sign convention depend on the vendor.
    const std::string current = value(parsed, "current_now");
    if (isSignedNumber(current)) {
        long long ma = atoll(current.c_str());
        if (ma > 10000 || ma < -10000) {
            ma /= 1000;
        }
        const long long magnitude = ma < 0 ? -ma : ma;
        std::string evidence = std::to_string(ma) + " mA (dấu +/- tuỳ hãng)";
        Status status = Status::Pass;
        if (status_code == 2 && magnitude < 50 && level < 95) {
            status = Status::Check;
            evidence += "  -> đang sạc nhưng dòng gần 0: nghi IC sạc/cổng sạc/pin.";
        } else if (status_code == 3 && magnitude > 1500) {
            status = Status::Check;
            evidence += "  -> xả rất lớn khi cắm máy: nghi chạm/rò trên main hoặc app chạy nền.";
        }
        items.push_back(makeItem("Dòng điện", status, "/sys/class/power_supply/battery/current_now", evidence, ts));
    } else {
        items.push_back(makeItem("Dòng điện", Status::Unsupported, "/sys/class/power_supply/battery/current_now",
                                 "Máy không cho đọc dòng điện qua ADB.", ts));
    }

    const std::string temperature = value(parsed, "temperature");
    if (isNumber(temperature)) {
        const int tenths = atoi(temperature.c_str());
        char text[64];
        snprintf(text, sizeof(text), "%d.%d C", tenths / 10, tenths % 10);
        items.push_back(makeItem("Nhiệt độ pin", tenths > 450 ? Status::Check : Status::Pass, source,
                                 std::string(text) + (tenths > 450 ? " -> nóng bất thường" : ""), ts));
    }

    const std::string voltage = value(parsed, "voltage");
    if (isNumber(voltage)) {
        const int mv = atoi(voltage.c_str());
        const bool abnormal = mv < 3300 || mv > 4500;
        items.push_back(makeItem("Điện áp", abnormal ? Status::Check : Status::Pass, source,
                                 std::to_string(mv) + " mV" + (abnormal ? " -> ngoài khoảng 3300-4500 mV" : ""),
                                 ts));
    }

    // Board temperatures: charger/USB/PMIC/battery zones point at the part that heats up.
    std::vector<std::pair<double, std::string>> zones;
    for (const std::string &line : parsed.thermal) {
        const size_t eq = line.rfind('=');
        if (eq == std::string::npos || eq == 0 || !isSignedNumber(line.substr(eq + 1))) {
            continue;
        }
        const long long raw = atoll(line.c_str() + eq + 1);
        const double celsius = raw >= 1000 || raw <= -1000 ? raw / 1000.0 : raw >= 200 ? raw / 10.0 : raw;
        if (celsius > -30 && celsius < 150 && celsius != 0) {
            zones.emplace_back(celsius, line.substr(0, eq));
        }
    }
    if (zones.empty()) {
        items.push_back(makeItem("Nhiệt độ linh kiện", Status::Unsupported, "/sys/class/thermal",
                                 "Máy không cho đọc thermal zone qua ADB.", ts));
    } else {
        std::sort(zones.begin(), zones.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::string hot;
        for (const auto &zone : zones) {
            const std::string type = lower(zone.second);
            const bool power_part = type.find("chg") != std::string::npos || type.find("charg") != std::string::npos ||
                                    type.find("usb") != std::string::npos || type.find("pmic") != std::string::npos ||
                                    type.find("batt") != std::string::npos || type.find("bms") != std::string::npos;
            if ((power_part && zone.first > 50) || zone.first > 80) {
                hot += (hot.empty() ? "" : ", ") + zone.second + " " + fmt1(zone.first) + " C";
            }
        }
        std::string evidence = "Nóng nhất: ";
        for (size_t i = 0; i < zones.size() && i < 6; ++i) {
            evidence += (i == 0 ? "" : ", ") + zones[i].second + " " + fmt1(zones[i].first) + " C";
        }
        evidence += " (" + std::to_string(zones.size()) + " vùng)";
        if (!hot.empty()) {
            evidence += "  -> nóng bất thường: " + hot + " (IC sạc/cổng USB/PMIC nóng: nghi chạm, rò).";
        }
        items.push_back(makeItem("Nhiệt độ linh kiện", hot.empty() ? Status::Pass : Status::Check,
                                 "/sys/class/thermal/thermal_zone*", evidence, ts));
    }
    return finish("BATTERY", std::move(items));
}

// ---- CAMERA ----

struct CameraInfo {
    std::string id;
    std::string facing;  // "back", "front", "external" or ""
    int flash = -1;      // 1 / 0, -1 unknown
    long long width = 0; // sensor pixel array
    long long height = 0;
};

Group checkCamera(const Parsed &parsed, uint32_t ts)
{
    const char *source = "dumpsys media.camera";
    std::vector<CameraInfo> cameras;
    long long service_count = -1;
    bool expect_pixel_array = false;
    for (const std::string &line : parsed.camera_lines) {
        const std::string low = lower(line);
        long long number = 0;
        if (expect_pixel_array && line[0] == '[') {
            expect_pixel_array = false;
            if (!cameras.empty()) {
                const char *p = line.c_str() + 1;
                char *end = nullptr;
                cameras.back().width = strtoll(p, &end, 10);
                cameras.back().height = strtoll(end, nullptr, 10);
            }
            continue;
        }
        expect_pixel_array = false;
        if (low.find("number of camera devices") != std::string::npos && intAfterLike(line, "devices", &number)) {
            service_count = number;
        } else if (low.find("== camera hal device") != std::string::npos ||
                   (low.find("static information") != std::string::npos && low.find("camera") != std::string::npos)) {
            // "== Camera HAL device device@3.6/internal/2 (v3.6) static information: ==" -> id "2"
            CameraInfo camera;
            const size_t paren = line.find(" (");
            const size_t slash = line.rfind('/', paren);
            if (slash != std::string::npos && paren != std::string::npos && paren > slash) {
                camera.id = line.substr(slash + 1, paren - slash - 1);
            } else {
                camera.id = std::to_string(cameras.size());
            }
            cameras.push_back(camera);
        } else if (!cameras.empty() && low.rfind("facing:", 0) == 0) {
            cameras.back().facing = trim(low.substr(strlen("facing:")));
        } else if (!cameras.empty() && low.rfind("has a flash unit:", 0) == 0) {
            cameras.back().flash = low.find("true") != std::string::npos ? 1 : 0;
        } else if (!cameras.empty() && low.find("pixelarraysize (") != std::string::npos) {
            expect_pixel_array = true;
        }
    }

    std::vector<Item> items;
    const bool declared_any = parsed.features.count("android.hardware.camera.any") > 0 ||
                              parsed.features.count("android.hardware.camera") > 0 ||
                              parsed.features.count("android.hardware.camera.front") > 0;
    if (parsed.camera_lines.empty()) {
        items.push_back(makeItem("Dịch vụ camera", Status::Unsupported, source,
                                 "Không đọc được dumpsys media.camera.", ts));
        return finish("CAMERA", std::move(items));
    }
    if (cameras.empty() && service_count <= 0) {
        items.push_back(makeItem("Dịch vụ camera", declared_any ? Status::Fail : Status::Unsupported, source,
                                 declared_any ? "Máy có camera nhưng dịch vụ camera không thấy camera nào: nghi nguồn "
                                                "camera (LDO/PMIC), cáp, hoặc tất cả module camera hỏng."
                                              : "Máy không khai báo camera.",
                                 ts));
        return finish("CAMERA", std::move(items));
    }
    items.push_back(makeItem("Dịch vụ camera", Status::Pass, source,
                             std::to_string(service_count < 0 ? 0 : service_count) + " camera cho ứng dụng, " +
                                 std::to_string(cameras.size()) +
                                 " camera phần cứng (kể cả camera con của cụm nhiều camera). So với thông số "
                                 "của model để biết có thiếu camera con nào không.",
                             ts));

    auto describe = [](const CameraInfo &camera) {
        std::string text = camera.facing == "back"    ? "Sau"
                           : camera.facing == "front" ? "Trước"
                           : camera.facing.empty()    ? "?"
                                                      : camera.facing;
        if (camera.width > 0 && camera.height > 0) {
            text += ", " + fmt1(camera.width * camera.height / 1e6) + " MP (" + std::to_string(camera.width) + "x" +
                    std::to_string(camera.height) + ")";
        }
        if (camera.flash == 1) {
            text += ", có flash";
        }
        return text;
    };

    // Declared (static config) vs what the camera HAL actually brought up.
    struct Side {
        const char *label;
        const char *facing;
        const char *feature;
    } kSides[] = {
        {"Camera sau", "back", "android.hardware.camera"},
        {"Camera trước", "front", "android.hardware.camera.front"},
    };
    bool back_flash = false;
    for (const auto &side : kSides) {
        std::string found;
        for (const CameraInfo &camera : cameras) {
            if (camera.facing == side.facing) {
                found += (found.empty() ? "" : "; ") + std::string("ID ") + camera.id + ": " + describe(camera);
                back_flash = back_flash || (camera.flash == 1 && strcmp(side.facing, "back") == 0);
            }
        }
        const bool declared = parsed.features.count(side.feature) > 0;
        if (!found.empty()) {
            items.push_back(makeItem(side.label, Status::Pass, source, found, ts));
        } else if (declared) {
            items.push_back(makeItem(side.label, Status::Fail, "dumpsys media.camera + pm list features",
                                     std::string("Máy khai báo ") + side.feature +
                                         " nhưng HAL không có camera này: nghi module camera, cáp/đầu nối, "
                                         "hoặc mất nguồn camera.",
                                     ts));
        } else {
            items.push_back(makeItem(side.label, Status::Unsupported, "pm list features",
                                     "Máy không khai báo camera này.", ts));
        }
    }

    if (parsed.features.count("android.hardware.camera.flash") > 0) {
        items.push_back(makeItem("Đèn flash", back_flash ? Status::Pass : Status::Check,
                                 "dumpsys media.camera (Has a flash unit)",
                                 back_flash ? "Camera sau báo có flash (chưa bật thử đèn)."
                                            : "Máy khai báo có flash nhưng camera sau không báo flash: nghi LED "
                                              "flash, IC flash hoặc cáp.",
                                 ts));
    } else {
        items.push_back(makeItem("Đèn flash", Status::Unsupported, "pm list features", "Máy không khai báo flash.",
                                 ts));
    }

    for (const CameraInfo &camera : cameras) {
        items.push_back(makeItem(("Camera ID " + camera.id).c_str(), Status::Pass, source,
                                 describe(camera) + " (HAL nhận; chưa chụp thử ảnh)", ts));
    }
    return finish("CAMERA", std::move(items));
}

} // namespace

const char *HCCAndroidDiagnostics::command()
{
    return kCommand;
}

HCCAndroidDiagnostics::FullResult HCCAndroidDiagnostics::parse(const char *output)
{
    const uint32_t ts = static_cast<uint32_t>(time(nullptr));
    const Parsed parsed = parseSections(output);
    FullResult result;
    // A group needs its own sections intact; a truncated tail only costs the groups it cut.
    auto has = [&parsed](const char *sections) {
        for (const char *s = sections; *s != '\0'; ++s) {
            if (parsed.done.count(*s) == 0) {
                return false;
            }
        }
        return true;
    };
    result.sensors = has("SF") ? checkSensors(parsed, ts) : notTested("SENSORS", ts);
    result.faceid = has("FPA") ? checkBiometrics(parsed, ts) : notTested("VÂN TAY / FACE", ts);
    result.baseband = has("FR") ? checkBaseband(parsed, ts) : notTested("BASEBAND / SIM", ts);
    result.battery = has("B") ? checkBattery(parsed, ts) : notTested("BATTERY", ts);
    result.camera = has("FK") ? checkCamera(parsed, ts) : notTested("CAMERA", ts);
    return result;
}
