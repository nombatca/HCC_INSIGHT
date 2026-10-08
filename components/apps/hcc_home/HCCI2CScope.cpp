#include "HCCI2CScope.hpp"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hcc_cores.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#endif

namespace HCCI2CScope {

// ==================== Giải mã ====================

std::vector<Event> decode(const uint8_t *s, int n)
{
    std::vector<Event> ev;
    bool in_tx = false;
    bool first = false;  // byte tiếp theo là byte địa chỉ
    int bits = 0;
    uint8_t byte = 0;
    int byte_start = 0;
    for (int i = 1; i < n; ++i) {
        const int scl0 = s[i - 1] & BIT_SCL, scl1 = s[i] & BIT_SCL;
        const int sda0 = (s[i - 1] & BIT_SDA) ? 1 : 0, sda1 = (s[i] & BIT_SDA) ? 1 : 0;
        if (scl0 && scl1 && sda0 != sda1) {
            if (sda0 && !sda1) {  // SDA xuống khi SCL cao: START / START lặp
                ev.push_back({in_tx ? Event::Restart : Event::Start, i, i, 0, false});
                in_tx = true;
                first = true;
                bits = 0;
                byte = 0;
            } else {  // SDA lên khi SCL cao: STOP
                ev.push_back({Event::Stop, i, i, 0, false});
                in_tx = false;
            }
            continue;
        }
        if (!in_tx || !(scl1 && !scl0)) {
            continue;
        }
        // Cạnh lên SCL: đọc SDA.
        if (bits < 8) {
            if (bits == 0) {
                byte_start = i;
            }
            byte = (uint8_t)((byte << 1) | sda1);
            ++bits;
            if (bits == 8) {
                ev.push_back({Event::Byte, byte_start, i, byte, first});
            }
        } else {
            ev.push_back({sda1 ? Event::Nack : Event::Ack, i, i, 0, false});
            bits = 0;
            byte = 0;
            first = false;
        }
    }
    return ev;
}

float sclFrequency(const uint8_t *s, int n, float period_us)
{
    int first = -1, last = -1, count = 0;
    for (int i = 1; i < n; ++i) {
        if ((s[i] & BIT_SCL) && !(s[i - 1] & BIT_SCL)) {
            if (first < 0) {
                first = i;
            }
            last = i;
            ++count;
        }
    }
    if (count < 3 || last <= first || period_us <= 0) {
        return 0;
    }
    // Trung bình khoảng cách các cạnh lên (bỏ khoảng nghỉ dài giữa các byte: lấy trung vị đơn giản).
    std::vector<int> d;
    int prev = -1;
    for (int i = 1; i < n; ++i) {
        if ((s[i] & BIT_SCL) && !(s[i - 1] & BIT_SCL)) {
            if (prev >= 0) {
                d.push_back(i - prev);
            }
            prev = i;
        }
    }
    // trung vị
    for (size_t a = 0; a < d.size(); ++a) {
        for (size_t b = a + 1; b < d.size(); ++b) {
            if (d[b] < d[a]) {
                const int t = d[a];
                d[a] = d[b];
                d[b] = t;
            }
        }
    }
    const int med = d[d.size() / 2];
    return med > 0 ? 1e6f / (med * period_us) : 0;
}

#ifdef ESP_PLATFORM

// ==================== Chụp ====================

namespace {

const char *TAG = "HCCI2CScope";
constexpr int PIN_SDA = 4;  // bus I2C ngoài (giống I2C SCANNER / EEPROM)
constexpr int PIN_SCL = 5;
constexpr int RING = 8192;  // số mẫu (lũy thừa 2)
constexpr int PRE = RING / 8;

DRAM_ATTR uint8_t s_ring[RING];
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_busy = false;
volatile uint32_t s_serial = 0;
volatile bool s_armed = false;
Capture s_cap;
Request s_req;
BusGetter s_get_bus = nullptr;
void *s_user = nullptr;
SemaphoreHandle_t s_done = nullptr;

struct CapState {
    uint32_t period_cycles;
    uint32_t window_cycles;  // thời gian tối đa cho một lần tắt ngắt
    int total;               // số mẫu đã ghi (vòng)
    int trig;                // chỉ số tuyệt đối mẫu kích, -1 chưa kích
    int overruns;
};

inline uint8_t readPins(uint32_t reg_scl, uint32_t m_scl, uint32_t reg_sda, uint32_t m_sda)
{
    const uint32_t a = REG_READ(reg_scl);
    const uint32_t b = reg_sda == reg_scl ? a : REG_READ(reg_sda);
    return (uint8_t)(((a & m_scl) ? BIT_SCL : 0) | ((b & m_sda) ? BIT_SDA : 0));
}

// Một cửa sổ chụp (ngắt bị tắt). Trả true khi xong (đã kích và đủ mẫu sau kích).
bool IRAM_ATTR captureWindow(CapState &st)
{
    const uint32_t reg_scl = PIN_SCL < 32 ? GPIO_IN_REG : GPIO_IN1_REG;
    const uint32_t reg_sda = PIN_SDA < 32 ? GPIO_IN_REG : GPIO_IN1_REG;
    const uint32_t m_scl = 1u << (PIN_SCL & 31);
    const uint32_t m_sda = 1u << (PIN_SDA & 31);
    const uint32_t t0 = esp_cpu_get_cycle_count();
    uint32_t t = t0;
    while (true) {
        const uint8_t v = readPins(reg_scl, m_scl, reg_sda, m_sda);
        s_ring[st.total & (RING - 1)] = v;
        if (st.trig < 0 && v != (BIT_SCL | BIT_SDA) && st.total >= PRE) {
            st.trig = st.total;
        }
        ++st.total;
        if (st.trig >= 0 && st.total - st.trig >= RING - PRE) {
            return true;
        }
        t += st.period_cycles;
        const uint32_t now = esp_cpu_get_cycle_count();
        if ((int32_t)(now - t) > (int32_t)st.period_cycles) {
            ++st.overruns;
            t = now;  // trễ quá một chu kỳ: bắt kịp, giữ nhịp
        }
        while ((int32_t)(esp_cpu_get_cycle_count() - t) < 0) {
        }
        if (st.trig < 0 && (uint32_t)(t - t0) > st.window_cycles) {
            return false;  // hết cửa sổ, chưa kích
        }
    }
}

void captureTask(void *)
{
    const uint32_t mhz = (uint32_t)(esp_clk_cpu_freq() / 1000000);
    CapState st = {};
    st.period_cycles = (uint32_t)(s_req.period_us * mhz + 0.5f);
    if (st.period_cycles < 8) {
        st.period_cycles = 8;
    }
    st.window_cycles = 20 * 1000 * mhz;  // tối đa 20 ms mỗi lần tắt ngắt (nhân 1 còn chạy giao diện)
    st.trig = -1;
    const int64_t deadline_ms =
        (s_req.mode == Mode::Passive ? s_req.passive_timeout_ms : 200) + (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool done = false;
    while (!done) {
        st.total = 0;  // cửa sổ mới: bỏ dữ liệu trước (không liên tục với lần tắt ngắt trước)
        st.trig = -1;
        portENTER_CRITICAL(&s_mux);
        s_armed = true;
        done = captureWindow(st);
        portEXIT_CRITICAL(&s_mux);
        if (done || (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) > deadline_ms) {
            break;
        }
        vTaskDelay(2);
    }
    // Sắp lại bộ đệm vòng theo thứ tự thời gian.
    const int n = st.total < RING ? st.total : RING;
    const int first = st.total - n;
    s_cap.s.resize(n);
    for (int k = 0; k < n; ++k) {
        s_cap.s[k] = s_ring[(first + k) & (RING - 1)];
    }
    s_cap.trigger = done ? st.trig - first : -1;
    s_cap.overruns = st.overruns;
    s_cap.period_us = (float)st.period_cycles / mhz;
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

void workerTask(void *)
{
    s_cap = Capture();
    s_armed = false;
    // Tạo bus (nếu chưa có) trên nhân 0: chân về chế độ open-drain, khi nghỉ thả nổi -> đọc được mức của bus.
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)(s_get_bus ? s_get_bus(s_user) : nullptr);
    if (bus == nullptr) {
        snprintf(s_cap.msg, sizeof(s_cap.msg), "Không khởi tạo được bus I2C (GPIO%d / GPIO%d)", PIN_SDA, PIN_SCL);
        goto out;
    }
    if (s_req.mode == Mode::Passive) {
        gpio_pullup_dis((gpio_num_t)PIN_SDA);  // chỉ nghe: không kéo bus ngoài lên 3,3 V
        gpio_pullup_dis((gpio_num_t)PIN_SCL);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    s_cap.idle_high = gpio_get_level((gpio_num_t)PIN_SDA) && gpio_get_level((gpio_num_t)PIN_SCL);

    if (xTaskCreatePinnedToCore(captureTask, "i2c_scope_cap", 3072, nullptr, configMAX_PRIORITIES - 2, nullptr,
                                HCC_CORE_UI) != pdPASS) {
        snprintf(s_cap.msg, sizeof(s_cap.msg), "Không tạo được task chụp");
        goto restore;
    }
    if (s_req.mode == Mode::Probe) {
        for (int k = 0; k < 200 && !s_armed; ++k) {
            vTaskDelay(1);
        }
        esp_rom_delay_us((uint32_t)(s_req.period_us * PRE * 1.2f) + 20);  // để có đoạn trước kích
        s_cap.probe_result = i2c_master_probe(bus, s_req.addr, 50);
    }
    xSemaphoreTake(s_done, pdMS_TO_TICKS(s_req.passive_timeout_ms + 2000));

    if (s_cap.trigger < 0) {
        snprintf(s_cap.msg, sizeof(s_cap.msg), "%s",
                 s_req.mode == Mode::Passive ? "Không thấy hoạt động trên bus (hết thời gian chờ)"
                                             : "Không thấy sóng - dây SCL / SDA chưa nối GPIO5 / GPIO4?");
    } else if (s_req.mode == Mode::Probe) {
        snprintf(s_cap.msg, sizeof(s_cap.msg), "Probe 0x%02X: %s", s_req.addr,
                 s_cap.probe_result == ESP_OK                ? "ACK (có thiết bị)"
                 : s_cap.probe_result == ESP_ERR_NOT_FOUND   ? "NACK (không có thiết bị)"
                 : s_cap.probe_result == ESP_ERR_TIMEOUT     ? "TIMEOUT (bus bị giữ mức 0?)"
                                                             : esp_err_to_name(s_cap.probe_result));
    } else {
        snprintf(s_cap.msg, sizeof(s_cap.msg), "Đã bắt được hoạt động trên bus");
    }
    if (!s_cap.idle_high) {
        const size_t l = strlen(s_cap.msg);
        snprintf(s_cap.msg + l, sizeof(s_cap.msg) - l, " | Bus không ở mức 1 khi nghỉ: thiếu kéo lên / chập GND");
    }
    if (s_cap.overruns > 0) {
        const size_t l = strlen(s_cap.msg);
        snprintf(s_cap.msg + l, sizeof(s_cap.msg) - l, " | Chu kỳ mẫu quá nhỏ (%d lần trễ)", s_cap.overruns);
    }

restore:
    if (s_req.mode == Mode::Passive) {
        gpio_pullup_en((gpio_num_t)PIN_SDA);  // trả lại như cấu hình bus I2C (kéo lên trong chip)
        gpio_pullup_en((gpio_num_t)PIN_SCL);
    }
out:
    ESP_LOGI(TAG, "%s (%d mẫu, %.2f us, kích %d)", s_cap.msg, (int)s_cap.s.size(), s_cap.period_us, s_cap.trigger);
    s_serial = s_serial + 1;
    s_busy = false;
    vTaskDelete(nullptr);
}

}  // namespace

int sdaPin(void)
{
    return PIN_SDA;
}

int sclPin(void)
{
    return PIN_SCL;
}

bool start(const Request &req, BusGetter get_bus, void *user)
{
    if (s_busy) {
        return false;
    }
    if (s_done == nullptr) {
        s_done = xSemaphoreCreateBinary();
    }
    xSemaphoreTake(s_done, 0);
    s_busy = true;
    s_req = req;
    s_get_bus = get_bus;
    s_user = user;
    if (xTaskCreatePinnedToCore(workerTask, "i2c_scope", 4096, nullptr, 5, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_busy = false;
        return false;
    }
    return true;
}

bool busy(void)
{
    return s_busy;
}

uint32_t serial(void)
{
    return s_serial;
}

const Capture &result(void)
{
    return s_cap;
}

#endif  // ESP_PLATFORM

}  // namespace HCCI2CScope
