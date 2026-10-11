#include "HCCEepromView.hpp"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <string>
#include <vector>

#include "HCCEeprom.hpp"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hcc_cores.h"

namespace HCCEepromView {

namespace {

const char *TAG = "HCCEepromView";

// ==================== Màu / kiểu ====================

const lv_color_t C_BG = lv_color_hex(0x0B1220);
const lv_color_t C_PANEL = lv_color_hex(0x111C2E);
const lv_color_t C_PANEL2 = lv_color_hex(0x0D1626);
const lv_color_t C_BORDER = lv_color_hex(0x1F3350);
const lv_color_t C_TITLE = lv_color_hex(0x3DB4FF);
const lv_color_t C_TEXT = lv_color_hex(0xE8EEF5);
const lv_color_t C_MUTED = lv_color_hex(0x9AAAC0);
const lv_color_t C_BLUE = lv_color_hex(0x1F7AE0);
const lv_color_t C_GREEN = lv_color_hex(0x1E9E4A);
const lv_color_t C_AMBER = lv_color_hex(0xD99A14);
const lv_color_t C_RED = lv_color_hex(0xD63A3A);
const lv_color_t C_PURPLE = lv_color_hex(0x7A3FD1);
const lv_color_t C_TEAL = lv_color_hex(0x1F8FB3);
const lv_color_t C_CHANGED = lv_color_hex(0xFFB020);
const lv_color_t C_OKC = lv_color_hex(0x3DDC84);
const lv_color_t C_BTN = lv_color_hex(0x1A2A42);

// ==================== Loại chip ====================

struct Chip {
    const char *name;
    uint32_t size;
    uint16_t page;
    uint8_t aw;  // số byte địa chỉ ô nhớ
};

// Dung lượng / trang / số byte địa chỉ theo họ 24Cxx thông dụng.
const Chip CHIPS[] = {
    {"24C01", 128, 8, 1},       {"24C02", 256, 8, 1},       {"24C04", 512, 16, 1},
    {"24C08", 1024, 16, 1},     {"24C16", 2048, 16, 1},     {"24C32", 4096, 32, 2},
    {"24C64", 8192, 32, 2},     {"24C128", 16384, 64, 2},   {"24C256", 32768, 64, 2},
    {"24C512", 65536, 128, 2},
};
constexpr int NCHIP = sizeof(CHIPS) / sizeof(CHIPS[0]);
constexpr int MAX_DIFF = 500;
constexpr int ASCII_W = 144;  // cột ASCII của HEX EDITOR

int s_chip = 3;     // 24C08
int s_addr = 0x50;
bool s_color_changed = true;
bool s_show_ascii = true;

// ==================== Dữ liệu ====================

uint8_t *s_buf = nullptr;  // bộ đệm đang sửa
uint8_t *s_ref = nullptr;  // tham chiếu (lần đọc chip / nạp file gần nhất) để tô byte thay đổi
uint32_t s_size = 0;       // kích thước bộ đệm hiện tại
bool s_has_data = false;
std::string s_file;        // file đã nạp / lưu gần nhất
std::vector<uint32_t> s_diffs;  // vị trí byte khác giữa bộ đệm và chip (lần xác minh gần nhất)
uint32_t s_diff_total = 0;
bool s_diff_valid = false;
uint8_t *s_chipbuf = nullptr;   // nội dung chip của lần xác minh gần nhất

// ==================== Tác vụ nền ====================

enum Op { OP_NONE, OP_SCAN, OP_READ, OP_WRITE, OP_VERIFY, OP_ERASE, OP_SAVE, OP_LOAD };

struct Job {
    Op op = OP_NONE;
    HCCEeprom::Config cfg;
    std::string path;  // OP_SAVE / OP_LOAD (đường dẫn tương đối thẻ SD)
};

struct Result {
    Op op = OP_NONE;
    esp_err_t err = ESP_OK;
    uint8_t found = 0;      // OP_SCAN: bit i = 0x50 + i trả ACK
    uint32_t len = 0;       // OP_LOAD: số byte
    uint32_t diffs = 0;     // OP_WRITE / OP_VERIFY / OP_ERASE
    uint32_t first_diff = 0;
    char msg[160] = "";
};

volatile bool s_busy = false;
volatile uint32_t s_done = 0, s_total = 0;
volatile uint32_t s_serial = 0;
Job s_job;
Result s_res;
uint8_t *s_work = nullptr;  // bản sao bộ đệm cho tác vụ (ghi / lưu) và nơi nhận dữ liệu (đọc / nạp)
uint8_t *s_work2 = nullptr; // đọc lại để xác minh
BusGetter s_get_bus = nullptr;
lv_event_cb_t s_back_cb = nullptr;
void *s_back_user = nullptr;
bool s_back_pending = false;
void *s_bus_user = nullptr;

void onProgress(uint32_t done, uint32_t total, void *)
{
    s_done = done;
    s_total = total;
}

uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

bool ensureSd(void)
{
    const esp_err_t r = bsp_sdcard_mount();
    return r == ESP_OK || r == ESP_ERR_INVALID_STATE;  // đã mount sẵn: dùng chung
}

void compare(const uint8_t *a, const uint8_t *b, uint32_t n, Result &r)
{
    r.diffs = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            if (r.diffs == 0) {
                r.first_diff = i;
            }
            ++r.diffs;
        }
    }
}

void workerTask(void *)
{
    Result r;
    r.op = s_job.op;
    const HCCEeprom::Config &cfg = s_job.cfg;
    i2c_master_bus_handle_t bus = nullptr;
    if (r.op != OP_SAVE && r.op != OP_LOAD) {
        bus = (i2c_master_bus_handle_t)(s_get_bus ? s_get_bus(s_bus_user) : nullptr);
        if (bus == nullptr) {
            r.err = ESP_FAIL;
            snprintf(r.msg, sizeof(r.msg), "Không khởi tạo được bus I2C (GPIO4 / GPIO5)");
            goto done;
        }
    }
    switch (r.op) {
    case OP_SCAN:
        for (int i = 0; i < 8; ++i) {
            if (i2c_master_probe(bus, 0x50 + i, 30) == ESP_OK) {
                r.found |= (uint8_t)(1u << i);
            }
            s_done = i + 1;
            s_total = 8;
        }
        break;
    case OP_READ:
        r.err = HCCEeprom::read(bus, cfg, 0, s_work, cfg.total_size, onProgress, nullptr);
        break;
    case OP_ERASE:
        memset(s_work, 0xFF, cfg.total_size);
        [[fallthrough]];  // ghi rồi xác minh
    case OP_WRITE:
        r.err = HCCEeprom::write(bus, cfg, 0, s_work, cfg.total_size, onProgress, nullptr);
        if (r.err == ESP_OK) {
            r.err = HCCEeprom::read(bus, cfg, 0, s_work2, cfg.total_size, onProgress, nullptr);
            if (r.err == ESP_OK) {
                compare(s_work, s_work2, cfg.total_size, r);
            }
        }
        break;
    case OP_VERIFY:
        r.err = HCCEeprom::read(bus, cfg, 0, s_work2, cfg.total_size, onProgress, nullptr);
        if (r.err == ESP_OK) {
            compare(s_work, s_work2, cfg.total_size, r);
        }
        break;
    case OP_SAVE: {
        if (!ensureSd()) {
            r.err = ESP_FAIL;
            snprintf(r.msg, sizeof(r.msg), "Không mở được thẻ nhớ");
            break;
        }
        mkdir(BSP_SD_MOUNT_POINT "/eeprom", 0775);
        r.err = HCCEeprom::saveBufferToSd(s_job.path.c_str(), s_work, cfg.total_size);
        break;
    }
    case OP_LOAD: {
        size_t n = 0;
        memset(s_work, 0xFF, cfg.total_size);
        r.err = HCCEeprom::loadBufferFromSd(s_job.path.c_str(), s_work, cfg.total_size, &n);
        r.len = (uint32_t)n;
        if (r.err == ESP_OK && n == 0) {
            r.err = ESP_ERR_INVALID_SIZE;
        }
        break;
    }
    default:
        break;
    }
done:
    s_res = r;
    s_serial = s_serial + 1;
    s_busy = false;
    vTaskDelete(nullptr);
}

