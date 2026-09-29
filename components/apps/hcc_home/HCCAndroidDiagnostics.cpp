#include "HCCAndroidDiagnostics.hpp"

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
    "echo '##R';echo \"baseband=$(getprop gsm.version.baseband)\";echo \"sim=$(getprop gsm.sim.state)\";"
    "echo \"operator=$(getprop gsm.operator.alpha)\";echo \"network=$(getprop gsm.network.type)\";"
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

Group checkBaseband(const Parsed &parsed, uint32_t ts)
{
    std::vector<Item> items;
    if (parsed.features.count("android.hardware.telephony") == 0) {
        items.push_back(makeItem("Telephony", Status::Unsupported, "pm list features",
                                 "Máy không có chức năng di động (tablet Wi-Fi).", ts));
        return finish("BASEBAND / SIM", std::move(items));
    }

    const std::string baseband = value(parsed, "baseband");
    const bool no_baseband = baseband.empty() || baseband == "unknown" || baseband == ",";
    items.push_back(makeItem("Baseband", no_baseband ? Status::Fail : Status::Pass, "getprop gsm.version.baseband",
                             no_baseband ? "Không đọc được phiên bản baseband (No baseband): kiểm tra IC modem, "
                                           "nguồn modem, phân vùng EFS/modem."
                                         : baseband,
                             ts));

    const std::string sim = value(parsed, "sim");
    Status sim_status = Status::Check;
    if (sim.find("READY") != std::string::npos || sim.find("LOADED") != std::string::npos) {
        sim_status = Status::Pass;
    }
    items.push_back(makeItem("SIM", sim_status, "getprop gsm.sim.state",
                             (sim.empty() ? std::string("(trống)") : sim) +
                                 (sim_status == Status::Pass ? "" : "  -> không có SIM hoặc chưa đọc được SIM "
                                                                    "(khay/chân SIM, IC SIM)."),
                             ts));

    const std::string op = value(parsed, "operator");
    const std::string network = value(parsed, "network");
    const bool registered = !op.empty() && op != ",";
    items.push_back(makeItem("Mạng", registered ? Status::Pass : (sim_status == Status::Pass ? Status::Check
                                                                                           : Status::Unsupported),
                             "getprop gsm.operator.alpha / gsm.network.type",
                             "Nhà mạng: " + (registered ? op : std::string("-")) +
                                 ", loại mạng: " + (network.empty() ? std::string("-") : network) +
                                 (registered || sim_status != Status::Pass ? ""
                                                                           : "  -> có SIM nhưng không đăng ký "
                                                                             "được mạng (RF/ăng-ten/IMEI)."),
                             ts));
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
