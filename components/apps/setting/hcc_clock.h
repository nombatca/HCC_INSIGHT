#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Giờ hệ thống của HCC INSIGHT với pin RTC (ML1220, chân VBAT -> vùng LP của ESP32-P4).
//
// ESP-IDF tính giờ = bộ đếm RTC (vùng LP) + mốc boot lưu trong thanh ghi LP. Khi pin giữ vùng LP
// sống lúc rút nguồn chính, giờ có thể còn đúng sau khi cắm lại; nếu chip coi đó là power-on
// reset thì LP cũng bị reset và giờ mất. Module này không đoán: chỉ ghi nhận giờ có hợp lệ không
// và lấy từ đâu, để case / file không mang ngày sai mà như đúng.
typedef enum {
    HCC_CLOCK_NONE = 0,  // chưa có giờ (sau mất nguồn, chưa có Wi-Fi)
    HCC_CLOCK_RTC,       // giờ còn sau khởi động (pin RTC / reset mềm), chưa đồng bộ lại
    HCC_CLOCK_SNTP,      // đã đồng bộ qua Wi-Fi trong lần chạy này
    HCC_CLOCK_DS3231,    // lấy từ module RTC rời DS3231 (I2C 0x68) lúc khởi động
} hcc_clock_source_t;

// Gọi sớm trong app_main, sau nvs_flash_init(): đặt múi giờ Việt Nam và ghi log chẩn đoán.
void hcc_clock_init(void);
// Module RTC rời DS3231 (pin riêng) trên bus I2C của board (GPIO7 SDA / GPIO8 SCL).
// Gọi sau khi bus I2C đã sẵn sàng (sau bsp_display_start): nếu có DS3231 hợp lệ và hệ thống
// chưa có giờ thì đặt giờ từ DS3231. Không có module thì chỉ ghi log, không làm gì khác.
void hcc_clock_ext_rtc_init(void);
// True khi đã nhận diện được DS3231 lúc khởi động.
bool hcc_clock_ext_rtc_present(void);

// Gọi từ callback SNTP với giờ vừa nhận: đo độ trôi của giờ RTC (so với giờ lúc khởi động).
void hcc_clock_on_sntp_sync(int64_t sntp_unix);

bool hcc_clock_is_valid(void);
hcc_clock_source_t hcc_clock_source(void);
const char *hcc_clock_source_name(void);

#ifdef __cplusplus
}
#endif