HCCEeprom::Config currentConfig(void)
{
    HCCEeprom::Config c;
    c.device_address = (uint16_t)s_addr;
    c.total_size = CHIPS[s_chip].size;
    c.page_size = CHIPS[s_chip].page;
    c.address_width = CHIPS[s_chip].aw;
    return c;
}

// ==================== Giao diện ====================

struct View {
    lv_obj_t *root = nullptr;
    lv_obj_t *chip_dd = nullptr;
    lv_obj_t *addr_dd = nullptr;
    lv_obj_t *found_btn[8] = {};
    lv_obj_t *op_btn[6] = {};
    lv_obj_t *tab_btn[3] = {};
    lv_obj_t *panel_hex = nullptr;
    lv_obj_t *panel_cmp = nullptr;
    lv_obj_t *panel_file = nullptr;
    lv_obj_t *table = nullptr;
    lv_obj_t *cmp_table = nullptr;
    lv_obj_t *cmp_title = nullptr;
    lv_obj_t *file_list = nullptr;
    lv_obj_t *page_lbl = nullptr;
    lv_obj_t *page_prev = nullptr;
    lv_obj_t *page_next = nullptr;
    lv_obj_t *log = nullptr;
    lv_obj_t *log_box = nullptr;
    lv_obj_t *bar = nullptr;
    lv_obj_t *info_chip[6] = {};
    lv_obj_t *info_data[4] = {};
    lv_obj_t *goto_lbl = nullptr;
    lv_obj_t *keypad = nullptr;
    lv_obj_t *kp_title = nullptr;
    lv_obj_t *kp_value = nullptr;
    lv_timer_t *timer = nullptr;
    int tab = 0;
    uint32_t page = 0;     // trang 256 byte
    int32_t sel = 0;       // byte đang chọn
    int kp_mode = 0;       // 1 = sửa byte, 2 = nhập địa chỉ
    char kp_text[8] = "";
    uint32_t goto_addr = 0;
    uint32_t seen = 0;
    Op confirm_op = OP_NONE;
    uint32_t confirm_tick = 0;
    std::string log_text;
    int log_lines = 0;
};

View v;

lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, C_PANEL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_border_color(p, C_BORDER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *t = label(p, title, &lv_font_be_vietnam_16, C_TITLE);
        lv_obj_set_pos(t, 10, 6);
    }
    return p;
}

lv_obj_t *button(lv_obj_t *parent, const char *t, int x, int y, int w, int h, lv_color_t bg, lv_event_cb_t cb,
                 void *ud, const lv_font_t *f = &lv_font_be_vietnam_16)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_t *l = label(b, t, f, C_TEXT);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }
    return b;
}

void addLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void addLog(const char *fmt, ...)
{
    char line[200];
    char ts[16];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year + 1900 >= 2024) {
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    } else {
        const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(ts, sizeof(ts), "%02u:%02u:%02u", (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60),
                 (unsigned)(s % 60));
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
    if (v.log == nullptr) {
        return;
    }
    if (v.log_lines >= 60) {  // giữ 60 dòng gần nhất
        const size_t nl = v.log_text.find('\n');
        v.log_text.erase(0, nl == std::string::npos ? v.log_text.size() : nl + 1);
        --v.log_lines;
    }
    if (!v.log_text.empty()) {
        v.log_text += '\n';
    }
    v.log_text += "[";
    v.log_text += ts;
    v.log_text += "]  ";
    v.log_text += line;
    ++v.log_lines;
    lv_label_set_text(v.log, v.log_text.c_str());
    lv_obj_update_layout(v.log_box);
    lv_obj_scroll_to_y(v.log_box, LV_COORD_MAX, LV_ANIM_OFF);
}

uint32_t pages(void)
{
    return s_size == 0 ? 1 : (s_size + 255) / 256;
}

uint32_t changedCount(void)
{
    if (!s_has_data) {
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_size; ++i) {
        n += s_buf[i] != s_ref[i];
    }
    return n;
}

void updateInfo(void)
{
    const Chip &c = CHIPS[s_chip];
    char b[64];
    lv_label_set_text(v.info_chip[0], c.name);
    if (c.size >= 1024) {
        snprintf(b, sizeof(b), "%u KB (%u B)", (unsigned)(c.size / 1024), (unsigned)c.size);
    } else {
        snprintf(b, sizeof(b), "%u B", (unsigned)c.size);
    }
    lv_label_set_text(v.info_chip[1], b);
    snprintf(b, sizeof(b), "%u B", (unsigned)c.page);
    lv_label_set_text(v.info_chip[2], b);
    snprintf(b, sizeof(b), "0x%02X", s_addr);
    lv_label_set_text(v.info_chip[3], b);
    lv_label_set_text(v.info_chip[4], "I2C GPIO4/5");
    if (s_busy) {
        lv_label_set_text(v.info_chip[5], "Đang chạy");
        lv_obj_set_style_text_color(v.info_chip[5], C_CHANGED, 0);
    } else {
        lv_label_set_text(v.info_chip[5], "Sẵn sàng");
        lv_obj_set_style_text_color(v.info_chip[5], C_OKC, 0);
    }
    if (s_has_data) {
        snprintf(b, sizeof(b), "%u B", (unsigned)s_size);
        lv_label_set_text(v.info_data[0], b);
        snprintf(b, sizeof(b), "0x%08X", (unsigned)crc32(s_buf, s_size));
        lv_label_set_text(v.info_data[1], b);
        snprintf(b, sizeof(b), "%u", (unsigned)changedCount());
        lv_label_set_text(v.info_data[2], b);
    } else {
        lv_label_set_text(v.info_data[0], "-");
        lv_label_set_text(v.info_data[1], "-");
        lv_label_set_text(v.info_data[2], "-");
    }
    const size_t slash = s_file.find_last_of('/');
    lv_label_set_text(v.info_data[3], s_file.empty() ? "-" : s_file.c_str() + (slash == std::string::npos ? 0 : slash + 1));
}

