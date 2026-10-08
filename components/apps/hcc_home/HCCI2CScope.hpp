/*
 * HCCI2CScope: chụp mức logic SCL / SDA trên chân GPIO (kiểu máy hiện sóng logic) và giải mã I2C.
 *
 * Chụp: task trên nhân UI (nhân 1) tắt ngắt và đọc thanh ghi GPIO_IN theo chu kỳ mẫu cố định (đếm chu kỳ CPU),
 * ghi vào bộ đệm vòng; kích (trigger) khi một trong hai dây xuống mức 0, giữ 1/8 bộ đệm trước điểm kích.
 *   PROBE  : sau khi đã sẵn sàng chụp, task nhân 0 gửi i2c_master_probe(địa chỉ) trên bus GPIO4/5 -> thấy đúng
 *            giao dịch START + địa chỉ + R/W + ACK/NACK + STOP do chính máy phát ra.
 *   PASSIVE: chỉ nghe (tắt điện trở kéo lên trong chip trong lúc chụp), chờ hoạt động của mạch ngoài.
 * Mức điện áp: GPIO 3,3 V; bus 1,8 V phải qua mạch chuyển mức thì mới đọc đúng mức 1.
 *
 * Giải mã (decode) không phụ thuộc ESP-IDF, kiểm thử được trên máy tính.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace HCCI2CScope {

// Mỗi mẫu: bit0 = SCL, bit1 = SDA.
constexpr uint8_t BIT_SCL = 1;
constexpr uint8_t BIT_SDA = 2;

struct Event {
    enum Type { Start, Restart, Stop, Byte, Ack, Nack };
    Type type;
    int i0;          // mẫu bắt đầu
    int i1;          // mẫu kết thúc
    uint8_t value;   // Byte
    bool addr;       // Byte là byte địa chỉ (ngay sau START)
};

std::vector<Event> decode(const uint8_t *s, int n);
// Tần số SCL ước lượng (Hz) từ khoảng cách các cạnh lên; 0 nếu không đủ cạnh.
float sclFrequency(const uint8_t *s, int n, float period_us);

enum class Mode { Probe, Passive };

struct Request {
    Mode mode = Mode::Probe;
    uint8_t addr = 0x18;
    float period_us = 0.5f;
    int passive_timeout_ms = 3000;
};

struct Capture {
    std::vector<uint8_t> s;
    float period_us = 0;     // chu kỳ mẫu thực tế
    int trigger = -1;        // mẫu kích, -1 nếu không kích
    int overruns = 0;        // số lần trễ hạn lấy mẫu (chu kỳ đặt quá nhỏ)
    bool idle_high = false;  // cả hai dây ở mức 1 trước khi chụp (có kéo lên)
    int probe_result = -1;   // esp_err_t của probe (Mode::Probe)
    char msg[128] = "";
};

typedef void *BusHandle;  // i2c_master_bus_handle_t
typedef BusHandle (*BusGetter)(void *user);

int sdaPin(void);
int sclPin(void);
// Bắt đầu chụp ở nền. false nếu đang bận.
bool start(const Request &req, BusGetter get_bus, void *user);
bool busy(void);
uint32_t serial(void);          // tăng mỗi lần chụp xong
const Capture &result(void);    // chỉ đọc khi !busy()

}  // namespace HCCI2CScope
