#include "HCCHome.hpp"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "driver/i2c_master.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <algorithm>
#include "HCCPanicStorage.hpp"
#include "HCCPanicParser.hpp"
#include "HCCPanicKB.hpp"
#include "HCCPanicAI.hpp"
#include "HCCPanicServer.hpp"
#include "HCCPanicKBStore.hpp"
#include "HCCPanicKBView.hpp"

// HCCMemory.cpp: operator new dùng PSRAM cho khối lớn; in RAM còn trống.
void hccMemoryReport(const char *where);
#include "HCCViText.hpp"
LV_IMG_DECLARE(img_app_hcc_insight);

// Lockdown ProductType -> marketing name. Unknown identifiers are shown as the raw code.
static const struct {
    const char *product_type;
    const char *name;
} kIPhoneModelNames[] = {
    {"iPhone8,1", "iPhone 6s"},           {"iPhone8,2", "iPhone 6s Plus"},
    {"iPhone8,4", "iPhone SE"},           {"iPhone9,1", "iPhone 7"},
    {"iPhone9,3", "iPhone 7"},            {"iPhone9,2", "iPhone 7 Plus"},
    {"iPhone9,4", "iPhone 7 Plus"},       {"iPhone10,1", "iPhone 8"},
    {"iPhone10,4", "iPhone 8"},           {"iPhone10,2", "iPhone 8 Plus"},
    {"iPhone10,5", "iPhone 8 Plus"},      {"iPhone10,3", "iPhone X"},
    {"iPhone10,6", "iPhone X"},           {"iPhone11,2", "iPhone XS"},
    {"iPhone11,4", "iPhone XS Max"},      {"iPhone11,6", "iPhone XS Max"},
    {"iPhone11,8", "iPhone XR"},          {"iPhone12,1", "iPhone 11"},
    {"iPhone12,3", "iPhone 11 Pro"},      {"iPhone12,5", "iPhone 11 Pro Max"},
    {"iPhone12,8", "iPhone SE (2nd gen)"}, {"iPhone13,1", "iPhone 12 mini"},
    {"iPhone13,2", "iPhone 12"},          {"iPhone13,3", "iPhone 12 Pro"},
    {"iPhone13,4", "iPhone 12 Pro Max"},  {"iPhone14,4", "iPhone 13 mini"},
    {"iPhone14,5", "iPhone 13"},          {"iPhone14,2", "iPhone 13 Pro"},
    {"iPhone14,3", "iPhone 13 Pro Max"},  {"iPhone14,6", "iPhone SE (3rd gen)"},
    {"iPhone14,7", "iPhone 14"},          {"iPhone14,8", "iPhone 14 Plus"},
    {"iPhone15,2", "iPhone 14 Pro"},      {"iPhone15,3", "iPhone 14 Pro Max"},
    {"iPhone15,4", "iPhone 15"},          {"iPhone15,5", "iPhone 15 Plus"},
    {"iPhone16,1", "iPhone 15 Pro"},      {"iPhone16,2", "iPhone 15 Pro Max"},
    {"iPhone17,3", "iPhone 16"},          {"iPhone17,4", "iPhone 16 Plus"},
    {"iPhone17,1", "iPhone 16 Pro"},      {"iPhone17,2", "iPhone 16 Pro Max"},
    {"iPhone17,5", "iPhone 16e"},         {"iPhone18,3", "iPhone 17"},
    {"iPhone18,4", "iPhone Air"},         {"iPhone18,1", "iPhone 17 Pro"},
    {"iPhone18,2", "iPhone 17 Pro Max"},  {"iPhone18,5", "iPhone 17e"},
};

static std::string panicMarketingName(const std::string &code);

static const char *iPhoneModelName(const char *product_type)
{
    for (const auto &entry : kIPhoneModelNames) {
        if (strcmp(entry.product_type, product_type) == 0) return entry.name;
    }
    return nullptr;
}

enum class AppleUdidDecodeResult {
    Ok,
    ActualLength,
    InvalidLength,
    OddLength,
    InvalidType,
    NonAscii,
    NonzeroAfterNul,
    DestinationCapacity,
    Empty,
};

static const char *appleUdidDecodeError(AppleUdidDecodeResult result)
{
    switch (result) {
    case AppleUdidDecodeResult::ActualLength: return "actual length below descriptor header";
    case AppleUdidDecodeResult::InvalidLength: return "invalid bLength";
    case AppleUdidDecodeResult::OddLength: return "odd bLength";
    case AppleUdidDecodeResult::InvalidType: return "invalid descriptor type";
    case AppleUdidDecodeResult::NonAscii: return "non-ASCII code unit";
    case AppleUdidDecodeResult::NonzeroAfterNul: return "nonzero code unit after trailing NUL";
    case AppleUdidDecodeResult::DestinationCapacity: return "destination capacity";
    case AppleUdidDecodeResult::Empty: return "empty serial";
    default: return "unknown";
    }
}

static AppleUdidDecodeResult normalizeAppleUdid(
    const uint8_t *descriptor, size_t actual_length,
    char *output, size_t output_capacity,
    size_t *descriptor_length, size_t *decoded_chars)
{
    if (descriptor_length != nullptr) *descriptor_length = 0;
    if (decoded_chars != nullptr) *decoded_chars = 0;
    if (descriptor == nullptr || output == nullptr || actual_length < 2) {
        return AppleUdidDecodeResult::ActualLength;
    }
    const size_t b_length = descriptor[0];
    if (descriptor_length != nullptr) *descriptor_length = b_length;
    if (b_length < 2 || b_length > actual_length) return AppleUdidDecodeResult::InvalidLength;
    if ((b_length & 1U) != 0) return AppleUdidDecodeResult::OddLength;
    if (descriptor[1] != USB_B_DESCRIPTOR_TYPE_STRING) return AppleUdidDecodeResult::InvalidType;

    const size_t source_chars = (b_length - 2) / 2;
    const size_t normalized_chars = source_chars + ((source_chars == 24) ? 1 : 0);
    if (normalized_chars + 1 > output_capacity) return AppleUdidDecodeResult::DestinationCapacity;
    if (source_chars == 0) return AppleUdidDecodeResult::Empty;

    size_t length = 0;
    bool trailing_nul_padding = false;
    for (size_t offset = 2; offset < b_length; offset += 2) {
        const uint16_t code_unit = static_cast<uint16_t>(descriptor[offset]) |
                                   (static_cast<uint16_t>(descriptor[offset + 1]) << 8);
        if (code_unit == 0) {
            trailing_nul_padding = true;
            continue;
        }
        if (trailing_nul_padding) return AppleUdidDecodeResult::NonzeroAfterNul;
        if (code_unit < 0x20 || code_unit > 0x7E) {
            return AppleUdidDecodeResult::NonAscii;
        }
        output[length++] = static_cast<char>(code_unit);
    }
    if (length == 0) return AppleUdidDecodeResult::Empty;
    if (decoded_chars != nullptr) *decoded_chars = length;
    if (length == 24) {
        memmove(output + 9, output + 8, 16);
        output[8] = '-';
        ++length;
    }
    output[length] = '\0';
    return AppleUdidDecodeResult::Ok;
}
bool HCCHome::claimUSBInterface(HCCHome *app)
{
    if (app == nullptr || app->_usb_client == nullptr || app->_usb_device == nullptr ||
        app->_usb_mux_bulk_out == 0 || app->_usb_mux_bulk_in == 0) {
        return false;
    }

    if (app->_usb_interface_claimed) {
        return true;
    }

    esp_err_t ret = usb_host_interface_claim(
        app->_usb_client,
        app->_usb_device,
        app->_usb_mux_interface,
        0
    );

    if (ret != ESP_OK) {
        printf("[HCC USB] Interface %u claim failed: %s\n", app->_usb_mux_interface, esp_err_to_name(ret));
        return false;
    }

    app->_usb_interface_claimed = true;
    printf("[HCC USB] Interface %u claimed successfully\n", app->_usb_mux_interface);
    return true;
}

bool HCCHome::releaseUSBInterface(HCCHome *app)
{
    if (app == nullptr ||
        app->_usb_client == nullptr ||
        app->_usb_device == nullptr ||
        !app->_usb_interface_claimed) {
        return true;
    }

    esp_err_t ret = usb_host_interface_release(
        app->_usb_client,
        app->_usb_device,
        app->_usb_mux_interface
    );

    if (ret == ESP_OK) {
        app->_usb_interface_claimed = false;
        printf("[HCC USB] Interface %u released\n", app->_usb_mux_interface);
        return true;
    } else {
        printf("[HCC USB] Interface %u release failed: %s\n", app->_usb_mux_interface, esp_err_to_name(ret));
        return false;
    }
}

bool HCCHome::findAppleUSBMuxInterface(HCCHome *app)
{
    if (app == nullptr || app->_usb_device == nullptr) {
        return false;
    }

    app->_usb_mux_interface = 0;
    app->_usb_mux_bulk_out = 0;
    app->_usb_mux_bulk_in = 0;
    app->_usb_mux_bulk_in_mps = 0;

    const usb_config_desc_t *config_desc = nullptr;
    esp_err_t ret = usb_host_get_active_config_descriptor(app->_usb_device, &config_desc);
    if (ret != ESP_OK || config_desc == nullptr) {
        printf("[HCC USB] Active configuration descriptor unavailable: %s\n", esp_err_to_name(ret));
        return false;
    }

    const uint8_t *data = config_desc->val;
    const size_t total_length = config_desc->wTotalLength;
    bool mux_interface_active = false;
    uint8_t mux_interface = 0;
    uint8_t bulk_out = 0;
    uint8_t bulk_in = 0;
    uint16_t bulk_in_mps = 0;

    printf("[HCC USB] Active configuration value=%u length=%u\n",
           config_desc->bConfigurationValue, static_cast<unsigned>(total_length));

    for (size_t offset = 0; offset + 2 <= total_length;) {
        const uint8_t descriptor_length = data[offset];
        const uint8_t descriptor_type = data[offset + 1];
        if (descriptor_length < 2 || offset + descriptor_length > total_length) {
            printf("[HCC USB] Malformed active configuration descriptor at offset=%u\n",
                   static_cast<unsigned>(offset));
            return false;
        }

        if (descriptor_type == USB_B_DESCRIPTOR_TYPE_INTERFACE && descriptor_length >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *interface_desc = reinterpret_cast<const usb_intf_desc_t *>(data + offset);
            mux_interface_active = interface_desc->bInterfaceClass == 0xFF &&
                                   interface_desc->bInterfaceSubClass == 0xFE &&
                                   interface_desc->bInterfaceProtocol == 0x02;
            if (mux_interface_active) {
                mux_interface = interface_desc->bInterfaceNumber;
                bulk_out = 0;
                bulk_in = 0;
                bulk_in_mps = 0;
            }
        } else if (mux_interface_active && descriptor_type == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   descriptor_length >= USB_EP_DESC_SIZE) {
            const usb_ep_desc_t *endpoint_desc = reinterpret_cast<const usb_ep_desc_t *>(data + offset);
            if (USB_EP_DESC_GET_XFERTYPE(endpoint_desc) == USB_TRANSFER_TYPE_BULK) {
                if (USB_EP_DESC_GET_EP_DIR(endpoint_desc)) {
                    bulk_in = endpoint_desc->bEndpointAddress;
                    bulk_in_mps = endpoint_desc->wMaxPacketSize;
                } else {
                    bulk_out = endpoint_desc->bEndpointAddress;
                }
                if (bulk_out != 0 && bulk_in != 0 && bulk_in_mps != 0) {
                    app->_usb_mux_interface = mux_interface;
                    app->_usb_mux_bulk_out = bulk_out;
                    app->_usb_mux_bulk_in = bulk_in;
                    app->_usb_mux_bulk_in_mps = bulk_in_mps;
                    printf("[HCC USB] USBMUX interface found: IF=%u OUT=0x%02X IN=0x%02X MPS=%u\n",
                           mux_interface, bulk_out, bulk_in, bulk_in_mps);
                    return true;
                }
            }
        }

        offset += descriptor_length;
    }

    printf("[HCC USB] USBMUX interface FF/FE/02 with bulk IN/OUT not found\n");
    return false;
}

