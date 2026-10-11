#include "HCCLogic.hpp"

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "HCCLogicFile.hpp"
#include "HCCRffe.hpp"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hcc_cores.h"
#include "sdkconfig.h"

namespace HCCLogic {

namespace {

const char *TAG = "HCCLogic";
constexpr size_t DESC_BYTES = 4032;
constexpr size_t NDESC = 16;
constexpr size_t RING_BYTES = DESC_BYTES * NDESC;

#ifndef CONFIG_HCC_LOGIC_CH0_GPIO
#define CONFIG_HCC_LOGIC_CH0_GPIO 20
#define CONFIG_HCC_LOGIC_CH1_GPIO 21
#define CONFIG_HCC_LOGIC_CH2_GPIO 22
#define CONFIG_HCC_LOGIC_CH3_GPIO 23
#define CONFIG_HCC_LOGIC_CH4_GPIO 26
#define CONFIG_HCC_LOGIC_CH5_GPIO 27
#define CONFIG_HCC_LOGIC_CH6_GPIO 47
#define CONFIG_HCC_LOGIC_CH7_GPIO 48
#endif

const int PINS[8] = {CONFIG_HCC_LOGIC_CH0_GPIO, CONFIG_HCC_LOGIC_CH1_GPIO, CONFIG_HCC_LOGIC_CH2_GPIO,
                     CONFIG_HCC_LOGIC_CH3_GPIO, CONFIG_HCC_LOGIC_CH4_GPIO, CONFIG_HCC_LOGIC_CH5_GPIO,
                     CONFIG_HCC_LOGIC_CH6_GPIO, CONFIG_HCC_LOGIC_CH7_GPIO};

// Chân đã dùng trên board (LCD, cảm ứng / I2C chung, âm thanh, thẻ SD, SDIO Wi-Fi C6, USB Serial/JTAG, UART0).
bool reserved(int p)
{
    static const int R[] = {7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 24, 25, 32, 33, 37, 38, 39, 40, 41, 42,
                            43, 44, 53, 54};
    for (int r : R) {
        if (r == p) {
            return true;
        }
    }
    return false;
}

struct Chunk {
    const uint8_t *data;
    uint32_t len;
};

SemaphoreHandle_t s_mux;
QueueHandle_t s_queue;
volatile uint32_t s_isr_overruns;
volatile bool s_running = false;
volatile bool s_stop_req = false;
volatile bool s_saving = false;
Status s_st;
Config s_cfg;
char s_path[160];
uint8_t *s_buf = nullptr;
uint32_t s_cap = 0;
HCCLogicCore::Mipmap s_mm;
bool s_save_ok = false;
char s_save_msg[200] = "";
char s_names[8][8];
uint32_t s_serial = 0;

void lock(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
}
void unlock(void)
{
    xSemaphoreGive(s_mux);
}

void ensureInit(void)
{
    if (s_mux == nullptr) {
        s_mux = xSemaphoreCreateMutex();
        s_queue = xQueueCreate(NDESC, sizeof(Chunk));
    }
}

bool ensureBuf(uint32_t depth, char *err, size_t es)
{
    if (s_buf && s_cap == depth) {
        return true;
    }
    heap_caps_free(s_buf);
    s_buf = nullptr;
    s_cap = 0;
    s_mm.clear();
    s_buf = (uint8_t *)heap_caps_malloc(depth, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == nullptr) {
        snprintf(err, es, "Hết PSRAM cho %u mẫu - giảm độ sâu", (unsigned)depth);
        return false;
    }
    s_cap = depth;
    return true;
}

bool IRAM_ATTR onPartial(parlio_rx_unit_handle_t, const parlio_rx_event_data_t *e, void *)
{
    Chunk c = {(const uint8_t *)e->data, (uint32_t)e->recv_bytes};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &c, &woken) != pdTRUE) {
        s_isr_overruns = s_isr_overruns + 1;
    }
    return woken == pdTRUE;
}

void setMsg(State st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void setMsg(State st, const char *fmt, ...)
{
    lock();
    s_st.state = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_st.msg, sizeof(s_st.msg), fmt, ap);
    va_end(ap);
    unlock();
}

// Ghi n byte vào vòng PSRAM tại vị trí tuyệt đối pos.
void ringWrite(uint64_t pos, const uint8_t *d, uint32_t n)
{
    uint32_t off = (uint32_t)(pos % s_cap);
    while (n > 0) {
        const uint32_t k = std::min<uint32_t>(n, s_cap - off);
        memcpy(s_buf + off, d, k);
        d += k;
        n -= k;
        off = 0;
    }
}

