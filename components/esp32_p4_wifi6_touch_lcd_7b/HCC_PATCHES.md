# Vendored Waveshare BSP (HCC INSIGHT)

Source: `waveshare/esp32_p4_wifi6_touch_lcd_7b` **3.0.0** from the ESP Component Registry
(repository commit `32d86900af6916d5e6eb629741d57ee19a577740`). Version is unchanged; this local
copy takes precedence over the managed component so the patches below survive a clean clone.

Local patches:

1. `include/bsp/esp32_p4_wifi6_touch_lcd_7b.h` - `bsp_display_rotate()` uses `lv_disp_rot_t`
   instead of `lv_disp_rotation_t` (LVGL 9 name) so the BSP compiles with LVGL 8.4.
2. `esp32_p4_wifi6_touch_lcd_7b.c` - same rename in the `bsp_display_rotate()` definition.
3. `esp32_p4_wifi6_touch_lcd_7b.c` - `bsp_usb_host_enum_filter()` registered via
   `usb_host_config_t.enum_filter_cb`: selects USB configuration 3 for Apple 05AC:12A8
   (normal mode) so the USBMUX interface is exposed. Requires
   `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y` (set in `sdkconfig.defaults`).
4. `esp32_p4_wifi6_touch_lcd_7b.c` - `usb_lib_task()` tolerates `ESP_ERR_NOT_FINISHED`
   from `usb_host_device_free_all()` instead of `ESP_ERROR_CHECK`-aborting on it (the
   ESP-IDF usb_host example pattern). Without this the board reboots when a device is
   removed while it is still being freed asynchronously.
5. `Kconfig` + `esp32_p4_wifi6_touch_lcd_7b.c` - new option `BSP_DISPLAY_PPA_ACCEL` (default y)
   sets `enable_ppa_accel` in `bsp_display_lcd_init()` so esp_lvgl_adapter routes LVGL v8
   fill/blend through the ESP32-P4 PPA (RGB565 only). Display mode is DOUBLE_DIRECT, where
   LVGL draws straight into the panel frame buffers, so `buffer_height`/`use_psram` stay
   unused and unchanged.
6. `esp32_p4_wifi6_touch_lcd_7b.c/.h` - SD power: the on-chip LDO channel 4 is adjustable and
   accepts one owner; `bsp_sdcard_mount()` acquired it on every call and never released it, so
   mounting again after `bsp_sdcard_unmount()` failed. The handle is now created once and reused
   (`bsp_sdcard_host_config()`); `bsp_sdcard_unmount()` clears `bsp_sdcard`. New
   `bsp_sdcard_raw_init()` / `bsp_sdcard_raw_deinit()` bring the card up without FAT for USB MSC;
   they release only SDMMC slot 0 (slot 1 is the ESP-Hosted SDIO link to the Wi-Fi chip).
