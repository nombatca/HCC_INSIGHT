#pragma once

// Phân nhân CPU ESP32-P4 (2 nhân):
//   nhân 1: LVGL (giao diện) - main.cpp đặt task_core_id của esp_lvgl_adapter
//   nhân 0: việc nặng / chờ I/O (phân tích panic, crypto ghép đôi, USB, I2C/EEPROM, tải web);
//           app_main, esp_timer và luồng camera cũng ở nhân 0.
// Nhờ vậy giao diện không khựng khi ANALYZE, tạo khoá hay đọc USB.
#define HCC_CORE_UI 1
#define HCC_CORE_WORK 0
