#include "HCCRffe.hpp"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "hcc_cores.h"
#include "HCCUsbDisk.hpp"
#include "rffe_jc_database.h"

#ifndef CONFIG_HCC_RFFE_SCLK_GPIO
#define CONFIG_HCC_RFFE_SCLK_GPIO -1
#endif
#ifndef CONFIG_HCC_RFFE_SDATA_GPIO
#define CONFIG_HCC_RFFE_SDATA_GPIO -1
#endif
#ifndef CONFIG_HCC_RFFE_HALF_PERIOD_US
#define CONFIG_HCC_RFFE_HALF_PERIOD_US 5
#endif

namespace HCCRffe {

static const char *TAG = "HCCRffe";
static const char *RFFE_DIR = BSP_SD_MOUNT_POINT "/rffe";

static const int PIN_SCLK = CONFIG_HCC_RFFE_SCLK_GPIO;
static const int PIN_SDATA = CONFIG_HCC_RFFE_SDATA_GPIO;
static const uint32_t HALF_US = CONFIG_HCC_RFFE_HALF_PERIOD_US;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_gpio_ready = false;
// Chân đang dùng cho SCLK / SDATA. Nối ngược hai dây thì lần quét tự thử đảo chiều; chiều nào tìm thấy USID
// được giữ cho lần quét sau.
static int s_pin_clk = PIN_SCLK;
static int s_pin_dat = PIN_SDATA;
static volatile bool s_busy = false;
static volatile uint32_t s_serial = 0;
static Report s_report;
static std::vector<RefEntry> s_scan_ref;

// Chân đang được board dùng cho việc khác: chọn trùng là phá chức năng đó.
static bool pinBusy(int pin)
{
#ifdef CONFIG_ESP_HOSTED_SDIO_PIN_CLK
    // SDIO tới chip Wi-Fi ESP32-C6 (esp_hosted).
    const int sdio[] = {CONFIG_ESP_HOSTED_SDIO_PIN_CLK, CONFIG_ESP_HOSTED_SDIO_PIN_CMD, CONFIG_ESP_HOSTED_SDIO_PIN_D0,
#ifdef CONFIG_ESP_HOSTED_SDIO_PIN_D1
                        CONFIG_ESP_HOSTED_SDIO_PIN_D1,
#endif
#ifdef CONFIG_ESP_HOSTED_SDIO_PIN_D2
                        CONFIG_ESP_HOSTED_SDIO_PIN_D2,
#endif
#ifdef CONFIG_ESP_HOSTED_SDIO_PIN_D3
                        CONFIG_ESP_HOSTED_SDIO_PIN_D3,
#endif
                        CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE};
    for (int p : sdio) {
        if (p == pin) {
            return true;
        }
    }
#endif
    return false;
}

const char *configError(void)
{
    if (PIN_SCLK < 0 || PIN_SDATA < 0) {
        return "Chưa cấu hình chân SCLK / SDATA (menuconfig > HCC RFFE)";
    }
    if (PIN_SCLK == PIN_SDATA) {
        return "SCLK và SDATA trùng chân";
    }
    if (pinBusy(PIN_SCLK) || pinBusy(PIN_SDATA)) {
        return "Chân RFFE trùng SDIO Wi-Fi (ESP32-C6) - chọn chân khác trong menuconfig";
    }
    return nullptr;
}

bool configured(void)
{
    return configError() == nullptr;
}

int sclkPin(void)
{
    return s_pin_clk;
}

int sdataPin(void)
{
    return s_pin_dat;
}

bool pinsSwapped(void)
{
    return s_pin_clk != PIN_SCLK;
}

// ==================== Thẻ SD ====================

static bool sdReady(void)
{
    if (HCCUsbDisk::state() != HCCUsbDisk::State::Off) {
        return false;  // thẻ đang giao cho máy tính (USB DISK)
    }
    const esp_err_t ret = bsp_sdcard_mount();
    return ret == ESP_OK || ret == ESP_ERR_INVALID_STATE;  // INVALID_STATE: đã mount nơi khác
}

static int listDir(const char *path, bool want_dirs, const char *ext, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!sdReady()) {
        return -1;
    }
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        return -1;
    }
    std::vector<std::string> names;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.' || (ent->d_type == DT_DIR) != want_dirs) {
            continue;
        }
        if (ext != nullptr) {
            const size_t nl = strlen(ent->d_name), el = strlen(ext);
            if (nl <= el || strcasecmp(ent->d_name + nl - el, ext) != 0) {
                continue;
            }
        }
        names.emplace_back(ent->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    size_t used = 0;
    int count = 0;
    for (const std::string &n : names) {
        const size_t need = n.size() + (count ? 1 : 0);
        if (used + need + 1 > out_size) {
            break;
        }
        used += snprintf(out + used, out_size - used, "%s%s", count ? "\n" : "", n.c_str());
        ++count;
    }
    return count;
}

