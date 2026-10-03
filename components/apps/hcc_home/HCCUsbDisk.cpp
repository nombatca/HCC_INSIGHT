#include "HCCUsbDisk.hpp"

#include <stdio.h>
#include <string.h>

#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hcc_cores.h"
#include "nvs.h"
#include "soc/lp_system_struct.h"
#include "soc/usb_serial_jtag_struct.h"
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
char s_phy_note[192] = "";
char s_last_crash[96] = "";

// Ghi bước đang làm vào NVS: nếu board crash giữa chừng (lúc bật USB DISK không có monitor),
// lần khởi động sau biết hỏng ở bước nào. 0 = xong / không dở dang.
void breadcrumb(uint8_t step)
{
    nvs_handle_t h;
    if (nvs_open("hcc_udisk", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "step", step);
    nvs_commit(h);
    nvs_close(h);
}

const char *stepName(uint8_t step)
{
    switch (step) {
    case 1: return "tháo thẻ khỏi board";
    case 2: return "khởi tạo thẻ";
    case 3: return "cài MSC / tạo ổ đĩa";
    case 4: return "bật TinyUSB";
    case 11: return "trả PHY về Type-C";
    case 12: return "tắt TinyUSB";
    case 13: return "xoá ổ đĩa MSC";
    case 14: return "giải phóng thẻ";
    case 15: return "gắn lại thẻ cho board";
    default: return "?";
    }
}

void setError(const char *msg, esp_err_t err)
{
    snprintf(s_error, sizeof(s_error), "%s (%s)", msg, esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", s_error);
}

// USB-Serial/JTAG còn nhận khung SOF từ máy tính (1 khung/ms) qua cổng Type-C không.
// Lần thử trước chỉ so số khung (fram_num) thì không đổi dù máy tính đang cắm, nên giờ dò thêm bit
// SOF trong int_raw. Tick hook của ESP-IDF (usb_serial_jtag_connection_monitor) xoá bit này mỗi tick,
// vì vậy phải đọc liên tục (busy-poll) trong khoảng 20 ms chứ không đọc một lần.
bool usjSeesHost()
{
    const uint32_t f0 = USB_SERIAL_JTAG.fram_num.val & 0x7FF;
    const int64_t end = esp_timer_get_time() + 20000;
    while (esp_timer_get_time() < end) {
        if ((USB_SERIAL_JTAG.int_raw.val & (1u << 1)) != 0) return true;  // USB_SERIAL_JTAG_INTR_SOF
        if ((USB_SERIAL_JTAG.fram_num.val & 0x7FF) != f0) return true;
    }
    return false;
}

void setPhyBits(int sw_hw, int sel)
{
    LP_SYS.usb_ctrl.sw_usb_phy_sel = sel;
    LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = sw_hw;
    vTaskDelay(pdMS_TO_TICKS(30));
}

// Đưa OTG Full-Speed ra PHY của cổng Type-C (PHY mà USB-Serial/JTAG đang dùng).
// LP_SYS.usb_ctrl: theo chú thích ESP-IDF, sw_hw_usb_phy_sel = 1 cho phần mềm chọn và
// sw_usb_phy_sel = 1 đưa USJ->PHY1, OTG->PHY0. Thử trên board: chỉ ghi như vậy thì Serial/JTAG
// vẫn chạy trên Type-C (monitor không ngắt) -> ý nghĩa bit chưa chắc đúng như chú thích. Vì vậy thử
// lần lượt các tổ hợp và KIỂM CHỨNG: Serial/JTAG hết thấy khung SOF của máy tính = Type-C đã nhả.
// (usb_wrap_ll_phy_select() của ESP-IDF 5.5 còn thiếu 'break', không dùng.)
bool swapPhy(bool otg_on_type_c)
{
    if (!otg_on_type_c) {
        setPhyBits(0, 0);  // mặc định lúc khởi động
        s_phy_swapped = false;
        ESP_LOGI(TAG, "PHY: mặc định (Type-C = Serial/JTAG)");
        return true;
    }
    const bool efuse_sel = esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
    const int want = efuse_sel ? 0 : 1;  // sw_usb_phy_sel theo chú thích ESP-IDF
    const bool host_seen = usjSeesHost();
    if (!host_seen) {
        // Chưa cắm máy tính (hoặc không có SOF): không kiểm chứng được, dùng cách theo chú thích.
        setPhyBits(1, want);
        s_phy_swapped = true;
        snprintf(s_phy_note, sizeof(s_phy_note), "eFuse=%d, sw_hw=1 sel=%d (chưa kiểm chứng: không thấy SOF, int_raw=%08lx frame=%lu)",
                 efuse_sel ? 1 : 0, want, (unsigned long)USB_SERIAL_JTAG.int_raw.val,
                 (unsigned long)(USB_SERIAL_JTAG.fram_num.val & 0x7FF));
        ESP_LOGW(TAG, "PHY: %s", s_phy_note);
        return true;
    }
    const int combos[4][2] = {{1, want}, {0, want}, {0, !want}, {1, !want}};
    for (const auto &c : combos) {
        setPhyBits(c[0], c[1]);
        const bool still = usjSeesHost();
        ESP_LOGI(TAG, "PHY thử sw_hw=%d sel=%d -> Serial/JTAG %s Type-C", c[0], c[1], still ? "VẪN ở" : "đã nhả");
        if (!still) {
            s_phy_swapped = true;
            snprintf(s_phy_note, sizeof(s_phy_note), "eFuse=%d, sw_hw=%d sel=%d (Type-C đã chuyển)",
                     efuse_sel ? 1 : 0, c[0], c[1]);
            return true;
        }
    }
    setPhyBits(0, 0);
    s_phy_swapped = false;
    snprintf(s_phy_note, sizeof(s_phy_note), "eFuse=%d: không tổ hợp nào chuyển được Type-C", efuse_sel ? 1 : 0);
    ESP_LOGE(TAG, "PHY: %s", s_phy_note);
    return false;
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
    // Trả cổng Type-C cho USB-Serial/JTAG trước tiên: nếu bước sau crash thì monitor vẫn đọc được log.
    breadcrumb(11);
    if (s_phy_swapped) {
        swapPhy(false);
    }
    breadcrumb(12);
    if (s_tinyusb_installed) {
        tinyusb_driver_uninstall();
        s_tinyusb_installed = false;
    }
    breadcrumb(13);
    if (s_storage != nullptr) {
        tinyusb_msc_delete_storage(s_storage);
        s_storage = nullptr;
    }
    if (s_msc_driver_installed) {
        tinyusb_msc_uninstall_driver();
        s_msc_driver_installed = false;
    }
    breadcrumb(14);
    if (s_card != nullptr) {
        bsp_sdcard_raw_deinit(s_card);
        s_card = nullptr;
    }
    s_capacity_mb = 0;
    // Board dùng lại thẻ SD ở /sdcard (panic, case, nhạc).
    breadcrumb(15);
    const esp_err_t m = bsp_sdcard_mount();
    if (m != ESP_OK && m != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Gắn lại thẻ SD cho board thất bại: %s", esp_err_to_name(m));
    }
    breadcrumb(0);
}

void doStart()
{
    s_error[0] = '\0';
    breadcrumb(1);
    // 1) Board nhả thẻ: tháo FAT ở /sdcard (nếu đang gắn). Không có thẻ thì raw_init báo lỗi bên dưới.
    if (bsp_sdcard != nullptr) {
        const esp_err_t u = bsp_sdcard_unmount();
        if (u != ESP_OK) {
            setError("Không tháo được thẻ SD khỏi board", u);
            s_state = State::Error;
            return;
        }
    }
    breadcrumb(2);
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

    breadcrumb(3);
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

    breadcrumb(4);
    // 4) Đưa OTG Full-Speed ra cổng Type-C rồi bật TinyUSB.
    if (!swapPhy(true)) {
        snprintf(s_error, sizeof(s_error), "Không chuyển được cổng Type-C sang USB (xem dòng PHY)");
        teardown();
        s_state = State::Error;
        return;
    }
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
    breadcrumb(0);
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
    return xTaskCreatePinnedToCore(worker, start ? "hcc_usbdisk_on" : "hcc_usbdisk_off", 10240,
                                   start ? reinterpret_cast<void *>(1) : nullptr, 5, nullptr,
                                   HCC_CORE_WORK) == pdPASS;
}

}  // namespace

void bootInit()
{
    // LP_SYS giữ nguyên qua crash / reset mềm: nếu lần trước dừng giữa lúc USB DISK bật, cổng Type-C
    // vẫn bị đưa sang OTG -> mất monitor/flash. Luôn trả PHY về mặc định khi khởi động.
    LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 0;
    LP_SYS.usb_ctrl.sw_usb_phy_sel = 0;
    nvs_handle_t h;
    uint8_t step = 0;
    if (nvs_open("hcc_udisk", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_u8(h, "step", &step) == ESP_OK && step != 0) {
            snprintf(s_last_crash, sizeof(s_last_crash), "Lần trước board dừng ở bước: %s (%u)", stepName(step), step);
            ESP_LOGW(TAG, "%s", s_last_crash);
            nvs_set_u8(h, "step", 0);
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

const char *lastCrash()
{
    return s_last_crash;
}

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