// Xếp lại [start, end) (vị trí tuyệt đối) thành mảng thẳng bắt đầu ở s_buf[0].
uint32_t linearize(uint64_t start, uint64_t end)
{
    const uint32_t n = (uint32_t)(end - start);
    const uint32_t off = (uint32_t)(start % s_cap);
    if (n == s_cap) {
        std::rotate(s_buf, s_buf + off, s_buf + s_cap);
    } else if (off + n <= s_cap) {
        memmove(s_buf, s_buf + off, n);
    } else {
        // Đoạn quấn vòng: [off, cap) + [0, n - (cap - off)). Xoay toàn bộ rồi lấy n byte đầu.
        std::rotate(s_buf, s_buf + off, s_buf + s_cap);
    }
    return n;
}

void finish(uint32_t samples, uint32_t trig, State st)
{
    s_mm.build(s_buf, samples);
    lock();
    s_st.samples = samples;
    s_st.trig = trig;
    s_st.state = st;
    s_st.running = false;
    s_st.serial = ++s_serial;
    unlock();
}

void captureTask(void *)
{
    const int64_t t0 = esp_timer_get_time();
    parlio_rx_unit_handle_t unit = nullptr;
    parlio_rx_delimiter_handle_t deli = nullptr;
    esp_err_t r = ESP_OK;
    uint8_t *ring = nullptr;
    bool claimed = false;
    uint64_t written = 0, valid_from = 0;
    int64_t trig_abs = -1;
    int prev = -1;
    uint32_t seen_ov = 0;
    const uint32_t pre = (uint32_t)((uint64_t)s_cap * s_cfg.pre_pct / 100);
    const uint32_t post = s_cap - pre;
    State end_state = State::DONE;
    bool hw_ok = false;
    uint32_t samples = 0, trig_rel = 0xFFFFFFFF;

    // Chân dùng chung với RFFE / SPMI: giữ chân để chức năng đó không chạy cùng lúc.
    for (int k = 0; k < 8; ++k) {
        if (PINS[k] >= 0 && (PINS[k] == HCCRffe::sclkPin() || PINS[k] == HCCRffe::sdataPin())) {
            claimed = true;
        }
    }
    if (claimed && !HCCRffe::claimPins()) {
        setMsg(State::ERROR, "Chân trùng RFFE / SPMI đang bận");
        claimed = false;
        goto out;
    }
    ring = (uint8_t *)heap_caps_aligned_calloc(64, 1, RING_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (ring == nullptr) {
        setMsg(State::ERROR, "Hết RAM nội cho bộ đệm DMA");
        goto out;
    }
    {
        parlio_rx_unit_config_t uc = {};
        uc.trans_queue_depth = 4;
        uc.max_recv_size = RING_BYTES;
        uc.data_width = 8;
        uc.clk_src = PARLIO_CLK_SRC_DEFAULT;
        uc.clk_in_gpio_num = GPIO_NUM_NC;
        uc.exp_clk_freq_hz = s_cfg.sample_hz;
        uc.clk_out_gpio_num = GPIO_NUM_NC;
        uc.valid_gpio_num = GPIO_NUM_NC;
        for (int i = 0; i < PARLIO_RX_UNIT_MAX_DATA_WIDTH; ++i) {
            uc.data_gpio_nums[i] = GPIO_NUM_NC;
        }
        for (int k = 0; k < 8; ++k) {
            uc.data_gpio_nums[k] = (gpio_num_t)PINS[k];
        }
        r = parlio_new_rx_unit(&uc, &unit);
    }
    if (r != ESP_OK) {
        setMsg(State::ERROR, "Không khởi tạo được PARLIO RX (%s)", esp_err_to_name(r));
        goto out;
    }
    for (int k = 0; k < 8; ++k) {
        if (PINS[k] >= 0) {
            gpio_pullup_dis((gpio_num_t)PINS[k]);
            gpio_pulldown_dis((gpio_num_t)PINS[k]);
        }
    }
    {
        parlio_rx_soft_delimiter_config_t dc = {};
        dc.sample_edge = PARLIO_SAMPLE_EDGE_POS;
        dc.bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB;
        dc.eof_data_len = 0xFFFF;
        dc.timeout_ticks = 0;
        r = parlio_new_rx_soft_delimiter(&dc, &deli);
    }
    if (r == ESP_OK) {
        parlio_rx_event_callbacks_t cbs = {};
        cbs.on_partial_receive = onPartial;
        r = parlio_rx_unit_register_event_callbacks(unit, &cbs, nullptr);
    }
    if (r == ESP_OK) {
        r = parlio_rx_unit_enable(unit, true);
    }
    if (r == ESP_OK) {
        r = parlio_rx_soft_delimiter_start_stop(unit, deli, true);
    }
    if (r == ESP_OK) {
        xQueueReset(s_queue);
        s_isr_overruns = 0;
        parlio_receive_config_t rc = {};
        rc.delimiter = deli;
        rc.flags.partial_rx_en = true;
        r = parlio_rx_unit_receive(unit, ring, RING_BYTES, &rc);
    }
    if (r != ESP_OK) {
        setMsg(State::ERROR, "Lỗi bật bộ thu PARLIO (%s)", esp_err_to_name(r));
        goto stop_hw;
    }
    hw_ok = true;
    setMsg(s_cfg.trig.type == HCCLogicCore::T_NONE ? State::CAPTURING : State::ARMED, "%s",
           s_cfg.trig.type == HCCLogicCore::T_NONE ? "Đang thu..." : "Chờ điểm kích...");
    while (true) {
        if (s_stop_req) {
            break;
        }
        Chunk c;
        if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(50)) != pdTRUE) {
            lock();
            s_st.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            unlock();
            continue;
        }
        const uint32_t ov = s_isr_overruns;
        const UBaseType_t waiting = uxQueueMessagesWaiting(s_queue);
        if (ov != seen_ov || waiting >= NDESC - 2) {
            seen_ov = ov;
            xQueueReset(s_queue);
            lock();
            ++s_st.lost;
            unlock();
            if (trig_abs >= 0) {
                end_state = State::OVERFLOW;  // sau kích: dừng, giữ phần liên tục
                break;
            }
            // Trước kích: bỏ phần cũ (không liên tục với phần sau), thu lại.
            valid_from = written;
            prev = -1;
            continue;
        }
        if (trig_abs < 0) {
            const long k = HCCLogicCore::findTrigger(s_cfg.trig, c.data, c.len, prev);
            if (k >= 0) {
                trig_abs = (int64_t)(written + (uint64_t)k);
                lock();
                s_st.state = State::CAPTURING;
                snprintf(s_st.msg, sizeof(s_st.msg), "Đã kích - đang thu phần sau...");
                unlock();
            }
        }
        // Chỉ chép phần cần: trước kích luôn chép (vòng); sau kích đến trig + post.
        uint32_t len = c.len;
        if (trig_abs >= 0 && written + len > (uint64_t)trig_abs + post) {
            len = (uint32_t)((uint64_t)trig_abs + post - written);
        }
        ringWrite(written, c.data, len);
        written += len;
        prev = c.data[c.len - 1];
        lock();
        s_st.written = (uint32_t)std::min<uint64_t>(written - valid_from, 0xFFFFFFFFu);
        s_st.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        unlock();
        if (trig_abs >= 0 && written >= (uint64_t)trig_abs + post) {
            break;
        }
    }
