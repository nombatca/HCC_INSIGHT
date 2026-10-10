/*
 * Nguồn PARLIO RX (ESP32-P4): lấy mẫu liên tục 2 dây SCLK / SDATA bằng DMA vào bộ đệm vòng 16 x 4032 byte
 * trong RAM nội. Chỉ nghe: PARLIO RX chỉ cấu hình chân vào, điện trở kéo trong chip bị tắt.
 *
 * Phát hiện mất dữ liệu: ISR đẩy từng chunk vào hàng đợi; hàng đợi đầy (giải mã không kịp) hoặc bộ giải mã tụt
 * sau >= NDESC - 2 chunk (DMA sắp ghi đè chunk đang đọc) -> bỏ các chunk đó, báo Feed::lost() với số mẫu tương
 * ứng để thời gian các gói sau vẫn đúng.
 */
#include <stdio.h>
#include <string.h>

#include "HCCRffe.hpp"
#include "HCCSpmiSource.hpp"
#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace HCCSpmiSource {

namespace {

const char *TAG = "HCCSpmiParlio";
constexpr size_t DESC_BYTES = 4032;  // bội 64, vừa một mô tả DMA
constexpr size_t NDESC = 16;
constexpr size_t RING_BYTES = DESC_BYTES * NDESC;

struct Chunk {
    const uint8_t *data;
    uint32_t len;
};

QueueHandle_t s_queue;
volatile uint32_t s_isr_overruns;

bool IRAM_ATTR onPartial(parlio_rx_unit_handle_t, const parlio_rx_event_data_t *e, void *)
{
    Chunk c = {(const uint8_t *)e->data, (uint32_t)e->recv_bytes};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &c, &woken) != pdTRUE) {
        s_isr_overruns = s_isr_overruns + 1;
    }
    return woken == pdTRUE;
}

class ParlioSource : public Source {
public:
    const char *name() const override { return "PARLIO"; }

    bool supported(const Params &p, char *why, size_t n) const override
    {
        if (!HCCRffe::configured()) {
            snprintf(why, n, "%s", HCCRffe::configError());
            return false;
        }
        if (!p.hw_confirmed) {
            snprintf(why, n,
                     "Chưa xác nhận mạch thu: GPIO P4 là 3,3 V, bus SPMI 1,2 / 1,8 V; mạch BSS138 của RFFE không dùng "
                     "được cho SPMI tốc độ cao. Cần mạch chuyển mức MỘT CHIỀU (vd SN74AXC / LVC1T45, VCCA = VIO của main).");
            return false;
        }
        if (p.sample_hz != 20000000 && p.sample_hz != 40000000 && p.sample_hz != 80000000) {
            snprintf(why, n, "Tần số lấy mẫu không hỗ trợ");
            return false;
        }
        return true;
    }

    bool run(const Params &p, Feed &f, char *err, size_t es) override
    {
        if (!HCCRffe::claimPins()) {
            snprintf(err, es, "Chân SCLK / SDATA đang bận (đang quét RFFE / SPMI)");
            return false;
        }
        if (s_queue == nullptr) {
            s_queue = xQueueCreate(NDESC, sizeof(Chunk));
        }
        xQueueReset(s_queue);
        s_isr_overruns = 0;
        f.setSampleHz(p.sample_hz);
        const int pin_clk = HCCRffe::sclkPin();
        const int pin_dat = HCCRffe::sdataPin();
        parlio_rx_unit_handle_t unit = nullptr;
        parlio_rx_delimiter_handle_t deli = nullptr;
        bool ok = false;
        uint32_t seen_ov = 0;
        esp_err_t r = ESP_OK;
        uint8_t *ring = (uint8_t *)heap_caps_aligned_calloc(64, 1, RING_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (ring == nullptr) {
            snprintf(err, es, "Hết RAM nội cho bộ đệm DMA (%u KB)", (unsigned)(RING_BYTES / 1024));
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
            uc.exp_clk_freq_hz = p.sample_hz;
            uc.clk_out_gpio_num = GPIO_NUM_NC;
            uc.valid_gpio_num = GPIO_NUM_NC;
            for (int i = 0; i < PARLIO_RX_UNIT_MAX_DATA_WIDTH; ++i) {
                uc.data_gpio_nums[i] = GPIO_NUM_NC;
            }
            uc.data_gpio_nums[0] = (gpio_num_t)pin_clk;  // dây 0
            uc.data_gpio_nums[1] = (gpio_num_t)pin_dat;  // dây 1
            r = parlio_new_rx_unit(&uc, &unit);
        }
        if (r != ESP_OK) {
            snprintf(err, es, "Không khởi tạo được PARLIO RX (%s)", esp_err_to_name(r));
            goto out;
        }
        for (int pin : {pin_clk, pin_dat}) {
            gpio_pullup_dis((gpio_num_t)pin);
            gpio_pulldown_dis((gpio_num_t)pin);
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
            parlio_receive_config_t rc = {};
            rc.delimiter = deli;
            rc.flags.partial_rx_en = true;
            r = parlio_rx_unit_receive(unit, ring, RING_BYTES, &rc);
        }
        if (r != ESP_OK) {
            snprintf(err, es, "Lỗi bật bộ thu PARLIO (%s)", esp_err_to_name(r));
            goto stop_hw;
        }
        ok = true;
        while (!f.stopRequested()) {
            Chunk c;
            if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(50)) != pdTRUE) {
                f.idle(0);  // cập nhật trạng thái (NO SIGNAL)
                continue;
            }
            const uint32_t ov = s_isr_overruns;
            const UBaseType_t waiting = uxQueueMessagesWaiting(s_queue);
            if (ov != seen_ov || waiting >= NDESC - 2) {
                // Bỏ chunk hiện tại + các chunk đang chờ (có thể đã bị DMA ghi đè) + các chunk ISR không đẩy được.
                const uint32_t dropped = 1 + (uint32_t)waiting + (ov - seen_ov);
                seen_ov = ov;
                xQueueReset(s_queue);
                f.lost((uint64_t)dropped * c.len * 4);
                continue;
            }
            f.samples(c.data, c.len);
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
        // Trả chân về vào-thả nổi (không gpio_reset_pin: hàm đó bật kéo lên, sẽ kéo bus đang nối lên 3,3 V).
        for (int pin : {pin_clk, pin_dat}) {
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
            gpio_pullup_dis((gpio_num_t)pin);
            gpio_pulldown_dis((gpio_num_t)pin);
        }
        HCCRffe::releasePins();
        ESP_LOGI(TAG, "PARLIO dừng (tràn ISR %u)", (unsigned)s_isr_overruns);
        return ok;
    }
};

ParlioSource s_parlio;

}  // namespace

Source *parlio()
{
    return &s_parlio;
}

}  // namespace HCCSpmiSource
