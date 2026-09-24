#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VN_FONT_DECL(size) extern const lv_font_t lv_font_be_vietnam_##size;
VN_FONT_DECL(8)
VN_FONT_DECL(10)
VN_FONT_DECL(12)
VN_FONT_DECL(14)
VN_FONT_DECL(16)
VN_FONT_DECL(18)
VN_FONT_DECL(20)
VN_FONT_DECL(22)
VN_FONT_DECL(24)
VN_FONT_DECL(26)
VN_FONT_DECL(28)
VN_FONT_DECL(30)
VN_FONT_DECL(32)
VN_FONT_DECL(34)
VN_FONT_DECL(36)
VN_FONT_DECL(38)
VN_FONT_DECL(40)
VN_FONT_DECL(42)
VN_FONT_DECL(44)
VN_FONT_DECL(46)
VN_FONT_DECL(48)
#undef VN_FONT_DECL

#ifdef __cplusplus
}
#endif
