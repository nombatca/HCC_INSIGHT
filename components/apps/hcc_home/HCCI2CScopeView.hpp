/*
 * HCCI2CScopeView: màn "SÓNG I2C" - hiển thị SCL / SDA như máy hiện sóng logic, kèm giải mã
 * (S = START, Sr = START lặp, P = STOP, byte địa chỉ + R/W, byte dữ liệu, A = ACK, N = NACK).
 *
 *   Chế độ "Probe địa chỉ": máy tự gửi probe tới địa chỉ chọn trên bus GPIO4 (SDA) / GPIO5 (SCL) và chụp lại.
 *   Chế độ "Nghe bus ngoài": chỉ nghe (tắt kéo lên trong chip), chờ tối đa 3 s cho mạch ngoài hoạt động.
 *   Kéo ngang để xem, nút - / + để thu phóng, VỪA để vừa vùng có tín hiệu; "Liên tục" để chụp lặp lại.
 */
#pragma once

#include "lvgl.h"
#include "HCCI2CScope.hpp"

namespace HCCI2CScopeView {

void show(lv_obj_t *screen, lv_event_cb_t back_cb, void *back_user, HCCI2CScope::BusGetter get_bus, void *bus_user);

}  // namespace HCCI2CScopeView