bool HCCHome::closeUSBDevice(HCCHome *app)
{
    if (app == nullptr || app->_usb_client == nullptr || app->_usb_device == nullptr) {
        return true;
    }

    if (app->_usb_get_mode_pending ||
        app->_usb_get_mode_transfer != nullptr ||
        app->_usb_string_descriptor_pending ||
        app->_usb_string_descriptor_transfer != nullptr ||
        app->_adb.hasPendingTransfers() ||
        app->_fastboot.hasPendingTransfers() ||
        app->_recovery_exit_transfer != nullptr ||
        app->_usb_transport.hasPendingTransfers()) {
        return false;
    }

    if (!HCCHome::releaseUSBInterface(app)) {
        return false;
    }

    esp_err_t ret = usb_host_device_close(app->_usb_client, app->_usb_device);
    if (ret != ESP_OK) {
        printf("[HCC USB] Device close failed: %s\n", esp_err_to_name(ret));
        return false;
    }

    app->_usb_device = nullptr;
    printf("[HCC USB] Device closed\n");
    // Runs in hcc_usb_client: lowest free stack reached so far, including the TLS application phase.
    printf("[HCC USB] client stack_hwm_bytes=%u\n",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return true;
}

// USB vendor IDs of common Android phone makers, shown as the brand on USB HUB.
static const struct {
    uint16_t vid;
    const char *name;
} kAndroidVendors[] = {
    {0x18D1, "Google"},   {0x04E8, "Samsung"},  {0x2717, "Xiaomi"},   {0x22B8, "Motorola"},
    {0x12D1, "Huawei"},   {0x22D9, "OPPO/realme"}, {0x2A70, "OnePlus"}, {0x2D95, "vivo"},
    {0x1004, "LG"},       {0x0FCE, "Sony"},     {0x0BB4, "HTC"},      {0x19D2, "ZTE"},
    {0x17EF, "Lenovo"},   {0x0B05, "ASUS"},     {0x2E04, "Nokia (HMD)"}, {0x1949, "Amazon"},
    {0x05C6, "Qualcomm"}, {0x0E8D, "MediaTek"},
};

static const char *androidVendorName(uint16_t vid)
{
    for (const auto &entry : kAndroidVendors) {
        if (entry.vid == vid) {
            return entry.name;
        }
    }
    return nullptr;
}

// Cached USB string descriptor (UTF-16LE) -> printable ASCII; non-ASCII becomes '?'.
static void usbStringToAscii(const usb_str_desc_t *desc, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (desc == nullptr || desc->bLength < 2) {
        return;
    }

    const size_t chars = (desc->bLength - 2) / 2;
    size_t len = 0;
    for (size_t i = 0; i < chars && len + 1 < out_size; ++i) {
        const uint16_t c = desc->wData[i];
        if (c == 0) {
            break;
        }
        out[len++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    out[len] = '\0';
}

void HCCHome::describeNonAppleDevice(HCCHome *app, const usb_config_desc_t *config_desc,
                                     const usb_device_info_t &dev_info)
{
    const uint16_t vid = app->_usb_vid;
    const uint16_t pid = app->_usb_pid;

    // Android function interfaces: ADB FF/42/01, fastboot FF/42/03, MTP/PTP 06/01/01.
    bool has_adb = false;
    bool has_fastboot = false;
    bool has_mtp = false;
    bool in_android_interface = false;
    app->_android_interface = 0;
    app->_android_bulk_out = 0;
    app->_android_bulk_in = 0;
    app->_android_in_mps = 0;
    app->_android_out_mps = 0;
    if (config_desc != nullptr) {
        const uint8_t *data = config_desc->val;
        const size_t total_length = config_desc->wTotalLength;
        for (size_t offset = 0; offset + 2 <= total_length;) {
            const uint8_t descriptor_length = data[offset];
            if (descriptor_length < 2 || offset + descriptor_length > total_length) {
                break;
            }
            if (data[offset + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && descriptor_length >= USB_INTF_DESC_SIZE) {
                const usb_intf_desc_t *intf = reinterpret_cast<const usb_intf_desc_t *>(data + offset);
                in_android_interface = false;
                if (intf->bInterfaceClass == 0xFF && intf->bInterfaceSubClass == 0x42) {
                    if (intf->bInterfaceProtocol == 0x01) {
                        has_adb = true;
                        in_android_interface = intf->bAlternateSetting == 0;
                        app->_android_interface = intf->bInterfaceNumber;
                    } else if (intf->bInterfaceProtocol == 0x03) {
                        has_fastboot = true;
                        in_android_interface = intf->bAlternateSetting == 0;
                        app->_android_interface = intf->bInterfaceNumber;
                    }
                } else if (intf->bInterfaceClass == 0x06 && intf->bInterfaceSubClass == 0x01 &&
                           intf->bInterfaceProtocol == 0x01) {
                    has_mtp = true;
                }
            } else if (in_android_interface && data[offset + 1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                       descriptor_length >= USB_EP_DESC_SIZE) {
                const usb_ep_desc_t *ep = reinterpret_cast<const usb_ep_desc_t *>(data + offset);
                if (USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK) {
                    if (USB_EP_DESC_GET_EP_DIR(ep)) {
                        app->_android_bulk_in = ep->bEndpointAddress;
                        app->_android_in_mps = ep->wMaxPacketSize & 0x7FF;
                    } else {
                        app->_android_bulk_out = ep->bEndpointAddress;
                        app->_android_out_mps = ep->wMaxPacketSize & 0x7FF;
                    }
                }
            }
            offset += descriptor_length;
        }
    }

    // Low-level flashing modes are recognised by VID:PID, before any Android function check.
    const char *vendor = androidVendorName(vid);
    if (vid == 0x05C6 && pid == 0x9008) {
        app->_usb_mode = USBMode::QUALCOMM_EDL;
    } else if (vid == 0x0E8D && (pid == 0x0003 || pid == 0x2000)) {
        app->_usb_mode = USBMode::MTK_BOOTROM;
    } else if (vid == 0x04E8 && pid == 0x685D) {
        app->_usb_mode = USBMode::ANDROID_DOWNLOAD;
    } else if (has_fastboot) {
        app->_usb_mode = USBMode::ANDROID_FASTBOOT;
    } else if (has_adb || vendor != nullptr) {
        app->_usb_mode = USBMode::ANDROID;
    } else {
        app->_usb_mode = USBMode::UNKNOWN;
    }

    const bool android = app->_usb_mode != USBMode::UNKNOWN;
    snprintf(app->_usb_status, sizeof(app->_usb_status), "%s",
             android ? "ANDROID DEVICE DETECTED" : "NON-APPLE USB DEVICE");

    char manufacturer[48];
    char product[64];
    char serial[48];
    usbStringToAscii(dev_info.str_desc_manufacturer, manufacturer, sizeof(manufacturer));
    usbStringToAscii(dev_info.str_desc_product, product, sizeof(product));
    usbStringToAscii(dev_info.str_desc_serial_num, serial, sizeof(serial));
    snprintf(app->_usb_desc_serial, sizeof(app->_usb_desc_serial), "%s", serial);

    const char *debugging = "-";
    if (app->_usb_mode == USBMode::ANDROID) {
        debugging = has_adb ? "ON" : "OFF (enable in Developer options)";
    }

    char interfaces[48] = "";
    snprintf(interfaces, sizeof(interfaces), "%s%s%s",
             has_mtp ? "MTP " : "", has_adb ? "ADB " : "", has_fastboot ? "FASTBOOT " : "");

    snprintf(app->_usb_info, sizeof(app->_usb_info),
             "Brand: %s\nVID:PID: %04X:%04X\nManufacturer: %s\nProduct: %s\nSerial: %s\n"
             "Interfaces: %s\nUSB debugging: %s",
             vendor != nullptr ? vendor : "Unknown", vid, pid,
             manufacturer[0] != '\0' ? manufacturer : "-",
             product[0] != '\0' ? product : "-",
             serial[0] != '\0' ? serial : "-",
             interfaces[0] != '\0' ? interfaces : "-",
             debugging);

    printf("[HCC USB] Non-Apple device %04X:%04X android=%u adb=%u fastboot=%u mtp=%u\n",
           vid, pid, static_cast<unsigned>(android), static_cast<unsigned>(has_adb),
           static_cast<unsigned>(has_fastboot), static_cast<unsigned>(has_mtp));
}

void HCCHome::startAndroidSession(HCCHome *app)
{
    const bool adb = app->_usb_mode == USBMode::ANDROID;
    const bool fastboot = app->_usb_mode == USBMode::ANDROID_FASTBOOT;
    if ((!adb && !fastboot) || app->_android_bulk_in == 0 || app->_android_bulk_out == 0 ||
        app->_android_in_mps == 0 || app->_android_out_mps == 0) {
        return;
    }

    // Reuse the claimed-interface bookkeeping so closeUSBDevice() releases the interface.
    app->_usb_mux_interface = app->_android_interface;
    app->_usb_mux_bulk_out = app->_android_bulk_out;
    app->_usb_mux_bulk_in = app->_android_bulk_in;
    app->_usb_mux_bulk_in_mps = app->_android_in_mps;
    if (!HCCHome::claimUSBInterface(app)) {
        snprintf(app->_usb_status, sizeof(app->_usb_status), "%s INTERFACE CLAIM FAILED",
                 adb ? "ADB" : "FASTBOOT");
        return;
    }
    if (adb && !app->_adb.start(app->_usb_device, app->_android_bulk_out, app->_android_bulk_in,
                                app->_android_in_mps, app->_android_out_mps)) {
        snprintf(app->_usb_status, sizeof(app->_usb_status), "ADB START FAILED");
    }
    if (fastboot && !app->_fastboot.start(app->_usb_device, app->_android_bulk_out, app->_android_bulk_in,
                                          app->_android_in_mps)) {
        snprintf(app->_usb_status, sizeof(app->_usb_status), "FASTBOOT START FAILED");
    }
}

void HCCHome::applyFastbootState(HCCHome *app)
{
    if (app->_usb_device == nullptr || app->_usb_device_gone_pending) {
        return;
    }

    const HCCFastbootClient &fb = app->_fastboot;
    switch (fb.state()) {
    case HCCFastbootClient::State::Idle:
        return;
    case HCCFastbootClient::State::Reading:
        snprintf(app->_usb_status, sizeof(app->_usb_status), "FASTBOOT - READING INFO...");
        break;
    case HCCFastbootClient::State::Busy:
        snprintf(app->_usb_status, sizeof(app->_usb_status), "FASTBOOT - SENDING COMMAND...");
        break;
    case HCCFastbootClient::State::Failed:
        snprintf(app->_usb_status, sizeof(app->_usb_status), "FASTBOOT FAILED - replug the phone");
        break;
    case HCCFastbootClient::State::Ready:
        if (fb.commandError()[0] != '\0') {
            snprintf(app->_usb_status, sizeof(app->_usb_status), "FAIL: %s", fb.commandError());
        } else if (fb.commandCompleted()) {
            snprintf(app->_usb_status, sizeof(app->_usb_status), "OKAY - máy đang khởi động lại");
        } else {
            snprintf(app->_usb_status, sizeof(app->_usb_status), "FASTBOOT READY");
        }
        break;
    }

    if (fb.state() == HCCFastbootClient::State::Ready) {
        auto value = [&fb](HCCFastbootClient::Var var) {
            return fb.value(var)[0] != '\0' ? fb.value(var) : "-";
        };
        const char *unlocked = fb.value(HCCFastbootClient::VarUnlocked);
        const char *bootloader_state = strcmp(unlocked, "yes") == 0  ? "UNLOCKED"
                                       : strcmp(unlocked, "no") == 0 ? "LOCKED"
                                                                     : "-";
        snprintf(app->_usb_info, sizeof(app->_usb_info),
                 "Product: %s\nSerial: %s\nBootloader: %s (%s)\nBaseband: %s\nSecure boot: %s\n"
                 "Slot: %s\nBattery: %s mV\nMode: %s",
                 value(HCCFastbootClient::VarProduct),
                 fb.value(HCCFastbootClient::VarSerial)[0] != '\0' ? fb.value(HCCFastbootClient::VarSerial)
                                                                   : (app->_usb_desc_serial[0] != '\0'
                                                                          ? app->_usb_desc_serial
                                                                          : "-"),
                 bootloader_state, value(HCCFastbootClient::VarBootloader),
                 value(HCCFastbootClient::VarBaseband), value(HCCFastbootClient::VarSecure),
                 value(HCCFastbootClient::VarSlot), value(HCCFastbootClient::VarBatteryVoltage),
                 strcmp(fb.value(HCCFastbootClient::VarUserspace), "yes") == 0 ? "fastbootd (userspace)"
                                                                              : "bootloader");
    }
    lv_async_call(HCCHome::applyUSBResult, app);
}

void HCCHome::applyAdbState(HCCHome *app)
{
    if (app->_usb_device == nullptr || app->_usb_device_gone_pending) {
        return;
    }

    const char *status = nullptr;
    switch (app->_adb.state()) {
    case HCCAdbClient::State::Idle:
        return;
    case HCCAdbClient::State::Connecting:
        status = "ANDROID - ADB CONNECTING...";
        break;
    case HCCAdbClient::State::WaitingKey:
        status = "ANDROID - ADB AUTH...";
        break;
    case HCCAdbClient::State::WaitingUserAuth:
        status = "ADB: Bấm 'Cho phép' trên điện thoại";
        break;
    case HCCAdbClient::State::ReadingInfo:
        status = "ANDROID - READING INFO...";
        break;
    case HCCAdbClient::State::Ready:
        status = "SESSION READY - Android (ADB)";
        break;
    case HCCAdbClient::State::Failed:
        status = "ADB FAILED - replug the phone";
        break;
    }
    snprintf(app->_usb_status, sizeof(app->_usb_status), "%s", status);

    if (app->_adb.state() == HCCAdbClient::State::Ready) {
        const HCCAdbClient::Info &info = app->_adb.info();
        auto value = [](const char *text) { return text[0] != '\0' ? text : "-"; };
        char model[96];
        snprintf(model, sizeof(model), "%s%s%s%s%s", info.brand, info.brand[0] != '\0' ? " " : "",
                 value(info.model), info.device[0] != '\0' ? " / " : "", info.device);
        snprintf(app->_usb_info, sizeof(app->_usb_info),
                 "Model: %s\nAndroid: %s (SDK %s)\nBuild: %s\nSerial: %s\nChipset: %s\n"
                 "Battery: %s  %s  %s  %s\nCycles: %s",
                 model, value(info.android), value(info.sdk), value(info.build),
                 info.serial[0] != '\0' ? info.serial : value(app->_usb_desc_serial),
                 value(info.chipset), value(info.battery_level), value(info.battery_health),
                 value(info.battery_temp), value(info.battery_voltage), value(info.battery_cycles));
    }
    lv_async_call(HCCHome::applyUSBResult, app);
}

HCCHome::USBMode HCCHome::detectUSBMode(uint16_t vid, uint16_t pid)
{
    if (vid != 0x05AC) {
        return USBMode::UNKNOWN;
    }

    if (pid == 0x12A8) {
        return USBMode::NORMAL;
    }

    if (pid == 0x1227) {
        return USBMode::DFU;
    }

    if (pid >= 0x1280 && pid <= 0x1283) {
        return USBMode::RECOVERY;
    }

    return USBMode::UNKNOWN;
}

HCCHome::HCCHome()
    : ESP_Brookesia_PhoneApp("HCC INSIGHT", &img_app_hcc_insight, true),
      _external_i2c_bus(nullptr),
      _screen(nullptr),
      _previous_screen(nullptr),
      _scan_status_label(nullptr),
      _scan_result_label(nullptr),
      _scan_button(nullptr),
      _usb_status_label(nullptr),
      _usb_info_label(nullptr),
      _usb_mode_label(nullptr),
      _usb_client(nullptr),
      _usb_device(nullptr),
      _usb_client_task(nullptr),
      _usb_client_registered(false),
      _usb_interface_claimed(false),
      _usb_mux_interface(0),
      _usb_mux_bulk_out(0),
      _usb_mux_bulk_in(0),
      _usb_mux_bulk_in_mps(0),
      _usb_shutdown_requested(false),
      _usb_device_gone_pending(false),
      _usb_new_device_address(0),
      _usb_new_device_pending(false),
      _usb_session_reported(false),
      _usb_close_pending(false),
      _recovery_confirm_pending(false),
      _recovery_confirm_tick(0),
      _recovery_exit_requested(false),
      _recovery_exit_active(false),
      _recovery_exit_step(0),
      _recovery_exit_transfer(nullptr),
      _recovery_button_label(nullptr),
      _usb_get_mode_transfer(nullptr),
      _usb_get_mode_pending(false),
      _usb_string_descriptor_transfer(nullptr),
      _usb_string_descriptor_pending(false),
      _usb_string_descriptor_request(USBStringDescriptorRequest::None),
      _usb_i_serial_number(0),
      _usb_generation(0),
      _usb_udid_generation(0),
      _hw_sensors_status(nullptr),
      _hw_faceid_status(nullptr),
      _hw_baseband_status(nullptr),
      _hw_battery_status(nullptr),
      _hw_dialog(nullptr),
      _hw_has_result(false),
      _usb_mode(USBMode::NO_DEVICE),
      _usb_vid(0),
      _usb_pid(0),
      _scan_scanning(false),
      _eeprom_status_label(nullptr),
      _eeprom_result_label(nullptr),
      _eeprom_button(nullptr),
      _eeprom_program_button(nullptr),
      _eeprom_file_dropdown(nullptr),
      _eeprom_dialog(nullptr),
      _eeprom_view_buf(nullptr),
      _eeprom_view_len(0),
      _panic_file_dropdown(nullptr),
      _panic_status_label(nullptr),
      _panic_url_label(nullptr),
      _panic_analyze_button(nullptr),
      _panic_error_label(nullptr),
      _panic_cause_label(nullptr),
      _panic_repair_label(nullptr),
      _panic_conf_label(nullptr),
      _panic_tier_label(nullptr),
      _panic_apikey_dialog(nullptr),
      _panic_apikey_input(nullptr),
      _panic_keyboard(nullptr),
      _panic_view_dialog(nullptr),
      _repair_dialog(nullptr),
      _repair_kb(nullptr),
      _repair_model(nullptr),
      _repair_sig(nullptr),
      _repair_action(nullptr),
      _repair_note(nullptr),
      _pi_list(nullptr),
      _pi_list_count(nullptr),
      _pi_model(nullptr),
      _pi_ios(nullptr),
      _pi_soc(nullptr),
      _pi_file(nullptr),
      _pi_sev(nullptr),
      _pi_sevsub(nullptr),
      _pi_keystr(nullptr),
      _pi_cause(nullptr),
      _pi_check(nullptr),
      _pi_kb(nullptr),
      _pi_conclusion(nullptr),
      _pi_detail(nullptr),
      _panic_busy(false),
      _eeprom_busy(false),
      _eeprom_program_confirm_pending(false),
      _eeprom_program_confirm_tick(0)
{
    _usb_status[0] = '\0';
    _usb_udid[0] = '\0';
    _usb_info[0] = '\0';
    _scan_result[0] = '\0';
    _scan_status[0] = '\0';
    _eeprom_status[0] = '\0';
    _eeprom_result[0] = '\0';
    snprintf(_eeprom_file, sizeof(_eeprom_file), "eeprom_dump.bin");
    _panic_file[0] = '\0';
    _panic_status[0] = '\0';
}
bool HCCHome::init(void)
{
    // ADB host key: loaded (or generated once, which takes a few seconds) in the background.
    HCCAdbClient::startKeyLoad();

    // Non-fatal: pair record access falls back to lazy initialization.
    if (!HCCUSBTransport::initPairRecordStorage()) {
        printf("[HCC PAIR] early pair record storage init failed; using lazy init\n");
    }
    return _pair_identity.initialize();
}

bool HCCHome::run(void)
{
    _screen = lv_scr_act();

    // Reopened before a deferred close completed: keep the running client alive.
    _usb_close_pending = false;

    showHome();

    return true;
}
void HCCHome::showHome()
{
    if (_screen == nullptr) {
        return;
    }

    _usb_status_label = nullptr;
    _usb_info_label = nullptr;
    _usb_mode_label = nullptr;
    _recovery_button_label = nullptr;
    _bootloader_button_label = nullptr;
    _purple_button_label = nullptr;
    _reboot_button_label = nullptr;
    // Detach EEPROM labels before the old screen is cleaned so a pending
    // eepromTask result never writes to freed LVGL objects.
    _eeprom_status_label = nullptr;
    _eeprom_result_label = nullptr;
    _eeprom_button = nullptr;
    _eeprom_program_button = nullptr;
    _eeprom_file_dropdown = nullptr;
    _eeprom_dialog = nullptr;
    _panic_file_dropdown = nullptr;
    _panic_status_label = nullptr;
    _panic_url_label = nullptr;
    _panic_analyze_button = nullptr;
    _panic_error_label = nullptr;
    _panic_cause_label = nullptr;
    _panic_repair_label = nullptr;
    _panic_conf_label = nullptr;
    _panic_tier_label = nullptr;
    _panic_apikey_dialog = nullptr;
    _panic_apikey_input = nullptr;
    _panic_keyboard = nullptr;
    _panic_view_dialog = nullptr;
    _panic_detail_dialog = nullptr;
    _pi_conf = nullptr;
    _repair_dialog = nullptr;
    _repair_kb = nullptr;
    _repair_model = nullptr;
    _repair_sig = nullptr;
    _repair_action = nullptr;
    _repair_note = nullptr;
    _pi_list = nullptr;
    _pi_list_count = nullptr;
    _pi_model = nullptr;
    _pi_ios = nullptr;
    _pi_soc = nullptr;
    _pi_file = nullptr;
    _pi_sev = nullptr;
    _pi_sevsub = nullptr;
    _pi_keystr = nullptr;
    _pi_cause = nullptr;
    _pi_check = nullptr;
    _pi_kb = nullptr;
    _pi_conclusion = nullptr;
    _pi_ai = nullptr;
    _pi_detail = nullptr;
    lv_obj_clean(_screen);

    /* Background */
    lv_obj_set_style_bg_color(
        _screen,
        lv_color_hex(0x101820),
        0
    );

    /* Header */
    lv_obj_t *title = lv_label_create(_screen);
    lv_label_set_text(title, "HCC INSIGHT");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 16);
    lv_obj_set_style_text_color(
        title,
        lv_color_hex(0xFFFFFF),
        0
    );
    lv_obj_set_style_text_font(
        title,
        &lv_font_be_vietnam_28,
        0
    );

    lv_obj_t *sub = lv_label_create(_screen);
    lv_label_set_text(
        sub,
        "Repair & Diagnostic Platform"
    );
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 28, 50);
    lv_obj_set_style_text_color(
        sub,
        lv_color_hex(0xB8C5CF),
        0
    );
    lv_obj_set_style_text_font(
        sub,
        &lv_font_be_vietnam_18,
        0
    );

    /* Common colors */
    const lv_color_t button_color = lv_color_hex(0x36C9E8);
    const lv_color_t button_color_pressed = lv_color_hex(0x25AFCB);
    const lv_color_t dark_text = lv_color_hex(0x0B1F33);
    const lv_color_t divider = lv_color_hex(0x1687A3);

    /*
     * I2C
     */
    lv_obj_t *i2c_button = lv_btn_create(_screen);
    lv_obj_set_size(i2c_button, 438, 72);
    lv_obj_align(i2c_button, LV_ALIGN_TOP_LEFT, 28, 96);
    lv_obj_add_event_cb(
        i2c_button,
        onI2CClicked,
        LV_EVENT_CLICKED,
        this
    );
    lv_obj_set_style_bg_color(i2c_button, button_color, 0);
    lv_obj_set_style_bg_color(
        i2c_button,
        button_color_pressed,
        LV_STATE_PRESSED
    );
    lv_obj_set_style_radius(i2c_button, 14, 0);
    lv_obj_set_style_border_width(i2c_button, 0, 0);

    /* I2C icon */
    lv_obj_t *i2c_icon = lv_label_create(i2c_button);
    lv_label_set_text(i2c_icon, "[IC]");
    lv_obj_align(i2c_icon, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_color(i2c_icon, dark_text, 0);
    lv_obj_set_style_text_font(
        i2c_icon,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *i2c_div = lv_obj_create(i2c_button);
    lv_obj_set_size(i2c_div, 2, 48);
    lv_obj_align(i2c_div, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_set_style_bg_color(i2c_div, divider, 0);
    lv_obj_set_style_bg_opa(i2c_div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(i2c_div, 0, 0);

    lv_obj_t *i2c_title = lv_label_create(i2c_button);
    lv_label_set_text(i2c_title, "I2C");
    lv_obj_align(i2c_title, LV_ALIGN_LEFT_MID, 116, -10);
    lv_obj_set_style_text_color(i2c_title, dark_text, 0);
    lv_obj_set_style_text_font(
        i2c_title,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *i2c_sub = lv_label_create(i2c_button);
    lv_label_set_text(i2c_sub, "Bus diagnostics");
    lv_obj_align(i2c_sub, LV_ALIGN_LEFT_MID, 116, 16);
    lv_obj_set_style_text_color(i2c_sub, dark_text, 0);
    lv_obj_set_style_text_font(
        i2c_sub,
        &lv_font_be_vietnam_16,
        0
    );

    /*
     * RFFE
     */
    lv_obj_t *rffe_button = lv_btn_create(_screen);
    lv_obj_set_size(rffe_button, 438, 72);
    lv_obj_align(rffe_button, LV_ALIGN_TOP_LEFT, 494, 96);
    lv_obj_add_event_cb(
        rffe_button,
        onModuleClicked,
        LV_EVENT_CLICKED,
        this
    );
    lv_obj_set_style_bg_color(rffe_button, button_color, 0);
    lv_obj_set_style_bg_color(
        rffe_button,
        button_color_pressed,
        LV_STATE_PRESSED
    );
    lv_obj_set_style_radius(rffe_button, 14, 0);
    lv_obj_set_style_border_width(rffe_button, 0, 0);

    lv_obj_t *rffe_icon = lv_label_create(rffe_button);
    lv_label_set_text(rffe_icon, "[RF]");
    lv_obj_align(rffe_icon, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_color(rffe_icon, dark_text, 0);
    lv_obj_set_style_text_font(
        rffe_icon,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *rffe_div = lv_obj_create(rffe_button);
    lv_obj_set_size(rffe_div, 2, 48);
    lv_obj_align(rffe_div, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_set_style_bg_color(rffe_div, divider, 0);
    lv_obj_set_style_bg_opa(rffe_div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rffe_div, 0, 0);

    lv_obj_t *rffe_title = lv_label_create(rffe_button);
    lv_label_set_text(rffe_title, "RFFE");
    lv_obj_align(rffe_title, LV_ALIGN_LEFT_MID, 116, -10);
    lv_obj_set_style_text_color(rffe_title, dark_text, 0);
    lv_obj_set_style_text_font(
        rffe_title,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *rffe_sub = lv_label_create(rffe_button);
    lv_label_set_text(
        rffe_sub,
        "RF front-end diagnostics"
    );
    lv_obj_align(rffe_sub, LV_ALIGN_LEFT_MID, 116, 16);
    lv_obj_set_style_text_color(rffe_sub, dark_text, 0);
    lv_obj_set_style_text_font(
        rffe_sub,
        &lv_font_be_vietnam_16,
        0
    );

    /*
     * EEPROM
     */
    lv_obj_t *eeprom_button = lv_btn_create(_screen);
    lv_obj_set_size(eeprom_button, 438, 72);
    lv_obj_align(eeprom_button, LV_ALIGN_TOP_LEFT, 28, 176);
    lv_obj_add_event_cb(
        eeprom_button,
        onEepromClicked,
        LV_EVENT_CLICKED,
        this
    );
    lv_obj_set_style_bg_color(eeprom_button, button_color, 0);
    lv_obj_set_style_bg_color(
        eeprom_button,
        button_color_pressed,
        LV_STATE_PRESSED
    );
    lv_obj_set_style_radius(eeprom_button, 14, 0);
    lv_obj_set_style_border_width(eeprom_button, 0, 0);

    lv_obj_t *eeprom_icon = lv_label_create(eeprom_button);
    lv_label_set_text(eeprom_icon, "[MEM]");
    lv_obj_align(eeprom_icon, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_color(eeprom_icon, dark_text, 0);
    lv_obj_set_style_text_font(
        eeprom_icon,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *eeprom_div = lv_obj_create(eeprom_button);
    lv_obj_set_size(eeprom_div, 2, 48);
    lv_obj_align(eeprom_div, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_set_style_bg_color(eeprom_div, divider, 0);
    lv_obj_set_style_bg_opa(eeprom_div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eeprom_div, 0, 0);

    lv_obj_t *eeprom_title = lv_label_create(eeprom_button);
    lv_label_set_text(eeprom_title, "EEPROM");
    lv_obj_align(eeprom_title, LV_ALIGN_LEFT_MID, 116, -10);
    lv_obj_set_style_text_color(eeprom_title, dark_text, 0);
    lv_obj_set_style_text_font(
        eeprom_title,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *eeprom_sub = lv_label_create(eeprom_button);
    lv_label_set_text(
        eeprom_sub,
        "Memory / data inspection"
    );
    lv_obj_align(eeprom_sub, LV_ALIGN_LEFT_MID, 116, 16);
    lv_obj_set_style_text_color(eeprom_sub, dark_text, 0);
    lv_obj_set_style_text_font(
        eeprom_sub,
        &lv_font_be_vietnam_16,
        0
    );

    /*
     * PANIC / AI ANALYZE
     */
    lv_obj_t *panic_button = lv_btn_create(_screen);
    lv_obj_set_size(panic_button, 438, 72);
    lv_obj_align(panic_button, LV_ALIGN_TOP_LEFT, 494, 256);
    lv_obj_add_event_cb(
        panic_button,
        onPanicClicked,
        LV_EVENT_CLICKED,
        this
    );
    lv_obj_set_style_bg_color(panic_button, button_color, 0);
    lv_obj_set_style_bg_color(
        panic_button,
        button_color_pressed,
        LV_STATE_PRESSED
    );
    lv_obj_set_style_radius(panic_button, 14, 0);
    lv_obj_set_style_border_width(panic_button, 0, 0);

    lv_obj_t *panic_icon = lv_label_create(panic_button);
    lv_label_set_text(panic_icon, "[DOC]");
    lv_obj_align(panic_icon, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_color(panic_icon, dark_text, 0);
    lv_obj_set_style_text_font(
        panic_icon,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *panic_div = lv_obj_create(panic_button);
    lv_obj_set_size(panic_div, 2, 48);
    lv_obj_align(panic_div, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_set_style_bg_color(panic_div, divider, 0);
    lv_obj_set_style_bg_opa(panic_div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panic_div, 0, 0);

    lv_obj_t *panic_title = lv_label_create(panic_button);
    lv_label_set_text(panic_title, "PANIC & AI ANALYZE");
    lv_obj_align(panic_title, LV_ALIGN_LEFT_MID, 116, -10);
    lv_obj_set_style_text_color(panic_title, dark_text, 0);
    lv_obj_set_style_text_font(
        panic_title,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *panic_sub = lv_label_create(panic_button);
    lv_label_set_text(
        panic_sub,
        "Panic & repair analysis"
    );
    lv_obj_align(panic_sub, LV_ALIGN_LEFT_MID, 116, 16);
    lv_obj_set_style_text_color(panic_sub, dark_text, 0);
    lv_obj_set_style_text_font(
        panic_sub,
        &lv_font_be_vietnam_16,
        0
    );

    /*
     * BASEBAND
     */
    lv_obj_t *baseband_button = lv_btn_create(_screen);
    lv_obj_set_size(baseband_button, 438, 72);
    lv_obj_align(baseband_button, LV_ALIGN_TOP_LEFT, 28, 256);
    lv_obj_add_event_cb(
        baseband_button,
        onModuleClicked,
        LV_EVENT_CLICKED,
        this
    );
    lv_obj_set_style_bg_color(baseband_button, button_color, 0);
    lv_obj_set_style_bg_color(
        baseband_button,
        button_color_pressed,
        LV_STATE_PRESSED
    );
    lv_obj_set_style_radius(baseband_button, 14, 0);
    lv_obj_set_style_border_width(baseband_button, 0, 0);

    lv_obj_t *baseband_icon = lv_label_create(baseband_button);
    lv_label_set_text(baseband_icon, "[BAR]");
    lv_obj_align(baseband_icon, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_color(baseband_icon, dark_text, 0);
    lv_obj_set_style_text_font(
        baseband_icon,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *baseband_div = lv_obj_create(baseband_button);
    lv_obj_set_size(baseband_div, 2, 48);
    lv_obj_align(baseband_div, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_set_style_bg_color(baseband_div, divider, 0);
    lv_obj_set_style_bg_opa(baseband_div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(baseband_div, 0, 0);

    lv_obj_t *baseband_title = lv_label_create(baseband_button);
    lv_label_set_text(baseband_title, "BASEBAND");
    lv_obj_align(baseband_title, LV_ALIGN_LEFT_MID, 116, -10);
    lv_obj_set_style_text_color(baseband_title, dark_text, 0);
    lv_obj_set_style_text_font(
        baseband_title,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *baseband_sub = lv_label_create(baseband_button);
    lv_label_set_text(
        baseband_sub,
        "Baseband rail analysis"
    );
    lv_obj_align(baseband_sub, LV_ALIGN_LEFT_MID, 116, 16);
    lv_obj_set_style_text_color(baseband_sub, dark_text, 0);
    lv_obj_set_style_text_font(
        baseband_sub,
        &lv_font_be_vietnam_16,
        0
    );

    /*
     * USB HUB
     */
    lv_obj_t *usb_button = lv_btn_create(_screen);
    lv_obj_set_size(usb_button, 438, 72);
    lv_obj_align(usb_button, LV_ALIGN_TOP_LEFT, 494, 176);
    lv_obj_add_event_cb(
        usb_button,
        onUSBClicked,
        LV_EVENT_CLICKED,
        this
    );
    lv_obj_set_style_bg_color(usb_button, button_color, 0);
    lv_obj_set_style_bg_color(
        usb_button,
        button_color_pressed,
        LV_STATE_PRESSED
    );
    lv_obj_set_style_radius(usb_button, 14, 0);
    lv_obj_set_style_border_width(usb_button, 0, 0);

    lv_obj_t *usb_icon = lv_label_create(usb_button);
    lv_label_set_text(usb_icon, "[USB]");
    lv_obj_align(usb_icon, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_color(usb_icon, dark_text, 0);
    lv_obj_set_style_text_font(
        usb_icon,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *usb_div = lv_obj_create(usb_button);
    lv_obj_set_size(usb_div, 2, 48);
    lv_obj_align(usb_div, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_set_style_bg_color(usb_div, divider, 0);
    lv_obj_set_style_bg_opa(usb_div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usb_div, 0, 0);

    lv_obj_t *usb_title = lv_label_create(usb_button);
    lv_label_set_text(usb_title, "USB HUB");
    lv_obj_align(usb_title, LV_ALIGN_LEFT_MID, 116, -10);
    lv_obj_set_style_text_color(usb_title, dark_text, 0);
    lv_obj_set_style_text_font(
        usb_title,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *usb_sub = lv_label_create(usb_button);
    lv_label_set_text(
        usb_sub,
        "USB diagnostics"
    );
    lv_obj_align(usb_sub, LV_ALIGN_LEFT_MID, 116, 16);
    lv_obj_set_style_text_color(usb_sub, dark_text, 0);
    lv_obj_set_style_text_font(
        usb_sub,
        &lv_font_be_vietnam_16,
        0
    );

    /* Status */
    lv_obj_t *status = lv_label_create(_screen);
    lv_label_set_text(status, "Ready");
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 28, -18);
    lv_obj_set_style_text_color(
        status,
        lv_color_hex(0x69E0C0),
        0
    );
    lv_obj_set_style_text_font(
        status,
        &lv_font_be_vietnam_16,
        0
    );
}

void HCCHome::onModuleClicked(lv_event_t *event)
{
    HCCHome *app =
        static_cast<HCCHome *>(
            lv_event_get_user_data(event)
        );

    if (app == nullptr || app->_screen == nullptr) {
        return;
    }

    lv_obj_t *button = lv_event_get_target(event);

    if (button == nullptr) {
        return;
    }

    lv_obj_t *label = lv_obj_get_child(button, 0);

    if (label == nullptr) {
        return;
    }

    const char *module = lv_label_get_text(label);

    if (module == nullptr) {
        return;
    }

    lv_obj_clean(app->_screen);

    lv_obj_set_style_bg_color(
        app->_screen,
        lv_color_hex(0x101820),
        0
    );

    lv_obj_t *title = lv_label_create(app->_screen);
    lv_label_set_text(title, module);
    lv_obj_align(
        title,
        LV_ALIGN_TOP_LEFT,
        28,
        24
    );
    lv_obj_set_style_text_color(
        title,
        lv_color_hex(0xFFFFFF),
        0
    );
    lv_obj_set_style_text_font(
        title,
        &lv_font_be_vietnam_28,
        0
    );

    lv_obj_t *info = lv_label_create(app->_screen);
    lv_label_set_text(
        info,
        "Module ready"
    );
    lv_obj_align(
        info,
        LV_ALIGN_TOP_LEFT,
        28,
        80
    );
    lv_obj_set_style_text_color(
        info,
        lv_color_hex(0xB8C5CF),
        0
    );
    lv_obj_set_style_text_font(
        info,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *back_button = lv_btn_create(app->_screen);
    lv_obj_set_size(
        back_button,
        180,
        60
    );
    lv_obj_align(
        back_button,
        LV_ALIGN_BOTTOM_LEFT,
        28,
        -25
    );

    lv_obj_add_event_cb(
        back_button,
        onBackClicked,
        LV_EVENT_CLICKED,
        app
    );

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(
        back_label,
        "BACK"
    );
    lv_obj_center(back_label);
    lv_obj_set_style_text_font(
        back_label,
        &lv_font_be_vietnam_18,
        0
    );
}
void HCCHome::usbClientTask(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = HCCHome::onUSBClientEvent,
            .callback_arg = app,
        },
    };

    esp_err_t ret = usb_host_client_register(&client_config, &app->_usb_client);
    if (ret != ESP_OK) {
        app->_usb_client = nullptr;
        app->_usb_client_registered = false;
        app->_usb_client_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    app->_usb_client_registered = true;

    while (!app->_usb_shutdown_requested) {
        // Submit the deferred SETUP before choosing this iteration's event wait.
        // This guarantees a newly submitted SETUP selects the finite diagnostic
        // wait instead of inheriting a previously selected portMAX_DELAY.
        if (!app->_usb_transport.submitPendingSetup()) {
            printf("[HCC USB] Deferred USBMUX SETUP submit failed\n");
        }
        if (!app->_usb_transport.submitPendingTcp()) {
            printf("[HCC USB] Deferred USBMUX TCP submit failed\n");
        }

        const bool dwc_delayed_snapshot_pending = app->_usb_transport.needsDwcDelayedSnapshot();
        const bool pair_time_wait = !dwc_delayed_snapshot_pending && app->_usb_transport.preparePairTimeWait();
        const bool credential_work_wait = !dwc_delayed_snapshot_pending && !pair_time_wait &&
                                          (app->_pair_credentials.hasDeferredWork() ||
                                           app->_usb_transport.serviceRetryPending() ||
                                           app->_adb.needsPoll());
        const TickType_t event_timeout = dwc_delayed_snapshot_pending ? pdMS_TO_TICKS(250) :
                                         (pair_time_wait ? pdMS_TO_TICKS(1000) :
                                          (credential_work_wait ? pdMS_TO_TICKS(250) : portMAX_DELAY));
        if (dwc_delayed_snapshot_pending) {
            printf("[DWC FLOW] before handle_events timeout_ticks=%u needed=%u tx_inflight=%u tx_kind=%u pending=%u\\n",
                   static_cast<unsigned>(event_timeout),
                   static_cast<unsigned>(dwc_delayed_snapshot_pending),
                   static_cast<unsigned>(app->_usb_transport.txInFlight()),
                   static_cast<unsigned>(app->_usb_transport.txKindForDiagnostic()),
                   static_cast<unsigned>(app->_usb_transport.hasDwcDelayedSnapshotPending()));
        }
        ret = usb_host_client_handle_events(app->_usb_client, event_timeout);
        if (!((pair_time_wait || credential_work_wait) && ret == ESP_ERR_TIMEOUT)) {
            puts("[USB FLOW] handle_events returned");
        }
        if (dwc_delayed_snapshot_pending) {
            printf("[DWC FLOW] after handle_events ret=%d\\n", static_cast<int>(ret));
        }
        if (ret != ESP_OK &&
            !((dwc_delayed_snapshot_pending || pair_time_wait || credential_work_wait) &&
              ret == ESP_ERR_TIMEOUT)) {
            printf("[HCC USB] Client event handling failed: %s\n", esp_err_to_name(ret));
        }

        const bool dwc_delayed_snapshot_still_needed = app->_usb_transport.needsDwcDelayedSnapshot();
        const TickType_t dwc_delayed_snapshot_now = xTaskGetTickCount();
        const TickType_t dwc_delayed_snapshot_due = app->_usb_transport.dwcDelayedSnapshotDue();
        if (dwc_delayed_snapshot_pending || dwc_delayed_snapshot_still_needed) {
            printf("[DWC FLOW] delayed_needed=%u now=%u due=%u\\n",
                   static_cast<unsigned>(dwc_delayed_snapshot_still_needed),
                   static_cast<unsigned>(dwc_delayed_snapshot_now),
                   static_cast<unsigned>(dwc_delayed_snapshot_due));
        }
        if (app->_usb_transport.isDwcDelayedSnapshotDue()) {
            printf("[DWC FLOW] calling delayed snapshot\\n");
        }
        app->_usb_transport.takeDwcDelayedSnapshotIfDue();

        if (!app->_usb_transport.handoffDevicePublicKey(HCCHome::handoffDevicePublicKeyToCredentials, app)) {
            printf("[HCC PAIR CRYPTO] DevicePublicKey handoff failed\n");
        }
        HCCHome::consumeCompletedCredentialBundle(app);
        app->_usb_transport.persistPendingPairRecord();

        // Once the encrypted session is up and its info queries are done, surface the device
        // as trusted/ready and show the lockdown-derived info. Runs in this task, which owns
        // _usb_info, so the transport strings are read where they are written.
        if (!app->_usb_session_reported && app->_usb_transport.deviceInfoReady()) {
            app->_usb_session_reported = true;
            // "iPhone 14 Pro Max (iPhone15,3)", or just the raw code when it is not in the table.
            const char *product_type = app->_usb_transport.productType();
            const char *model_name = iPhoneModelName(product_type);
            char model[80];
            if (model_name != nullptr) {
                snprintf(model, sizeof(model), "%s (%s)", model_name, product_type);
            } else {
                snprintf(model, sizeof(model), "%s", product_type);
            }
            const struct {
                const char *label;
                const char *value;
            } rows[] = {
                {"Model", model},
                {"SN", app->_usb_transport.serialNumber()},
                {"IMEI", app->_usb_transport.imei()},
                {"Wi-Fi", app->_usb_transport.wifiAddress()},
                {"Bluetooth", app->_usb_transport.bluetoothAddress()},
                {"Baseband", app->_usb_transport.basebandVersion()},
            };
            size_t len = 0;
            app->_usb_info[0] = '\0';
            for (const auto &row : rows) {
                if (len >= sizeof(app->_usb_info)) break;
                len += snprintf(app->_usb_info + len, sizeof(app->_usb_info) - len, "%s%s: %s",
                                len == 0 ? "" : "\n", row.label,
                                row.value[0] != '\0' ? row.value : "-");
            }
            snprintf(app->_usb_status, sizeof(app->_usb_status), "SESSION READY - iPhone trusted");
            lv_async_call(HCCHome::applyUSBResult, app);
        }

        // Android: sign a pending AUTH token once the host key is ready, and surface ADB progress.
        app->_adb.poll();
        if (app->_adb.stateSerial() != app->_adb_state_seen) {
            app->_adb_state_seen = app->_adb.stateSerial();
            HCCHome::applyAdbState(app);
        }
        if (app->_android_action_requested != AndroidAction::None) {
            const AndroidAction action = app->_android_action_requested;
            app->_android_action_requested = AndroidAction::None;
            HCCHome::performAndroidAction(app, action);
        }
        if (app->_hw_android_requested || app->_cam_job_requested) {
            // Camera test steps run their own command (written by the UI before the flag).
            const bool camera_job = app->_cam_job_requested;
            app->_hw_android_requested = false;
            app->_cam_job_requested = false;
            app->_hw_last_job_camera = camera_job;
            if (!app->_adb.runJob(camera_job ? app->_cam_job_command : HCCAndroidDiagnostics::command())) {
                printf("[HCC ADB] diagnostics job could not start\n");
                app->_hw_android_start_failed = true;
                lv_async_call(HCCHome::applyAndroidDiagnostics, app);
            }
        }
        if (app->_adb.jobSerial() != app->_adb_job_seen) {
            app->_adb_job_seen = app->_adb.jobSerial();
            if (app->_adb.jobState() == HCCAdbClient::JobState::Done ||
                app->_adb.jobState() == HCCAdbClient::JobState::TimedOut) {
                lv_async_call(HCCHome::applyAndroidDiagnostics, app);
            }
        }
        if (app->_fastboot.stateSerial() != app->_fastboot_state_seen) {
            app->_fastboot_state_seen = app->_fastboot.stateSerial();
            HCCHome::applyFastbootState(app);
        }

        // UI-requested recovery-mode exit: start the iBoot command sequence from this task.
        if (app->_recovery_exit_requested && !app->_recovery_exit_active &&
            app->_usb_device != nullptr) {
            app->_recovery_exit_requested = false;
            app->_recovery_exit_active = true;
            app->_recovery_exit_step = 0;
            HCCHome::submitRecoveryExitCommand(app);
        }

        // Close a removed device before opening a new one, so a quick unplug/replug
        // does not overwrite the handle that is still waiting to be closed.
        if (app->_usb_device_gone_pending &&
            !app->_usb_get_mode_pending &&
            app->_usb_get_mode_transfer == nullptr &&
            !app->_usb_string_descriptor_pending &&
            app->_usb_string_descriptor_transfer == nullptr &&
            !app->_usb_transport.hasPendingTransfers() &&
            !app->_adb.hasPendingTransfers() &&
            !app->_fastboot.hasPendingTransfers()) {
            if (HCCHome::closeUSBDevice(app)) {
                app->_usb_device_gone_pending = false;
            }
        }

        if (app->_usb_new_device_pending) {
            HCCHome::processUSBNewDevice(app);
        }

        // Deferred shutdown: the app was closed while a device was attached; now that
        // it is gone, stop the client cleanly (no present device for the host to free).
        if (app->_usb_close_pending && app->_usb_device == nullptr &&
            !app->_usb_device_gone_pending) {
            app->_usb_close_pending = false;
            app->requestUSBClientShutdown();
        }
    }

    /*
     * A deliberate client shutdown must first retire a live bulk transfer. This
     * path is only for shutdown while the device is still connected; DEV_GONE is
     * handled by ESP-IDF, which already halts and flushes endpoints before it
     * dispatches the device-gone event.
     */
    ++app->_usb_generation;
    if (app->_usb_generation == 0) {
        ++app->_usb_generation;
    }
    app->_pair_credentials.setActiveGeneration(app->_usb_generation);
    app->_usb_transport.stopForShutdown();
    app->_adb.stopForShutdown();
    app->_fastboot.stopForShutdown();
    while (app->_usb_transport.hasPendingTransfers() || app->_adb.hasPendingTransfers() ||
           app->_fastboot.hasPendingTransfers()) {
        ret = usb_host_client_handle_events(app->_usb_client, portMAX_DELAY);
        if (ret != ESP_OK) {
            printf("[HCC USB] Client event handling during shutdown failed: %s\n", esp_err_to_name(ret));
        }
    }

    while (app->_usb_get_mode_pending ||
           app->_usb_get_mode_transfer != nullptr) {
        ret = usb_host_client_handle_events(app->_usb_client, portMAX_DELAY);
        if (ret != ESP_OK) {
            printf("[HCC USB] Client event handling during GET_MODE shutdown failed: %s\n",
                   esp_err_to_name(ret));
        }
    }

    while (app->_usb_string_descriptor_pending ||
           app->_usb_string_descriptor_transfer != nullptr) {
        ret = usb_host_client_handle_events(app->_usb_client, portMAX_DELAY);
        if (ret != ESP_OK) {
            printf("[HCC USB] Client event handling during string-descriptor shutdown failed: %s\n",
                   esp_err_to_name(ret));
        }
    }

    while (!HCCHome::closeUSBDevice(app)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    do {
        ret = usb_host_client_deregister(app->_usb_client);
        if (ret != ESP_OK) {
            printf("[HCC USB] Client deregister failed: %s\n", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } while (ret != ESP_OK);

    app->_usb_client = nullptr;
    app->_usb_client_registered = false;
    app->_usb_client_task = nullptr;
    app->_usb_shutdown_requested = false;
    app->_usb_device_gone_pending = false;
    printf("[HCC USB] Client stopped\n");
    vTaskDelete(nullptr);
}

bool HCCHome::handoffDevicePublicKeyToCredentials(void *context, const uint8_t *data, size_t length)
{
    HCCHome *app = static_cast<HCCHome *>(context);
    return app != nullptr &&
           app->_pair_credentials.queueDevicePublicKey(data, length, app->_usb_generation);
}

void HCCHome::consumeCompletedCredentialBundle(HCCHome *app)
{
    if (app == nullptr) {
        return;
    }
    HCCPairCredentialBundle *bundle = app->_pair_credentials.takeCompletedBundle(app->_usb_generation);
    if (bundle == nullptr) {
        return;
    }
    if (HCCPairCredentials::validateBundle(bundle)) {
        printf("[HCC PAIR] CREDENTIAL BUNDLE READY\n");
        printf("[HCC PAIR] generation=%u\n", static_cast<unsigned>(bundle->generation));
        printf("[HCC PAIR] root_key_bytes=%u\n",
               static_cast<unsigned>(bundle->root_private_key.length));
        printf("[HCC PAIR] host_key_bytes=%u\n",
               static_cast<unsigned>(bundle->host_private_key.length));
        printf("[HCC PAIR] root_cert_bytes=%u\n",
               static_cast<unsigned>(bundle->root_certificate.length));
        printf("[HCC PAIR] host_cert_bytes=%u\n",
               static_cast<unsigned>(bundle->host_certificate.length));
        printf("[HCC PAIR] device_cert_bytes=%u\n",
               static_cast<unsigned>(bundle->device_certificate.length));
        printf("[HCC PAIR] CREDENTIAL BUNDLE PHASE1 PASS\n");
        if (app->_usb_udid_generation != app->_usb_generation || app->_usb_udid[0] == '\0') {
            HCCPairCredentials::destroyBundle(bundle);
            printf("[HCC PAIR] Pair preparation failed\n");
        } else if (!app->_usb_transport.beginPair(bundle, app->_usb_generation,
                                                  app->_pair_identity.hostId(),
                                                  app->_pair_identity.systemBuid(), app->_usb_udid)) {
            printf("[HCC PAIR] Pair preparation failed\n");
        }
        return;
    } else {
        printf("[HCC PAIR] credential bundle validation failed\n");
    }
    HCCPairCredentials::destroyBundle(bundle);
}

void HCCHome::applyUSBResult(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app == nullptr) {
        return;
    }

    if (app->_usb_status_label != nullptr) {
        lv_label_set_text(app->_usb_status_label, app->_usb_status);
    }

    if (app->_usb_mode_label != nullptr) {
        const char *mode = "UNKNOWN";

        switch (app->_usb_mode) {
        case USBMode::NO_DEVICE:
            mode = "NO DEVICE";
            break;
        case USBMode::NORMAL:
            mode = "NORMAL";
            break;
        case USBMode::RECOVERY:
            mode = "RECOVERY";
            break;
        case USBMode::DFU:
            mode = "DFU";
            break;
        case USBMode::PURPLE:
            mode = "PURPLE";
            break;
        case USBMode::ANDROID:
            mode = "ANDROID";
            break;
        case USBMode::ANDROID_FASTBOOT:
            mode = "FASTBOOT";
            break;
        case USBMode::ANDROID_DOWNLOAD:
            mode = "DOWNLOAD (ODIN)";
            break;
        case USBMode::QUALCOMM_EDL:
            mode = "EDL 9008";
            break;
        case USBMode::MTK_BOOTROM:
            mode = "MTK BROM";
            break;
        case USBMode::UNKNOWN:
            mode = "UNKNOWN";
            break;
        }

        char mode_text[32];
        snprintf(mode_text, sizeof(mode_text), "MODE: %s", mode);
        lv_label_set_text(app->_usb_mode_label, mode_text);
    }

    if (app->_usb_info_label != nullptr) {
        lv_label_set_text(app->_usb_info_label, app->_usb_info);
    }

    HCCHome::updateActionLabels(app);
}

void HCCHome::updateActionLabels(HCCHome *app)
{
    // The RECOVERY button enters recovery in normal mode and exits it in recovery mode.
    if (app->_recovery_button_label != nullptr) {
        lv_label_set_text(app->_recovery_button_label,
                          app->_usb_mode == USBMode::RECOVERY ? "EXIT REC" : "RECOVERY");
    }

    // Android repurposes the DFU slot as "reboot to bootloader"; PURPLE has no Android meaning.
    const bool android = app->_usb_mode == USBMode::ANDROID || app->_usb_mode == USBMode::ANDROID_FASTBOOT;
    if (app->_bootloader_button_label != nullptr) {
        lv_label_set_text(app->_bootloader_button_label,
                          !android ? "DFU" : (app->_usb_mode == USBMode::ANDROID ? "FASTBOOT" : "-"));
    }
    if (app->_purple_button_label != nullptr) {
        lv_label_set_text(app->_purple_button_label, android ? "-" : "PURPLE");
    }
}

void HCCHome::continueUSBMuxStartup(HCCHome *app)
{
    if (app == nullptr ||
        app->_usb_client == nullptr ||
        app->_usb_device == nullptr ||
        app->_usb_shutdown_requested ||
        app->_usb_device_gone_pending ||
        app->_usb_string_descriptor_pending) {
        return;
    }

    if (!HCCHome::findAppleUSBMuxInterface(app)) {
        snprintf(
            app->_usb_status,
            sizeof(app->_usb_status),
            "USBMUX INTERFACE NOT FOUND"
        );
    } else if (HCCHome::claimUSBInterface(app)) {
        snprintf(
            app->_usb_status,
            sizeof(app->_usb_status),
            "USB INTERFACE READY"
        );

        if (app->_usb_i_serial_number == 0) {
            HCCHome::failUSBStringDescriptorStartup(app, "iSerialNumber is zero");
        } else if (!HCCHome::submitUSBStringDescriptor(
                       app,
                       USBStringDescriptorRequest::LangId,
                       0,
                       0)) {
            HCCHome::failUSBStringDescriptorStartup(app, "LANGID submit failed");
        }
    } else {
        snprintf(
            app->_usb_status,
            sizeof(app->_usb_status),
            "USB INTERFACE CLAIM FAILED"
        );
    }

    lv_async_call(HCCHome::applyUSBResult, app);
}

void HCCHome::failUSBStringDescriptorStartup(HCCHome *app, const char *reason)
{
    if (app == nullptr) {
        return;
    }

    printf("[HCC USB] USB string descriptor startup failed: %s\n", reason);
    snprintf(app->_usb_status, sizeof(app->_usb_status), "USB STRING DESCRIPTOR FAILED");
    lv_async_call(HCCHome::applyUSBResult, app);
}

bool HCCHome::submitUSBStringDescriptor(HCCHome *app, USBStringDescriptorRequest request,
                                         uint8_t string_index, uint16_t lang_id)
{
    static constexpr uint16_t kStringRequestLength = 1024 + sizeof(usb_setup_packet_t);

    if (app == nullptr ||
        app->_usb_client == nullptr ||
        app->_usb_device == nullptr ||
        app->_usb_string_descriptor_pending ||
        app->_usb_string_descriptor_transfer != nullptr ||
        request == USBStringDescriptorRequest::None) {
        return false;
    }

    usb_transfer_t *transfer = nullptr;
    esp_err_t ret = usb_host_transfer_alloc(
        sizeof(usb_setup_packet_t) + kStringRequestLength,
        0,
        &transfer
    );
    if (ret != ESP_OK || transfer == nullptr) {
        printf("[HCC USB] %s descriptor alloc failed: %s\n",
               (request == USBStringDescriptorRequest::LangId) ? "LANGID" : "SERIAL",
               esp_err_to_name(ret));
        return false;
    }

    usb_setup_packet_t *setup = reinterpret_cast<usb_setup_packet_t *>(transfer->data_buffer);
    setup->bmRequestType = 0x80;
    setup->bRequest = 0x06;
    setup->wValue = static_cast<uint16_t>(0x0300 | string_index);
    setup->wIndex = lang_id;
    setup->wLength = kStringRequestLength;

    transfer->device_handle = app->_usb_device;
    transfer->bEndpointAddress = 0x80;
    transfer->num_bytes = sizeof(usb_setup_packet_t) + kStringRequestLength;
    transfer->callback = HCCHome::onUSBStringDescriptorComplete;
    transfer->context = app;

    app->_usb_string_descriptor_transfer = transfer;
    app->_usb_string_descriptor_pending = true;
    app->_usb_string_descriptor_request = request;

    ret = usb_host_transfer_submit_control(app->_usb_client, transfer);
    printf("[HCC USB] %s descriptor submit: %s (wValue=0x%04X wIndex=0x%04X wLength=%u)\n",
           (request == USBStringDescriptorRequest::LangId) ? "LANGID" : "SERIAL",
           esp_err_to_name(ret),
           setup->wValue,
           setup->wIndex,
           static_cast<unsigned>(setup->wLength));
    if (ret != ESP_OK) {
        app->_usb_string_descriptor_pending = false;
        app->_usb_string_descriptor_transfer = nullptr;
        app->_usb_string_descriptor_request = USBStringDescriptorRequest::None;
        usb_host_transfer_free(transfer);
        return false;
    }

    return true;
}

void HCCHome::onUSBStringDescriptorComplete(usb_transfer_t *transfer)
{
    if (transfer == nullptr) {
        return;
    }

    HCCHome *app = static_cast<HCCHome *>(transfer->context);
    if (app == nullptr) {
        usb_host_transfer_free(transfer);
        return;
    }

    const USBStringDescriptorRequest request = app->_usb_string_descriptor_request;
    app->_usb_string_descriptor_pending = false;
    app->_usb_string_descriptor_transfer = nullptr;
    app->_usb_string_descriptor_request = USBStringDescriptorRequest::None;

    const char *request_name = (request == USBStringDescriptorRequest::LangId) ? "LANGID" : "SERIAL";
    const size_t setup_size = sizeof(usb_setup_packet_t);
    const bool completed = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
    const bool has_descriptor_header = transfer->actual_num_bytes >= static_cast<int>(setup_size + 2);
    const uint8_t *descriptor = has_descriptor_header ? transfer->data_buffer + setup_size : nullptr;
    const size_t payload_length = has_descriptor_header
                                      ? static_cast<size_t>(transfer->actual_num_bytes) - setup_size
                                      : 0;
    const bool descriptor_valid = descriptor != nullptr &&
                                  descriptor[0] >= 2 &&
                                  descriptor[0] <= payload_length &&
                                  descriptor[1] == USB_B_DESCRIPTOR_TYPE_STRING;

    printf("[HCC USB] %s descriptor complete: status=%d actual=%u\n",
           request_name,
           static_cast<int>(transfer->status),
           static_cast<unsigned>(transfer->actual_num_bytes));

    if (!completed) {
        usb_host_transfer_free(transfer);
        HCCHome::failUSBStringDescriptorStartup(app, "control transfer failed");
        return;
    }

    if (request == USBStringDescriptorRequest::LangId) {
        if (!descriptor_valid || descriptor[0] < 4 || (descriptor[0] & 1U) != 0) {
            usb_host_transfer_free(transfer);
            HCCHome::failUSBStringDescriptorStartup(app, "LANGID descriptor has no language ID");
            return;
        }

        const uint16_t lang_id = static_cast<uint16_t>(descriptor[2]) |
                                 (static_cast<uint16_t>(descriptor[3]) << 8);
        printf("[HCC USB] LANGID descriptor: lang=0x%04X\n", lang_id);
        usb_host_transfer_free(transfer);

        if (!HCCHome::submitUSBStringDescriptor(
                app,
                USBStringDescriptorRequest::Serial,
                app->_usb_i_serial_number,
                lang_id)) {
            HCCHome::failUSBStringDescriptorStartup(app, "SERIAL submit failed");
        }
        return;
    }

    if (request == USBStringDescriptorRequest::Serial) {
        size_t b_length = 0;
        size_t decoded_chars = 0;
        const AppleUdidDecodeResult decode_result = normalizeAppleUdid(
            descriptor, payload_length, app->_usb_udid, sizeof(app->_usb_udid),
            &b_length, &decoded_chars);
        if (decode_result != AppleUdidDecodeResult::Ok) {
            printf("[HCC USB] SERIAL descriptor parse failed: %s\n",
                   appleUdidDecodeError(decode_result));
            usb_host_transfer_free(transfer);
            HCCHome::failUSBStringDescriptorStartup(app, "SERIAL descriptor cannot form UDID");
            return;
        }
        app->_usb_udid_generation = app->_usb_generation;
        printf("[HCC USB] SERIAL descriptor parsed: bLength=%u chars=%u\n",
               static_cast<unsigned>(b_length), static_cast<unsigned>(decoded_chars));
        printf("[HCC PAIR] UDID READY length=%u\n",
               static_cast<unsigned>(strlen(app->_usb_udid)));
        usb_host_transfer_free(transfer);

        if (!app->_usb_shutdown_requested &&
            !app->_usb_device_gone_pending &&
            app->_usb_device != nullptr) {
            if (!app->_usb_transport.start(
                    app->_usb_device,
                    app->_usb_mux_bulk_out,
                    app->_usb_mux_bulk_in,
                    app->_usb_mux_bulk_in_mps)) {
                snprintf(app->_usb_status, sizeof(app->_usb_status), "USB MUX START FAILED");
                lv_async_call(HCCHome::applyUSBResult, app);
            } else {
                app->_usb_transport.loadPairRecordForReuse(app->_usb_udid, app->_usb_generation);
            }
        }
        return;
    }

    usb_host_transfer_free(transfer);
    HCCHome::failUSBStringDescriptorStartup(app, "unknown string descriptor request");
}

void HCCHome::onUSBGetModeComplete(usb_transfer_t *transfer)
{
    if (transfer == nullptr) {
        return;
    }

    HCCHome *app = static_cast<HCCHome *>(transfer->context);

    if (app == nullptr) {
        usb_host_transfer_free(transfer);
        return;
    }

    app->_usb_get_mode_pending = false;

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes >= 4) {
        const uint8_t *data =
            transfer->data_buffer + sizeof(usb_setup_packet_t);

        printf(
            "[HCC USB] GET_MODE complete: status=%d actual=%u data=%02X %02X %02X %02X\n",
            static_cast<int>(transfer->status),
            static_cast<unsigned>(transfer->actual_num_bytes),
            data[0], data[1], data[2], data[3]
        );
    } else {
        printf(
            "[HCC USB] GET_MODE complete: status=%d actual=%u\n",
            static_cast<int>(transfer->status),
            static_cast<unsigned>(transfer->actual_num_bytes)
        );
    }

    app->_usb_get_mode_transfer = nullptr;
    usb_host_transfer_free(transfer);

    if (!app->_usb_shutdown_requested &&
        !app->_usb_device_gone_pending &&
        app->_usb_device != nullptr) {
        HCCHome::continueUSBMuxStartup(app);
    }
}

bool HCCHome::submitUSBGetMode(HCCHome *app)
{
    if (app == nullptr ||
        app->_usb_client == nullptr ||
        app->_usb_device == nullptr ||
        app->_usb_get_mode_pending) {
        return false;
    }

    usb_transfer_t *transfer = nullptr;

    esp_err_t ret = usb_host_transfer_alloc(
        sizeof(usb_setup_packet_t) + 4,
        0,
        &transfer
    );

    if (ret != ESP_OK || transfer == nullptr) {
        printf(
            "[HCC USB] GET_MODE alloc failed: %s\n",
            esp_err_to_name(ret)
        );
        return false;
    }

    usb_setup_packet_t *setup =
        reinterpret_cast<usb_setup_packet_t *>(transfer->data_buffer);

    setup->bmRequestType = 0xC0;
    setup->bRequest = 0x45;
    setup->wValue = 0;
    setup->wIndex = 0;
    setup->wLength = 4;

    transfer->device_handle = app->_usb_device;
    transfer->bEndpointAddress = 0x80;
    transfer->num_bytes = sizeof(usb_setup_packet_t) + 4;
    transfer->callback = HCCHome::onUSBGetModeComplete;
    transfer->context = app;

    app->_usb_get_mode_transfer = transfer;
    app->_usb_get_mode_pending = true;

    ret = usb_host_transfer_submit_control(
        app->_usb_client,
        transfer
    );

    printf(
        "[HCC USB] GET_MODE submit: %s\n",
        esp_err_to_name(ret)
    );

    if (ret != ESP_OK) {
        app->_usb_get_mode_pending = false;
        app->_usb_get_mode_transfer = nullptr;
        usb_host_transfer_free(transfer);
        return false;
    }

    return true;
}

void HCCHome::processUSBNewDevice(HCCHome *app)
{
    if (app == nullptr || app->_usb_client == nullptr || app->_usb_shutdown_requested) {
        return;
    }

    if (!app->_usb_new_device_pending) {
        return;
    }

    // The previous device is still being closed; retry once its handle is released.
    if (app->_usb_device != nullptr) {
        return;
    }

    const uint8_t address = app->_usb_new_device_address;
    app->_usb_new_device_pending = false;
    app->_usb_i_serial_number = 0;

    usb_device_handle_t dev_hdl = nullptr;
    esp_err_t ret = usb_host_device_open(
        app->_usb_client,
        address,
        &dev_hdl
    );

    if (ret == ESP_OK) {
        const usb_device_desc_t *dev_desc = nullptr;
        usb_device_info_t dev_info = {};
        const usb_config_desc_t *config_desc = nullptr;
        bool non_apple = false;

        ret = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
        if (ret == ESP_OK && dev_desc != nullptr) {
            usb_host_device_info(dev_hdl, &dev_info);

            if (usb_host_get_active_config_descriptor(dev_hdl, &config_desc) == ESP_OK && config_desc != nullptr) {
                usb_print_config_descriptor(config_desc, nullptr);
            }

            app->_usb_vid = dev_desc->idVendor;
            app->_usb_pid = dev_desc->idProduct;
            app->_usb_i_serial_number = dev_desc->iSerialNumber;
            app->_usb_mode = HCCHome::detectUSBMode(
                app->_usb_vid,
                app->_usb_pid
            );
            non_apple = app->_usb_vid != 0x05AC;
            if (non_apple) {
                HCCHome::describeNonAppleDevice(app, config_desc, dev_info);
            }
        }

        if (non_apple) {
            // Nothing to claim: the handle stays open only so DEV_GONE closes it.
        } else if (ret == ESP_OK && dev_desc != nullptr) {
            snprintf(
                app->_usb_status,
                sizeof(app->_usb_status),
                "USB DEVICE DETECTED"
            );

            // Device info (Model, SN, IMEI, ...) replaces this once the encrypted session is up.
            snprintf(
                app->_usb_info,
                sizeof(app->_usb_info),
                "%s",
                app->_usb_mode == USBMode::NORMAL ? "Reading device info..." : ""
            );
        } else {
            snprintf(
                app->_usb_status,
                sizeof(app->_usb_status),
                "USB DEVICE OPENED"
            );
            snprintf(
                app->_usb_info,
                sizeof(app->_usb_info),
                "Address: %u\nDescriptor read failed",
                address
            );
        }

        app->_usb_device = dev_hdl;

        if (non_apple) {
            HCCHome::startAndroidSession(app);
            lv_async_call(HCCHome::applyUSBResult, app);
        } else if (app->_usb_vid == 0x05AC) {
            if (!HCCHome::submitUSBGetMode(app)) {
                printf("[HCC USB] GET_MODE submit failed - continuing USBMUX startup\n");
                HCCHome::continueUSBMuxStartup(app);
            }
        } else {
            HCCHome::continueUSBMuxStartup(app);
        }
    } else {
        snprintf(
            app->_usb_status,
            sizeof(app->_usb_status),
            "USB OPEN FAILED"
        );
        snprintf(
            app->_usb_info,
            sizeof(app->_usb_info),
            "Address: %u\nError: %s",
            address,
            esp_err_to_name(ret)
        );
        lv_async_call(HCCHome::applyUSBResult, app);
    }
}

void HCCHome::onUSBClientEvent(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app == nullptr || event_msg == nullptr) {
        return;
    }

    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (app->_usb_shutdown_requested) {
            return;
        }

        // One device at a time: a second device (e.g. through a hub) must not reset the
        // active device's session state or replace its handle. It is left unopened.
        if (app->_usb_device != nullptr && !app->_usb_device_gone_pending) {
            printf("[HCC USB] Ignoring new device at address %u: another device is active\n",
                   event_msg->new_dev.address);
            return;
        }

        ++app->_usb_generation;
        if (app->_usb_generation == 0) {
            ++app->_usb_generation;
        }
        app->_pair_credentials.setActiveGeneration(app->_usb_generation);
        app->_usb_udid_generation = 0;
        memset(app->_usb_udid, 0, sizeof(app->_usb_udid));
        app->_usb_new_device_address = event_msg->new_dev.address;
        app->_usb_new_device_pending = true;
        app->_usb_session_reported = false;
        app->_recovery_exit_requested = false;
        app->_recovery_exit_active = false;
        app->_recovery_exit_step = 0;
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        // Only the device this client opened; anything else must not clear its state.
        if (event_msg->dev_gone.dev_hdl != app->_usb_device) {
            return;
        }

        ++app->_usb_generation;
        if (app->_usb_generation == 0) {
            ++app->_usb_generation;
        }
        app->_pair_credentials.setActiveGeneration(app->_usb_generation);
        app->_usb_udid_generation = 0;
        memset(app->_usb_udid, 0, sizeof(app->_usb_udid));
        app->_usb_transport.stopForDeviceGone();
        app->_adb.stopForDeviceGone();
        app->_fastboot.stopForDeviceGone();
        app->_android_action_requested = AndroidAction::None;
        app->_usb_device_gone_pending = true;
        app->_usb_mode = USBMode::NO_DEVICE;
        app->_usb_vid = 0;
        app->_usb_pid = 0;
        app->_usb_session_reported = false;

        snprintf(
            app->_usb_status,
            sizeof(app->_usb_status),
            "USB DEVICE REMOVED"
        );
        snprintf(
            app->_usb_info,
            sizeof(app->_usb_info),
            "No USB device detected"
        );
        lv_async_call(HCCHome::applyUSBResult, app);
    }
}

void HCCHome::onUSBClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }
    app->showUSB();
    if (!app->_usb_client_registered && app->_usb_client_task == nullptr) {
        app->_usb_shutdown_requested = false;
        BaseType_t task_created = xTaskCreate(
            HCCHome::usbClientTask,
            "hcc_usb_client",
            6144,
            app,
            5,
            &app->_usb_client_task
        );
        if (task_created != pdPASS) {
            app->_usb_client_task = nullptr;
            printf("[HCC USB] Client task creation failed\n");
        }
    }
}

void HCCHome::onI2CClicked(lv_event_t *event)
{
    HCCHome *app =
        static_cast<HCCHome *>(
            lv_event_get_user_data(event)
        );

    if (app == nullptr) {
        return;
    }

    app->showI2C();
}

void HCCHome::onEepromClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }
    app->showEeprom();
}

// Shared setup for both EEPROM operations: mark busy, show status, disable both buttons.
void HCCHome::beginEepromOp(const char *status)
{
    _eeprom_busy = true;
    snprintf(_eeprom_status, sizeof(_eeprom_status), "%s", status);
    if (_eeprom_status_label != nullptr) {
        lv_label_set_text(_eeprom_status_label, _eeprom_status);
    }
    if (_eeprom_result_label != nullptr) {
        lv_label_set_text(_eeprom_result_label, "Vui lòng chờ...");
    }
    if (_eeprom_button != nullptr) {
        lv_obj_add_state(_eeprom_button, LV_STATE_DISABLED);
    }
    if (_eeprom_program_button != nullptr) {
        lv_obj_add_state(_eeprom_program_button, LV_STATE_DISABLED);
    }
}

void HCCHome::onEepromDumpClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_eeprom_busy) {
        return;
    }
    // Reading cancels any pending write confirmation.
    app->_eeprom_program_confirm_pending = false;
    app->beginEepromOp("Đang đọc EEPROM...");
    xTaskCreate(eepromTask, "hcc_eeprom", 6144, app, 5, nullptr);
}

void HCCHome::onEepromProgramClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_eeprom_busy) {
        return;
    }

    // First tap arms a 5s confirmation window; a second tap within it starts the write.
    if (!app->_eeprom_program_confirm_pending ||
        lv_tick_elaps(app->_eeprom_program_confirm_tick) > 5000) {
        app->_eeprom_program_confirm_pending = true;
        app->_eeprom_program_confirm_tick = lv_tick_get();
        snprintf(app->_eeprom_status, sizeof(app->_eeprom_status),
                 "Bấm lần nữa để xác nhận GHI");
        if (app->_eeprom_status_label != nullptr) {
            lv_label_set_text(app->_eeprom_status_label, app->_eeprom_status);
        }
        return;
    }

    app->_eeprom_program_confirm_pending = false;
    app->beginEepromOp("Đang ghi EEPROM...");
    xTaskCreate(eepromProgramTask, "hcc_eeprom_wr", 6144, app, 5, nullptr);
}

void HCCHome::onBackClicked(lv_event_t *event)
{
    HCCHome *app =
        static_cast<HCCHome *>(
            lv_event_get_user_data(event)
        );

    if (app == nullptr) {
        return;
    }

    app->showHome();
}

// UI task. Arms a 5 s confirmation on the first tap and hands the action to the USB task on
// the second. Returns false when the connected device is not an Android phone.
bool HCCHome::requestAndroidAction(HCCHome *app, AndroidAction action)
{
    const bool adb_mode = app->_usb_mode == USBMode::ANDROID;
    const bool fastboot_mode = app->_usb_mode == USBMode::ANDROID_FASTBOOT;
    if (!adb_mode && !fastboot_mode) {
        return false;
    }

    const char *name = action == AndroidAction::Reboot     ? "REBOOT"
                       : action == AndroidAction::Recovery ? "RECOVERY"
                                                           : "FASTBOOT";
    char text[96];
    auto show = [app](const char *message) {
        if (app->_usb_status_label != nullptr) {
            lv_label_set_text(app->_usb_status_label, message);
        }
    };

    if (fastboot_mode && action == AndroidAction::Bootloader) {
        show("Máy đang ở chế độ FASTBOOT");
        return true;
    }

    const HCCAdbClient::State adb_state = app->_adb.state();
    const HCCFastbootClient::State fastboot_state = app->_fastboot.state();
    const bool ready = adb_mode ? (adb_state == HCCAdbClient::State::ReadingInfo ||
                                   adb_state == HCCAdbClient::State::Ready)
                                : (fastboot_state == HCCFastbootClient::State::Reading ||
                                   fastboot_state == HCCFastbootClient::State::Ready);
    if (!ready) {
        app->_android_confirm_action = AndroidAction::None;
        snprintf(text, sizeof(text), adb_mode ? "%s: cần bật gỡ lỗi USB và bấm Cho phép" : "%s: chưa sẵn sàng",
                 name);
        show(text);
        return true;
    }

    if (app->_android_confirm_action != action || lv_tick_elaps(app->_recovery_confirm_tick) > 5000) {
        app->_android_confirm_action = action;
        app->_recovery_confirm_tick = lv_tick_get();
        snprintf(text, sizeof(text), "Bấm %s lần nữa để xác nhận", name);
        show(text);
        return true;
    }

    app->_android_confirm_action = AndroidAction::None;
    app->_android_action_requested = action;
    if (app->_usb_client_registered && app->_usb_client != nullptr) {
        usb_host_client_unblock(app->_usb_client);
    }
    snprintf(text, sizeof(text), "ĐANG GỬI LỆNH %s...", name);
    show(text);
    return true;
}

// USB client task: sends the reboot request over ADB or fastboot.
void HCCHome::performAndroidAction(HCCHome *app, AndroidAction action)
{
    bool sent = false;
    if (app->_usb_mode == USBMode::ANDROID) {
        const char *service = action == AndroidAction::Reboot     ? "reboot:"
                              : action == AndroidAction::Recovery ? "reboot:recovery"
                                                                  : "reboot:bootloader";
        sent = app->_adb.runService(service);
    } else if (app->_usb_mode == USBMode::ANDROID_FASTBOOT) {
        const char *command = action == AndroidAction::Reboot     ? "reboot"
                              : action == AndroidAction::Recovery ? "reboot-recovery"
                                                                  : "reboot-bootloader";
        sent = app->_fastboot.sendCommand(command);
    }
    snprintf(app->_usb_status, sizeof(app->_usb_status), "%s",
             sent ? "ĐÃ GỬI LỆNH - chờ máy khởi động lại" : "KHÔNG GỬI ĐƯỢC LỆNH");
    lv_async_call(HCCHome::applyUSBResult, app);
}

void HCCHome::onBootloaderClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        HCCHome::requestAndroidAction(app, AndroidAction::Bootloader);
    }
}

void HCCHome::onRebootClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        HCCHome::requestAndroidAction(app, AndroidAction::Reboot);
    }
}

void HCCHome::onRecoveryClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }

    if (HCCHome::requestAndroidAction(app, AndroidAction::Recovery)) {
        return;
    }

    // Exit (recovery mode) uses iBoot control commands and needs no session; only the
    // enter path requires the encrypted session to be up.
    const bool in_recovery = (app->_usb_mode == USBMode::RECOVERY);
    if (!in_recovery && !app->_usb_transport.sessionEstablished()) {
        app->_recovery_confirm_pending = false;
        if (app->_usb_status_label != nullptr) {
            lv_label_set_text(app->_usb_status_label, "RECOVERY: chưa sẵn sàng");
        }
        return;
    }

    // First tap arms a 5s confirmation window; a second tap within it sends the request.
    if (!app->_recovery_confirm_pending || lv_tick_elaps(app->_recovery_confirm_tick) > 5000) {
        app->_recovery_confirm_pending = true;
        app->_recovery_confirm_tick = lv_tick_get();
        if (app->_usb_status_label != nullptr) {
            lv_label_set_text(app->_usb_status_label, "Bấm RECOVERY lần nữa để xác nhận");
        }
        return;
    }

    app->_recovery_confirm_pending = false;

    // In recovery mode the same button exits recovery via iBoot commands.
    if (in_recovery) {
        app->_recovery_exit_requested = true;
        if (app->_usb_client_registered && app->_usb_client != nullptr) {
            usb_host_client_unblock(app->_usb_client);
        }
        if (app->_usb_status_label != nullptr) {
            lv_label_set_text(app->_usb_status_label, "ĐANG THOÁT RECOVERY...");
        }
        return;
    }

    if (app->_usb_transport.requestEnterRecovery()) {
        // Wake the USB client so the queued request is flushed over the session.
        if (app->_usb_client_registered && app->_usb_client != nullptr) {
            usb_host_client_unblock(app->_usb_client);
        }
        if (app->_usb_status_label != nullptr) {
            lv_label_set_text(app->_usb_status_label, "ĐANG VÀO RECOVERY...");
        }
    } else if (app->_usb_status_label != nullptr) {
        lv_label_set_text(app->_usb_status_label, "RECOVERY: chưa sẵn sàng");
    }
}

bool HCCHome::submitRecoveryExitCommand(HCCHome *app)
{
    static const char *const kExitCommands[] = {"setenv auto-boot true", "saveenv", "reboot"};
    if (app == nullptr || app->_usb_client == nullptr || app->_usb_device == nullptr) {
        if (app != nullptr) app->_recovery_exit_active = false;
        return false;
    }
    if (app->_recovery_exit_step >= 3) {
        app->_recovery_exit_active = false;
        return true;
    }

    const char *command = kExitCommands[app->_recovery_exit_step];
    const size_t command_length = strlen(command) + 1;  // include the NUL iBoot expects

    usb_transfer_t *transfer = nullptr;
    esp_err_t ret = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + command_length, 0, &transfer);
    if (ret != ESP_OK || transfer == nullptr) {
        printf("[HCC RECOVERY] exit cmd alloc failed: %s\n", esp_err_to_name(ret));
        app->_recovery_exit_active = false;
        return false;
    }

    usb_setup_packet_t *setup = reinterpret_cast<usb_setup_packet_t *>(transfer->data_buffer);
    setup->bmRequestType = 0x40;  // host-to-device, vendor, device
    setup->bRequest = 0;
    setup->wValue = 0;
    setup->wIndex = 0;
    setup->wLength = static_cast<uint16_t>(command_length);
    memcpy(transfer->data_buffer + sizeof(usb_setup_packet_t), command, command_length);

    transfer->device_handle = app->_usb_device;
    transfer->bEndpointAddress = 0x00;  // control OUT
    transfer->num_bytes = sizeof(usb_setup_packet_t) + command_length;
    transfer->callback = HCCHome::onRecoveryExitComplete;
    transfer->context = app;

    app->_recovery_exit_transfer = transfer;
    ret = usb_host_transfer_submit_control(app->_usb_client, transfer);
    if (ret != ESP_OK) {
        printf("[HCC RECOVERY] exit cmd submit failed: %s\n", esp_err_to_name(ret));
        usb_host_transfer_free(transfer);
        app->_recovery_exit_transfer = nullptr;
        app->_recovery_exit_active = false;
        return false;
    }
    printf("[HCC RECOVERY] exit cmd: %s\n", command);
    return true;
}

void HCCHome::onRecoveryExitComplete(usb_transfer_t *transfer)
{
    if (transfer == nullptr) {
        return;
    }
    HCCHome *app = static_cast<HCCHome *>(transfer->context);
    printf("[HCC RECOVERY] exit cmd complete: status=%d\n", static_cast<int>(transfer->status));
    usb_host_transfer_free(transfer);
    if (app == nullptr) {
        return;
    }
    app->_recovery_exit_transfer = nullptr;
    // Advance regardless of status so 'reboot' still runs even if an earlier command stalls.
    app->_recovery_exit_step++;
    if (app->_recovery_exit_step < 3 && !app->_usb_shutdown_requested && app->_usb_device != nullptr) {
        HCCHome::submitRecoveryExitCommand(app);
    } else {
        app->_recovery_exit_active = false;
        printf("[HCC RECOVERY] exit sequence done\n");
    }
}

bool HCCHome::back(void)
{
    notifyCoreClosed();
    return true;
}

bool HCCHome::close(void)
{
    // Brookesia cleans the app's LVGL objects after this returns; detach the label
    // pointers so the background USB client task never writes to freed objects.
    _usb_status_label = nullptr;
    _usb_info_label = nullptr;
    _usb_mode_label = nullptr;
    _recovery_button_label = nullptr;
    _bootloader_button_label = nullptr;
    _purple_button_label = nullptr;
    _reboot_button_label = nullptr;
    _scan_status_label = nullptr;
    _scan_result_label = nullptr;
    _scan_button = nullptr;
    _hw_sensors_status = nullptr;
    _hw_faceid_status = nullptr;
    _hw_baseband_status = nullptr;
    _hw_battery_status = nullptr;
    _hw_camera_status = nullptr;
    _hw_session_label = nullptr;
    _hw_dialog = nullptr;
    _panic_file_dropdown = nullptr;
    _panic_status_label = nullptr;
    _panic_url_label = nullptr;
    _panic_analyze_button = nullptr;
    _panic_error_label = nullptr;
    _panic_cause_label = nullptr;
    _panic_repair_label = nullptr;
    _panic_conf_label = nullptr;
    _panic_tier_label = nullptr;
    _panic_apikey_dialog = nullptr;
    _panic_apikey_input = nullptr;
    _panic_keyboard = nullptr;
    _panic_view_dialog = nullptr;
    _panic_detail_dialog = nullptr;
    _pi_conf = nullptr;
    _repair_dialog = nullptr;
    _repair_kb = nullptr;
    _repair_model = nullptr;
    _repair_sig = nullptr;
    _repair_action = nullptr;
    _repair_note = nullptr;
    _pi_list = nullptr;
    _pi_list_count = nullptr;
    _pi_model = nullptr;
    _pi_ios = nullptr;
    _pi_soc = nullptr;
    _pi_file = nullptr;
    _pi_sev = nullptr;
    _pi_sevsub = nullptr;
    _pi_keystr = nullptr;
    _pi_cause = nullptr;
    _pi_check = nullptr;
    _pi_kb = nullptr;
    _pi_conclusion = nullptr;
    _pi_ai = nullptr;
    _pi_detail = nullptr;

    // Dừng HTTP server nhận panic khi đóng app.
    HCCPanicServer::stop();

    if (_usb_device == nullptr) {
        // No device attached: stopping the client now is safe.
        _usb_close_pending = false;
        requestUSBClientShutdown();
    } else {
        // A device is still attached. Deregistering the client now would make the
        // USB host free a present device and later abort in the hub driver, so keep
        // the client running and shut it down once the device is removed.
        _usb_close_pending = true;
    }
    return true;
}

void HCCHome::requestUSBClientShutdown()
{
    _usb_shutdown_requested = true;

    if (_usb_client_registered && _usb_client != nullptr) {
        esp_err_t ret = usb_host_client_unblock(_usb_client);
        if (ret != ESP_OK) {
            printf("[HCC USB] Client unblock failed: %s\n", esp_err_to_name(ret));
        }
    }
}
void HCCHome::showUSB()
{
    if (_screen == nullptr) {
        return;
    }

    _usb_status_label = nullptr;
    _usb_info_label = nullptr;
    _usb_mode_label = nullptr;
    _recovery_button_label = nullptr;
    _bootloader_button_label = nullptr;
    _purple_button_label = nullptr;
    _reboot_button_label = nullptr;

    lv_obj_clean(_screen);

    lv_obj_set_style_bg_color(
        _screen,
        lv_color_hex(0x101820),
        0
    );

    /* Header */
    lv_obj_t *title = lv_label_create(_screen);
    lv_label_set_text(title, "USB HUB");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 18);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_be_vietnam_28, 0);

    lv_obj_t *subtitle = lv_label_create(_screen);
    lv_label_set_text(subtitle, "APPLE DEVICE REPAIR");
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 30, 55);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x7F96A8), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_be_vietnam_18, 0);

    _usb_status_label = lv_label_create(_screen);
    lv_label_set_text(_usb_status_label, "USB HOST READY");
    lv_obj_align(_usb_status_label, LV_ALIGN_TOP_RIGHT, -30, 24);
    lv_obj_set_style_text_color(_usb_status_label, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(_usb_status_label, &lv_font_be_vietnam_18, 0);

    /* Device information panel */
    lv_obj_t *device_panel = lv_obj_create(_screen);
    lv_obj_set_size(device_panel, 460, 250);
    lv_obj_align(device_panel, LV_ALIGN_TOP_LEFT, 28, 95);
    lv_obj_set_style_bg_color(device_panel, lv_color_hex(0x182630), 0);
    lv_obj_set_style_border_width(device_panel, 1, 0);
    lv_obj_set_style_border_color(device_panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_radius(device_panel, 12, 0);
    lv_obj_clear_flag(device_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *device_title = lv_label_create(device_panel);
    lv_label_set_text(device_title, "DEVICE");
    lv_obj_align(device_title, LV_ALIGN_TOP_LEFT, 18, 14);
    lv_obj_set_style_text_color(device_title, lv_color_hex(0x7F96A8), 0);
    lv_obj_set_style_text_font(device_title, &lv_font_be_vietnam_18, 0);

    _usb_info_label = lv_label_create(device_panel);
    lv_label_set_text(_usb_info_label, "No USB device detected");
    lv_obj_align(_usb_info_label, LV_ALIGN_TOP_LEFT, 18, 42);
    lv_obj_set_width(_usb_info_label, 420);
    lv_obj_set_style_text_color(_usb_info_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_usb_info_label, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_text_line_space(_usb_info_label, -2, 0);

    /* Mode panel */
    lv_obj_t *mode_panel = lv_obj_create(_screen);
    lv_obj_set_size(mode_panel, 480, 250);
    lv_obj_align(mode_panel, LV_ALIGN_TOP_LEFT, 514, 95);
    lv_obj_set_style_bg_color(mode_panel, lv_color_hex(0x182630), 0);
    lv_obj_set_style_border_width(mode_panel, 1, 0);
    lv_obj_set_style_border_color(mode_panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_radius(mode_panel, 12, 0);
    lv_obj_clear_flag(mode_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mode_title = lv_label_create(mode_panel);
    lv_label_set_text(mode_title, "CURRENT MODE");
    lv_obj_align(mode_title, LV_ALIGN_TOP_LEFT, 18, 14);
    lv_obj_set_style_text_color(mode_title, lv_color_hex(0x7F96A8), 0);
    lv_obj_set_style_text_font(mode_title, &lv_font_be_vietnam_18, 0);

    _usb_mode_label = lv_label_create(mode_panel);
    lv_label_set_text(_usb_mode_label, "MODE: NO DEVICE");
    lv_obj_align(_usb_mode_label, LV_ALIGN_TOP_LEFT, 18, 52);
    lv_obj_set_style_text_color(_usb_mode_label, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(_usb_mode_label, &lv_font_be_vietnam_28, 0);

    lv_obj_t *mode_hint = lv_label_create(mode_panel);
    lv_label_set_text(mode_hint, "USB mode detection");
    lv_obj_align(mode_hint, LV_ALIGN_TOP_LEFT, 18, 100);
    lv_obj_set_style_text_color(mode_hint, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(mode_hint, &lv_font_be_vietnam_18, 0);

    /* Repair action area */
    lv_obj_t *action_title = lv_label_create(_screen);
    lv_label_set_text(action_title, "REPAIR ACTIONS");
    lv_obj_align(action_title, LV_ALIGN_TOP_LEFT, 28, 362);
    lv_obj_set_style_text_color(action_title, lv_color_hex(0x7F96A8), 0);
    lv_obj_set_style_text_font(action_title, &lv_font_be_vietnam_18, 0);

    const char *action_names[] = {
        "RECOVERY",
        "DFU",
        "PURPLE",
        "REBOOT"
    };

    const int action_x[] = {
        28, 274, 520, 766
    };

    for (int i = 0; i < 4; ++i) {
        lv_obj_t *button = lv_btn_create(_screen);
        lv_obj_set_size(button, 220, 72);
        lv_obj_align(
            button,
            LV_ALIGN_TOP_LEFT,
            action_x[i],
            395
        );

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, action_names[i]);
        lv_obj_center(label);
        lv_obj_set_style_text_font(label, &lv_font_be_vietnam_16, 0);

        // RECOVERY works for iPhone and Android; FASTBOOT (DFU slot) and REBOOT act on
        // Android only. PURPLE remains an inert placeholder.
        if (i == 0) {
            _recovery_button_label = label;
            lv_obj_add_event_cb(button, onRecoveryClicked, LV_EVENT_CLICKED, this);
        } else if (i == 1) {
            _bootloader_button_label = label;
            lv_obj_add_event_cb(button, onBootloaderClicked, LV_EVENT_CLICKED, this);
        } else if (i == 2) {
            _purple_button_label = label;
        } else {
            _reboot_button_label = label;
            lv_obj_add_event_cb(button, onRebootClicked, LV_EVENT_CLICKED, this);
        }
    }
    _recovery_confirm_pending = false;
    _android_confirm_action = AndroidAction::None;
    HCCHome::updateActionLabels(this);

    /* HARDWARE CHECK (kiểm tra phần cứng iPhone qua phiên Lockdown) */
    lv_obj_t *hw_button = lv_btn_create(_screen);
    lv_obj_set_size(hw_button, 260, 52);
    lv_obj_align(hw_button, LV_ALIGN_BOTTOM_LEFT, 28, -22);
    lv_obj_set_style_bg_color(hw_button, lv_color_hex(0x2478D0), 0);
    lv_obj_add_event_cb(hw_button, onHardwareCheckClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *hw_label = lv_label_create(hw_button);
    lv_label_set_text(hw_label, "HARDWARE CHECK");
    lv_obj_center(hw_label);
    lv_obj_set_style_text_font(hw_label, &lv_font_be_vietnam_18, 0);

    /* Back */
    lv_obj_t *back_button = lv_btn_create(_screen);
    lv_obj_set_size(back_button, 160, 52);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_RIGHT, -28, -22);
    lv_obj_add_event_cb(back_button, onBackClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, "BACK");
    lv_obj_center(back_label);
    lv_obj_set_style_text_font(back_label, &lv_font_be_vietnam_18, 0);
}

void HCCHome::showI2C()
{
    if (_screen == nullptr) {
        return;
    }

    detachUsbLabels();

    lv_obj_clean(_screen);

    lv_obj_set_style_bg_color(
        _screen,
        lv_color_hex(0x101820),
        0
    );

    _scan_scanning = false;
    _scan_result[0] = '\0';
    snprintf(
        _scan_status,
        sizeof(_scan_status),
        "Ready to scan"
    );

    lv_obj_t *title = lv_label_create(_screen);
    lv_label_set_text(title, "I2C SCANNER");
    lv_obj_align(
        title,
        LV_ALIGN_TOP_LEFT,
        28,
        20
    );
    lv_obj_set_style_text_color(
        title,
        lv_color_hex(0xFFFFFF),
        0
    );
    lv_obj_set_style_text_font(
        title,
        &lv_font_be_vietnam_28,
        0
    );

    lv_obj_t *bus_info = lv_label_create(_screen);
    lv_label_set_text(
        bus_info,
        "SDA: GPIO 4    SCL: GPIO 5"
    );
    lv_obj_align(
        bus_info,
        LV_ALIGN_TOP_LEFT,
        28,
        62
    );
    lv_obj_set_style_text_color(
        bus_info,
        lv_color_hex(0xB8C5CF),
        0
    );
    lv_obj_set_style_text_font(
        bus_info,
        &lv_font_be_vietnam_18,
        0
    );

    _scan_button = lv_btn_create(_screen);
    lv_obj_set_size(
        _scan_button,
        180,
        60
    );
    lv_obj_align(
        _scan_button,
        LV_ALIGN_TOP_LEFT,
        28,
        105
    );

    lv_obj_add_event_cb(
        _scan_button,
        onScanClicked,
        LV_EVENT_CLICKED,
        this
    );

    lv_obj_t *scan_label = lv_label_create(_scan_button);
    lv_label_set_text(
        scan_label,
        "SCAN"
    );
    lv_obj_center(scan_label);
    lv_obj_set_style_text_font(
        scan_label,
        &lv_font_be_vietnam_18,
        0
    );

    _scan_status_label = lv_label_create(_screen);
    lv_label_set_text(
        _scan_status_label,
        _scan_status
    );
    lv_obj_align(
        _scan_status_label,
        LV_ALIGN_TOP_LEFT,
        240,
        120
    );
    lv_obj_set_style_text_color(
        _scan_status_label,
        lv_color_hex(0xB8C5CF),
        0
    );
    lv_obj_set_style_text_font(
        _scan_status_label,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *result_title = lv_label_create(_screen);
    lv_label_set_text(
        result_title,
        "Detected devices:"
    );
    lv_obj_align(
        result_title,
        LV_ALIGN_TOP_LEFT,
        28,
        190
    );
    lv_obj_set_style_text_color(
        result_title,
        lv_color_hex(0xFFFFFF),
        0
    );
    lv_obj_set_style_text_font(
        result_title,
        &lv_font_be_vietnam_18,
        0
    );

    _scan_result_label = lv_label_create(_screen);
    lv_label_set_text(
        _scan_result_label,
        "No scan performed"
    );
    lv_obj_align(
        _scan_result_label,
        LV_ALIGN_TOP_LEFT,
        28,
        230
    );
    lv_obj_set_style_text_color(
        _scan_result_label,
        lv_color_hex(0xB8C5CF),
        0
    );
    lv_obj_set_style_text_font(
        _scan_result_label,
        &lv_font_be_vietnam_18,
        0
    );

    lv_obj_t *back_button = lv_btn_create(_screen);
    lv_obj_set_size(
        back_button,
        180,
        60
    );
    lv_obj_align(
        back_button,
        LV_ALIGN_BOTTOM_LEFT,
        28,
        -25
    );

    lv_obj_add_event_cb(
        back_button,
        onBackClicked,
        LV_EVENT_CLICKED,
        this
    );

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(
        back_label,
        "BACK"
    );
    lv_obj_center(back_label);
    lv_obj_set_style_text_font(
        back_label,
        &lv_font_be_vietnam_18,
        0
    );
}
void HCCHome::onScanClicked(lv_event_t *event)
{
    HCCHome *app =
        static_cast<HCCHome *>(
            lv_event_get_user_data(event)
        );

    if (app == nullptr) {
        return;
    }

    if (app->_scan_scanning) {
        return;
    }

    app->_scan_scanning = true;

    snprintf(
        app->_scan_status,
        sizeof(app->_scan_status),
        "Scanning..."
    );

    if (app->_scan_status_label != nullptr) {
        lv_label_set_text(
            app->_scan_status_label,
            app->_scan_status
        );
    }

    if (app->_scan_result_label != nullptr) {
        lv_label_set_text(
            app->_scan_result_label,
            "Scanning I2C bus..."
        );
    }

    if (app->_scan_button != nullptr) {
        lv_obj_add_state(
            app->_scan_button,
            LV_STATE_DISABLED
        );
    }

    xTaskCreate(
        scanTask,
        "hcc_i2c_scan",
        4096,
        app,
        5,
        nullptr
    );
}


void HCCHome::scanTask(void *arg)
{
    HCCHome *app =
        static_cast<HCCHome *>(arg);

    if (app != nullptr) {
        app->performScan();

        lv_async_call(
            applyScanResult,
            app
        );
    }

    vTaskDelete(nullptr);
}


void HCCHome::applyScanResult(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);

    if (app == nullptr) {
        return;
    }

    if (app->_scan_status_label != nullptr) {
        lv_label_set_text(
            app->_scan_status_label,
            app->_scan_status
        );
    }

    if (app->_scan_result_label != nullptr) {
        lv_label_set_text(
            app->_scan_result_label,
            app->_scan_result
        );
    }

    app->_scan_scanning = false;

    if (app->_scan_button != nullptr) {
        lv_obj_clear_state(
            app->_scan_button,
            LV_STATE_DISABLED
        );
    }
} 
void HCCHome::performScan()
{
    i2c_master_bus_handle_t bus = _external_i2c_bus;
    if (bus == nullptr) {
        i2c_master_bus_config_t bus_config = {};

        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = GPIO_NUM_4;
        bus_config.scl_io_num = GPIO_NUM_5;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.trans_queue_depth = 0;
        bus_config.flags.enable_internal_pullup = true;

        esp_err_t ret = i2c_new_master_bus(
            &bus_config,
            &_external_i2c_bus
        );

        if (ret != ESP_OK) {
            snprintf(
                _scan_status,
                sizeof(_scan_status),
                "I2C init: %s",
                esp_err_to_name(ret)
            );
            return;
        }

        bus = _external_i2c_bus;
    }

    if (bus == nullptr) {
        snprintf(
            _scan_status,
            sizeof(_scan_status),
            "I2C bus unavailable"
        );
        return;
    }

    _scan_result[0] = '\0';

    esp_err_t reset_ret = i2c_master_bus_reset(bus);

    if (reset_ret != ESP_OK) {
        snprintf(
            _scan_status,
            sizeof(_scan_status),
            "I2C reset: %s",
            esp_err_to_name(reset_ret)
        );
        return;
    }

    snprintf(
        _scan_result,
        sizeof(_scan_result),
        "Testing 0x18..."
    );

    esp_err_t probe_ret =
        i2c_master_probe(
            bus,
            0x18,
            20
        );

    if (probe_ret == ESP_OK) {

        snprintf(
            _scan_result,
            sizeof(_scan_result),
            "0x18 ACK"
        );

        snprintf(
            _scan_status,
            sizeof(_scan_status),
            "I2C OK"
        );

    } else if (probe_ret == ESP_ERR_NOT_FOUND) {

        snprintf(
            _scan_result,
            sizeof(_scan_result),
            "0x18 NACK"
        );

        snprintf(
            _scan_status,
            sizeof(_scan_status),
            "Bus OK - device absent"
        );

    } else if (probe_ret == ESP_ERR_TIMEOUT) {

        snprintf(
            _scan_result,
            sizeof(_scan_result),
            "0x18 TIMEOUT"
        );

        snprintf(
            _scan_status,
            sizeof(_scan_status),
            "I2C BUS TIMEOUT"
        );

    } else {

        snprintf(
            _scan_result,
            sizeof(_scan_result),
            "%s",
            esp_err_to_name(probe_ret)
        );

        snprintf(
            _scan_status,
            sizeof(_scan_status),
            "I2C probe error"
        );
    }
}

// Lazily bring up the shared external I2C bus (same pins as the scanner).
i2c_master_bus_handle_t HCCHome::ensureI2CBus()
{
    if (_external_i2c_bus != nullptr) {
        return _external_i2c_bus;
    }

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = GPIO_NUM_4;
    bus_config.scl_io_num = GPIO_NUM_5;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.trans_queue_depth = 0;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t ret = i2c_new_master_bus(&bus_config, &_external_i2c_bus);
    if (ret != ESP_OK) {
        printf("[HCC EEPROM] I2C init failed: %s\n", esp_err_to_name(ret));
        _external_i2c_bus = nullptr;
    }
    return _external_i2c_bus;
}

void HCCHome::performEepromDump()
{
    i2c_master_bus_handle_t bus = ensureI2CBus();
    if (bus == nullptr) {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Lỗi khởi tạo I2C");
        _eeprom_result[0] = '\0';
        return;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "%s", _eeprom_file);
    esp_err_t ret = HCCEeprom::dumpToSd(bus, _eeprom_config, filename);

    if (ret == ESP_OK) {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Đã lưu thẻ SD");
        snprintf(_eeprom_result, sizeof(_eeprom_result),
                 "%u byte @0x%02X\n-> %s/%s",
                 static_cast<unsigned>(_eeprom_config.total_size),
                 _eeprom_config.device_address,
                 BSP_SD_MOUNT_POINT, filename);
    } else {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Thất bại");
        snprintf(_eeprom_result, sizeof(_eeprom_result), "%s", esp_err_to_name(ret));
    }
}

void HCCHome::performEepromProgram()
{
    i2c_master_bus_handle_t bus = ensureI2CBus();
    if (bus == nullptr) {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Lỗi khởi tạo I2C");
        _eeprom_result[0] = '\0';
        return;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "%s", _eeprom_file);
    esp_err_t ret = HCCEeprom::programFromSd(bus, _eeprom_config, filename);
    if (ret != ESP_OK) {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Ghi thất bại");
        snprintf(_eeprom_result, sizeof(_eeprom_result), "%s", esp_err_to_name(ret));
        return;
    }

    // Verify: đọc lại EEPROM và so với file vừa ghi.
    size_t mismatch = 0;
    esp_err_t vret = HCCEeprom::verifyAgainstSd(bus, _eeprom_config, filename, &mismatch);
    if (vret == ESP_OK) {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Ghi & verify OK");
        snprintf(_eeprom_result, sizeof(_eeprom_result),
                 "Ghi từ %s/%s @0x%02X, verify khớp",
                 BSP_SD_MOUNT_POINT, filename, _eeprom_config.device_address);
    } else if (vret == ESP_ERR_INVALID_CRC) {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Verify LỆCH");
        snprintf(_eeprom_result, sizeof(_eeprom_result),
                 "Đã ghi nhưng verify lệch tại offset 0x%04X",
                 static_cast<unsigned>(mismatch));
    } else {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Verify lỗi");
        snprintf(_eeprom_result, sizeof(_eeprom_result),
                 "Đã ghi, verify lỗi: %s", esp_err_to_name(vret));
    }
}

void HCCHome::eepromTask(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app != nullptr) {
        app->performEepromDump();
        lv_async_call(applyEepromResult, app);
    }
    vTaskDelete(nullptr);
}

void HCCHome::eepromProgramTask(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app != nullptr) {
        app->performEepromProgram();
        lv_async_call(applyEepromResult, app);
    }
    vTaskDelete(nullptr);
}

void HCCHome::applyEepromResult(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app == nullptr) {
        return;
    }

    if (app->_eeprom_status_label != nullptr) {
        lv_label_set_text(app->_eeprom_status_label, app->_eeprom_status);
    }
    if (app->_eeprom_result_label != nullptr) {
        lv_label_set_text(app->_eeprom_result_label, app->_eeprom_result);
    }
    if (app->_eeprom_button != nullptr) {
        lv_obj_clear_state(app->_eeprom_button, LV_STATE_DISABLED);
    }
    if (app->_eeprom_program_button != nullptr) {
        lv_obj_clear_state(app->_eeprom_program_button, LV_STATE_DISABLED);
    }
    app->_eeprom_busy = false;
}

void HCCHome::populateEepromFileList()
{
    if (_eeprom_file_dropdown == nullptr) {
        return;
    }

    char list[512];
    int count = HCCEeprom::listFiles(list, sizeof(list), ".bin");
    if (count <= 0) {
        // Chưa có file .bin (hoặc lỗi thẻ): giữ tên mặc định để ĐỌC có thể tạo file mới.
        snprintf(list, sizeof(list), "eeprom_dump.bin");
    }

    lv_dropdown_set_options(_eeprom_file_dropdown, list);
    // set_options đưa lựa chọn về mục đầu: đồng bộ _eeprom_file theo mục đang chọn.
    lv_dropdown_get_selected_str(_eeprom_file_dropdown, _eeprom_file, sizeof(_eeprom_file));
}

void HCCHome::onEepromFileChanged(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_eeprom_file_dropdown == nullptr) {
        return;
    }
    lv_dropdown_get_selected_str(app->_eeprom_file_dropdown,
                                 app->_eeprom_file, sizeof(app->_eeprom_file));
    // Đổi file thì hủy xác nhận ghi đang chờ.
    app->_eeprom_program_confirm_pending = false;
}

void HCCHome::onEepromRefreshClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_eeprom_busy) {
        return;
    }
    app->populateEepromFileList();
    snprintf(app->_eeprom_status, sizeof(app->_eeprom_status), "Đã làm mới danh sách");
    if (app->_eeprom_status_label != nullptr) {
        lv_label_set_text(app->_eeprom_status_label, app->_eeprom_status);
    }
}

void HCCHome::onEepromViewClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_eeprom_busy || app->_eeprom_file[0] == '\0') {
        return;
    }
    app->beginEepromOp("Đang đọc file...");
    xTaskCreate(eepromViewTask, "hcc_eeprom_vw", 6144, app, 5, nullptr);
}

void HCCHome::onEepromDialogCloseClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_eeprom_dialog == nullptr) {
        return;
    }
    lv_obj_del(app->_eeprom_dialog);
    app->_eeprom_dialog = nullptr;
}

void HCCHome::performEepromView()
{
    free(_eeprom_view_buf);
    _eeprom_view_buf = nullptr;
    _eeprom_view_len = 0;

    uint8_t *buf = static_cast<uint8_t *>(malloc(_eeprom_config.total_size));
    if (buf == nullptr) {
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Hết bộ nhớ");
        snprintf(_eeprom_result, sizeof(_eeprom_result),
                 "Không cấp phát được %u byte",
                 static_cast<unsigned>(_eeprom_config.total_size));
        return;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "%s", _eeprom_file);
    size_t len = 0;
    esp_err_t ret = HCCEeprom::loadBufferFromSd(filename, buf,
                                                _eeprom_config.total_size, &len);
    if (ret != ESP_OK || len == 0) {
        free(buf);
        snprintf(_eeprom_status, sizeof(_eeprom_status), "Đọc file lỗi");
        snprintf(_eeprom_result, sizeof(_eeprom_result), "%s: %s", filename,
                 (ret == ESP_OK) ? "file rỗng" : esp_err_to_name(ret));
        return;
    }

    _eeprom_view_buf = buf;
    _eeprom_view_len = len;
    snprintf(_eeprom_status, sizeof(_eeprom_status), "Đã nạp %u byte",
             static_cast<unsigned>(len));
    snprintf(_eeprom_result, sizeof(_eeprom_result), "Xem HEX: %s (%u byte)",
             filename, static_cast<unsigned>(len));
}

void HCCHome::eepromViewTask(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app != nullptr) {
        app->performEepromView();
        lv_async_call(applyEepromView, app);
    }
    vTaskDelete(nullptr);
}

void HCCHome::applyEepromView(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app == nullptr) {
        return;
    }

    if (app->_eeprom_status_label != nullptr) {
        lv_label_set_text(app->_eeprom_status_label, app->_eeprom_status);
    }
    if (app->_eeprom_result_label != nullptr) {
        lv_label_set_text(app->_eeprom_result_label, app->_eeprom_result);
    }
    if (app->_eeprom_button != nullptr) {
        lv_obj_clear_state(app->_eeprom_button, LV_STATE_DISABLED);
    }
    if (app->_eeprom_program_button != nullptr) {
        lv_obj_clear_state(app->_eeprom_program_button, LV_STATE_DISABLED);
    }
    app->_eeprom_busy = false;

    // Chỉ mở hộp thoại nếu vẫn ở màn hình EEPROM và có dữ liệu.
    if (app->_eeprom_button != nullptr && app->_eeprom_view_buf != nullptr) {
        app->showBinaryDialog(app->_eeprom_view_buf, app->_eeprom_view_len);
    }
    free(app->_eeprom_view_buf);
    app->_eeprom_view_buf = nullptr;
    app->_eeprom_view_len = 0;
}

void HCCHome::showBinaryDialog(const uint8_t *data, size_t len)
{
    if (_screen == nullptr || data == nullptr || len == 0) {
        return;
    }
    if (_eeprom_dialog != nullptr) {
        lv_obj_del(_eeprom_dialog);
        _eeprom_dialog = nullptr;
    }

    /* Lớp phủ mờ toàn màn hình */
    _eeprom_dialog = lv_obj_create(_screen);
    lv_obj_set_size(_eeprom_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_eeprom_dialog);
    lv_obj_set_style_bg_color(_eeprom_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_eeprom_dialog, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_eeprom_dialog, 0, 0);
    lv_obj_set_style_radius(_eeprom_dialog, 0, 0);
    lv_obj_clear_flag(_eeprom_dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* Bảng nội dung */
    lv_obj_t *panel = lv_obj_create(_eeprom_dialog);
    lv_obj_set_size(panel, 880, 540);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B141C), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    char title_text[96];
    snprintf(title_text, sizeof(title_text), "HEX: %s (%u byte)",
             _eeprom_file, static_cast<unsigned>(len));
    lv_obj_t *dlg_title = lv_label_create(panel);
    lv_label_set_text(dlg_title, title_text);
    lv_obj_align(dlg_title, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_text_color(dlg_title, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(dlg_title, &lv_font_be_vietnam_18, 0);

    lv_obj_t *close_button = lv_btn_create(panel);
    lv_obj_set_size(close_button, 110, 42);
    lv_obj_align(close_button, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_add_event_cb(close_button, onEepromDialogCloseClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "ĐÓNG");
    lv_obj_center(close_label);
    lv_obj_set_style_text_font(close_label, &lv_font_be_vietnam_16, 0);

    /* Vùng cuộn chứa hex dump */
    lv_obj_t *content = lv_obj_create(panel);
    lv_obj_set_size(content, 840, 450);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x05090D), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 6, 0);

    lv_obj_t *hex_label = lv_label_create(content);
    lv_obj_set_style_text_color(hex_label, lv_color_hex(0xCFE3EF), 0);
    lv_obj_set_style_text_font(hex_label, &lv_font_be_vietnam_16, 0);

    const size_t bytes_per_line = 16;
    const size_t lines = (len + bytes_per_line - 1) / bytes_per_line;
    const size_t cap = lines * 80 + 64;
    char *text = static_cast<char *>(malloc(cap));
    if (text == nullptr) {
        lv_label_set_text(hex_label, "Hết bộ nhớ khi dựng hex");
        return;
    }

    size_t p = 0;
    for (size_t off = 0; off < len && p < cap - 1; off += bytes_per_line) {
        p += snprintf(text + p, cap - p, "%04X  ", static_cast<unsigned>(off));
        const size_t n = (len - off < bytes_per_line) ? (len - off) : bytes_per_line;
        for (size_t i = 0; i < bytes_per_line; ++i) {
            if (i < n) {
                p += snprintf(text + p, cap - p, "%02X ", data[off + i]);
            } else {
                p += snprintf(text + p, cap - p, "   ");
            }
        }
        p += snprintf(text + p, cap - p, " |");
        for (size_t i = 0; i < n && p < cap - 1; ++i) {
            const uint8_t c = data[off + i];
            text[p++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
        }
        p += snprintf(text + p, cap - p, "|\n");
    }
    text[(p < cap) ? p : cap - 1] = '\0';
    lv_label_set_text(hex_label, text);
    free(text);
}

void HCCHome::showEeprom()
{
    if (_screen == nullptr) {
        return;
    }

    _usb_status_label = nullptr;
    _usb_info_label = nullptr;
    _usb_mode_label = nullptr;
    _recovery_button_label = nullptr;
    _bootloader_button_label = nullptr;
    _purple_button_label = nullptr;
    _reboot_button_label = nullptr;
    _eeprom_status_label = nullptr;
    _eeprom_result_label = nullptr;
    _eeprom_button = nullptr;
    _eeprom_program_button = nullptr;
    _eeprom_file_dropdown = nullptr;
    _eeprom_dialog = nullptr;

    lv_obj_clean(_screen);
    lv_obj_set_style_bg_color(_screen, lv_color_hex(0x101820), 0);

    _eeprom_busy = false;
    _eeprom_program_confirm_pending = false;
    snprintf(_eeprom_status, sizeof(_eeprom_status), "Sẵn sàng");
    _eeprom_result[0] = '\0';

    /* Header */
    lv_obj_t *title = lv_label_create(_screen);
    lv_label_set_text(title, "EEPROM");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 20);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_be_vietnam_28, 0);

    char cfg_text[96];
    snprintf(cfg_text, sizeof(cfg_text),
             "Addr 0x%02X   %u byte   addr %u byte   SDA4/SCL5",
             _eeprom_config.device_address,
             static_cast<unsigned>(_eeprom_config.total_size),
             _eeprom_config.address_width);
    lv_obj_t *cfg = lv_label_create(_screen);
    lv_label_set_text(cfg, cfg_text);
    lv_obj_align(cfg, LV_ALIGN_TOP_LEFT, 28, 62);
    lv_obj_set_style_text_color(cfg, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(cfg, &lv_font_be_vietnam_18, 0);

    /* File selector row: nhãn + dropdown chọn file + nút làm mới danh sách */
    lv_obj_t *file_title = lv_label_create(_screen);
    lv_label_set_text(file_title, "File:");
    lv_obj_align(file_title, LV_ALIGN_TOP_LEFT, 28, 118);
    lv_obj_set_style_text_color(file_title, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(file_title, &lv_font_be_vietnam_16, 0);

    _eeprom_file_dropdown = lv_dropdown_create(_screen);
    lv_obj_set_size(_eeprom_file_dropdown, 300, 40);
    lv_obj_align(_eeprom_file_dropdown, LV_ALIGN_TOP_LEFT, 86, 110);
    lv_obj_set_style_text_font(_eeprom_file_dropdown, &lv_font_be_vietnam_16, 0);
    lv_obj_add_event_cb(_eeprom_file_dropdown, onEepromFileChanged, LV_EVENT_VALUE_CHANGED, this);
    populateEepromFileList();

    lv_obj_t *refresh_button = lv_btn_create(_screen);
    lv_obj_set_size(refresh_button, 110, 40);
    lv_obj_align(refresh_button, LV_ALIGN_TOP_LEFT, 398, 110);
    lv_obj_add_event_cb(refresh_button, onEepromRefreshClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *refresh_label = lv_label_create(refresh_button);
    lv_label_set_text(refresh_label, "LÀM MỚI");
    lv_obj_center(refresh_label);
    lv_obj_set_style_text_font(refresh_label, &lv_font_be_vietnam_16, 0);

    /* Hàng nút thao tác (nhỏ gọn) */
    const lv_coord_t btn_w = 168;
    const lv_coord_t btn_h = 46;
    const lv_coord_t btn_y = 164;

    /* ĐỌC EEPROM -> file trên SD */
    _eeprom_button = lv_btn_create(_screen);
    lv_obj_set_size(_eeprom_button, btn_w, btn_h);
    lv_obj_align(_eeprom_button, LV_ALIGN_TOP_LEFT, 28, btn_y);
    lv_obj_add_event_cb(_eeprom_button, onEepromDumpClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *dump_label = lv_label_create(_eeprom_button);
    lv_label_set_text(dump_label, "ĐỌC -> SD");
    lv_obj_center(dump_label);
    lv_obj_set_style_text_font(dump_label, &lv_font_be_vietnam_16, 0);

    /* GHI file trên SD -> EEPROM */
    _eeprom_program_button = lv_btn_create(_screen);
    lv_obj_set_size(_eeprom_program_button, btn_w, btn_h);
    lv_obj_align(_eeprom_program_button, LV_ALIGN_TOP_LEFT, 208, btn_y);
    lv_obj_add_event_cb(_eeprom_program_button, onEepromProgramClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *program_label = lv_label_create(_eeprom_program_button);
    lv_label_set_text(program_label, "GHI <- SD");
    lv_obj_center(program_label);
    lv_obj_set_style_text_font(program_label, &lv_font_be_vietnam_16, 0);

    /* XEM HEX: mở hộp thoại hiển thị binary của file đang chọn */
    lv_obj_t *view_button = lv_btn_create(_screen);
    lv_obj_set_size(view_button, btn_w, btn_h);
    lv_obj_align(view_button, LV_ALIGN_TOP_LEFT, 388, btn_y);
    lv_obj_add_event_cb(view_button, onEepromViewClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *view_label = lv_label_create(view_button);
    lv_label_set_text(view_label, "XEM HEX");
    lv_obj_center(view_label);
    lv_obj_set_style_text_font(view_label, &lv_font_be_vietnam_16, 0);

    _eeprom_status_label = lv_label_create(_screen);
    lv_label_set_text(_eeprom_status_label, _eeprom_status);
    lv_obj_align(_eeprom_status_label, LV_ALIGN_TOP_LEFT, 572, btn_y + 12);
    lv_obj_set_style_text_color(_eeprom_status_label, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(_eeprom_status_label, &lv_font_be_vietnam_16, 0);

    lv_obj_t *result_title = lv_label_create(_screen);
    lv_label_set_text(result_title, "Kết quả:");
    lv_obj_align(result_title, LV_ALIGN_TOP_LEFT, 28, 232);
    lv_obj_set_style_text_color(result_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(result_title, &lv_font_be_vietnam_16, 0);

    _eeprom_result_label = lv_label_create(_screen);
    lv_label_set_text(_eeprom_result_label, "Chưa có thao tác");
    lv_obj_align(_eeprom_result_label, LV_ALIGN_TOP_LEFT, 28, 264);
    lv_obj_set_width(_eeprom_result_label, 900);
    lv_obj_set_style_text_color(_eeprom_result_label, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(_eeprom_result_label, &lv_font_be_vietnam_16, 0);

    /* Back */
    lv_obj_t *back_button = lv_btn_create(_screen);
    lv_obj_set_size(back_button, 130, 46);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_LEFT, 28, -25);
    lv_obj_add_event_cb(back_button, onBackClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, "BACK");
    lv_obj_center(back_label);
    lv_obj_set_style_text_font(back_label, &lv_font_be_vietnam_16, 0);
}

// ==================== PANIC & AI ANALYZE ====================

void HCCHome::onPanicClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }
    app->showPanic();
}

void HCCHome::onPanicFileChanged(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_panic_file_dropdown == nullptr) {
        return;
    }
    lv_dropdown_get_selected_str(app->_panic_file_dropdown, app->_panic_file, sizeof(app->_panic_file));
    app->refreshPanicDetail();
}

void HCCHome::onPanicListItemClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_pi_list == nullptr) {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(event);
    const char *key = static_cast<const char *>(lv_obj_get_user_data(btn));
    if (key && key[0]) {
        snprintf(app->_panic_file, sizeof(app->_panic_file), "%s", key);
        app->refreshPanicDetail();
    }
}

void HCCHome::onPanicListItemDeleted(lv_event_t *event)
{
    lv_obj_t *btn = lv_event_get_target(event);
    if (btn) {
        void *key = lv_obj_get_user_data(btn);
        if (key) {
            free(key);
            lv_obj_set_user_data(btn, nullptr);
        }
    }
}

void HCCHome::updatePanicUrlLabel()
{
    if (_panic_url_label == nullptr) {
        return;
    }
    char host[64], ip[64], txt[160];
    HCCPanicServer::hostUrl(host, sizeof(host));
    _panic_url_has_ip = HCCPanicServer::deviceUrl(ip, sizeof(ip));
    if (_panic_url_has_ip) {
        snprintf(txt, sizeof(txt), "Gửi panic: %s  (hoặc %s)", host, ip);
    } else {
        snprintf(txt, sizeof(txt), "Gửi panic: %s  (WiFi chưa kết nối)", host);
    }
    lv_label_set_text(_panic_url_label, txt);
}

void HCCHome::onPanicPollTimer(lv_timer_t *timer)
{
    HCCHome *app = static_cast<HCCHome *>(timer->user_data);
    if (app == nullptr || app->_pi_detail == nullptr) {
        return;
    }
    // Wi-Fi vừa có / mất IP: cập nhật địa chỉ gửi panic.
    char ip[64];
    if (HCCPanicServer::deviceUrl(ip, sizeof(ip)) != app->_panic_url_has_ip) {
        app->updatePanicUrlLabel();
    }
    // Panic mới từ iPhone: làm mới danh sách và chọn nó (mục [RAM] đứng đầu).
    // Đang ANALYZE thì chờ xong mới làm mới, để kết quả không bị gán nhầm file.
    const uint32_t serial = HCCPanicServer::receivedSerial();
    if (serial == app->_panic_seen_serial || app->_panic_busy) {
        return;
    }
    app->_panic_seen_serial = serial;
    app->_panic_result = PanicResult();
    app->populatePanicFileList();
    if (app->_panic_status_label) {
        lv_label_set_text(app->_panic_status_label, "Đã nhận panic mới");
    }
}

void HCCHome::onPanicListDeleted(lv_event_t *event)
{
    // Vùng kết quả bị xoá = rời màn PANIC: dừng timer.
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }
    if (app->_panic_poll_timer) {
        lv_timer_del(app->_panic_poll_timer);
        app->_panic_poll_timer = nullptr;
    }
    app->_pi_list = nullptr;
    app->_pi_detail = nullptr;
}

void HCCHome::onPanicAnalyzeClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_panic_busy) {
        return;
    }
    if (app->_panic_file[0] == '\0' || app->_panic_file[0] == '(') {
        if (app->_panic_status_label != nullptr) {
            lv_label_set_text(app->_panic_status_label, "Chưa chọn file panic");
        }
        return;
    }

    app->_panic_busy = true;
    snprintf(app->_panic_status, sizeof(app->_panic_status), "Đang phân tích...");
    if (app->_panic_status_label != nullptr) {
        lv_label_set_text(app->_panic_status_label, app->_panic_status);
    }
    if (app->_panic_analyze_button != nullptr) {
        lv_obj_add_state(app->_panic_analyze_button, LV_STATE_DISABLED);
    }
    // Task lớn: đọc SD + parse + (KB / TLS gọi AI) + cJSON.
    xTaskCreate(panicAnalyzeTask, "hcc_panic", 16384, app, 4, nullptr);
}

void HCCHome::panicAnalyzeTask(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app != nullptr) {
        hccMemoryReport("ANALYZE bắt đầu");
        app->performPanicAnalyze();
        hccMemoryReport("ANALYZE xong");
        lv_async_call(applyPanicResult, app);
    }
    vTaskDelete(nullptr);
}

void HCCHome::performPanicAnalyze()
{
    // ẢNH panic (RAM): AI chỉ chép lại chữ log trong ảnh, rồi phân tích chữ đó
    // như file .ips (parser -> KB -> AI) bên dưới.
    std::string img_mime;
    bool is_img = (strncmp(_panic_file, "[RAM] ", 6) == 0) &&
                  HCCPanicServer::receivedIsImage(&img_mime);
    std::string raw;
    if (is_img) {
        std::string name, bytes;
        if (!HCCPanicServer::getReceived(name, bytes) || bytes.empty()) {
            _panic_result = PanicResult();
            _panic_result.ok = false;
            _panic_result.message = "Không lấy được ảnh panic";
            _kv_ai = _panic_result.message;
            snprintf(_panic_status, sizeof(_panic_status), "Lỗi đọc ảnh");
            return;
        }
        std::string err;
        if (!HCCPanicAI::transcribeImage(bytes, img_mime, raw, err)) {
            _panic_result = PanicResult();
            _panic_result.ok = false;
            _panic_result.message = err;
            _kv_ai = "#FF6B6B AI đọc ảnh lỗi:# " + HCCKB::noRecolor(err);
            snprintf(_panic_status, sizeof(_panic_status), "AI đọc ảnh lỗi");
            return;
        }
    } else if (!loadSelectedRaw(raw)) {
        _panic_result = PanicResult();
        _panic_result.ok = false;
        _panic_result.message = "Không đọc được panic đang chọn";
        _kv_ai = _panic_result.message;
        snprintf(_panic_status, sizeof(_panic_status), "Lỗi đọc panic");
        return;
    }

    ParsedPanic parsed = HCCPanicParser::parse(raw);
    if (is_img) {
        // Ảnh: model/iOS chỉ biết sau khi AI chép chữ; applyPanicResult hiện lên ô thiết bị.
        _panic_img_parsed = parsed;
        _panic_img_parsed_ready = true;
        _panic_img_parsed_file = _panic_file;
    }

    // 1) KB V1: chuỗi phân tích + đối chiếu case (VERIFIED cùng model + signature ưu tiên cao nhất).
    //    Case cũ (kb/verified, kb/repair_confirmed) đã được migrate khi nạp KB.
    PanicResult cr;
    if (computeKbView(parsed, &cr)) {
        _panic_result = cr;
        _kv_ai = "#8A97A3 Không gọi AI: đã có case " + cr.source_tier + " khớp (độ tin cậy " + cr.confidence + ").#";
        snprintf(_panic_status, sizeof(_panic_status), "Khớp case %s%s", cr.source_tier.c_str(),
                 is_img ? " (từ ảnh)" : "");
        return;
    }

    // 2) Chưa có case đủ tin cậy -> AI (module AI không đổi), hiện riêng ở ô AI.
    PanicResult ai = HCCPanicAI::analyze(parsed);
    _panic_result = ai;
    if (ai.ok) {
        // Lưu cache vào tầng AI_INFERENCE (không bao giờ tự thành VERIFIED).
        HCCPanicKB::saveAiResult(parsed, ai);
        _kv_ai = "#B48CFF [AI_INFERENCE - chưa kiểm chứng]#\nLỖI: " + HCCKB::noRecolor(ai.error) + "\n\n" +
                 HCCKB::noRecolor(ai.cause) + "\n\nKIỂM TRA:\n" + HCCKB::noRecolor(ai.repair) +
                 "\n\nAI tự đánh giá: " + HCCKB::noRecolor(ai.confidence);
        snprintf(_panic_status, sizeof(_panic_status), is_img ? "Phân tích AI xong (từ ảnh)" : "Phân tích AI xong");
    } else {
        _kv_ai = "#FF6B6B AI lỗi:# " + HCCKB::noRecolor(ai.message);
        snprintf(_panic_status, sizeof(_panic_status), "KB xong; AI lỗi");
    }
}

void HCCHome::applyPanicResult(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app == nullptr) {
        return;
    }

    if (app->_panic_status_label != nullptr) {
        lv_label_set_text(app->_panic_status_label, app->_panic_status);
    }

    if (app->_panic_img_parsed_ready) {
        app->_panic_img_parsed_ready = false;
        app->showPanicDeviceInfo(app->_panic_img_parsed);
    }
    // KB V1 (chuỗi phân tích, đo kiểm, đối chiếu case, kết luận) + ô AI riêng.
    app->showKbView();

    if (app->_panic_analyze_button != nullptr) {
        lv_obj_clear_state(app->_panic_analyze_button, LV_STATE_DISABLED);
    }
    app->_panic_busy = false;
}

static std::string dateFromName(const std::string &n);  // định nghĩa bên dưới

void HCCHome::populatePanicFileList()
{
    if (_panic_file_dropdown == nullptr) {
        return;
    }
    std::vector<std::string> items;
    std::string rname, rraw;
    if (HCCPanicServer::getReceived(rname, rraw)) {
        items.push_back("[RAM] " + rname);
    }
    std::vector<std::string> files = HCCPanicStorage::listDir(HCCPanicServer::rawDir(), nullptr);
    for (const auto &f : files) items.push_back(f);

    // Dropdown chọn nhanh.
    std::string opts;
    if (items.empty()) {
        opts = "(chưa có panic)";
    } else {
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) opts += "\n";
            opts += items[i];
        }
    }
    lv_dropdown_set_options(_panic_file_dropdown, opts.c_str());
    lv_dropdown_get_selected_str(_panic_file_dropdown, _panic_file, sizeof(_panic_file));

    // Danh sách bên trái: mỗi mục 2 dòng (tên + model/ngày), key lưu ở user_data.
    if (_pi_list) {
        lv_obj_clean(_pi_list);
        for (const auto &it : items) {
            std::string model, raw;
            if (it.rfind("[RAM] ", 0) == 0) {
                std::string n, r;
                if (HCCPanicServer::getReceived(n, r)) raw = r;
            } else {
                HCCPanicStorage::readFile(std::string(HCCPanicServer::rawDir()) + "/" + it, raw);
            }
            if (!raw.empty()) {
                model = HCCPanicParser::parse(raw).model;
                const std::string mn = panicMarketingName(model);
                if (!mn.empty()) model = mn;
            }
            std::string date = dateFromName(it);
            std::string sub = model;
            if (!date.empty()) sub += (sub.empty() ? "" : "   ") + date;

            lv_obj_t *b = lv_btn_create(_pi_list);
            lv_obj_set_width(b, lv_pct(100));
            lv_obj_set_height(b, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x18293A), 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x1E5AA8), LV_STATE_PRESSED);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_set_style_pad_all(b, 8, 0);

            lv_obj_t *l = lv_label_create(b);
            lv_label_set_recolor(l, true);
            lv_obj_set_width(l, lv_pct(100));
            std::string txt = it;
            if (!sub.empty()) txt += "\n#7F96A8 " + sub + "#";
            lv_label_set_text(l, txt.c_str());
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(l, &lv_font_be_vietnam_16, 0);

            char *keycopy = strdup(it.c_str());
            lv_obj_set_user_data(b, keycopy);
            lv_obj_add_event_cb(b, onPanicListItemClicked, LV_EVENT_CLICKED, this);
            lv_obj_add_event_cb(b, onPanicListItemDeleted, LV_EVENT_DELETE, this);
        }
    }
    if (_pi_list_count) {
        char c[16];
        snprintf(c, sizeof(c), "(%u)", (unsigned)items.size());
        lv_label_set_text(_pi_list_count, c);
    }
    refreshPanicDetail();
}