stop_hw:
    parlio_rx_soft_delimiter_start_stop(unit, deli, false);
    parlio_rx_unit_disable(unit);
out:
    if (deli) {
        parlio_del_rx_delimiter(deli);
    }
    if (unit) {
        parlio_del_rx_unit(unit);
    }
    heap_caps_free(ring);
    for (int k = 0; k < 8; ++k) {
        if (PINS[k] >= 0) {
            gpio_set_direction((gpio_num_t)PINS[k], GPIO_MODE_INPUT);
            gpio_pullup_dis((gpio_num_t)PINS[k]);
            gpio_pulldown_dis((gpio_num_t)PINS[k]);
        }
    }
    if (claimed) {
        HCCRffe::releasePins();
    }
    if (hw_ok) {
        const uint64_t end = written;
        uint64_t start = end > s_cap ? end - s_cap : 0;
        start = std::max(start, valid_from);
        if (end > start) {
            samples = linearize(start, end);
            if (trig_abs >= 0 && (uint64_t)trig_abs >= start) {
                trig_rel = (uint32_t)((uint64_t)trig_abs - start);
            }
        }
        if (trig_abs < 0 && s_cfg.trig.type != HCCLogicCore::T_NONE) {
            setMsg(end_state, "Dừng - không gặp điểm kích, giữ %u mẫu cuối", (unsigned)samples);
        } else if (end_state == State::OVERFLOW) {
            setMsg(end_state, "OVERFLOW: mất dữ liệu sau điểm kích - giữ %u mẫu liên tục. Giảm tần số lấy mẫu.",
                   (unsigned)samples);
        } else {
            setMsg(end_state, "Xong: %u mẫu", (unsigned)samples);
        }
        finish(samples, trig_rel, end_state);
    } else {
        lock();
        s_st.running = false;
        unlock();
    }
    ESP_LOGI(TAG, "Dừng: %u mẫu, kích %d, mất %u", (unsigned)samples, (int)trig_rel, (unsigned)s_st.lost);
    s_running = false;
    vTaskDelete(nullptr);
}