// ==================== Dữ liệu JC BUS2MM4 (model -> đường RFFE -> linh kiện -> USID) ====================
// Bảng người dùng trích từ JC BUS2MM4, nhúng sẵn trong firmware (rffe_jc_database.h). Nếu thẻ SD có
// /sdcard/rffe/JC_BUS2MM4_iPhone_RFFE_USID.csv thì dùng file đó (cập nhật không cần build lại). Dùng nguyên giá
// trị, không suy đoán thêm. Dữ liệu chỉ có USID (không có giá trị thanh ghi) nên mỗi linh kiện được kiểm bằng
// thanh ghi 0x1F: bit[3:0] phải bằng USID (đúng điều kiện probeUsid() dùng để nhận thiết bị).

static const char *JC_FILE = "JC_BUS2MM4_iPhone_RFFE_USID.csv";

struct JcEntry {
    const char *model;
    const char *line;
    const char *part;
    uint8_t usid;
};

static std::vector<JcEntry> s_jc;
static char *s_jc_buf = nullptr;  // nội dung file trên thẻ (các trường trỏ vào đây); nullptr = bảng firmware
static bool s_jc_from_sd = false;
static off_t s_jc_size = -1;
static time_t s_jc_mtime = 0;

static void useBuiltinJc(void)
{
    if (!s_jc_from_sd && !s_jc.empty()) {
        return;
    }
    s_jc.clear();
    free(s_jc_buf);
    s_jc_buf = nullptr;
    s_jc_from_sd = false;
    s_jc_size = -1;
    s_jc.reserve(HccJcRffeDb::kEntryCount);
    for (size_t i = 0; i < HccJcRffeDb::kEntryCount; ++i) {
        const HccJcRffeDb::Entry &e = HccJcRffeDb::kEntries[i];
        s_jc.push_back({e.model_id, e.rffe_line, e.device_part, e.usid});
    }
    ESP_LOGI(TAG, "Bảng JC trong firmware: %d dòng", (int)s_jc.size());
}

// Tách một dòng CSV (RFC 4180): trường có thể nằm trong "..." với "" là một dấu ". Sửa tại chỗ.
static int splitCsv(char *s, char **fields, int max)
{
    int n = 0;
    while (n < max) {
        char *out = s;
        fields[n++] = out;
        if (*s == '"') {
            ++s;
            while (*s) {
                if (*s == '"' && s[1] == '"') {
                    *out++ = '"';
                    s += 2;
                } else if (*s == '"') {
                    ++s;
                    break;
                } else {
                    *out++ = *s++;
                }
            }
        }
        while (*s && *s != ',') {
            *out++ = *s++;
        }
        const bool more = (*s == ',');
        *out = '\0';
        if (!more) {
            break;
        }
        s += 1;
    }
    return n;
}

static void trimRight(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace(static_cast<unsigned char>(s[n - 1]))) {
        s[--n] = '\0';
    }
}

