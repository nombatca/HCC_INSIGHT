#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "HCCPairCredentials.hpp"
#include "HCCPairIdentity.hpp"
#include "HCCUSBTransport.hpp"
class HCCHome : public ESP_Brookesia_PhoneApp {
private:
    enum class USBMode {
        NO_DEVICE,
        NORMAL,
        RECOVERY,
        DFU,
        PURPLE,
        UNKNOWN
    };

    enum class USBStringDescriptorRequest {
        None,
        LangId,
        Serial,
    };

public:
    HCCHome();
    ~HCCHome() override = default;

    bool init(void) override;
    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    static void onI2CClicked(lv_event_t *event);
    static void onUSBClicked(lv_event_t *event);
    static void usbClientTask(void *arg);
    static void onUSBClientEvent(const usb_host_client_event_msg_t *event_msg, void *arg);
    static void processUSBNewDevice(HCCHome *app);
    static void onUSBGetModeComplete(usb_transfer_t *transfer);
    static bool submitUSBGetMode(HCCHome *app);
    static void onUSBStringDescriptorComplete(usb_transfer_t *transfer);
    static bool submitUSBStringDescriptor(HCCHome *app, USBStringDescriptorRequest request,
                                          uint8_t string_index, uint16_t lang_id);
    static void failUSBStringDescriptorStartup(HCCHome *app, const char *reason);
    static bool handoffDevicePublicKeyToCredentials(void *context, const uint8_t *data, size_t length);
    static void consumeCompletedCredentialBundle(HCCHome *app);
    static void continueUSBMuxStartup(HCCHome *app);
    static bool findAppleUSBMuxInterface(HCCHome *app);
    static bool claimUSBInterface(HCCHome *app);
    static bool releaseUSBInterface(HCCHome *app);
    static bool closeUSBDevice(HCCHome *app);
    static USBMode detectUSBMode(uint16_t vid, uint16_t pid);
    static void applyUSBResult(void *arg);
    static void onModuleClicked(lv_event_t *event);
    static void onBackClicked(lv_event_t *event);
    static void onRecoveryClicked(lv_event_t *event);

    static void onScanClicked(lv_event_t *event);

    static void scanTask(void *arg);
    void performScan();
    i2c_master_bus_handle_t _external_i2c_bus;
    static void applyScanResult(void *arg);

    void requestUSBClientShutdown();

    void showHome();
    void showI2C();
    void showUSB();

    lv_obj_t *_screen;
    lv_obj_t *_previous_screen;

    lv_obj_t *_scan_status_label;
    lv_obj_t *_scan_result_label;
    lv_obj_t *_scan_button;

    lv_obj_t *_usb_status_label;
    lv_obj_t *_usb_info_label;
    lv_obj_t *_usb_mode_label;

    usb_host_client_handle_t _usb_client;
    usb_device_handle_t _usb_device;
    TaskHandle_t _usb_client_task;
    bool _usb_client_registered;
    bool _usb_interface_claimed;
    uint8_t _usb_mux_interface;
    uint8_t _usb_mux_bulk_out;
    uint8_t _usb_mux_bulk_in;
    uint16_t _usb_mux_bulk_in_mps;
    volatile bool _usb_shutdown_requested;
    bool _usb_device_gone_pending;
    uint8_t _usb_new_device_address;
    bool _usb_new_device_pending;
    // Rising-edge guard so the encrypted-session info is rendered once per connection.
    bool _usb_session_reported;
    // Two-tap confirmation state for the RECOVERY action.
    bool _recovery_confirm_pending;
    uint32_t _recovery_confirm_tick;
    usb_transfer_t *_usb_get_mode_transfer;
    bool _usb_get_mode_pending;
    usb_transfer_t *_usb_string_descriptor_transfer;
    bool _usb_string_descriptor_pending;
    USBStringDescriptorRequest _usb_string_descriptor_request;
    uint8_t _usb_i_serial_number;
    uint32_t _usb_generation;
    uint32_t _usb_udid_generation;
    // USB string bLength is one byte: at most 126 UTF-16LE code units,
    // plus the optional 24-character normalization hyphen and trailing NUL.
    char _usb_udid[128];
    HCCPairIdentity _pair_identity;
    HCCPairCredentials _pair_credentials;
    HCCUSBTransport _usb_transport;
    USBMode _usb_mode;
    uint16_t _usb_vid;
    uint16_t _usb_pid;
    char _usb_status[64];
    char _usb_info[384];

    volatile bool _scan_scanning;

    char _scan_result[512];
    char _scan_status[64];
};
