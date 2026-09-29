#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usb/usb_host.h"

// Minimal fastboot host (Android bootloader, interface FF/42/03): reads a fixed set of
// `getvar` values, then accepts one reboot command.
//
// All methods must be called from the USB client task; the transfer callbacks run there too.
class HCCFastbootClient {
public:
    enum class State {
        Idle,
        Reading, // getvar sequence running
        Ready,   // values collected, commands accepted
        Busy,    // user command sent, waiting for OKAY/FAIL
        Failed,
    };

    enum Var {
        VarProduct,
        VarSerial,
        VarBootloader,
        VarBaseband,
        VarUnlocked,
        VarSecure,
        VarSlot,
        VarBatteryVoltage,
        VarUserspace,
        VarCount,
    };

    bool start(usb_device_handle_t device, uint8_t out_endpoint, uint8_t in_endpoint,
               uint16_t in_mps);
    // Sends a command such as "reboot" once the getvar sequence is done (queued until then).
    bool sendCommand(const char *command);
    void stopForDeviceGone();
    void stopForShutdown();
    bool hasPendingTransfers() const { return _rx_transfer != nullptr || _tx_transfer != nullptr; }

    State state() const { return _state; }
    uint32_t stateSerial() const { return _state_serial; }
    const char *value(Var var) const { return _values[var]; }
    // True once a user command got its OKAY/FAIL; commandError() is its FAIL text ("" on OKAY).
    bool commandCompleted() const { return _command_completed; }
    const char *commandError() const { return _command_error; }

private:
    static constexpr size_t kResponseSize = 512;
    static constexpr size_t kCommandCapacity = 64;
    static constexpr size_t kValueCapacity = 48;

    static void onRxComplete(usb_transfer_t *transfer);
    static void onTxComplete(usb_transfer_t *transfer);

    void setState(State state);
    bool submitRx();
    bool submitCommand(const char *command);
    void advance();
    void handleResponse(const char *data, size_t length);
    void fail(const char *reason);

    usb_device_handle_t _device = nullptr;
    uint8_t _out_endpoint = 0;
    uint8_t _in_endpoint = 0;
    usb_transfer_t *_rx_transfer = nullptr;
    usb_transfer_t *_tx_transfer = nullptr;
    bool _rx_in_flight = false;
    bool _tx_in_flight = false;
    bool _stopping = false;

    State _state = State::Idle;
    uint32_t _state_serial = 0;
    int _var_index = 0;
    bool _awaiting_response = false;
    bool _command_completed = false;
    char _pending_command[kCommandCapacity] = {};
    char _values[VarCount][kValueCapacity] = {};
    char _command_error[kValueCapacity] = {};
};