void demoTask(void *)
{
    const uint32_t trig = HCCLogicCore::makeDemo(s_buf, s_cap, s_cfg.sample_hz);
    lock();
    s_st.written = s_cap;
    snprintf(s_st.msg, sizeof(s_st.msg), "DEMO: dữ liệu mô phỏng, không phải kết quả đo (%u mẫu)", (unsigned)s_cap);
    unlock();
    finish(s_cap, trig, State::DONE);
    s_running = false;
    vTaskDelete(nullptr);
}

void loadTask(void *)
{
    HCCLogicFile::Info info;
    char err[200] = "";
    bool ok = HCCLogicFile::readLabInfo(s_path, info, err, sizeof(err));
    if (ok) {
        uint32_t want = info.samples;
        if (!ensureBuf(want, err, sizeof(err))) {
            want = std::min<uint32_t>(want, 8u * 1024 * 1024);
            ok = ensureBuf(want, err, sizeof(err));
        }
        if (ok) {
            ok = HCCLogicFile::loadLab(s_path, info, s_buf, s_cap, err, sizeof(err));
        }
    }
    if (ok) {
        lock();
        s_st.sample_hz = info.sample_hz;
        s_st.demo = (info.flags & HCCLogicFile::LF_DEMO) != 0;
        s_st.written = info.samples;
        const char *b = strrchr(s_path, '/');
        snprintf(s_st.msg, sizeof(s_st.msg), "Đã mở %s: %u mẫu%s%s", b ? b + 1 : s_path, (unsigned)info.samples,
                 (info.flags & HCCLogicFile::LF_PARTIAL) ? " (không đủ)" : "", s_st.demo ? " - DEMO" : "");
        unlock();
        finish(info.samples, info.trig, State::DONE);
        lock();
        for (int k = 0; k < 8; ++k) {
            memcpy(s_names[k], info.names[k], 8);
        }
        unlock();
    } else {
        setMsg(State::ERROR, "%s", err);
        lock();
        s_st.running = false;
        unlock();
    }
    s_running = false;
    vTaskDelete(nullptr);
}

void saveTask(void *)
{
    char base[96], path[112], err[200] = "";
    mkdir(BSP_SD_MOUNT_POINT "/logic", 0775);
    const Status st = status();
    for (int n = 1; n < 1000; ++n) {
        snprintf(base, sizeof(base), BSP_SD_MOUNT_POINT "/logic/la_%03d%s", n, st.demo ? "_DEMO" : "");
        snprintf(path, sizeof(path), "%s.lab", base);
        struct stat sb;
        if (stat(path, &sb) != 0) {
            break;
        }
    }
    HCCLogicFile::Info info;
    info.sample_hz = st.sample_hz;
    info.samples = st.samples;
    info.trig = st.trig;
    info.flags = 0;
    if (st.demo) {
        info.flags |= HCCLogicFile::LF_DEMO;
    }
    if (st.state == State::OVERFLOW) {
        info.flags |= HCCLogicFile::LF_PARTIAL;
    }
    memcpy(info.names, s_names, sizeof(info.names));
    bool ok = HCCLogicFile::saveLab(path, info, s_buf, err, sizeof(err));
    if (ok) {
        snprintf(path, sizeof(path), "%s.vcd", base);
        ok = HCCLogicFile::exportVcd(path, info, s_buf, 0xFF, err, sizeof(err));
    }
    if (ok) {
        snprintf(s_save_msg, sizeof(s_save_msg), "Đã lưu %s.lab + .vcd (mở .vcd bằng PulseView)",
                 base + strlen(BSP_SD_MOUNT_POINT) + 1);
    } else {
        snprintf(s_save_msg, sizeof(s_save_msg), "%s", err);
    }
    s_save_ok = ok;
    s_saving = false;
    vTaskDelete(nullptr);
}

