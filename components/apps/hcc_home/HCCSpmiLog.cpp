#include "HCCSpmiLog.hpp"

#include <algorithm>
#include <map>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

namespace HCCSpmiLog {

using HCCSpmiDecode::Txn;

namespace {

const char *TAG = "HCCSpmiLog";
constexpr size_t DESC_BYTES = 4032;              // bội 64, vừa một mô tả DMA
constexpr size_t NDESC = 16;
constexpr size_t RING_BYTES = DESC_BYTES * NDESC;  // ~63 KB RAM nội
constexpr uint32_t MAX_TXN = 150000;               // ~3,6 MB PSRAM

struct Chunk {
    const uint8_t *data;
    uint32_t len;
};

SemaphoreHandle_t s_mux;
QueueHandle_t s_queue;
volatile bool s_running = false;
volatile bool s_stop_req = false;
volatile uint32_t s_isr_overruns = 0;
Status s_status;
Config s_cfg;
Txn *s_txns = nullptr;
volatile uint32_t s_count = 0;
std::map<uint32_t, Stat> s_stats;
HCCSpmiDecode::Decoder s_dec;
uint32_t s_sample_hz = 0;
volatile bool s_saving = false;
bool s_save_ok = false;
char s_save_msg[160] = "";

void lock(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
}

void unlock(void)
{
    xSemaphoreGive(s_mux);
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

void onTxn(const Txn &t, void *)
{
    lock();
    if (s_count < MAX_TXN) {
        s_txns[s_count] = t;
        s_count = s_count + 1;
    } else {
        ++s_status.dropped_txns;
    }
    ++s_status.txns;
    if (t.parity_err) {
        ++s_status.parity_errs;
    }
    if (t.kind == HCCSpmiDecode::K_BAD) {
        ++s_status.bad_frames;
    }
    if (t.short_bits) {
        ++s_status.short_frames;
    }
    const bool rd = HCCSpmiDecode::isRead(t.kind);
    const bool wr = HCCSpmiDecode::isWrite(t.kind);
    if ((rd || wr) && t.len > 0 && !t.parity_err && !t.short_bits) {
        const bool ext = t.kind != HCCSpmiDecode::K_REG_WRITE && t.kind != HCCSpmiDecode::K_REG_READ &&
                         t.kind != HCCSpmiDecode::K_REG0_WRITE;
        // Nhiều byte: byte i ứng với địa chỉ addr + i.
        for (int i = 0; i < t.len; ++i) {
            const uint16_t a = (uint16_t)(t.addr + i);
            const uint32_t key = ((uint32_t)t.sid << 24) | ((uint32_t)ext << 16) | a;
            auto it = s_stats.find(key);
            if (it == s_stats.end()) {
                if (s_stats.size() >= 8192) {
                    continue;
                }
                Stat st = {};
                st.sid = t.sid;
                st.addr = a;
                st.ext = ext;
                it = s_stats.emplace(key, st).first;
            }
            Stat &st = it->second;
            if (wr) {
                if (!st.has_w) {
                    st.first_w = t.data[i];
                }
                st.has_w = true;
                st.last_w = t.data[i];
                ++st.writes;
            } else {
                st.has_r = true;
                st.last_r = t.data[i];
                ++st.reads;
            }
        }
    }
    unlock();
}

// Chưa biết dây nào là SCLK: đếm số lần đổi mức của từng dây (SCLK đổi nhiều gấp ~2 lần SDATA).
uint32_t s_edges[2];
int s_prev = -1;

bool detectLines(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        uint8_t v = d[i];
        if (v == 0 && s_prev == 0) {
            continue;
        }
        for (int k = 0; k < 4; ++k) {
            const int s = v & 3;
            v >>= 2;
            if (s_prev >= 0) {
                if ((s ^ s_prev) & 1) {
                    ++s_edges[0];
                }
                if ((s ^ s_prev) & 2) {
                    ++s_edges[1];
                }
            }
            s_prev = s;
        }
    }
    return s_edges[0] + s_edges[1] >= 400;
}

void captureTask(void *)
{
    parlio_rx_unit_handle_t unit = nullptr;
    parlio_rx_delimiter_handle_t deli = nullptr;
    uint8_t *ring = nullptr;
    const int pin_clk = HCCRffe::sclkPin();
    const int pin_dat = HCCRffe::sdataPin();
    esp_err_t r = ESP_OK;
    bool lines_ok = false;
    uint32_t seen_overruns = 0;
    int64_t t0 = 0;

    ring = (uint8_t *)heap_caps_aligned_calloc(64, 1, RING_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (ring == nullptr) {
        snprintf(s_status.msg, sizeof(s_status.msg), "Hết RAM nội cho bộ đệm DMA (%u KB)", (unsigned)(RING_BYTES / 1024));
        goto out;
    }
    {
        parlio_rx_unit_config_t uc = {};
        uc.trans_queue_depth = 4;
        uc.max_recv_size = RING_BYTES;
        uc.data_width = 2;
        uc.clk_src = PARLIO_CLK_SRC_DEFAULT;
        uc.ext_clk_freq_hz = 0;
        uc.clk_in_gpio_num = GPIO_NUM_NC;
        uc.exp_clk_freq_hz = s_cfg.sample_hz;
        uc.clk_out_gpio_num = GPIO_NUM_NC;
        uc.valid_gpio_num = GPIO_NUM_NC;
        for (int i = 0; i < PARLIO_RX_UNIT_MAX_DATA_WIDTH; ++i) {
            uc.data_gpio_nums[i] = GPIO_NUM_NC;
        }
        uc.data_gpio_nums[0] = (gpio_num_t)pin_clk;  // bit 0 = SCLK
        uc.data_gpio_nums[1] = (gpio_num_t)pin_dat;  // bit 1 = SDATA
        r = parlio_new_rx_unit(&uc, &unit);
    }
    if (r != ESP_OK) {
        snprintf(s_status.msg, sizeof(s_status.msg), "Không khởi tạo được PARLIO RX (%s)", esp_err_to_name(r));
        goto out;
    }
    // Chỉ nghe: không kéo bus lên / xuống.
    gpio_pullup_dis((gpio_num_t)pin_clk);
    gpio_pulldown_dis((gpio_num_t)pin_clk);
    gpio_pullup_dis((gpio_num_t)pin_dat);
    gpio_pulldown_dis((gpio_num_t)pin_dat);
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
        parlio_receive_config_t rc = {};
        rc.delimiter = deli;
        rc.flags.partial_rx_en = true;
        r = parlio_rx_unit_receive(unit, ring, RING_BYTES, &rc);
    }
    if (r != ESP_OK) {
        snprintf(s_status.msg, sizeof(s_status.msg), "Lỗi bật bộ thu PARLIO (%s)", esp_err_to_name(r));
        goto stop_hw;
    }

    t0 = esp_timer_get_time();
    s_edges[0] = s_edges[1] = 0;
    s_prev = -1;
    // Hết khoảng nghỉ ~ 2 µs (hoặc 32 mẫu) coi là hết chuỗi lệnh.
    s_dec.reset(std::max<uint32_t>(32, s_cfg.sample_hz / 500000), onTxn, nullptr);
    while (!s_stop_req) {
        const int64_t el = (esp_timer_get_time() - t0) / 1000;
        lock();
        s_status.elapsed_ms = (uint32_t)el;
        unlock();
        if (el >= (int64_t)s_cfg.seconds * 1000) {
            break;
        }
        Chunk c;
        if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        // Mất dữ liệu: DMA đã ghi đè trước khi giải mã -> bỏ chuỗi đang dở.
        const uint32_t ov = s_isr_overruns;
        if (ov != seen_overruns || uxQueueMessagesWaiting(s_queue) >= NDESC - 2) {
            seen_overruns = ov;
            lock();
            ++s_status.overruns;
            unlock();
            s_dec.resync();
            xQueueReset(s_queue);
            continue;
        }
        if (!lines_ok) {
            if (detectLines(c.data, c.len)) {
                lines_ok = true;
                s_dec.swap = s_edges[1] > s_edges[0];  // dây đổi mức nhiều hơn là SCLK
                lock();
                s_status.lines_found = true;
                s_status.swapped = s_dec.swap;
                unlock();
                ESP_LOGI(TAG, "Nhận vai dây: SCLK = GPIO%d (đổi mức %u / %u)", s_dec.swap ? pin_dat : pin_clk,
                         (unsigned)s_edges[0], (unsigned)s_edges[1]);
            }
            continue;
        }
        s_dec.feed(c.data, c.len);
        if (s_dec.sclkHighSamples() > 0) {
            const uint64_t falls = s_dec.sclkFalls();
            const uint32_t n = s_count;
            if (falls > n) {
                lock();
                s_status.sclk_mhz =
                    (float)((double)s_cfg.sample_hz * (double)(falls - n) / (double)s_dec.sclkHighSamples() / 1e6);
                unlock();
            }
        }
    }
    s_dec.flush();

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
    // Trả chân về vào-thả nổi (không dùng gpio_reset_pin: hàm đó bật kéo lên, sẽ kéo bus SPMI đang nối lên 3,3 V).
    for (int p : {pin_clk, pin_dat}) {
        gpio_set_direction((gpio_num_t)p, GPIO_MODE_INPUT);
        gpio_pullup_dis((gpio_num_t)p);
        gpio_pulldown_dis((gpio_num_t)p);
    }
    HCCRffe::releasePins();
    lock();
    if (s_status.msg[0] == '\0') {
        if (!s_status.lines_found) {
            snprintf(s_status.msg, sizeof(s_status.msg),
                     "Không thấy hoạt động trên SCLK / SDATA (dây, mạch chuyển mức, main đã cấp nguồn?)");
        } else {
            snprintf(s_status.msg, sizeof(s_status.msg), "Xong: %u giao dịch, SCLK ~%.2f MHz", (unsigned)s_status.txns,
                     s_status.sclk_mhz);
        }
    }
    s_status.running = false;
    s_status.done = true;
    unlock();
    ESP_LOGI(TAG, "%s (tràn %u, parity %u)", s_status.msg, (unsigned)s_status.overruns, (unsigned)s_status.parity_errs);
    s_running = false;
    vTaskDelete(nullptr);
}

void saveTask(void *)
{
    char path[96] = "";
    bool ok = false;
    mkdir(BSP_SD_MOUNT_POINT "/spmi", 0775);
    for (int n = 1; n < 1000; ++n) {
        snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/spmi/spmi_%03d.csv", n);
        struct stat st;
        if (stat(path, &st) != 0) {
            break;
        }
    }
    FILE *f = fopen(path, "w");
    if (f == nullptr) {
        snprintf(s_save_msg, sizeof(s_save_msg), "Không ghi được thẻ SD (%s)", path);
    } else {
        setvbuf(f, nullptr, _IOFBF, 16 * 1024);
        fprintf(f, "stt,thoi_gian_us,sid,lenh_hex,loai,dia_chi_hex,so_byte,du_lieu_hex,loi_parity,thieu_bit,bit_du\n");
        const uint32_t n = s_count;
        const double us_per = 1e6 / (double)(s_sample_hz ? s_sample_hz : 1);
        for (uint32_t i = 0; i < n; ++i) {
            const Txn &t = s_txns[i];
            char data[32] = "";
            int k = 0;
            for (int j = 0; j < t.len && j < 8; ++j) {
                k += snprintf(data + k, sizeof(data) - k, "%s%02X", j ? " " : "", t.data[j]);
            }
            fprintf(f, "%u,%.2f,%u,%02X,%s,%04X,%u,%s,%u,%u,%u\n", (unsigned)(i + 1), t.t_sample * us_per,
                    (unsigned)t.sid, t.cmd, HCCSpmiDecode::kindName(t.kind), t.addr, (unsigned)t.len, data,
                    (unsigned)t.parity_err, (unsigned)t.short_bits, (unsigned)t.extra_bits);
        }
        ok = fclose(f) == 0;
        if (ok) {
            snprintf(s_save_msg, sizeof(s_save_msg), "Đã lưu %u giao dịch: %s", (unsigned)n, path);
        } else {
            snprintf(s_save_msg, sizeof(s_save_msg), "Lỗi ghi file %s", path);
        }
    }
    s_save_ok = ok;
    s_saving = false;
    vTaskDelete(nullptr);
}

}  // namespace

bool start(const Config &cfg, char *err, size_t err_size)
{
    if (s_mux == nullptr) {
        s_mux = xSemaphoreCreateMutex();
        s_queue = xQueueCreate(NDESC, sizeof(Chunk));
    }
    if (s_running || s_saving) {
        snprintf(err, err_size, "Đang bận");
        return false;
    }
    if (!HCCRffe::configured()) {
        snprintf(err, err_size, "%s", HCCRffe::configError());
        return false;
    }
    if (s_txns == nullptr) {
        s_txns = (Txn *)heap_caps_malloc(sizeof(Txn) * MAX_TXN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_txns == nullptr) {
            snprintf(err, err_size, "Hết PSRAM cho bộ nhớ log");
            return false;
        }
    }
    if (!HCCRffe::claimPins()) {
        snprintf(err, err_size, "Chân SCLK / SDATA đang bận (đang quét RFFE / SPMI)");
        return false;
    }
    s_cfg = cfg;
    if (s_cfg.seconds < 10) {
        s_cfg.seconds = 10;
    }
    if (s_cfg.seconds > 90) {
        s_cfg.seconds = 90;
    }
    s_sample_hz = s_cfg.sample_hz;
    lock();
    s_status = Status();
    s_status.running = true;
    s_status.total_ms = s_cfg.seconds * 1000;
    s_status.sample_hz = s_cfg.sample_hz;
    s_count = 0;
    s_stats.clear();
    unlock();
    s_isr_overruns = 0;
    s_stop_req = false;
    xQueueReset(s_queue);
    s_running = true;
    if (xTaskCreatePinnedToCore(captureTask, "hcc_spmi_log", 6144, nullptr, 6, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_running = false;
        s_status.running = false;
        HCCRffe::releasePins();
        snprintf(err, err_size, "Không tạo được task");
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
    if (s_mux == nullptr) {
        return Status();
    }
    lock();
    Status st = s_status;
    unlock();
    return st;
}

uint32_t count(void)
{
    return s_count;
}

const Txn *txn(uint32_t i)
{
    return (s_txns && i < s_count) ? &s_txns[i] : nullptr;
}

uint32_t sampleHz(void)
{
    return s_sample_hz;
}

std::vector<Stat> stats(void)
{
    std::vector<Stat> v;
    if (s_mux == nullptr) {
        return v;
    }
    lock();
    v.reserve(s_stats.size());
    for (const auto &kv : s_stats) {
        v.push_back(kv.second);
    }
    unlock();
    return v;  // std::map đã sắp theo khoá SID / loại / địa chỉ
}

bool startSave(void)
{
    if (s_running || s_saving || s_count == 0) {
        return false;
    }
    s_saving = true;
    s_save_msg[0] = '\0';
    if (xTaskCreatePinnedToCore(saveTask, "hcc_spmi_csv", 4096, nullptr, 4, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_saving = false;
        return false;
    }
    return true;
}

bool saving(void)
{
    return s_saving;
}

bool saveResult(char *msg, size_t size)
{
    snprintf(msg, size, "%s", s_save_msg);
    return s_save_ok;
}

}  // namespace HCCSpmiLog