void renderHex(void)
{
    if (v.table == nullptr) {
        return;
    }
    if (v.page >= pages()) {
        v.page = pages() - 1;
    }
    lv_table_set_col_width(v.table, 17, s_show_ascii ? ASCII_W : 1);
    char b[24];
    for (int r = 0; r < 16; ++r) {
        const uint32_t base = v.page * 256 + r * 16;
        const bool row_ok = s_has_data && base < s_size;
        snprintf(b, sizeof(b), "%04X", (unsigned)base);
        lv_table_set_cell_value(v.table, r + 1, 0, base < (s_size ? s_size : CHIPS[s_chip].size) ? b : "");
        char ascii[17];
        for (int c = 0; c < 16; ++c) {
            const uint32_t i = base + c;
            if (row_ok && i < s_size) {
                snprintf(b, sizeof(b), "%02X", s_buf[i]);
                ascii[c] = (s_buf[i] >= 0x20 && s_buf[i] < 0x7F) ? (char)s_buf[i] : '.';
            } else {
                snprintf(b, sizeof(b), "%s", row_ok ? "" : "--");
                ascii[c] = ' ';
            }
            lv_table_set_cell_value(v.table, r + 1, c + 1, b);
        }
        ascii[16] = '\0';
        lv_table_set_cell_value(v.table, r + 1, 17, row_ok ? ascii : "");
    }
    snprintf(b, sizeof(b), "%04X-%04X", (unsigned)(v.page * 256), (unsigned)(v.page * 256 + 255));
    lv_label_set_text(v.page_lbl, b);
}

void renderCompare(void)
{
    if (!s_diff_valid) {
        lv_label_set_text(v.cmp_title, "Chưa so sánh. Bấm XÁC MINH để đọc chip và so với bộ đệm.");
        lv_table_set_row_cnt(v.cmp_table, 1);
        return;
    }
    char b[96];
    if (s_diff_total == 0) {
        snprintf(b, sizeof(b), "Bộ đệm và chip giống nhau (%u byte).", (unsigned)s_size);
    } else {
        snprintf(b, sizeof(b), "Khác nhau %u byte%s", (unsigned)s_diff_total,
                 s_diff_total > s_diffs.size() ? " (hiện 500 byte đầu)" : "");
    }
    lv_label_set_text(v.cmp_title, b);
    lv_table_set_row_cnt(v.cmp_table, 1 + (uint16_t)s_diffs.size());
    for (size_t k = 0; k < s_diffs.size(); ++k) {
        const uint32_t i = s_diffs[k];
        snprintf(b, sizeof(b), "%04X", (unsigned)i);
        lv_table_set_cell_value(v.cmp_table, k + 1, 0, b);
        snprintf(b, sizeof(b), "%02X", s_buf[i]);
        lv_table_set_cell_value(v.cmp_table, k + 1, 1, b);
        snprintf(b, sizeof(b), "%02X", s_chipbuf ? s_chipbuf[i] : 0);
        lv_table_set_cell_value(v.cmp_table, k + 1, 2, b);
    }
}

// Danh sách file .bin: /sdcard/eeprom/ và gốc thẻ nhớ.
void onFileClicked(lv_event_t *e);

void renderFiles(void)
{
    lv_obj_clean(v.file_list);
    int count = 0;
    const char *dirs[] = {BSP_SD_MOUNT_POINT "/eeprom", BSP_SD_MOUNT_POINT};
    for (const char *d : dirs) {
        DIR *dir = opendir(d);
        if (dir == nullptr) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr && count < 200) {
            if (ent->d_type == DT_DIR || ent->d_name[0] == '.') {
                continue;
            }
            const size_t n = strlen(ent->d_name);
            if (n < 4 || strcasecmp(ent->d_name + n - 4, ".bin") != 0) {
                continue;
            }
            std::string rel = (d == dirs[0] ? std::string("eeprom/") : std::string()) + ent->d_name;
            lv_obj_t *btn = lv_list_add_btn(v.file_list, LV_SYMBOL_FILE, rel.c_str());
            lv_obj_set_style_bg_color(btn, C_PANEL2, 0);
            lv_obj_set_style_text_color(btn, C_TEXT, 0);
            lv_obj_set_style_text_font(btn, &lv_font_be_vietnam_16, 0);
            lv_obj_add_event_cb(btn, onFileClicked, LV_EVENT_CLICKED, nullptr);
            ++count;
        }
        closedir(dir);
    }
    if (count == 0) {
        lv_obj_t *l = lv_list_add_text(v.file_list, "Chưa có file .bin (thư mục /eeprom trên thẻ nhớ). Bấm SAO LƯU để tạo.");
        lv_obj_set_style_text_font(l, &lv_font_be_vietnam_16, 0);
        lv_obj_set_style_text_color(l, C_MUTED, 0);
        lv_obj_set_style_bg_color(l, C_PANEL2, 0);
    }
}