// Dùng file trên thẻ nếu có và hợp lệ (giữ cache khi file không đổi), nếu không thì bảng trong firmware.
static void loadJc(bool sd_ok)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", RFFE_DIR, JC_FILE);
    struct stat st;
    if (!sd_ok || stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 512 * 1024) {
        useBuiltinJc();
        return;
    }
    if (s_jc_from_sd && st.st_size == s_jc_size && st.st_mtime == s_jc_mtime) {
        return;
    }
    char *buf = static_cast<char *>(malloc(st.st_size + 1));
    FILE *f = buf ? fopen(path, "rb") : nullptr;
    const size_t got = f ? fread(buf, 1, st.st_size, f) : 0;
    if (f) {
        fclose(f);
    }
    if (got == 0) {
        free(buf);
        useBuiltinJc();
        return;
    }
    buf[got] = '\0';

    std::vector<JcEntry> rows;
    bool header_ok = false;
    int bad = 0;
    char *save = nullptr;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        line[strcspn(line, "\r")] = '\0';
        if (!header_ok) {
            // Cột: model_id,type_code,rffe_line,device_part,usid_dec,usid_hex
            header_ok = strstr(line, "model_id") != nullptr && strstr(line, "usid_hex") != nullptr;
            if (!header_ok) {
                break;
            }
            continue;
        }
        char *fld[6];
        if (splitCsv(line, fld, 6) != 6 || fld[0][0] == '\0') {
            ++bad;
            continue;
        }
        for (char *x : fld) {
            trimRight(x);  // vài tên đường trong file có dấu cách thừa ở cuối
        }
        char *end1 = nullptr, *end2 = nullptr;
        const long dec = strtol(fld[4], &end1, 10);
        const long hex = strtol(fld[5], &end2, 16);
        if (*end1 != '\0' || *end2 != '\0' || dec != hex || dec < 0 || dec > 15) {
            ++bad;  // USID hỏng / không khớp giữa 2 cột: bỏ, không tự sửa
            continue;
        }
        rows.push_back({fld[0], fld[2], fld[3], static_cast<uint8_t>(dec)});
    }
    ESP_LOGI(TAG, "%s trên thẻ: %d dòng%s, bỏ %d dòng lỗi", JC_FILE, (int)rows.size(),
             header_ok ? "" : " (sai dòng tiêu đề)", bad);
    if (rows.empty()) {
        free(buf);
        useBuiltinJc();
        return;
    }
    s_jc.swap(rows);
    free(s_jc_buf);
    s_jc_buf = buf;
    s_jc_from_sd = true;
    s_jc_size = st.st_size;
    s_jc_mtime = st.st_mtime;
}

const char *jcSource(void)
{
    return s_jc_from_sd ? "thẻ SD" : "firmware";
}

// Tên hiển thị (chỉ đổi cách hiện, dữ liệu gốc giữ nguyên):
//  - model: IDS_iphxne15Pro_Max_TYPE -> "IP 15 PROMAX", IDS_IPHXNE7PLUS_INTEL -> "IP 7 PLUS INTEL"
//  - đường RFFE: bỏ ký tự rác do trích xuất để lại ở cuối tên: ký tự lạ (@ & [ ...) và một ký tự dính sau
//    phần tên chuẩn cuối cùng (DATA3 -> DATA, SDR0B -> SDR0, SDAT4 -> SDAT, RFFEe -> RFFE, R9 -> R ...).
// Đã kiểm với bảng hiện có: không có hai model / hai đường cùng model bị trùng tên sau khi đổi.
static std::string jcModelName(const char *raw)
{
    std::string s(raw);
    if (s.size() > 4 && strncasecmp(s.c_str(), "IDS_", 4) == 0) {
        s.erase(0, 4);
    }
    if (s.size() > 5 && strcasecmp(s.c_str() + s.size() - 5, "_TYPE") == 0) {
        s.erase(s.size() - 5);
    }
    if (s.size() > 6 && strncasecmp(s.c_str(), "iphxne", 6) == 0) {
        s.erase(0, 6);
    }
    std::string out = "IP ";
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = static_cast<char>(toupper(static_cast<unsigned char>(s[i])));
        // Số liền chữ dài (15Pro, 7PLUS, 12Mini) -> thêm dấu cách; giữ 6S, SE3, XS.
        if (i > 0 && isdigit(static_cast<unsigned char>(s[i - 1])) && isalpha(static_cast<unsigned char>(s[i]))) {
            size_t n = 0;
            while (i + n < s.size() && isalpha(static_cast<unsigned char>(s[i + n]))) {
                ++n;
            }
            if (n >= 3) {
                out += ' ';
            }
        }
        out += (c == '_') ? ' ' : c;
    }
    const size_t pm = out.find("PRO MAX");
    if (pm != std::string::npos) {
        out.erase(pm + 3, 1);  // "PRO MAX" -> "PROMAX"
    }
    return out;
}

