#include "HCCUsbDisk.hpp"

#include <stdio.h>
#include <string.h>

#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hcc_cores.h"
#include "soc/lp_system_struct.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

namespace HCCUsbDisk {
namespace {

const char *TAG = "HCCUsbDisk";

volatile State s_state = State::Off;
char s_error[96] = "";
uint32_t s_capacity_mb = 0;
sdmmc_card_t *s_card = nullptr;
tinyusb_msc_storage_handle_t s_storage = nullptr;
bool s_tinyusb_installed = false;
bool s_msc_driver_installed = false;
bool s_phy_swapped = false;
char s_phy_note[64] = "";

void setError(const char *msg, esp_err_t err)
{
    snprintf(s_error, sizeof(s_error), "%s (%s)", msg, esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", s_error);
}

// Đưa OTG Full-Speed ra đúng PHY mà USB-Serial/JTAG đang dùng (cổng Type-C dùng để flash).
// Mặc định (eFuse USB_PHY_SEL = 0): USJ ở PHY0, OTG FS ở PHY1. eFuse = 1 thì ngược lại.
// LP_SYS.usb_ctrl: sw_hw_usb_phy_sel = 1 cho phần mềm chọn; sw_usb_phy_sel = 0: USJ->PHY0, OTG->PHY1;
// = 1: USJ->PHY1, OTG->PHY0. Ghi thẳng thanh ghi: usb_wrap_ll_phy_select() của ESP-IDF 5.5 thiếu
// 'break' (case 0 rơi xuống case 1) nên luôn trả về ánh xạ mặc định.
void swapPhy(bool otg_on_type_c)
{
    if (otg_on_type_c) {
        const bool efuse_sel = esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
        const int usj_phy = efuse_sel ? 1 : 0;     // PHY của cổng Type-C (USJ lúc khởi động)
        LP_SYS.usb_ctrl.sw_usb_phy_sel = usj_phy == 0 ? 1 : 0;  // OTG FS -> PHY đó
        LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 1;
        snprintf(s_phy_note, sizeof(s_phy_note), "eFuse USB_PHY_SEL=%d, OTG FS -> PHY%d", efuse_sel ? 1 : 0, usj_phy);
    } else {
        LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 0;  // trả lại cho eFuse quyết định như lúc khởi động
        LP_SYS.usb_ctrl.sw_usb_phy_sel = 0;
    }
    s_phy_swapped = otg_on_type_c;
    ESP_LOGI(TAG, "PHY: %s (%s)", otg_on_type_c ? "OTG FS -> Type-C (USB DISK)" : "mặc định (Type-C = Serial/JTAG)",
             s_phy_note);
}

void onUsbEvent(tinyusb_event_t *event, void *)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        if (s_state == State::Ready) s_state = State::Connected;
        ESP_LOGI(TAG, "Máy tính đã nhận ổ đĩa");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        if (s_state == State::Connected) s_state = State::Ready;
        ESP_LOGI(TAG, "Máy tính đã ngắt ổ đĩa");
    }
}

// Tháo hết những gì đã dựng (dùng cho cả stop và khi start lỗi giữa chừng), rồi trả thẻ cho board.
void teardown()
{
    if (s_tinyusb_installed) {
        tinyusb_driver_uninstall();
        s_tinyusb_installed = false;
    }
    if (s_storage != nullptr) {
        tinyusb_msc_delete_storage(s_storage);
        s_storage = nullptr;
    }
    if (s_msc_driver_installed) {
        tinyusb_msc_uninstall_driver();
        s_msc_driver_installed = false;
    }
    if (s_phy_swapped) {
        swapPhy(false);
    }
    if (s_card != nullptr) {
        bsp_sdcard_raw_deinit(s_card);
        s_card = nullptr;
    }
    s_capacity_mb = 0;
    // Board dùng lại thẻ SD ở /sdcard (panic, case, nhạc).
    const esp_err_t m = bsp_sdcard_mount();
    if (m != ESP_OK && m != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Gắn lại thẻ SD cho board thất bại: %s", esp_err_to_name(m));
    }
}

void doStart()
{
    s_error[0] = '\0';
    // 1) Board nhả thẻ: tháo FAT ở /sdcard (nếu đang gắn). Không có thẻ thì raw_init báo lỗi bên dưới.
    if (bsp_sdcard != nullptr) {
        const esp_err_t u = bsp_sdcard_unmount();
        if (u != ESP_OK) {
            setError("Không tháo được thẻ SD khỏi board", u);
            s_state = State::Error;
            return;
        }
    }
    // 2) Khởi tạo thẻ "thô" (không FAT): máy tính sẽ quản lý hệ thống file.
    esp_err_t err = bsp_sdcard_raw_init(&s_card);
    if (err != ESP_OK) {
        s_card = nullptr;
        setError("Không đọc được thẻ SD (chưa cắm thẻ?)", err);
        teardown();
        s_state = State::Error;
        return;
    }
    s_capacity_mb = (uint32_t)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));

    // 3) Ổ đĩa MSC trên thẻ, luôn thuộc máy tính; không tự gắn lại cho app khi rút cáp.
    tinyusb_msc_driver_config_t drv = {};
    drv.user_flags.auto_mount_off = 1;
    err = tinyusb_msc_install_driver(&drv);
    if (err != ESP_OK) {
        setError("Lỗi cài MSC", err);
        teardown();
        s_state = State::Error;
        return;
    }
    s_msc_driver_installed = true;

    tinyusb_msc_storage_config_t cfg = {};
    cfg.medium.card = s_card;
    cfg.fat_fs.base_path = nullptr;
    cfg.fat_fs.do_not_format = true;  // không bao giờ tự format thẻ của người dùng
    cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
    if (err != ESP_OK) {
        s_storage = nullptr;
        setError("Lỗi tạo ổ đĩa USB", err);
        teardown();
        s_state = State::Error;
        return;
    }

    // 4) Đưa OTG Full-Speed ra cổng Type-C rồi bật TinyUSB.
    swapPhy(true);
    tinyusb_config_t tcfg = TINYUSB_DEFAULT_CONFIG(onUsbEvent);
    tcfg.port = TINYUSB_PORT_FULL_SPEED_0;
    tcfg.task.xCoreID = HCC_CORE_WORK;
    err = tinyusb_driver_install(&tcfg);
    if (err != ESP_OK) {
        setError("Lỗi khởi động USB", err);
        teardown();
        s_state = State::Error;
        return;
    }
    s_tinyusb_installed = true;
    s_state = State::Ready;
    ESP_LOGI(TAG, "USB DISK bật: thẻ %u MB", (unsigned)s_capacity_mb);
}