bool HCCHome::loadSelectedRaw(std::string &raw)
{
    if (_panic_file[0] == '\0' || _panic_file[0] == '(') {
        return false;
    }
    const char *ram_prefix = "[RAM] ";
    if (strncmp(_panic_file, ram_prefix, strlen(ram_prefix)) == 0) {
        std::string name;
        if (!HCCPanicServer::getReceived(name, raw)) return false;
        return !raw.empty();
    }
    std::string rel = std::string(HCCPanicServer::rawDir()) + "/" + _panic_file;
    return HCCPanicStorage::readFile(rel, raw) && !raw.empty();
}

// Tên thương mại từ mã model:
//  1) panic/models.txt trên thẻ SD (kỹ thuật viên tự thêm máy mới), mỗi dòng
//     "iPhone19,7=iPhone 18 Pro Max";
//  2) bảng có sẵn (chỉ mapping chắc chắn);
//  3) mã mới hơn bảng: chỉ nêu dòng máy, vd iPhone19,x -> "iPhone 18 series".
// "" nếu không biết.
static std::string panicMarketingName(const std::string &code)
{
    if (code.empty()) return std::string();
    std::string user;
    if (HCCPanicStorage::readFile("panic/models.txt", user)) {
        size_t start = 0;
        while (start < user.size()) {
            size_t nl = user.find('\n', start);
            std::string line = user.substr(start, (nl == std::string::npos ? user.size() : nl) - start);
            start = (nl == std::string::npos) ? user.size() : nl + 1;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            auto trim = [](std::string &x) {
                while (!x.empty() && isspace((unsigned char)x.back())) x.pop_back();
                while (!x.empty() && isspace((unsigned char)x.front())) x.erase(0, 1);
            };
            trim(k);
            trim(v);
            if (k == code && !v.empty()) return v;
        }
    }
    int newest = 0;
    for (const auto &e : kIPhoneModelNames) {
        if (code == e.product_type) return e.name;
        newest = std::max(newest, atoi(e.product_type + 6));
    }
    // iPhoneN,M với N lớn hơn mọi mã trong bảng: từ iPhone 12 trở đi dòng máy = N - 1.
    if (code.rfind("iPhone", 0) == 0) {
        int major = atoi(code.c_str() + 6);
        if (major > newest) return "iPhone " + std::to_string(major - 1) + " series";
    }
    return std::string();
}