static std::string jcLineName(const char *raw)
{
    std::string s(raw);
    while (!s.empty() && !isalnum(static_cast<unsigned char>(s.back())) && s.back() != '_') {
        s.pop_back();
    }
    const size_t us = s.rfind('_');
    if (us != std::string::npos && us + 1 < s.size()) {
        static const char *const STEMS[] = {"SDATA", "DATA", "SDAT", "SDR0", "BUFFER", "RFFE", "R", "K"};
        const std::string last = s.substr(us + 1);
        bool exact = (last == "SDR");
        for (const char *st : STEMS) {
            exact = exact || last == st;
        }
        if (!exact) {
            bool cut = false;
            for (const char *st : STEMS) {
                const size_t n = strlen(st);
                if (last.size() == n + 1 && last.compare(0, n, st) == 0) {
                    s.pop_back();
                    cut = true;
                    break;
                }
            }
            if (!cut && last.size() == 4 && last.compare(0, 3, "SDR") == 0 && last[3] != '0') {
                s.pop_back();  // SDR2 / SDR9 / SDR5 -> SDR (SDR0 là tên thật)
            }
        }
    }
    return s.empty() ? std::string(raw) : s;
}

bool isJcModel(const char *model)
{
    for (const JcEntry &e : s_jc) {
        if (jcModelName(e.model) == model) {
            return true;
        }
    }
    return false;
}

const char *jcFileName(void)
{
    return JC_FILE;
}

// Thêm name vào danh sách dropdown nếu chưa có.
static void appendUnique(std::vector<std::string> &names, const std::string &name)
{
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
    }
}

static int joinNames(const std::vector<std::string> &names, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t used = 0;
    int count = 0;
    for (const std::string &n : names) {
        const size_t need = n.size() + (count ? 1 : 0);
        if (used + need + 1 > out_size) {
            break;
        }
        used += snprintf(out + used, out_size - used, "%s%s", count ? "\n" : "", n.c_str());
        ++count;
    }
    return count;
}

int listModels(char *out, size_t out_size)
{
    out[0] = '\0';
    const bool sd_ok = sdReady();
    loadJc(sd_ok);  // không có thẻ vẫn có bảng JC trong firmware
    std::vector<std::string> names;
    for (const JcEntry &e : s_jc) {
        appendUnique(names, jcModelName(e.model));  // giữ thứ tự trong bảng
    }
    char dirs[1024];
    if (sd_ok && listDir(RFFE_DIR, true, nullptr, dirs, sizeof(dirs)) > 0) {
        char *save = nullptr;
        for (char *t = strtok_r(dirs, "\n", &save); t; t = strtok_r(nullptr, "\n", &save)) {
            appendUnique(names, t);
        }
    }
    if (names.empty()) {
        return -1;
    }
    return joinNames(names, out, out_size);
}

int listData(const char *model, char *out, size_t out_size)
{
    if (isJcModel(model)) {
        std::vector<std::string> lines;
        for (const JcEntry &e : s_jc) {
            if (jcModelName(e.model) == model) {
                appendUnique(lines, jcLineName(e.line));
            }
        }
        return joinNames(lines, out, out_size);
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", RFFE_DIR, model);
    return listDir(path, false, ".csv", out, out_size);
}

// Tách trường kế tiếp (ngăn bởi ',' hoặc ';'), bỏ khoảng trắng hai đầu.
static char *nextField(char **cursor)
{
    char *s = *cursor;
    if (s == nullptr) {
        return nullptr;
    }
    char *sep = strpbrk(s, ",;");
    if (sep) {
        *sep = '\0';
        *cursor = sep + 1;
    } else {
        *cursor = nullptr;
    }
    while (isspace(static_cast<unsigned char>(*s))) {
        ++s;
    }
    char *e = s + strlen(s);
    while (e > s && isspace(static_cast<unsigned char>(e[-1]))) {
        *--e = '\0';
    }
    return s;
}

static bool parseNum(const char *s, long max, long *out)
{
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char *end = nullptr;
    const long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || v < 0 || v > max) {
        return false;
    }
    *out = v;
    return true;
}

