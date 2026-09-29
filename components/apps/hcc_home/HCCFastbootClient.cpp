#include "HCCFastbootClient.hpp"

#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *kVarNames[HCCFastbootClient::VarCount] = {
    "product",
    "serialno",
    "version-bootloader",
    "version-baseband",
    "unlocked",
    "secure",
    "current-slot",
    "battery-voltage",
    "is-userspace",
};

} // namespace

bool HCCFastbootClient::start(usb_device_handle_t device, uint8_t out_endpoint, uint8_t in_endpoint,
                              uint16_t in_mps)
{
    if (device == nullptr || out_endpoint == 0 || in_endpoint == 0 || in_mps == 0 || hasPendingTransfers()) {
        return false;
    }

    _device = device;
    _out_endpoint = out_endpoint;
    _in_endpoint = in_endpoint;
    _stopping = false;
    _var_index = 0;
    _awaiting_response = false;
    _pending_command[0] = '\0';
    _command_error[0] = '\0';
    _command_completed = false;
    memset(_values, 0, sizeof(_values));

    // Bulk IN transfers must be a whole number of max-size packets; responses are <= 256 bytes.
    const size_t rx_size = ((kResponseSize + in_mps - 1) / in_mps) * in_mps;
    if (usb_host_transfer_alloc(rx_size, 0, &_rx_transfer) != ESP_OK ||
        usb_host_transfer_alloc(kCommandCapacity, 0, &_tx_transfer) != ESP_OK) {
        printf("[HCC FASTBOOT] transfer alloc failed\n");
        stopForDeviceGone();
        _state = State::Failed;
        return false;
    }
    _rx_transfer->device_handle = _device;
    _rx_transfer->bEndpointAddress = _in_endpoint;
    _rx_transfer->num_bytes = rx_size;
    _rx_transfer->callback = HCCFastbootClient::onRxComplete;
    _rx_transfer->context = this;
    _tx_transfer->device_handle = _device;
    _tx_transfer->bEndpointAddress = _out_endpoint;
    _tx_transfer->callback = HCCFastbootClient::onTxComplete;
    _tx_transfer->context = this;

    setState(State::Reading);
    if (!submitRx()) {
        fail("RX submit failed");
        return false;
    }
    advance();
    return _state != State::Failed;
}

bool HCCFastbootClient::sendCommand(const char *command)
{
    if (_stopping || _state == State::Idle || _state == State::Failed || _state == State::Busy ||
        strlen(command) >= kCommandCapacity) {
        return false;
    }
    // A user command takes priority over the remaining getvars.
    snprintf(_pending_command, sizeof(_pending_command), "%s", command);
    if (!_awaiting_response && !_tx_in_flight) {
        advance();
    }
    return true;
}

void HCCFastbootClient::stopForDeviceGone()
{
    _stopping = true;
    // Idle transfers are freed now; in-flight ones are freed by their callback.
    if (_tx_transfer != nullptr && !_tx_in_flight) {
        usb_host_transfer_free(_tx_transfer);
        _tx_transfer = nullptr;
    }
    if (_rx_transfer != nullptr && !_rx_in_flight) {
        usb_host_transfer_free(_rx_transfer);
        _rx_transfer = nullptr;
    }
    setState(State::Idle);
}

void HCCFastbootClient::stopForShutdown()
{
    const bool rx_in_flight = _rx_in_flight;
    const bool tx_in_flight = _tx_in_flight;
    stopForDeviceGone();
    if (_device == nullptr) {
        return;
    }
    // Cancel in-flight transfers so their callbacks run (CANCELED) and free them.
    if (rx_in_flight && usb_host_endpoint_halt(_device, _in_endpoint) == ESP_OK) {
        usb_host_endpoint_flush(_device, _in_endpoint);
    }
    if (tx_in_flight && usb_host_endpoint_halt(_device, _out_endpoint) == ESP_OK) {
        usb_host_endpoint_flush(_device, _out_endpoint);
    }
}

void HCCFastbootClient::setState(State state)
{
    if (_state != state) {
        _state = state;
        ++_state_serial;
    }
}

void HCCFastbootClient::fail(const char *reason)
{
    printf("[HCC FASTBOOT] failed: %s\n", reason);
    setState(State::Failed);
}

bool HCCFastbootClient::submitRx()
{
    if (_rx_transfer == nullptr || _rx_in_flight || _stopping) {
        return false;
    }
    if (usb_host_transfer_submit(_rx_transfer) != ESP_OK) {
        return false;
    }
    _rx_in_flight = true;
    return true;
}