void setTab(int t)
{
    v.tab = t;
    lv_obj_t *panels[3] = {v.panel_hex, v.panel_cmp, v.panel_file};
    for (int i = 0; i < 3; ++i) {
        if (i == t) {
            lv_obj_clear_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(v.tab_btn[i], i == t ? C_BLUE : C_BTN, 0);
    }
    const bool hex = t == 0;
    for (lv_obj_t *o : {v.page_lbl, v.page_prev, v.page_next}) {
        if (hex) {
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (t == 1) {
        renderCompare();
    } else if (t == 2) {
        renderFiles();
    }
}

void setBusyUi(bool busy)
{
    for (lv_obj_t *b : v.op_btn) {
        if (busy) {
            lv_obj_add_state(b, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(b, LV_STATE_DISABLED);
        }
    }
    updateInfo();
}

bool ensureBuffers(uint32_t size)
{
    if (s_buf && s_size == size) {
        return true;
    }
    heap_caps_free(s_buf);
    heap_caps_free(s_ref);
    heap_caps_free(s_work);
    heap_caps_free(s_work2);
    heap_caps_free(s_chipbuf);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_buf = (uint8_t *)heap_caps_malloc(size, caps);
    s_ref = (uint8_t *)heap_caps_malloc(size, caps);
    s_work = (uint8_t *)heap_caps_malloc(size, caps);
    s_work2 = (uint8_t *)heap_caps_malloc(size, caps);
    s_chipbuf = (uint8_t *)heap_caps_malloc(size, caps);
    if (!s_buf || !s_ref || !s_work || !s_work2 || !s_chipbuf) {
        addLog("Hết bộ nhớ cho bộ đệm %u byte", (unsigned)size);
        return false;
    }
    s_size = size;
    s_has_data = false;
    s_diff_valid = false;
    return true;
}

bool startJob(Op op, const char *path = nullptr)
{
    if (s_busy) {
        addLog("Đang bận, chờ thao tác trước xong");
        return false;
    }
    const uint32_t size = CHIPS[s_chip].size;
    if (op != OP_SCAN) {
        if ((op == OP_WRITE || op == OP_VERIFY || op == OP_SAVE) && (!s_has_data || s_size != size)) {
            addLog("Chưa có dữ liệu trong bộ đệm (ĐỌC chip hoặc PHỤC HỒI từ file trước)");
            return false;
        }
        if (!ensureBuffers(size)) {
            return false;
        }
        if (op == OP_WRITE || op == OP_VERIFY || op == OP_SAVE) {
            memcpy(s_work, s_buf, size);
        }
    }
    s_job.op = op;
    s_job.cfg = currentConfig();
    s_job.path = path ? path : "";
    s_done = 0;
    s_total = 1;
    s_busy = true;
    if (xTaskCreatePinnedToCore(workerTask, "hcc_eeprom_ui", 6144, nullptr, 5, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_busy = false;
        addLog("Không tạo được task");
        return false;
    }
    setBusyUi(true);
    lv_bar_set_value(v.bar, 0, LV_ANIM_OFF);
    return true;
}

void applyResult(void)
{
    const Result r = s_res;
    const Chip &c = CHIPS[s_chip];
    setBusyUi(false);
    lv_bar_set_value(v.bar, 100, LV_ANIM_OFF);
    if (r.err != ESP_OK) {
        if (r.msg[0]) {
            addLog("%s", r.msg);
        } else if (r.op == OP_SAVE || r.op == OP_LOAD) {
            addLog("Lỗi thẻ nhớ / file (%s)", esp_err_to_name(r.err));
        } else {
            addLog("Lỗi I2C: %s (kiểm tra dây, nguồn, địa chỉ 0x%02X)", esp_err_to_name(r.err), s_addr);
        }
        return;
    }
    switch (r.op) {
    case OP_SCAN: {
        std::string list;
        for (int i = 0; i < 8; ++i) {
            const bool f = r.found & (1u << i);
            lv_obj_set_style_bg_color(v.found_btn[i], (0x50 + i == s_addr) ? C_BLUE : (f ? C_GREEN : C_BTN), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(v.found_btn[i], 0), f ? C_TEXT : C_MUTED, 0);
            if (f) {
                char b[8];
                snprintf(b, sizeof(b), " 0x%02X", 0x50 + i);
                list += b;
            }
        }
        addLog("Quét I2C (0x50-0x57): %s", list.empty() ? "không thấy thiết bị" : list.c_str());
        break;
    }
    case OP_READ:
        memcpy(s_buf, s_work, c.size);
        memcpy(s_ref, s_work, c.size);
        s_has_data = true;
        s_diff_valid = false;
        s_file.clear();
        addLog("Đọc xong %s @0x%02X: %u byte, CRC32 0x%08X", c.name, s_addr, (unsigned)c.size,
             (unsigned)crc32(s_buf, c.size));
        break;
    case OP_WRITE:
    case OP_ERASE:
        if (r.op == OP_ERASE) {
            memset(s_buf, 0xFF, c.size);
            s_has_data = true;
        }
        memcpy(s_chipbuf, s_work2, c.size);
        s_diff_valid = true;
        s_diff_total = r.diffs;
        s_diffs.clear();
        for (uint32_t i = 0; i < c.size && s_diffs.size() < MAX_DIFF; ++i) {
            if (s_buf[i] != s_chipbuf[i]) {
                s_diffs.push_back(i);
            }
        }
        if (r.diffs == 0) {
            memcpy(s_ref, s_buf, c.size);  // đã ghi: không còn byte "thay đổi"
            addLog("%s xong %u byte, đọc lại khớp hoàn toàn", r.op == OP_ERASE ? "Xoá (FF)" : "Ghi", (unsigned)c.size);
        } else {
            addLog("%s xong nhưng đọc lại lệch %u byte (đầu tiên 0x%04X) - xem tab SO SÁNH",
                 r.op == OP_ERASE ? "Xoá" : "Ghi", (unsigned)r.diffs, (unsigned)r.first_diff);
        }
        break;
    case OP_VERIFY:
        memcpy(s_chipbuf, s_work2, c.size);
        s_diff_valid = true;
        s_diff_total = r.diffs;
        s_diffs.clear();
        for (uint32_t i = 0; i < c.size && s_diffs.size() < MAX_DIFF; ++i) {
            if (s_buf[i] != s_chipbuf[i]) {
                s_diffs.push_back(i);
            }
        }
        if (r.diffs == 0) {
            addLog("Xác minh: chip khớp bộ đệm (%u byte)", (unsigned)c.size);
        } else {
            addLog("Xác minh: lệch %u byte, đầu tiên 0x%04X - xem tab SO SÁNH", (unsigned)r.diffs,
                 (unsigned)r.first_diff);
            setTab(1);
        }
        break;
    case OP_SAVE:
        s_file = s_job.path;
        addLog("Đã sao lưu %u byte -> /%s", (unsigned)c.size, s_job.path.c_str());
        break;
    case OP_LOAD:
        memcpy(s_buf, s_work, c.size);
        memcpy(s_ref, s_work, c.size);
        s_has_data = true;
        s_diff_valid = false;
        s_file = s_job.path;
        if (r.len < c.size) {
            addLog("Nạp /%s: %u byte (< %u, phần còn lại = FF)", s_job.path.c_str(), (unsigned)r.len, (unsigned)c.size);
        } else {
            addLog("Nạp /%s: %u byte. Bấm GHI để ghi vào chip", s_job.path.c_str(), (unsigned)c.size);
        }
        break;
    default:
        break;
    }
    renderHex();
    if (v.tab == 1) {
        renderCompare();
    } else if (v.tab == 2) {
        renderFiles();
    }
    updateInfo();
}

void onTimer(lv_timer_t *)
{
    if (s_busy) {
        const uint32_t t = s_total ? s_total : 1;
        lv_bar_set_value(v.bar, (int32_t)((uint64_t)s_done * 100 / t), LV_ANIM_OFF);
    } else if (s_serial != v.seen) {
        v.seen = s_serial;
        applyResult();
    }
    if (v.confirm_op != OP_NONE && lv_tick_elaps(v.confirm_tick) > 5000) {
        v.confirm_op = OP_NONE;
    }
}

// ---------- Sự kiện ----------

bool confirm(Op op, const char *name)
{
    if (v.confirm_op == op && lv_tick_elaps(v.confirm_tick) <= 5000) {
        v.confirm_op = OP_NONE;
        return true;
    }
    v.confirm_op = op;
    v.confirm_tick = lv_tick_get();
    addLog("Bấm %s lần nữa trong 5 giây để xác nhận (ghi đè chip %s @0x%02X)", name, CHIPS[s_chip].name, s_addr);
    return false;
}

void nextBackupName(char *out, size_t n)
{
    for (int k = 1; k < 1000; ++k) {
        snprintf(out, n, "eeprom/%s_%02X_%03d.bin", CHIPS[s_chip].name, s_addr, k);
        char full[96];
        snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, out);
        struct stat st;
        if (stat(full, &st) != 0) {
            return;
        }
    }
}

void onOp(lv_event_t *e)
{
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch (id) {
    case 0:
        if (startJob(OP_READ)) {
            addLog("Đọc EEPROM %s @0x%02X...", CHIPS[s_chip].name, s_addr);
        }
        break;
    case 1:
        if (!s_has_data) {
            addLog("Chưa có dữ liệu để ghi");
        } else if (confirm(OP_WRITE, "GHI") && startJob(OP_WRITE)) {
            addLog("Ghi %u byte vào %s @0x%02X...", (unsigned)CHIPS[s_chip].size, CHIPS[s_chip].name, s_addr);
        }
        break;
    case 2:
        if (startJob(OP_VERIFY)) {
            addLog("Xác minh: đọc chip và so với bộ đệm...");
        }
        break;
    case 3:
        if (confirm(OP_ERASE, "XOÁ") && startJob(OP_ERASE)) {
            addLog("Xoá chip (ghi FF toàn bộ)...");
        }
        break;
    case 4: {
        char name[64];
        nextBackupName(name, sizeof(name));
        if (startJob(OP_SAVE, name)) {
            addLog("Sao lưu -> /%s...", name);
        }
        break;
    }
    case 5:
        setTab(2);
        addLog("Chọn file .bin trong tab FILE để phục hồi vào bộ đệm");
        break;
    }
}

void onFileClicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = lv_list_get_btn_text(v.file_list, btn);
    if (name && startJob(OP_LOAD, name)) {
        addLog("Nạp /%s...", name);
        setTab(0);
    }
}

void onScan(lv_event_t *)
{
    if (startJob(OP_SCAN)) {
        addLog("Đang quét I2C...");
    }
}

void selectAddr(int a)
{
    s_addr = a;
    lv_dropdown_set_selected(v.addr_dd, a - 0x50);
    for (int i = 0; i < 8; ++i) {
        if (0x50 + i == a) {
            lv_obj_set_style_bg_color(v.found_btn[i], C_BLUE, 0);
        } else if (lv_color_to32(lv_obj_get_style_bg_color(v.found_btn[i], 0)) == lv_color_to32(C_BLUE)) {
            lv_obj_set_style_bg_color(v.found_btn[i], C_BTN, 0);
        }
    }
    updateInfo();
}

void onFoundAddr(lv_event_t *e)
{
    selectAddr(0x50 + (int)(intptr_t)lv_event_get_user_data(e));
    addLog("Chọn địa chỉ 0x%02X", s_addr);
}

void onAddrDd(lv_event_t *e)
{
    selectAddr(0x50 + lv_dropdown_get_selected(lv_event_get_target(e)));
}

void onChipDd(lv_event_t *e)
{
    const int c = lv_dropdown_get_selected(lv_event_get_target(e));
    if (s_busy) {
        lv_dropdown_set_selected(v.chip_dd, s_chip);
        return;
    }
    s_chip = c;
    if (s_size != CHIPS[c].size) {
        s_has_data = false;  // khác dung lượng: bộ đệm cũ không còn dùng được
        s_diff_valid = false;
    }
    v.page = 0;
    v.sel = 0;
    addLog("Đã chọn chip %s (%u byte, trang %u)", CHIPS[c].name, (unsigned)CHIPS[c].size, (unsigned)CHIPS[c].page);
    renderHex();
    updateInfo();
}

void onTab(lv_event_t *e)
{
    setTab((int)(intptr_t)lv_event_get_user_data(e));
}

void onPage(lv_event_t *e)
{
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    const int32_t p = (int32_t)v.page + d;
    if (p >= 0 && (uint32_t)p < pages()) {
        v.page = (uint32_t)p;
        renderHex();
    }
}

// ---------- Bàn phím hex ----------

void closeKeypad(void)
{
    if (v.keypad) {
        lv_obj_add_flag(v.keypad, LV_OBJ_FLAG_HIDDEN);
    }
    v.kp_mode = 0;
}

void updateKeypadTitle(void)
{
    char b[96];
    if (v.kp_mode == 1) {
        snprintf(b, sizeof(b), "Sửa byte 0x%04X  (hiện tại %02X)", (unsigned)v.sel, s_buf[v.sel]);
    } else {
        snprintf(b, sizeof(b), "Đến địa chỉ (hex, 0000 - %04X)", (unsigned)(CHIPS[s_chip].size - 1));
    }
    lv_label_set_text(v.kp_title, b);
    lv_label_set_text(v.kp_value, v.kp_text[0] ? v.kp_text : "_");
}

void openKeypad(int mode)
{
    v.kp_mode = mode;
    v.kp_text[0] = '\0';
    lv_obj_clear_flag(v.keypad, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v.keypad);
    updateKeypadTitle();
}

void gotoAddr(uint32_t a)
{
    if (a >= CHIPS[s_chip].size) {
        a = CHIPS[s_chip].size - 1;
    }
    v.goto_addr = a;
    v.page = a / 256;
    v.sel = (int32_t)a;
    char b[16];
    snprintf(b, sizeof(b), "0x%04X", (unsigned)a);
    lv_label_set_text(v.goto_lbl, b);
    renderHex();
    setTab(0);
}

void onKey(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    const uint16_t id = lv_btnmatrix_get_selected_btn(bm);
    const char *t = lv_btnmatrix_get_btn_text(bm, id);
    if (t == nullptr) {
        return;
    }
    if (strcmp(t, "ĐÓNG") == 0) {
        closeKeypad();
        return;
    }
    if (strcmp(t, "XOÁ") == 0) {
        v.kp_text[0] = '\0';
        updateKeypadTitle();
        return;
    }
    if (v.kp_mode == 1 && (strcmp(t, LV_SYMBOL_LEFT) == 0 || strcmp(t, LV_SYMBOL_RIGHT) == 0)) {
        int32_t s = v.sel + (strcmp(t, LV_SYMBOL_LEFT) == 0 ? -1 : 1);
        if (s >= 0 && (uint32_t)s < s_size) {
            v.sel = s;
            v.page = (uint32_t)s / 256;
            v.kp_text[0] = '\0';
            renderHex();
            updateKeypadTitle();
        }
        return;
    }
    if (strcmp(t, "OK") == 0) {
        if (v.kp_mode == 2 && v.kp_text[0]) {
            gotoAddr((uint32_t)strtoul(v.kp_text, nullptr, 16));
        }
        closeKeypad();
        return;
    }
    if (strlen(t) != 1 || !isxdigit((unsigned char)t[0])) {
        return;
    }
    size_t n = strlen(v.kp_text);
    const size_t maxlen = v.kp_mode == 1 ? 2 : 4;
    if (n < maxlen) {
        v.kp_text[n] = t[0];
        v.kp_text[n + 1] = '\0';
        ++n;
    }
    if (v.kp_mode == 1 && n == 2) {
        // Đủ 2 chữ số: ghi vào bộ đệm, sang byte kế tiếp.
        s_buf[v.sel] = (uint8_t)strtoul(v.kp_text, nullptr, 16);
        if ((uint32_t)v.sel + 1 < s_size) {
            ++v.sel;
            v.page = (uint32_t)v.sel / 256;
        }
        v.kp_text[0] = '\0';
        renderHex();
        updateInfo();
    }
    updateKeypadTitle();
}

void buildKeypad(lv_obj_t *root, int W, int H)
{
    v.keypad = lv_obj_create(root);
    lv_obj_remove_style_all(v.keypad);
    lv_obj_set_size(v.keypad, 420, 330);
    lv_obj_set_pos(v.keypad, (W - 420) / 2, (H - 330) / 2);
    lv_obj_set_style_bg_color(v.keypad, C_PANEL, 0);
    lv_obj_set_style_bg_opa(v.keypad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(v.keypad, C_TITLE, 0);
    lv_obj_set_style_border_width(v.keypad, 2, 0);
    lv_obj_set_style_radius(v.keypad, 12, 0);
    lv_obj_set_style_shadow_width(v.keypad, 30, 0);
    lv_obj_set_style_shadow_color(v.keypad, lv_color_black(), 0);
    lv_obj_clear_flag(v.keypad, LV_OBJ_FLAG_SCROLLABLE);
    v.kp_title = label(v.keypad, "", &lv_font_be_vietnam_16, C_TITLE);
    lv_obj_set_pos(v.kp_title, 14, 10);
    v.kp_value = label(v.keypad, "_", &lv_font_be_vietnam_28, C_TEXT);
    lv_obj_set_pos(v.kp_value, 14, 34);
    static const char *MAP[] = {"0", "1", "2", "3", "4", "5", "\n",
                                "6", "7", "8", "9", "A", "B", "\n",
                                "C", "D", "E", "F", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "\n",
                                "XOÁ", "OK", "ĐÓNG", ""};
    lv_obj_t *bm = lv_btnmatrix_create(v.keypad);
    lv_btnmatrix_set_map(bm, MAP);
    lv_obj_set_pos(bm, 6, 76);
    lv_obj_set_size(bm, 408, 248);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_set_style_pad_gap(bm, 6, 0);
    lv_obj_set_style_bg_color(bm, C_BTN, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, &lv_font_be_vietnam_20, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, onKey, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_flag(v.keypad, LV_OBJ_FLAG_HIDDEN);
}

void onTableEvent(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        uint16_t row = 0, col = 0;
        lv_table_get_selected_cell(t, &row, &col);
        if (row == LV_TABLE_CELL_NONE || row == 0 || col < 1 || col > 16) {
            return;
        }
        const int32_t i = (int32_t)(v.page * 256 + (row - 1) * 16 + (col - 1));
        if (!s_has_data || (uint32_t)i >= s_size) {
            return;
        }
        if (i == v.sel) {
            openKeypad(1);  // chạm lại ô đang chọn: sửa
        } else {
            v.sel = i;
            lv_obj_invalidate(t);
        }
        return;
    }
    if (code == LV_EVENT_DRAW_PART_BEGIN) {
        lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
        if (d->part != LV_PART_ITEMS) {
            return;
        }
        const uint32_t row = d->id / 18, col = d->id % 18;
        if (row == 0) {
            d->label_dsc->color = C_TITLE;
            return;
        }
        if (col == 0) {
            d->label_dsc->color = C_MUTED;
            return;
        }
        if (col == 17) {
            d->label_dsc->color = C_MUTED;
            return;
        }
        const uint32_t i = v.page * 256 + (row - 1) * 16 + (col - 1);
        if (!s_has_data || i >= s_size) {
            d->label_dsc->color = C_BORDER;
            return;
        }
        if ((int32_t)i == v.sel) {
            d->rect_dsc->bg_color = C_BLUE;
            d->rect_dsc->bg_opa = LV_OPA_COVER;
            d->label_dsc->color = C_TEXT;
        } else if (s_color_changed && s_buf[i] != s_ref[i]) {
            d->label_dsc->color = C_CHANGED;
        }
    }
}

void onGoto(lv_event_t *)
{
    openKeypad(2);
}

void onGotoGo(lv_event_t *)
{
    gotoAddr(v.goto_addr);
}

void onOption(lv_event_t *e)
{
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    const bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (id == 0) {
        s_show_ascii = on;
    } else {
        s_color_changed = on;
    }
    renderHex();
    lv_obj_invalidate(v.table);
}

void onClearLog(lv_event_t *)
{
    v.log_text.clear();
    v.log_lines = 0;
    lv_label_set_text(v.log, "");
}

// Trở về: chạy ngay khi CHẠM (không chờ nhả tay) - trên máy thật lần nhả tay đôi khi không thành CLICKED nên
// phải bấm nhiều lần. Chờ nhả tay rồi mới dựng màn mới (lv_async_call) để cú nhả không rơi vào nút của màn sau.
void doBack(void *)
{
    s_back_pending = false;
    if (s_back_cb == nullptr) {
        return;
    }
    lv_event_t ev = {};
    ev.user_data = s_back_user;
    s_back_cb(&ev);
}

void onBack(lv_event_t *)
{
    if (s_back_pending) {
        return;
    }
    s_back_pending = true;
    ESP_LOGI(TAG, "Trở về");
    lv_indev_t *in = lv_indev_get_act();
    if (in) {
        lv_indev_wait_release(in);
    }
    lv_async_call(doBack, nullptr);
}

void onRootDeleted(lv_event_t *)
{
    if (v.timer) {
        lv_timer_del(v.timer);
    }
    v = View();  // tác vụ đang chạy (nếu có) vẫn xong ở nền; bộ đệm giữ lại cho lần mở sau
}

lv_obj_t *infoRow(lv_obj_t *p, int y, const char *name)
{
    lv_obj_t *n = label(p, name, &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(n, 10, y);
    lv_obj_t *val = label(p, "-", &lv_font_be_vietnam_14, C_TEXT);
    lv_obj_set_pos(val, 96, y);
    lv_obj_set_width(val, 96);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    return val;
}

lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int sel, int x, int y, int w, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_pos(dd, x, y);
    lv_obj_set_size(dd, w, 38);
    lv_obj_set_style_pad_ver(dd, 8, 0);
    lv_obj_set_style_text_font(dd, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &lv_font_be_vietnam_18, 0);
    lv_obj_set_style_bg_color(dd, C_PANEL2, 0);
    lv_obj_set_style_text_color(dd, C_TEXT, 0);
    lv_obj_set_style_border_color(dd, C_BORDER, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

}  // namespace

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user, BusGetter get_bus, void *bus_user)
{
    if (v.root) {
        lv_obj_del(v.root);
    }
    s_get_bus = get_bus;
    s_back_cb = back_cb;
    s_back_user = back_user;
    s_back_pending = false;
    s_bus_user = bus_user;
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_t *root = lv_obj_create(screen);
    v.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    int W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    if (W < 900 || W > 1280) {
        W = 1024;
    }
    if (H < 400 || H > 800) {
        H = 527;
    }
    const int M = 6, LW = 204, RW = 204;
    const int CX = M + LW + M, CW = W - LW - RW - 4 * M, RX = W - M - RW;

    // ---------- Cột trái ----------
    lv_obj_t *p1 = panel(root, M, M, LW, 254, "1. KẾT NỐI & CẤU HÌNH");
    lv_obj_t *l = label(p1, "Chip", &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(l, 10, 38);
    std::string opts;
    for (int i = 0; i < NCHIP; ++i) {
        char b[48];
        snprintf(b, sizeof(b), "%s%s %s", i ? "\n" : "", CHIPS[i].name,
                 CHIPS[i].size >= 1024 ? (std::to_string(CHIPS[i].size / 1024) + "KB").c_str()
                                       : (std::to_string(CHIPS[i].size) + "B").c_str());
        opts += b;
    }
    v.chip_dd = dropdown(p1, opts.c_str(), s_chip, 60, 30, LW - 68, onChipDd);
    l = label(p1, "Địa chỉ", &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(l, 10, 82);
    v.addr_dd = dropdown(p1, "0x50\n0x51\n0x52\n0x53\n0x54\n0x55\n0x56\n0x57", s_addr - 0x50, 60, 74, LW - 68,
                         onAddrDd);
    button(p1, LV_SYMBOL_REFRESH "  QUÉT I2C", 10, 120, LW - 20, 40, C_BLUE, onScan, nullptr);
    l = label(p1, "ĐỊA CHỈ TÌM THẤY", &lv_font_be_vietnam_14, C_MUTED);
    lv_obj_set_pos(l, 10, 166);
    const int bw = (LW - 20 - 3 * 4) / 4;
    for (int i = 0; i < 8; ++i) {
        char b[8];
        snprintf(b, sizeof(b), "%02X", 0x50 + i);
        v.found_btn[i] = button(p1, b, 10 + (i % 4) * (bw + 4), 186 + (i / 4) * 32, bw, 28,
                                0x50 + i == s_addr ? C_BLUE : C_BTN, onFoundAddr, (void *)(intptr_t)i,
                                &lv_font_be_vietnam_14);
    }
    const int p2y = M + 254 + M;
    lv_obj_t *p2 = panel(root, M, p2y, LW, H - p2y - M, "2. THAO TÁC");
    static const char *OPS[] = {LV_SYMBOL_DOWNLOAD " ĐỌC", LV_SYMBOL_UPLOAD " GHI", LV_SYMBOL_OK " XÁC MINH",
                                LV_SYMBOL_TRASH " XOÁ (FF)", LV_SYMBOL_SAVE " SAO LƯU", LV_SYMBOL_DIRECTORY " PHỤC HỒI"};
    const lv_color_t OPC[] = {C_BLUE, C_GREEN, C_AMBER, C_RED, C_PURPLE, C_TEAL};
    const int ow = (LW - 24) / 2;
    const int oh = (H - p2y - M - 40 - 2 * 6 - 8) / 3;
    for (int i = 0; i < 6; ++i) {
        v.op_btn[i] = button(p2, OPS[i], 8 + (i % 2) * (ow + 8), 34 + (i / 2) * (oh + 6), ow, oh, OPC[i], onOp,
                             (void *)(intptr_t)i, &lv_font_be_vietnam_14);
    }

    // ---------- Giữa: tab + nội dung ----------
    static const char *TABS[] = {"HEX EDITOR", "SO SÁNH", "FILE"};
    for (int i = 0; i < 3; ++i) {
        v.tab_btn[i] = button(root, TABS[i], CX + i * 124, M, 118, 38, C_BTN, onTab, (void *)(intptr_t)i);
    }
    v.page_prev = button(root, LV_SYMBOL_UP, CX + CW - 44 - 112 - 44, M, 44, 38, C_BTN, onPage, (void *)(intptr_t)-1);
    v.page_lbl = label(root, "", &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_set_width(v.page_lbl, 112);
    lv_obj_set_style_text_align(v.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v.page_lbl, CX + CW - 44 - 112, M + 10);
    v.page_next = button(root, LV_SYMBOL_DOWN, CX + CW - 44, M, 44, 38, C_BTN, onPage, (void *)(intptr_t)1);

    const int cy = M + 38 + 4;
    const int log_h = 118;
    const int ch = H - cy - log_h - 2 * M;
    v.panel_hex = panel(root, CX, cy, CW, ch, nullptr);
    v.table = lv_table_create(v.panel_hex);
    lv_obj_set_pos(v.table, 2, 2);
    lv_obj_set_size(v.table, CW - 4, ch - 4);
    lv_obj_set_style_bg_opa(v.table, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v.table, 0, 0);
    lv_obj_set_style_pad_all(v.table, 0, 0);
    lv_obj_set_style_bg_opa(v.table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(v.table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.table, C_TEXT, LV_PART_ITEMS);
    // Bảng hex chỉ có chữ số / ASCII: font Montserrat (dòng thấp hơn font tiếng Việt) để vừa 16 dòng không cuộn.
    lv_obj_set_style_text_font(v.table, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(v.table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(v.table, 2, LV_PART_ITEMS);
    lv_obj_set_style_text_align(v.table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);
    lv_table_set_col_cnt(v.table, 18);
    lv_table_set_row_cnt(v.table, 17);
    const int hexw = (CW - 4 - 42 - ASCII_W - 4) / 16;
    lv_table_set_col_width(v.table, 0, 42);
    for (int r = 0; r < 17; ++r) {
        for (int c = 0; c < 18; ++c) {
            lv_table_add_cell_ctrl(v.table, r, c, LV_TABLE_CELL_CTRL_TEXT_CROP);  // không xuống dòng
        }
    }
    for (int c = 0; c < 16; ++c) {
        char b[4];
        snprintf(b, sizeof(b), "%02X", c);
        lv_table_set_col_width(v.table, c + 1, hexw);
        lv_table_set_cell_value(v.table, 0, c + 1, b);
    }
    lv_table_set_col_width(v.table, 17, ASCII_W);
    lv_table_set_cell_value(v.table, 0, 0, "ADR");
    lv_table_set_cell_value(v.table, 0, 17, "ASCII");
    lv_obj_add_event_cb(v.table, onTableEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(v.table, onTableEvent, LV_EVENT_DRAW_PART_BEGIN, nullptr);

    v.panel_cmp = panel(root, CX, cy, CW, ch, nullptr);
    v.cmp_title = label(v.panel_cmp, "", &lv_font_be_vietnam_16, C_TEXT);
    lv_obj_set_pos(v.cmp_title, 10, 8);
    v.cmp_table = lv_table_create(v.panel_cmp);
    lv_obj_set_pos(v.cmp_table, 6, 34);
    lv_obj_set_size(v.cmp_table, CW - 12, ch - 40);
    lv_obj_set_style_bg_opa(v.cmp_table, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v.cmp_table, 0, 0);
    lv_obj_set_style_bg_color(v.cmp_table, C_PANEL2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(v.cmp_table, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(v.cmp_table, &lv_font_be_vietnam_14, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(v.cmp_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_border_color(v.cmp_table, C_BORDER, LV_PART_ITEMS);
    lv_table_set_col_cnt(v.cmp_table, 3);
    lv_table_set_col_width(v.cmp_table, 0, 160);
    lv_table_set_col_width(v.cmp_table, 1, 160);
    lv_table_set_col_width(v.cmp_table, 2, 160);
    lv_table_set_cell_value(v.cmp_table, 0, 0, "Địa chỉ");
    lv_table_set_cell_value(v.cmp_table, 0, 1, "Bộ đệm");
    lv_table_set_cell_value(v.cmp_table, 0, 2, "Chip");

    v.panel_file = panel(root, CX, cy, CW, ch, nullptr);
    v.file_list = lv_list_create(v.panel_file);
    lv_obj_set_pos(v.file_list, 6, 6);
    lv_obj_set_size(v.file_list, CW - 12, ch - 12);
    lv_obj_set_style_bg_color(v.file_list, C_PANEL2, 0);
    lv_obj_set_style_border_width(v.file_list, 0, 0);

    // Log + tiến trình.
    const int ly = cy + ch + M;
    lv_obj_t *lp = panel(root, CX, ly, CW, log_h, "LOG HOẠT ĐỘNG");
    v.bar = lv_bar_create(lp);
    lv_obj_set_pos(v.bar, 160, 12);
    lv_obj_set_size(v.bar, CW - 160 - 100, 10);
    lv_obj_set_style_bg_color(v.bar, C_BTN, 0);
    lv_obj_set_style_bg_color(v.bar, C_OKC, LV_PART_INDICATOR);
    button(lp, LV_SYMBOL_TRASH " Xoá log", CW - 92, 4, 84, 26, C_BTN, onClearLog, nullptr, &lv_font_be_vietnam_14);
    v.log_box = lv_obj_create(lp);
    lv_obj_remove_style_all(v.log_box);
    lv_obj_set_pos(v.log_box, 8, 32);
    lv_obj_set_size(v.log_box, CW - 16, log_h - 38);
    lv_obj_set_scroll_dir(v.log_box, LV_DIR_VER);
    lv_obj_add_flag(v.log_box, LV_OBJ_FLAG_SCROLLABLE);
    v.log = label(v.log_box, "", &lv_font_be_vietnam_14, C_TEXT);
    lv_obj_set_width(v.log, CW - 22);
    lv_label_set_long_mode(v.log, LV_LABEL_LONG_WRAP);

    // ---------- Cột phải ----------
    lv_obj_t *r1 = panel(root, RX, M, RW, 158, "THÔNG TIN CHIP");
    static const char *IC[] = {"Tên chip", "Dung lượng", "Page size", "Địa chỉ I2C", "Giao tiếp", "Trạng thái"};
    for (int i = 0; i < 6; ++i) {
        v.info_chip[i] = infoRow(r1, 32 + i * 20, IC[i]);
    }
    const int r2y = M + 158 + M;
    lv_obj_t *r2 = panel(root, RX, r2y, RW, 116, "THÔNG TIN DỮ LIỆU");
    static const char *ID[] = {"Kích thước", "CRC32", "Byte đổi", "File"};
    for (int i = 0; i < 4; ++i) {
        v.info_data[i] = infoRow(r2, 32 + i * 20, ID[i]);
    }
    const int r3y = r2y + 116 + M;
    lv_obj_t *r3 = panel(root, RX, r3y, RW, 80, "ĐẾN ĐỊA CHỈ");
    lv_obj_t *gb = button(r3, "", 10, 34, 110, 38, C_PANEL2, onGoto, nullptr);
    v.goto_lbl = lv_obj_get_child(gb, 0);
    lv_label_set_text(v.goto_lbl, "0x0000");
    button(r3, "ĐI", 128, 34, RW - 138, 38, C_BLUE, onGotoGo, nullptr);
    const int r4y = r3y + 80 + M;
    const int r4h = H - r4y - M - 46 - M;
    lv_obj_t *r4 = panel(root, RX, r4y, RW, r4h, "TUỲ CHỌN HIỂN THỊ");
    static const char *OPT[] = {"Hiện ASCII", "Tô màu byte đổi"};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *cb = lv_checkbox_create(r4);
        lv_checkbox_set_text(cb, OPT[i]);
        lv_obj_set_pos(cb, 10, 30 + i * 28);
        lv_obj_set_style_text_font(cb, &lv_font_be_vietnam_14, 0);
        lv_obj_set_style_text_color(cb, C_TEXT, 0);
        if (i == 0 ? s_show_ascii : s_color_changed) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(cb, onOption, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    lv_obj_t *back = button(root, LV_SYMBOL_LEFT "  Trở về", RX, H - M - 46, RW, 46, C_BTN, nullptr, nullptr);
    lv_obj_add_event_cb(back, onBack, LV_EVENT_PRESSED, nullptr);

    buildKeypad(root, W, H);
    setTab(0);
    renderHex();
    updateInfo();
    setBusyUi(s_busy);
    v.seen = s_serial;
    v.timer = lv_timer_create(onTimer, 100, nullptr);
    addLog("Khởi động EEPROM TOOL - bus I2C GPIO4 (SDA) / GPIO5 (SCL)");
    if (s_has_data) {
        addLog("Bộ đệm còn dữ liệu lần trước: %u byte", (unsigned)s_size);
    }
}

}  // namespace HCCEepromView
