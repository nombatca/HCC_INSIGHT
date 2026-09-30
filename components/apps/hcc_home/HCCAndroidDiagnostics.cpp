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

// Sections are introduced by "##X" lines; "##E" proves the output is complete.
constexpr char kCommand[] =
    "echo '##S';dumpsys sensorservice|grep -o 'android\\.sensor\\.[a-z_]*'|sort -u;"
    "echo '##F';pm list features|grep -E 'sensor|fingerprint|biometrics|telephony';"
    "echo '##P';dumpsys fingerprint 2>/dev/null|grep -ciE 'sensorid|sensor_id|sensorprops|prints';"
    "echo '##A';dumpsys face 2>/dev/null|grep -ciE 'sensorid|sensor_id|sensorprops';"
    "echo '##R';for p in gsm.version.baseband gsm.sim.state gsm.sim.operator.alpha gsm.sim.operator.numeric "
    "persist.radio.multisim.config ro.boot.baseband;do echo \"$p=$(getprop $p)\";done;"
    "echo \"airplane=$(settings get global airplane_mode_on)\";echo \"mdata=$(settings get global mobile_data)\";"
    "echo '##T';dumpsys telephony.registry|grep -E '^ *(Phone Id=|mServiceState=|mSignalStrength=|mCellInfo=)'"
    "|cut -c1-900;"
    "echo '##L';x=$(logcat -b radio -d -t 3000 2>/dev/null|grep -iE "
    "'RADIO_NOT_AVAILABLE|modem.{0,20}(crash|reset|restart)|subsystem restart');"
    "echo \"n=$(echo \"$x\"|grep -c .)\";echo \"$x\"|tail -2|cut -c1-160;"
    "echo '##I';service call iphonesubinfo 1 s16 com.android.shell 2>&1|cut -c1-80;getprop|grep -i imei|cut -c1-80;"
    "echo '##B';dumpsys battery;d=/sys/class/power_supply/battery;"
    "echo \"cycle=$(cat $d/cycle_count 2>/dev/null)\";echo \"full=$(cat $d/charge_full 2>/dev/null)\";"
    "echo \"design=$(cat $d/charge_full_design 2>/dev/null)\";"
    "echo '##E'";

struct Parsed {
    bool complete = false;
    std::set<std::string> sensors;  // "accelerometer", "proximity", ...
    std::set<std::string> features; // "android.hardware.fingerprint", ...
    int fingerprint_lines = -1;
    int face_lines = -1;
    std::map<std::string, std::string> values; // radio "key=value" and battery "key: value"
    std::vector<std::string> telephony_lines;   // dumpsys telephony.registry (filtered)
    int radio_errors = -1;                      // modem error lines in the radio log; -1 = not read
    std::vector<std::string> radio_log;         // last matching lines
    std::vector<std::string> imei_lines;        // service call / getprop output
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
    items.push_back(makeItem("Health", health_code == 2 ? Status::Pass : (health_code <= 1 ? Status::Check
                                                                                            : Status::Fail),
                             source,
                             std::string(health_name) + " (health=" + (health.empty() ? "-" : health) +
                                 "), level " + (value(parsed, "level").empty() ? "-" : value(parsed, "level")) + "%",
                             ts));

    const std::string temperature = value(parsed, "temperature");
    if (isNumber(temperature)) {
        const int tenths = atoi(temperature.c_str());
        char text[64];
        snprintf(text, sizeof(text), "%d.%d C", tenths / 10, tenths % 10);
        items.push_back(makeItem("Nhiệt độ", tenths > 450 ? Status::Check : Status::Pass, source,
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

    const std::string full = value(parsed, "full");
    const std::string design = value(parsed, "design");
    if (isNumber(full) && isNumber(design) && atoll(design.c_str()) > 0) {
        const long long percent = atoll(full.c_str()) * 100 / atoll(design.c_str());
        items.push_back(makeItem("Dung lượng còn lại", percent < 80 ? Status::Check : Status::Pass,
                                 "/sys/class/power_supply/battery",
                                 std::to_string(percent) + "% (" + full + " / " + design + " uAh)" +
                                     (percent < 80 ? " -> pin chai, nên thay" : ""),
                                 ts));
    } else {
        items.push_back(makeItem("Dung lượng còn lại", Status::Unsupported, "/sys/class/power_supply/battery",
                                 "Máy không cho đọc charge_full/charge_full_design qua ADB.", ts));
    }

    const std::string cycles = value(parsed, "cycle");
    if (isNumber(cycles)) {
        const int count = atoi(cycles.c_str());
        items.push_back(makeItem("Chu kỳ sạc", count > 800 ? Status::Check : Status::Pass,
                                 "/sys/class/power_supply/battery/cycle_count",
                                 std::to_string(count) + (count > 800 ? " -> nhiều chu kỳ" : ""), ts));
    } else {
        items.push_back(makeItem("Chu kỳ sạc", Status::Unsupported, "/sys/class/power_supply/battery/cycle_count",
                                 "Máy không cho đọc cycle_count qua ADB.", ts));
    }
    return finish("BATTERY", std::move(items));
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
    if (!parsed.complete) {
        result.sensors = notTested("SENSORS", ts);
        result.faceid = notTested("VÂN TAY / FACE", ts);
        result.baseband = notTested("BASEBAND / SIM", ts);
        result.battery = notTested("BATTERY", ts);
        return result;
    }
    result.sensors = checkSensors(parsed, ts);
    result.faceid = checkBiometrics(parsed, ts);
    result.baseband = checkBaseband(parsed, ts);
    result.battery = checkBattery(parsed, ts);
    return result;
}