bool HCCFastbootClient::submitCommand(const char *command)
{
    const size_t length = strlen(command);
    memcpy(_tx_transfer->data_buffer, command, length);
    _tx_transfer->num_bytes = static_cast<int>(length);
    if (usb_host_transfer_submit(_tx_transfer) != ESP_OK) {
        fail("TX submit failed");
        return false;
    }
    _tx_in_flight = true;
    _awaiting_response = true;
    printf("[HCC FASTBOOT] > %s\n", command);
    return true;
}

// Sends the next command; only called when no command is outstanding and TX is idle.
void HCCFastbootClient::advance()
{
    if (_stopping || _tx_transfer == nullptr || _state == State::Failed) {
        return;
    }
    if (_pending_command[0] != '\0') {
        char command[kCommandCapacity];
        memcpy(command, _pending_command, sizeof(command));
        _pending_command[0] = '\0';
        _command_error[0] = '\0';
        setState(State::Busy);
        submitCommand(command);
        return;
    }
    if (_state == State::Reading) {
        if (_var_index < VarCount) {
            char command[kCommandCapacity];
            snprintf(command, sizeof(command), "getvar:%s", kVarNames[_var_index]);
            submitCommand(command);
        } else {
            printf("[HCC FASTBOOT] info ready: product=%s unlocked=%s\n", _values[VarProduct],
                   _values[VarUnlocked]);
            setState(State::Ready);
        }
    }
}

void HCCFastbootClient::handleResponse(const char *data, size_t length)
{
    if (length < 4) {
        return;
    }
    char text[kValueCapacity];
    size_t text_length = length - 4 < sizeof(text) - 1 ? length - 4 : sizeof(text) - 1;
    memcpy(text, data + 4, text_length);
    while (text_length > 0 && (text[text_length - 1] == '\0' || text[text_length - 1] == '\r' ||
                               text[text_length - 1] == '\n' || text[text_length - 1] == ' ')) {
        --text_length;
    }
    text[text_length] = '\0';

    // INFO/TEXT are progress messages; the command ends with OKAY or FAIL.
    if (memcmp(data, "INFO", 4) == 0 || memcmp(data, "TEXT", 4) == 0) {
        printf("[HCC FASTBOOT] (info) %s\n", text);
        return;
    }
    const bool okay = memcmp(data, "OKAY", 4) == 0;
    if (!okay) {
        printf("[HCC FASTBOOT] < %.4s %s\n", data, text);
    }

    if (_state == State::Reading && _var_index < VarCount) {
        snprintf(_values[_var_index], sizeof(_values[_var_index]), "%s", okay ? text : "");
        ++_var_index;
    } else if (_state == State::Busy) {
        snprintf(_command_error, sizeof(_command_error), "%s", okay ? "" : (text[0] != '\0' ? text : "FAIL"));
        _command_completed = true;
        setState(State::Ready);
    }

    _awaiting_response = false;
    if (!_tx_in_flight) {
        advance();
    }
}

void HCCFastbootClient::onTxComplete(usb_transfer_t *transfer)
{
    HCCFastbootClient *client = static_cast<HCCFastbootClient *>(transfer->context);
    client->_tx_in_flight = false;
    if (client->_stopping) {
        usb_host_transfer_free(transfer);
        client->_tx_transfer = nullptr;
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        printf("[HCC FASTBOOT] TX status=%d\n", transfer->status);
        client->fail("TX error");
        return;
    }
    // The response can arrive before this callback; continue here if it already has.
    if (!client->_awaiting_response) {
        client->advance();
    }
}

void HCCFastbootClient::onRxComplete(usb_transfer_t *transfer)
{
    HCCFastbootClient *client = static_cast<HCCFastbootClient *>(transfer->context);
    client->_rx_in_flight = false;

    if (!client->_stopping) {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            client->handleResponse(reinterpret_cast<const char *>(transfer->data_buffer),
                                   static_cast<size_t>(transfer->actual_num_bytes));
            if (client->_state != State::Failed && client->submitRx()) {
                return;
            }
        } else {
            printf("[HCC FASTBOOT] RX status=%d\n", transfer->status);
            client->fail("RX error");
        }
    }

    usb_host_transfer_free(transfer);
    client->_rx_transfer = nullptr;
}
