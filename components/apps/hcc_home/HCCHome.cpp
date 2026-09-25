#include "HCCHome.hpp"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "driver/i2c_master.h"
#include <stdio.h>
#include <string.h>
LV_IMG_DECLARE(img_app_hcc_insight);

static void usbStringToAscii(
    const usb_str_desc_t *desc,
    char *out,
    size_t out_size
)
{
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';

    if (desc == nullptr || desc->bLength < 2) {
        snprintf(out, out_size, "N/A");
        return;
    }

    size_t char_count = (desc->bLength - 2) / 2;
    if (char_count >= out_size) {
        char_count = out_size - 1;
    }

    for (size_t i = 0; i < char_count; ++i) {
        uint16_t ch = desc->wData[i];

        if (ch >= 0x20 && ch <= 0x7E) {
            out[i] = (char)ch;
        } else {
            out[i] = '?';
        }
    }

    out[char_count] = '\0';
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
      _usb_mode(USBMode::NO_DEVICE),
      _usb_vid(0),
      _usb_pid(0),
      _scan_scanning(false)
{
    _usb_status[0] = '\0';
    _usb_udid[0] = '\0';
    _usb_info[0] = '\0';
    _scan_result[0] = '\0';
    _scan_status[0] = '\0';
}
bool HCCHome::init(void)
{
    // Non-fatal: pair record access falls back to lazy initialization.
    if (!HCCUSBTransport::initPairRecordStorage()) {
        printf("[HCC PAIR] early pair record storage init failed; using lazy init\n");
    }
    return _pair_identity.initialize();
}

bool HCCHome::run(void)
{
    _screen = lv_scr_act();

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
        onModuleClicked,
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
        onModuleClicked,
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
                                          app->_pair_credentials.hasDeferredWork();
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

        // Once the encrypted lockdown session is up, surface the device as trusted/ready
        // and append any lockdown-derived info captured this connection. Runs in this task,
        // which owns _usb_info, so the transport strings are read where they are written.
        if (!app->_usb_session_reported && app->_usb_transport.sessionEstablished()) {
            app->_usb_session_reported = true;
            const char *ios = app->_usb_transport.productVersion();
            const char *wifi = app->_usb_transport.wifiAddress();
            size_t len = strnlen(app->_usb_info, sizeof(app->_usb_info));
            if (ios[0] != '\0' && len < sizeof(app->_usb_info)) {
                len += snprintf(app->_usb_info + len, sizeof(app->_usb_info) - len, "\niOS: %s", ios);
            }
            if (wifi[0] != '\0' && len < sizeof(app->_usb_info)) {
                snprintf(app->_usb_info + len, sizeof(app->_usb_info) - len, "\nWi-Fi: %s", wifi);
            }
            snprintf(app->_usb_status, sizeof(app->_usb_status), "SESSION READY - iPhone trusted");
            lv_async_call(HCCHome::applyUSBResult, app);
        }

        // UI-requested recovery-mode exit: start the iBoot command sequence from this task.
        if (app->_recovery_exit_requested && !app->_recovery_exit_active &&
            app->_usb_device != nullptr) {
            app->_recovery_exit_requested = false;
            app->_recovery_exit_active = true;
            app->_recovery_exit_step = 0;
            HCCHome::submitRecoveryExitCommand(app);
        }

        if (app->_usb_new_device_pending) {
            HCCHome::processUSBNewDevice(app);
        }

        if (app->_usb_device_gone_pending &&
            !app->_usb_get_mode_pending &&
            app->_usb_get_mode_transfer == nullptr &&
            !app->_usb_string_descriptor_pending &&
            app->_usb_string_descriptor_transfer == nullptr &&
            !app->_usb_transport.hasPendingTransfers()) {
            if (HCCHome::closeUSBDevice(app)) {
                app->_usb_device_gone_pending = false;
            }
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
    while (app->_usb_transport.hasPendingTransfers()) {
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

    // The RECOVERY button enters recovery in normal mode and exits it in recovery mode.
    if (app->_recovery_button_label != nullptr) {
        lv_label_set_text(app->_recovery_button_label,
                          app->_usb_mode == USBMode::RECOVERY ? "EXIT REC" : "RECOVERY");
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

        ret = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
        if (ret == ESP_OK && dev_desc != nullptr) {
            usb_host_device_info(dev_hdl, &dev_info);

            const usb_config_desc_t *config_desc = nullptr;
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

            const char *speed = "UNKNOWN";
            if (dev_info.speed == USB_SPEED_LOW) {
                speed = "LOW";
            } else if (dev_info.speed == USB_SPEED_FULL) {
                speed = "FULL";
            } else if (dev_info.speed == USB_SPEED_HIGH) {
                speed = "HIGH";
            }

            char manufacturer[64];
            char product[64];
            char serial[64];

            usbStringToAscii(
                dev_info.str_desc_manufacturer,
                manufacturer,
                sizeof(manufacturer)
            );

            usbStringToAscii(
                dev_info.str_desc_product,
                product,
                sizeof(product)
            );

            usbStringToAscii(
                dev_info.str_desc_serial_num,
                serial,
                sizeof(serial)
            );

            snprintf(
                app->_usb_status,
                sizeof(app->_usb_status),
                "USB DEVICE DETECTED"
            );

            // Compact identity so the encrypted-session lines (iOS, Wi-Fi) also fit the panel.
            snprintf(
                app->_usb_info,
                sizeof(app->_usb_info),
                "VID:PID  %04X:%04X\nProduct: %s\nSerial: %s",
                dev_desc->idVendor,
                dev_desc->idProduct,
                product,
                serial
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

        if (app->_usb_vid == 0x05AC) {
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
        if (event_msg->dev_gone.dev_hdl == app->_usb_device) {
            ++app->_usb_generation;
            if (app->_usb_generation == 0) {
                ++app->_usb_generation;
            }
            app->_pair_credentials.setActiveGeneration(app->_usb_generation);
            app->_usb_udid_generation = 0;
            memset(app->_usb_udid, 0, sizeof(app->_usb_udid));
            app->_usb_transport.stopForDeviceGone();
            app->_usb_device_gone_pending = true;
        }
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

void HCCHome::onRecoveryClicked(lv_event_t *event)
{
    HCCHome *app = static_cast<HCCHome *>(lv_event_get_user_data(event));
    if (app == nullptr) {
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
    requestUSBClientShutdown();
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

        // Only RECOVERY is wired for now; the other actions remain inert placeholders.
        if (i == 0) {
            _recovery_button_label = label;
            lv_obj_add_event_cb(button, onRecoveryClicked, LV_EVENT_CLICKED, this);
        }
    }
    _recovery_confirm_pending = false;

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

    _usb_status_label = nullptr;
    _usb_info_label = nullptr;

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
