#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// Đọc/ghi EEPROM I2C (họ 24Cxx) và lưu/nạp bản dump qua thẻ nhớ SD.
// Mọi thao tác I2C dùng driver i2c_master mới; thẻ SD được mount qua BSP.
class HCCEeprom {
public:
    struct Config {
        uint16_t device_address = 0x50;    // địa chỉ 7-bit của EEPROM trên bus
        uint32_t total_size     = 256;     // tổng dung lượng (byte)
        uint16_t page_size      = 16;      // số byte tối đa mỗi lần ghi trang
        uint8_t  address_width  = 1;       // số byte địa chỉ ô nhớ: 1 (24C01..16) hoặc 2 (24C32+)
        uint32_t scl_speed_hz   = 100000;  // tốc độ SCL
        uint32_t write_cycle_ms = 6;       // thời gian chờ sau mỗi lần ghi trang (t_WR)
        int      xfer_timeout_ms = 1000;   // timeout mỗi giao dịch I2C
    };

    // Đọc toàn bộ EEPROM vào out (out_size phải >= cfg.total_size).
    static esp_err_t readAll(i2c_master_bus_handle_t bus, const Config &cfg,
                             uint8_t *out, size_t out_size);

    // Ghi/nạp toàn bộ dữ liệu vào EEPROM (data_size <= cfg.total_size).
    static esp_err_t writeAll(i2c_master_bus_handle_t bus, const Config &cfg,
                              const uint8_t *data, size_t data_size);

    // Đọc EEPROM rồi lưu thẳng ra file nhị phân trên thẻ SD.
    static esp_err_t dumpToSd(i2c_master_bus_handle_t bus, const Config &cfg,
                              const char *filename);

    // Nạp file nhị phân từ thẻ SD rồi ghi vào EEPROM.
    static esp_err_t programFromSd(i2c_master_bus_handle_t bus, const Config &cfg,
                                   const char *filename);

    // Đọc lại EEPROM và so sánh với file trên thẻ SD.
    // Trả về ESP_OK nếu khớp; ESP_ERR_INVALID_CRC nếu lệch (*mismatch_offset = vị trí byte
    // đầu tiên lệch); mã lỗi khác nếu đọc file/EEPROM thất bại.
    static esp_err_t verifyAgainstSd(i2c_master_bus_handle_t bus, const Config &cfg,
                                     const char *filename, size_t *mismatch_offset);

    // Ghi buffer ra file trên thẻ SD (tự mount/unmount nếu cần).
    static esp_err_t saveBufferToSd(const char *filename,
                                    const uint8_t *data, size_t size);

    // Đọc file trên thẻ SD vào buffer; *out_len nhận số byte đọc được.
    static esp_err_t loadBufferFromSd(const char *filename,
                                      uint8_t *out, size_t out_size, size_t *out_len);

    // Liệt kê file ở thư mục gốc thẻ SD, ngăn cách bằng '\n' (hợp lv_dropdown).
    // ext_filter (vd ".bin") lọc theo đuôi, nullptr = mọi file. Trả về số file, -1 nếu lỗi.
    static int listFiles(char *out, size_t out_size, const char *ext_filter);

private:
    static esp_err_t addDevice(i2c_master_bus_handle_t bus, const Config &cfg,
                               i2c_master_dev_handle_t *out_dev);
    static size_t    putMemAddress(uint8_t *buf, const Config &cfg, uint32_t mem_addr);
    static esp_err_t ensureSdMounted(bool *mounted_here);
    static void      buildPath(char *out, size_t out_size, const char *filename);
};
