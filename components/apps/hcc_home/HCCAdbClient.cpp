#include "HCCAdbClient.hpp"
#include "hcc_cores.h"

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "nvs.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr uint32_t kCmdCnxn = 0x4E584E43;
constexpr uint32_t kCmdAuth = 0x48545541;
constexpr uint32_t kCmdOpen = 0x4E45504F;
constexpr uint32_t kCmdOkay = 0x59414B4F;
constexpr uint32_t kCmdClse = 0x45534C43;
constexpr uint32_t kCmdWrte = 0x45545257;
constexpr uint32_t kAdbVersion = 0x01000001;
constexpr uint32_t kAuthToken = 1;
constexpr uint32_t kAuthSignature = 2;
constexpr uint32_t kAuthRsaPublicKey = 3;

constexpr char kHostBanner[] = "host::";
// One shell round trip: "X=value" lines from getprop, then the raw `dumpsys battery` block.
constexpr char kInfoCommand[] =
    "shell:"
    "echo \"B=$(getprop ro.product.brand)\";"
    "echo \"M=$(getprop ro.product.model)\";"
    "echo \"D=$(getprop ro.product.device)\";"
    "echo \"V=$(getprop ro.build.version.release)\";"
    "echo \"K=$(getprop ro.build.version.sdk)\";"
    "echo \"I=$(getprop ro.build.display.id)\";"
    "echo \"S=$(getprop ro.serialno)\";"
    "echo \"C=$(getprop ro.soc.model)\";"
    "echo \"P=$(getprop ro.board.platform)\";"
    "echo \"Y=$(cat /sys/class/power_supply/battery/cycle_count 2>/dev/null)\";"
    "echo \"W=$( (cmd wifi get-factory-mac 2>/dev/null; cat /sys/class/net/wlan0/address 2>/dev/null; "
    "ip link show wlan0 2>/dev/null) | grep -oE '([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}' | "
    "grep -viE '^(00:00:00:00:00:00|02:00:00:00:00:00)$' | head -n1)\";"
    "echo \"T=$(settings get secure bluetooth_address 2>/dev/null)\";"
    // IMEI 2 SIM: thu vai ma lenh iphonesubinfo, lay chuoi ASCII (field giua 2 dau '),
    // chi giu 14-16 chu so. Android 10+ thuong chan -> rong.
    "echo \"G=$(for c in 1 2 3 4; do v=$(service call iphonesubinfo $c i32 0 2>/dev/null | "
    "cut -d\\' -f2 | tr -cd 0-9); if [ ${#v} -ge 14 ] && [ ${#v} -le 16 ]; then echo $v; break; fi; done)\";"
    "echo \"H=$(for c in 1 2 3 4; do v=$(service call iphonesubinfo $c i32 1 2>/dev/null | "
    "cut -d\\' -f2 | tr -cd 0-9); if [ ${#v} -ge 14 ] && [ ${#v} -le 16 ]; then echo $v; break; fi; done)\";"
    "dumpsys battery";

// ---- Host key (RSA-2048, persisted in NVS, shared by every connection) ----

constexpr char kNvsNamespace[] = "hcc_adb";
constexpr char kNvsKey[] = "host_key";
constexpr char kKeyComment[] = " hcc@insight";
constexpr size_t kRsaBytes = 256;
constexpr size_t kRsaWords = kRsaBytes / 4;
constexpr size_t kKeyDerCapacity = 1800;
constexpr size_t kAndroidPublicKeySize = 4 + 4 + kRsaBytes + kRsaBytes + 4;

std::atomic<bool> g_key_loading{false};
std::atomic<bool> g_key_ready{false};
std::atomic<bool> g_key_failed{false};
mbedtls_pk_context g_key;
char g_public_key[800];

int hardwareRandom(void *, unsigned char *output, size_t length)
{
    esp_fill_random(output, length);
    return 0;
}

bool isRsa2048(mbedtls_pk_context *key)
{
    return mbedtls_pk_get_type(key) == MBEDTLS_PK_RSA && mbedtls_pk_get_bitlen(key) == 2048;
}