// SoC suy từ họ model (A13..A19); nullptr nếu không rõ.
static const char *panicSocForModel(const std::string &code)
{
    if (code.rfind("iPhone", 0) != 0) return nullptr;
    const char *p = code.c_str() + 6;
    int major = atoi(p);
    const char *comma = strchr(p, ',');
    int minor = comma ? atoi(comma + 1) : 0;
    switch (major) {
        case 8: return "A9";
        case 9: return "A10 Fusion";
        case 10: return "A11 Bionic";
        case 11: return "A12 Bionic";
        case 12: return "A13";
        case 13: return "A14";
        case 14: return "A15";
        case 15: return "A16";
        case 16: return "A17 Pro";
        case 17: return (minor <= 2) ? "A18 Pro" : "A18";
        case 18: return (minor == 3 || minor == 5) ? "A19" : "A19 Pro";  // 18,4 = iPhone Air (A19 Pro)
        case 19: return "A20 Pro";  // iPhone 18 Pro / Pro Max
        default: return nullptr;
    }
}

// ---- Card helpers (layout giống mockup) ----
static lv_obj_t *mkCard(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x142433), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x2A3E52), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_pad_row(c, 5, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void mkCardTitle(lv_obj_t *card, const char *sym, const char *text, lv_color_t col)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (sym && sym[0]) {
        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, sym);
        lv_obj_set_style_text_color(ic, col, 0);
    }
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_color(t, col, 0);
    lv_obj_set_style_text_font(t, &lv_font_be_vietnam_16, 0);
}

