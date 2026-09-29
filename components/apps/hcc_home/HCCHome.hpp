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
#include "HCCAdbClient.hpp"
#include "HCCFastbootClient.hpp"
#include "HCCEeprom.hpp"
#include "HCCPanicTypes.hpp"
#include "HCCiPhoneDiagnostics.hpp"
class HCCHome : public ESP_Brookesia_PhoneApp {
private:
    enum class USBMode {
        NO_DEVICE,
        NORMAL,
        RECOVERY,
        DFU,
        PURPLE,
        ANDROID,
        ANDROID_FASTBOOT,
        ANDROID_DOWNLOAD,
        QUALCOMM_EDL,
        MTK_BOOTROM,
        UNKNOWN
    };

    enum class AndroidAction : uint8_t {
        None,
        Reboot,
        Recovery,
        Bootloader,
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
    static void describeNonAppleDevice(HCCHome *app, const usb_config_desc_t *config_desc,
                                       const usb_device_info_t &dev_info);
    static void startAndroidSession(HCCHome *app);
    static void applyAdbState(HCCHome *app);
    static void applyFastbootState(HCCHome *app);
    static void updateActionLabels(HCCHome *app);
    static bool requestAndroidAction(HCCHome *app, AndroidAction action);
    static void performAndroidAction(HCCHome *app, AndroidAction action);
    static void onBootloaderClicked(lv_event_t *event);
    static void onRebootClicked(lv_event_t *event);
    static void applyUSBResult(void *arg);
    static void onModuleClicked(lv_event_t *event);
    static void onBackClicked(lv_event_t *event);
    static void onRecoveryClicked(lv_event_t *event);
    // IPHONE HARDWARE CHECK (trong màn USB HUB).
    static void onHardwareCheckClicked(lv_event_t *event);
    void showHardwareCheck();
    void updateHwCards();
    static void onHwRunFullClicked(lv_event_t *event);
    static void onHwCardClicked(lv_event_t *event);
    static void onHwDialogCloseClicked(lv_event_t *event);
    static void onHwBackClicked(lv_event_t *event);
    // Recovery-mode exit: send iBoot commands (setenv auto-boot true / saveenv / reboot).
    static bool submitRecoveryExitCommand(HCCHome *app);
    static void onRecoveryExitComplete(usb_transfer_t *transfer);

    static void onScanClicked(lv_event_t *event);

    static void scanTask(void *arg);
    void performScan();
    i2c_master_bus_handle_t _external_i2c_bus;
    static void applyScanResult(void *arg);

    // EEPROM module: đọc EEPROM I2C và lưu bản dump ra thẻ SD.
    static void onEepromClicked(lv_event_t *event);
    static void onEepromDumpClicked(lv_event_t *event);
    static void onEepromProgramClicked(lv_event_t *event);
    static void onEepromFileChanged(lv_event_t *event);
    static void onEepromRefreshClicked(lv_event_t *event);
    static void onEepromViewClicked(lv_event_t *event);
    static void onEepromDialogCloseClicked(lv_event_t *event);
    void populateEepromFileList();
    void showEeprom();
    void showBinaryDialog(const uint8_t *data, size_t len);
    static void eepromTask(void *arg);
    static void eepromProgramTask(void *arg);
    static void eepromViewTask(void *arg);
    void performEepromDump();
    void performEepromProgram();
    void performEepromView();
    void beginEepromOp(const char *status);
    static void applyEepromResult(void *arg);
    static void applyEepromView(void *arg);
    i2c_master_bus_handle_t ensureI2CBus();