bool begin(const Config &cfg, bool demo, char *err, size_t es)
{
    ensureInit();
    if (s_running || s_saving) {
        snprintf(err, es, "Đang bận");
        return false;
    }
    if (!ensureBuf(cfg.depth, err, es)) {
        return false;
    }
    s_cfg = cfg;
    s_stop_req = false;
    lock();
    s_st = Status();
    s_st.serial = s_serial;
    s_st.running = true;
    s_st.demo = demo;
    s_st.sample_hz = cfg.sample_hz;
    s_st.state = demo ? State::CAPTURING : State::ARMED;
    unlock();
    s_mm.clear();
    s_running = true;
    if (xTaskCreatePinnedToCore(demo ? demoTask : captureTask, "hcc_logic", 6144, nullptr, 6, nullptr, HCC_CORE_WORK) !=
        pdPASS) {
        s_running = false;
        lock();
        s_st.running = false;
        s_st.state = State::ERROR;
        unlock();
        snprintf(err, es, "Không tạo được task");
        return false;
    }
    return true;
}

}  // namespace

const char *stateName(State s)
{
    switch (s) {
    case State::IDLE: return "READY";
    case State::ARMED: return "WAIT TRIGGER";
    case State::CAPTURING: return "CAPTURING";
    case State::DONE: return "DONE";
    case State::OVERFLOW: return "OVERFLOW";
    case State::ERROR: return "ERROR";
    case State::NO_PINS: return "NO PINS";
    }
    return "?";
}

int pin(int ch)
{
    return (ch >= 0 && ch < 8) ? PINS[ch] : -1;
}

const char *pinError(void)
{
    static char b[120];
    int used = 0;
    for (int k = 0; k < 8; ++k) {
        const int p = PINS[k];
        if (p < 0) {
            continue;
        }
        ++used;
        if (reserved(p)) {
            snprintf(b, sizeof(b), "D%d = GPIO%d đã dùng trên board (LCD / SD / âm thanh / Wi-Fi / USB) - đổi trong menuconfig", k, p);
            return b;
        }
        for (int j = 0; j < k; ++j) {
            if (PINS[j] == p) {
                snprintf(b, sizeof(b), "D%d và D%d trùng GPIO%d", j, k, p);
                return b;
            }
        }
    }
    if (used == 0) {
        return "Chưa cấu hình chân (menuconfig > HCC LOGIC ANALYZER)";
    }
    return nullptr;
}

bool start(const Config &cfg, char *err, size_t es)
{
    ensureInit();
    if (const char *e = pinError()) {
        lock();
        s_st.state = State::NO_PINS;
        snprintf(s_st.msg, sizeof(s_st.msg), "%s", e);
        unlock();
        snprintf(err, es, "%s", e);
        return false;
    }
    return begin(cfg, false, err, es);
}

bool startDemo(const Config &cfg, char *err, size_t es)
{
    return begin(cfg, true, err, es);
}

bool startLoad(const char *path, char *err, size_t es)
{
    ensureInit();
    if (s_running || s_saving) {
        snprintf(err, es, "Đang bận");
        return false;
    }
    snprintf(s_path, sizeof(s_path), "%s", path);
    lock();
    s_st = Status();
    s_st.serial = s_serial;
    s_st.running = true;
    s_st.file = true;
    s_st.state = State::CAPTURING;
    snprintf(s_st.msg, sizeof(s_st.msg), "Đang đọc file...");
    unlock();
    s_running = true;
    if (xTaskCreatePinnedToCore(loadTask, "hcc_logic_ld", 6144, nullptr, 4, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_running = false;
        lock();
        s_st.running = false;
        unlock();
        snprintf(err, es, "Không tạo được task");
        return false;
    }
    return true;
}

void stop(void)
{
    s_stop_req = true;
}

Status status(void)
{
    ensureInit();
    lock();
    Status s = s_st;
    unlock();
    return s;
}

const uint8_t *data(void)
{
    return s_buf;
}

const HCCLogicCore::Mipmap &mipmap(void)
{
    return s_mm;
}

bool startSave(const char names[8][8])
{
    ensureInit();
    const Status st = status();
    if (s_running || s_saving || st.samples == 0) {
        return false;
    }
    memcpy(s_names, names, sizeof(s_names));
    s_saving = true;
    s_save_msg[0] = '\0';
    if (xTaskCreatePinnedToCore(saveTask, "hcc_logic_sv", 6144, nullptr, 4, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_saving = false;
        return false;
    }
    return true;
}

bool saving(void)
{
    return s_saving;
}

bool fileNames(char out[8][8])
{
    ensureInit();
    lock();
    const bool f = s_st.file && !s_st.running;
    if (f) {
        memcpy(out, s_names, sizeof(s_names));
    }
    unlock();
    return f;
}

bool saveResult(char *msg, size_t size)
{
    snprintf(msg, size, "%s", s_save_msg);
    return s_save_ok;
}

}  // namespace HCCLogic