static lv_obj_t *mkVal(lv_obj_t *card, const lv_font_t *f, lv_color_t col)
{
    lv_obj_t *v = lv_label_create(card);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_color(v, col, 0);
    lv_obj_set_style_text_font(v, f, 0);
    return v;
}

static lv_obj_t *mkRow(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 10, 0);
    return r;
}

// "20260926_174030.ips" -> "2026-09-26 17:40"
static std::string dateFromName(const std::string &n)
{
    std::string s = n;
    if (s.rfind("[RAM] ", 0) == 0) s = s.substr(6);
    size_t i = 0;
    while (i < s.size() && !isdigit((unsigned char)s[i])) i++;
    std::string d = s.substr(i);
    if (d.size() < 15 || d[8] != '_') return std::string();
    char buf[24];
    snprintf(buf, sizeof(buf), "%.4s-%.2s-%.2s %.2s:%.2s",
             d.c_str(), d.c_str() + 4, d.c_str() + 6, d.c_str() + 9, d.c_str() + 11);
    return buf;
}

void HCCHome::showPanic()
{
    if (_screen == nullptr) {
        return;
    }

    detachUsbLabels();
    _panic_file_dropdown = nullptr;
    _panic_status_label = nullptr;
    _panic_url_label = nullptr;
    _panic_analyze_button = nullptr;
    _panic_error_label = nullptr;
    _panic_cause_label = nullptr;
    _panic_repair_label = nullptr;
    _panic_conf_label = nullptr;
    _panic_tier_label = nullptr;
    _panic_apikey_dialog = nullptr;
    _panic_apikey_input = nullptr;
    _panic_keyboard = nullptr;
    _panic_view_dialog = nullptr;
    _panic_detail_dialog = nullptr;
    _pi_conf = nullptr;
    _repair_dialog = nullptr;
    _repair_kb = nullptr;
    _repair_model = nullptr;
    _repair_sig = nullptr;
    _repair_action = nullptr;
    _repair_note = nullptr;
    _pi_list = nullptr;
    _pi_list_count = nullptr;
    _pi_model = nullptr;
    _pi_ios = nullptr;
    _pi_soc = nullptr;
    _pi_file = nullptr;
    _pi_sev = nullptr;
    _pi_sevsub = nullptr;
    _pi_keystr = nullptr;
    _pi_cause = nullptr;
    _pi_check = nullptr;
    _pi_kb = nullptr;
    _pi_conclusion = nullptr;
    _pi_ai = nullptr;
    _pi_detail = nullptr;
    _panic_busy = false;

    lv_obj_clean(_screen);
    lv_obj_set_style_bg_color(_screen, lv_color_hex(0x0B1420), 0);
    HCCPanicServer::start();
    HCCPanicStorage::ensureMounted();

    auto styleBtn = [](lv_obj_t *b, const char *txt, lv_event_cb_t cb, void *u,
                       lv_color_t bg, const lv_font_t *f) {
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, u);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_center(l);
        lv_obj_set_style_text_font(l, f, 0);
    };

    /* ===== Toolbar ===== */
    lv_obj_t *ftitle = lv_label_create(_screen);
    lv_label_set_text(ftitle, "File panic:");
    lv_obj_align(ftitle, LV_ALIGN_TOP_LEFT, 24, 26);
    lv_obj_set_style_text_color(ftitle, lv_color_hex(0xB8C5CF), 0);
    lv_obj_set_style_text_font(ftitle, &lv_font_be_vietnam_16, 0);

    _panic_file_dropdown = lv_dropdown_create(_screen);
    lv_obj_set_size(_panic_file_dropdown, 570, 46);
    lv_obj_align(_panic_file_dropdown, LV_ALIGN_TOP_LEFT, 128, 14);
    lv_obj_set_style_text_font(_panic_file_dropdown, &lv_font_be_vietnam_16, 0);
    lv_obj_add_event_cb(_panic_file_dropdown, onPanicFileChanged, LV_EVENT_VALUE_CHANGED, this);

    _panic_analyze_button = lv_btn_create(_screen);
    lv_obj_set_size(_panic_analyze_button, 138, 46);
    lv_obj_align(_panic_analyze_button, LV_ALIGN_TOP_LEFT, 712, 14);
    styleBtn(_panic_analyze_button, "ANALYZE", onPanicAnalyzeClicked, this, lv_color_hex(0x2478D0), &lv_font_be_vietnam_16);

    lv_obj_t *view_btn = lv_btn_create(_screen);
    lv_obj_set_size(view_btn, 132, 46);
    lv_obj_align(view_btn, LV_ALIGN_TOP_LEFT, 856, 14);
    styleBtn(view_btn, "XEM PANIC", onPanicViewClicked, this, lv_color_hex(0x1F3242), &lv_font_be_vietnam_16);

    /* Địa chỉ + trạng thái (dòng mảnh dưới toolbar) */
    _panic_url_label = lv_label_create(_screen);
    updatePanicUrlLabel();
    lv_obj_align(_panic_url_label, LV_ALIGN_TOP_LEFT, 24, 66);
    lv_obj_set_style_text_color(_panic_url_label, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(_panic_url_label, &lv_font_be_vietnam_16, 0);

    _panic_status_label = lv_label_create(_screen);
    lv_label_set_text(_panic_status_label, "Sẵn sàng");
    lv_obj_align(_panic_status_label, LV_ALIGN_TOP_RIGHT, -24, 66);
    lv_obj_set_style_text_color(_panic_status_label, lv_color_hex(0x69E0C0), 0);
    lv_obj_set_style_text_font(_panic_status_label, &lv_font_be_vietnam_16, 0);

    /* Không còn ô danh sách panic: chọn panic bằng ô "File panic" ở trên; kết quả dùng toàn chiều ngang. */
    _pi_list = nullptr;
    _pi_list_count = nullptr;

    /* ===== Kết quả (phải) =====
     * Khung KẾT LUẬN cố định (không cuộn): Model -> Lỗi -> PHẦN CỨNG NGHI NGỜ -> Confidence.
     * Bên dưới cuộn: Signature/HEX -> Các bước kiểm tra -> Verified/Repair case.
     * Mọi suy luận kỹ thuật (UNKNOWN, INFERRED, mailbox, bitmask, RTKit...) ở nút XEM PHÂN TÍCH CHI TIẾT. */
    const lv_color_t white = lv_color_hex(0xFFFFFF);
    const lv_color_t cyan = lv_color_hex(0x00D9FF);
    const lv_color_t green = lv_color_hex(0x8CE6A8);
    const lv_font_t *f16 = &lv_font_be_vietnam_16;

    lv_obj_t *hero = lv_obj_create(_screen);
    lv_obj_set_size(hero, 976, 214);
    lv_obj_align(hero, LV_ALIGN_TOP_LEFT, 24, 92);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x142433), 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(0x2A3E52), 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_radius(hero, 12, 0);
    lv_obj_set_style_pad_all(hero, 10, 0);
    lv_obj_set_style_pad_row(hero, 4, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    // Model | iOS | SoC | mức độ
    lv_obj_t *mrow = mkRow(hero);
    lv_obj_set_style_pad_column(mrow, 14, 0);
    _pi_model = lv_label_create(mrow);
    _pi_ios = lv_label_create(mrow);
    _pi_soc = lv_label_create(mrow);
    _pi_sev = lv_label_create(mrow);
    for (lv_obj_t *l : {_pi_model, _pi_ios, _pi_soc, _pi_sev}) {
        lv_label_set_text(l, "-");
        lv_obj_set_style_text_font(l, f16, 0);
        lv_obj_set_style_text_color(l, white, 0);
    }
    _pi_file = nullptr;
    _pi_sevsub = nullptr;

    // Lỗi (1 dòng)
    _pi_keystr = lv_label_create(hero);
    lv_label_set_long_mode(_pi_keystr, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_pi_keystr, lv_pct(100));
    lv_obj_set_height(_pi_keystr, 22);
    lv_obj_set_style_text_font(_pi_keystr, f16, 0);
    lv_obj_set_style_text_color(_pi_keystr, lv_color_hex(0xFF8A8A), 0);
    lv_label_set_text(_pi_keystr, "-");

    // PHẦN CỨNG NGHI NGỜ + CONFIDENCE
    lv_obj_t *hrow = mkRow(hero);
    lv_obj_t *htitle = lv_label_create(hrow);
    lv_label_set_text(htitle, LV_SYMBOL_WARNING " PHẦN CỨNG NGHI NGỜ");
    lv_obj_set_style_text_font(htitle, f16, 0);
    lv_obj_set_style_text_color(htitle, cyan, 0);
    lv_obj_set_flex_grow(htitle, 1);
    _pi_conf = lv_label_create(hrow);
    lv_label_set_recolor(_pi_conf, true);
    lv_obj_set_style_text_font(_pi_conf, f16, 0);
    lv_obj_set_style_text_color(_pi_conf, white, 0);
    lv_label_set_text(_pi_conf, "-");

    _pi_conclusion = lv_label_create(hero);
    lv_label_set_long_mode(_pi_conclusion, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_pi_conclusion, lv_pct(100));
    lv_obj_set_height(_pi_conclusion, 96);  // 3 dòng chữ 22: luôn thấy, không cuộn
    lv_obj_set_style_text_font(_pi_conclusion, &lv_font_be_vietnam_22, 0);
    lv_obj_set_style_text_color(_pi_conclusion, white, 0);
    lv_label_set_text(_pi_conclusion, "-");

    // Vùng cuộn: Signature/HEX -> Các bước kiểm tra -> Verified/Repair case.
    _pi_detail = lv_obj_create(_screen);
    lv_obj_set_size(_pi_detail, 976, 152);
    lv_obj_align(_pi_detail, LV_ALIGN_TOP_LEFT, 24, 312);
    lv_obj_set_style_bg_opa(_pi_detail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_pi_detail, 0, 0);
    lv_obj_set_style_pad_all(_pi_detail, 2, 0);
    lv_obj_set_style_pad_row(_pi_detail, 8, 0);
    lv_obj_set_flex_flow(_pi_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_pi_detail, LV_DIR_VER);

    lv_obj_t *cardSig = mkCard(_pi_detail);
    lv_obj_set_width(cardSig, lv_pct(100));
    mkCardTitle(cardSig, LV_SYMBOL_LIST, "SIGNATURE / HEX", cyan);
    _pi_cause = mkVal(cardSig, f16, white);

    lv_obj_t *cardCheck = mkCard(_pi_detail);
    lv_obj_set_width(cardCheck, lv_pct(100));
    mkCardTitle(cardCheck, LV_SYMBOL_EDIT, "CÁC BƯỚC KIỂM TRA", green);
    _pi_check = mkVal(cardCheck, f16, green);

    lv_obj_t *cardKb = mkCard(_pi_detail);
    lv_obj_set_width(cardKb, lv_pct(100));
    mkCardTitle(cardKb, LV_SYMBOL_OK, "VERIFIED / REPAIR CASE", cyan);
    _pi_kb = mkVal(cardKb, f16, cyan);
    _pi_ai = nullptr;

    /* ===== Danh sách + chi tiết ban đầu ===== */
    ensureKbLoaded();
    hccMemoryReport("PANIC sau khi nạp KB");
    _panic_seen_serial = HCCPanicServer::receivedSerial();
    populatePanicFileList();

    // Tự làm mới khi iPhone gửi panic mới (thay nút LÀM MỚI). Timer dừng khi rời màn này.
    if (_panic_poll_timer) {
        lv_timer_del(_panic_poll_timer);
    }
    _panic_poll_timer = lv_timer_create(onPanicPollTimer, 1000, this);
    lv_obj_add_event_cb(_pi_detail, onPanicListDeleted, LV_EVENT_DELETE, this);

    /* ===== Thanh nút dưới cùng ===== */
    const lv_coord_t by = 474;
    lv_obj_t *back_btn = lv_btn_create(_screen);
    lv_obj_set_size(back_btn, 150, 52);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 24, by);
    styleBtn(back_btn, "QUAY LẠI", onBackClicked, this, lv_color_hex(0x1F3242), &lv_font_be_vietnam_16);

    lv_obj_t *verify_btn = lv_btn_create(_screen);
    lv_obj_set_size(verify_btn, 190, 52);
    lv_obj_align(verify_btn, LV_ALIGN_TOP_LEFT, 810, by);
    styleBtn(verify_btn, "XÁC NHẬN ĐÃ SỬA", onConfirmRepairClicked, this, lv_color_hex(0x3DD66E), &lv_font_be_vietnam_16);

    lv_obj_t *detail_btn = lv_btn_create(_screen);
    lv_obj_set_size(detail_btn, 300, 52);
    lv_obj_align(detail_btn, LV_ALIGN_TOP_LEFT, 300, by);
    styleBtn(detail_btn, "XEM PHÂN TÍCH CHI TIẾT", onPanicDetailClicked, this, lv_color_hex(0x2478D0), &lv_font_be_vietnam_16);
}