bool loadReference(const char *model, const char *file, std::vector<RefEntry> &out, int *skipped,
                   char *err, size_t err_size)
{
    out.clear();
    *skipped = 0;
    if (isJcModel(model)) {
        // Mỗi linh kiện trên đường RFFE đã chọn: kiểm 0x1F, bit[3:0] == USID theo file.
        for (const JcEntry &e : s_jc) {
            if (jcModelName(e.model) != model || jcLineName(e.line) != file) {
                continue;
            }
            RefEntry r = {};
            r.usid = e.usid;
            r.reg = 0x1F;
            r.expected = e.usid;
            r.mask = 0x0F;
            snprintf(r.name, sizeof(r.name), "%s", e.part);
            snprintf(r.ic, sizeof(r.ic), "%s", e.part);
            out.push_back(r);
        }
        if (out.empty()) {
            snprintf(err, err_size, "Không có linh kiện cho %s / %s", model, file);
            return false;
        }
        err[0] = '\0';
        return true;
    }
    if (!sdReady()) {
        snprintf(err, err_size, "Không đọc được thẻ SD");
        return false;
    }
    char path[200];
    snprintf(path, sizeof(path), "%s/%s/%s", RFFE_DIR, model, file);
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        snprintf(err, err_size, "Không mở được %s", file);
        return false;
    }
    char line[160];
    bool first = true;
    while (fgets(line, sizeof(line), f) != nullptr) {
        char *p = line;
        if (first && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
            p += 3;  // BOM UTF-8
        }
        while (isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (*p == '\0' || *p == '#') {
            first = false;
            continue;
        }
        p[strcspn(p, "\r\n")] = '\0';
        char *cur = p;
        char *f_usid = nextField(&cur);
        char *f_reg = nextField(&cur);
        char *f_exp = nextField(&cur);
        char *f_mask = nextField(&cur);
        char *f_name = nextField(&cur);
        char *f_ic = nextField(&cur);
        long usid, reg, exp, mask = 0xFF;
        const bool ok = parseNum(f_usid, 15, &usid) && parseNum(f_reg, 0xFF, &reg) &&
                        parseNum(f_exp, 0xFF, &exp) &&
                        (f_mask == nullptr || *f_mask == '\0' || parseNum(f_mask, 0xFF, &mask));
        if (!ok) {
            if (!first) {  // dòng đầu không phải số = dòng tiêu đề cột
                ++*skipped;
            }
            first = false;
            continue;
        }
        first = false;
        RefEntry e = {};
        e.usid = static_cast<uint8_t>(usid);
        e.reg = static_cast<uint16_t>(reg);
        e.expected = static_cast<uint8_t>(exp);
        e.mask = static_cast<uint8_t>(mask);
        if (f_name) {
            snprintf(e.name, sizeof(e.name), "%s", f_name);
        }
        if (f_ic) {
            snprintf(e.ic, sizeof(e.ic), "%s", f_ic);
        }
        out.push_back(e);
        if (out.size() >= 512) {
            break;
        }
    }
    fclose(f);
    if (out.empty()) {
        snprintf(err, err_size, "%s không có dòng hợp lệ (usid,reg,expected[,mask[,tên[,ic]]])", file);
        return false;
    }
    err[0] = '\0';
    return true;
}

// ==================== Bus RFFE (bit-bang) ====================
// Master đổi SDATA ở cạnh lên SCLK, slave lấy mẫu ở cạnh xuống; khi đọc, slave đổi ở cạnh lên và master
// lấy mẫu trước cạnh xuống. Bit chẵn lẻ: lẻ (tổng số bit 1 kể cả bit chẵn lẻ là số lẻ).

static inline void sclk(int v)
{
    gpio_set_level(static_cast<gpio_num_t>(s_pin_clk), v);
}

static inline void sdata(int v)
{
    gpio_set_level(static_cast<gpio_num_t>(s_pin_dat), v);
}

static inline void sdataDrive(bool drive)
{
    gpio_set_direction(static_cast<gpio_num_t>(s_pin_dat), drive ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
}

// Cả hai chân cấu hình giống nhau (vào / ra, pull-down) để đổi vai SCLK <-> SDATA chỉ cần đổi s_pin_clk / s_pin_dat.
// Pull-down: không có slave thì SDATA đọc về 0 -> sai chẵn lẻ -> "không phản hồi".
static void gpioInit(void)
{
    if (s_gpio_ready) {
        return;
    }
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << (PIN_SCLK & 63)) | (1ULL << (PIN_SDATA & 63));  // configured() đã kiểm tra chân >= 0
    io.mode = GPIO_MODE_INPUT_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    gpio_set_level(static_cast<gpio_num_t>(PIN_SCLK), 0);
    gpio_set_level(static_cast<gpio_num_t>(PIN_SDATA), 0);
    s_gpio_ready = true;
}

