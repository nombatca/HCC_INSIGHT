#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usb/usb_host.h"

// Minimal ADB host over USB (Android Debug Bridge, interface FF/42/01).
// Connects, authenticates with a persistent RSA-2048 key (the phone shows "Allow USB
// debugging?" the first time), then runs one shell command that prints device info.
//
// All methods except startKeyLoad() must be called from the USB client task; the transfer
// callbacks run there too (inside usb_host_client_handle_events), so no locking is needed.
class HCCAdbClient {
public:
    enum class State {
        Idle,
        Connecting,      // CNXN sent, waiting for AUTH/CNXN
        WaitingKey,      // AUTH token received, signature pending (or host key still loading)
        WaitingUserAuth, // public key sent, waiting for the user to tap "Allow" on the phone
        ReadingInfo,     // connected, shell command running
        Ready,           // info collected
        Failed,
    };

    enum class JobState {
        Idle,
        Running,
        Done,
        TimedOut, // no CLSE within kJobTimeoutMs; jobOutput() holds what arrived
    };

    struct Info {
        char brand[32];
        char model[48];
        char device[32];
        char android[16];
        char sdk[8];
        char build[64];
        char serial[40];
        char chipset[32];
        char battery_level[8];
        char battery_health[24];
        char battery_temp[12];
        char battery_voltage[12];
        char battery_cycles[12];
        char wifi_mac[20];
        char bt_mac[20];
    };

    // Loads the ADB host key from NVS, or generates and stores it, on a background task.
    static void startKeyLoad();

    bool start(usb_device_handle_t device, uint8_t out_endpoint, uint8_t in_endpoint,
               uint16_t in_mps, uint16_t out_mps);
    // Opens a one-shot ADB service such as "reboot:recovery" once connected.
    bool runService(const char *service);
    // Runs one on-demand shell command (e.g. diagnostics); its output is jobOutput() once
    // jobState() is Done. Only one job at a time.
    bool runJob(const char *command);
    JobState jobState() const { return _job_state; }
    uint32_t jobSerial() const { return _job_serial; }
    const char *jobOutput() const { return _job_output != nullptr ? _job_output : ""; }
    size_t jobOutputLength() const { return _job_output_length; }
    // Resumes work deferred on the host key; call every USB loop iteration.
    void poll();
    // True while poll() has work to do without a USB event (the loop must not block forever).
    bool needsPoll() const { return _state == State::WaitingKey || _job_state == JobState::Running; }
    void stopForDeviceGone();
    void stopForShutdown();
    bool hasPendingTransfers() const { return _rx_transfer != nullptr || _tx_transfer != nullptr; }

    State state() const { return _state; }
    // Increments on every state change so the UI can refresh only when needed.
    uint32_t stateSerial() const { return _state_serial; }
    const Info &info() const { return _info; }

private:
    static constexpr size_t kMaxPayload = 4096;
    static constexpr size_t kHeaderSize = 24;
    static constexpr size_t kRxStreamCapacity = kHeaderSize + kMaxPayload + 1024;
    // Large enough for the diagnostics shell command; the phone accepts at least 4096.
    static constexpr size_t kTxPayloadCapacity = 3072;
    static constexpr size_t kTxQueueDepth = 4;
    static constexpr size_t kShellOutputCapacity = 3072;
    static constexpr size_t kTokenSize = 20;
    static constexpr uint32_t kShellLocalId = 1;
    static constexpr uint32_t kServiceLocalId = 2;
    static constexpr uint32_t kJobLocalId = 3;
    static constexpr size_t kJobOutputCapacity = 20480;
    static constexpr uint32_t kJobTimeoutMs = 60000;

    struct TxMessage {
        uint8_t header[kHeaderSize];
        uint16_t length;
        uint8_t payload[kTxPayloadCapacity];
    };

    static void onRxComplete(usb_transfer_t *transfer);
    static void onTxComplete(usb_transfer_t *transfer);

    void setState(State state);
    bool submitRx();
    bool queueMessage(uint32_t command, uint32_t arg0, uint32_t arg1, const void *payload, size_t length);
    bool submitNextTx();
    bool appendRx(const uint8_t *data, size_t length);
    bool handleMessage(uint32_t command, uint32_t arg0, uint32_t arg1, const uint8_t *payload, size_t length);
    void handleAuthToken(const uint8_t *token, size_t length);
    bool sendSignature();
    void parseBanner(const uint8_t *payload, size_t length);
    void parseShellOutput();
    void fail(const char *reason);

    usb_device_handle_t _device = nullptr;
    uint8_t _out_endpoint = 0;
    uint8_t _in_endpoint = 0;
    uint16_t _out_mps = 0;
    usb_transfer_t *_rx_transfer = nullptr;
    usb_transfer_t *_tx_transfer = nullptr;
    bool _rx_in_flight = false;
    bool _tx_in_flight = false;
    bool _stopping = false;

    // Large staging buffers live in PSRAM (allocated once, on first start()); the app object
    // itself is in internal RAM.
    TxMessage *_tx_queue = nullptr;
    size_t _tx_head = 0;
    size_t _tx_count = 0;
    bool _tx_payload_phase = false;

    uint8_t _rx_stream[kRxStreamCapacity] = {};
    size_t _rx_length = 0;

    State _state = State::Idle;
    uint32_t _state_serial = 0;
    uint8_t _token[kTokenSize] = {};
    size_t _token_length = 0;
    bool _signature_sent = false;
    bool _public_key_sent = false;
    uint32_t _max_payload = kMaxPayload;
    uint32_t _shell_remote_id = 0;
    char _shell_output[kShellOutputCapacity] = {};
    size_t _shell_output_length = 0;
    JobState _job_state = JobState::Idle;
    uint32_t _job_serial = 0;
    uint32_t _job_started_ms = 0;
    char *_job_output = nullptr;
    size_t _job_output_length = 0;
    Info _info = {};
};