void worker(void *arg)
{
    const bool start = arg != nullptr;
    if (start) {
        doStart();
    } else {
        teardown();
        s_state = State::Off;
        ESP_LOGI(TAG, "USB DISK tắt, thẻ SD trả lại cho board");
    }
    vTaskDelete(nullptr);
}

bool launch(bool start)
{
    // Ngăn xếp đủ cho sdmmc_card_init + cài TinyUSB.
    return xTaskCreatePinnedToCore(worker, start ? "hcc_usbdisk_on" : "hcc_usbdisk_off", 6144,
                                   start ? reinterpret_cast<void *>(1) : nullptr, 5, nullptr,
                                   HCC_CORE_WORK) == pdPASS;
}

}  // namespace

bool requestStart()
{
    if (s_state != State::Off && s_state != State::Error) return false;
    s_state = State::Starting;
    if (!launch(true)) {
        s_state = State::Error;
        snprintf(s_error, sizeof(s_error), "Không tạo được task");
        return false;
    }
    return true;
}

bool requestStop()
{
    if (s_state != State::Ready && s_state != State::Connected && s_state != State::Error) return false;
    s_state = State::Stopping;
    if (!launch(false)) {
        s_state = State::Error;
        snprintf(s_error, sizeof(s_error), "Không tạo được task");
        return false;
    }
    return true;
}

State state()
{
    return s_state;
}

const char *stateText()
{
    switch (s_state) {
    case State::Off: return "TẮT - thẻ SD đang dùng cho board";
    case State::Starting: return "Đang bật...";
    case State::Ready: return "ĐÃ BẬT - cắm cáp Type-C vào máy tính";
    case State::Connected: return "MÁY TÍNH ĐÃ NHẬN Ổ ĐĨA";
    case State::Stopping: return "Đang tắt...";
    case State::Error: return "LỖI";
    }
    return "?";
}

const char *lastError()
{
    return s_error;
}

uint32_t capacityMb()
{
    return s_capacity_mb;
}

const char *phyNote()
{
    return s_phy_note;
}

}  // namespace HCCUsbDisk