    // PANIC & AI ANALYZE module.
    static void onPanicClicked(lv_event_t *event);
    static void onPanicAnalyzeClicked(lv_event_t *event);
    static void onPanicRefreshClicked(lv_event_t *event);
    static void onPanicFileChanged(lv_event_t *event);
    static void onPanicApiKeyClicked(lv_event_t *event);
    static void onPanicApiKeySaveClicked(lv_event_t *event);
    static void onPanicApiKeyCloseClicked(lv_event_t *event);
    // Xem nội dung RAW panic của file đang chọn.
    static void onPanicViewClicked(lv_event_t *event);
    static void onPanicViewCloseClicked(lv_event_t *event);
    // CONFIRM REPAIR / VERIFY (xây Verified KB từ sửa chữa thực tế).
    static void onConfirmRepairClicked(lv_event_t *event);
    static void onRepairSaveClicked(lv_event_t *event);
    static void onRepairVerifyClicked(lv_event_t *event);
    static void onRepairCloseClicked(lv_event_t *event);
    static void onRepairFieldFocused(lv_event_t *event);
    void openConfirmRepair();
    bool submitRepairCase(bool verified);
    void showPanic();
    void populatePanicFileList();
    // Đọc raw panic của mục đang chọn: từ RAM (mục "[RAM] ...") hoặc file SD.
    bool loadSelectedRaw(std::string &raw);
    // Layout mới: cập nhật panel chi tiết (thiết bị, mức độ lỗi, key panic string).
    void refreshPanicDetail();
    static void onPanicListItemClicked(lv_event_t *event);
    static void onPanicListItemDeleted(lv_event_t *event);
    static void onPanicChooseFileClicked(lv_event_t *event);
    static void onPanicDeleteFileClicked(lv_event_t *event);
    static void onPanicExportClicked(lv_event_t *event);
    static void panicAnalyzeTask(void *arg);
    void performPanicAnalyze();
    static void applyPanicResult(void *arg);

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
    // App closed while a device is still attached: defer client shutdown until it is removed,
    // so the host stack never frees a present device (which later aborts in the hub driver).
    bool _usb_close_pending;
    // Two-tap confirmation state for the RECOVERY action.
    bool _recovery_confirm_pending;
    uint32_t _recovery_confirm_tick;
    // Recovery-mode exit sequence state.
    volatile bool _recovery_exit_requested;
    bool _recovery_exit_active;
    uint8_t _recovery_exit_step;
    usb_transfer_t *_recovery_exit_transfer;
    lv_obj_t *_recovery_button_label;
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
    // Android: ADB (normal mode) and fastboot (bootloader) clients.
    HCCAdbClient _adb;
    uint32_t _adb_state_seen = 0;
    HCCFastbootClient _fastboot;
    uint32_t _fastboot_state_seen = 0;
    // Set by the UI (after a confirming second tap), consumed by the USB client task.
    volatile AndroidAction _android_action_requested = AndroidAction::None;
    AndroidAction _android_confirm_action = AndroidAction::None;
    lv_obj_t *_bootloader_button_label = nullptr;
    lv_obj_t *_purple_button_label = nullptr;
    lv_obj_t *_reboot_button_label = nullptr;
    // ADB or fastboot interface found by describeNonAppleDevice().
    uint8_t _android_interface = 0;
    uint8_t _android_bulk_out = 0;
    uint8_t _android_bulk_in = 0;
    uint16_t _android_in_mps = 0;
    uint16_t _android_out_mps = 0;
    char _usb_desc_serial[48] = {};
    // Hardware Check UI state.
    lv_obj_t *_hw_sensors_status;
    lv_obj_t *_hw_faceid_status;
    lv_obj_t *_hw_baseband_status;
    lv_obj_t *_hw_battery_status;
    lv_obj_t *_hw_dialog;
    HCCiPhoneDiagnostics::FullResult _hw_result;
    bool _hw_has_result;
    USBMode _usb_mode;
    uint16_t _usb_vid;
    uint16_t _usb_pid;
    char _usb_status[64];
    char _usb_info[384];

    volatile bool _scan_scanning;

    char _scan_result[512];
    char _scan_status[64];

    // EEPROM screen state.
    lv_obj_t *_eeprom_status_label;
    lv_obj_t *_eeprom_result_label;
    lv_obj_t *_eeprom_button;
    lv_obj_t *_eeprom_program_button;
    lv_obj_t *_eeprom_file_dropdown;
    lv_obj_t *_eeprom_dialog;
    // File trên thẻ SD đang chọn: đích khi ĐỌC, nguồn khi GHI.
    char _eeprom_file[64];
    // Buffer tạm giữ nội dung file để dựng hex dump trên luồng LVGL.
    uint8_t *_eeprom_view_buf;
    size_t _eeprom_view_len;

    // PANIC screen state.
    lv_obj_t *_panic_file_dropdown;
    lv_obj_t *_panic_status_label;
    lv_obj_t *_panic_url_label;
    lv_obj_t *_panic_analyze_button;
    lv_obj_t *_panic_error_label;
    lv_obj_t *_panic_cause_label;
    lv_obj_t *_panic_repair_label;
    lv_obj_t *_panic_conf_label;
    lv_obj_t *_panic_tier_label;
    lv_obj_t *_panic_apikey_dialog;
    lv_obj_t *_panic_apikey_input;
    lv_obj_t *_panic_keyboard;
    lv_obj_t *_panic_view_dialog;
    // CONFIRM REPAIR form.
    lv_obj_t *_repair_dialog;
    lv_obj_t *_repair_kb;
    lv_obj_t *_repair_model;
    lv_obj_t *_repair_sig;
    lv_obj_t *_repair_code;
    lv_obj_t *_repair_source;
    lv_obj_t *_repair_hw;
    lv_obj_t *_repair_action;
    lv_obj_t *_repair_note;
    ParsedPanic _repair_parsed;
    // Panel layout mới (theo mockup).
    lv_obj_t *_pi_list;         // danh sách panic (trái)
    lv_obj_t *_pi_list_count;   // "(N)"
    lv_obj_t *_pi_model;
    lv_obj_t *_pi_ios;
    lv_obj_t *_pi_soc;
    lv_obj_t *_pi_file;
    lv_obj_t *_pi_sev;
    lv_obj_t *_pi_sevsub;
    lv_obj_t *_pi_keystr;
    lv_obj_t *_pi_cause;        // NGUYÊN NHÂN DỰ ĐOÁN (AI)
    lv_obj_t *_pi_check;        // HƯỚNG KIỂM TRA / ĐỀ XUẤT
    lv_obj_t *_pi_kb;           // THAM KHẢO KB
    lv_obj_t *_pi_conclusion;   // KẾT LUẬN
    lv_obj_t *_pi_detail;       // container cuộn chi tiết (để cuộn về đầu)
    ParsedPanic _pi_parsed;     // parse của file đang chọn (để hiện chi tiết)
    volatile bool _panic_busy;
    char _panic_file[96];
    char _panic_status[96];
    PanicResult _panic_result;
    HCCEeprom::Config _eeprom_config;
    volatile bool _eeprom_busy;
    // Two-tap confirmation before writing EEPROM (SD -> EEPROM overwrites the chip).
    bool _eeprom_program_confirm_pending;
    uint32_t _eeprom_program_confirm_tick;
    char _eeprom_status[64];
    char _eeprom_result[256];
};