void HCCHome::showPanicDeviceInfo(const ParsedPanic &parsed)
{
    auto setL = [](lv_obj_t *o, const std::string &s) {
        if (o) lv_label_set_text(o, s.empty() ? "-" : s.c_str());
    };
    // Đời máy trước, mã máy trong ngoặc: "Model: iPhone 13 Pro (iPhone14,2)".
    const std::string &mk = parsed.model;
    const std::string mn = panicMarketingName(mk);
    std::string modeltxt = "Model: ";
    if (!mn.empty()) {
        modeltxt += mn + " (" + mk + ")";
    } else {
        modeltxt += mk.empty() ? std::string("-") : mk;
    }
    setL(_pi_model, modeltxt);
    // "iPhone OS 17.5 (21F79)" -> "17.5 (21F79)".
    std::string os = parsed.os_version;
    if (os.rfind("iPhone OS ", 0) == 0) os = os.substr(10);
    setL(_pi_ios, "iOS: " + (os.empty() ? std::string("-") : os));
    const char *soc = panicSocForModel(mk);
    setL(_pi_soc, std::string("SoC: ") + (soc ? soc : "-"));
}

// ---- Knowledge Base V1 ----

void HCCHome::ensureKbLoaded(bool force)
{
    if (_kb_loaded && !force) return;
    HCCPanicKBStore::LoadReport rep;
    _kb_loaded = HCCPanicKBStore::load(_kb, _kb_cases, rep);
    for (const auto &m : rep.messages) printf("[HCC KB] %s\n", m.c_str());
}

bool HCCHome::computeKbView(const ParsedPanic &parsed, PanicResult *case_result)
{
    if (!_kb_loaded) {
        setKbPlaceholder("KB chưa nạp được (firmware/SD)");
        return false;
    }
    const HCCKB::Analysis a = HCCKB::analyze(parsed.facts, _kb, _kb_cases);
    const HCCKB::BasicView b = HCCKB::renderBasic(parsed.facts, a);
    _kv_error = b.error;
    _kv_sig = b.signature;
    _kv_hw = b.hardware;
    _kv_hw_confirmed = b.hardware_confirmed;
    _kv_hw_known = b.hardware_known;
    _kv_conf = b.confidence_text;
    _kv_conf_level = b.confidence;
    _kv_steps = b.steps;
    _kv_case = b.case_match;
    _kv_detail = HCCKB::renderDetail(parsed.facts, a);
    if (a.cases.empty() || a.cases[0].confidence < HCCKB::Confidence::Medium) return false;
    if (case_result) {
        const HCCKB::CaseRecord &c = *a.cases[0].rec;
        PanicResult r;
        r.ok = true;
        r.source_tier = c.status;
        r.confidence = HCCKB::confidenceName(a.cases[0].confidence);
        r.error = c.hardware_area;
        r.cause = a.cases[0].reason;
        r.repair = c.repair_action;
        *case_result = r;
    }
    return true;
}

void HCCHome::showKbView()
{
    auto set = [](lv_obj_t *o, const std::string &s) {
        if (o) lv_label_set_text(o, s.empty() ? "-" : s.c_str());
    };
    set(_pi_keystr, _kv_error);
    set(_pi_cause, _kv_sig);
    set(_pi_check, _kv_steps);
    set(_pi_kb, _kv_case);
    set(_pi_conclusion, _kv_hw);
    if (_pi_conclusion) {
        // Xanh: VERIFIED case khớp chính xác; cam: nghi ngờ chưa xác nhận; xám: chưa xác định.
        const uint32_t col = _kv_hw_confirmed ? 0x3DD66E : (_kv_hw_known ? 0xFFB347 : 0xB8C5CF);
        lv_obj_set_style_text_color(_pi_conclusion, lv_color_hex(col), 0);
    }
    if (_pi_conf) {
        const char *col = "#8A97A3 ";
        if (_kv_conf_level == HCCKB::Confidence::High) col = "#3DD66E ";
        else if (_kv_conf_level == HCCKB::Confidence::Medium) col = "#F2C94C ";
        else if (_kv_conf_level == HCCKB::Confidence::Low) col = "#FF9F43 ";
        const std::string t = std::string("CONFIDENCE: ") + col + (_kv_conf.empty() ? "-" : _kv_conf) + "#";
        lv_label_set_text(_pi_conf, t.c_str());
    }
}

void HCCHome::setKbPlaceholder(const char *text)
{
    _kv_error = _kv_sig = _kv_steps = _kv_case = "-";
    _kv_hw = text;
    _kv_detail = text;
    _kv_conf = "-";
    _kv_conf_level = HCCKB::Confidence::None;
    _kv_hw_confirmed = _kv_hw_known = false;
}

void HCCHome::refreshPanicDetail()
{
    auto setL = [](lv_obj_t *o, const std::string &s) {
        if (o) lv_label_set_text(o, s.empty() ? "-" : s.c_str());
    };

    std::string fileshown = _panic_file;
    if (fileshown.rfind("[RAM] ", 0) == 0) fileshown = fileshown.substr(6);
    setL(_pi_file, "File: " + fileshown);

    // Ảnh panic?
    std::string mime;
    if (strncmp(_panic_file, "[RAM] ", 6) == 0 && HCCPanicServer::receivedIsImage(&mime)) {
        setL(_pi_model, "Model: (ảnh panic)");
        setL(_pi_ios, "iOS: -");
        setL(_pi_soc, "SoC: -");
        if (_pi_sev) { lv_label_set_text(_pi_sev, "ẢNH"); lv_obj_set_style_text_color(_pi_sev, lv_color_hex(0x00D9FF), 0); }
        setL(_pi_sevsub, "Ảnh chụp panic - dùng ANALYZE để AI đọc");
        setL(_pi_keystr, "(Ảnh panic)");
        if (!_panic_busy) {
            setKbPlaceholder("Ảnh panic: bấm ANALYZE để AI chép chữ rồi phân tích");
            _kv_error = "(Ảnh panic)";
            _kv_ai = "-";
            showKbView();
        }
        if (_pi_detail) lv_obj_scroll_to_y(_pi_detail, 0, LV_ANIM_OFF);
        return;
    }

    std::string raw;
    if (!loadSelectedRaw(raw)) {
        setL(_pi_model, "Model: -");
        setL(_pi_ios, "iOS: -");
        setL(_pi_soc, "SoC: -");
        if (_pi_sev) { lv_label_set_text(_pi_sev, "-"); lv_obj_set_style_text_color(_pi_sev, lv_color_hex(0xB8C5CF), 0); }
        setL(_pi_sevsub, "-");
        setL(_pi_keystr, "(chọn panic để xem)");
        if (!_panic_busy) {
            setKbPlaceholder("Chọn panic để xem kết quả");
            _kv_ai = "-";
            showKbView();
        }
        if (_pi_detail) lv_obj_scroll_to_y(_pi_detail, 0, LV_ANIM_OFF);
        return;
    }

    _pi_parsed = HCCPanicParser::parse(raw);
    showPanicDeviceInfo(_pi_parsed);

    // Mức độ lỗi.
    std::string low = _pi_parsed.panic_type + " " + _pi_parsed.panic_string;
    for (auto &c : low) c = (char)tolower((unsigned char)c);
    bool severe = low.find("panic-full") != std::string::npos ||
                  low.find("kernel") != std::string::npos ||
                  low.find("panic(") != std::string::npos ||
                  low.find("busy timeout") != std::string::npos ||
                  _pi_parsed.panic_type == "210";
    if (_pi_sev) {
        lv_label_set_text(_pi_sev, severe ? "NGHIÊM TRỌNG" : "PANIC");
        lv_obj_set_style_text_color(_pi_sev, severe ? lv_color_hex(0xFF5252) : lv_color_hex(0xF2C94C), 0);
    }
    setL(_pi_sevsub, severe ? "Kernel Panic (Panic Full) - tự động khởi động lại"
                            : "Panic - kiểm tra chi tiết");

    // KB V1 hiện ngay khi chọn panic (không cần mạng); AI chỉ chạy khi bấm ANALYZE.
    // Đang ANALYZE thì không dựng lại (task đang ghi các chuỗi _kv_*).
    if (!_panic_busy) {
        computeKbView(_pi_parsed);
        _kv_ai = "#8A97A3 Chưa gọi AI - bấm ANALYZE nếu chưa có case khớp.#";
        showKbView();
    }

    // Dòng LỖI (khung kết luận) do showKbView() điền từ Basic Result.
    if (_pi_detail) lv_obj_scroll_to_y(_pi_detail, 0, LV_ANIM_OFF);
}

void HCCHome::onPanicApiKeyClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_screen == nullptr || app->_panic_apikey_dialog != nullptr) {
        return;
    }

    // Lớp phủ mờ toàn màn hình.
    app->_panic_apikey_dialog = lv_obj_create(app->_screen);
    lv_obj_set_size(app->_panic_apikey_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(app->_panic_apikey_dialog);
    lv_obj_set_style_bg_color(app->_panic_apikey_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(app->_panic_apikey_dialog, LV_OPA_60, 0);
    lv_obj_set_style_border_width(app->_panic_apikey_dialog, 0, 0);
    lv_obj_clear_flag(app->_panic_apikey_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(app->_panic_apikey_dialog);
    lv_obj_set_size(panel, 860, 260);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B141C), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(panel);
    lv_label_set_text(t, "Nhập API key AI (OpenAI sk-... hoặc Claude sk-ant-...)");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_text_color(t, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(t, &lv_font_be_vietnam_18, 0);

    app->_panic_apikey_input = lv_textarea_create(panel);
    lv_obj_set_size(app->_panic_apikey_input, 820, 60);
    lv_obj_align(app->_panic_apikey_input, LV_ALIGN_TOP_LEFT, 4, 40);
    lv_textarea_set_one_line(app->_panic_apikey_input, true);
    lv_textarea_set_placeholder_text(app->_panic_apikey_input, "sk-... (OpenAI) hoặc sk-ant-... (Claude)");
    lv_obj_set_style_text_font(app->_panic_apikey_input, &lv_font_be_vietnam_18, 0);

    lv_obj_t *save_btn = lv_btn_create(panel);
    lv_obj_set_size(save_btn, 140, 50);
    lv_obj_align(save_btn, LV_ALIGN_TOP_LEFT, 4, 120);
    lv_obj_add_event_cb(save_btn, onPanicApiKeySaveClicked, LV_EVENT_CLICKED, app);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "LƯU");
    lv_obj_center(save_lbl);
    lv_obj_set_style_text_font(save_lbl, &lv_font_be_vietnam_18, 0);

    lv_obj_t *close_btn = lv_btn_create(panel);
    lv_obj_set_size(close_btn, 140, 50);
    lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 160, 120);
    lv_obj_add_event_cb(close_btn, onPanicApiKeyCloseClicked, LV_EVENT_CLICKED, app);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "ĐÓNG");
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_font(close_lbl, &lv_font_be_vietnam_18, 0);

    // Bàn phím lớn (API key là ASCII nên KHÔNG bật Telex).
    app->_panic_keyboard = lv_keyboard_create(app->_panic_apikey_dialog);
    lv_obj_set_size(app->_panic_keyboard, LV_PCT(100), 320);
    lv_obj_align(app->_panic_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(app->_panic_keyboard, &lv_font_be_vietnam_22, 0);
    lv_keyboard_set_textarea(app->_panic_keyboard, app->_panic_apikey_input);
}

