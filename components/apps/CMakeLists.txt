set(APPS_DIR ./)
# CONFIGURE_DEPENDS: mỗi lần build, CMake kiểm tra lại danh sách file nên file .c/.cpp mới được build
# luôn, không cần "idf.py reconfigure" (trước đây quên reconfigure -> lỗi link "undefined reference").
# (đã thêm: setting/hcc_clock.c, hcc_home/HCCUsbDisk.cpp, music_player/HCCMusicEngine.c, music_player/HCCAudioMeta.c;
#  xiaozhi/*.cpp + xiaozhi/assets/img_app_xiaozhi.c; đã xoá music_player/gui_music)
file(GLOB_RECURSE APPS_C_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.c)
file(GLOB_RECURSE APPS_CPP_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.cpp)
list(FILTER APPS_C_SRCS EXCLUDE REGEX "/camera/")
list(FILTER APPS_CPP_SRCS EXCLUDE REGEX "/camera/")

# HCC INSIGHT app icon: keep asset discovery deterministic (same assets/ pattern as Settings).
list(APPEND APPS_C_SRCS ${APPS_DIR}/hcc_home/assets/img_app_hcc_insight.c)
list(APPEND APPS_C_SRCS ${APPS_DIR}/hcc_home/assets/img_hcc_home_wallpaper.c)

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    # Knowledge Base V1 mặc định cho phân tích panic (HCCPanicKBStore::defaultKbJson).
    EMBED_TXTFILES ${APPS_DIR}/hcc_home/kb/hcc_kb_default.json
    REQUIRES lvgl__lvgl esp_event esp_wifi esp_http_client esp_http_server nvs_flash esp_driver_jpeg esp_mm esp-brookesia product_audio esp32_p4_wifi6_touch_lcd_7b esp_video vn_fonts esp_netif mbedtls esp_websocket_client json usb mdns esp_tinyusb efuse esp_audio_codec esp_app_format esp_timer)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-format
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wno-format
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
    >
    -DLV_LVGL_H_INCLUDE_SIMPLE
    # Chỉ C/C++: EMBED_TXTFILES sinh file assembly (.S), không được -include header C.
    "$<$<COMPILE_LANGUAGE:C,CXX>:-include${CMAKE_CURRENT_LIST_DIR}/../vn_fonts/include/vn_fonts.h>"
)
