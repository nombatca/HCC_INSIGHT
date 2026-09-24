#include "HCCUSBTransport.hpp"

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "hal/usb_dwc_ll.h"
#include "soc/usb_dwc_struct.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

constexpr char kPairRecordPartition[] = "hcc_pair";
constexpr char kPairRecordNamespace[] = "hcc_pairrec";
constexpr char kLegacyPairRecordKey[] = "current";
constexpr char kPairRecordIndexKey[] = "index";
constexpr uint32_t kPairRecordIndexMagic = 0x48434349; // "HCCI"
constexpr uint32_t kPairRecordIndexVersion = 1;
constexpr size_t kPairRecordMaximumDevices = 16;
constexpr size_t kPairRecordStoredUdidCapacity = 128;

struct PairRecordIndexEntry {
    uint32_t slot;
    char udid[kPairRecordStoredUdidCapacity];
};

struct PairRecordIndex {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    PairRecordIndexEntry entries[kPairRecordMaximumDevices];
};

esp_err_t openPairRecordPartition(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (handle == nullptr) return ESP_ERR_INVALID_ARG;
    static bool initialized = false;
    static esp_err_t init_result = ESP_FAIL;
    if (!initialized) {
        init_result = nvs_flash_init_partition(kPairRecordPartition);
        if (init_result == ESP_ERR_NVS_NO_FREE_PAGES || init_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // Only the dedicated pair-record partition is erased; the default nvs is never touched.
            printf("[HCC PAIR] dedicated NVS reformat partition=%s reason=%s\n",
                   kPairRecordPartition, esp_err_to_name(init_result));
            init_result = nvs_flash_erase_partition(kPairRecordPartition);
            if (init_result == ESP_OK) {
                init_result = nvs_flash_init_partition(kPairRecordPartition);
            }
        }
        // Cache success only, so a failed early init is retried lazily on first pair record access.
        initialized = init_result == ESP_OK;
        if (init_result == ESP_OK) {
            printf("[HCC PAIR] dedicated NVS ready partition=%s\n", kPairRecordPartition);
        } else {
            printf("[HCC PAIR] dedicated NVS init failed: %s\n", esp_err_to_name(init_result));
        }
    }
    if (init_result != ESP_OK) return init_result;
    return nvs_open_from_partition(kPairRecordPartition, kPairRecordNamespace, mode, handle);
}

bool makePairRecordSlotKey(uint32_t slot, char *key, size_t key_capacity)
{
    return key != nullptr && key_capacity >= 10 &&
           snprintf(key, key_capacity, "r%08x", static_cast<unsigned>(slot)) == 9;
}

bool loadPairRecordIndex(nvs_handle_t handle, PairRecordIndex *index)
{
    if (index == nullptr) return false;
    memset(index, 0, sizeof(*index));
    size_t size = sizeof(*index);
    esp_err_t err = nvs_get_blob(handle, kPairRecordIndexKey, index, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        index->magic = kPairRecordIndexMagic;
        index->version = kPairRecordIndexVersion;
        return true;
    }
    if (err != ESP_OK || size != sizeof(*index) ||
        index->magic != kPairRecordIndexMagic ||
        index->version != kPairRecordIndexVersion ||
        index->count > kPairRecordMaximumDevices) return false;
    for (uint32_t i = 0; i < index->count; ++i) {
        if (index->entries[i].slot >= kPairRecordMaximumDevices ||
            strnlen(index->entries[i].udid, sizeof(index->entries[i].udid)) == sizeof(index->entries[i].udid)) return false;
    }
    return true;
}

bool findPairRecordSlot(const PairRecordIndex &index, const char *udid, uint32_t *slot)
{
    if (udid == nullptr || slot == nullptr) return false;
    for (uint32_t i = 0; i < index.count; ++i) {
        if (strcmp(index.entries[i].udid, udid) == 0) {
            *slot = index.entries[i].slot;
            return true;
        }
    }
    return false;
}