bool loadKey()
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t length = 0;
    bool ok = nvs_get_blob(handle, kNvsKey, nullptr, &length) == ESP_OK && length > 0 &&
              length <= kKeyDerCapacity;
    uint8_t *der = ok ? static_cast<uint8_t *>(malloc(length)) : nullptr;
    ok = der != nullptr && nvs_get_blob(handle, kNvsKey, der, &length) == ESP_OK;
    nvs_close(handle);

    if (ok) {
        const int ret = mbedtls_pk_parse_key(&g_key, der, length, nullptr, 0, hardwareRandom, nullptr);
        ok = ret == 0 && isRsa2048(&g_key);
        if (!ok) {
            printf("[HCC ADB] stored key invalid (ret=-0x%04X); regenerating\n", static_cast<unsigned>(-ret));
            mbedtls_pk_free(&g_key);
            mbedtls_pk_init(&g_key);
        }
    }
    if (der != nullptr) {
        memset(der, 0, length);
        free(der);
    }
    return ok;
}

bool generateKey()
{
    printf("[HCC ADB] generating RSA-2048 host key...\n");
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    static const char kPersonalization[] = "hcc_adb_key";
    int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char *>(kPersonalization),
                                    sizeof(kPersonalization) - 1);
    if (ret == 0) {
        ret = mbedtls_pk_setup(&g_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    }
    if (ret == 0) {
        ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(g_key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
    }
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (ret != 0) {
        printf("[HCC ADB] key generation failed: -0x%04X\n", static_cast<unsigned>(-ret));
        return false;
    }

    // Persist so the phone's "Always allow from this computer" keeps working after reboot.
    uint8_t *der = static_cast<uint8_t *>(malloc(kKeyDerCapacity));
    if (der == nullptr) {
        return true;
    }
    const int length = mbedtls_pk_write_key_der(&g_key, der, kKeyDerCapacity);
    nvs_handle_t handle = 0;
    if (length > 0 && nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        esp_err_t err = nvs_set_blob(handle, kNvsKey, der + kKeyDerCapacity - length, length);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        printf("[HCC ADB] host key stored: %s\n", esp_err_to_name(err));
    } else {
        printf("[HCC ADB] host key not stored (der=%d)\n", length);
    }
    memset(der, 0, kKeyDerCapacity);
    free(der);
    return true;
}

// Android's RSAPublicKey struct (little-endian words), base64-encoded, plus a comment:
// { len=64, n0inv=-1/n[0] mod 2^32, n[64], rr=(2^2048)^2 mod n [64], exponent }.
bool buildPublicKey()
{
    uint8_t blob[kAndroidPublicKeySize] = {};
    mbedtls_mpi n, e, r, rr;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&rr);

    int ret = mbedtls_rsa_export(mbedtls_pk_rsa(g_key), &n, nullptr, nullptr, nullptr, &e);
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary_le(&n, blob + 8, kRsaBytes);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_lset(&r, 1);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_shift_l(&r, kRsaBytes * 8 * 2);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_mod_mpi(&rr, &r, &n);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary_le(&rr, blob + 8 + kRsaBytes, kRsaBytes);
    }
    uint32_t exponent = 0;
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary_le(&e, reinterpret_cast<unsigned char *>(&exponent), sizeof(exponent));
    }
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&rr);
    if (ret != 0) {
        printf("[HCC ADB] public key export failed: -0x%04X\n", static_cast<unsigned>(-ret));
        return false;
    }

    const uint32_t n0 = static_cast<uint32_t>(blob[8]) | (static_cast<uint32_t>(blob[9]) << 8) |
                        (static_cast<uint32_t>(blob[10]) << 16) | (static_cast<uint32_t>(blob[11]) << 24);
    uint32_t inverse = n0; // Newton iteration: correct to 3, 6, 12, 24, 48 bits for odd n0.
    for (int i = 0; i < 4; ++i) {
        inverse *= 2 - n0 * inverse;
    }
    const uint32_t words[] = {static_cast<uint32_t>(kRsaWords), 0u - inverse};
    for (size_t i = 0; i < 2; ++i) {
        for (size_t b = 0; b < 4; ++b) {
            blob[i * 4 + b] = static_cast<uint8_t>(words[i] >> (8 * b));
        }
    }
    memcpy(blob + 8 + 2 * kRsaBytes, &exponent, sizeof(exponent));

    size_t written = 0;
    ret = mbedtls_base64_encode(reinterpret_cast<unsigned char *>(g_public_key), sizeof(g_public_key),
                                &written, blob, sizeof(blob));
    if (ret != 0 || written + sizeof(kKeyComment) > sizeof(g_public_key)) {
        printf("[HCC ADB] public key encode failed\n");
        return false;
    }
    memcpy(g_public_key + written, kKeyComment, sizeof(kKeyComment));
    return true;
}

