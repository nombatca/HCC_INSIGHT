file(GLOB VN_FONT_SRCS ${CMAKE_CURRENT_LIST_DIR}/fonts/*.c)

idf_component_register(
    SRCS ${VN_FONT_SRCS}
    INCLUDE_DIRS include
    REQUIRES lvgl__lvgl)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