void HCCHome::onPanicApiKeySaveClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_panic_apikey_input == nullptr) {
        return;
    }
    const char *key = lv_textarea_get_text(app->_panic_apikey_input);
    bool ok = false;
    if (key && key[0]) {
        ok = HCCPanicAI::saveApiKey(std::string(key));
    }
    if (app->_panic_status_label != nullptr) {
        lv_label_set_text(app->_panic_status_label,
                          ok ? "Đã lưu API key" : "Lưu API key thất bại (thẻ SD?)");
    }
    if (app->_panic_apikey_dialog != nullptr) {
        lv_obj_del(app->_panic_apikey_dialog);
        app->_panic_apikey_dialog = nullptr;
        app->_panic_apikey_input = nullptr;
        app->_panic_keyboard = nullptr;
    }
}

void HCCHome::onPanicApiKeyCloseClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_panic_apikey_dialog == nullptr) {
        return;
    }
    lv_obj_del(app->_panic_apikey_dialog);
    app->_panic_apikey_dialog = nullptr;
    app->_panic_apikey_input = nullptr;
    app->_panic_keyboard = nullptr;
}

// ==================== CONFIRM REPAIR / VERIFY ====================

// Tạo 1 ô nhập có nhãn trong container; gắn sự kiện focus để bật bàn phím.
static lv_obj_t *repairField(lv_obj_t *parent, const char *label, const char *prefill,
                             lv_event_cb_t focus_cb, void *user)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(0x7F96A8), 0);
    lv_obj_set_style_text_font(l, &lv_font_be_vietnam_16, 0);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, lv_pct(100));
    lv_textarea_set_one_line(ta, true);
    if (prefill && prefill[0]) lv_textarea_set_text(ta, prefill);
    lv_obj_set_style_text_font(ta, &lv_font_be_vietnam_16, 0);
    lv_obj_add_event_cb(ta, focus_cb, LV_EVENT_FOCUSED, user);
    return ta;
}

static const char *kRepairActions[] = {"replaced", "repaired", "reworked", "cleaned", "other"};
static const char *kMeasureTypes[] = {"voltage", "diode", "resistance", "current", "other"};

static lv_obj_t *repairDropdown(lv_obj_t *parent, const char *label, const char *options)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(0x7F96A8), 0);
    lv_obj_set_style_text_font(l, &lv_font_be_vietnam_16, 0);
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_width(dd, lv_pct(100));
    lv_dropdown_set_options(dd, options);
    lv_obj_set_style_text_font(dd, &lv_font_be_vietnam_16, 0);
    return dd;
}

void HCCHome::onConfirmRepairClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_repair_dialog != nullptr) {
        return;
    }
    if (app->_panic_busy) {
        if (app->_panic_status_label) lv_label_set_text(app->_panic_status_label, "Đang phân tích, chờ xong");
        return;
    }
    if (app->_panic_file[0] == '\0' || app->_panic_file[0] == '(') {
        if (app->_panic_status_label) lv_label_set_text(app->_panic_status_label, "Chưa chọn file panic");
        return;
    }
    if (strncmp(app->_panic_file, "[RAM] ", 6) == 0 && HCCPanicServer::receivedIsImage(nullptr) &&
        app->_panic_img_parsed_file != app->_panic_file) {
        if (app->_panic_status_label)
            lv_label_set_text(app->_panic_status_label, "Ảnh panic: bấm ANALYZE trước để AI chép chữ");
        return;
    }
    app->openConfirmRepair();
}

void HCCHome::onRepairFieldFocused(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_repair_kb == nullptr) return;
    lv_obj_t *ta = lv_event_get_target(event);
    lv_keyboard_set_textarea(app->_repair_kb, ta);
    lv_obj_clear_flag(app->_repair_kb, LV_OBJ_FLAG_HIDDEN);
}

void HCCHome::openConfirmRepair()
{
    // Panic đang chọn (RAM/SD), hoặc chữ AI đã chép từ ảnh panic.
    if (strncmp(_panic_file, "[RAM] ", 6) == 0 && HCCPanicServer::receivedIsImage(nullptr)) {
        _repair_parsed = _panic_img_parsed;
    } else {
        std::string raw;
        if (!loadSelectedRaw(raw)) {
            if (_panic_status_label) lv_label_set_text(_panic_status_label, "Không đọc được panic đang chọn");
            return;
        }
        _repair_parsed = HCCPanicParser::parse(raw);
    }
    const PanicFacts &f = _repair_parsed.facts;

    // Overlay mờ.
    _repair_dialog = lv_obj_create(_screen);
    lv_obj_set_size(_repair_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_repair_dialog);
    lv_obj_set_style_bg_color(_repair_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_repair_dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_repair_dialog, 0, 0);
    lv_obj_clear_flag(_repair_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(_repair_dialog);
    lv_obj_set_size(panel, 940, 300);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B141C), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    auto button = [&](const char *txt, lv_coord_t x, lv_coord_t w, uint32_t bg, lv_event_cb_t cb, bool dark) {
        lv_obj_t *b = lv_btn_create(panel);
        lv_obj_set_size(b, w, 44);
        lv_obj_align(b, x >= 0 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, x >= 0 ? x : 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_center(l);
        if (dark) lv_obj_set_style_text_color(l, lv_color_hex(0x0B141C), 0);
        lv_obj_set_style_text_font(l, &lv_font_be_vietnam_16, 0);
    };
    button("LƯU (CHƯA XÁC NHẬN)", 0, 240, 0xF2C94C, onRepairSaveClicked, true);
    button("XÁC NHẬN VERIFIED", 250, 220, 0x3DD66E, onRepairVerifyClicked, true);
    button("ĐÓNG", -1, 120, 0x1F3242, onRepairCloseClicked, false);

    // Vùng cuộn chứa các trường.
    lv_obj_t *form = lv_obj_create(panel);
    lv_obj_set_size(form, 900, 230);
    lv_obj_align(form, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_bg_opa(form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(form, 0, 0);
    lv_obj_set_style_pad_row(form, 4, 0);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(form, LV_DIR_VER);

    // Dữ kiện tự lấy từ panic (Parser v2) - không sửa tay để signature luôn khớp.
    std::string info = "Loại: " + std::string(panicKindName(f.primary_kind)) + "   bug_type: " +
                       (f.bug_type.empty() ? "-" : f.bug_type) + "   iOS: " +
                       (f.os_version.empty() ? (f.build.empty() ? "-" : f.build) : f.os_version) +
                       "\nSignature: " + f.signature;
    HCCKB::CaseRecord pre = HCCKB::caseFromFacts(f);
    auto joinv = [](const std::vector<std::string> &v) {
        std::string o;
        for (size_t i = 0; i < v.size() && i < 8; ++i) o += (i ? ", " : "") + v[i];
        return o.empty() ? std::string("-") : o;
    };
    info += "\nDriver: " + joinv(pre.drivers) + "\nSensor: " + joinv(pre.sensors) + "\nMã: " + joinv(pre.codes);
    _repair_sig = lv_label_create(form);
    lv_label_set_long_mode(_repair_sig, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_repair_sig, lv_pct(100));
    lv_label_set_text(_repair_sig, info.c_str());
    lv_obj_set_style_text_color(_repair_sig, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(_repair_sig, &lv_font_be_vietnam_16, 0);

    _repair_model = repairField(form, "MODEL (iPhoneN,M)", f.product.c_str(), onRepairFieldFocused, this);
    _rp_hw = repairField(form, "KHU VỰC PHẦN CỨNG (thực tế)", "", onRepairFieldFocused, this);
    _rp_comp = repairField(form, "LINH KIỆN / CÁP ĐÃ THAY HOẶC SỬA", "", onRepairFieldFocused, this);
    _rp_ref = repairField(form, "KÝ HIỆU TRÊN MAIN / MÃ LINH KIỆN (vd U3300)", "", onRepairFieldFocused, this);
    _rp_comp_action = repairDropdown(form, "HÀNH ĐỘNG VỚI LINH KIỆN",
                                     "replaced - thay mới\nrepaired - sửa\nreworked - làm lại / đóng lại\n"
                                     "cleaned - vệ sinh\nother - khác");
    _rp_busrail = repairField(form, "BUS / RAIL LIÊN QUAN (cách nhau dấu ;)", "", onRepairFieldFocused, this);
    _rp_m_point = repairField(form, "ĐIỂM ĐO (net / chân)", "", onRepairFieldFocused, this);
    _rp_m_type = repairDropdown(form, "LOẠI ĐO",
                                "voltage - điện áp\ndiode - đo diode\nresistance - điện trở\ncurrent - dòng\n"
                                "other - khác");
    _rp_m_before = repairField(form, "GIÁ TRỊ TRƯỚC KHI SỬA", "", onRepairFieldFocused, this);
    _rp_m_after = repairField(form, "GIÁ TRỊ SAU KHI SỬA", "", onRepairFieldFocused, this);
    _rp_m_unit = repairField(form, "ĐƠN VỊ (V, mA, ohm...)", "", onRepairFieldFocused, this);
    _repair_action = repairField(form, "CÁCH SỬA (repair action)", "", onRepairFieldFocused, this);
    _rp_resolved = lv_checkbox_create(form);
    lv_checkbox_set_text(_rp_resolved, "Đã hết panic sau khi sửa (bắt buộc để VERIFIED)");
    lv_obj_set_style_text_font(_rp_resolved, &lv_font_be_vietnam_16, 0);
    lv_obj_set_style_text_color(_rp_resolved, lv_color_hex(0xFFFFFF), 0);
    _rp_days = repairField(form, "SỐ NGÀY THEO DÕI SAU SỬA", "", onRepairFieldFocused, this);
    lv_textarea_set_accepted_chars(_rp_days, "0123456789");
    _repair_note = repairField(form, "GHI CHÚ", "", onRepairFieldFocused, this);

    // Gõ tiếng Việt cho các ô mô tả (MODEL / ký hiệu / số đo là ASCII).
    for (lv_obj_t *ta : {_rp_hw, _rp_comp, _rp_busrail, _repair_action, _repair_note}) HCCViText::markVietnamese(ta);

    // Bàn phím lớn + Telex (ẩn cho tới khi focus 1 ô).
    _repair_kb = HCCViText::createKeyboard(_repair_dialog, nullptr);
    lv_obj_add_flag(_repair_kb, LV_OBJ_FLAG_HIDDEN);
}

bool HCCHome::submitRepairCase(bool verified)
{
    if (_repair_model == nullptr) return false;
    const PanicFacts &f = _repair_parsed.facts;
    auto txt = [](lv_obj_t *ta) { return std::string(ta ? lv_textarea_get_text(ta) : ""); };
    auto setStatus = [&](const char *m) {
        if (_panic_status_label) lv_label_set_text(_panic_status_label, m);
    };

    HCCKB::CaseRecord c = HCCKB::caseFromFacts(f);
    const std::string model = txt(_repair_model);
    if (model != f.product) {
        // Model nhập tay (log không có / sai): dựng lại signature cho model đó.
        c.model = model;
        c.signature = model.empty() ? std::string() : "v1|" + model + f.signature_generic.substr(4);
    }
    c.hardware_area = txt(_rp_hw);
    if (!txt(_rp_comp).empty() || !txt(_rp_ref).empty()) {
        HCCKB::Component cp;
        cp.name = txt(_rp_comp);
        cp.ref = txt(_rp_ref);
        cp.action = kRepairActions[std::min<uint16_t>(lv_dropdown_get_selected(_rp_comp_action), 4)];
        c.components.push_back(cp);
    }
    {
        std::string br = txt(_rp_busrail), cur;
        for (char ch : br + ";") {
            if (ch == ';' || ch == ',') {
                size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
                if (a != std::string::npos) c.bus_rail.push_back(cur.substr(a, b - a + 1));
                cur.clear();
            } else {
                cur += ch;
            }
        }
    }
    if (!txt(_rp_m_point).empty()) {
        HCCKB::Measurement m;
        m.point = txt(_rp_m_point);
        m.type = kMeasureTypes[std::min<uint16_t>(lv_dropdown_get_selected(_rp_m_type), 4)];
        m.before = txt(_rp_m_before);
        m.after = txt(_rp_m_after);
        m.unit = txt(_rp_m_unit);
        c.measurements.push_back(m);
    }
    c.repair_action = txt(_repair_action);
    c.result.panic_resolved = lv_obj_has_state(_rp_resolved, LV_STATE_CHECKED);
    c.result.observed_days = txt(_rp_days).empty() ? -1 : atoi(txt(_rp_days).c_str());
    c.notes = txt(_repair_note);

    if (verified) {
        // Chặn trước khi lưu (store cũng tự hạ, nhưng báo rõ cho kỹ thuật viên).
        if (c.model.empty() || c.signature.empty()) { setStatus("VERIFIED cần MODEL + signature"); return false; }
        if (c.repair_action.empty()) { setStatus("VERIFIED cần ghi CÁCH SỬA"); return false; }
        if (!c.result.panic_resolved) { setStatus("VERIFIED cần tick 'Đã hết panic sau khi sửa'"); return false; }
    }
    c.status = verified ? "VERIFIED" : "REPAIR_CONFIRMED";

    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32], idts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    strftime(idts, sizeof(idts), "%Y%m%d_%H%M%S", &tmv);
    c.date = ts;
    std::string mid;
    for (char ch : c.model) mid += isalnum((unsigned char)ch) ? (char)tolower((unsigned char)ch) : '_';
    c.id = std::string("case_") + idts + "_" + (mid.empty() ? "unknown" : mid);
    c.evidence_panic_file = _panic_file;
    if (c.evidence_panic_file.rfind("[RAM] ", 0) == 0) c.evidence_panic_file = c.evidence_panic_file.substr(6);

    std::string err;
    if (!HCCPanicKBStore::saveCase(c, err)) {
        printf("[HCC KB] lưu case lỗi: %s\n", err.c_str());
        setStatus("Lưu case thất bại (thẻ SD?)");
        return false;
    }
    ensureKbLoaded(true);
    setStatus(verified ? "Đã lưu VERIFIED case" : "Đã lưu REPAIR_CONFIRMED case");
    return true;
}

void HCCHome::onRepairSaveClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) return;
    if (app->submitRepairCase(false)) {
        onRepairCloseClicked(event);
        app->refreshPanicDetail();
    }
}

void HCCHome::onRepairVerifyClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) return;
    if (app->submitRepairCase(true)) {
        onRepairCloseClicked(event);
        app->refreshPanicDetail();
    }
}

void HCCHome::onRepairCloseClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_repair_dialog == nullptr) return;
    lv_obj_del(app->_repair_dialog);
    app->_repair_dialog = nullptr;
    app->_repair_kb = nullptr;
    app->_repair_model = nullptr;
    app->_repair_sig = nullptr;
    app->_repair_action = nullptr;
    app->_repair_note = nullptr;
    for (lv_obj_t **o : {&app->_rp_hw, &app->_rp_comp, &app->_rp_ref, &app->_rp_comp_action, &app->_rp_busrail,
                         &app->_rp_m_point, &app->_rp_m_type, &app->_rp_m_before, &app->_rp_m_after,
                         &app->_rp_m_unit, &app->_rp_resolved, &app->_rp_days}) {
        *o = nullptr;
    }
}

// ==================== XEM PANIC (raw viewer) ====================

void HCCHome::onPanicViewClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_screen == nullptr || app->_panic_view_dialog != nullptr) {
        return;
    }
    if (app->_panic_file[0] == '\0' || app->_panic_file[0] == '(') {
        if (app->_panic_status_label) lv_label_set_text(app->_panic_status_label, "Chưa chọn file panic");
        return;
    }

    // Ảnh panic: không hiển thị raw nhị phân, chỉ báo.
    std::string vmime;
    if (strncmp(app->_panic_file, "[RAM] ", 6) == 0 && HCCPanicServer::receivedIsImage(&vmime)) {
        std::string n, b;
        HCCPanicServer::getReceived(n, b);
        char note[128];
        snprintf(note, sizeof(note), "(Ảnh panic: %s, %u byte)\nDùng ANALYZE để AI đọc ảnh.",
                 vmime.c_str(), (unsigned)b.size());
        std::string raw_note = note;
        // dựng dialog với raw_note
        std::string raw = raw_note;
        // (rơi xuống dùng chung phần dựng dialog bên dưới qua biến raw)
        // -- tiếp tục như text --
        app->_panic_view_dialog = nullptr; // placeholder tránh cảnh báo (sẽ set dưới)
        // Xây dialog ngay tại đây:
        app->_panic_view_dialog = lv_obj_create(app->_screen);
        lv_obj_set_size(app->_panic_view_dialog, LV_PCT(100), LV_PCT(100));
        lv_obj_center(app->_panic_view_dialog);
        lv_obj_set_style_bg_color(app->_panic_view_dialog, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(app->_panic_view_dialog, LV_OPA_70, 0);
        lv_obj_set_style_border_width(app->_panic_view_dialog, 0, 0);
        lv_obj_clear_flag(app->_panic_view_dialog, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *p = lv_obj_create(app->_panic_view_dialog);
        lv_obj_set_size(p, 700, 240);
        lv_obj_center(p);
        lv_obj_set_style_bg_color(p, lv_color_hex(0x0B141C), 0);
        lv_obj_set_style_radius(p, 12, 0);
        lv_obj_t *lbl = lv_label_create(p);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_label_set_text(lbl, raw.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCFE3EF), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_be_vietnam_18, 0);
        lv_obj_t *cb = lv_btn_create(p);
        lv_obj_set_size(cb, 120, 44);
        lv_obj_align(cb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(cb, onPanicViewCloseClicked, LV_EVENT_CLICKED, app);
        lv_obj_t *cbl = lv_label_create(cb);
        lv_label_set_text(cbl, "ĐÓNG");
        lv_obj_center(cbl);
        lv_obj_set_style_text_font(cbl, &lv_font_be_vietnam_16, 0);
        return;
    }

    std::string raw;
    if (!app->loadSelectedRaw(raw)) {
        if (app->_panic_status_label) lv_label_set_text(app->_panic_status_label, "Không đọc được panic đang chọn");
        return;
    }
    if (raw.size() > 8000) raw = raw.substr(0, 8000) + "\n... (da cat bot)";

    app->_panic_view_dialog = lv_obj_create(app->_screen);
    lv_obj_set_size(app->_panic_view_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(app->_panic_view_dialog);
    lv_obj_set_style_bg_color(app->_panic_view_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(app->_panic_view_dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(app->_panic_view_dialog, 0, 0);
    lv_obj_clear_flag(app->_panic_view_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(app->_panic_view_dialog);
    lv_obj_set_size(panel, 940, 540);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B141C), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dlg_title = lv_label_create(panel);
    char tt[128];
    snprintf(tt, sizeof(tt), "PANIC: %s", app->_panic_file);
    lv_label_set_text(dlg_title, tt);
    lv_obj_align(dlg_title, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_text_color(dlg_title, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(dlg_title, &lv_font_be_vietnam_18, 0);

    lv_obj_t *close_btn = lv_btn_create(panel);
    lv_obj_set_size(close_btn, 110, 42);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_add_event_cb(close_btn, onPanicViewCloseClicked, LV_EVENT_CLICKED, app);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "ĐÓNG");
    lv_obj_center(cl);
    lv_obj_set_style_text_font(cl, &lv_font_be_vietnam_16, 0);

    lv_obj_t *content = lv_obj_create(panel);
    lv_obj_set_size(content, 900, 470);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x05090D), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 6, 0);

    lv_obj_t *body = lv_label_create(content);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_text(body, raw.c_str());
    lv_obj_set_style_text_color(body, lv_color_hex(0xCFE3EF), 0);
    lv_obj_set_style_text_font(body, &lv_font_be_vietnam_16, 0);
}

// ==================== XEM PHÂN TÍCH CHI TIẾT (Technical Detail) ====================

void HCCHome::onPanicDetailClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_screen == nullptr || app->_panic_detail_dialog != nullptr) {
        return;
    }
    if (app->_panic_busy) {
        if (app->_panic_status_label) lv_label_set_text(app->_panic_status_label, "Đang phân tích, chờ xong");
        return;
    }
    // Chi tiết kỹ thuật (KB + Parser v2) + phần AI (nếu đã ANALYZE).
    std::string text = app->_kv_detail.empty() ? std::string("Chưa có phân tích") : app->_kv_detail;
    text += "\n\n#B48CFF == PHÂN TÍCH AI (AI_INFERENCE - tham khảo) ==#\n" +
            (app->_kv_ai.empty() ? std::string("-") : app->_kv_ai);
    if (text.size() > 24000) text = text.substr(0, 24000) + "\n... (đã cắt bớt)";

    app->_panic_detail_dialog = lv_obj_create(app->_screen);
    lv_obj_t *dlg = app->_panic_detail_dialog;
    lv_obj_set_size(dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(dlg, 0, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(dlg);
    lv_obj_set_size(panel, 940, 540);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B141C), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "PHÂN TÍCH CHI TIẾT (kỹ thuật)");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_be_vietnam_18, 0);

    lv_obj_t *close_btn = lv_btn_create(panel);
    lv_obj_set_size(close_btn, 110, 42);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_add_event_cb(close_btn, onPanicDetailCloseClicked, LV_EVENT_CLICKED, app);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "ĐÓNG");
    lv_obj_center(cl);
    lv_obj_set_style_text_font(cl, &lv_font_be_vietnam_16, 0);

    lv_obj_t *area = lv_obj_create(panel);
    lv_obj_set_size(area, 910, 470);
    lv_obj_align(area, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_set_style_bg_color(area, lv_color_hex(0x101C28), 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_scroll_dir(area, LV_DIR_VER);

    lv_obj_t *lbl = lv_label_create(area);
    lv_label_set_recolor(lbl, true);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_text(lbl, text.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xCFE3EF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_be_vietnam_16, 0);
}

void HCCHome::onPanicDetailCloseClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_panic_detail_dialog == nullptr) return;
    lv_obj_del(app->_panic_detail_dialog);
    app->_panic_detail_dialog = nullptr;
}

void HCCHome::onPanicViewCloseClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_panic_view_dialog == nullptr) return;
    lv_obj_del(app->_panic_view_dialog);
    app->_panic_view_dialog = nullptr;
}

// ==================== IPHONE HARDWARE CHECK ====================

static lv_color_t hwStatusColor(HCCiPhoneDiagnostics::Status s)
{
    switch (s) {
        case HCCiPhoneDiagnostics::Status::Pass: return lv_color_hex(0x3DD66E);
        case HCCiPhoneDiagnostics::Status::Fail: return lv_color_hex(0xFF5252);
        case HCCiPhoneDiagnostics::Status::Check: return lv_color_hex(0xF2C94C);
        case HCCiPhoneDiagnostics::Status::Unsupported: return lv_color_hex(0x8AA0B4);
        case HCCiPhoneDiagnostics::Status::NotTested:
        default: return lv_color_hex(0x7F96A8);
    }
}

void HCCHome::onHardwareCheckClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app) app->showHardwareCheck();
}

void HCCHome::onHwBackClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app) {
        // A diagnostics result may still arrive asynchronously; it must not touch freed labels.
        app->detachHwLabels();
        app->showUSB();
    }
}

// The USB client task refreshes the USB HUB labels (lv_async_call(applyUSBResult)) at any
// time, e.g. when the phone is unplugged; every other screen must detach them before
// lv_obj_clean() frees them.
void HCCHome::detachUsbLabels()
{
    _usb_status_label = nullptr;
    _usb_info_label = nullptr;
    _usb_mode_label = nullptr;
    _recovery_button_label = nullptr;
    _bootloader_button_label = nullptr;
    _purple_button_label = nullptr;
    _reboot_button_label = nullptr;
}

void HCCHome::detachHwLabels()
{
    _hw_sensors_status = nullptr;
    _hw_faceid_status = nullptr;
    _hw_baseband_status = nullptr;
    _hw_battery_status = nullptr;
    _hw_camera_status = nullptr;
    _hw_session_label = nullptr;
    _hw_dialog = nullptr;
    // The camera test overlay lives on the same screen: drop it (a late job result is ignored).
    _cam_panel = nullptr;
    _cam_title = nullptr;
    _cam_text = nullptr;
    _cam_status = nullptr;
    _cam_phase = CamWizardPhase::Idle;
}

// LVGL task (lv_async_call from the USB task): parse the finished ADB diagnostics job, or
// report that it could not run.
void HCCHome::applyAndroidDiagnostics(void *arg)
{
    HCCHome *app = static_cast<HCCHome *>(arg);
    if (app == nullptr) {
        return;
    }
    const HCCAdbClient::JobState job = app->_adb.jobState();
    const bool finished = !app->_hw_android_start_failed &&
                          (job == HCCAdbClient::JobState::Done || job == HCCAdbClient::JobState::TimedOut);
    app->_hw_android_start_failed = false;
    if (app->_hw_last_job_camera) {
        HCCHome::camWizardJobDone(app, finished && job == HCCAdbClient::JobState::Done);
        return;
    }
    char message[160];
    if (!finished) {
        snprintf(message, sizeof(message), "ADB: không chạy được lệnh chẩn đoán (kiểm tra kết nối)");
    } else {
        // Parse whatever arrived; groups whose section is missing stay NOT TESTED.
        app->_hw_result = HCCAndroidDiagnostics::parse(app->_adb.jobOutput());
        app->_hw_result_android = true;
        app->_hw_has_result = true;
        app->updateHwCards();
        const unsigned bytes = static_cast<unsigned>(app->_adb.jobOutputLength());
        if (job == HCCAdbClient::JobState::TimedOut) {
            snprintf(message, sizeof(message), "ADB: điện thoại không trả lời xong trong 60 giây (%u byte) - gửi log", bytes);
        } else if (strstr(app->_adb.jobOutput(), "##E") == nullptr) {
            snprintf(message, sizeof(message), "ADB: kết quả không đầy đủ (%u byte) - gửi log", bytes);
        } else {
            snprintf(message, sizeof(message), "ADB: XONG - chạm vào từng thẻ để xem evidence");
        }
    }
    if (app->_hw_session_label != nullptr) {
        lv_label_set_text(app->_hw_session_label, message);
    }
}

void HCCHome::updateHwCards()
{
    if (!_hw_has_result) return;
    auto set = [](lv_obj_t *l, const HCCiPhoneDiagnostics::Group &g) {
        if (!l) return;
        lv_label_set_text(l, HCCiPhoneDiagnostics::statusText(g.status));
        lv_obj_set_style_text_color(l, hwStatusColor(g.status), 0);
    };
    set(_hw_sensors_status, _hw_result.sensors);
    set(_hw_faceid_status, _hw_result.faceid);
    set(_hw_baseband_status, _hw_result.baseband);
    set(_hw_battery_status, _hw_result.battery);
    set(_hw_camera_status, _hw_result.camera);
}

// UI task: false (with a message) while ADB is not connected or another job is running.
bool HCCHome::androidJobReady(HCCHome *app)
{
    const HCCAdbClient::State state = app->_adb.state();
    const bool ready = state == HCCAdbClient::State::ReadingInfo || state == HCCAdbClient::State::Ready;
    if (!ready || app->_adb.jobState() == HCCAdbClient::JobState::Running) {
        if (app->_hw_session_label != nullptr) {
            lv_label_set_text(app->_hw_session_label,
                              ready ? "ADB: đang chạy, vui lòng chờ..."
                                    : "ADB: CHƯA SẴN SÀNG (bật gỡ lỗi USB, bấm Cho phép)");
        }
        return false;
    }
    return true;
}

