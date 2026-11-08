#include "HCCBackupCapture.hpp"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_vfs_fat.h"
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
hcc::backup::Error HCCBackupCapture::open(std::uint64_t minimum_free_bytes)
{
    using hcc::backup::Error;
    if (!lease_.acquire()) return Error::busy;
    if (bsp_sdcard == nullptr) {
        const auto e = bsp_sdcard_mount();
        if ((e != ESP_OK && e != ESP_ERR_INVALID_STATE) || bsp_sdcard == nullptr) {
            lease_.release(); return Error::io_error;
        }
    }
    if (minimum_free_bytes) {
        std::uint64_t total=0,available=0;
        if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT,&total,&available)!=ESP_OK || available>total) {
            lease_.release();return Error::io_error;
        }
        if (available<minimum_free_bytes) {lease_.release();return Error::too_large;}
    }
    char parent[256];
    const int n = snprintf(parent,sizeof(parent),"%s/hcc_backup",BSP_SD_MOUNT_POINT);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(parent)) {lease_.release();return Error::too_large;}
    if (mkdir(parent,0700) != 0 && errno != EEXIST) {lease_.release();return Error::io_error;}
    const auto result = storage_.open_session(parent);
    if (result != Error::ok) lease_.release();
    return result;
}
