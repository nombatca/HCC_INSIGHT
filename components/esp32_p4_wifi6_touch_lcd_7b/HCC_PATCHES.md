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
