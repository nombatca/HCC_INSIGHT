/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief BSP Touchscreen
 *
 * This file offers API for basic touchscreen initialization.
 * It is useful for users who want to use the touchscreen without the default Graphical Library LVGL.
 *
 * For standard LCD initialization with LVGL graphical library, you can call all-in-one function bsp_display_start().
 */

#pragma once
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BSP touch configuration structure
 *
 */
typedef struct {
    struct {
        unsigned int swap_xy: 1;   /*!< Swap X and Y after reading coordinates */
        unsigned int mirror_x: 1;  /*!< Mirror X after reading coordinates */
        unsigned int mirror_y: 1;  /*!< Mirror Y after reading coordinates */
    } flags;
} bsp_touch_config_t;

/**
 * @brief Create new touchscreen
 *
 * If you want to free resources allocated by this function, you can use esp_lcd_touch API, ie.:
 *
 * \code{.c}
 * esp_lcd_touch_del(tp);
 * \endcode
 *
 * @param[in]  config    touch configuration
 * @param[out] ret_touch esp_lcd_touch touchscreen handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd_touch failure
 */
esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch);

/**
 * @brief Đọc các điểm chạm hiện tại (đa điểm, tối đa CONFIG_ESP_LCD_TOUCH_MAX_POINTS) theo toạ độ màn hình
 *
 * LVGL 8 chỉ nhận một điểm; BSP bọc hàm read_data của driver touch để lưu lại mọi điểm (dùng cho zoom hai
 * ngón). Toạ độ đã lật / đảo giống esp_lcd_touch. Trả về 0 nếu không chạm hoặc dữ liệu cũ hơn 150 ms.
 */
int bsp_touch_get_points(int16_t *x, int16_t *y, int max_points);

#ifdef __cplusplus
}
#endif