// Copies an already validated legacy record into the dedicated partition. Legacy data is left intact.
esp_err_t storePairRecordInPartition(const char *udid, const uint8_t *blob, size_t blob_size, uint32_t *stored_slot)
{
    if (udid == nullptr || blob == nullptr || blob_size == 0) return ESP_ERR_INVALID_ARG;
    const size_t udid_length = strnlen(udid, kPairRecordStoredUdidCapacity);
    if (udid_length == 0 || udid_length >= kPairRecordStoredUdidCapacity) return ESP_ERR_INVALID_SIZE;

    PairRecordIndex *index = static_cast<PairRecordIndex *>(
        heap_caps_calloc(1, sizeof(PairRecordIndex), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (index == nullptr) return ESP_ERR_NO_MEM;

    nvs_handle_t handle = 0;
    char record_key[16] = {};
    uint32_t record_slot = 0;
    bool index_changed = false;
    esp_err_t ret = openPairRecordPartition(NVS_READWRITE, &handle);
    if (ret == ESP_OK && !loadPairRecordIndex(handle, index)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && !findPairRecordSlot(*index, udid, &record_slot)) {
        if (index->count >= kPairRecordMaximumDevices) {
            ret = ESP_ERR_NO_MEM;
        } else {
            bool used[kPairRecordMaximumDevices] = {};
            for (uint32_t i = 0; i < index->count; ++i) used[index->entries[i].slot] = true;
            while (record_slot < kPairRecordMaximumDevices && used[record_slot]) ++record_slot;
            if (record_slot >= kPairRecordMaximumDevices) {
                ret = ESP_ERR_INVALID_SIZE;
            } else {
                PairRecordIndexEntry &entry = index->entries[index->count++];
                entry.slot = record_slot;
                memcpy(entry.udid, udid, udid_length);
                entry.udid[udid_length] = '\0';
                index_changed = true;
            }
        }
    }
    if (ret == ESP_OK && !makePairRecordSlotKey(record_slot, record_key, sizeof(record_key))) {
        ret = ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, record_key, blob, blob_size);
    }
    if (ret == ESP_OK && index_changed) {
        ret = nvs_set_blob(handle, kPairRecordIndexKey, index, sizeof(*index));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (handle != 0) nvs_close(handle);
    heap_caps_free(index);
    if (ret == ESP_OK && stored_slot != nullptr) *stored_slot = record_slot;
    return ret;
}

// Removes a rejected record from the dedicated partition. The legacy default-nvs store is never touched.
esp_err_t erasePairRecordInPartition(const char *udid, uint32_t *erased_slot)
{
    if (udid == nullptr || udid[0] == '\0') return ESP_ERR_INVALID_ARG;

    PairRecordIndex *index = static_cast<PairRecordIndex *>(
        heap_caps_calloc(1, sizeof(PairRecordIndex), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (index == nullptr) return ESP_ERR_NO_MEM;

    nvs_handle_t handle = 0;
    char record_key[16] = {};
    uint32_t record_slot = 0;
    esp_err_t ret = openPairRecordPartition(NVS_READWRITE, &handle);
    if (ret == ESP_OK && !loadPairRecordIndex(handle, index)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && !findPairRecordSlot(*index, udid, &record_slot)) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK && !makePairRecordSlotKey(record_slot, record_key, sizeof(record_key))) {
        ret = ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, record_key);
        if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < index->count; ++i) {
            if (strcmp(index->entries[i].udid, udid) != 0) {
                if (kept != i) index->entries[kept] = index->entries[i];
                ++kept;
            }
        }
        for (uint32_t i = kept; i < index->count; ++i) {
            memset(&index->entries[i], 0, sizeof(index->entries[i]));
        }
        index->count = kept;
        ret = nvs_set_blob(handle, kPairRecordIndexKey, index, sizeof(*index));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (handle != 0) nvs_close(handle);
    heap_caps_free(index);
    if (ret == ESP_OK && erased_slot != nullptr) *erased_slot = record_slot;
    return ret;
}

constexpr uint32_t kPairRecordMagic = 0x48434350; // "HCCP"
constexpr uint32_t kPairRecordVersion = 1;

struct PairRecordBlobHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t generation;
    uint32_t udid_length;
    uint32_t host_id_length;
    uint32_t system_buid_length;
    uint32_t root_private_key_length;
    uint32_t host_private_key_length;
    uint32_t root_certificate_length;
    uint32_t host_certificate_length;
    uint32_t device_certificate_length;
    uint32_t wifi_mac_address_length;
    uint32_t escrow_bag_length;
};

bool appendPairRecordField(uint8_t *blob, size_t capacity, size_t *offset,
                           const void *data, size_t length)
{
    if (blob == nullptr || offset == nullptr || *offset > capacity ||
        (length > 0 && data == nullptr) || length > capacity - *offset) {
        return false;
    }
    if (length > 0) {
        memcpy(blob + *offset, data, length);
        *offset += length;
    }
    return true;
}

void logUsbAllocationHeap(const char *stage)
{
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    printf("[HCC HEAP] USB %s internal free=%u largest=%u min=%u dma_free=%u dma_largest=%u dma_min=%u psram_free=%u psram_largest=%u psram_min=%u\n", stage,
           static_cast<unsigned>(heap_caps_get_free_size(internal)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
           static_cast<unsigned>(heap_caps_get_free_size(dma)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(dma)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(dma)),
           static_cast<unsigned>(heap_caps_get_free_size(psram)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(psram)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(psram)));
}

void logTlsMemory(const char *stage)
{
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    printf("[HCC TLS] %s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u stack_hwm_bytes=%u\n",
           stage,
           static_cast<unsigned>(heap_caps_get_free_size(internal)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
           static_cast<unsigned>(heap_caps_get_free_size(dma)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(dma)),
           static_cast<unsigned>(heap_caps_get_free_size(psram)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(psram)),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void logTlsError(const char *stage, int error)
{
    char message[160] = {};
    mbedtls_strerror(error, message, sizeof(message));
    printf("[HCC TLS] %s ret=-0x%04X error=%s\n",
           stage, static_cast<unsigned>(-error), message);
}

uint32_t readBigEndianU32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void logVersionFields(const uint8_t *data, size_t length)
{
    if (data == nullptr || length < 20) {
        printf("[HCC USB MUX] VERSION fields: protocol=%s length=%s major=%s minor=%s padding=%s (actual=%u)\n",
               (data != nullptr && length >= 4) ? "available" : "unavailable",
               (data != nullptr && length >= 8) ? "available" : "unavailable",
               (data != nullptr && length >= 12) ? "available" : "unavailable",
               (data != nullptr && length >= 16) ? "available" : "unavailable",
               (data != nullptr && length >= 20) ? "available" : "unavailable",
               static_cast<unsigned>(length));
        return;
    }

    printf("[HCC USB MUX] VERSION fields: protocol=%u length=%u major=%u minor=%u padding=%u\n",
           readBigEndianU32(data),
           readBigEndianU32(data + 4),
           readBigEndianU32(data + 8),
           readBigEndianU32(data + 12),
           readBigEndianU32(data + 16));
}

void logVersionRaw(const uint8_t *data)
{
    printf("[HCC USB MUX] RX VERSION RAW (20):");
    for (size_t index = 0; index < 20; ++index) {
        printf(" %02X", data[index]);
    }
    printf("\n");
}

void logRxRaw(int slot, const uint8_t *data, size_t length)
{
    constexpr size_t kRawLogLimit = 128;
    const size_t logged_length = (length < kRawLogLimit) ? length : kRawLogLimit;
    printf("[HCC USB MUX] RX RAW[%d] (%u):", slot, static_cast<unsigned>(length));
    for (size_t index = 0; index < logged_length; ++index) {
        printf(" %02X", data[index]);
    }
    if (logged_length != length) {
        printf(" ...");
    }
    printf("\n");
}

uint16_t readBigEndianU16(const uint8_t *data)
{
    return (static_cast<uint16_t>(data[0]) << 8) |
           static_cast<uint16_t>(data[1]);
}

void writeBigEndianU16(uint8_t *data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void writeBigEndianU32(uint8_t *data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

static constexpr char kLockdownQueryTypeXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Request</key>\n"
    "<string>QueryType</string>\n"
    "</dict>\n"
    "</plist>";

static constexpr size_t kLockdownQueryTypeBodySize = sizeof(kLockdownQueryTypeXml) - 1;

static constexpr char kLockdownGetValueProductVersionXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Key</key>\n"
    "<string>ProductVersion</string>\n"
    "<key>Request</key>\n"
    "<string>GetValue</string>\n"
    "</dict>\n"
    "</plist>";

static constexpr size_t kLockdownGetValueProductVersionBodySize =
    sizeof(kLockdownGetValueProductVersionXml) - 1;

static constexpr char kLockdownGetValueDeviceClassXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Key</key>\n"
    "<string>DeviceClass</string>\n"
    "<key>Request</key>\n"
    "<string>GetValue</string>\n"
    "</dict>\n"
    "</plist>";

static constexpr size_t kLockdownGetValueDeviceClassBodySize =
    sizeof(kLockdownGetValueDeviceClassXml) - 1;

static constexpr char kLockdownGetValueDevicePublicKeyXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Key</key>\n"
    "<string>DevicePublicKey</string>\n"
    "<key>Request</key>\n"
    "<string>GetValue</string>\n"
    "</dict>\n"
    "</plist>";

static constexpr size_t kLockdownGetValueDevicePublicKeyBodySize =
    sizeof(kLockdownGetValueDevicePublicKeyXml) - 1;

static constexpr char kLockdownGetValueWiFiAddressXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Key</key>\n"
    "<string>WiFiAddress</string>\n"
    "<key>Request</key>\n"
    "<string>GetValue</string>\n"
    "</dict>\n"
    "</plist>";

static constexpr size_t kLockdownGetValueWiFiAddressBodySize =
    sizeof(kLockdownGetValueWiFiAddressXml) - 1;

static constexpr char kLockdownStartNotificationServiceXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Request</key>\n"
    "<string>StartService</string>\n"
    "<key>Service</key>\n"
    "<string>com.apple.mobile.insecure_notification_proxy</string>\n"
    "</dict>\n"
    "</plist>";

static constexpr size_t kLockdownStartNotificationServiceBodySize =
    sizeof(kLockdownStartNotificationServiceXml) - 1;

static constexpr char kNotificationObservePairXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Command</key>\n"
    "<string>ObserveNotification</string>\n"
    "<key>Name</key>\n"
    "<string>com.apple.mobile.lockdown.request_pair</string>\n"
    "</dict>\n"
    "</plist>";

static constexpr char kNotificationObserveHostBuidXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Command</key>\n"
    "<string>ObserveNotification</string>\n"
    "<key>Name</key>\n"
    "<string>com.apple.mobile.lockdown.request_host_buid</string>\n"
    "</dict>\n"
    "</plist>";

struct NotificationObserveRequest {
    const char *xml;
    size_t length;
    const char *name;
};

static constexpr NotificationObserveRequest kNotificationObserveRequests[] = {
    {kNotificationObservePairXml, sizeof(kNotificationObservePairXml) - 1,
     "com.apple.mobile.lockdown.request_pair"},
    {kNotificationObserveHostBuidXml, sizeof(kNotificationObserveHostBuidXml) - 1,
     "com.apple.mobile.lockdown.request_host_buid"},
};

bool containsBytes(const uint8_t *data, size_t length, const char *needle)
{
    if (data == nullptr || needle == nullptr) {
        return false;
    }
    const size_t needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > length) {
        return false;
    }
    for (size_t index = 0; index <= (length - needle_length); ++index) {
        if (memcmp(data + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

bool extractXmlStringForKey(const uint8_t *data, size_t length, const char *key,
                            char *value, size_t value_capacity)
{
    if (data == nullptr || key == nullptr || value == nullptr || value_capacity == 0) {
        return false;
    }

    static constexpr char kKeyOpen[] = "<key>";
    static constexpr char kKeyClose[] = "</key>";
    static constexpr char kStringOpen[] = "<string>";
    static constexpr char kStringClose[] = "</string>";
    const size_t key_length = strlen(key);
    for (size_t index = 0; index < length; ++index) {
        if ((length - index) < (sizeof(kKeyOpen) - 1 + key_length + sizeof(kKeyClose) - 1) ||
            memcmp(data + index, kKeyOpen, sizeof(kKeyOpen) - 1) != 0) {
            continue;
        }
        const size_t key_start = index + sizeof(kKeyOpen) - 1;
        if (memcmp(data + key_start, key, key_length) != 0 ||
            memcmp(data + key_start + key_length, kKeyClose, sizeof(kKeyClose) - 1) != 0) {
            continue;
        }

        size_t value_start = key_start + key_length + sizeof(kKeyClose) - 1;
        while (value_start < length && (data[value_start] == ' ' || data[value_start] == '\n' ||
                                        data[value_start] == '\r' || data[value_start] == '\t')) {
            ++value_start;
        }
        if ((length - value_start) < sizeof(kStringOpen) - 1 ||
            memcmp(data + value_start, kStringOpen, sizeof(kStringOpen) - 1) != 0) {
            return false;
        }
        value_start += sizeof(kStringOpen) - 1;
        for (size_t value_end = value_start; value_end < length; ++value_end) {
            if ((length - value_end) >= sizeof(kStringClose) - 1 &&
                memcmp(data + value_end, kStringClose, sizeof(kStringClose) - 1) == 0) {
                const size_t value_length = value_end - value_start;
                if (value_length >= value_capacity) {
                    return false;
                }
                memcpy(value, data + value_start, value_length);
                value[value_length] = '\0';
                return true;
            }
        }
        return false;
    }
    return false;
}

bool isXmlWhitespace(uint8_t value)
{
    return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

bool findXmlValueAfterKey(const uint8_t *data, size_t length, const char *key,
                          size_t *value_offset)
{
    if (data == nullptr || key == nullptr || value_offset == nullptr) return false;
    static constexpr char kKeyOpen[] = "<key>";
    static constexpr char kKeyClose[] = "</key>";
    const size_t key_length = strlen(key);
    const size_t token_length = sizeof(kKeyOpen) - 1 + key_length + sizeof(kKeyClose) - 1;
    for (size_t index = 0; index + token_length <= length; ++index) {
        if (memcmp(data + index, kKeyOpen, sizeof(kKeyOpen) - 1) != 0) continue;
        const size_t key_start = index + sizeof(kKeyOpen) - 1;
        if (memcmp(data + key_start, key, key_length) != 0 ||
            memcmp(data + key_start + key_length, kKeyClose, sizeof(kKeyClose) - 1) != 0) continue;
        size_t offset = index + token_length;
        while (offset < length && isXmlWhitespace(data[offset])) ++offset;
        *value_offset = offset;
        return true;
    }
    return false;
}

bool extractXmlUint16ForKey(const uint8_t *data, size_t length, const char *key, uint16_t *value)
{
    static constexpr char kOpen[] = "<integer>";
    static constexpr char kClose[] = "</integer>";
    size_t offset = 0;
    if (value == nullptr || !findXmlValueAfterKey(data, length, key, &offset) ||
        offset + sizeof(kOpen) - 1 >= length ||
        memcmp(data + offset, kOpen, sizeof(kOpen) - 1) != 0) return false;
    offset += sizeof(kOpen) - 1;
    uint32_t parsed = 0;
    size_t digits = 0;
    while (offset < length && data[offset] >= '0' && data[offset] <= '9') {
        parsed = parsed * 10 + static_cast<uint32_t>(data[offset] - '0');
        if (parsed > UINT16_MAX) return false;
        ++offset;
        ++digits;
    }
    if (digits == 0 || offset + sizeof(kClose) - 1 > length ||
        memcmp(data + offset, kClose, sizeof(kClose) - 1) != 0) return false;
    *value = static_cast<uint16_t>(parsed);
    return true;
}

bool extractXmlBoolForKey(const uint8_t *data, size_t length, const char *key, bool *value)
{
    size_t offset = 0;
    if (value == nullptr || !findXmlValueAfterKey(data, length, key, &offset)) return false;
    static constexpr char kTrue[] = "<true/>";
    static constexpr char kFalse[] = "<false/>";
    if (offset + sizeof(kTrue) - 1 <= length &&
        memcmp(data + offset, kTrue, sizeof(kTrue) - 1) == 0) {
        *value = true;
        return true;
    }
    if (offset + sizeof(kFalse) - 1 <= length &&
        memcmp(data + offset, kFalse, sizeof(kFalse) - 1) == 0) {
        *value = false;
        return true;
    }
    return false;
}

bool extractXmlBase64DataForKey(const uint8_t *data, size_t length, const char *key,
                                size_t *base64_chars, size_t *decoded_bytes)
{
    if (data == nullptr || key == nullptr || base64_chars == nullptr || decoded_bytes == nullptr) {
        return false;
    }

    static constexpr char kKeyOpen[] = "<key>";
    static constexpr char kKeyClose[] = "</key>";
    static constexpr char kDataOpen[] = "<data>";
    static constexpr char kDataClose[] = "</data>";
    const size_t key_length = strlen(key);
    for (size_t index = 0; index < length; ++index) {
        if ((length - index) < (sizeof(kKeyOpen) - 1 + key_length + sizeof(kKeyClose) - 1) ||
            memcmp(data + index, kKeyOpen, sizeof(kKeyOpen) - 1) != 0) {
            continue;
        }
        const size_t key_start = index + sizeof(kKeyOpen) - 1;
        if (memcmp(data + key_start, key, key_length) != 0 ||
            memcmp(data + key_start + key_length, kKeyClose, sizeof(kKeyClose) - 1) != 0) {
            continue;
        }

        size_t value_start = key_start + key_length + sizeof(kKeyClose) - 1;
        while (value_start < length && isXmlWhitespace(data[value_start])) {
            ++value_start;
        }
        if ((length - value_start) < sizeof(kDataOpen) - 1 ||
            memcmp(data + value_start, kDataOpen, sizeof(kDataOpen) - 1) != 0) {
            return false;
        }
        value_start += sizeof(kDataOpen) - 1;

        size_t value_end = value_start;
        while (value_end < length) {
            if ((length - value_end) >= sizeof(kDataClose) - 1 &&
                memcmp(data + value_end, kDataClose, sizeof(kDataClose) - 1) == 0) {
                break;
            }
            ++value_end;
        }
        if ((length - value_end) < sizeof(kDataClose) - 1) {
            return false;
        }

        size_t chars = 0;
        size_t padding = 0;
        bool saw_padding = false;
        for (size_t cursor = value_start; cursor < value_end; ++cursor) {
            const uint8_t value = data[cursor];
            if (isXmlWhitespace(value)) {
                continue;
            }
            const bool base64 = (value >= 'A' && value <= 'Z') ||
                                (value >= 'a' && value <= 'z') ||
                                (value >= '0' && value <= '9') || value == '+' || value == '/';
            if (value == '=') {
                saw_padding = true;
                ++padding;
                if (padding > 2) {
                    return false;
                }
            } else if (!base64 || saw_padding) {
                return false;
            }
            ++chars;
        }
        if (chars == 0 || (chars % 4) != 0) {
            return false;
        }
        *base64_chars = chars;
        *decoded_bytes = (chars / 4) * 3 - padding;
        return true;
    }
    return false;
}

bool decodeXmlBase64DataForKey(const uint8_t *data, size_t length, const char *key,
                               uint8_t *decoded, size_t decoded_capacity, size_t *decoded_bytes)
{
    if (data == nullptr || key == nullptr || decoded == nullptr || decoded_bytes == nullptr) {
        return false;
    }

    static constexpr char kKeyOpen[] = "<key>";
    static constexpr char kKeyClose[] = "</key>";
    static constexpr char kDataOpen[] = "<data>";
    static constexpr char kDataClose[] = "</data>";
    const size_t key_length = strlen(key);
    for (size_t index = 0; index < length; ++index) {
        if ((length - index) < (sizeof(kKeyOpen) - 1 + key_length + sizeof(kKeyClose) - 1) ||
            memcmp(data + index, kKeyOpen, sizeof(kKeyOpen) - 1) != 0) {
            continue;
        }
        const size_t key_start = index + sizeof(kKeyOpen) - 1;
        if (memcmp(data + key_start, key, key_length) != 0 ||
            memcmp(data + key_start + key_length, kKeyClose, sizeof(kKeyClose) - 1) != 0) {
            continue;
        }
        size_t value_start = key_start + key_length + sizeof(kKeyClose) - 1;
        while (value_start < length && isXmlWhitespace(data[value_start])) {
            ++value_start;
        }
        if ((length - value_start) < sizeof(kDataOpen) - 1 ||
            memcmp(data + value_start, kDataOpen, sizeof(kDataOpen) - 1) != 0) {
            return false;
        }
        value_start += sizeof(kDataOpen) - 1;
        size_t value_end = value_start;
        while (value_end < length && ((length - value_end) < sizeof(kDataClose) - 1 ||
                                      memcmp(data + value_end, kDataClose, sizeof(kDataClose) - 1) != 0)) {
            ++value_end;
        }
        if ((length - value_end) < sizeof(kDataClose) - 1) {
            return false;
        }

        constexpr size_t kBase64WorkspaceSize = 4096;
        uint8_t *base64 = static_cast<uint8_t *>(heap_caps_malloc(kBase64WorkspaceSize, MALLOC_CAP_8BIT));
        if (base64 == nullptr) {
            printf("[HCC LOCKDOWN] DevicePublicKey base64 workspace allocation failed\n");
            return false;
        }

        bool decoded_successfully = false;
        bool workspace_overflow = false;
        size_t base64_length = 0;
        for (size_t cursor = value_start; cursor < value_end; ++cursor) {
            if (!isXmlWhitespace(data[cursor])) {
                if (base64_length == kBase64WorkspaceSize) {
                    workspace_overflow = true;
                    break;
                }
                base64[base64_length++] = data[cursor];
            }
        }
        if (!workspace_overflow) {
            size_t output_length = 0;
            const int ret = mbedtls_base64_decode(decoded, decoded_capacity, &output_length,
                                                  base64, base64_length);
            if (ret == 0) {
                *decoded_bytes = output_length;
                decoded_successfully = true;
            }
        }
        mbedtls_platform_zeroize(base64, kBase64WorkspaceSize);
        heap_caps_free(base64);
        return decoded_successfully;
    }
    return false;
}

bool appendBounded(uint8_t *output, size_t capacity, size_t *length, const char *text)
{
    const size_t text_length = strlen(text);
    if (output == nullptr || length == nullptr || text_length > capacity - *length) {
        return false;
    }
    memcpy(output + *length, text, text_length);
    *length += text_length;
    return true;
}

bool appendXmlString(uint8_t *output, size_t capacity, size_t *length, const char *value)
{
    if (!appendBounded(output, capacity, length, "<string>")) return false;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        const char *encoded = nullptr;
        switch (*cursor) {
        case '&': encoded = "&amp;"; break;
        case '<': encoded = "&lt;"; break;
        case '>': encoded = "&gt;"; break;
        case '\"': encoded = "&quot;"; break;
        case '\'': encoded = "&apos;"; break;
        default: break;
        }
        if (encoded != nullptr) {
            if (!appendBounded(output, capacity, length, encoded)) return false;
        } else {
            if (*length == capacity) return false;
            output[(*length)++] = static_cast<uint8_t>(*cursor);
        }
    }
    return appendBounded(output, capacity, length, "</string>\n");
}

bool appendXmlData(uint8_t *output, size_t capacity, size_t *length,
                   const HCCPairPemBuffer &pem)
{
    if (!appendBounded(output, capacity, length, "<data>")) return false;
    size_t encoded_length = 0;
    const int ret = mbedtls_base64_encode(output + *length, capacity - *length,
                                          &encoded_length, pem.data, pem.length);
    if (ret != 0 || encoded_length > capacity - *length) return false;
    *length += encoded_length;
    return appendBounded(output, capacity, length, "</data>\n");
}

} // namespace

struct HCCUSBTransport::TlsContext {
    enum class AppState {
        Idle,
        StartServiceWrite,
        StartServiceRead,
        StartServicePassed,
        PeerClosed,
        Failed,
    };

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt root_certificate;
    mbedtls_pk_context root_private_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    uint8_t *rx_fifo;
    size_t rx_head;
    size_t rx_length;
    uint8_t *tx_staging;
    size_t tx_length;
    size_t tx_offset;
    uint8_t *lockdown_tx;
    size_t lockdown_tx_length;
    size_t lockdown_tx_offset;
    uint8_t *lockdown_rx;
    size_t lockdown_rx_length;
    bool lockdown_tx_waiting_for_ciphertext;
    AppState app_state;
};

bool HCCUSBTransport::start(usb_device_handle_t device, uint8_t out_endpoint, uint8_t in_endpoint, uint16_t in_mps)
{
    if (device == nullptr || out_endpoint == 0 || in_endpoint == 0 || in_mps == 0 || hasPendingTransfers()) {
        _state = State::Failed;
        printf("[HCC USB MUX] VERSION failed: transport is not idle\n");
        return false;
    }

    destroyPairState();
    _device = device;
    _out_endpoint = out_endpoint;
    _in_endpoint = in_endpoint;
    _rx_buffer_size = ((kRxMinimumBufferSize + in_mps - 1) / in_mps) * in_mps;
    _stopping = false;
    _v2_framing = false;
    _setup_requested = false;
    _tcp_syn_requested = false;
    _tcp_ack_requested = false;
    _lockdown_prefix_requested = false;
    _lockdown_body_requested = false;
    _tcp_data_ack_requested = false;
    _getvalue_product_version_after_ack = false;
    _getvalue_product_version_prefix_requested = false;
    _getvalue_product_version_body_requested = false;
    _getvalue_device_class_after_ack = false;
    _getvalue_device_class_prefix_requested = false;
    _getvalue_device_class_body_requested = false;
    _getvalue_device_public_key_after_ack = false;
    _getvalue_device_public_key_prefix_requested = false;
    _getvalue_device_public_key_body_requested = false;
    _getvalue_wifi_address_after_ack = false;
    _getvalue_wifi_address_prefix_requested = false;
    _getvalue_wifi_address_body_requested = false;
    _wifi_address_valid = false;
    mbedtls_platform_zeroize(_wifi_address.data(), _wifi_address.size());
    _lockdown_expected_response = LockdownResponse::None;
    _tx_seq = 0;
    _rx_seq = 0;
    _next_source_port = 1;
    _tcp_source_port = 0;
    _tcp_destination_port = kLockdownPort;
    _tcp_tx_seq = 0;
    _tcp_ack_seq = 0;
    _tcp_peer_seq = 0;
    _tcp_peer_ack = 0;
    _tcp_peer_window = 0;
    _primary_connection_epoch = 0;
    _query_type_epoch = 0;
    _pair_authorized_epoch = 0;
    _primary_tcp_closed = false;
    _primary_trust_fin_requested = false;
    _primary_trust_fin_dispatched = false;
    _primary_trust_close_ack_requested = false;
    _primary_reconnect_pending = false;
    _primary_reconnect_attempted = false;
    _trust_pair_retry_requested = false;
    _session_reconnect_pending = false;
    _session_reconnect_attempted = false;
    _pair_record_rejected = false;
    _repair_reconnect_pending = false;
    _repair_attempted = false;
    _primary_connection_purpose = PrimaryConnectionPurpose::InitialDiscovery;
    _pair_after_querytype_ack = false;
    _pair_success_epoch = 0;
    clearStartSessionConnectionState(false);
    resetServiceTcpContext();
    _rx_stream_length = 0;
    _lockdown_rx_stream_length = 0;
    mbedtls_platform_zeroize(_device_public_key.data(), _device_public_key.size());
    _device_public_key_length = 0;
    _device_public_key_pending = false;
    _device_public_key_wait_logged = false;
    _device_public_key_wakeup_logged = false;
    _device_public_key_utc_ready_logged = false;
    _dwc_out_channel = -1;
    _dwc_device_address = 0;
    _dwc_delayed_snapshot_due = 0;
    _dwc_delayed_snapshot_pending = false;
    _tx_kind = TxKind::None;
    _state = State::WaitingVersion;
    _tcp_state = TcpState::Inactive;

    esp_err_t ret = ESP_OK;
    static constexpr const char *kRxHeapSuccess[] = {"AFTER RX0", "AFTER RX1", "AFTER RX2"};
    static constexpr const char *kRxHeapFailure[] = {"RX0 FAIL", "RX1 FAIL", "RX2 FAIL"};
    logUsbAllocationHeap("BEFORE RX");
    for (size_t slot = 0; slot < kRxTransferCount; ++slot) {
        // RX transfers persist across devices so the DMA heap is not re-fragmented on every
        // reconnect. hasPendingTransfers() above guarantees every slot is fully retired here.
        if (_rx_transfers[slot] != nullptr && _rx_transfers[slot]->data_buffer_size < _rx_buffer_size) {
            usb_host_transfer_free(_rx_transfers[slot]);
            _rx_transfers[slot] = nullptr;
        }
        if (_rx_transfers[slot] != nullptr) {
            printf("[HCC USB MUX] RX[%u] reusing persistent transfer\n", static_cast<unsigned>(slot));
        } else {
            ret = usb_host_transfer_alloc(_rx_buffer_size, 0, &_rx_transfers[slot]);
            if (ret != ESP_OK || _rx_transfers[slot] == nullptr) {
                _rx_transfers[slot] = nullptr;
                logUsbAllocationHeap(kRxHeapFailure[slot]);
                _state = State::Failed;
                printf("[HCC USB MUX] VERSION failed: RX[%u] alloc: %s\n",
                       static_cast<unsigned>(slot), esp_err_to_name(ret));
                return false;
            }
            logUsbAllocationHeap(kRxHeapSuccess[slot]);
        }

        _rx_transfers[slot]->num_bytes = _rx_buffer_size;
        _rx_transfers[slot]->device_handle = _device;
        _rx_transfers[slot]->bEndpointAddress = _in_endpoint;
        _rx_transfers[slot]->callback = HCCUSBTransport::onRxComplete;
        _rx_transfers[slot]->context = this;
    }

    ret = usb_host_transfer_alloc(kVersionPacketSize, 0, &_tx_transfer);
    if (ret != ESP_OK || _tx_transfer == nullptr) {
        _state = State::Failed;
        printf("[HCC USB MUX] VERSION failed: TX alloc: %s\n", esp_err_to_name(ret));
        return false;
    }

    _tx_transfer->num_bytes = kVersionPacketSize;
    _tx_transfer->device_handle = _device;
    _tx_transfer->bEndpointAddress = _out_endpoint;
    _tx_transfer->callback = HCCUSBTransport::onTxComplete;
    _tx_transfer->context = this;
    _tx_kind = TxKind::Version;

    static constexpr uint8_t kVersionRequest[kVersionPacketSize] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    memcpy(_tx_transfer->data_buffer, kVersionRequest, sizeof(kVersionRequest));

    printf("[HCC USB MUX] Starting transport: OUT=0x%02X IN=0x%02X RX=%u\n",
           _out_endpoint, _in_endpoint, static_cast<unsigned>(_rx_buffer_size));

    ret = usb_host_transfer_submit(_tx_transfer);
    if (ret != ESP_OK) {
        _state = State::Failed;
        printf("[HCC USB MUX] VERSION failed: TX submit: %s\n", esp_err_to_name(ret));
        usb_host_transfer_free(_tx_transfer);
        _tx_transfer = nullptr;
        return false;
    }

    _tx_in_flight = true;
    printf("[HCC USB MUX] TX VERSION 2.0\n");

    for (size_t slot = 0; slot < kRxTransferCount; ++slot) {
        if (submitRx(slot)) {
            continue;
        }

        _state = State::Failed;
        stopForShutdown();
        return false;
    }

    return true;
}

bool HCCUSBTransport::submitPendingSetup()
{
    if (!_setup_requested || _stopping || _state == State::Failed) {
        return true;
    }

    // VERSION TX may be retired by a later client event. Keep SETUP pending
    // until the USB client task sees that the TX object is no longer in flight.
    if (_tx_in_flight) {
        return true;
    }

    return sendSetup();
}

bool HCCUSBTransport::submitPendingTcp()
{
    if (_stopping || _state == State::Failed || _tx_in_flight) {
        return true;
    }

    if (_session_reconnect_pending) {
        _primary_connection_purpose = PrimaryConnectionPurpose::SessionReconnect;
        resetPrimaryTcpForPairReconnect();
        _session_reconnect_pending = false;
        _session_reconnect_attempted = true;
        _tcp_syn_requested = true;
    } else if (_primary_reconnect_pending) {
        _primary_connection_purpose = PrimaryConnectionPurpose::PairReconnect;
        resetPrimaryTcpForPairReconnect();
        _primary_reconnect_pending = false;
        _primary_reconnect_attempted = true;
        _tcp_syn_requested = true;
    } else if (_repair_reconnect_pending) {
        // No record is loaded, so QueryType on this connection takes the fresh pairing path.
        _primary_connection_purpose = PrimaryConnectionPurpose::InitialDiscovery;
        resetPrimaryTcpForPairReconnect();
        _repair_reconnect_pending = false;
        _repair_attempted = true;
        _pair_record_rejected = false;
        _tcp_syn_requested = true;
        printf("[HCC PAIR] reconnecting for fresh pairing\n");
    }

    // Preserve the established Lockdown stream first: acknowledge its latest
    // ciphertext before using the shared USB TX pipe for TCP #2.
    if (_tcp_state == TcpState::Connected && _tcp_data_ack_requested) {
        return sendTcpDataAck();
    }

    // TCP #2 shares the USBMUX TX pipe and v2 sequence numbers, but owns an
    // independent TCP tuple and sequence space. Give its handshake a deferred
    // TX opportunity without disturbing the established Lockdown connection.
    if (_service_tcp.syn_requested) {
        return sendServiceTcpSyn();
    }
    if (_service_tcp.ack_requested) {
        return sendServiceTcpAck();
    }
    if (_primary_trust_fin_requested) {
        return sendPrimaryTrustFin();
    }
    if (_primary_trust_close_ack_requested) {
        return sendPrimaryTrustCloseAck();
    }
    if (_service_tcp.notification_prefix_requested) {
        return sendNotificationPrefix();
    }
    if (_service_tcp.notification_body_requested) {
        return sendNotificationBody();
    }

    if (_tcp_state == TcpState::Failed) {
        return true;
    }

    if (_tls_state != TlsState::PlainLockdown) {
        return processTlsDeferred();
    }

    if (_tcp_syn_requested) {
        return sendTcpSyn();
    }
    if (_tcp_ack_requested) {
        return sendTcpAck();
    }
    if (_lockdown_prefix_requested) {
        return sendLockdownQueryTypePrefix();
    }
    if (_lockdown_body_requested) {
        return sendLockdownQueryTypeBody();
    }
    if (_tcp_data_ack_requested) {
        return sendTcpDataAck();
    }
    if (_getvalue_product_version_prefix_requested) {
        return sendGetValueProductVersionPrefix();
    }
    if (_getvalue_product_version_body_requested) {
        return sendGetValueProductVersionBody();
    }
    if (_getvalue_device_class_prefix_requested) {
        return sendGetValueDeviceClassPrefix();
    }
    if (_getvalue_device_class_body_requested) {
        return sendGetValueDeviceClassBody();
    }
    if (_getvalue_device_public_key_prefix_requested) {
        return sendGetValueDevicePublicKeyPrefix();
    }
    if (_getvalue_device_public_key_body_requested) {
        return sendGetValueDevicePublicKeyBody();
    }
    if (_getvalue_wifi_address_prefix_requested) {
        return sendGetValueWiFiAddressPrefix();
    }
    if (_getvalue_wifi_address_body_requested) {
        return sendGetValueWiFiAddressBody();
    }
    if (_pair_prefix_requested) {
        return sendPairPrefix();
    }
    if (_pair_chunk_requested) {
        return sendPairChunk();
    }
    if (_start_notification_service_prefix_requested) {
        return sendStartNotificationServicePrefix();
    }
    if (_start_notification_service_body_requested) {
        return sendStartNotificationServiceBody();
    }
    if (_start_session_prefix_requested) {
        return sendStartSessionPrefix();
    }
    if (_start_session_body_requested) {
        return sendStartSessionBody();
    }
    return true;
}

bool HCCUSBTransport::sendSetup()
{
    if (_stopping || !_v2_framing || _tx_transfer != nullptr || _tx_in_flight) {
        return false;
    }

    esp_err_t ret = usb_host_transfer_alloc(kSetupPacketSize, 0, &_tx_transfer);
    if (ret != ESP_OK || _tx_transfer == nullptr) {
        _state = State::Failed;
        printf("[HCC USB MUX] SETUP failed: TX alloc: %s\n", esp_err_to_name(ret));
        return false;
    }

    _tx_transfer->num_bytes = kSetupPacketSize;
    _tx_transfer->device_handle = _device;
    _tx_transfer->bEndpointAddress = _out_endpoint;
    _tx_transfer->callback = HCCUSBTransport::onTxComplete;
    _tx_transfer->context = this;

    // Raw usbmux v2 SETUP: 16-byte mux header followed by payload 0x07.
    writeBigEndianU32(_tx_transfer->data_buffer, 2);
    writeBigEndianU32(_tx_transfer->data_buffer + 4, kSetupPacketSize);
    writeBigEndianU32(_tx_transfer->data_buffer + 8, 0xFEEDFACE);
    writeBigEndianU16(_tx_transfer->data_buffer + 12, _tx_seq);
    writeBigEndianU16(_tx_transfer->data_buffer + 14, _rx_seq);
    _tx_transfer->data_buffer[16] = 0x07;

    _tx_kind = TxKind::Setup;
    ret = usb_host_transfer_submit(_tx_transfer);
    printf("[HCC USB MUX] SETUP submit: ret=%d (%s)\n", ret, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        printf("[HCC USB MUX] SETUP failed: TX submit: %s\n", esp_err_to_name(ret));
        usb_host_transfer_free(_tx_transfer);
        _tx_transfer = nullptr;
        _tx_kind = TxKind::None;
        _state = State::Failed;
        return false;
    }

    printf("[HCC USB MUX] TX SETUP: protocol=2 length=%u magic=0xFEEDFACE tx_seq=%u rx_seq=%u payload=0x07\n",
           static_cast<unsigned>(kSetupPacketSize), _tx_seq, _rx_seq);
    ++_tx_seq;
    _tx_in_flight = true;
    _setup_requested = false;
    _state = State::SetupSent;
    beginDwcSetupDiagnostic();
    return true;
}

uint16_t HCCUSBTransport::allocateSourcePort()
{
    // Phase 2A has exactly one connection, but preserve usbmuxd's non-zero,
    // monotonically advancing source-port allocation rule.
    if (_tcp_source_port != 0) {
        return _tcp_source_port;
    }
    if (_next_source_port == 0) {
        _next_source_port = 1;
    }
    _tcp_source_port = _next_source_port++;
    if (_next_source_port == 0) {
        _next_source_port = 1;
    }
    return _tcp_source_port;
}

uint16_t HCCUSBTransport::allocateServiceSourcePort()
{
    if (_service_tcp.source_port != 0) {
        return _service_tcp.source_port;
    }
    for (uint32_t attempts = 0; attempts < UINT16_MAX; ++attempts) {
        if (_next_source_port == 0) {
            _next_source_port = 1;
        }
        const uint16_t candidate = _next_source_port++;
        if (candidate != 0 && candidate != _tcp_source_port) {
            _service_tcp.source_port = candidate;
            return candidate;
        }
    }
    return 0;
}

void HCCUSBTransport::resetServiceTcpContext()
{
    _service_tcp = ServiceTcpContext{};
    _notification_tcp_after_ack = false;
    _notification_rx_stream_length = 0;
    mbedtls_platform_zeroize(_notification_rx_stream.data(), _notification_rx_stream.size());
}

bool HCCUSBTransport::sendTcpSyn()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Inactive) {
        return false;
    }

    const uint16_t source_port = allocateSourcePort();
    if (source_port == 0) {
        _tcp_state = TcpState::Failed;
        _state = State::Failed;
        return false;
    }

    ++_primary_connection_epoch;
    if (_primary_connection_epoch == 0) {
        ++_primary_connection_epoch;
    }

    if (_primary_connection_purpose == PrimaryConnectionPurpose::PairReconnect) {
        printf("[HCC LOCKDOWN] reconnecting port=62078 source_port=%u epoch=%u\n",
               source_port, static_cast<unsigned>(_primary_connection_epoch));
    } else if (_primary_connection_purpose == PrimaryConnectionPurpose::SessionReconnect) {
        printf("[HCC LOCKDOWN] reconnecting for StartSession port=62078 source_port=%u epoch=%u\n",
               source_port, static_cast<unsigned>(_primary_connection_epoch));
    }

    if (!sendTcpPacket(TxKind::TcpSyn, 0x02)) {
        return false;
    }

    _tcp_syn_requested = false;
    _tcp_state = TcpState::SynSent;
    return true;
}

bool HCCUSBTransport::sendTcpAck()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::SynSent) {
        return false;
    }
    if (!sendTcpPacket(TxKind::TcpAck, 0x10)) {
        return false;
    }

    _tcp_ack_requested = false;
    return true;
}

bool HCCUSBTransport::sendServiceTcpSyn()
{
    if (_state != State::MuxActive || _service_tcp.state != TcpState::Inactive ||
        _service_tcp.destination_port == 0 || _service_tcp.generation == 0 ||
        _service_tcp.generation != _notification_service_generation) {
        _service_tcp.state = TcpState::Failed;
        return false;
    }
    const uint16_t source_port = allocateServiceSourcePort();
    if (source_port == 0 || source_port == _tcp_source_port) {
        _service_tcp.state = TcpState::Failed;
        return false;
    }
    ++_service_tcp.epoch;
    if (_service_tcp.epoch == 0) ++_service_tcp.epoch;
    printf("[HCC SERVICE] connecting port=%u source_port=%u\n",
           static_cast<unsigned>(_service_tcp.destination_port),
           static_cast<unsigned>(source_port));
    if (!sendServiceTcpPacket(TxKind::ServiceTcpSyn, 0x02)) return false;
    _service_tcp.syn_requested = false;
    _service_tcp.state = TcpState::SynSent;
    return true;
}

bool HCCUSBTransport::sendServiceTcpAck()
{
    if (_state != State::MuxActive ||
        (_service_tcp.state != TcpState::SynSent &&
         _service_tcp.state != TcpState::Connected)) {
        _service_tcp.state = TcpState::Failed;
        return false;
    }
    if (!sendServiceTcpPacket(TxKind::ServiceTcpAck, 0x10)) return false;
    _service_tcp.ack_requested = false;
    return true;
}

bool HCCUSBTransport::sendPrimaryTrustFin()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected ||
        _pair_trust_state != PairTrustState::NotificationServiceReady) {
        return false;
    }
    printf("[HCC NOTIFY] closing primary Lockdown before ObserveNotification\n");
    if (!sendTcpPacket(TxKind::PrimaryTrustFin, 0x11)) return false;
    _primary_trust_fin_requested = false;
    return true;
}

bool HCCUSBTransport::sendPrimaryTrustCloseAck()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected ||
        !_primary_trust_fin_dispatched) {
        return false;
    }
    if (!sendTcpPacket(TxKind::PrimaryTrustCloseAck, 0x10)) return false;
    _primary_trust_close_ack_requested = false;
    return true;
}

bool HCCUSBTransport::sendNotificationPrefix()
{
    if (_service_tcp.state != TcpState::Connected ||
        _service_tcp.notification_index >=
            (sizeof(kNotificationObserveRequests) / sizeof(kNotificationObserveRequests[0]))) {
        _service_tcp.state = TcpState::Failed;
        return false;
    }
    uint8_t prefix[sizeof(uint32_t)] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(
        kNotificationObserveRequests[_service_tcp.notification_index].length));
    if (!sendServiceTcpPacket(TxKind::NotificationPrefix, 0x10,
                              prefix, sizeof(prefix))) {
        return false;
    }
    _service_tcp.notification_prefix_requested = false;
    return true;
}