// Đặt chiều chân: swapped = SCLK ra chân SDATA cấu hình và ngược lại. Cả hai chân về mức 0, đang lái ra.
static void setOrientation(bool swapped)
{
    s_pin_clk = swapped ? PIN_SDATA : PIN_SCLK;
    s_pin_dat = swapped ? PIN_SCLK : PIN_SDATA;
    gpio_set_direction(static_cast<gpio_num_t>(s_pin_clk), GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(s_pin_dat), GPIO_MODE_INPUT_OUTPUT);
    sclk(0);
    sdata(0);
    esp_rom_delay_us(50);
}

static void writeBit(int b)
{
    sclk(1);
    sdata(b);
    esp_rom_delay_us(HALF_US);
    sclk(0);
    esp_rom_delay_us(HALF_US);
}

static int readBit(void)
{
    sclk(1);
    esp_rom_delay_us(HALF_US);
    const int v = gpio_get_level(static_cast<gpio_num_t>(s_pin_dat));
    sclk(0);
    esp_rom_delay_us(HALF_US);
    return v;
}

// Ghi n bit (MSB trước), trả số bit 1.
static int writeBits(uint32_t value, int n)
{
    int ones = 0;
    for (int i = n - 1; i >= 0; --i) {
        const int b = (value >> i) & 1;
        ones += b;
        writeBit(b);
    }
    return ones;
}

// Bus park: master kéo SDATA xuống trong nửa chu kỳ đầu rồi thả ra ở cạnh xuống.
static void busPark(bool release_after)
{
    sclk(1);
    sdata(0);
    esp_rom_delay_us(HALF_US);
    sclk(0);
    if (release_after) {
        sdataDrive(false);
    }
    esp_rom_delay_us(HALF_US);
}

// Đọc 1 thanh ghi. Trả true khi bit chẵn lẻ của byte đọc về đúng.
static bool readReg(uint8_t usid, uint16_t reg, uint8_t *value)
{
    int v = 0;
    int ones = 0;
    portENTER_CRITICAL(&s_mux);
    sdataDrive(true);
    sclk(0);
    sdata(0);
    esp_rom_delay_us(HALF_US);
    // Sequence Start Condition: xung SDATA lên-xuống khi SCLK ở mức thấp.
    sdata(1);
    esp_rom_delay_us(HALF_US * 2);
    sdata(0);
    esp_rom_delay_us(HALF_US);
    if (reg <= 0x1F) {
        // Register Read: USID[3:0] 011 A[4:0] P
        const int c = writeBits(((uint32_t)usid << 8) | 0x60 | reg, 12);
        writeBit((c & 1) ? 0 : 1);
    } else {
        // Extended Register Read (1 byte): USID[3:0] 0010 0000 P, A[7:0] P
        const int c = writeBits(((uint32_t)usid << 8) | 0x20, 12);
        writeBit((c & 1) ? 0 : 1);
        const int a = writeBits(reg & 0xFF, 8);
        writeBit((a & 1) ? 0 : 1);
    }
    busPark(true);
    for (int i = 0; i < 8; ++i) {
        const int b = readBit();
        v = (v << 1) | b;
        ones += b;
    }
    ones += readBit();  // bit chẵn lẻ của slave
    // Bus park cuối: slave thả SDATA, master lấy lại bus và giữ mức thấp.
    sclk(1);
    esp_rom_delay_us(HALF_US);
    sclk(0);
    sdataDrive(true);
    sdata(0);
    esp_rom_delay_us(HALF_US);
    portEXIT_CRITICAL(&s_mux);
    *value = static_cast<uint8_t>(v);
    return (ones & 1) == 1;
}

// ==================== Quét ====================

// Khi master thả SDATA (chỉ còn pull-down nội) bus phải ở mức 0. Nếu bị kéo lên (pull-up của mạch chuyển
// mức, chạm nguồn) thì mọi lần đọc ra 0xFF + P=1 vẫn đúng parity lẻ -> sẽ sinh USID ảo, nên không quét.
static bool busIdleLow(void)
{
    sclk(0);
    sdata(0);
    sdataDrive(false);
    esp_rom_delay_us(50);
    int highs = 0;
    for (int i = 0; i < 16; ++i) {
        highs += gpio_get_level(static_cast<gpio_num_t>(s_pin_dat));
        esp_rom_delay_us(5);
    }
    sdataDrive(true);
    sdata(0);
    return highs == 0;
}

