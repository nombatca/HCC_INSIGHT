/*
 * HCCEepromView: màn "EEPROM TOOL" - đọc / ghi / xác minh / xoá EEPROM I2C họ 24Cxx trên bus GPIO4 (SDA) /
 * GPIO5 (SCL), sửa trực tiếp bằng HEX EDITOR, so sánh với chip, sao lưu / phục hồi file .bin trên thẻ SD.
 *
 *   Cột trái : loại chip, địa chỉ I2C, QUÉT I2C (0x50..0x57), các nút ĐỌC / GHI / XÁC MINH / XOÁ (FF) /
 *              SAO LƯU / PHỤC HỒI. GHI và XOÁ phải bấm 2 lần trong 5 giây mới chạy.
 *   Giữa     : HEX EDITOR (256 byte mỗi trang, chạm 1 lần chọn ô, chạm lại để sửa bằng bàn phím hex),
 *              SO SÁNH (byte khác nhau giữa bộ đệm và chip), FILE (danh sách .bin); log hoạt động + tiến trình.
 *   Cột phải : thông tin chip, thông tin dữ liệu (CRC32, số byte thay đổi), đến địa chỉ, tuỳ chọn hiển thị.
 * File: /sdcard/eeprom/<chip>_<địa chỉ>_NNN.bin (đọc thêm file .bin ở gốc thẻ nhớ - bản dump kiểu cũ).
 */
#pragma once

#include "lvgl.h"

namespace HCCEepromView {

typedef void *(*BusGetter)(void *user);  // trả i2c_master_bus_handle_t (gọi từ task nền, nhân 0)

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user, BusGetter get_bus, void *bus_user);

}  // namespace HCCEepromView