bool HCCUSBTransport::sendNotificationBody()
{
    if (_service_tcp.state != TcpState::Connected ||
        _service_tcp.notification_index >=
            (sizeof(kNotificationObserveRequests) / sizeof(kNotificationObserveRequests[0]))) {
        _service_tcp.state = TcpState::Failed;
        return false;
    }
    const NotificationObserveRequest &request =
        kNotificationObserveRequests[_service_tcp.notification_index];
    if (!sendServiceTcpPacket(TxKind::NotificationBody, 0x10,
                              reinterpret_cast<const uint8_t *>(request.xml), request.length)) {
        return false;
    }
    _service_tcp.notification_body_requested = false;
    return true;
}

bool HCCUSBTransport::sendLockdownQueryTypePrefix()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(kLockdownQueryTypeBodySize));
    if (!sendTcpPacket(TxKind::LockdownPrefix, 0x10, prefix, sizeof(prefix))) {
        return false;
    }

    _lockdown_prefix_requested = false;
    printf("[HCC LOCKDOWN] QueryType prefix submit: body_len=%u\n",
           static_cast<unsigned>(kLockdownQueryTypeBodySize));
    return true;
}

bool HCCUSBTransport::sendLockdownQueryTypeBody()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    if (!sendTcpPacket(TxKind::LockdownBody, 0x10,
                       reinterpret_cast<const uint8_t *>(kLockdownQueryTypeXml),
                       kLockdownQueryTypeBodySize)) {
        return false;
    }

    _lockdown_body_requested = false;
    printf("[HCC LOCKDOWN] QueryType XML body submit: body_len=%u\n",
           static_cast<unsigned>(kLockdownQueryTypeBodySize));
    return true;
}

bool HCCUSBTransport::sendTcpDataAck()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }
    if (!sendTcpPacket(TxKind::TcpDataAck, 0x10)) {
        return false;
    }

    _tcp_data_ack_requested = false;
    return true;
}

bool HCCUSBTransport::sendGetValueProductVersionPrefix()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(kLockdownGetValueProductVersionBodySize));
    if (!sendTcpPacket(TxKind::GetValueProductVersionPrefix, 0x10, prefix, sizeof(prefix))) {
        return false;
    }

    _getvalue_product_version_prefix_requested = false;
    printf("[HCC LOCKDOWN] GetValue ProductVersion prefix submit: body_len=%u\n",
           static_cast<unsigned>(kLockdownGetValueProductVersionBodySize));
    return true;
}

bool HCCUSBTransport::sendGetValueProductVersionBody()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    if (!sendTcpPacket(TxKind::GetValueProductVersionBody, 0x10,
                       reinterpret_cast<const uint8_t *>(kLockdownGetValueProductVersionXml),
                       kLockdownGetValueProductVersionBodySize)) {
        return false;
    }

    _getvalue_product_version_body_requested = false;
    printf("[HCC LOCKDOWN] GetValue ProductVersion XML body submit: body_len=%u\n",
           static_cast<unsigned>(kLockdownGetValueProductVersionBodySize));
    return true;
}

bool HCCUSBTransport::sendGetValueDeviceClassPrefix()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(kLockdownGetValueDeviceClassBodySize));
    const uint16_t mux_tx_seq = _tx_seq;
    const uint16_t mux_rx_seq = _rx_seq;
    const uint32_t tcp_seq = _tcp_tx_seq;
    const uint32_t tcp_ack = _tcp_ack_seq;
    if (!sendTcpPacket(TxKind::GetValueDeviceClassPrefix, 0x10, prefix, sizeof(prefix))) {
        return false;
    }

    _getvalue_device_class_prefix_requested = false;
    printf("[HCC LOCKDOWN] GetValue DeviceClass prefix submit: mux_tx=%u mux_rx=%u seq=%u ack=%u body_len=%u\n",
           mux_tx_seq, mux_rx_seq, tcp_seq, tcp_ack,
           static_cast<unsigned>(kLockdownGetValueDeviceClassBodySize));
    return true;
}

bool HCCUSBTransport::sendGetValueDeviceClassBody()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    const uint16_t mux_tx_seq = _tx_seq;
    const uint16_t mux_rx_seq = _rx_seq;
    const uint32_t tcp_seq = _tcp_tx_seq;
    const uint32_t tcp_ack = _tcp_ack_seq;
    if (!sendTcpPacket(TxKind::GetValueDeviceClassBody, 0x10,
                       reinterpret_cast<const uint8_t *>(kLockdownGetValueDeviceClassXml),
                       kLockdownGetValueDeviceClassBodySize)) {
        return false;
    }

    _getvalue_device_class_body_requested = false;
    printf("[HCC LOCKDOWN] GetValue DeviceClass XML body submit: mux_tx=%u mux_rx=%u seq=%u ack=%u body_len=%u\n",
           mux_tx_seq, mux_rx_seq, tcp_seq, tcp_ack,
           static_cast<unsigned>(kLockdownGetValueDeviceClassBodySize));
    return true;
}

bool HCCUSBTransport::sendGetValueDevicePublicKeyPrefix()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(kLockdownGetValueDevicePublicKeyBodySize));
    const uint16_t mux_tx_seq = _tx_seq;
    const uint16_t mux_rx_seq = _rx_seq;
    const uint32_t tcp_seq = _tcp_tx_seq;
    const uint32_t tcp_ack = _tcp_ack_seq;
    if (!sendTcpPacket(TxKind::GetValueDevicePublicKeyPrefix, 0x10, prefix, sizeof(prefix))) {
        return false;
    }

    _getvalue_device_public_key_prefix_requested = false;
    printf("[HCC LOCKDOWN] GetValue DevicePublicKey prefix submit: mux_tx=%u mux_rx=%u seq=%u ack=%u body_len=%u\n",
           mux_tx_seq, mux_rx_seq, tcp_seq, tcp_ack,
           static_cast<unsigned>(kLockdownGetValueDevicePublicKeyBodySize));
    return true;
}

bool HCCUSBTransport::sendGetValueDevicePublicKeyBody()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    const uint16_t mux_tx_seq = _tx_seq;
    const uint16_t mux_rx_seq = _rx_seq;
    const uint32_t tcp_seq = _tcp_tx_seq;
    const uint32_t tcp_ack = _tcp_ack_seq;
    if (!sendTcpPacket(TxKind::GetValueDevicePublicKeyBody, 0x10,
                       reinterpret_cast<const uint8_t *>(kLockdownGetValueDevicePublicKeyXml),
                       kLockdownGetValueDevicePublicKeyBodySize)) {
        return false;
    }

    _getvalue_device_public_key_body_requested = false;
    printf("[HCC LOCKDOWN] GetValue DevicePublicKey XML body submit: mux_tx=%u mux_rx=%u seq=%u ack=%u body_len=%u\n",
           mux_tx_seq, mux_rx_seq, tcp_seq, tcp_ack,
           static_cast<unsigned>(kLockdownGetValueDevicePublicKeyBodySize));
    return true;
}

bool HCCUSBTransport::sendGetValueWiFiAddressPrefix()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(kLockdownGetValueWiFiAddressBodySize));
    const uint16_t mux_tx_seq = _tx_seq;
    const uint16_t mux_rx_seq = _rx_seq;
    const uint32_t tcp_seq = _tcp_tx_seq;
    const uint32_t tcp_ack = _tcp_ack_seq;
    if (!sendTcpPacket(TxKind::GetValueWiFiAddressPrefix, 0x10, prefix, sizeof(prefix))) {
        return false;
    }

    _getvalue_wifi_address_prefix_requested = false;
    printf("[HCC LOCKDOWN] GetValue WiFiAddress prefix submit: mux_tx=%u mux_rx=%u seq=%u ack=%u body_len=%u\n",
           mux_tx_seq, mux_rx_seq, tcp_seq, tcp_ack,
           static_cast<unsigned>(kLockdownGetValueWiFiAddressBodySize));
    return true;
}

bool HCCUSBTransport::sendGetValueWiFiAddressBody()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected) {
        return false;
    }

    const uint16_t mux_tx_seq = _tx_seq;
    const uint16_t mux_rx_seq = _rx_seq;
    const uint32_t tcp_seq = _tcp_tx_seq;
    const uint32_t tcp_ack = _tcp_ack_seq;
    if (!sendTcpPacket(TxKind::GetValueWiFiAddressBody, 0x10,
                       reinterpret_cast<const uint8_t *>(kLockdownGetValueWiFiAddressXml),
                       kLockdownGetValueWiFiAddressBodySize)) {
        return false;
    }

    _getvalue_wifi_address_body_requested = false;
    printf("[HCC LOCKDOWN] GetValue WiFiAddress XML body submit: mux_tx=%u mux_rx=%u seq=%u ack=%u body_len=%u\n",
           mux_tx_seq, mux_rx_seq, tcp_seq, tcp_ack,
           static_cast<unsigned>(kLockdownGetValueWiFiAddressBodySize));
    return true;
}