void keyTask(void *)
{
    mbedtls_pk_init(&g_key);
    const bool loaded = loadKey();
    const bool ok = (loaded || generateKey()) && buildPublicKey();
    printf("[HCC ADB] host key %s (%s)\n", ok ? "ready" : "FAILED", loaded ? "nvs" : "generated");
    if (ok) {
        g_key_ready.store(true, std::memory_order_release);
    } else {
        g_key_failed.store(true, std::memory_order_release);
    }
    printf("[HCC ADB] key task stack_hwm_bytes=%u\n", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelete(nullptr);
}

// PSRAM when available (malloc here only uses internal RAM), else internal RAM.
void *allocLarge(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer != nullptr ? buffer : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void writeU32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t readU32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

// Copies [src, src+length) trimmed of spaces/CR/NUL into a NUL-terminated field.
void copyField(char *dst, size_t size, const char *src, size_t length)
{
    while (length > 0 && (*src == ' ' || *src == '\t')) {
        ++src;
        --length;
    }
    while (length > 0 && (src[length - 1] == ' ' || src[length - 1] == '\r' || src[length - 1] == '\0')) {
        --length;
    }
    if (length >= size) {
        length = size - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

const char *batteryHealthName(int health)
{
    switch (health) {
    case 2: return "Good";
    case 3: return "Overheat";
    case 4: return "Dead";
    case 5: return "Over voltage";
    case 6: return "Failure";
    case 7: return "Cold";
    default: return "Unknown";
    }
}

} // namespace

void HCCAdbClient::startKeyLoad()
{
    bool expected = false;
    if (g_key_ready.load(std::memory_order_acquire) ||
        !g_key_loading.compare_exchange_strong(expected, true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(keyTask, "hcc_adb_key", 12288, nullptr, 2, nullptr, HCC_CORE_WORK) != pdPASS) {
        printf("[HCC ADB] key task creation failed\n");
        g_key_failed.store(true, std::memory_order_release);
    }
}

bool HCCAdbClient::start(usb_device_handle_t device, uint8_t out_endpoint, uint8_t in_endpoint,
                         uint16_t in_mps, uint16_t out_mps)
{
    if (device == nullptr || out_endpoint == 0 || in_endpoint == 0 || in_mps == 0 || out_mps == 0 ||
        hasPendingTransfers()) {
        return false;
    }

    _device = device;
    _out_endpoint = out_endpoint;
    _in_endpoint = in_endpoint;
    _out_mps = out_mps;
    _stopping = false;
    _tx_head = 0;
    _tx_count = 0;
    _tx_payload_phase = false;
    _rx_length = 0;
    _token_length = 0;
    _signature_sent = false;
    _public_key_sent = false;
    _max_payload = kMaxPayload;
    _shell_remote_id = 0;
    _shell_output_length = 0;
    _info = {};
    _job_state = JobState::Idle;
    _job_output_length = 0;
    ++_job_serial;

    if (_tx_queue == nullptr) {
        _tx_queue = static_cast<TxMessage *>(allocLarge(sizeof(TxMessage) * kTxQueueDepth));
    }
    if (_job_output == nullptr) {
        _job_output = static_cast<char *>(allocLarge(kJobOutputCapacity));
    }
    if (_tx_queue == nullptr || _job_output == nullptr) {
        printf("[HCC ADB] buffer alloc failed\n");
        _state = State::Failed;
        return false;
    }
    _job_output[0] = '\0';

    // Bulk IN transfers must be a whole number of max-size packets.
    const size_t rx_size = ((kMaxPayload + in_mps - 1) / in_mps) * in_mps;
    if (usb_host_transfer_alloc(rx_size, 0, &_rx_transfer) != ESP_OK ||
        usb_host_transfer_alloc(kTxPayloadCapacity, 0, &_tx_transfer) != ESP_OK) {
        printf("[HCC ADB] transfer alloc failed\n");
        stopForDeviceGone();
        _state = State::Failed;
        return false;
    }
    _rx_transfer->device_handle = _device;
    _rx_transfer->bEndpointAddress = _in_endpoint;
    _rx_transfer->num_bytes = rx_size;
    _rx_transfer->callback = HCCAdbClient::onRxComplete;
    _rx_transfer->context = this;
    _tx_transfer->device_handle = _device;
    _tx_transfer->bEndpointAddress = _out_endpoint;
    _tx_transfer->callback = HCCAdbClient::onTxComplete;
    _tx_transfer->context = this;

    setState(State::Connecting);
    if (!submitRx() ||
        !queueMessage(kCmdCnxn, kAdbVersion, kMaxPayload, kHostBanner, sizeof(kHostBanner))) {
        fail("connect submit failed");
        return false;
    }
    printf("[HCC ADB] CNXN sent: OUT=0x%02X IN=0x%02X\n", _out_endpoint, _in_endpoint);
    return true;
}

bool HCCAdbClient::runService(const char *service)
{
    if (_stopping || (_state != State::ReadingInfo && _state != State::Ready)) {
        return false;
    }
    printf("[HCC ADB] OPEN %s\n", service);
    return queueMessage(kCmdOpen, kServiceLocalId, 0, service, strlen(service) + 1);
}

bool HCCAdbClient::runJob(const char *command)
{
    static constexpr char kPrefix[] = "shell:";
    // Static: 2 KB is too much for the USB client task's stack; only that task calls this.
    static char service[kTxPayloadCapacity];
    const int length = snprintf(service, sizeof(service), "%s%s", kPrefix, command);
    if (_stopping || _job_state == JobState::Running || length <= 0 ||
        static_cast<size_t>(length) >= sizeof(service) ||
        (_state != State::ReadingInfo && _state != State::Ready)) {
        return false;
    }
    _job_output_length = 0;
    _job_output[0] = '\0';
    if (!queueMessage(kCmdOpen, kJobLocalId, 0, service, static_cast<size_t>(length) + 1)) {
        return false;
    }
    printf("[HCC ADB] job started (%d bytes)\n", length);
    _job_state = JobState::Running;
    _job_started_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ++_job_serial;
    return true;
}

void HCCAdbClient::poll()
{
    if (_job_state == JobState::Running &&
        static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS) - _job_started_ms > kJobTimeoutMs) {
        printf("[HCC ADB] job timed out after %u ms (%u bytes received)\n", static_cast<unsigned>(kJobTimeoutMs),
               static_cast<unsigned>(_job_output_length));
        _job_state = JobState::TimedOut;
        ++_job_serial;
    }
    if (_state != State::WaitingKey || _stopping) {
        return;
    }
    if (g_key_ready.load(std::memory_order_acquire)) {
        sendSignature();
    } else if (g_key_failed.load(std::memory_order_acquire)) {
        fail("host key unavailable");
    }
}

void HCCAdbClient::stopForDeviceGone()
{
    _stopping = true;
    _tx_count = 0;
    // Idle transfers are freed now; in-flight ones are freed by their callback.
    if (_tx_transfer != nullptr && !_tx_in_flight) {
        usb_host_transfer_free(_tx_transfer);
        _tx_transfer = nullptr;
    }
    if (_rx_transfer != nullptr && !_rx_in_flight) {
        usb_host_transfer_free(_rx_transfer);
        _rx_transfer = nullptr;
    }
    if (_state != State::Idle) {
        setState(State::Idle);
    }
    if (_job_state == JobState::Running) {
        _job_state = JobState::Idle;
        ++_job_serial;
    }
}

void HCCAdbClient::stopForShutdown()
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

void HCCAdbClient::setState(State state)
{
    if (_state != state) {
        _state = state;
        ++_state_serial;
    }
}

void HCCAdbClient::fail(const char *reason)
{
    printf("[HCC ADB] failed: %s\n", reason);
    setState(State::Failed);
}

bool HCCAdbClient::submitRx()
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

bool HCCAdbClient::queueMessage(uint32_t command, uint32_t arg0, uint32_t arg1, const void *payload,
                                size_t length)
{
    if (_stopping || _tx_count >= kTxQueueDepth || length > kTxPayloadCapacity || length > _max_payload) {
        printf("[HCC ADB] TX queue rejected cmd=0x%08X len=%u\n", static_cast<unsigned>(command),
               static_cast<unsigned>(length));
        return false;
    }

    TxMessage &message = _tx_queue[(_tx_head + _tx_count) % kTxQueueDepth];
    uint32_t checksum = 0;
    const uint8_t *bytes = static_cast<const uint8_t *>(payload);
    for (size_t i = 0; i < length; ++i) {
        checksum += bytes[i];
    }
    writeU32(message.header + 0, command);
    writeU32(message.header + 4, arg0);
    writeU32(message.header + 8, arg1);
    writeU32(message.header + 12, static_cast<uint32_t>(length));
    writeU32(message.header + 16, checksum);
    writeU32(message.header + 20, command ^ 0xFFFFFFFFu);
    if (length > 0) {
        memcpy(message.payload, payload, length);
    }
    message.length = static_cast<uint16_t>(length);
    ++_tx_count;
    return submitNextTx();
}

// ADB over USB sends the 24-byte header and the payload as separate bulk transfers.
bool HCCAdbClient::submitNextTx()
{
    if (_tx_in_flight || _tx_count == 0 || _stopping || _tx_transfer == nullptr) {
        return true;
    }

    const TxMessage &message = _tx_queue[_tx_head];
    if (!_tx_payload_phase) {
        memcpy(_tx_transfer->data_buffer, message.header, kHeaderSize);
        _tx_transfer->num_bytes = kHeaderSize;
        _tx_transfer->flags = 0;
    } else {
        memcpy(_tx_transfer->data_buffer, message.payload, message.length);
        _tx_transfer->num_bytes = message.length;
        // A payload that fills its last packet needs a ZLP so the phone sees the end of it.
        _tx_transfer->flags = (message.length % _out_mps) == 0 ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
    }

    if (usb_host_transfer_submit(_tx_transfer) != ESP_OK) {
        fail("TX submit failed");
        return false;
    }
    _tx_in_flight = true;
    return true;
}

void HCCAdbClient::onTxComplete(usb_transfer_t *transfer)
{
    HCCAdbClient *client = static_cast<HCCAdbClient *>(transfer->context);
    client->_tx_in_flight = false;
    if (client->_stopping) {
        usb_host_transfer_free(transfer);
        client->_tx_transfer = nullptr;
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        printf("[HCC ADB] TX status=%d\n", transfer->status);
        client->_tx_count = 0;
        client->fail("TX error");
        return;
    }

    const TxMessage &message = client->_tx_queue[client->_tx_head];
    if (!client->_tx_payload_phase && message.length > 0) {
        client->_tx_payload_phase = true;
    } else {
        client->_tx_payload_phase = false;
        client->_tx_head = (client->_tx_head + 1) % kTxQueueDepth;
        --client->_tx_count;
    }
    client->submitNextTx();
}

void HCCAdbClient::onRxComplete(usb_transfer_t *transfer)
{
    HCCAdbClient *client = static_cast<HCCAdbClient *>(transfer->context);
    client->_rx_in_flight = false;

    if (!client->_stopping) {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            if (client->appendRx(transfer->data_buffer, static_cast<size_t>(transfer->actual_num_bytes)) &&
                client->_state != State::Failed && client->submitRx()) {
                return;
            }
        } else {
            printf("[HCC ADB] RX status=%d\n", transfer->status);
            client->fail("RX error");
        }
    }

    usb_host_transfer_free(transfer);
    client->_rx_transfer = nullptr;
}

bool HCCAdbClient::appendRx(const uint8_t *data, size_t length)
{
    if (_rx_length + length > kRxStreamCapacity) {
        fail("RX stream overflow");
        return false;
    }
    memcpy(_rx_stream + _rx_length, data, length);
    _rx_length += length;

    // Messages may span or share USB transfers, so parse the byte stream.
    while (_rx_length >= kHeaderSize) {
        const uint32_t command = readU32(_rx_stream);
        const uint32_t payload_length = readU32(_rx_stream + 12);
        if (readU32(_rx_stream + 20) != (command ^ 0xFFFFFFFFu) || payload_length > kMaxPayload) {
            fail("bad message header");
            return false;
        }
        const size_t total = kHeaderSize + payload_length;
        if (_rx_length < total) {
            break;
        }
        if (!handleMessage(command, readU32(_rx_stream + 4), readU32(_rx_stream + 8),
                           _rx_stream + kHeaderSize, payload_length)) {
            return false;
        }
        memmove(_rx_stream, _rx_stream + total, _rx_length - total);
        _rx_length -= total;
    }
    return true;
}

bool HCCAdbClient::handleMessage(uint32_t command, uint32_t arg0, uint32_t arg1, const uint8_t *payload,
                                 size_t length)
{
    switch (command) {
    case kCmdAuth:
        if (arg0 == kAuthToken) {
            handleAuthToken(payload, length);
        }
        break;

    case kCmdCnxn:
        printf("[HCC ADB] CNXN from device: version=0x%08X max=%u\n", static_cast<unsigned>(arg0),
               static_cast<unsigned>(arg1));
        _max_payload = arg1 < kMaxPayload ? arg1 : kMaxPayload;
        parseBanner(payload, length);
        setState(State::ReadingInfo);
        if (!queueMessage(kCmdOpen, kShellLocalId, 0, kInfoCommand, sizeof(kInfoCommand))) {
            // Model/device from the banner are still shown.
            setState(State::Ready);
        }
        break;

    case kCmdOkay:
        if (arg1 == kShellLocalId) {
            _shell_remote_id = arg0;
        }
        break;

    case kCmdWrte:
        if (arg1 == kShellLocalId) {
            const size_t room = kShellOutputCapacity - 1 - _shell_output_length;
            const size_t copy = length < room ? length : room;
            memcpy(_shell_output + _shell_output_length, payload, copy);
            _shell_output_length += copy;
            _shell_output[_shell_output_length] = '\0';
        } else if (arg1 == kJobLocalId && _job_state == JobState::Running) {
            const size_t room = kJobOutputCapacity - 1 - _job_output_length;
            const size_t copy = length < room ? length : room;
            memcpy(_job_output + _job_output_length, payload, copy);
            _job_output_length += copy;
            _job_output[_job_output_length] = '\0';
        }
        // Every stream waits for OKAY before its next WRTE.
        queueMessage(kCmdOkay, arg1, arg0, nullptr, 0);
        break;

    case kCmdClse:
        if (arg1 == kJobLocalId && _job_state == JobState::Running) {
            printf("[HCC ADB] job done (%u bytes)\n", static_cast<unsigned>(_job_output_length));
            // Full output in the serial log, to tune the parser for a phone that reports oddly.
            printf("[HCC ADB] ---- job output ----\n%s\n[HCC ADB] ---- end job output ----\n", _job_output);
            _job_state = JobState::Done;
            ++_job_serial;
        }
        if (arg1 == kShellLocalId && _state == State::ReadingInfo) {
            parseShellOutput();
            printf("[HCC ADB] info ready: %s %s Android %s\n", _info.brand, _info.model, _info.android);
            setState(State::Ready);
        }
        break;

    default:
        break;
    }
    return _state != State::Failed;
}

void HCCAdbClient::handleAuthToken(const uint8_t *token, size_t length)
{
    if (length != kTokenSize) {
        fail("unexpected AUTH token size");
        return;
    }
    memcpy(_token, token, kTokenSize);
    _token_length = kTokenSize;

    if (!_signature_sent) {
        // Signed from poll() at the top of the USB loop (not in this transfer callback) to keep
        // the RSA operation off the deep callback stack; it also waits there for the key.
        setState(State::WaitingKey);
    } else if (!_public_key_sent) {
        // The phone does not know this key yet: offer it, which shows "Allow USB debugging?".
        _public_key_sent = queueMessage(kCmdAuth, kAuthRsaPublicKey, 0, g_public_key, strlen(g_public_key) + 1);
        printf("[HCC ADB] signature rejected; public key sent, waiting for user\n");
        setState(State::WaitingUserAuth);
    }
    // Further tokens mean the prompt is still open or was dismissed; keep waiting.
}

bool HCCAdbClient::sendSignature()
{
    uint8_t signature[kRsaBytes];
    const int ret = mbedtls_rsa_pkcs1_sign(mbedtls_pk_rsa(g_key), hardwareRandom, nullptr, MBEDTLS_MD_SHA1,
                                           static_cast<unsigned int>(_token_length), _token, signature);
    if (ret != 0) {
        printf("[HCC ADB] sign failed: -0x%04X\n", static_cast<unsigned>(-ret));
        fail("sign failed");
        return false;
    }
    _signature_sent = true;
    setState(State::Connecting);
    return queueMessage(kCmdAuth, kAuthSignature, 0, signature, sizeof(signature));
}

// "device::ro.product.name=x;ro.product.model=y;ro.product.device=z;features=..."
void HCCAdbClient::parseBanner(const uint8_t *payload, size_t length)
{
    const char *text = reinterpret_cast<const char *>(payload);
    const char *end = text + length;
    const char *props = static_cast<const char *>(memchr(text, ':', length));
    if (props == nullptr || props + 1 >= end || props[1] != ':') {
        return;
    }
    for (const char *p = props + 2; p < end;) {
        const char *semi = static_cast<const char *>(memchr(p, ';', end - p));
        const char *item_end = semi != nullptr ? semi : end;
        const char *eq = static_cast<const char *>(memchr(p, '=', item_end - p));
        if (eq != nullptr) {
            const size_t key_length = eq - p;
            if (key_length == 16 && memcmp(p, "ro.product.model", 16) == 0) {
                copyField(_info.model, sizeof(_info.model), eq + 1, item_end - eq - 1);
            } else if (key_length == 17 && memcmp(p, "ro.product.device", 17) == 0) {
                copyField(_info.device, sizeof(_info.device), eq + 1, item_end - eq - 1);
            }
        }
        p = item_end + 1;
    }
}

void HCCAdbClient::parseShellOutput()
{
    char soc[32] = "";
    char platform[32] = "";
    for (char *line = _shell_output; line != nullptr && *line != '\0';) {
        char *newline = strchr(line, '\n');
        const size_t length = newline != nullptr ? static_cast<size_t>(newline - line) : strlen(line);

        if (length >= 2 && line[1] == '=') {
            char *field = nullptr;
            size_t size = 0;
            switch (line[0]) {
            case 'B': field = _info.brand; size = sizeof(_info.brand); break;
            case 'M': field = _info.model; size = sizeof(_info.model); break;
            case 'D': field = _info.device; size = sizeof(_info.device); break;
            case 'V': field = _info.android; size = sizeof(_info.android); break;
            case 'K': field = _info.sdk; size = sizeof(_info.sdk); break;
            case 'I': field = _info.build; size = sizeof(_info.build); break;
            case 'S': field = _info.serial; size = sizeof(_info.serial); break;
            case 'C': field = soc; size = sizeof(soc); break;
            case 'P': field = platform; size = sizeof(platform); break;
            case 'Y': field = _info.battery_cycles; size = sizeof(_info.battery_cycles); break;
            case 'W': field = _info.wifi_mac; size = sizeof(_info.wifi_mac); break;
            case 'T': field = _info.bt_mac; size = sizeof(_info.bt_mac); break;
            case 'G': field = _info.imei1; size = sizeof(_info.imei1); break;
            case 'H': field = _info.imei2; size = sizeof(_info.imei2); break;
            default: break;
            }
            // Keep banner values when getprop printed nothing.
            char value[64];
            copyField(value, sizeof(value), line + 2, length - 2);
            if (field != nullptr && value[0] != '\0') {
                snprintf(field, size, "%s", value);
            }
        } else {
            // `dumpsys battery` lines: "  level: 87", "  temperature: 312", ...
            char value[32];
            const char *trimmed = line;
            while (*trimmed == ' ') {
                ++trimmed;
            }
            const size_t rest = length - static_cast<size_t>(trimmed - line);
            auto match = [&](const char *key) {
                const size_t key_length = strlen(key);
                if (rest > key_length && strncmp(trimmed, key, key_length) == 0) {
                    copyField(value, sizeof(value), trimmed + key_length, rest - key_length);
                    return true;
                }
                return false;
            };
            if (match("level:")) {
                snprintf(_info.battery_level, sizeof(_info.battery_level), "%s%%", value);
            } else if (match("health:")) {
                snprintf(_info.battery_health, sizeof(_info.battery_health), "%s", batteryHealthName(atoi(value)));
            } else if (match("temperature:")) {
                const int tenths = atoi(value);
                snprintf(_info.battery_temp, sizeof(_info.battery_temp), "%d.%dC", tenths / 10, abs(tenths % 10));
            } else if (match("voltage:")) {
                snprintf(_info.battery_voltage, sizeof(_info.battery_voltage), "%dmV", atoi(value));
            }
        }

        line = newline != nullptr ? newline + 1 : nullptr;
    }
    snprintf(_info.chipset, sizeof(_info.chipset), "%s", soc[0] != '\0' ? soc : platform);

    // MAC bị Android ẩn vì riêng tư (02:00:...) hoặc không đọc được ("null") -> để trống.
    if (strcmp(_info.bt_mac, "02:00:00:00:00:00") == 0 || strcmp(_info.bt_mac, "null") == 0) {
        _info.bt_mac[0] = '\0';
    }
    if (strcmp(_info.wifi_mac, "null") == 0) {
        _info.wifi_mac[0] = '\0';
    }
}