// Chỉ nhận USID khi có thiết bị thật trả lời: thanh ghi 0x1F (bit[3:0] = USID của chính thiết bị theo spec
// RFFE) đúng parity, 4 bit thấp trùng USID đang hỏi, và đọc lại lần hai ra cùng giá trị.
static bool probeUsid(uint8_t usid, UsidResult &u)
{
    uint8_t r1 = 0, r2 = 0;
    const bool p1 = readReg(usid, 0x1F, &r1);
    if (!p1 || (r1 & 0x0F) != usid) {
        if (p1) {
            // Chẩn đoán: có byte đúng parity nhưng 0x1F[3:0] khác USID đang hỏi -> không nhận (chống USID ảo).
            uint8_t a = 0, b = 0;
            const bool pa = readReg(usid, 0x1D, &a);
            const bool pb = readReg(usid, 0x1E, &b);
            ESP_LOGW(TAG, "USID 0x%X: 0x1F=0x%02X đúng parity nhưng [3:0]=0x%X != USID (0x1D=%s0x%02X 0x1E=%s0x%02X) - bỏ",
                     usid, r1, r1 & 0x0F, pa ? "" : "?", a, pb ? "" : "?", b);
        }
        return false;
    }
    esp_rom_delay_us(20);
    if (!readReg(usid, 0x1F, &r2) || r2 != r1) {
        ESP_LOGW(TAG, "USID 0x%X: 0x1F lần 1 = 0x%02X, lần 2 = 0x%02X - không ổn định, bỏ", usid, r1, r2);
        return false;
    }
    u.id_regs[2] = r1;
    u.id_ok[2] = true;
    u.id_ok[0] = readReg(usid, 0x1D, &u.id_regs[0]);
    u.id_ok[1] = readReg(usid, 0x1E, &u.id_regs[1]);
    ESP_LOGI(TAG, "USID 0x%X: 0x1F=0x%02X 0x1D=%s0x%02X 0x1E=%s0x%02X", usid, r1, u.id_ok[0] ? "" : "?",
             u.id_regs[0], u.id_ok[1] ? "" : "?", u.id_regs[1]);
    return true;
}

static void addReg(UsidResult &u, uint16_t reg, const RefEntry *ref)
{
    RegResult r = {};
    r.reg = reg;
    if (ref) {
        r.has_ref = true;
        r.expected = ref->expected;
        r.mask = ref->mask;
        snprintf(r.name, sizeof(r.name), "%s", ref->name);
    }
    u.regs.push_back(r);
}

void buildPreview(const std::vector<RefEntry> &ref, Report &out)
{
    out = Report();
    for (const RefEntry &e : ref) {
        auto it = std::find_if(out.usids.begin(), out.usids.end(),
                               [&](const UsidResult &u) { return u.usid == e.usid; });
        if (it == out.usids.end()) {
            UsidResult u = {};
            u.usid = e.usid;
            u.in_ref = true;
            out.usids.push_back(u);
            it = out.usids.end() - 1;
        }
        if (e.ic[0] != '\0' && strstr(it->ic, e.ic) == nullptr) {
            // Nhiều linh kiện cùng USID: ghép tên, cắt khi đầy.
            const size_t n = strlen(it->ic);
            snprintf(it->ic + n, sizeof(it->ic) - n, "%s%s", n ? "/" : "", e.ic);
        }
        addReg(*it, e.reg, &e);
    }
    std::sort(out.usids.begin(), out.usids.end(),
              [](const UsidResult &a, const UsidResult &b) { return a.usid < b.usid; });
}