bool HCCUSBTransport::buildPairXml(const char *host_id, const char *system_buid)
{
    _pair_xml = static_cast<uint8_t *>(heap_caps_calloc(1, kPairXmlMaximum, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_pair_xml == nullptr) return false;
    size_t length = 0;
    bool ok = appendBounded(_pair_xml, kPairXmlMaximum, &length,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n<key>PairRecord</key>\n<dict>\n"
        "<key>DeviceCertificate</key>\n") &&
        appendXmlData(_pair_xml, kPairXmlMaximum, &length, _pair_bundle->device_certificate) &&
        appendBounded(_pair_xml, kPairXmlMaximum, &length, "<key>HostCertificate</key>\n") &&
        appendXmlData(_pair_xml, kPairXmlMaximum, &length, _pair_bundle->host_certificate) &&
        appendBounded(_pair_xml, kPairXmlMaximum, &length, "<key>HostID</key>\n") &&
        appendXmlString(_pair_xml, kPairXmlMaximum, &length, host_id) &&
        appendBounded(_pair_xml, kPairXmlMaximum, &length, "<key>RootCertificate</key>\n") &&
        appendXmlData(_pair_xml, kPairXmlMaximum, &length, _pair_bundle->root_certificate) &&
        appendBounded(_pair_xml, kPairXmlMaximum, &length, "<key>SystemBUID</key>\n") &&
        appendXmlString(_pair_xml, kPairXmlMaximum, &length, system_buid) &&
        appendBounded(_pair_xml, kPairXmlMaximum, &length,
        "</dict>\n<key>Request</key>\n<string>Pair</string>\n"
        "<key>ProtocolVersion</key>\n<string>2</string>\n"
        "<key>PairingOptions</key>\n<dict>\n<key>ExtendedPairingErrors</key>\n<true/>\n"
        "</dict>\n</dict>\n</plist>");
    if (!ok || length == 0 || length > kPairXmlMaximum) {
        mbedtls_platform_zeroize(_pair_xml, kPairXmlMaximum);
        heap_caps_free(_pair_xml);
        _pair_xml = nullptr;
        return false;
    }
    _pair_xml_length = length;
    printf("[HCC PAIR] Pair XML ready body_len=%u\n", static_cast<unsigned>(length));
    return true;
}

bool HCCUSBTransport::loadPairRecordForReuse(const char *udid, uint32_t generation)
{
    if (udid == nullptr || udid[0] == '\0' || generation == 0) return false;
    const size_t wanted_udid_length = strnlen(udid, _session_udid.size());
    if (wanted_udid_length == 0 || wanted_udid_length >= _session_udid.size()) return false;

    nvs_handle_t handle = 0;
    size_t blob_size = 0;
    uint8_t *blob = nullptr;
    char record_key[16] = {};
    bool loaded_from_legacy = false;
    esp_err_t err = openPairRecordPartition(NVS_READONLY, &handle);
    if (err == ESP_OK) {
        PairRecordIndex *index = static_cast<PairRecordIndex *>(
            heap_caps_calloc(1, sizeof(PairRecordIndex), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint32_t slot = 0;
        if (index != nullptr && loadPairRecordIndex(handle, index) && findPairRecordSlot(*index, udid, &slot) &&
            makePairRecordSlotKey(slot, record_key, sizeof(record_key))) {
            err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
        } else {
            // Backward compatibility with the old single-slot HW-PASS record.
            snprintf(record_key, sizeof(record_key), "%s", kLegacyPairRecordKey);
            err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
        }
        if (index != nullptr) heap_caps_free(index);
    }
    if (err != ESP_OK) {
        if (handle != 0) { nvs_close(handle); handle = 0; }
        blob_size = 0;
        record_key[0] = '\0';
        err = nvs_open(kPairRecordNamespace, NVS_READONLY, &handle);
        if (err == ESP_OK) {
            PairRecordIndex *legacy_index = static_cast<PairRecordIndex *>(
                heap_caps_calloc(1, sizeof(PairRecordIndex), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            uint32_t legacy_slot = 0;
            if (legacy_index != nullptr && loadPairRecordIndex(handle, legacy_index) &&
                findPairRecordSlot(*legacy_index, udid, &legacy_slot) &&
                makePairRecordSlotKey(legacy_slot, record_key, sizeof(record_key))) {
                err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
            } else {
                snprintf(record_key, sizeof(record_key), "%s", kLegacyPairRecordKey);
                err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
            }
            if (legacy_index != nullptr) heap_caps_free(legacy_index);
            if (err == ESP_OK) {
                loaded_from_legacy = true;
                printf("[HCC PAIR] PAIR RECORD LEGACY FALLBACK\n");
            }
        }
    }
    if (err == ESP_OK && blob_size >= sizeof(PairRecordBlobHeader)) {
        blob = static_cast<uint8_t *>(heap_caps_malloc(blob_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (blob == nullptr) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = nvs_get_blob(handle, record_key, blob, &blob_size);
    if (handle != 0) nvs_close(handle);

    PairRecordBlobHeader h = {};
    bool valid = err == ESP_OK;
    if (valid) memcpy(&h, blob, sizeof(h));
    const size_t lengths[] = {h.udid_length, h.host_id_length, h.system_buid_length,
        h.root_private_key_length, h.host_private_key_length, h.root_certificate_length,
        h.host_certificate_length, h.device_certificate_length, h.wifi_mac_address_length,
        h.escrow_bag_length};
    size_t checked = sizeof(h);
    valid = valid && h.magic == kPairRecordMagic && h.version == kPairRecordVersion &&
            h.total_size == blob_size;
    for (size_t n : lengths) {
        if (n == 0 || checked > blob_size || n > blob_size - checked) { valid = false; break; }
        checked += n;
    }
    valid = valid && checked == blob_size && h.udid_length == wanted_udid_length &&
            h.host_id_length < _session_host_id.size() &&
            h.system_buid_length < _session_system_buid.size() &&
            memcmp(blob + sizeof(h), udid, wanted_udid_length) == 0;

    if (valid) {
        size_t off = sizeof(h) + h.udid_length;
        memcpy(_session_host_id.data(), blob + off, h.host_id_length);
        _session_host_id[h.host_id_length] = '\0'; off += h.host_id_length;
        memcpy(_session_system_buid.data(), blob + off, h.system_buid_length);
        _session_system_buid[h.system_buid_length] = '\0';
        memcpy(_session_udid.data(), udid, wanted_udid_length);
        _session_udid[wanted_udid_length] = '\0';
        _session_generation = generation;
        _session_record_loaded = buildStartSessionXml();
        valid = _session_record_loaded;
    }
    if (valid && loaded_from_legacy) {
        // Migration failure is non-fatal: reuse proceeds and the legacy fallback remains available.
        uint32_t migrated_slot = 0;
        const esp_err_t migrate_ret = storePairRecordInPartition(udid, blob, blob_size, &migrated_slot);
        if (migrate_ret == ESP_OK) {
            printf("[HCC PAIR] PAIR RECORD MIGRATED slot=%u\n", static_cast<unsigned>(migrated_slot));
        } else {
            printf("[HCC PAIR] PAIR RECORD MIGRATE FAILED: %s\n", esp_err_to_name(migrate_ret));
        }
    }
    if (blob != nullptr) {
        mbedtls_platform_zeroize(blob, blob_size);
        heap_caps_free(blob);
    }
    if (!valid) {
        clearStartSessionConnectionState(false);
        printf("[HCC PAIR] PAIR RECORD REUSE MISS\n");
        return false;
    }
    printf("[HCC PAIR] PAIR RECORD REUSE READY\n");
    return true;
}

bool HCCUSBTransport::beginPair(HCCPairCredentialBundle *bundle, uint32_t generation,
                                const char *host_id, const char *system_buid, const char *udid)
{
    const size_t host_id_length = host_id != nullptr ? strlen(host_id) : 0;
    const size_t system_buid_length = system_buid != nullptr ? strlen(system_buid) : 0;
    if (bundle == nullptr || !HCCPairCredentials::validateBundle(bundle) || _pair_bundle != nullptr ||
        generation == 0 || bundle->generation != generation || host_id == nullptr ||
        system_buid == nullptr || udid == nullptr || strlen(udid) >= _pair_udid.size() ||
        host_id_length == 0 || host_id_length >= _pair_host_id.size() ||
        system_buid_length == 0 || system_buid_length >= _pair_system_buid.size()) {
        HCCPairCredentials::destroyBundle(bundle);
        return false;
    }
    _pair_bundle = bundle;
    _pair_generation = generation;
    memcpy(_pair_udid.data(), udid, strlen(udid) + 1);
    memcpy(_pair_host_id.data(), host_id, host_id_length + 1);
    memcpy(_pair_system_buid.data(), system_buid, system_buid_length + 1);
    if (!buildPairXml(host_id, system_buid)) {
        destroyPairState();
        return false;
    }
    _pair_xml_offset = 0;
    if (!_wifi_address_valid || _stopping || _state != State::MuxActive) {
        destroyPairState();
        return false;
    }

    if (_primary_tcp_closed || _tcp_state == TcpState::Failed) {
        if (_primary_reconnect_attempted || _primary_reconnect_pending) {
            printf("[HCC LOCKDOWN] primary reconnect unavailable\n");
            destroyPairState();
            return false;
        }
        _primary_reconnect_pending = true;
        printf("[HCC LOCKDOWN] primary reconnect pending\n");
    } else if (pairReadyForCurrentConnection()) {
        authorizePairForCurrentConnection();
    }
    return true;
}

void HCCUSBTransport::resetPrimaryTcpForPairReconnect()
{
    _tcp_syn_requested = false;
    _tcp_ack_requested = false;
    _lockdown_prefix_requested = false;
    _lockdown_body_requested = false;
    _tcp_data_ack_requested = false;
    _getvalue_product_version_after_ack = false;
    _getvalue_product_version_prefix_requested = false;
    _getvalue_product_version_body_requested = false;
    _getvalue_device_class_after_ack = false;
    _getvalue_device_class_prefix_requested = false;
    _getvalue_device_class_body_requested = false;
    _getvalue_device_public_key_after_ack = false;
    _getvalue_device_public_key_prefix_requested = false;
    _getvalue_device_public_key_body_requested = false;
    _getvalue_wifi_address_after_ack = false;
    _getvalue_wifi_address_prefix_requested = false;
    _getvalue_wifi_address_body_requested = false;
    _pair_prefix_requested = false;
    _pair_chunk_requested = false;
    _pair_after_querytype_ack = false;
    _start_notification_service_after_ack = false;
    _start_notification_service_prefix_requested = false;
    _start_notification_service_body_requested = false;
    _start_session_after_ack = false;
    _start_session_after_querytype_ack = false;
    _start_session_prefix_requested = false;
    _start_session_body_requested = false;
    _lockdown_expected_response = LockdownResponse::None;
    _lockdown_rx_stream_length = 0;
    mbedtls_platform_zeroize(_lockdown_rx_stream.data(), _lockdown_rx_stream.size());
    if (_pair_response_stream != nullptr) {
        mbedtls_platform_zeroize(_pair_response_stream, _pair_response_stream_capacity);
        heap_caps_free(_pair_response_stream);
        _pair_response_stream = nullptr;
        _pair_response_stream_length = 0;
        _pair_response_stream_capacity = 0;
    }
    _pair_xml_offset = 0;
    _tcp_source_port = 0;
    _tcp_destination_port = kLockdownPort;
    _tcp_tx_seq = 0;
    _tcp_ack_seq = 0;
    _tcp_peer_seq = 0;
    _tcp_peer_ack = 0;
    _tcp_peer_window = 0;
    _query_type_epoch = 0;
    _pair_authorized_epoch = 0;
    _primary_tcp_closed = false;
    _primary_trust_fin_requested = false;
    _primary_trust_fin_dispatched = false;
    _primary_trust_close_ack_requested = false;
    _tcp_state = TcpState::Inactive;
}

void HCCUSBTransport::clearStartSessionConnectionState(bool keep_pair_record)
{
    cleanupTls();
    _tls_state = TlsState::PlainLockdown;
    _notification_service_generation = 0;
    _notification_service_epoch = 0;
    _notification_service_port = 0;
    _notification_service_ssl = false;
    _notification_tcp_after_ack = false;
    _start_session_after_ack = false;
    _start_session_after_querytype_ack = false;
    _start_session_prefix_requested = false;
    _start_session_body_requested = false;
    _session_epoch = 0;
    _session_ssl_required = false;
    _session_ready = false;
    mbedtls_platform_zeroize(_session_id.data(), _session_id.size());
    if (!keep_pair_record) {
        _session_record_loaded = false;
        _session_generation = 0;
        _start_session_xml_length = 0;
        mbedtls_platform_zeroize(_session_host_id.data(), _session_host_id.size());
        mbedtls_platform_zeroize(_session_system_buid.data(), _session_system_buid.size());
        mbedtls_platform_zeroize(_session_udid.data(), _session_udid.size());
        mbedtls_platform_zeroize(_start_session_xml.data(), _start_session_xml.size());
    }
}

bool HCCUSBTransport::pairReadyForCurrentConnection() const
{
    return !_stopping && _state == State::MuxActive && _tcp_state == TcpState::Connected &&
           _pair_bundle != nullptr && _pair_xml != nullptr && _pair_generation != 0 &&
           _wifi_address_valid && _query_type_epoch != 0 &&
           _query_type_epoch == _primary_connection_epoch && !_tx_in_flight;
}

void HCCUSBTransport::authorizePairForCurrentConnection()
{
    if (!pairReadyForCurrentConnection() || _pair_authorized_epoch == _primary_connection_epoch) {
        return;
    }
    _pair_authorized_epoch = _primary_connection_epoch;
    _pair_prefix_requested = true;
    printf("[HCC PAIR] Pair authorized epoch=%u\n",
           static_cast<unsigned>(_primary_connection_epoch));
}

bool HCCUSBTransport::sendPairPrefix()
{
    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(_pair_xml_length));
    if (!sendTcpPacket(TxKind::PairPrefix, 0x10, prefix, sizeof(prefix))) return false;
    _pair_prefix_requested = false;
    printf("[HCC PAIR] Pair request starting\n");
    printf("[HCC PAIR] Pair prefix sent body_len=%u\n", static_cast<unsigned>(_pair_xml_length));
    return true;
}

bool HCCUSBTransport::sendPairChunk()
{
    if (_pair_xml == nullptr || _pair_xml_offset >= _pair_xml_length) return false;
    const size_t length = ((_pair_xml_length - _pair_xml_offset) < kPairTxChunkSize)
                              ? (_pair_xml_length - _pair_xml_offset) : kPairTxChunkSize;
    if (!sendTcpPacket(TxKind::PairChunk, 0x10, _pair_xml + _pair_xml_offset, length)) return false;
    _pair_chunk_requested = false;
    printf("[HCC PAIR] Pair chunk offset=%u len=%u\n",
           static_cast<unsigned>(_pair_xml_offset), static_cast<unsigned>(length));
    return true;
}

bool HCCUSBTransport::sendStartNotificationServicePrefix()
{
    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(kLockdownStartNotificationServiceBodySize));
    if (!sendTcpPacket(TxKind::StartNotificationServicePrefix, 0x10,
                       prefix, sizeof(prefix))) return false;
    _start_notification_service_prefix_requested = false;
    printf("[HCC NOTIFY] StartService prefix submit body_len=%u\n",
           static_cast<unsigned>(kLockdownStartNotificationServiceBodySize));
    return true;
}

bool HCCUSBTransport::sendStartNotificationServiceBody()
{
    if (!sendTcpPacket(TxKind::StartNotificationServiceBody, 0x10,
                       reinterpret_cast<const uint8_t *>(kLockdownStartNotificationServiceXml),
                       kLockdownStartNotificationServiceBodySize)) return false;
    _start_notification_service_body_requested = false;
    printf("[HCC NOTIFY] StartService body submit body_len=%u\n",
           static_cast<unsigned>(kLockdownStartNotificationServiceBodySize));
    return true;
}

bool HCCUSBTransport::buildStartSessionXml()
{
    static constexpr char kFormat[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "<key>HostID</key>\n<string>%s</string>\n"
        "<key>Label</key>\n<string>HCC INSIGHT</string>\n"
        "<key>Request</key>\n<string>StartSession</string>\n"
        "<key>SystemBUID</key>\n<string>%s</string>\n"
        "</dict>\n</plist>";
    const int written = snprintf(reinterpret_cast<char *>(_start_session_xml.data()),
                                 _start_session_xml.size(), kFormat,
                                 _session_host_id.data(), _session_system_buid.data());
    if (written <= 0 || static_cast<size_t>(written) >= _start_session_xml.size()) {
        _start_session_xml_length = 0;
        mbedtls_platform_zeroize(_start_session_xml.data(), _start_session_xml.size());
        return false;
    }
    _start_session_xml_length = static_cast<size_t>(written);
    return true;
}

bool HCCUSBTransport::sendStartSessionPrefix()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected ||
        !_session_record_loaded || _session_generation == 0 ||
        _start_session_xml_length == 0) {
        return false;
    }
    uint8_t prefix[4] = {};
    writeBigEndianU32(prefix, static_cast<uint32_t>(_start_session_xml_length));
    if (!sendTcpPacket(TxKind::StartSessionPrefix, 0x10, prefix, sizeof(prefix))) {
        return false;
    }
    _start_session_prefix_requested = false;
    return true;
}

bool HCCUSBTransport::sendStartSessionBody()
{
    if (_state != State::MuxActive || _tcp_state != TcpState::Connected ||
        !_session_record_loaded || _start_session_xml_length == 0) {
        return false;
    }
    if (!sendTcpPacket(TxKind::StartSessionBody, 0x10, _start_session_xml.data(),
                       _start_session_xml_length)) {
        return false;
    }
    _start_session_body_requested = false;
    return true;
}

int HCCUSBTransport::tlsVerifyPeer(void *, mbedtls_x509_crt *, int, uint32_t *flags)
{
    if (flags != nullptr) {
        *flags = 0;
    }
    return 0;
}

int HCCUSBTransport::tlsBioSend(void *context, const unsigned char *buffer, size_t length)
{
    HCCUSBTransport *transport = static_cast<HCCUSBTransport *>(context);
    if (transport == nullptr || buffer == nullptr || transport->_tls == nullptr ||
        transport->_stopping ||
        (transport->_tls_state != TlsState::TlsHandshaking &&
         transport->_tls_state != TlsState::TlsEstablished) ||
        transport->_tls_generation != transport->_session_generation ||
        transport->_tls_epoch != transport->_primary_connection_epoch ||
        transport->_tcp_state != TcpState::Connected) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    TlsContext *tls = transport->_tls;
    if (length == 0) {
        return 0;
    }
    if (tls->tx_length != 0 || length > kTlsTxCapacity) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    memcpy(tls->tx_staging, buffer, length);
    tls->tx_length = length;
    tls->tx_offset = 0;
    return static_cast<int>(length);
}

int HCCUSBTransport::tlsBioRecv(void *context, unsigned char *buffer, size_t length)
{
    HCCUSBTransport *transport = static_cast<HCCUSBTransport *>(context);
    if (transport == nullptr || buffer == nullptr || transport->_tls == nullptr ||
        transport->_stopping || transport->_tcp_state != TcpState::Connected ||
        transport->_tls_generation != transport->_session_generation ||
        transport->_tls_epoch != transport->_primary_connection_epoch) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    TlsContext *tls = transport->_tls;
    if (tls->rx_length == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    const size_t copied = length < tls->rx_length ? length : tls->rx_length;
    const size_t first = copied < (kTlsRxCapacity - tls->rx_head)
                             ? copied : (kTlsRxCapacity - tls->rx_head);
    memcpy(buffer, tls->rx_fifo + tls->rx_head, first);
    if (copied > first) {
        memcpy(buffer + first, tls->rx_fifo, copied - first);
    }
    tls->rx_head = (tls->rx_head + copied) % kTlsRxCapacity;
    tls->rx_length -= copied;
    return static_cast<int>(copied);
}

bool HCCUSBTransport::appendTlsRx(const uint8_t *data, size_t length)
{
    if (_tls == nullptr || data == nullptr || length > kTlsRxCapacity - _tls->rx_length) {
        printf("[HCC TLS] RX ciphertext overflow len=%u buffered=%u\n",
               static_cast<unsigned>(length),
               static_cast<unsigned>(_tls != nullptr ? _tls->rx_length : 0));
        return false;
    }
    const size_t tail = (_tls->rx_head + _tls->rx_length) % kTlsRxCapacity;
    const size_t first = length < (kTlsRxCapacity - tail) ? length : (kTlsRxCapacity - tail);
    memcpy(_tls->rx_fifo + tail, data, first);
    if (length > first) {
        memcpy(_tls->rx_fifo, data + first, length - first);
    }
    _tls->rx_length += length;
    _tls_handshake_step_requested = true;
    return true;
}

bool HCCUSBTransport::loadTlsCredentials()
{
    nvs_handle_t handle = 0;
    size_t blob_size = 0;
    uint8_t *blob = nullptr;
    uint8_t *pem = nullptr;
    char record_key[16] = {};
    esp_err_t err = openPairRecordPartition(NVS_READONLY, &handle);
    if (err == ESP_OK) {
        PairRecordIndex *index = static_cast<PairRecordIndex *>(
            heap_caps_calloc(1, sizeof(PairRecordIndex), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint32_t slot = 0;
        if (index != nullptr && loadPairRecordIndex(handle, index) &&
            findPairRecordSlot(*index, _session_udid.data(), &slot) &&
            makePairRecordSlotKey(slot, record_key, sizeof(record_key))) {
            err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
        } else {
            snprintf(record_key, sizeof(record_key), "%s", kLegacyPairRecordKey);
            err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
        }
        if (index != nullptr) heap_caps_free(index);
    }
    if (err != ESP_OK) {
        if (handle != 0) { nvs_close(handle); handle = 0; }
        blob_size = 0;
        record_key[0] = '\0';
        err = nvs_open(kPairRecordNamespace, NVS_READONLY, &handle);
        if (err == ESP_OK) {
            PairRecordIndex *legacy_index = static_cast<PairRecordIndex *>(
                heap_caps_calloc(1, sizeof(PairRecordIndex), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            uint32_t legacy_slot = 0;
            if (legacy_index != nullptr && loadPairRecordIndex(handle, legacy_index) &&
                findPairRecordSlot(*legacy_index, _session_udid.data(), &legacy_slot) &&
                makePairRecordSlotKey(legacy_slot, record_key, sizeof(record_key))) {
                err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
            } else {
                snprintf(record_key, sizeof(record_key), "%s", kLegacyPairRecordKey);
                err = nvs_get_blob(handle, record_key, nullptr, &blob_size);
            }
            if (legacy_index != nullptr) heap_caps_free(legacy_index);
        }
    }
    if (err == ESP_OK && blob_size >= sizeof(PairRecordBlobHeader)) {
        blob = static_cast<uint8_t *>(heap_caps_malloc(blob_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (blob == nullptr) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = nvs_get_blob(handle, record_key, blob, &blob_size);

    PairRecordBlobHeader header = {};
    bool valid = err == ESP_OK;
    if (valid) memcpy(&header, blob, sizeof(header));
    const size_t lengths[] = { header.udid_length, header.host_id_length,
        header.system_buid_length, header.root_private_key_length,
        header.host_private_key_length, header.root_certificate_length,
        header.host_certificate_length, header.device_certificate_length,
        header.wifi_mac_address_length, header.escrow_bag_length };
    size_t checked_size = sizeof(header);
    valid = valid && header.magic == kPairRecordMagic && header.version == kPairRecordVersion &&
            header.total_size == blob_size && _session_generation == _tls_generation;
    for (size_t field_length : lengths) {
        if (field_length == 0 || field_length > blob_size - checked_size) {
            valid = false;
            break;
        }
        checked_size += field_length;
    }
    const size_t udid_length = strnlen(_session_udid.data(), _session_udid.size());
    valid = valid && checked_size == blob_size && udid_length == header.udid_length &&
            udid_length > 0 && memcmp(blob + sizeof(header), _session_udid.data(), udid_length) == 0;

    size_t root_key_offset = sizeof(header) + header.udid_length +
                             header.host_id_length + header.system_buid_length;
    size_t root_cert_offset = root_key_offset + header.root_private_key_length +
                              header.host_private_key_length;
    int ret = -1;
    if (valid) {
        pem = static_cast<uint8_t *>(heap_caps_malloc(
            header.root_certificate_length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pem != nullptr) {
            memcpy(pem, blob + root_cert_offset, header.root_certificate_length);
            pem[header.root_certificate_length] = '\0';
            ret = mbedtls_x509_crt_parse(&_tls->root_certificate, pem,
                                         header.root_certificate_length + 1);
            mbedtls_platform_zeroize(pem, header.root_certificate_length + 1);
            heap_caps_free(pem);
            pem = nullptr;
        }
        valid = ret == 0;
    }
    if (valid) {
        pem = static_cast<uint8_t *>(heap_caps_malloc(
            header.root_private_key_length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pem != nullptr) {
            memcpy(pem, blob + root_key_offset, header.root_private_key_length);
            pem[header.root_private_key_length] = '\0';
            ret = mbedtls_pk_parse_key(&_tls->root_private_key, pem,
                                       header.root_private_key_length + 1,
                                       nullptr, 0, mbedtls_ctr_drbg_random, &_tls->drbg);
            mbedtls_platform_zeroize(pem, header.root_private_key_length + 1);
            heap_caps_free(pem);
            pem = nullptr;
        }
        valid = ret == 0;
    }
    if (pem != nullptr) {
        mbedtls_platform_zeroize(pem, blob_size);
        heap_caps_free(pem);
    }
    if (blob != nullptr) {
        mbedtls_platform_zeroize(blob, blob_size);
        heap_caps_free(blob);
    }
    if (handle != 0) nvs_close(handle);
    return valid;
}

bool HCCUSBTransport::prepareTls()
{
    if (_tls_state != TlsState::TlsPrepare || !_session_ready || !_session_ssl_required ||
        _session_generation == 0 || _session_epoch != _primary_connection_epoch ||
        _tcp_state != TcpState::Connected || _primary_tcp_closed) {
        return false;
    }
    logTlsMemory("PREPARE");
    printf("[HCC TLS] handshake starting\n");
    _tls = static_cast<TlsContext *>(heap_caps_calloc(
        1, sizeof(TlsContext), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_tls == nullptr) return false;
    mbedtls_ssl_init(&_tls->ssl);
    mbedtls_ssl_config_init(&_tls->config);
    mbedtls_x509_crt_init(&_tls->root_certificate);
    mbedtls_pk_init(&_tls->root_private_key);
    mbedtls_entropy_init(&_tls->entropy);
    mbedtls_ctr_drbg_init(&_tls->drbg);
    _tls->rx_fifo = static_cast<uint8_t *>(heap_caps_malloc(
        kTlsRxCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _tls->tx_staging = static_cast<uint8_t *>(heap_caps_malloc(
        kTlsTxCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _tls->lockdown_tx = static_cast<uint8_t *>(heap_caps_malloc(
        kTlsLockdownCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _tls->lockdown_rx = static_cast<uint8_t *>(heap_caps_malloc(
        kTlsLockdownCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    static constexpr unsigned char kPersonalization[] = "HCC Lockdown TLS";
    int ret = (_tls->rx_fifo != nullptr && _tls->tx_staging != nullptr &&
               _tls->lockdown_tx != nullptr && _tls->lockdown_rx != nullptr)
                  ? mbedtls_ctr_drbg_seed(&_tls->drbg, mbedtls_entropy_func, &_tls->entropy,
                                          kPersonalization, sizeof(kPersonalization) - 1)
                  : MBEDTLS_ERR_SSL_ALLOC_FAILED;
    if (ret == 0 && !loadTlsCredentials()) ret = MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    if (ret == 0) {
        ret = mbedtls_ssl_config_defaults(&_tls->config, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (ret == 0) {
        mbedtls_ssl_conf_rng(&_tls->config, mbedtls_ctr_drbg_random, &_tls->drbg);
        mbedtls_ssl_conf_verify(&_tls->config, HCCUSBTransport::tlsVerifyPeer, this);
        mbedtls_ssl_conf_ca_chain(&_tls->config, &_tls->root_certificate, nullptr);
        ret = mbedtls_ssl_conf_own_cert(&_tls->config, &_tls->root_certificate,
                                        &_tls->root_private_key);
    }
    if (ret == 0) ret = mbedtls_ssl_setup(&_tls->ssl, &_tls->config);
    if (ret == 0) {
        // Lockdown has no DNS hostname. Calling with nullptr explicitly selects
        // the source-compatible paired-device policy: validate through the
        // configured callback without performing a certificate CN/SAN match.
        ret = mbedtls_ssl_set_hostname(&_tls->ssl, nullptr);
    }
    if (ret != 0) {
        logTlsError("prepare failed", ret);
        return false;
    }
    mbedtls_ssl_set_bio(&_tls->ssl, this, HCCUSBTransport::tlsBioSend,
                        HCCUSBTransport::tlsBioRecv, nullptr);
    printf("[HCC TLS] host credentials loaded\n");
    _tls_state = TlsState::TlsHandshaking;
    _tls_handshake_step_requested = true;
    return true;
}

bool HCCUSBTransport::sendTlsCiphertext()
{
    if (_tls == nullptr || _tls->tx_offset >= _tls->tx_length) return true;
    const size_t remaining = _tls->tx_length - _tls->tx_offset;
    const size_t length = remaining < kTlsTcpChunkSize ? remaining : kTlsTcpChunkSize;
    return sendTcpPacket(TxKind::TlsCiphertext, 0x10,
                         _tls->tx_staging + _tls->tx_offset, length);
}

bool HCCUSBTransport::processTlsApplication()
{
    if (_tls == nullptr || _tls_state != TlsState::TlsEstablished ||
        _tls_generation != _session_generation || _tls_epoch != _primary_connection_epoch ||
        _tcp_state != TcpState::Connected || _primary_tcp_closed) {
        return false;
    }
    if (_tx_in_flight) return true;
    if (_tls->tx_offset < _tls->tx_length) return sendTlsCiphertext();

    if (_tls->app_state == TlsContext::AppState::StartServiceWrite) {
        if (_tls->lockdown_tx_offset >= _tls->lockdown_tx_length) {
            if (_tls->tx_length == 0) {
                _tls->app_state = TlsContext::AppState::StartServiceRead;
                printf("[HCC TLS] encrypted StartService request sent\n");
            }
            return true;
        }
        const int ret = mbedtls_ssl_write(
            &_tls->ssl,
            _tls->lockdown_tx + _tls->lockdown_tx_offset,
            _tls->lockdown_tx_length - _tls->lockdown_tx_offset);
        if (ret > 0) {
            _tls->lockdown_tx_offset += static_cast<size_t>(ret);
            if (_tls->lockdown_tx_offset == _tls->lockdown_tx_length) {
                _tls->lockdown_tx_waiting_for_ciphertext = _tls->tx_length != 0;
                if (!_tls->lockdown_tx_waiting_for_ciphertext) {
                    _tls->app_state = TlsContext::AppState::StartServiceRead;
                    printf("[HCC TLS] encrypted StartService request sent\n");
                }
            }
            if (_tls->tx_offset < _tls->tx_length) return sendTlsCiphertext();
            return true;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (_tls->tx_offset < _tls->tx_length) return sendTlsCiphertext();
            return true;
        }
        logTlsError("encrypted StartService write failed", ret);
        _tls->app_state = TlsContext::AppState::Failed;
        return false;
    }

    if (_tls->app_state == TlsContext::AppState::PeerClosed) return true;
    bool response_pending =
        _tls->app_state == TlsContext::AppState::StartServiceRead;
    if (!response_pending &&
        _tls->app_state != TlsContext::AppState::StartServicePassed) {
        return true;
    }

    // Drain only plaintext already available to mbedTLS. This never waits: the
    // non-blocking BIO terminates the drain immediately with WANT_READ/WRITE.
    while (true) {
        if (_tls->lockdown_rx_length == kTlsLockdownCapacity) {
            printf("[HCC TLS] decrypted Lockdown response overflow\n");
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        const int ret = mbedtls_ssl_read(
            &_tls->ssl,
            _tls->lockdown_rx + _tls->lockdown_rx_length,
            kTlsLockdownCapacity - _tls->lockdown_rx_length);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            if (response_pending) {
                printf("[HCC SERVICE] StartService failed: TLS close_notify before complete response\n");
                _tls->app_state = TlsContext::AppState::Failed;
                return false;
            }
            printf("[HCC TLS] peer close_notify after StartService\n");
            _tls->app_state = TlsContext::AppState::PeerClosed;
            return true;
        }
        if (ret <= 0) {
            if (ret == 0) {
                printf("[HCC SERVICE] StartService failed: TLS EOF before complete response\n");
            } else {
                logTlsError("encrypted StartService read failed", ret);
            }
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        if (!response_pending) {
            printf("[HCC TLS] unexpected application data after StartService\n");
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        _tls->lockdown_rx_length += static_cast<size_t>(ret);
        if (_tls->lockdown_rx_length < sizeof(uint32_t)) continue;

        const uint32_t body_length = readBigEndianU32(_tls->lockdown_rx);
        if (body_length == 0 || body_length > kTlsLockdownCapacity - sizeof(uint32_t)) {
            printf("[HCC TLS] invalid decrypted Lockdown body length=%u\n",
                   static_cast<unsigned>(body_length));
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        const size_t frame_length = sizeof(uint32_t) + static_cast<size_t>(body_length);
        if (_tls->lockdown_rx_length < frame_length) continue;
        const uint8_t *body = _tls->lockdown_rx + sizeof(uint32_t);
        size_t error_offset = 0;
        char error[128] = {};
        char request[32] = {};
        if (findXmlValueAfterKey(body, body_length, "Error", &error_offset)) {
            if (extractXmlStringForKey(body, body_length, "Error", error, sizeof(error))) {
                printf("[HCC SERVICE] StartService failed Error=%s\n", error);
            } else {
                printf("[HCC SERVICE] StartService failed: invalid Error\n");
            }
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        size_t field_offset = 0;
        if (findXmlValueAfterKey(body, body_length, "Request", &field_offset) &&
            (!extractXmlStringForKey(body, body_length, "Request", request, sizeof(request)) ||
             strcmp(request, "StartService") != 0)) {
            printf("[HCC SERVICE] StartService failed: invalid Request\n");
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        if (findXmlValueAfterKey(body, body_length, "Result", &field_offset)) {
            char result[32] = {};
            if (!extractXmlStringForKey(body, body_length, "Result", result, sizeof(result)) ||
                strcmp(result, "Success") != 0) {
                printf("[HCC SERVICE] StartService failed: invalid Result\n");
                _tls->app_state = TlsContext::AppState::Failed;
                return false;
            }
        }
        uint16_t port = 0;
        if (!extractXmlUint16ForKey(body, body_length, "Port", &port) || port == 0) {
            printf("[HCC SERVICE] StartService failed: missing/invalid Port\n");
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        bool ssl = false;
        if (findXmlValueAfterKey(body, body_length, "EnableServiceSSL", &field_offset) &&
            !extractXmlBoolForKey(body, body_length, "EnableServiceSSL", &ssl)) {
            printf("[HCC SERVICE] StartService failed: invalid EnableServiceSSL\n");
            _tls->app_state = TlsContext::AppState::Failed;
            return false;
        }
        _notification_service_port = port;
        _notification_service_ssl = ssl;
        _notification_service_generation = _session_generation;
        _notification_service_epoch = _primary_connection_epoch;
        resetServiceTcpContext();
        _service_tcp.destination_port = port;
        _service_tcp.generation = _session_generation;
        if (!ssl) {
            _service_tcp.syn_requested = true;
        } else {
            _service_tcp.state = TcpState::Failed;
        }
        _tls->lockdown_rx_length = 0;
        _tls->app_state = TlsContext::AppState::StartServicePassed;
        response_pending = false;
        printf("[HCC SERVICE] STARTSERVICE PASS\n");
        printf("[HCC SERVICE] port=%u\n", static_cast<unsigned>(port));
        printf("[HCC SERVICE] EnableServiceSSL=%u\n", static_cast<unsigned>(ssl));
        if (_service_tcp.syn_requested) {
            // This function runs only from the deferred USB-client path. Submit
            // TCP #2 now; otherwise the task can enter handle_events(portMAX_DELAY)
            // with no transfer pending and wait until the peer closes TCP #1.
            return sendServiceTcpSyn();
        }
        // Keep draining mbedTLS non-blockingly so a peer close_notify is
        // classified as a graceful TLS close rather than malformed plist data.
        continue;
    }
}

bool HCCUSBTransport::processTlsDeferred()
{
    if (_tls_state == TlsState::Failed) return true;
    if (_tls_generation != _session_generation || _tls_epoch != _primary_connection_epoch ||
        _tcp_state != TcpState::Connected || _primary_tcp_closed) {
        clearStartSessionConnectionState(true);
        _tls_state = TlsState::Failed;
        return false;
    }
    if (_tls_state == TlsState::TlsPrepare && !prepareTls()) {
        logTlsMemory("HANDSHAKE-FAIL");
        clearStartSessionConnectionState(true);
        _tls_state = TlsState::Failed;
        return false;
    }
    if (_tx_in_flight) return true;
    if (_tls != nullptr && _tls->tx_offset < _tls->tx_length) return sendTlsCiphertext();
    if (_tls_state == TlsState::TlsEstablished) {
        if (processTlsApplication()) return true;
        clearStartSessionConnectionState(true);
        _tls_state = TlsState::Failed;
        return false;
    }
    if (!_tls_handshake_step_requested) return true;
    _tls_handshake_step_requested = false;
    const int ret = mbedtls_ssl_handshake(&_tls->ssl);
    if (_tls->tx_offset < _tls->tx_length && !_tx_in_flight) {
        if (!sendTlsCiphertext()) return false;
    }
    if (ret == 0) {
        _tls_state = TlsState::TlsEstablished;
        writeBigEndianU32(_tls->lockdown_tx,
                          static_cast<uint32_t>(kLockdownStartNotificationServiceBodySize));
        memcpy(_tls->lockdown_tx + sizeof(uint32_t), kLockdownStartNotificationServiceXml,
               kLockdownStartNotificationServiceBodySize);
        _tls->lockdown_tx_length = sizeof(uint32_t) + kLockdownStartNotificationServiceBodySize;
        _tls->lockdown_tx_offset = 0;
        _tls->lockdown_rx_length = 0;
        _tls->app_state = TlsContext::AppState::StartServiceWrite;
        logTlsMemory("HANDSHAKE-PASS");
        printf("[HCC TLS] HANDSHAKE PASS\n");
        if (processTlsApplication()) return true;
        clearStartSessionConnectionState(true);
        _tls_state = TlsState::Failed;
        return false;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
    logTlsError("handshake failed", ret);
    logTlsMemory("HANDSHAKE-FAIL");
    clearStartSessionConnectionState(true);
    _tls_state = TlsState::Failed;
    return false;
}

void HCCUSBTransport::cleanupTls()
{
    if (_tls != nullptr) {
        if (_tls->rx_fifo != nullptr) {
            mbedtls_platform_zeroize(_tls->rx_fifo, kTlsRxCapacity);
            heap_caps_free(_tls->rx_fifo);
        }
        if (_tls->tx_staging != nullptr) {
            mbedtls_platform_zeroize(_tls->tx_staging, kTlsTxCapacity);
            heap_caps_free(_tls->tx_staging);
        }
        if (_tls->lockdown_tx != nullptr) {
            mbedtls_platform_zeroize(_tls->lockdown_tx, kTlsLockdownCapacity);
            heap_caps_free(_tls->lockdown_tx);
        }
        if (_tls->lockdown_rx != nullptr) {
            mbedtls_platform_zeroize(_tls->lockdown_rx, kTlsLockdownCapacity);
            heap_caps_free(_tls->lockdown_rx);
        }
        mbedtls_ssl_free(&_tls->ssl);
        mbedtls_ssl_config_free(&_tls->config);
        mbedtls_x509_crt_free(&_tls->root_certificate);
        mbedtls_pk_free(&_tls->root_private_key);
        mbedtls_ctr_drbg_free(&_tls->drbg);
        mbedtls_entropy_free(&_tls->entropy);
        mbedtls_platform_zeroize(_tls, sizeof(TlsContext));
        heap_caps_free(_tls);
        _tls = nullptr;
    }
    _tls_after_ack = false;
    _tls_handshake_step_requested = false;
    _tls_generation = 0;
    _tls_epoch = 0;
}

void HCCUSBTransport::destroyPairState()
{
    if (_pair_xml != nullptr) {
        mbedtls_platform_zeroize(_pair_xml, kPairXmlMaximum);
        heap_caps_free(_pair_xml);
    }
    if (_pair_response_stream != nullptr) {
        mbedtls_platform_zeroize(_pair_response_stream, _pair_response_stream_capacity);
        heap_caps_free(_pair_response_stream);
    }
    if (_pair_escrow_bag != nullptr) {
        mbedtls_platform_zeroize(_pair_escrow_bag, _pair_escrow_bag_length);
        heap_caps_free(_pair_escrow_bag);
    }
    HCCPairCredentials::destroyBundle(_pair_bundle);
    _pair_bundle = nullptr;
    _pair_xml = nullptr;
    _pair_xml_length = 0;
    _pair_xml_offset = 0;
    _pair_response_stream = nullptr;
    _pair_response_stream_length = 0;
    _pair_response_stream_capacity = 0;
    _pair_escrow_bag = nullptr;
    _pair_escrow_bag_length = 0;
    _pair_record_persist_pending = false;
    _pair_prefix_requested = false;
    _pair_chunk_requested = false;
    _pair_after_querytype_ack = false;
    _pair_destroy_after_response = false;
    _start_notification_service_after_ack = false;
    _start_notification_service_prefix_requested = false;
    _start_notification_service_body_requested = false;
    _pair_generation = 0;
    _pair_authorized_epoch = 0;
    _notification_service_generation = 0;
    _notification_service_epoch = 0;
    _notification_service_port = 0;
    _notification_service_ssl = false;
    _notification_tcp_after_ack = false;
    _pair_trust_state = PairTrustState::Inactive;
    _trust_pair_retry_requested = false;
    mbedtls_platform_zeroize(_pair_udid.data(), _pair_udid.size());
    mbedtls_platform_zeroize(_pair_host_id.data(), _pair_host_id.size());
    mbedtls_platform_zeroize(_pair_system_buid.data(), _pair_system_buid.size());
}

bool HCCUSBTransport::sendTcpPacket(TxKind tx_kind, uint8_t flags, const uint8_t *payload, size_t payload_length)
{
    if (_stopping || _tx_transfer != nullptr || _tx_in_flight || _tcp_source_port == 0 ||
        (payload_length > 0 && payload == nullptr)) {
        return false;
    }

    const size_t packet_size = kTcpMuxPacketSize + payload_length;
    esp_err_t ret = usb_host_transfer_alloc(packet_size, 0, &_tx_transfer);
    if (ret != ESP_OK || _tx_transfer == nullptr) {
        printf("[HCC USB MUX] TCP failed: TX alloc: %s\n", esp_err_to_name(ret));
        _tcp_state = TcpState::Failed;
        _state = State::Failed;
        return false;
    }

    _tx_transfer->num_bytes = packet_size;
    _tx_transfer->device_handle = _device;
    _tx_transfer->bEndpointAddress = _out_endpoint;
    _tx_transfer->callback = HCCUSBTransport::onTxComplete;
    _tx_transfer->context = this;

    uint8_t *data = _tx_transfer->data_buffer;
    writeBigEndianU32(data, 6);
    writeBigEndianU32(data + 4, static_cast<uint32_t>(packet_size));
    writeBigEndianU32(data + 8, 0xFEEDFACE);
    writeBigEndianU16(data + 12, _tx_seq);
    writeBigEndianU16(data + 14, _rx_seq);
    writeBigEndianU16(data + 16, _tcp_source_port);
    writeBigEndianU16(data + 18, _tcp_destination_port);
    writeBigEndianU32(data + 20, _tcp_tx_seq);
    writeBigEndianU32(data + 24, _tcp_ack_seq);
    data[28] = 0x50;
    data[29] = flags;
    writeBigEndianU16(data + 30, 0x0200);
    writeBigEndianU16(data + 32, 0);
    writeBigEndianU16(data + 34, 0);
    if (payload_length > 0) {
        memcpy(data + kTcpMuxPacketSize, payload, payload_length);
    }

    _tx_kind = tx_kind;
    ret = usb_host_transfer_submit(_tx_transfer);
    printf("[HCC USB MUX] TCP %s submit: ret=%d (%s)\n",
           txKindName(tx_kind), ret, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        usb_host_transfer_free(_tx_transfer);
        _tx_transfer = nullptr;
        _tx_kind = TxKind::None;
        _tcp_state = TcpState::Failed;
        _state = State::Failed;
        return false;
    }

    if (tx_kind == TxKind::TcpSyn) {
        printf("[HCC USB MUX] TCP SYN RAW (36):");
        for (size_t index = 0; index < kTcpMuxPacketSize; ++index) {
            printf(" %02X", data[index]);
        }
        printf("\n");
    }
    printf("[HCC USB MUX] TX TCP %s: sport=%u dport=%u mux_tx=%u mux_rx=%u seq=%u ack=%u flags=0x%02X\n",
           txKindName(tx_kind), _tcp_source_port, _tcp_destination_port,
           _tx_seq, _rx_seq, _tcp_tx_seq, _tcp_ack_seq, flags);
    ++_tx_seq;
    _tx_in_flight = true;
    return true;
}

bool HCCUSBTransport::sendServiceTcpPacket(TxKind tx_kind, uint8_t flags,
                                           const uint8_t *payload, size_t payload_length)
{
    if (_stopping || _tx_transfer != nullptr || _tx_in_flight ||
        _service_tcp.source_port == 0 || _service_tcp.destination_port == 0 ||
        (payload_length > 0 && payload == nullptr) ||
        payload_length > UINT32_MAX - kTcpMuxPacketSize) {
        return false;
    }

    const size_t packet_size = kTcpMuxPacketSize + payload_length;
    esp_err_t ret = usb_host_transfer_alloc(packet_size, 0, &_tx_transfer);
    if (ret != ESP_OK || _tx_transfer == nullptr) {
        printf("[HCC SERVICE] TCP TX alloc failed: %s\n", esp_err_to_name(ret));
        _service_tcp.state = TcpState::Failed;
        return false;
    }
    _tx_transfer->num_bytes = packet_size;
    _tx_transfer->device_handle = _device;
    _tx_transfer->bEndpointAddress = _out_endpoint;
    _tx_transfer->callback = HCCUSBTransport::onTxComplete;
    _tx_transfer->context = this;

    uint8_t *data = _tx_transfer->data_buffer;
    writeBigEndianU32(data, 6);
    writeBigEndianU32(data + 4, static_cast<uint32_t>(packet_size));
    writeBigEndianU32(data + 8, 0xFEEDFACE);
    writeBigEndianU16(data + 12, _tx_seq);
    writeBigEndianU16(data + 14, _rx_seq);
    writeBigEndianU16(data + 16, _service_tcp.source_port);
    writeBigEndianU16(data + 18, _service_tcp.destination_port);
    writeBigEndianU32(data + 20, _service_tcp.tx_seq);
    writeBigEndianU32(data + 24, _service_tcp.ack_seq);
    data[28] = 0x50;
    data[29] = flags;
    writeBigEndianU16(data + 30, 0x0200);
    writeBigEndianU16(data + 32, 0);
    writeBigEndianU16(data + 34, 0);
    if (payload_length > 0) {
        memcpy(data + kTcpMuxPacketSize, payload, payload_length);
    }

    if (tx_kind == TxKind::ServiceTcpSyn) {
        printf("[HCC SERVICE] SYN sport=%u dport=%u wire_sport=%02X%02X wire_dport=%02X%02X\n",
               static_cast<unsigned>(_service_tcp.source_port),
               static_cast<unsigned>(_service_tcp.destination_port),
               data[16], data[17], data[18], data[19]);
    }

    _tx_kind = tx_kind;
    ret = usb_host_transfer_submit(_tx_transfer);
    if (ret != ESP_OK) {
        printf("[HCC SERVICE] TCP %s submit failed: %s\n",
               txKindName(tx_kind), esp_err_to_name(ret));
        usb_host_transfer_free(_tx_transfer);
        _tx_transfer = nullptr;
        _tx_kind = TxKind::None;
        _service_tcp.state = TcpState::Failed;
        return false;
    }
    ++_tx_seq;
    _tx_in_flight = true;
    return true;
}

bool HCCUSBTransport::needsDwcDelayedSnapshot() const
{
    return _dwc_delayed_snapshot_pending &&
           !_stopping &&
           _tx_in_flight &&
           _tx_kind == TxKind::Setup;
}

bool HCCUSBTransport::hasDwcDelayedSnapshotPending() const
{
    return _dwc_delayed_snapshot_pending;
}

bool HCCUSBTransport::isDwcDelayedSnapshotDue() const
{
    return needsDwcDelayedSnapshot() &&
           static_cast<int32_t>(xTaskGetTickCount() - _dwc_delayed_snapshot_due) >= 0;
}

TickType_t HCCUSBTransport::dwcDelayedSnapshotDue() const
{
    return _dwc_delayed_snapshot_due;
}

bool HCCUSBTransport::txInFlight() const
{
    return _tx_in_flight;
}

uint8_t HCCUSBTransport::txKindForDiagnostic() const
{
    return static_cast<uint8_t>(_tx_kind);
}

void HCCUSBTransport::takeDwcDelayedSnapshotIfDue()
{
    if (!needsDwcDelayedSnapshot()) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (static_cast<int32_t>(now - _dwc_delayed_snapshot_due) < 0) {
        return;
    }

    _dwc_delayed_snapshot_pending = false;
    takeDwcSnapshot("delayed");
}

void HCCUSBTransport::beginDwcSetupDiagnostic()
{
    _dwc_out_channel = -1;
    _dwc_device_address = 0;
    _dwc_delayed_snapshot_due = xTaskGetTickCount() + pdMS_TO_TICKS(250);
    _dwc_delayed_snapshot_pending = true;

    findDwcOutChannel();
    takeDwcSnapshot("immediate");
}

bool HCCUSBTransport::findDwcOutChannel()
{
    usb_device_info_t device_info = {};
    if (_device == nullptr || usb_host_device_info(_device, &device_info) != ESP_OK) {
        printf("[HCC USB DWC] EP04 channel lookup failed: device info unavailable\n");
        return false;
    }

    _dwc_device_address = device_info.dev_addr;
    unsigned channel_count = usb_dwc_ll_ghwcfg_get_channel_num(&USB_DWC_HS);
    const unsigned max_channels = sizeof(USB_DWC_HS.host_chans) / sizeof(USB_DWC_HS.host_chans[0]);
    if (channel_count > max_channels) {
        channel_count = max_channels;
    }

    for (unsigned channel = 0; channel < channel_count; ++channel) {
        const uint32_t hcchar_raw = USB_DWC_HS.host_chans[channel].hcchar_reg.val;
        usb_dwc_hcchar_reg_t hcchar = {};
        hcchar.val = hcchar_raw;
        if (hcchar.devaddr == _dwc_device_address &&
            hcchar.epnum == (_out_endpoint & 0x0F) &&
            hcchar.epdir == 0 &&
            hcchar.eptype == USB_DWC_XFER_TYPE_BULK &&
            hcchar.mps == 512) {
            _dwc_out_channel = static_cast<int>(channel);
            return true;
        }
    }

    printf("[HCC USB DWC] EP04 channel lookup failed: devaddr=%u\n",
           static_cast<unsigned>(_dwc_device_address));
    return false;
}

void HCCUSBTransport::takeDwcSnapshot(const char *snapshot_name)
{
    if (_stopping || _dwc_out_channel < 0) {
        if (_dwc_out_channel < 0) {
            printf("[HCC USB DWC] EP04 channel unavailable snapshot=%s\n", snapshot_name);
        }
        return;
    }

    usb_device_info_t device_info = {};
    if (_device == nullptr || usb_host_device_info(_device, &device_info) != ESP_OK) {
        printf("[HCC USB DWC] EP04 channel validation failed snapshot=%s: device info unavailable\n", snapshot_name);
        return;
    }

    unsigned channel_count = usb_dwc_ll_ghwcfg_get_channel_num(&USB_DWC_HS);
    const unsigned max_channels = sizeof(USB_DWC_HS.host_chans) / sizeof(USB_DWC_HS.host_chans[0]);
    if (channel_count > max_channels) {
        channel_count = max_channels;
    }
    if (static_cast<unsigned>(_dwc_out_channel) >= channel_count) {
        printf("[HCC USB DWC] EP04 channel validation failed snapshot=%s: CH=%d unavailable\n",
               snapshot_name, _dwc_out_channel);
        return;
    }

    volatile usb_dwc_host_chan_regs_t *regs = &USB_DWC_HS.host_chans[_dwc_out_channel];
    const uint32_t hcchar_raw = regs->hcchar_reg.val;
    const uint32_t hcint_raw = regs->hcint_reg.val;
    const uint32_t hcintmsk_raw = regs->hcintmsk_reg.val;
    const uint32_t hctsiz_raw = regs->hctsiz_reg.val;
    const uint32_t hcdma_raw = regs->hcdma_reg.val;

    usb_dwc_hcchar_reg_t hcchar = {};
    hcchar.val = hcchar_raw;
    if (device_info.dev_addr != _dwc_device_address ||
        hcchar.devaddr != _dwc_device_address ||
        hcchar.epnum != (_out_endpoint & 0x0F) ||
        hcchar.epdir != 0 ||
        hcchar.eptype != USB_DWC_XFER_TYPE_BULK ||
        hcchar.mps != 512) {
        printf("[HCC USB DWC] EP04 channel validation failed snapshot=%s CH=%d\n",
               snapshot_name, _dwc_out_channel);
        return;
    }

    usb_dwc_hcint_reg_t hcint = {};
    hcint.val = hcint_raw;
    usb_dwc_hctsiz_reg_t hctsiz = {};
    hctsiz.val = hctsiz_raw;
    usb_dwc_hcdma_reg_t hcdma = {};
    hcdma.val = hcdma_raw;

    printf("[HCC USB DWC] EP04 CH=%d snapshot=%s\n", _dwc_out_channel, snapshot_name);
    printf("[HCC USB DWC] HCCHAR=0x%08X CHENA=%u CHDIS=%u EPNUM=%u EPDIR=%u EPTYPE=%u DEVADDR=%u MPS=%u\n",
           hcchar_raw, hcchar.chena, hcchar.chdis, hcchar.epnum, hcchar.epdir,
           hcchar.eptype, hcchar.devaddr, hcchar.mps);
    printf("[HCC USB DWC] HCINT=0x%08X XFERCOMPL=%u CHHLTD=%u ACK=%u NAK=%u STALL=%u XACTERR=%u BNA=%u\n",
           hcint_raw, hcint.xfercompl, hcint.chhltd, hcint.ack, hcint.nack,
           hcint.stall, hcint.xacterr, hcint.bnaintr);
    printf("[HCC USB DWC] HCINTMSK=0x%08X\n", hcintmsk_raw);
    printf("[HCC USB DWC] HCTSIZ=0x%08X\n", hctsiz_raw);
    printf("[HCC USB DWC] HCDMA=0x%08X QTD_IDX=%u\n",
           hcdma_raw, static_cast<unsigned>((hcdma.dmaaddr >> 3) & 0x3F));
}

void HCCUSBTransport::stopForDeviceGone()
{
    _stopping = true;
    _pair_success_epoch = 0;
    _session_reconnect_pending = false;
    _session_reconnect_attempted = false;
    _pair_record_rejected = false;
    _repair_reconnect_pending = false;
    _repair_attempted = false;
    clearStartSessionConnectionState(false);
    resetServiceTcpContext();
    destroyPairState();
    mbedtls_platform_zeroize(_wifi_address.data(), _wifi_address.size());
    _wifi_address_valid = false;
    mbedtls_platform_zeroize(_device_public_key.data(), _device_public_key.size());
    _device_public_key_length = 0;
    _device_public_key_pending = false;
    _device_public_key_wait_logged = false;
    _device_public_key_wakeup_logged = false;
    _device_public_key_utc_ready_logged = false;
    _dwc_delayed_snapshot_pending = false;
    _tcp_syn_requested = false;
    _tcp_ack_requested = false;
    _lockdown_prefix_requested = false;
    _lockdown_body_requested = false;
    _tcp_data_ack_requested = false;
    _getvalue_product_version_after_ack = false;
    _getvalue_product_version_prefix_requested = false;
    _getvalue_product_version_body_requested = false;
    _getvalue_device_class_after_ack = false;
    _getvalue_device_class_prefix_requested = false;
    _getvalue_device_class_body_requested = false;
    _getvalue_device_public_key_after_ack = false;
    _getvalue_device_public_key_prefix_requested = false;
    _getvalue_device_public_key_body_requested = false;
    _getvalue_wifi_address_after_ack = false;
    _getvalue_wifi_address_prefix_requested = false;
    _getvalue_wifi_address_body_requested = false;
    _lockdown_expected_response = LockdownResponse::None;
    _primary_reconnect_pending = false;
    _primary_reconnect_attempted = false;
    _primary_tcp_closed = true;
    _query_type_epoch = 0;
    _pair_authorized_epoch = 0;
    _pair_after_querytype_ack = false;
    _tcp_state = TcpState::Failed;
    _rx_stream_length = 0;
    _lockdown_rx_stream_length = 0;
}

void HCCUSBTransport::stopForShutdown()
{
    _stopping = true;
    _pair_success_epoch = 0;
    _session_reconnect_pending = false;
    _session_reconnect_attempted = false;
    _pair_record_rejected = false;
    _repair_reconnect_pending = false;
    _repair_attempted = false;
    clearStartSessionConnectionState(false);
    resetServiceTcpContext();
    destroyPairState();
    mbedtls_platform_zeroize(_wifi_address.data(), _wifi_address.size());
    _wifi_address_valid = false;
    mbedtls_platform_zeroize(_device_public_key.data(), _device_public_key.size());
    _device_public_key_length = 0;
    _device_public_key_pending = false;
    _device_public_key_wait_logged = false;
    _device_public_key_wakeup_logged = false;
    _device_public_key_utc_ready_logged = false;
    _dwc_delayed_snapshot_pending = false;
    _tcp_syn_requested = false;
    _tcp_ack_requested = false;
    _lockdown_prefix_requested = false;
    _lockdown_body_requested = false;
    _tcp_data_ack_requested = false;
    _getvalue_product_version_after_ack = false;
    _getvalue_product_version_prefix_requested = false;
    _getvalue_product_version_body_requested = false;
    _getvalue_device_class_after_ack = false;
    _getvalue_device_class_prefix_requested = false;
    _getvalue_device_class_body_requested = false;
    _getvalue_device_public_key_after_ack = false;
    _getvalue_device_public_key_prefix_requested = false;
    _getvalue_device_public_key_body_requested = false;
    _getvalue_wifi_address_after_ack = false;
    _getvalue_wifi_address_prefix_requested = false;
    _getvalue_wifi_address_body_requested = false;
    _lockdown_expected_response = LockdownResponse::None;
    _primary_reconnect_pending = false;
    _primary_reconnect_attempted = false;
    _primary_tcp_closed = true;
    _query_type_epoch = 0;
    _pair_authorized_epoch = 0;
    _pair_after_querytype_ack = false;
    _tcp_state = TcpState::Failed;
    _rx_stream_length = 0;
    _lockdown_rx_stream_length = 0;

    if (_device == nullptr) {
        return;
    }

    if (hasRxTransfersInFlight()) {
        esp_err_t ret = usb_host_endpoint_halt(_device, _in_endpoint);
        if (ret == ESP_OK) {
            ret = usb_host_endpoint_flush(_device, _in_endpoint);
        }
        if (ret != ESP_OK) {
            printf("[HCC USB MUX] VERSION failed: RX cancel: %s\n", esp_err_to_name(ret));
        }
    }

    if (_tx_in_flight) {
        esp_err_t ret = usb_host_endpoint_halt(_device, _out_endpoint);
        if (ret == ESP_OK) {
            ret = usb_host_endpoint_flush(_device, _out_endpoint);
        }
        if (ret != ESP_OK) {
            printf("[HCC USB MUX] VERSION failed: TX cancel: %s\n", esp_err_to_name(ret));
        }
    }
}

bool HCCUSBTransport::hasPendingTransfers() const
{
    if (_tx_transfer != nullptr) {
        return true;
    }

    // RX transfers stay allocated across devices; only in-flight ones are pending.
    return hasRxTransfersInFlight();
}

HCCUSBTransport::State HCCUSBTransport::state() const
{
    return _state;
}

bool HCCUSBTransport::submitRx(size_t slot)
{
    if (slot >= kRxTransferCount || _rx_transfers[slot] == nullptr || _rx_in_flight[slot] || _stopping) {
        return false;
    }

    esp_err_t ret = usb_host_transfer_submit(_rx_transfers[slot]);
    printf("[HCC USB MUX] RX[%u] submit: ret=%d (%s)\\n",
           static_cast<unsigned>(slot),
           ret,
           esp_err_to_name(ret));
    if (ret != ESP_OK) {
        printf("[HCC USB MUX] VERSION failed: RX[%u] submit: %s\n",
               static_cast<unsigned>(slot), esp_err_to_name(ret));
        return false;
    }

    _rx_in_flight[slot] = true;
    return true;
}

bool HCCUSBTransport::parseVersionResponse(const uint8_t *data, size_t length)
{
    if (data == nullptr || length < 8) {
        printf("[HCC USB MUX] Invalid VERSION response\n");
        logVersionFields(data, length);
        _state = State::Failed;
        return false;
    }

    const uint32_t protocol = readBigEndianU32(data);
    const uint32_t packet_length = readBigEndianU32(data + 4);
    if (protocol != 0 || packet_length != kVersionPacketSize || length != kVersionPacketSize) {
        printf("[HCC USB MUX] Invalid VERSION response\n");
        logVersionFields(data, length);
        _state = State::Failed;
        return false;
    }

    const uint32_t major = readBigEndianU32(data + 8);
    const uint32_t minor = readBigEndianU32(data + 12);
    if (major != 1 && major != 2) {
        printf("[HCC USB MUX] Invalid VERSION response\n");
        logVersionFields(data, length);
        _state = State::Failed;
        return false;
    }

    printf("[HCC USB MUX] RX VERSION: major=%u minor=%u\n", major, minor);
    if (major == 1) {
        _state = State::Version1;
        printf("[HCC USB MUX] VERSION 1 supported\n");
    } else {
        _state = State::Version2;
        _v2_framing = true;
        _setup_requested = true;
        _tx_seq = 0;
        _rx_seq = 0xFFFF;
        printf("[HCC USB MUX] VERSION 2 supported - SETUP pending\n");
    }

    return true;
}

bool HCCUSBTransport::appendRxStream(const uint8_t *data, size_t length)
{
    if (length == 0) {
        return true;
    }
    if (data == nullptr || length > (kRxStreamCapacity - _rx_stream_length)) {
        printf("[HCC USB MUX] RX stream overflow: buffered=%u incoming=%u\n",
               static_cast<unsigned>(_rx_stream_length), static_cast<unsigned>(length));
        _state = State::Failed;
        _tcp_state = TcpState::Failed;
        return false;
    }

    memcpy(_rx_stream.data() + _rx_stream_length, data, length);
    _rx_stream_length += length;
    return processRxStream();
}

bool HCCUSBTransport::processRxStream()
{
    size_t consumed = 0;
    while ((_rx_stream_length - consumed) >= kV2HeaderSize) {
        const uint8_t *frame = _rx_stream.data() + consumed;
        const uint32_t protocol = readBigEndianU32(frame);
        const uint32_t packet_length = readBigEndianU32(frame + 4);
        const uint32_t magic = readBigEndianU32(frame + 8);
        const uint16_t tx_seq = readBigEndianU16(frame + 12);
        const uint16_t rx_seq = readBigEndianU16(frame + 14);
        if (packet_length < kV2HeaderSize || packet_length > kRxStreamCapacity) {
            printf("[HCC USB MUX] RX V2 candidate: protocol=%u length=%u magic=0x%08X mux_tx_seq=%u mux_rx_seq=%u\n",
                   protocol, packet_length, magic, tx_seq, rx_seq);
            printf("[HCC USB MUX] Invalid RX V2 header: length %u is outside 16..65536\n", packet_length);
            _state = State::Failed;
            _tcp_state = TcpState::Failed;
            return false;
        }
        if (packet_length > (_rx_stream_length - consumed)) {
            break;
        }
        if (!dispatchV2Frame(frame, packet_length)) {
            return false;
        }
        consumed += packet_length;
    }

    if (consumed > 0) {
        const size_t remaining = _rx_stream_length - consumed;
        if (remaining > 0) {
            memmove(_rx_stream.data(), _rx_stream.data() + consumed, remaining);
        }
        _rx_stream_length = remaining;
    }
    return true;
}

bool HCCUSBTransport::dispatchV2Frame(const uint8_t *data, size_t length)
{
    if (data == nullptr || length < kV2HeaderSize) {
        return false;
    }

    const uint32_t protocol = readBigEndianU32(data);
    const uint32_t packet_length = readBigEndianU32(data + 4);
    const uint32_t magic = readBigEndianU32(data + 8);
    const uint16_t tx_seq = readBigEndianU16(data + 12);
    const uint16_t rx_seq = readBigEndianU16(data + 14);

    // usbmuxd sends 0xFEEDFACE only on outbound v2 packets. Its inbound
    // parser decodes magic but does not make it an acceptance condition.
    if (packet_length != length) {
        printf("[HCC USB MUX] RX V2 candidate: protocol=%u length=%u magic=0x%08X mux_tx_seq=%u mux_rx_seq=%u\n",
               protocol, packet_length, magic, tx_seq, rx_seq);
        printf("[HCC USB MUX] Invalid RX V2 header: declared length=%u actual frame=%u\n",
               packet_length, static_cast<unsigned>(length));
        return false;
    }

    // The peer's rx_seq acknowledges our next outgoing v2 header, matching usbmuxd.
    _rx_seq = rx_seq;
    printf("[HCC USB MUX] RX V2: protocol=%u length=%u magic=0x%08X tx_seq=%u rx_seq=%u\n",
           protocol, packet_length, magic, tx_seq, rx_seq);

    if (protocol == 6) {
        return handleInboundTcp(data, length);
    }
    printf("[HCC USB MUX] RX V2 protocol %u ignored\n", protocol);
    return true;
}

bool HCCUSBTransport::handleInboundTcp(const uint8_t *data, size_t length)
{
    if (length < kTcpMuxPacketSize) {
        printf("[HCC USB MUX] Invalid RX TCP frame length=%u\n", static_cast<unsigned>(length));
        return false;
    }

    const uint8_t *tcp = data + kV2HeaderSize;
    const uint16_t sport = readBigEndianU16(tcp);
    const uint16_t dport = readBigEndianU16(tcp + 2);
    const uint32_t seq = readBigEndianU32(tcp + 4);
    const uint32_t ack = readBigEndianU32(tcp + 8);
    const size_t tcp_header_length = static_cast<size_t>(tcp[12] >> 4) * 4;
    const uint8_t flags = tcp[13];
    const uint16_t window = readBigEndianU16(tcp + 14);

    if (tcp_header_length < kTcpHeaderSize || tcp_header_length > (length - kV2HeaderSize)) {
        printf("[HCC USB MUX] Invalid RX TCP header length=%u\n", static_cast<unsigned>(tcp_header_length));
        return false;
    }
    const size_t payload_length = length - kV2HeaderSize - tcp_header_length;
    const uint8_t *payload = tcp + tcp_header_length;
    if (_service_tcp.source_port != 0 &&
        sport == _service_tcp.destination_port && dport == _service_tcp.source_port) {
        return handleInboundServiceTcp(seq, ack, flags, window, payload, payload_length);
    }
    if (sport != _tcp_destination_port || dport != _tcp_source_port) {
        printf("[HCC USB MUX] RX TCP ignored: sport=%u dport=%u\n", sport, dport);
        return true;
    }
    if (flags & 0x04) {
        if ((_primary_trust_fin_requested || _primary_trust_fin_dispatched) &&
            _pair_trust_state == PairTrustState::NotificationServiceReady) {
            _tcp_state = TcpState::Failed;
            _primary_tcp_closed = true;
            _primary_trust_fin_requested = false;
            _primary_trust_close_ack_requested = false;
            _lockdown_expected_response = LockdownResponse::None;
            _lockdown_rx_stream_length = 0;
            _service_tcp.notification_prefix_requested = true;
            printf("[HCC NOTIFY] primary Lockdown closed; notification TCP preserved\n");
            return true;
        }
        const bool session_was_ready = _session_ready &&
                                       _session_epoch == _primary_connection_epoch;
        _tcp_state = TcpState::Failed;
        _primary_tcp_closed = true;
        _query_type_epoch = 0;
        _pair_authorized_epoch = 0;
        _lockdown_expected_response = LockdownResponse::None;
        _lockdown_rx_stream_length = 0;
        cleanupTls();
        _tls_state = TlsState::Failed;
        const bool can_reconnect_for_session = !session_was_ready && _session_record_loaded &&
                                               !_session_reconnect_attempted &&
                                               !_session_reconnect_pending && !_stopping;
        clearStartSessionConnectionState(true);
        if (_pair_success_epoch == _primary_connection_epoch) {
            printf("[HCC LOCKDOWN] primary connection closed after Pair success\n");
        } else if (_primary_connection_purpose == PrimaryConnectionPurpose::PairReconnect) {
            printf("[HCC LOCKDOWN] primary reconnect failed before Pair\n");
        } else if (_primary_connection_purpose == PrimaryConnectionPurpose::SessionReconnect) {
            printf("[HCC LOCKDOWN] %s\n", session_was_ready
                       ? "session connection closed after StartSession"
                       : "StartSession reconnect failed");
        }
        if (can_reconnect_for_session) {
            _session_reconnect_pending = true;
            printf("[HCC LOCKDOWN] session reconnect pending\n");
        } else if (_pair_record_rejected && !_repair_attempted && !_stopping) {
            _repair_reconnect_pending = true;
            printf("[HCC PAIR] fresh pairing reconnect pending\n");
        }
        printf("[HCC USB MUX] TCP RST received sport=%u dport=%u\n", sport, dport);
        return true;
    }
    if ((flags & 0x01) && _primary_trust_fin_dispatched &&
        _pair_trust_state == PairTrustState::NotificationServiceReady) {
        _tcp_peer_seq = seq;
        _tcp_peer_ack = ack;
        _tcp_peer_window = static_cast<uint32_t>(window) << 8;
        if (seq == _tcp_ack_seq) {
            _tcp_ack_seq += static_cast<uint32_t>(payload_length) + 1;
            _primary_trust_close_ack_requested = true;
            printf("[HCC NOTIFY] primary Lockdown peer FIN received\n");
        }
        return true;
    }
    if (_tcp_state == TcpState::SynSent && (flags & 0x12) == 0x12) {
        _tcp_peer_seq = seq;
        _tcp_peer_ack = ack;
        _tcp_peer_window = static_cast<uint32_t>(window) << 8;
        // Match usbmuxd: its first SYN uses seq/ack zero, then a valid SYN/ACK
        // advances both local sequence values once before its ACK.
        ++_tcp_tx_seq;
        ++_tcp_ack_seq;
        _tcp_ack_requested = true;
        printf("[HCC USB MUX] TCP SYN/ACK received sport=%u dport=%u seq=%u ack=%u window=%u\n",
               sport, dport, seq, ack, _tcp_peer_window);
        return true;
    }

    if (_tcp_state != TcpState::Connected || flags != 0x10) {
        printf("[HCC USB MUX] RX TCP ignored: flags=0x%02X state=%u\n",
               flags, static_cast<unsigned>(_tcp_state));
        return true;
    }

    _tcp_peer_seq = seq;
    _tcp_peer_ack = ack;
    _tcp_peer_window = static_cast<uint32_t>(window) << 8;
    if (payload_length == 0) {
        printf("[HCC LOCKDOWN] TCP pure ACK: seq=%u ack=%u\n", seq, ack);
        return true;
    }
    if (seq != _tcp_ack_seq) {
        printf("[HCC LOCKDOWN] TCP payload out of sequence: expected=%u received=%u len=%u\n",
               _tcp_ack_seq, seq, static_cast<unsigned>(payload_length));
        return true;
    }
    const bool payload_accepted = (_tls_state == TlsState::TlsPrepare ||
                                   _tls_state == TlsState::TlsHandshaking ||
                                   _tls_state == TlsState::TlsEstablished)
                                      ? appendTlsRx(payload, payload_length)
                                      : appendLockdownRxStream(payload, payload_length);
    if (!payload_accepted) {
        _tcp_state = TcpState::Failed;
        _state = State::Failed;
        return false;
    }
    _tcp_ack_seq += static_cast<uint32_t>(payload_length);
    _tcp_data_ack_requested = true;
    printf("[HCC LOCKDOWN] TCP payload received: len=%u seq=%u ack=%u\n",
           static_cast<unsigned>(payload_length), seq, _tcp_ack_seq);
    return true;
}

bool HCCUSBTransport::handleInboundServiceTcp(uint32_t seq, uint32_t ack,
                                              uint8_t flags, uint16_t window,
                                              const uint8_t *payload,
                                              size_t payload_length)
{
    if (flags & 0x04) {
        _service_tcp.state = TcpState::Failed;
        _service_tcp.syn_requested = false;
        _service_tcp.ack_requested = false;
        _service_tcp.notification_prefix_requested = false;
        _service_tcp.notification_body_requested = false;
        _notification_rx_stream_length = 0;
        printf("[HCC NOTIFY] service TCP RST\n");
        return true;
    }
    if (_service_tcp.state == TcpState::SynSent && (flags & 0x12) == 0x12) {
        _service_tcp.peer_seq = seq;
        _service_tcp.peer_ack = ack;
        _service_tcp.peer_window = static_cast<uint32_t>(window) << 8;
        ++_service_tcp.tx_seq;
        ++_service_tcp.ack_seq;
        _service_tcp.ack_requested = true;
        return true;
    }
    if (_service_tcp.state != TcpState::Connected || (flags & 0x10) == 0) {
        return true;
    }
    _service_tcp.peer_seq = seq;
    _service_tcp.peer_ack = ack;
    _service_tcp.peer_window = static_cast<uint32_t>(window) << 8;
    if (payload_length == 0) return true;

    if (seq == _service_tcp.ack_seq) {
        if (!appendNotificationRx(payload, payload_length)) {
            _service_tcp.state = TcpState::Failed;
            return false;
        }
        _service_tcp.ack_seq += static_cast<uint32_t>(payload_length);
        _service_tcp.ack_requested = true;
    }
    return true;
}

bool HCCUSBTransport::appendNotificationRx(const uint8_t *data, size_t length)
{
    if (length == 0) return true;
    if (data == nullptr || length > _notification_rx_stream.size() - _notification_rx_stream_length) {
        printf("[HCC NOTIFY] RX overflow length=%u used=%u\n",
               static_cast<unsigned>(length),
               static_cast<unsigned>(_notification_rx_stream_length));
        return false;
    }
    memcpy(_notification_rx_stream.data() + _notification_rx_stream_length, data, length);
    _notification_rx_stream_length += length;
    return processNotificationRx();
}

bool HCCUSBTransport::processNotificationRx()
{
    while (_notification_rx_stream_length >= sizeof(uint32_t)) {
        const uint32_t body_length = readBigEndianU32(_notification_rx_stream.data());
        if (body_length == 0 || body_length > kNotificationRxCapacity - sizeof(uint32_t)) {
            printf("[HCC NOTIFY] invalid frame length=%u\n", body_length);
            return false;
        }
        const size_t frame_length = sizeof(uint32_t) + static_cast<size_t>(body_length);
        if (_notification_rx_stream_length < frame_length) return true;

        const uint8_t *body = _notification_rx_stream.data() + sizeof(uint32_t);
        char command[48] = {};
        char name[96] = {};
        if (!extractXmlStringForKey(body, body_length, "Command", command, sizeof(command)) ||
            !extractXmlStringForKey(body, body_length, "Name", name, sizeof(name))) {
            printf("[HCC NOTIFY] invalid notification plist\n");
            return false;
        }
        printf("[HCC NOTIFY] Command=%s Name=%s\n", command, name);

        if (strcmp(command, "RelayNotification") == 0 &&
            strcmp(name, "com.apple.mobile.lockdown.request_pair") == 0 &&
            _pair_trust_state == PairTrustState::NotificationServiceReady &&
            !_trust_pair_retry_requested && _pair_bundle != nullptr &&
            _pair_xml != nullptr && _pair_generation != 0 &&
            _notification_service_generation == _pair_generation && !_stopping) {
            _trust_pair_retry_requested = true;
            if (!_primary_reconnect_pending && !_primary_reconnect_attempted) {
                _primary_reconnect_pending = true;
                printf("[HCC PAIR] TRUST NOTIFICATION -> Pair #2 reconnect pending\n");
            } else {
                printf("[HCC PAIR] Pair #2 reconnect unavailable\n");
            }
        }

        const size_t remaining = _notification_rx_stream_length - frame_length;
        if (remaining > 0) {
            memmove(_notification_rx_stream.data(),
                    _notification_rx_stream.data() + frame_length, remaining);
        }
        _notification_rx_stream_length = remaining;
    }
    return true;
}

bool HCCUSBTransport::appendLockdownRxStream(const uint8_t *data, size_t length)
{
    if (length == 0) {
        return true;
    }
    if (_lockdown_expected_response == LockdownResponse::Pair && _pair_response_stream == nullptr) {
        const size_t prefix_needed = sizeof(uint32_t) - _lockdown_rx_stream_length;
        const size_t prefix_bytes = length < prefix_needed ? length : prefix_needed;
        memcpy(_lockdown_rx_stream.data() + _lockdown_rx_stream_length, data, prefix_bytes);
        _lockdown_rx_stream_length += prefix_bytes;
        data += prefix_bytes;
        length -= prefix_bytes;
        if (_lockdown_rx_stream_length < sizeof(uint32_t)) return true;

        const uint32_t body_length = readBigEndianU32(_lockdown_rx_stream.data());
        if (body_length == 0 || body_length > kPairResponseMaximum) {
            printf("[HCC PAIR] Invalid Pair response body length=%u\n", body_length);
            _pair_destroy_after_response = true;
            destroyPairState();
            return false;
        }
        _pair_response_stream_capacity = sizeof(uint32_t) + static_cast<size_t>(body_length);
        _pair_response_stream = static_cast<uint8_t *>(heap_caps_calloc(
            1, _pair_response_stream_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (_pair_response_stream == nullptr) {
            printf("[HCC PAIR] Pair response allocation failed body_len=%u\n", body_length);
            destroyPairState();
            return false;
        }
        memcpy(_pair_response_stream, _lockdown_rx_stream.data(), sizeof(uint32_t));
        _pair_response_stream_length = sizeof(uint32_t);
        _lockdown_rx_stream_length = 0;
    }

    uint8_t *stream = _lockdown_rx_stream.data();
    size_t capacity = kLockdownRxStreamCapacity;
    size_t *stream_length = &_lockdown_rx_stream_length;
    if (_lockdown_expected_response == LockdownResponse::Pair && _pair_response_stream != nullptr) {
        stream = _pair_response_stream;
        capacity = _pair_response_stream_capacity;
        stream_length = &_pair_response_stream_length;
    }
    if (data == nullptr || length > (capacity - *stream_length)) {
        printf("[HCC LOCKDOWN] RX stream overflow: buffered=%u incoming=%u\n",
               static_cast<unsigned>(*stream_length), static_cast<unsigned>(length));
        return false;
    }

    memcpy(stream + *stream_length, data, length);
    *stream_length += length;
    return processLockdownRxStream();
}

bool HCCUSBTransport::processLockdownRxStream()
{
    uint8_t *stream = _lockdown_rx_stream.data();
    size_t capacity = kLockdownRxStreamCapacity;
    size_t *stream_length = &_lockdown_rx_stream_length;
    if (_lockdown_expected_response == LockdownResponse::Pair && _pair_response_stream != nullptr) {
        stream = _pair_response_stream;
        capacity = _pair_response_stream_capacity;
        stream_length = &_pair_response_stream_length;
    }
    size_t consumed = 0;
    while ((*stream_length - consumed) >= sizeof(uint32_t)) {
        const uint8_t *frame = stream + consumed;
        const uint32_t body_length = readBigEndianU32(frame);
        if (body_length == 0 || body_length > (capacity - sizeof(uint32_t))) {
            printf("[HCC LOCKDOWN] Invalid response body length=%u\n", body_length);
            return false;
        }

        const size_t response_length = sizeof(uint32_t) + static_cast<size_t>(body_length);
        if (response_length > (*stream_length - consumed)) {
            break;
        }

        printf("[HCC LOCKDOWN] Response body length=%u\n", body_length);
        if (!handleLockdownResponse(frame + sizeof(uint32_t), body_length)) {
            return false;
        }
        consumed += response_length;
    }

    if (consumed > 0) {
        const size_t remaining = *stream_length - consumed;
        if (remaining > 0) {
            memmove(stream, stream + consumed, remaining);
        }
        *stream_length = remaining;
    }
    if (_pair_destroy_after_response) {
        _pair_destroy_after_response = false;
        destroyPairState();
    }
    return true;
}

bool HCCUSBTransport::handlePairResponse(const uint8_t *body, size_t length)
{
    char request[32] = {};
    if (!extractXmlStringForKey(body, length, "Request", request, sizeof(request)) ||
        strcmp(request, "Pair") != 0) {
        printf("[HCC PAIR] Pair response validation failed\n");
        _pair_destroy_after_response = true;
        return true;
    }
    char error[128] = {};
    if (extractXmlStringForKey(body, length, "Error", error, sizeof(error))) {
        if (strcmp(error, "PairingDialogResponsePending") == 0) {
            printf("[HCC PAIR] TRUST PENDING\n");
            printf("[HCC PAIR] WAITING FOR TRUST NOTIFICATION\n");
            _pair_trust_state = PairTrustState::WaitingStartNotificationService;
            _start_notification_service_after_ack = true;
        } else if (strcmp(error, "UserDeniedPairing") == 0) {
            printf("[HCC PAIR] USER DENIED PAIRING\n");
            _pair_destroy_after_response = true;
        } else {
            printf("[HCC PAIR] Pair failed error=%s\n", error);
            _pair_destroy_after_response = true;
        }
        return true;
    }
    char result[32] = {};
    if (extractXmlStringForKey(body, length, "Result", result, sizeof(result)) &&
        strcmp(result, "Success") != 0) {
        printf("[HCC PAIR] Pair failed result=%s\n", result);
        _pair_destroy_after_response = true;
        return true;
    }
    size_t base64_chars = 0;
    size_t escrow_bytes = 0;
    if (!extractXmlBase64DataForKey(body, length, "EscrowBag", &base64_chars, &escrow_bytes) ||
        escrow_bytes == 0 || escrow_bytes > kEscrowBagMaximum) {
        printf("[HCC PAIR] PairRecord preparation failed: invalid EscrowBag\n");
        _pair_destroy_after_response = true;
        return true;
    }
    _pair_escrow_bag = static_cast<uint8_t *>(
        heap_caps_calloc(1, escrow_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t decoded_escrow_bytes = 0;
    if (_pair_escrow_bag == nullptr ||
        !decodeXmlBase64DataForKey(body, length, "EscrowBag", _pair_escrow_bag,
                                   escrow_bytes, &decoded_escrow_bytes) ||
        decoded_escrow_bytes != escrow_bytes) {
        printf("[HCC PAIR] PairRecord preparation failed: EscrowBag decode\n");
        if (_pair_escrow_bag != nullptr) {
            mbedtls_platform_zeroize(_pair_escrow_bag, escrow_bytes);
            heap_caps_free(_pair_escrow_bag);
            _pair_escrow_bag = nullptr;
        }
        _pair_escrow_bag_length = 0;
        _pair_destroy_after_response = true;
        return true;
    }
    _pair_escrow_bag_length = decoded_escrow_bytes;
    _pair_record_persist_pending = true;
    _pair_success_epoch = _primary_connection_epoch;
    printf("[HCC PAIR] PAIR RESPONSE SUCCESS\n");
    printf("[HCC PAIR] escrow_bag_bytes=%u\n", static_cast<unsigned>(escrow_bytes));
    printf("[HCC PAIR] PHASE2A PASS\n");
    return true;
}

bool HCCUSBTransport::initPairRecordStorage()
{
    // Runs once at boot so the NVS page cache is allocated before USB RX buffers churn the DMA heap.
    nvs_handle_t handle = 0;
    const esp_err_t ret = openPairRecordPartition(NVS_READONLY, &handle);
    if (handle != 0) nvs_close(handle);
    // An empty partition has no namespace yet; the partition itself is initialized.
    return ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND;
}

bool HCCUSBTransport::persistPendingPairRecord()
{
    if (!_pair_record_persist_pending) {
        return true;
    }

    _pair_record_persist_pending = false;
    const size_t udid_length = strnlen(_pair_udid.data(), _pair_udid.size());
    const size_t host_id_length = strnlen(_pair_host_id.data(), _pair_host_id.size());
    const size_t system_buid_length = strnlen(_pair_system_buid.data(), _pair_system_buid.size());
    const size_t wifi_length = strnlen(_wifi_address.data(), _wifi_address.size());
    if (!HCCPairCredentials::validateBundle(_pair_bundle) ||
        _pair_bundle->generation != _pair_generation || udid_length == 0 ||
        udid_length == _pair_udid.size() || host_id_length == 0 ||
        host_id_length == _pair_host_id.size() || system_buid_length == 0 ||
        system_buid_length == _pair_system_buid.size() || !_wifi_address_valid ||
        wifi_length == 0 || wifi_length == _wifi_address.size() ||
        _pair_escrow_bag == nullptr || _pair_escrow_bag_length == 0) {
        printf("[HCC PAIR] PAIR RECORD PERSIST FAILED: invalid retained material\n");
        destroyPairState();
        return false;
    }

    PairRecordBlobHeader header = {};
    header.magic = kPairRecordMagic;
    header.version = kPairRecordVersion;
    header.generation = _pair_generation;
    header.udid_length = static_cast<uint32_t>(udid_length);
    header.host_id_length = static_cast<uint32_t>(host_id_length);
    header.system_buid_length = static_cast<uint32_t>(system_buid_length);
    header.root_private_key_length = static_cast<uint32_t>(_pair_bundle->root_private_key.length);
    header.host_private_key_length = static_cast<uint32_t>(_pair_bundle->host_private_key.length);
    header.root_certificate_length = static_cast<uint32_t>(_pair_bundle->root_certificate.length);
    header.host_certificate_length = static_cast<uint32_t>(_pair_bundle->host_certificate.length);
    header.device_certificate_length = static_cast<uint32_t>(_pair_bundle->device_certificate.length);
    header.wifi_mac_address_length = static_cast<uint32_t>(wifi_length);
    header.escrow_bag_length = static_cast<uint32_t>(_pair_escrow_bag_length);

    const size_t field_lengths[] = {
        udid_length,
        host_id_length,
        system_buid_length,
        _pair_bundle->root_private_key.length,
        _pair_bundle->host_private_key.length,
        _pair_bundle->root_certificate.length,
        _pair_bundle->host_certificate.length,
        _pair_bundle->device_certificate.length,
        wifi_length,
        _pair_escrow_bag_length,
    };
    size_t blob_size = sizeof(header);
    for (const size_t field_length : field_lengths) {
        if (field_length > SIZE_MAX - blob_size) {
            printf("[HCC PAIR] PAIR RECORD PERSIST FAILED: size overflow\n");
            destroyPairState();
            return false;
        }
        blob_size += field_length;
    }
    if (blob_size > UINT32_MAX) {
        printf("[HCC PAIR] PAIR RECORD PERSIST FAILED: record too large\n");
        destroyPairState();
        return false;
    }
    header.total_size = static_cast<uint32_t>(blob_size);

    uint8_t *blob = static_cast<uint8_t *>(
        heap_caps_calloc(1, blob_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t *readback = nullptr;
    nvs_handle_t handle = 0;
    bool success = blob != nullptr;
    size_t offset = 0;
    if (success) {
        success = appendPairRecordField(blob, blob_size, &offset, &header, sizeof(header)) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_udid.data(), udid_length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_host_id.data(), host_id_length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_system_buid.data(), system_buid_length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_bundle->root_private_key.data,
                                        _pair_bundle->root_private_key.length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_bundle->host_private_key.data,
                                        _pair_bundle->host_private_key.length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_bundle->root_certificate.data,
                                        _pair_bundle->root_certificate.length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_bundle->host_certificate.data,
                                        _pair_bundle->host_certificate.length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_bundle->device_certificate.data,
                                        _pair_bundle->device_certificate.length) &&
                  appendPairRecordField(blob, blob_size, &offset, _wifi_address.data(), wifi_length) &&
                  appendPairRecordField(blob, blob_size, &offset, _pair_escrow_bag,
                                        _pair_escrow_bag_length) &&
                  offset == blob_size;
    }

    char record_key[16] = {};
    PairRecordIndex *index = static_cast<PairRecordIndex *>(
        heap_caps_calloc(1, sizeof(PairRecordIndex), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint32_t record_slot = 0;
    bool index_changed = false;
    esp_err_t ret = success ? openPairRecordPartition(NVS_READWRITE, &handle) : ESP_ERR_NO_MEM;
    if (ret == ESP_OK && index == nullptr) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK && !loadPairRecordIndex(handle, index)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && !findPairRecordSlot(*index, _pair_udid.data(), &record_slot)) {
        if (index->count >= kPairRecordMaximumDevices) {
            ret = ESP_ERR_NO_MEM;
        } else {
            bool used[kPairRecordMaximumDevices] = {};
            for (uint32_t i = 0; i < index->count; ++i) used[index->entries[i].slot] = true;
            while (record_slot < kPairRecordMaximumDevices && used[record_slot]) ++record_slot;
            if (record_slot >= kPairRecordMaximumDevices || udid_length >= kPairRecordStoredUdidCapacity) {
                ret = ESP_ERR_INVALID_SIZE;
            } else {
                PairRecordIndexEntry &entry = index->entries[index->count++];
                entry.slot = record_slot;
                memcpy(entry.udid, _pair_udid.data(), udid_length);
                entry.udid[udid_length] = '\0';
                index_changed = true;
            }
        }
    }
    if (ret == ESP_OK && !makePairRecordSlotKey(record_slot, record_key, sizeof(record_key))) {
        ret = ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, record_key, blob, blob_size);
    }
    if (ret == ESP_OK && index_changed) {
        ret = nvs_set_blob(handle, kPairRecordIndexKey, index, sizeof(*index));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (index != nullptr) {
        heap_caps_free(index);
        index = nullptr;
    }
    size_t readback_size = blob_size;
    if (ret == ESP_OK) {
        readback = static_cast<uint8_t *>(
            heap_caps_malloc(blob_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (readback == nullptr) {
            ret = ESP_ERR_NO_MEM;
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_get_blob(handle, record_key, readback, &readback_size);
        if (ret == ESP_OK && (readback_size != blob_size || memcmp(readback, blob, blob_size) != 0)) {
            ret = ESP_FAIL;
        }
    }
    if (ret == ESP_OK) {
        PairRecordBlobHeader stored = {};
        memcpy(&stored, readback, sizeof(stored));
        const size_t stored_lengths[] = {
            stored.udid_length,
            stored.host_id_length,
            stored.system_buid_length,
            stored.root_private_key_length,
            stored.host_private_key_length,
            stored.root_certificate_length,
            stored.host_certificate_length,
            stored.device_certificate_length,
            stored.wifi_mac_address_length,
            stored.escrow_bag_length,
        };
        size_t stored_size = sizeof(stored);
        bool stored_valid = stored.magic == kPairRecordMagic &&
                            stored.version == kPairRecordVersion &&
                            stored.total_size == readback_size &&
                            stored.generation == _pair_generation;
        for (const size_t field_length : stored_lengths) {
            if (field_length == 0 || field_length > SIZE_MAX - stored_size) {
                stored_valid = false;
                break;
            }
            stored_size += field_length;
        }
        stored_valid = stored_valid && stored_size == readback_size &&
                       stored.udid_length == udid_length &&
                       stored.host_id_length < _session_host_id.size() &&
                       stored.system_buid_length < _session_system_buid.size();
        size_t stored_offset = sizeof(stored);
        if (stored_valid && memcmp(readback + stored_offset, _pair_udid.data(), udid_length) != 0) {
            stored_valid = false;
        }
        if (stored_valid && stored.udid_length < _session_udid.size()) {
            memcpy(_session_udid.data(), readback + sizeof(stored), stored.udid_length);
            _session_udid[stored.udid_length] = '\0';
        } else if (stored_valid) {
            stored_valid = false;
        }
        stored_offset += stored.udid_length;
        if (stored_valid) {
            memcpy(_session_host_id.data(), readback + stored_offset, stored.host_id_length);
            _session_host_id[stored.host_id_length] = '\0';
        }
        stored_offset += stored.host_id_length;
        if (stored_valid) {
            memcpy(_session_system_buid.data(), readback + stored_offset, stored.system_buid_length);
            _session_system_buid[stored.system_buid_length] = '\0';
            _session_generation = stored.generation;
            _session_record_loaded = buildStartSessionXml();
            stored_valid = _session_record_loaded;
        }
        if (!stored_valid) {
            clearStartSessionConnectionState(false);
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (handle != 0) {
        nvs_close(handle);
    }

    if (blob != nullptr) {
        mbedtls_platform_zeroize(blob, blob_size);
        heap_caps_free(blob);
    }
    if (readback != nullptr) {
        mbedtls_platform_zeroize(readback, blob_size);
        heap_caps_free(readback);
    }

    if (ret != ESP_OK || !success) {
        printf("[HCC PAIR] PAIR RECORD PERSIST FAILED: %s\n", esp_err_to_name(ret));
        destroyPairState();
        return false;
    }

    printf("[HCC PAIR] PAIR RECORD PERSISTED\n");
    printf("[HCC PAIR] udid_length=%u\n", static_cast<unsigned>(udid_length));
    printf("[HCC PAIR] escrow_bag_bytes=%u\n",
           static_cast<unsigned>(_pair_escrow_bag_length));
    printf("[HCC PAIR] PAIR RECORD READY\n");
    if (_tcp_state == TcpState::Connected && !_primary_tcp_closed &&
        _primary_connection_epoch == _pair_success_epoch) {
        if (_tcp_data_ack_requested) {
            _start_session_after_ack = true;
        } else {
            _start_session_prefix_requested = true;
        }
    } else {
        _session_reconnect_pending = true;
        printf("[HCC LOCKDOWN] session reconnect pending\n");
    }
    destroyPairState();
    return true;
}

bool HCCUSBTransport::handleStartNotificationServiceResponse(const uint8_t *body, size_t length)
{
    char request[32] = {};
    char error[128] = {};
    if (!extractXmlStringForKey(body, length, "Request", request, sizeof(request)) ||
        strcmp(request, "StartService") != 0) {
        printf("[HCC NOTIFY] StartService response validation failed\n");
        _pair_trust_state = PairTrustState::Failed;
        return true;
    }
    size_t field_offset = 0;
    if (findXmlValueAfterKey(body, length, "Error", &field_offset)) {
        if (extractXmlStringForKey(body, length, "Error", error, sizeof(error))) {
            printf("[HCC NOTIFY] StartService failed error=%s\n", error);
        } else {
            printf("[HCC NOTIFY] StartService response invalid Error\n");
        }
        _pair_trust_state = PairTrustState::Failed;
        return true;
    }
    if (findXmlValueAfterKey(body, length, "Result", &field_offset)) {
        char result[32] = {};
        if (!extractXmlStringForKey(body, length, "Result", result, sizeof(result)) ||
            strcmp(result, "Success") != 0) {
            printf("[HCC NOTIFY] StartService response invalid Result\n");
            _pair_trust_state = PairTrustState::Failed;
            return true;
        }
    }

    uint16_t port = 0;
    if (!extractXmlUint16ForKey(body, length, "Port", &port) || port == 0) {
        printf("[HCC NOTIFY] StartService response missing/invalid Port\n");
        _pair_trust_state = PairTrustState::Failed;
        return true;
    }
    bool ssl = false;
    if (findXmlValueAfterKey(body, length, "EnableServiceSSL", &field_offset) &&
        !extractXmlBoolForKey(body, length, "EnableServiceSSL", &ssl)) {
        printf("[HCC NOTIFY] StartService response invalid EnableServiceSSL\n");
        _pair_trust_state = PairTrustState::Failed;
        return true;
    }

    _notification_service_port = port;
    _notification_service_ssl = ssl;
    _notification_service_generation = _pair_generation;
    _notification_service_epoch = _primary_connection_epoch;
    resetServiceTcpContext();
    _service_tcp.destination_port = port;
    _service_tcp.generation = _notification_service_generation;
    if (!ssl) {
        _notification_tcp_after_ack = true;
    } else {
        _service_tcp.state = TcpState::Failed;
    }
    _pair_trust_state = PairTrustState::NotificationServiceReady;
    printf("[HCC NOTIFY] STARTSERVICE PASS\n");
    printf("[HCC NOTIFY] port=%u\n", static_cast<unsigned>(port));
    printf("[HCC NOTIFY] ssl=%u\n", static_cast<unsigned>(ssl));
    return true;
}

bool HCCUSBTransport::handleStartSessionResponse(const uint8_t *body, size_t length)
{
    char error[128] = {};
    size_t field_offset = 0;
    if (findXmlValueAfterKey(body, length, "Error", &field_offset)) {
        if (extractXmlStringForKey(body, length, "Error", error, sizeof(error))) {
            printf("[HCC LOCKDOWN] StartSession failed error=%s\n", error);
        } else {
            printf("[HCC LOCKDOWN] StartSession failed: invalid Error\n");
        }
        if (strcmp(error, "InvalidHostID") == 0 && _session_record_loaded && !_repair_attempted &&
            !_stopping) {
            // The device no longer trusts the reused record: drop it and pair afresh once.
            printf("[HCC PAIR] PAIR RECORD REJECTED InvalidHostID\n");
            uint32_t erased_slot = 0;
            const esp_err_t erase_ret = erasePairRecordInPartition(_session_udid.data(), &erased_slot);
            if (erase_ret == ESP_OK) {
                printf("[HCC PAIR] PAIR RECORD ERASED slot=%u\n", static_cast<unsigned>(erased_slot));
            } else {
                printf("[HCC PAIR] PAIR RECORD ERASE: %s\n", esp_err_to_name(erase_ret));
            }
            _pair_record_rejected = true;
            clearStartSessionConnectionState(false);
            // The device may keep this connection open: start fresh pairing on it right away.
            // If it closes first, the RST handler falls back to a fresh-pairing reconnect.
            _getvalue_product_version_after_ack = true;
            printf("[HCC PAIR] fresh pairing on current connection\n");
            return true;
        }
        clearStartSessionConnectionState(true);
        return true;
    }
    if (findXmlValueAfterKey(body, length, "Request", &field_offset)) {
        char request[32] = {};
        if (!extractXmlStringForKey(body, length, "Request", request, sizeof(request)) ||
            strcmp(request, "StartSession") != 0) {
            printf("[HCC LOCKDOWN] StartSession response validation failed\n");
            clearStartSessionConnectionState(true);
            return true;
        }
    }
    if (findXmlValueAfterKey(body, length, "Result", &field_offset)) {
        char result[32] = {};
        if (!extractXmlStringForKey(body, length, "Result", result, sizeof(result)) ||
            strcmp(result, "Success") != 0) {
            printf("[HCC LOCKDOWN] StartSession failed: invalid Result\n");
            clearStartSessionConnectionState(true);
            return true;
        }
    }

    char session_id[kSessionIdCapacity] = {};
    if (!extractXmlStringForKey(body, length, "SessionID", session_id, sizeof(session_id)) ||
        session_id[0] == '\0') {
        printf("[HCC LOCKDOWN] StartSession failed: missing/invalid SessionID\n");
        clearStartSessionConnectionState(true);
        return true;
    }
    bool ssl_required = false;
    if (findXmlValueAfterKey(body, length, "EnableSessionSSL", &field_offset) &&
        !extractXmlBoolForKey(body, length, "EnableSessionSSL", &ssl_required)) {
        printf("[HCC LOCKDOWN] StartSession failed: invalid EnableSessionSSL\n");
        clearStartSessionConnectionState(true);
        return true;
    }
    if (!_session_record_loaded || _session_generation == 0 ||
        _tcp_state != TcpState::Connected || _primary_tcp_closed ||
        _query_type_epoch != _primary_connection_epoch) {
        printf("[HCC LOCKDOWN] StartSession failed: stale generation/epoch\n");
        clearStartSessionConnectionState(true);
        return true;
    }

    memcpy(_session_id.data(), session_id, strlen(session_id) + 1);
    mbedtls_platform_zeroize(session_id, sizeof(session_id));
    _session_ssl_required = ssl_required;
    _session_epoch = _primary_connection_epoch;
    _session_ready = true;
    _tls_after_ack = ssl_required;
    printf("[HCC LOCKDOWN] STARTSESSION PASS\n");
    printf("[HCC LOCKDOWN] EnableSessionSSL=%u\n", static_cast<unsigned>(ssl_required));
    return true;
}

bool HCCUSBTransport::handleLockdownResponse(const uint8_t *body, size_t length)
{
    static constexpr char kXmlPrefix[] = "<?xml";
    static constexpr char kBinaryPrefix[] = "bplist00";
    if (length >= sizeof(kBinaryPrefix) - 1 && memcmp(body, kBinaryPrefix, sizeof(kBinaryPrefix) - 1) == 0) {
        if (_lockdown_expected_response == LockdownResponse::GetValueProductVersion) {
            printf("[HCC LOCKDOWN] GetValue ProductVersion response is binary plist - parser not implemented\n");
        } else if (_lockdown_expected_response == LockdownResponse::GetValueDeviceClass) {
            printf("[HCC LOCKDOWN] GetValue DeviceClass response is binary plist - parser not implemented\n");
        } else if (_lockdown_expected_response == LockdownResponse::GetValueDevicePublicKey) {
            printf("[HCC LOCKDOWN] GetValue DevicePublicKey response is binary plist - parser not implemented\n");
        } else if (_lockdown_expected_response == LockdownResponse::GetValueWiFiAddress) {
            printf("[HCC LOCKDOWN] GetValue WiFiAddress response is binary plist - parser not implemented\n");
        } else if (_lockdown_expected_response == LockdownResponse::Pair) {
            printf("[HCC PAIR] Pair response is binary plist - parser not implemented\n");
            _pair_destroy_after_response = true;
        } else if (_lockdown_expected_response == LockdownResponse::StartNotificationService) {
            printf("[HCC NOTIFY] StartService response is binary plist - parser not implemented\n");
            _pair_trust_state = PairTrustState::Failed;
        } else if (_lockdown_expected_response == LockdownResponse::StartSession) {
            printf("[HCC LOCKDOWN] StartSession response is binary plist - parser not implemented\n");
            clearStartSessionConnectionState(true);
        } else {
            printf("[HCC LOCKDOWN] QueryType response is binary plist - parser not implemented\n");
        }
        _lockdown_expected_response = LockdownResponse::None;
        return true;
    }
    if (length < sizeof(kXmlPrefix) - 1 || memcmp(body, kXmlPrefix, sizeof(kXmlPrefix) - 1) != 0) {
        printf("[HCC LOCKDOWN] Lockdown response is not a recognized plist\n");
        if (_lockdown_expected_response == LockdownResponse::Pair) {
            _pair_destroy_after_response = true;
        } else if (_lockdown_expected_response == LockdownResponse::StartSession) {
            clearStartSessionConnectionState(true);
        }
        _lockdown_expected_response = LockdownResponse::None;
        return true;
    }

    if (_lockdown_expected_response == LockdownResponse::Pair) {
        const bool result = handlePairResponse(body, length);
        _lockdown_expected_response = LockdownResponse::None;
        return result;
    }
    if (_lockdown_expected_response == LockdownResponse::StartNotificationService) {
        const bool result = handleStartNotificationServiceResponse(body, length);
        _lockdown_expected_response = LockdownResponse::None;
        return result;
    }
    if (_lockdown_expected_response == LockdownResponse::StartSession) {
        const bool result = handleStartSessionResponse(body, length);
        _lockdown_expected_response = LockdownResponse::None;
        return result;
    }

    char error[128] = {};
    if (extractXmlStringForKey(body, length, "Error", error, sizeof(error))) {
        printf("[HCC LOCKDOWN] Lockdown XML Error=%s\n", error);
        _lockdown_expected_response = LockdownResponse::None;
        return true;
    }

    if (_lockdown_expected_response == LockdownResponse::QueryType) {
        printf("[HCC LOCKDOWN] QueryType response complete: XML bytes=%u\n", static_cast<unsigned>(length));
        if (containsBytes(body, length, "Type") &&
            containsBytes(body, length, "com.apple.mobile.lockdown")) {
            printf("[HCC LOCKDOWN] QUERYTYPE PASS type=com.apple.mobile.lockdown\n");
            _query_type_epoch = _primary_connection_epoch;
            if (_primary_connection_purpose == PrimaryConnectionPurpose::PairReconnect) {
                printf("[HCC LOCKDOWN] reconnect QUERYTYPE PASS epoch=%u\n",
                       static_cast<unsigned>(_primary_connection_epoch));
                _pair_after_querytype_ack = true;
            } else if (_primary_connection_purpose == PrimaryConnectionPurpose::SessionReconnect) {
                printf("[HCC LOCKDOWN] session QUERYTYPE PASS epoch=%u\n",
                       static_cast<unsigned>(_primary_connection_epoch));
                _start_session_after_querytype_ack = true;
            } else {
                if (_session_record_loaded && _session_generation != 0) {
                    printf("[HCC PAIR] PAIR RECORD REUSE -> StartSession\n");
                    _start_session_after_querytype_ack = true;
                } else {
                    _getvalue_product_version_after_ack = true;
                }
            }
        } else {
            printf("[HCC LOCKDOWN] QueryType XML response does not contain Type=com.apple.mobile.lockdown\n");
        }
    } else if (_lockdown_expected_response == LockdownResponse::GetValueProductVersion) {
        char request[32] = {};
        char value[128] = {};
        if (extractXmlStringForKey(body, length, "Request", request, sizeof(request)) &&
            strcmp(request, "GetValue") == 0 &&
            extractXmlStringForKey(body, length, "Value", value, sizeof(value))) {
            printf("[HCC LOCKDOWN] GETVALUE PRODUCTVERSION PASS value=%s\n", value);
            _getvalue_device_class_after_ack = true;
        } else {
            printf("[HCC LOCKDOWN] GetValue ProductVersion XML response validation failed\n");
        }
    } else if (_lockdown_expected_response == LockdownResponse::GetValueDeviceClass) {
        char request[32] = {};
        char value[128] = {};
        if (extractXmlStringForKey(body, length, "Request", request, sizeof(request)) &&
            strcmp(request, "GetValue") == 0 &&
            extractXmlStringForKey(body, length, "Value", value, sizeof(value))) {
            printf("[HCC LOCKDOWN] GETVALUE DEVICECLASS PASS value=%s\n", value);
            _getvalue_device_public_key_after_ack = true;
        } else {
            printf("[HCC LOCKDOWN] GetValue DeviceClass XML response validation failed\n");
        }
    } else if (_lockdown_expected_response == LockdownResponse::GetValueDevicePublicKey) {
        char request[32] = {};
        size_t base64_chars = 0;
        size_t data_bytes = 0;
        if (extractXmlStringForKey(body, length, "Request", request, sizeof(request)) &&
            strcmp(request, "GetValue") == 0 &&
            extractXmlBase64DataForKey(body, length, "Value", &base64_chars, &data_bytes)) {
            size_t copied_bytes = 0;
            if (data_bytes <= _device_public_key.size() &&
                decodeXmlBase64DataForKey(body, length, "Value", _device_public_key.data(),
                                           _device_public_key.size(), &copied_bytes) &&
                copied_bytes == data_bytes) {
                _device_public_key_length = copied_bytes;
                _device_public_key_pending = true;
                _device_public_key_wait_logged = false;
                _device_public_key_wakeup_logged = false;
                _device_public_key_utc_ready_logged = false;
                printf("[HCC LOCKDOWN] GETVALUE DEVICEPUBLICKEY PASS data_bytes=%u base64_chars=%u\n",
                       static_cast<unsigned>(data_bytes), static_cast<unsigned>(base64_chars));
                _getvalue_wifi_address_after_ack = true;
                // Key is now queued for pairing; the fresh-pairing reconnect fallback must not replay it.
                _pair_record_rejected = false;
            } else {
                mbedtls_platform_zeroize(_device_public_key.data(), _device_public_key.size());
                _device_public_key_length = 0;
                printf("[HCC LOCKDOWN] GetValue DevicePublicKey decoded handoff failed\n");
            }
        } else {
            printf("[HCC LOCKDOWN] GetValue DevicePublicKey XML response validation failed\n");
        }
    } else if (_lockdown_expected_response == LockdownResponse::GetValueWiFiAddress) {
        char request[32] = {};
        char value[128] = {};
        if (extractXmlStringForKey(body, length, "Request", request, sizeof(request)) &&
            strcmp(request, "GetValue") == 0 &&
            extractXmlStringForKey(body, length, "Value", value, sizeof(value))) {
            printf("[HCC LOCKDOWN] GETVALUE WIFIADDRESS PASS value=%s\n", value);
            const size_t value_length = strlen(value);
            if (value_length < _wifi_address.size()) {
                memcpy(_wifi_address.data(), value, value_length + 1);
                _wifi_address_valid = true;
                if (_pair_bundle != nullptr) {
                    _pair_prefix_requested = true;
                }
            }
        } else {
            printf("[HCC LOCKDOWN] GetValue WiFiAddress XML response validation failed\n");
        }
    } else {
        printf("[HCC LOCKDOWN] Unexpected XML response bytes=%u\n", static_cast<unsigned>(length));
    }
    _lockdown_expected_response = LockdownResponse::None;
    return true;
}

bool HCCUSBTransport::preparePairTimeWait()
{
    if (!_device_public_key_pending || _stopping ||
        time(nullptr) >= static_cast<time_t>(1704067200)) {
        return false;
    }
    if (!_device_public_key_wakeup_logged) {
        printf("[HCC PAIR] UTC wait wakeup enabled interval_ms=1000\n");
        _device_public_key_wakeup_logged = true;
    }
    return true;
}

bool HCCUSBTransport::handoffDevicePublicKey(DevicePublicKeyConsumer consumer, void *context)
{
    if (!_device_public_key_pending) {
        return true;
    }
    if (consumer == nullptr || _device_public_key_length == 0 ||
        _device_public_key_length > _device_public_key.size()) {
        return false;
    }
    // Defer without consuming the key; the USB client owner checks again
    // after its next handle_events() return. Match the worker's UTC threshold.
    if (time(nullptr) < static_cast<time_t>(1704067200)) {
        if (!_device_public_key_wait_logged) {
            printf("[HCC PAIR] waiting for valid UTC\n");
            _device_public_key_wait_logged = true;
        }
        return true;
    }
    if (!_device_public_key_utc_ready_logged) {
        printf("[HCC PAIR] UTC ready - starting crypto\n");
        _device_public_key_utc_ready_logged = true;
    }
    if (!consumer(context, _device_public_key.data(), _device_public_key_length)) {
        return false;
    }
    mbedtls_platform_zeroize(_device_public_key.data(), _device_public_key.size());
    _device_public_key_length = 0;
    _device_public_key_pending = false;
    _device_public_key_wait_logged = false;
    _device_public_key_wakeup_logged = false;
    _device_public_key_utc_ready_logged = false;
    return true;
}

const char *HCCUSBTransport::txKindName(TxKind tx_kind)
{
    switch (tx_kind) {
    case TxKind::Version:
        return "VERSION";
    case TxKind::Setup:
        return "SETUP";
    case TxKind::TcpSyn:
        return "SYN";
    case TxKind::TcpAck:
        return "ACK";
    case TxKind::LockdownPrefix:
        return "LOCKDOWN_PREFIX";
    case TxKind::LockdownBody:
        return "LOCKDOWN_BODY";
    case TxKind::TcpDataAck:
        return "DATA_ACK";
    case TxKind::GetValueProductVersionPrefix:
        return "GETVALUE_PRODUCTVERSION_PREFIX";
    case TxKind::GetValueProductVersionBody:
        return "GETVALUE_PRODUCTVERSION_BODY";
    case TxKind::GetValueDeviceClassPrefix:
        return "GETVALUE_DEVICECLASS_PREFIX";
    case TxKind::GetValueDeviceClassBody:
        return "GETVALUE_DEVICECLASS_BODY";
    case TxKind::GetValueDevicePublicKeyPrefix:
        return "GETVALUE_DEVICEPUBLICKEY_PREFIX";
    case TxKind::GetValueDevicePublicKeyBody:
        return "GETVALUE_DEVICEPUBLICKEY_BODY";
    case TxKind::GetValueWiFiAddressPrefix:
        return "GETVALUE_WIFIADDRESS_PREFIX";
    case TxKind::GetValueWiFiAddressBody:
        return "GETVALUE_WIFIADDRESS_BODY";
    case TxKind::PairPrefix:
        return "PAIR_PREFIX";
    case TxKind::PairChunk:
        return "PAIR_CHUNK";
    case TxKind::StartNotificationServicePrefix:
        return "START_NOTIFICATION_SERVICE_PREFIX";
    case TxKind::StartNotificationServiceBody:
        return "START_NOTIFICATION_SERVICE_BODY";
    case TxKind::StartSessionPrefix:
        return "STARTSESSION_PREFIX";
    case TxKind::StartSessionBody:
        return "STARTSESSION_BODY";
    case TxKind::TlsCiphertext:
        return "TLS_CIPHERTEXT";
    case TxKind::ServiceTcpSyn:
        return "SERVICE_SYN";
    case TxKind::ServiceTcpAck:
        return "SERVICE_ACK";
    case TxKind::PrimaryTrustFin:
        return "PRIMARY_TRUST_FIN";
    case TxKind::PrimaryTrustCloseAck:
        return "PRIMARY_TRUST_CLOSE_ACK";
    case TxKind::NotificationPrefix:
        return "NOTIFY_PREFIX";
    case TxKind::NotificationBody:
        return "NOTIFY_BODY";
    default:
        return "UNKNOWN";
    }
}

void HCCUSBTransport::retireTransfer(usb_transfer_t *transfer, bool is_rx)
{
    if (is_rx) {
        const int slot = findRxSlot(transfer);
        if (slot >= 0) {
            // Keep the persistent RX transfer allocated for reuse by the next start().
            _rx_in_flight[slot] = false;
            return;
        }
    } else if (_tx_transfer == transfer) {
        _tx_in_flight = false;
        _tx_transfer = nullptr;
    }

    usb_host_transfer_free(transfer);
}

int HCCUSBTransport::findRxSlot(const usb_transfer_t *transfer) const
{
    for (size_t slot = 0; slot < kRxTransferCount; ++slot) {
        if (_rx_transfers[slot] == transfer) {
            return static_cast<int>(slot);
        }
    }

    return -1;
}

bool HCCUSBTransport::hasRxTransfersInFlight() const
{
    for (bool in_flight : _rx_in_flight) {
        if (in_flight) {
            return true;
        }
    }

    return false;
}

void HCCUSBTransport::onRxComplete(usb_transfer_t *transfer)
{
    if (transfer == nullptr) {
        return;
    }

    HCCUSBTransport *transport = static_cast<HCCUSBTransport *>(transfer->context);
    if (transport == nullptr) {
        usb_host_transfer_free(transfer);
        return;
    }

    const int rx_slot = transport->findRxSlot(transfer);
    if (rx_slot < 0) {
        printf("[HCC USB MUX] RX callback for unknown transfer\n");
        usb_host_transfer_free(transfer);
        return;
    }

    transport->_rx_in_flight[rx_slot] = false;

    printf("[HCC USB MUX] RX[%u] complete: actual=%d status=%d\\n",
           static_cast<unsigned>(rx_slot),
           transfer->actual_num_bytes,
           transfer->status);

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        bool packet_valid = true;
        if (transfer->actual_num_bytes < 0) {
            packet_valid = false;
        } else if (transport->_state == State::WaitingVersion) {
            if (transfer->actual_num_bytes == static_cast<int>(kVersionPacketSize)) {
                logVersionRaw(transfer->data_buffer);
            }
            packet_valid = transport->parseVersionResponse(
                transfer->data_buffer,
                static_cast<size_t>(transfer->actual_num_bytes)
            );
        } else if (transport->_v2_framing) {
            logRxRaw(rx_slot, transfer->data_buffer, static_cast<size_t>(transfer->actual_num_bytes));
            packet_valid = transport->appendRxStream(
                transfer->data_buffer,
                static_cast<size_t>(transfer->actual_num_bytes)
            );
        }

        if (packet_valid && !transport->_stopping && transport->_state != State::Failed &&
            transport->submitRx(static_cast<size_t>(rx_slot))) {
            return;
        }
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        printf("[HCC USB MUX] VERSION failed: RX NO_DEVICE\n");
    } else if (transfer->status == USB_TRANSFER_STATUS_CANCELED) {
        printf("[HCC USB MUX] VERSION failed: RX CANCELED\n");
    } else {
        printf("[HCC USB MUX] VERSION failed: RX status=%d\n", transfer->status);
    }

    transport->retireTransfer(transfer, true);
}

void HCCUSBTransport::onTxComplete(usb_transfer_t *transfer)
{
    if (transfer == nullptr) {
        return;
    }

    HCCUSBTransport *transport = static_cast<HCCUSBTransport *>(transfer->context);
    if (transport == nullptr) {
        usb_host_transfer_free(transfer);
        return;
    }

    puts("[TXCB] enter");
    const TxKind tx_kind = transport->_tx_kind;
    const bool service_tx = tx_kind == TxKind::ServiceTcpSyn ||
                            tx_kind == TxKind::ServiceTcpAck ||
                            tx_kind == TxKind::NotificationPrefix ||
                            tx_kind == TxKind::NotificationBody;
    const bool tx_completed = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
    const int actual_num_bytes = transfer->actual_num_bytes;
    puts("[TXCB] fields-read");
    printf("[HCC USB MUX] TX %s complete: actual=%d status=%d\\n",
           txKindName(tx_kind),
           transfer->actual_num_bytes,
           transfer->status);

    if (!tx_completed) {
        if (tx_kind == TxKind::TlsCiphertext) {
            transport->clearStartSessionConnectionState(true);
            transport->_tls_state = TlsState::Failed;
        }
        if (service_tx) {
            transport->_service_tcp.state = TcpState::Failed;
            transport->_service_tcp.syn_requested = false;
            transport->_service_tcp.ack_requested = false;
        } else if (!transport->_stopping) {
            transport->_state = State::Failed;
            transport->_tcp_state = TcpState::Failed;
        }
        if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
            printf("[HCC USB MUX] %s failed: TX NO_DEVICE\n", txKindName(tx_kind));
        } else if (transfer->status == USB_TRANSFER_STATUS_CANCELED) {
            printf("[HCC USB MUX] %s failed: TX CANCELED\n", txKindName(tx_kind));
        } else {
            printf("[HCC USB MUX] %s failed: TX status=%d\n", txKindName(tx_kind), transfer->status);
        }
    }

    transport->retireTransfer(transfer, false);
    if (tx_completed) {
        if (tx_kind == TxKind::Setup) {
            transport->_state = State::MuxActive;
            transport->_tcp_syn_requested = true;
            printf("[HCC USB MUX] MUX active - TCP SYN pending port=62078\n");
        } else if (tx_kind == TxKind::TcpAck) {
            transport->_tcp_state = TcpState::Connected;
            transport->_lockdown_prefix_requested = true;
            if (transport->_primary_connection_purpose == PrimaryConnectionPurpose::PairReconnect) {
                printf("[HCC LOCKDOWN] LOCKDOWN TCP RECONNECTED port=62078 epoch=%u\n",
                       static_cast<unsigned>(transport->_primary_connection_epoch));
            } else if (transport->_primary_connection_purpose == PrimaryConnectionPurpose::SessionReconnect) {
                printf("[HCC LOCKDOWN] LOCKDOWN TCP RECONNECTED for StartSession port=62078 epoch=%u\n",
                       static_cast<unsigned>(transport->_primary_connection_epoch));
            } else {
                printf("[HCC USB MUX] LOCKDOWN TCP CONNECTED port=62078\n");
            }
        } else if (tx_kind == TxKind::LockdownPrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_lockdown_body_requested = true;
            printf("[HCC LOCKDOWN] QueryType prefix complete\n");
        } else if (tx_kind == TxKind::LockdownBody) {
            transport->_tcp_tx_seq += static_cast<uint32_t>(kLockdownQueryTypeBodySize);
            transport->_lockdown_expected_response = LockdownResponse::QueryType;
            printf("[HCC LOCKDOWN] QueryType sent body_len=%u\n",
                   static_cast<unsigned>(kLockdownQueryTypeBodySize));
        } else if (tx_kind == TxKind::TcpDataAck) {
            if (transport->_tls_after_ack) {
                transport->_tls_after_ack = false;
                transport->_tls_generation = transport->_session_generation;
                transport->_tls_epoch = transport->_session_epoch;
                transport->_tls_state = TlsState::TlsPrepare;
                transport->_tls_handshake_step_requested = true;
            } else if (transport->_pair_after_querytype_ack) {
                transport->_pair_after_querytype_ack = false;
                transport->authorizePairForCurrentConnection();
            } else if (transport->_start_session_after_querytype_ack ||
                       transport->_start_session_after_ack) {
                transport->_start_session_after_querytype_ack = false;
                transport->_start_session_after_ack = false;
                if (transport->_session_record_loaded &&
                    transport->_query_type_epoch == transport->_primary_connection_epoch) {
                    transport->_start_session_prefix_requested = true;
                }
            } else if (transport->_getvalue_product_version_after_ack) {
                transport->_getvalue_product_version_after_ack = false;
                transport->_getvalue_product_version_prefix_requested = true;
                printf("[HCC LOCKDOWN] GetValue ProductVersion prefix pending\n");
            } else if (transport->_getvalue_device_class_after_ack) {
                transport->_getvalue_device_class_after_ack = false;
                transport->_getvalue_device_class_prefix_requested = true;
                printf("[HCC LOCKDOWN] GetValue DeviceClass prefix pending\n");
            } else if (transport->_getvalue_device_public_key_after_ack) {
                transport->_getvalue_device_public_key_after_ack = false;
                transport->_getvalue_device_public_key_prefix_requested = true;
                printf("[HCC LOCKDOWN] GetValue DevicePublicKey prefix pending\n");
            } else if (transport->_getvalue_wifi_address_after_ack) {
                transport->_getvalue_wifi_address_after_ack = false;
                transport->_getvalue_wifi_address_prefix_requested = true;
                printf("[HCC LOCKDOWN] GetValue WiFiAddress prefix pending\n");
            } else if (transport->_start_notification_service_after_ack) {
                transport->_start_notification_service_after_ack = false;
                transport->_start_notification_service_prefix_requested = true;
                transport->_pair_trust_state = PairTrustState::StartNotificationServicePending;
                printf("[HCC NOTIFY] StartService prefix pending\n");
            } else if (transport->_notification_tcp_after_ack) {
                transport->_notification_tcp_after_ack = false;
                transport->_service_tcp.syn_requested = true;
            }
        } else if (tx_kind == TxKind::GetValueProductVersionPrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_getvalue_product_version_body_requested = true;
            printf("[HCC LOCKDOWN] GetValue ProductVersion prefix complete\n");
        } else if (tx_kind == TxKind::GetValueProductVersionBody) {
            transport->_tcp_tx_seq += static_cast<uint32_t>(kLockdownGetValueProductVersionBodySize);
            transport->_lockdown_expected_response = LockdownResponse::GetValueProductVersion;
            printf("[HCC LOCKDOWN] GetValue ProductVersion sent body_len=%u\n",
                   static_cast<unsigned>(kLockdownGetValueProductVersionBodySize));
        } else if (tx_kind == TxKind::GetValueDeviceClassPrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_getvalue_device_class_body_requested = true;
            printf("[HCC LOCKDOWN] GetValue DeviceClass prefix complete\n");
        } else if (tx_kind == TxKind::GetValueDeviceClassBody) {
            transport->_tcp_tx_seq += static_cast<uint32_t>(kLockdownGetValueDeviceClassBodySize);
            transport->_lockdown_expected_response = LockdownResponse::GetValueDeviceClass;
            printf("[HCC LOCKDOWN] GetValue DeviceClass sent body_len=%u\n",
                   static_cast<unsigned>(kLockdownGetValueDeviceClassBodySize));
        } else if (tx_kind == TxKind::GetValueDevicePublicKeyPrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_getvalue_device_public_key_body_requested = true;
            printf("[HCC LOCKDOWN] GetValue DevicePublicKey prefix complete\n");
        } else if (tx_kind == TxKind::GetValueDevicePublicKeyBody) {
            transport->_tcp_tx_seq += static_cast<uint32_t>(kLockdownGetValueDevicePublicKeyBodySize);
            transport->_lockdown_expected_response = LockdownResponse::GetValueDevicePublicKey;
            printf("[HCC LOCKDOWN] GetValue DevicePublicKey sent body_len=%u\n",
                   static_cast<unsigned>(kLockdownGetValueDevicePublicKeyBodySize));
        } else if (tx_kind == TxKind::GetValueWiFiAddressPrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_getvalue_wifi_address_body_requested = true;
            printf("[HCC LOCKDOWN] GetValue WiFiAddress prefix complete\n");
        } else if (tx_kind == TxKind::GetValueWiFiAddressBody) {
            transport->_tcp_tx_seq += static_cast<uint32_t>(kLockdownGetValueWiFiAddressBodySize);
            transport->_lockdown_expected_response = LockdownResponse::GetValueWiFiAddress;
            printf("[HCC LOCKDOWN] GetValue WiFiAddress sent body_len=%u\n",
                   static_cast<unsigned>(kLockdownGetValueWiFiAddressBodySize));
        } else if (tx_kind == TxKind::PairPrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_pair_chunk_requested = true;
        } else if (tx_kind == TxKind::PairChunk) {
            const size_t payload_length = actual_num_bytes >= static_cast<int>(kTcpMuxPacketSize)
                                              ? static_cast<size_t>(actual_num_bytes) - kTcpMuxPacketSize : 0;
            transport->_tcp_tx_seq += static_cast<uint32_t>(payload_length);
            transport->_pair_xml_offset += payload_length;
            if (transport->_pair_xml_offset < transport->_pair_xml_length) {
                transport->_pair_chunk_requested = true;
            } else {
                transport->_lockdown_expected_response = LockdownResponse::Pair;
                printf("[HCC PAIR] Pair request sent\n");
            }
        } else if (tx_kind == TxKind::StartNotificationServicePrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_start_notification_service_body_requested = true;
            printf("[HCC NOTIFY] StartService prefix complete\n");
        } else if (tx_kind == TxKind::StartNotificationServiceBody) {
            transport->_tcp_tx_seq += static_cast<uint32_t>(kLockdownStartNotificationServiceBodySize);
            transport->_lockdown_expected_response = LockdownResponse::StartNotificationService;
            printf("[HCC NOTIFY] StartService request sent\n");
        } else if (tx_kind == TxKind::StartSessionPrefix) {
            transport->_tcp_tx_seq += sizeof(uint32_t);
            transport->_start_session_body_requested = true;
        } else if (tx_kind == TxKind::StartSessionBody) {
            transport->_tcp_tx_seq += static_cast<uint32_t>(transport->_start_session_xml_length);
            transport->_lockdown_expected_response = LockdownResponse::StartSession;
            printf("[HCC LOCKDOWN] StartSession request sent\n");
        } else if (tx_kind == TxKind::TlsCiphertext) {
            const size_t payload_length = actual_num_bytes >= static_cast<int>(kTcpMuxPacketSize)
                                              ? static_cast<size_t>(actual_num_bytes) - kTcpMuxPacketSize : 0;
            transport->_tcp_tx_seq += static_cast<uint32_t>(payload_length);
            if (transport->_tls != nullptr) {
                transport->_tls->tx_offset += payload_length;
                if (transport->_tls->tx_offset >= transport->_tls->tx_length) {
                    mbedtls_platform_zeroize(transport->_tls->tx_staging,
                                              transport->_tls->tx_length);
                    transport->_tls->tx_length = 0;
                    transport->_tls->tx_offset = 0;
                    transport->_tls_handshake_step_requested = true;
                    if (transport->_tls_state == TlsState::TlsEstablished &&
                        transport->_tls->lockdown_tx_waiting_for_ciphertext &&
                        transport->_tls->lockdown_tx_offset ==
                            transport->_tls->lockdown_tx_length) {
                        transport->_tls->lockdown_tx_waiting_for_ciphertext = false;
                        transport->_tls->app_state = TlsContext::AppState::StartServiceRead;
                        printf("[HCC TLS] encrypted StartService request sent\n");
                    }
                }
            }
        } else if (tx_kind == TxKind::ServiceTcpAck) {
            if (transport->_service_tcp.state == TcpState::SynSent) {
                transport->_service_tcp.state = TcpState::Connected;
                transport->_service_tcp.notification_index = 0;
                if (transport->_pair_trust_state == PairTrustState::NotificationServiceReady) {
                    transport->_primary_trust_fin_requested = true;
                    printf("[HCC NOTIFY] TCP #2 connected; primary close pending\n");
                } else {
                    transport->_service_tcp.notification_prefix_requested = true;
                }
                printf("[HCC SERVICE] TCP CONNECTED port=%u\n",
                       static_cast<unsigned>(transport->_service_tcp.destination_port));
            }
        } else if (tx_kind == TxKind::PrimaryTrustFin) {
            ++transport->_tcp_tx_seq;
            transport->_primary_trust_fin_dispatched = true;
            transport->_service_tcp.notification_prefix_requested = true;
            printf("[HCC NOTIFY] primary Lockdown FIN dispatched; starting ObserveNotification\n");
        } else if (tx_kind == TxKind::PrimaryTrustCloseAck) {
            transport->_tcp_state = TcpState::Failed;
            transport->_primary_tcp_closed = true;
            printf("[HCC NOTIFY] primary Lockdown close ACK dispatched\n");
        } else if (tx_kind == TxKind::NotificationPrefix) {
            const size_t payload_length = actual_num_bytes >= static_cast<int>(kTcpMuxPacketSize)
                                              ? static_cast<size_t>(actual_num_bytes) - kTcpMuxPacketSize : 0;
            transport->_service_tcp.tx_seq += static_cast<uint32_t>(payload_length);
            transport->_service_tcp.notification_body_requested = true;
        } else if (tx_kind == TxKind::NotificationBody) {
            const size_t payload_length = actual_num_bytes >= static_cast<int>(kTcpMuxPacketSize)
                                              ? static_cast<size_t>(actual_num_bytes) - kTcpMuxPacketSize : 0;
            transport->_service_tcp.tx_seq += static_cast<uint32_t>(payload_length);
            const size_t request_count =
                sizeof(kNotificationObserveRequests) / sizeof(kNotificationObserveRequests[0]);
            const uint8_t completed_index = transport->_service_tcp.notification_index;
            if (completed_index < request_count) {
                printf("[HCC NOTIFY] observing Name=%s\n",
                       kNotificationObserveRequests[completed_index].name);
            }
            ++transport->_service_tcp.notification_index;
            if (transport->_service_tcp.notification_index < request_count) {
                transport->_service_tcp.notification_prefix_requested = true;
            } else {
                printf("[HCC NOTIFY] ObserveNotification subscriptions ready\n");
            }
        }
    }
    transport->_tx_kind = TxKind::None;
    if (tx_completed) {
        puts("[TXCB] exit");
    }
}
