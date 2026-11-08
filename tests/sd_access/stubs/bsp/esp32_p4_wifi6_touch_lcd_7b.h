#pragma once
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1;
extern void* bsp_sdcard;
int bsp_sdcard_mount();
const char* hcc_test_mount_point();
#define BSP_SD_MOUNT_POINT hcc_test_mount_point()