static void scanTask(void *)
{
    Report rep;
    buildPreview(s_scan_ref, rep);
    rep.scanned = true;
    gpioInit();

    // Dò USID theo chiều chân lần trước; không thấy USID nào thì đảo SCLK <-> SDATA và dò lại (nối ngược dây).
    // Mỗi chiều dùng cùng điều kiện nhận USID (0x1F đúng parity, [3:0] == USID, đọc lại khớp) nên đảo chiều
    // không sinh USID ảo; cả hai chiều không thấy -> 0 USID.
    UsidResult probes[16] = {};
    bool idle_ok = false;
    int found = 0;
    const bool first = pinsSwapped();
    for (int attempt = 0; attempt < 2 && found == 0; ++attempt) {
        const bool sw = (attempt == 0) ? first : !first;
        setOrientation(sw);
        const bool idle = busIdleLow();
        if (!idle) {
            ESP_LOGW(TAG, "SCLK GPIO%d / SDATA GPIO%d: SDATA bị kéo lên khi bus nghỉ - bỏ chiều này", s_pin_clk,
                     s_pin_dat);
            continue;
        }
        idle_ok = true;
        for (uint8_t usid = 1; usid <= 15; ++usid) {  // USID 0 là broadcast
            probes[usid] = UsidResult();
            probes[usid].usid = usid;
            probes[usid].responded = probeUsid(usid, probes[usid]);
            found += probes[usid].responded ? 1 : 0;
        }
        ESP_LOGI(TAG, "SCLK GPIO%d / SDATA GPIO%d%s: %d USID", s_pin_clk, s_pin_dat, sw ? " (đảo)" : "", found);
        vTaskDelay(1);
    }
    if (found == 0) {
        setOrientation(false);  // không thấy gì: trở về chiều cấu hình
    }

    for (uint8_t usid = 1; idle_ok && usid <= 15; ++usid) {
        UsidResult probe = probes[usid];
        auto it = std::find_if(rep.usids.begin(), rep.usids.end(),
                               [&](const UsidResult &u) { return u.usid == usid; });
        if (it == rep.usids.end()) {
            if (!probe.responded) {
                continue;
            }
            // Không có tham chiếu: đọc thô 0x00..0x1F, không kết luận.
            for (uint16_t r = 0; r <= 0x1F; ++r) {
                addReg(probe, r, nullptr);
            }
            rep.usids.push_back(probe);
            it = rep.usids.end() - 1;
        } else {
            it->responded = probe.responded;
            memcpy(it->id_regs, probe.id_regs, sizeof(it->id_regs));
            memcpy(it->id_ok, probe.id_ok, sizeof(it->id_ok));
        }
        for (RegResult &r : it->regs) {
            r.read_ok = it->responded && readReg(usid, r.reg, &r.value);
            if (!r.read_ok) {
                ++it->no_read;
            } else if (r.has_ref) {
                if ((r.value & r.mask) == (r.expected & r.mask)) {
                    ++it->pass;
                } else {
                    ++it->fail;
                }
            }
        }
        vTaskDelay(1);
    }
    std::sort(rep.usids.begin(), rep.usids.end(),
              [](const UsidResult &a, const UsidResult &b) { return a.usid < b.usid; });

    int pass = 0, fail = 0, missing = 0;
    for (const UsidResult &u : rep.usids) {
        pass += u.pass;
        fail += u.fail;
        missing += (u.in_ref && !u.responded && u.usid != 0) ? 1 : 0;  // USID 0 (broadcast) không quét
    }
    if (!idle_ok) {
        snprintf(rep.message, sizeof(rep.message), "SDATA bị kéo lên khi nghỉ ở cả hai chiều chân (pull-up / chạm nguồn) - không quét");
    } else if (found == 0) {
        snprintf(rep.message, sizeof(rep.message), "Không có USID nào phản hồi (đã thử cả hai chiều SCLK/SDATA; kiểm tra nguồn 1,8 V, chuyển mức)");
    } else if (s_scan_ref.empty()) {
        snprintf(rep.message, sizeof(rep.message), "Tìm thấy %d USID%s - chưa chọn dữ liệu để so", found,
                 pinsSwapped() ? " (dây đảo)" : "");
    } else {
        snprintf(rep.message, sizeof(rep.message), "Tìm thấy %d USID%s   PASS %d   FAIL %d   USID thiếu %d",
                 found, pinsSwapped() ? " (dây đảo)" : "", pass, fail, missing);
    }
    ESP_LOGI(TAG, "%s", rep.message);

    s_report = std::move(rep);
    s_serial = s_serial + 1;
    s_busy = false;
    vTaskDelete(nullptr);
}

bool startScan(const std::vector<RefEntry> &ref)
{
    if (s_busy || !configured()) {
        return false;
    }
    s_busy = true;
    s_scan_ref = ref;
    if (xTaskCreatePinnedToCore(scanTask, "hcc_rffe", 6144, nullptr, 5, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_busy = false;
        return false;
    }
    return true;
}

bool busy(void)
{
    return s_busy;
}

uint32_t reportSerial(void)
{
    return s_serial;
}

const Report &report(void)
{
    return s_report;
}

}  // namespace HCCRffe
