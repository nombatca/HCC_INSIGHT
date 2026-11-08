#include "HCCBackupCapture.hpp"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
hcc::backup::Error HCCBackupCapture::open()
{
    using hcc::backup::Error;
    if (!lease_.acquire()) return Error::busy;
    if (bsp_sdcard == nullptr) {
        const auto e = bsp_sdcard_mount();
        if ((e != ESP_OK && e != ESP_ERR_INVALID_STATE) || bsp_sdcard == nullptr) {
            lease_.release(); return Error::io_error;
        }
    }
    char parent[256];
    const int n = snprintf(parent,sizeof(parent),"%s/hcc_backup",BSP_SD_MOUNT_POINT);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(parent)) {lease_.release();return Error::too_large;}
    if (mkdir(parent,0700) != 0 && errno != EEXIST) {lease_.release();return Error::io_error;}
    const auto result = storage_.open_session(parent);
    if (result != Error::ok) lease_.release();
    return result;
}
