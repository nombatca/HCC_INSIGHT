#include "HCCPairIdentity.hpp"

#include "esp_err.h"
#include "esp_random.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr char kNvsNamespace[] = "hcc_pair";
constexpr char kSystemBuidKey[] = "system_buid";
constexpr char kHostIdKey[] = "host_id";
constexpr char kHex[] = "0123456789ABCDEF";

enum class IdentitySource {
    Nvs,
    Generated,
};

bool loadOrGenerate(nvs_handle_t handle, const char *key, char *value, size_t value_size,
                    IdentitySource *source, void (*generate_uuid)(char *))
{
    size_t stored_size = value_size;
    const esp_err_t read_ret = nvs_get_str(handle, key, value, &stored_size);
    if (read_ret == ESP_OK) {
        *source = IdentitySource::Nvs;
        return true;
    }
    if (read_ret != ESP_ERR_NVS_NOT_FOUND) {
        printf("[HCC PAIR] HOST IDENTITY failure: key=%s malformed or unreadable: %s\n",
               key, esp_err_to_name(read_ret));
        return false;
    }

    generate_uuid(value);
    *source = IdentitySource::Generated;
    return true;
}

} // namespace

bool HCCPairIdentity::initialize()
{
    if (_initialized) {
        return true;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        printf("[HCC PAIR] NVS open failed namespace=%s: %s\n", kNvsNamespace, esp_err_to_name(ret));
        return false;
    }

    IdentitySource system_buid_source = IdentitySource::Nvs;
    IdentitySource host_id_source = IdentitySource::Nvs;
    bool success = loadOrGenerate(handle, kSystemBuidKey, _system_buid.data(), _system_buid.size(),
                                  &system_buid_source, HCCPairIdentity::generateUuid) &&
                   loadOrGenerate(handle, kHostIdKey, _host_id.data(), _host_id.size(),
                                  &host_id_source, HCCPairIdentity::generateUuid);

    if (success && (!isValidUuid(_system_buid.data()) || !isValidUuid(_host_id.data()))) {
        printf("[HCC PAIR] HOST IDENTITY validation failed: malformed NVS UUID\n");
        success = false;
    }
    if (success && strcmp(_system_buid.data(), _host_id.data()) == 0) {
        printf("[HCC PAIR] HOST IDENTITY validation failed: SystemBUID equals HostID\n");
        success = false;
    }

    if (success && system_buid_source == IdentitySource::Generated) {
        ret = nvs_set_str(handle, kSystemBuidKey, _system_buid.data());
        if (ret != ESP_OK) {
            printf("[HCC PAIR] NVS write failed key=%s: %s\n", kSystemBuidKey, esp_err_to_name(ret));
            success = false;
        }
    }
    if (success && host_id_source == IdentitySource::Generated) {
        ret = nvs_set_str(handle, kHostIdKey, _host_id.data());
        if (ret != ESP_OK) {
            printf("[HCC PAIR] NVS write failed key=%s: %s\n", kHostIdKey, esp_err_to_name(ret));
            success = false;
        }
    }
    if (success && (system_buid_source == IdentitySource::Generated || host_id_source == IdentitySource::Generated)) {
        ret = nvs_commit(handle);
        if (ret != ESP_OK) {
            printf("[HCC PAIR] NVS commit failed: %s\n", esp_err_to_name(ret));
            success = false;
        }
    }

    std::array<char, kUuidStorageSize> readback_system_buid{};
    std::array<char, kUuidStorageSize> readback_host_id{};
    if (success) {
        size_t system_buid_size = readback_system_buid.size();
        size_t host_id_size = readback_host_id.size();
        ret = nvs_get_str(handle, kSystemBuidKey, readback_system_buid.data(), &system_buid_size);
        if (ret == ESP_OK) {
            ret = nvs_get_str(handle, kHostIdKey, readback_host_id.data(), &host_id_size);
        }
        if (ret != ESP_OK || !isValidUuid(readback_system_buid.data()) ||
            !isValidUuid(readback_host_id.data()) ||
            strcmp(readback_system_buid.data(), _system_buid.data()) != 0 ||
            strcmp(readback_host_id.data(), _host_id.data()) != 0 ||
            strcmp(readback_system_buid.data(), readback_host_id.data()) == 0) {
            printf("[HCC PAIR] HOST IDENTITY validation failed: NVS readback\n");
            success = false;
        }
    }

    nvs_close(handle);
    if (!success) {
        return false;
    }

    printf("[HCC PAIR] SystemBUID=%s source=%s\n", _system_buid.data(),
           system_buid_source == IdentitySource::Generated ? "generated" : "nvs");
    printf("[HCC PAIR] HostID=%s source=%s\n", _host_id.data(),
           host_id_source == IdentitySource::Generated ? "generated" : "nvs");
    printf("[HCC PAIR] HOST IDENTITY PASS\n");
    _initialized = true;
    return true;
}

const char *HCCPairIdentity::systemBuid() const
{
    return _system_buid.data();
}

const char *HCCPairIdentity::hostId() const
{
    return _host_id.data();
}

bool HCCPairIdentity::isValidUuid(const char *value)
{
    if (value == nullptr || strnlen(value, kUuidStorageSize) != kUuidStringLength) {
        return false;
    }
    for (size_t index = 0; index < kUuidStringLength; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'A' && value[index] <= 'F'))) {
            return false;
        }
    }
    return true;
}

void HCCPairIdentity::generateUuid(char *value)
{
    uint8_t bytes[16] = {};
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    size_t output = 0;
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        if (output == 8 || output == 13 || output == 18 || output == 23) {
            value[output++] = '-';
        }
        value[output++] = kHex[bytes[index] >> 4];
        value[output++] = kHex[bytes[index] & 0x0F];
    }
    value[output] = '\0';
}
