#include "freertos/FreeRTOS.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "product_audio.h"

#include "esp_brookesia.hpp"
#include "apps.h"
#include "hcc_cores.h"
#include "setting/hcc_clock.h"
#include "hcc_home/HCCUsbDisk.hpp"

static const char *TAG = "main";

LV_IMG_DECLARE(img_hcc_home_wallpaper);

// The status bar is transparent only on the home screen so the wallpaper shows behind it.
// App screens start below the bar (y = bar height) and never repaint that strip, so the
// bar must be opaque while an app is shown or stale pixels show through it.
static lv_obj_t *s_status_bar_obj = nullptr;

static lv_obj_t *findStatusBarObject(lv_obj_t *system_screen_obj)
{
    // Brookesia keeps the bar object private; it is the full-width strip at the top of the
    // system screen (the recents screen is full height, the navigation bar is at the bottom).
    lv_obj_update_layout(system_screen_obj);
    const uint32_t count = lv_obj_get_child_cnt(system_screen_obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(system_screen_obj, i);
        if (lv_obj_get_y(child) == 0 && lv_obj_get_width(child) == lv_obj_get_width(system_screen_obj) &&
                lv_obj_get_height(child) < lv_obj_get_height(system_screen_obj) / 4) {
            return child;
        }
    }
    return nullptr;
}

static void onHomeScreenLoadEvent(lv_event_t *event)
{
    if (s_status_bar_obj == nullptr) {
        return;
    }
    const bool home_shown = (lv_event_get_code(event) == LV_EVENT_SCREEN_LOAD_START);
    lv_obj_set_style_bg_opa(s_status_bar_obj, home_shown ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    // Giờ từ pin RTC (ML1220): đặt múi giờ VN + log chẩn đoán trước mọi việc dùng time().
    hcc_clock_init();
    // USB DISK: trả cổng Type-C về USB-Serial/JTAG (LP_SYS không bị xoá khi crash/reset mềm).
    HCCUsbDisk::bootInit();

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
    ESP_LOGI(TAG, "SD card mount successfully");
#endif

    ESP_ERROR_CHECK(product_audio_codec_init());
    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false));

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    // LVGL chạy riêng nhân HCC_CORE_UI; việc nặng ở nhân còn lại (components/apps/hcc_cores.h).
    cfg.lv_adapter_cfg.task_core_id = HCC_CORE_UI;
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "Failed to start BSP display");
        return;
    }

    // Bus I2C đã khởi tạo (cảm ứng): đọc giờ từ module DS3231 nếu có gắn.
    hcc_clock_ext_rtc_init();

    ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);

    ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone();
    assert(phone != nullptr && "Failed to create phone");

    ESP_Brookesia_PhoneStylesheet_t *phone_stylesheet = new ESP_Brookesia_PhoneStylesheet_t ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET();
    if (phone_stylesheet == nullptr) {
        ESP_LOGE(TAG, "Failed to create phone stylesheet");
        bsp_display_unlock();
        return;
    }
    // HCC INSIGHT wallpaper (1024x600) replaces the default 480x480 home background.
    phone_stylesheet->core.home.background.wallpaper_image_resource = ESP_BROOKESIA_STYLE_IMAGE(&img_hcc_home_wallpaper);
    if (!phone->addStylesheet(*phone_stylesheet)) {
        ESP_LOGE(TAG, "Failed to add phone stylesheet");
        bsp_display_unlock();
        return;
    }
    if (!phone->activateStylesheet(*phone_stylesheet)) {
        ESP_LOGE(TAG, "Failed to activate phone stylesheet");
        bsp_display_unlock();
        return;
    }

    if (!phone->begin()) {
        ESP_LOGE(TAG, "Failed to begin phone");
        bsp_display_unlock();
        return;
    }

    s_status_bar_obj = findStatusBarObject(phone->getCoreHome().getSystemScreenObject());
    if (s_status_bar_obj == nullptr) {
        ESP_LOGW(TAG, "Status bar object not found; keeping it opaque");
    } else {
        lv_obj_t *home_screen = phone->getCoreHome().getMainScreen();
        lv_obj_add_event_cb(home_screen, onHomeScreenLoadEvent, LV_EVENT_SCREEN_LOAD_START, nullptr);
        lv_obj_add_event_cb(home_screen, onHomeScreenLoadEvent, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
        lv_obj_set_style_bg_opa(s_status_bar_obj, LV_OPA_TRANSP, 0);  // home is shown at boot
    }

    Calculator *calculator = new Calculator();
    assert(calculator != nullptr && "Failed to create calculator");
    if (phone->installApp(calculator) < 0) {
        ESP_LOGE(TAG, "Failed to install calculator");
        bsp_display_unlock();
        return;
    }

    HCCNews *news = new HCCNews();
    assert(news != nullptr && "Failed to create news");
    if (phone->installApp(news) < 0) {
        ESP_LOGE(TAG, "Failed to install news");
        bsp_display_unlock();
        return;
    }

    MusicPlayer *music_player = new MusicPlayer();
    assert(music_player != nullptr && "Failed to create music_player");
    if (phone->installApp(music_player) < 0) {
        ESP_LOGE(TAG, "Failed to install music_player");
        bsp_display_unlock();
        return;
    }

    AppSettings *app_settings = new AppSettings();
    assert(app_settings != nullptr && "Failed to create app_settings");
    if (phone->installApp(app_settings) < 0) {
        ESP_LOGE(TAG, "Failed to install app_settings");
        bsp_display_unlock();
        return;
    }

    Game2048 *game_2048 = new Game2048();
    assert(game_2048 != nullptr && "Failed to create game_2048");
    if (phone->installApp(game_2048) < 0) {
        ESP_LOGE(TAG, "Failed to install game_2048");
        bsp_display_unlock();
        return;
    }

    Minesweeper *minesweeper = new Minesweeper();
    assert(minesweeper != nullptr && "Failed to create minesweeper");
    if (phone->installApp(minesweeper) < 0) {
        ESP_LOGE(TAG, "Failed to install minesweeper");
        bsp_display_unlock();
        return;
    }

    PhoneRadar *phone_radar = new PhoneRadar();
    assert(phone_radar != nullptr && "Failed to create phone_radar");
    if (phone->installApp(phone_radar) < 0) {
        ESP_LOGE(TAG, "Failed to install phone_radar");
        bsp_display_unlock();
        return;
    }

    HCCHome *hcc_home = new HCCHome();
    assert(hcc_home != nullptr && "Failed to create hcc_home");
    if (phone->installApp(hcc_home) < 0) {
        ESP_LOGE(TAG, "Failed to install hcc_home");
        bsp_display_unlock();
        return;
    }

    XiaozhiApp *xiaozhi = new XiaozhiApp();
    assert(xiaozhi != nullptr && "Failed to create xiaozhi");
    if (phone->installApp(xiaozhi) < 0) {
        ESP_LOGE(TAG, "Failed to install xiaozhi");
        bsp_display_unlock();
        return;
    }

    

   
#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    ESP_LOGW(TAG, "Using Video Player example requires inserting the SD card in advance and saving an MJPEG format video on the SD card");
    AppVideoPlayer *app_video_player = new AppVideoPlayer();
    assert(app_video_player != nullptr && "Failed to create app_video_player");
    if (phone->installApp(app_video_player) < 0) {
        ESP_LOGE(TAG, "Failed to install app_video_player");
        bsp_display_unlock();
        return;
    }
#endif

    bsp_display_unlock();
    bsp_display_backlight_on();

}