// UI task: hands the full diagnostics job to the USB task.
void HCCHome::startAndroidJob(HCCHome *app)
{
    // Runs as an ADB shell job in the USB task; applyAndroidDiagnostics() shows the result.
    if (!HCCHome::androidJobReady(app)) {
        return;
    }
    app->_hw_android_requested = true;
    if (app->_usb_client_registered && app->_usb_client != nullptr) {
        usb_host_client_unblock(app->_usb_client);
    }
    lv_obj_t *all_labels[] = {app->_hw_sensors_status, app->_hw_faceid_status, app->_hw_baseband_status,
                              app->_hw_battery_status, app->_hw_camera_status};
    for (lv_obj_t *label : all_labels) {
        if (label != nullptr) {
            lv_label_set_text(label, "ĐANG CHẠY...");
            lv_obj_set_style_text_color(label, lv_color_hex(0x7F96A8), 0);
        }
    }
    if (app->_hw_session_label != nullptr) {
        lv_label_set_text(app->_hw_session_label, "ADB: đang chạy chẩn đoán (5-10 giây)...");
    }
}

// ==================== GUIDED CAMERA TEST ====================
//
// UI task state machine. Each step: a job notes the phone clock (and opens the camera app for the
// first front / back step), the technician looks at the phone and taps ĐẠT / LỖI / BỎ QUA on the
// board, then a job reads which camera is open and the camera errors logged since the step began.
// Camera errors turn an ĐẠT into FAIL.

namespace {

// Camera failures worth flagging (HAL / camera service), for grep -iE inside single quotes.
constexpr char kCameraErrorPattern[] =
    "CameraService.*(error|fail)|camera.*(open|connect|config).*(fail|error)|cam_err|probe fail|"
    "ERROR_CAMERA|CameraDevice.*error|camera.*died";

// Keeps only characters a logcat -T timestamp uses ("10-01 10:00:01.000").
std::string sanitizePhoneTime(const std::string &raw)
{
    std::string clean;
    for (char c : raw) {
        if ((c >= '0' && c <= '9') || c == '-' || c == ':' || c == '.' || c == ' ') {
            clean += c;
        }
    }
    while (!clean.empty() && clean.back() == ' ') {
        clean.pop_back();
    }
    return clean.size() >= 14 && clean.size() <= 24 ? clean : "";
}

std::string phoneTimeFrom(const char *output)
{
    const char *found = strstr(output, "PHONETIME=");
    if (found == nullptr) {
        return "";
    }
    found += strlen("PHONETIME=");
    const char *end = strpbrk(found, "\r\n");
    return sanitizePhoneTime(end != nullptr ? std::string(found, end - found) : std::string(found));
}

} // namespace

void HCCHome::requestCameraJob(HCCHome *app, const char *command)
{
    snprintf(app->_cam_job_command, sizeof(app->_cam_job_command), "%s", command);
    app->_cam_job_requested = true;
    if (app->_usb_client_registered && app->_usb_client != nullptr) {
        usb_host_client_unblock(app->_usb_client);
    }
}

void HCCHome::camWizardShow(HCCHome *app, const char *status)
{
    if (app->_cam_panel == nullptr) {
        return;
    }
    const bool in_step = app->_cam_index < app->_cam_steps.size() &&
                         (app->_cam_phase == CamWizardPhase::Starting ||
                          app->_cam_phase == CamWizardPhase::Tapping ||
                          app->_cam_phase == CamWizardPhase::Waiting ||
                          app->_cam_phase == CamWizardPhase::Checking);
    if (in_step) {
        const HCCAndroidDiagnostics::CameraStep &step = app->_cam_steps[app->_cam_index];
        char title[160];
        snprintf(title, sizeof(title), "Bước %u/%u: %s", static_cast<unsigned>(app->_cam_index + 1),
                 static_cast<unsigned>(app->_cam_steps.size()), step.title.c_str());
        lv_label_set_text(app->_cam_title, title);
        lv_label_set_text(app->_cam_text, step.instruction.c_str());
    } else if (app->_cam_phase == CamWizardPhase::Listing) {
        lv_label_set_text(app->_cam_title, "TEST CAMERA");
        lv_label_set_text(app->_cam_text,
                          "Mở khoá màn hình điện thoại và hướng camera vào vật có chi tiết (chữ, đồ vật), đủ sáng.\n"
                          "Board sẽ mở camera trước, rồi lần lượt từng camera sau.");
    }
    lv_label_set_text(app->_cam_status, status);
}

void HCCHome::onHwCameraTestClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr || app->_usb_mode != USBMode::ANDROID || app->_screen == nullptr ||
        app->_cam_panel != nullptr || !HCCHome::androidJobReady(app)) {
        return;
    }

    lv_obj_t *panel = lv_obj_create(app->_screen);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B141C), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    app->_cam_panel = panel;

    app->_cam_title = lv_label_create(panel);
    lv_label_set_long_mode(app->_cam_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(app->_cam_title, 940);
    lv_obj_align(app->_cam_title, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_set_style_text_color(app->_cam_title, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(app->_cam_title, &lv_font_be_vietnam_22, 0);

    app->_cam_text = lv_label_create(panel);
    lv_label_set_long_mode(app->_cam_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(app->_cam_text, 940);
    lv_obj_align(app->_cam_text, LV_ALIGN_TOP_LEFT, 12, 60);
    lv_obj_set_style_text_color(app->_cam_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(app->_cam_text, &lv_font_be_vietnam_22, 0);

    app->_cam_status = lv_label_create(panel);
    lv_label_set_long_mode(app->_cam_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(app->_cam_status, 940);
    lv_obj_align(app->_cam_status, LV_ALIGN_TOP_LEFT, 12, 250);
    lv_obj_set_style_text_color(app->_cam_status, lv_color_hex(0xF2C94C), 0);
    lv_obj_set_style_text_font(app->_cam_status, &lv_font_be_vietnam_18, 0);

    const struct {
        const char *text;
        uint32_t color;
        lv_event_cb_t callback;
    } kButtons[] = {
        {"ĐẠT", 0x2E9E5B, onCamWizardPass},
        {"LỖI", 0xC0392B, onCamWizardFail},
        {"BỎ QUA", 0x5B7286, onCamWizardSkip},
        {"ĐÓNG", 0x2478D0, onCamWizardClose},
    };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *button = lv_btn_create(panel);
        lv_obj_set_size(button, 220, 64);
        lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 4 + i * 240, -8);
        lv_obj_set_style_bg_color(button, lv_color_hex(kButtons[i].color), 0);
        lv_obj_add_event_cb(button, kButtons[i].callback, LV_EVENT_CLICKED, app);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, kButtons[i].text);
        lv_obj_center(label);
        lv_obj_set_style_text_font(label, &lv_font_be_vietnam_22, 0);
    }

    app->_cam_steps.clear();
    app->_cam_results.clear();
    app->_cam_index = 0;
    app->_cam_list_output.clear();
    app->_cam_phase = CamWizardPhase::Listing;
    HCCHome::camWizardShow(app, "Đang đọc danh sách camera từ điện thoại...");
    HCCHome::requestCameraJob(app, HCCAndroidDiagnostics::cameraListCommand());
}

void HCCHome::camWizardBeginStep(HCCHome *app)
{
    const HCCAndroidDiagnostics::CameraStep &step = app->_cam_steps[app->_cam_index];
    app->_cam_phase = CamWizardPhase::Starting;
    app->_cam_step_time.clear();
    app->_cam_note.clear();
    app->_cam_tapped_switch = false;
    app->_cam_zoom_tapped = false;

    // Phone clock first, so the error check covers the camera app launch too; then what is open
    // and which buttons the camera app shows.
    std::string command = "echo \"PHONETIME=$(date +'%m-%d %H:%M:%S.000')\";";
    if (step.launch_facing >= 0) {
        const char *facing = step.launch_facing == 1 ? "1" : "0";
        command += std::string("am start -W -a android.media.action.STILL_IMAGE_CAMERA "
                               "--ei android.intent.extras.CAMERA_FACING ") + facing +
                   " --ei android.intent.extras.LENS_FACING_FRONT " + facing +
                   " --ez android.intent.extra.USE_FRONT_CAMERA " + (step.launch_facing == 1 ? "true" : "false") +
                   " >/dev/null 2>&1;sleep 2;";
    } else {
        command += "sleep 1;";
    }
    command += HCCAndroidDiagnostics::cameraProbeCommand();
    command += "echo '##E'";
    HCCHome::camWizardShow(app, step.launch_facing >= 0 ? "Đang mở camera trên điện thoại..."
                                                        : "Đang tìm nút trong app camera...");
    HCCHome::requestCameraJob(app, command.c_str());
}

void HCCHome::camWizardTap(HCCHome *app, int x, int y)
{
    app->_cam_phase = CamWizardPhase::Tapping;
    char tap[48];
    snprintf(tap, sizeof(tap), "input tap %d %d;sleep 2;", x, y);
    std::string command = tap;
    command += HCCAndroidDiagnostics::cameraProbeCommand();
    command += "echo '##E'";
    HCCHome::camWizardShow(app, (app->_cam_note + "\nĐang chờ app camera chuyển...").c_str());
    HCCHome::requestCameraJob(app, command.c_str());
}

void HCCHome::camWizardWait(HCCHome *app)
{
    app->_cam_phase = CamWizardPhase::Waiting;
    const std::string status = (app->_cam_note.empty() ? std::string() : app->_cam_note + "\n") +
                               "Nhìn điện thoại rồi bấm ĐẠT hoặc LỖI (BỎ QUA nếu không xem được camera này).";
    HCCHome::camWizardShow(app, status.c_str());
}

// After a probe (step start or a tap): make sure the app shows the right side, then the right lens.
// Each button is tapped at most once per step; anything the board cannot do is left to the
// technician with a note.
void HCCHome::camWizardAfterProbe(HCCHome *app, const char *output)
{
    const HCCAndroidDiagnostics::CameraStep &step = app->_cam_steps[app->_cam_index];
    std::string opened;
    std::string errors;
    HCCAndroidDiagnostics::parseCameraCheck(output, &opened, &errors);

    bool side_ok = false;
    for (const std::string &id : step.facing_ids) {
        // opened is "0" or "0, 2": match whole IDs.
        const std::string padded = ", " + opened + ",";
        side_ok = side_ok || padded.find(", " + id + ",") != std::string::npos;
    }
    const char *want = step.front ? "trước" : "sau";
    if (!opened.empty() && !side_ok) {
        if (!app->_cam_tapped_switch) {
            const HCCAndroidDiagnostics::UiTap button = HCCAndroidDiagnostics::findSwitchCameraButton(output);
            app->_cam_tapped_switch = true;
            if (button.found) {
                app->_cam_note = "Board đã bấm nút đổi camera ('" + button.label + "').";
                HCCHome::camWizardTap(app, button.x, button.y);
                return;
            }
            app->_cam_note = std::string("App đang mở nhầm camera, board không tìm thấy nút đổi camera: tự đổi sang "
                                         "camera ") + want + " trên điện thoại.";
        } else {
            app->_cam_note += std::string(" App vẫn chưa sang camera ") + want + ": tự đổi trên điện thoại.";
        }
        HCCHome::camWizardWait(app);
        return;
    }
    if (opened.empty() && app->_cam_note.empty()) {
        app->_cam_note = "Không thấy camera nào đang mở: app camera có đang hiện trên điện thoại không (màn hình "
                         "khoá / hỏi quyền)?";
    }

    if (step.zoom > 0 && !app->_cam_zoom_tapped) {
        app->_cam_zoom_tapped = true;
        const HCCAndroidDiagnostics::UiTap button =
            HCCAndroidDiagnostics::findZoomButton(output, step.zoom, step.tele_rank);
        if (button.found) {
            app->_cam_note += (app->_cam_note.empty() ? "" : " ") + std::string("Board đã bấm '") + button.label + "'.";
            HCCHome::camWizardTap(app, button.x, button.y);
            return;
        }
        if (step.zoom != 1) {
            char zoom[16];
            snprintf(zoom, sizeof(zoom), "%.1fx", step.zoom);
            app->_cam_note += (app->_cam_note.empty() ? "" : " ") +
                              std::string("Không tìm thấy nút ") + zoom + " trong app: tự bấm / zoom trên điện thoại.";
        }
    }
    HCCHome::camWizardWait(app);
}

void HCCHome::camWizardNext(HCCHome *app)
{
    ++app->_cam_index;
    if (app->_cam_index < app->_cam_steps.size()) {
        HCCHome::camWizardBeginStep(app);
    } else {
        HCCHome::camWizardFinish(app);
    }
}

void HCCHome::camWizardFinish(HCCHome *app)
{
    app->_cam_phase = CamWizardPhase::Finishing;
    HCCHome::camWizardShow(app, "Đang đóng app camera trên điện thoại...");
    HCCHome::requestCameraJob(app, "input keyevent KEYCODE_HOME;echo '##E'");
}

void HCCHome::camWizardVerdict(HCCHome *app, int verdict)
{
    if (app->_cam_phase != CamWizardPhase::Waiting) {
        return;
    }
    if (verdict < 0) {
        HCCAndroidDiagnostics::CameraStepResult result;
        result.title = app->_cam_steps[app->_cam_index].title;
        result.verdict = -1;
        app->_cam_results.push_back(result);
        HCCHome::camWizardNext(app);
        return;
    }
    app->_cam_verdict = verdict;
    app->_cam_phase = CamWizardPhase::Checking;
    // Errors since the step began (or the last lines when the phone clock was not read).
    std::string command = "echo '##O';dumpsys media.camera|grep -iE 'device [0-9]+ is open'|head -4|cut -c1-100;"
                          "echo '##Y';logcat -d ";
    command += app->_cam_step_time.empty() ? std::string("-t 300") : "-T \"" + app->_cam_step_time + "\"";
    command += std::string(" 2>/dev/null|grep -iE '") + kCameraErrorPattern + "'|tail -5|cut -c1-160;echo '##E'";
    HCCHome::camWizardShow(app, "Đang đọc log lỗi camera...");
    HCCHome::requestCameraJob(app, command.c_str());
}

void HCCHome::camWizardJobDone(HCCHome *app, bool ok)
{
    if (app->_cam_panel == nullptr || app->_cam_phase == CamWizardPhase::Idle ||
        app->_cam_phase == CamWizardPhase::Done) {
        return; // closed or cancelled meanwhile
    }
    if (!ok) {
        app->_cam_phase = CamWizardPhase::Done;
        HCCHome::camWizardShow(app, "Lỗi ADB: lệnh không chạy xong (điện thoại rút ra?). Bấm ĐÓNG rồi thử lại.");
        return;
    }
    const char *output = app->_adb.jobOutput();
    switch (app->_cam_phase) {
    case CamWizardPhase::Listing:
        app->_cam_list_output = output;
        app->_cam_steps = HCCAndroidDiagnostics::cameraSteps(output);
        if (app->_cam_steps.empty()) {
            app->_cam_phase = CamWizardPhase::Done;
            HCCHome::camWizardShow(app, "Không đọc được danh sách camera từ điện thoại. Chạy RUN FULL TEST để xem "
                                        "thẻ CAMERA.");
            return;
        }
        app->_cam_index = 0;
        HCCHome::camWizardBeginStep(app);
        return;
    case CamWizardPhase::Starting:
        app->_cam_step_time = phoneTimeFrom(output);
        HCCHome::camWizardAfterProbe(app, output);
        return;
    case CamWizardPhase::Tapping:
        HCCHome::camWizardAfterProbe(app, output);
        return;
    case CamWizardPhase::Checking: {
        HCCAndroidDiagnostics::CameraStepResult result;
        result.title = app->_cam_steps[app->_cam_index].title;
        result.verdict = app->_cam_verdict;
        HCCAndroidDiagnostics::parseCameraCheck(output, &result.opened, &result.errors);
        app->_cam_results.push_back(result);
        HCCHome::camWizardNext(app);
        return;
    }
    case CamWizardPhase::Finishing: {
        // Only the CAMERA card changes; the other cards keep their results.
        if (!app->_hw_has_result || !app->_hw_result_android) {
            app->_hw_result = HCCAndroidDiagnostics::parse("");
        }
        app->_hw_result.camera =
            HCCAndroidDiagnostics::cameraTestGroup(app->_cam_list_output.c_str(), app->_cam_results);
        app->_hw_result_android = true;
        app->_hw_has_result = true;
        app->updateHwCards();

        // The first items are the steps; the camera list follows them.
        unsigned pass = 0, fail = 0, skipped = 0;
        const auto &items = app->_hw_result.camera.items;
        for (size_t i = 0; i < app->_cam_results.size() && i < items.size(); ++i) {
            pass += items[i].status == HCCiPhoneDiagnostics::Status::Pass;
            fail += items[i].status == HCCiPhoneDiagnostics::Status::Fail;
            skipped += items[i].status == HCCiPhoneDiagnostics::Status::NotTested;
        }
        char summary[200];
        snprintf(summary, sizeof(summary),
                 "Xong %u camera: %u ĐẠT, %u LỖI, %u bỏ qua.\nBấm ĐÓNG rồi chạm thẻ CAMERA để xem chi tiết.",
                 static_cast<unsigned>(app->_cam_results.size()), pass, fail, skipped);
        app->_cam_phase = CamWizardPhase::Done;
        lv_label_set_text(app->_cam_title, "KẾT QUẢ TEST CAMERA");
        lv_label_set_text(app->_cam_text, summary);
        lv_label_set_text(app->_cam_status, "");
        if (app->_hw_session_label != nullptr) {
            lv_label_set_text(app->_hw_session_label, "ADB: test camera XONG - chạm thẻ CAMERA để xem");
        }
        return;
    }
    default:
        return;
    }
}

void HCCHome::camWizardClose(HCCHome *app)
{
    if (app->_cam_panel != nullptr) {
        lv_obj_del(app->_cam_panel);
    }
    app->_cam_panel = nullptr;
    app->_cam_title = nullptr;
    app->_cam_text = nullptr;
    app->_cam_status = nullptr;
    // A job still running reports into Idle and is ignored.
    app->_cam_phase = CamWizardPhase::Idle;
}

void HCCHome::onCamWizardPass(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        HCCHome::camWizardVerdict(app, 1);
    }
}

void HCCHome::onCamWizardFail(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        HCCHome::camWizardVerdict(app, 0);
    }
}

void HCCHome::onCamWizardSkip(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        HCCHome::camWizardVerdict(app, -1);
    }
}

void HCCHome::onCamWizardClose(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        HCCHome::camWizardClose(app);
    }
}

void HCCHome::onHwRunFullClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (!app) return;
    if (app->_usb_mode == USBMode::ANDROID) {
        HCCHome::startAndroidJob(app);
        return;
    }
    HCCiPhoneDiagnostics diag(&app->_usb_transport);
    app->_hw_result = diag.runFullCheck();
    app->_hw_result_android = false;
    app->_hw_has_result = true;
    app->updateHwCards();
}

void HCCHome::onHwCardClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (!app || app->_screen == nullptr || app->_hw_dialog != nullptr) return;
    if (!app->_hw_has_result) {
        return;  // chưa chạy test
    }
    lv_obj_t *card = lv_event_get_target(event);
    intptr_t idx = reinterpret_cast<intptr_t>(lv_obj_get_user_data(card));
    const HCCiPhoneDiagnostics::Group *g = nullptr;
    if (idx == 0) g = &app->_hw_result.sensors;
    else if (idx == 1) g = &app->_hw_result.faceid;
    else if (idx == 2) g = &app->_hw_result.baseband;
    else if (idx == 3) g = &app->_hw_result.battery;
    else g = &app->_hw_result.camera;

    // Dựng nội dung evidence.
    std::string body;
    for (const auto &it : g->items) {
        body += it.name + "  [" + HCCiPhoneDiagnostics::statusText(it.status) + "]\n";
        if (!it.source.empty())   body += "  Nguồn: " + it.source + "\n";
        if (!it.evidence.empty()) body += "  Evidence: " + it.evidence + "\n";
        if (!it.code.empty())     body += "  Code: " + it.code + "\n";
        if (it.ts != 0) {
            time_t t = (time_t)it.ts;
            struct tm tmv;
            localtime_r(&t, &tmv);
            char ts[24];
            strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
            body += std::string("  Thời điểm: ") + ts + "\n";
        }
        body += "\n";
    }
    if (body.empty()) body = "(không có dữ liệu)";

    app->_hw_dialog = lv_obj_create(app->_screen);
    lv_obj_set_size(app->_hw_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(app->_hw_dialog);
    lv_obj_set_style_bg_color(app->_hw_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(app->_hw_dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(app->_hw_dialog, 0, 0);
    lv_obj_clear_flag(app->_hw_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(app->_hw_dialog);
    lv_obj_set_size(panel, 900, 500);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B141C), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x304553), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dtitle = lv_label_create(panel);
    lv_label_set_text(dtitle, g->name.c_str());
    lv_obj_align(dtitle, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_text_color(dtitle, lv_color_hex(0x00D9FF), 0);
    lv_obj_set_style_text_font(dtitle, &lv_font_be_vietnam_18, 0);

    lv_obj_t *close_btn = lv_btn_create(panel);
    lv_obj_set_size(close_btn, 110, 42);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_add_event_cb(close_btn, onHwDialogCloseClicked, LV_EVENT_CLICKED, app);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "ĐÓNG");
    lv_obj_center(cl);
    lv_obj_set_style_text_font(cl, &lv_font_be_vietnam_16, 0);

    lv_obj_t *content = lv_obj_create(panel);
    lv_obj_set_size(content, 860, 420);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x05090D), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);

    lv_obj_t *lbl = lv_label_create(content);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_text(lbl, body.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xCFE3EF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_be_vietnam_16, 0);
}

void HCCHome::onHwDialogCloseClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app && app->_hw_dialog) {
        lv_obj_del(app->_hw_dialog);
        app->_hw_dialog = nullptr;
    }
}

void HCCHome::showHardwareCheck()
{
    if (_screen == nullptr) return;

    detachUsbLabels();
    detachHwLabels();

    const bool android = _usb_mode == USBMode::ANDROID;
    // Never show an iPhone result for an Android phone or the other way round.
    if (_hw_has_result && _hw_result_android != android) {
        _hw_has_result = false;
    }

    lv_obj_clean(_screen);
    lv_obj_set_style_bg_color(_screen, lv_color_hex(0x101820), 0);

    lv_obj_t *title = lv_label_create(_screen);
    lv_label_set_text(title, android ? "ANDROID HARDWARE CHECK" : "IPHONE HARDWARE CHECK");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 18);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_be_vietnam_28, 0);

    // Trạng thái phiên.
    lv_obj_t *sess = lv_label_create(_screen);
    _hw_session_label = sess;
    bool ready = false;
    if (android) {
        const HCCAdbClient::State adb_state = _adb.state();
        ready = adb_state == HCCAdbClient::State::ReadingInfo || adb_state == HCCAdbClient::State::Ready;
        lv_label_set_text(sess, ready ? "ADB: SẴN SÀNG" : "ADB: CHƯA SẴN SÀNG (bật gỡ lỗi USB, bấm Cho phép)");
    } else {
        ready = _usb_transport.sessionEstablished() && _usb_transport.deviceInfoReady();
        lv_label_set_text(sess, ready ? "Phiên Lockdown: SẴN SÀNG" : "Phiên Lockdown: CHƯA SẴN SÀNG (cắm & mở khoá máy)");
    }
    lv_obj_align(sess, LV_ALIGN_TOP_LEFT, 30, 58);
    lv_obj_set_style_text_color(sess, ready ? lv_color_hex(0x69E0C0) : lv_color_hex(0xF2C94C), 0);
    lv_obj_set_style_text_font(sess, &lv_font_be_vietnam_18, 0);

    // iPhone: SENSORS | FACE ID | BASEBAND | BATTERY; Android adds CAMERA (5 narrower cards).
    const char *iphone_names[] = {"SENSORS", "FACE ID", "BASEBAND", "BATTERY"};
    const char *android_names[] = {"SENSORS", "VÂN TAY / FACE", "BASEBAND / SIM", "BATTERY", "CAMERA"};
    const char *const *names = android ? android_names : iphone_names;
    lv_obj_t **statusLabels[] = {&_hw_sensors_status, &_hw_faceid_status,
                                 &_hw_baseband_status, &_hw_battery_status, &_hw_camera_status};
    const int card_count = android ? 5 : 4;
    const int card_width = android ? 184 : 232;
    const int card_step = android ? 196 : 246;
    for (int i = 0; i < card_count; ++i) {
        lv_obj_t *card = lv_obj_create(_screen);
        lv_obj_set_size(card, card_width, 200);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, 28 + i * card_step, 100);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x182630), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x304553), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(card, onHwCardClicked, LV_EVENT_CLICKED, this);

        lv_obj_t *cn = lv_label_create(card);
        lv_label_set_text(cn, names[i]);
        lv_label_set_long_mode(cn, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(cn, card_width - 32);
        lv_obj_align(cn, LV_ALIGN_TOP_LEFT, 16, 14);
        lv_obj_set_style_text_color(cn, lv_color_hex(0xB8C5CF), 0);
        lv_obj_set_style_text_font(cn, &lv_font_be_vietnam_18, 0);

        lv_obj_t *st = lv_label_create(card);
        lv_label_set_text(st, "NOT TESTED");
        lv_obj_align(st, LV_ALIGN_LEFT_MID, 16, 6);
        lv_obj_set_style_text_color(st, lv_color_hex(0x7F96A8), 0);
        lv_obj_set_style_text_font(st, &lv_font_be_vietnam_22, 0);
        *statusLabels[i] = st;

        lv_obj_t *hint = lv_label_create(card);
        lv_label_set_text(hint, android ? "Chạm để xem" : "Chạm để xem evidence");
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 16, -14);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x5B7286), 0);
        lv_obj_set_style_text_font(hint, &lv_font_be_vietnam_16, 0);
    }
    if (_hw_has_result) updateHwCards();

    // RUN FULL TEST
    lv_obj_t *run_btn = lv_btn_create(_screen);
    lv_obj_set_size(run_btn, 300, 56);
    lv_obj_align(run_btn, LV_ALIGN_TOP_LEFT, 28, 330);
    lv_obj_set_style_bg_color(run_btn, lv_color_hex(0x2E9E5B), 0);
    lv_obj_add_event_cb(run_btn, onHwRunFullClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *run_lbl = lv_label_create(run_btn);
    lv_label_set_text(run_lbl, "RUN FULL TEST");
    lv_obj_center(run_lbl);
    lv_obj_set_style_text_font(run_lbl, &lv_font_be_vietnam_18, 0);

    // Android: guided test of every camera (the camera list alone does not prove a module works).
    if (android) {
        lv_obj_t *cam_btn = lv_btn_create(_screen);
        lv_obj_set_size(cam_btn, 300, 56);
        lv_obj_align(cam_btn, LV_ALIGN_TOP_LEFT, 348, 330);
        lv_obj_set_style_bg_color(cam_btn, lv_color_hex(0x2478D0), 0);
        lv_obj_add_event_cb(cam_btn, onHwCameraTestClicked, LV_EVENT_CLICKED, this);
        lv_obj_t *cam_lbl = lv_label_create(cam_btn);
        lv_label_set_text(cam_lbl, "TEST CAMERA");
        lv_obj_center(cam_lbl);
        lv_obj_set_style_text_font(cam_lbl, &lv_font_be_vietnam_18, 0);
    }

    // Ghi chú giới hạn.
    lv_obj_t *note = lv_label_create(_screen);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 940);
    lv_label_set_text(note, android ?
        "SENSORS/VÂN TAY/CAMERA: so sánh linh kiện máy khai báo với linh kiện hệ thống thực sự thấy; "
        "khai báo mà không thấy -> FAIL/CHECK (nghi hỏng IC, cáp). PASS = linh kiện lên, chưa đo chức năng. "
        "BASEBAND: modem, IMEI, SIM, mạng, sóng, băng tần. BATTERY: % chai, chu kỳ, sạc, dòng, nhiệt độ." :
        "SENSORS: iOS không cấp dữ liệu sensor qua Lockdown -> UNSUPPORTED. "
        "FACE ID: chỉ dò lỗi subsystem qua os_trace (không bao giờ PASS, không đụng sinh trắc). "
        "BASEBAND: dùng Lockdown GetValue thật + log CommCenter làm evidence.");
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 28, 400);
    lv_obj_set_style_text_color(note, lv_color_hex(0x8AA0B4), 0);
    lv_obj_set_style_text_font(note, &lv_font_be_vietnam_16, 0);

    // Back về USB HUB.
    lv_obj_t *back_button = lv_btn_create(_screen);
    lv_obj_set_size(back_button, 160, 52);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_RIGHT, -28, -22);
    lv_obj_add_event_cb(back_button, onHwBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, "BACK");
    lv_obj_center(back_label);
    lv_obj_set_style_text_font(back_label, &lv_font_be_vietnam_18, 0);
}
