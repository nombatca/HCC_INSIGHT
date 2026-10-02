#include "HCCPairCredentials.hpp"
#include "hcc_cores.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/asn1write.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

constexpr size_t kWorkerStackBytes = 28672;
constexpr UBaseType_t kWorkerPriority = 4;
constexpr size_t kPemBufferSize = 4096;
constexpr time_t kEarliestValidUtc = 1704067200; // 2024-01-01T00:00:00Z
constexpr char kDrbgPersonalization[] = "HCCPairCredentials-v1";
constexpr char kRootCertificateName[] = "CN=HCC Root CA";
constexpr char kHostCertificateName[] = "CN=HCC Host";
constexpr char kDeviceCertificateName[] = "CN=HCC Device";

void logWorkerStack(const char *stage)
{
    // ESP-IDF 5.5.5 / ESP32-P4 reports the minimum free stack in bytes.
    printf("[HCC PAIR CRYPTO] stack stage=%s high_water_mark_bytes=%u\n", stage,
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void logHeap(const char *stage)
{
    printf("[HCC PAIR CRYPTO] heap stage=%s free=%u min=%u\n", stage,
           static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(esp_get_minimum_free_heap_size()));
}

void logCryptoCreationHeap(const char *stage)
{
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    printf("[HCC HEAP] CRYPTO %s internal free=%u largest=%u min=%u psram_free=%u psram_largest=%u psram_min=%u\n", stage,
           static_cast<unsigned>(heap_caps_get_free_size(internal)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
           static_cast<unsigned>(heap_caps_get_free_size(psram)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(psram)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(psram)));
}

void logRootCertificateHeap(const char *stage)
{
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    printf("[HCC PAIR CRYPTO] %s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u internal_min=%u stack_high_water_mark_bytes=%u\n", stage,
           static_cast<unsigned>(heap_caps_get_free_size(internal)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
           static_cast<unsigned>(heap_caps_get_free_size(dma)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(dma)),
           static_cast<unsigned>(heap_caps_get_free_size(psram)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(psram)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void logMbedtlsFailure(const char *stage, int ret)
{
    char error[128] = {};
    mbedtls_strerror(ret, error, sizeof(error));
    printf("[HCC PAIR CRYPTO] %s failed ret=%d (%s)\n", stage, ret, error);
}

int setBasicConstraintsCritical(mbedtls_x509write_cert *cert, bool is_ca)
{
    int ret = 0;
    unsigned char buffer[9] = {};
    unsigned char *cursor = buffer + sizeof(buffer);
    size_t length = 0;

    if (is_ca) {
        MBEDTLS_ASN1_CHK_ADD(length, mbedtls_asn1_write_bool(&cursor, buffer, 1));
    }
    MBEDTLS_ASN1_CHK_ADD(length, mbedtls_asn1_write_len(&cursor, buffer, length));
    MBEDTLS_ASN1_CHK_ADD(length, mbedtls_asn1_write_tag(&cursor, buffer,
                                                        MBEDTLS_ASN1_CONSTRUCTED |
                                                        MBEDTLS_ASN1_SEQUENCE));
    return mbedtls_x509write_crt_set_extension(
        cert, MBEDTLS_OID_BASIC_CONSTRAINTS, MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS),
        1, cursor, length);
}

bool writeCertificatePem(mbedtls_pk_context *subject_key, mbedtls_pk_context *issuer_key,
                         const char *subject_name, const char *issuer_name,
                         const char *not_before, const char *not_after,
                         bool is_ca, bool key_usage, mbedtls_ctr_drbg_context *drbg,
                         unsigned char *output, size_t output_size, size_t *pem_size,
                         const char *stage)
{
    mbedtls_x509write_cert cert;
    mbedtls_x509write_crt_init(&cert);
    unsigned char serial[] = {0x01};
    int ret = mbedtls_x509write_crt_set_serial_raw(&cert, serial, sizeof(serial));
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_subject_name(&cert, subject_name);
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_issuer_name(&cert, issuer_name);
    }
    if (ret == 0) {
        mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
        ret = setBasicConstraintsCritical(&cert, is_ca);
    }
    if (ret == 0) {
        mbedtls_x509write_crt_set_subject_key(&cert, subject_key);
        ret = mbedtls_x509write_crt_set_subject_key_identifier(&cert);
    }
    if (ret == 0 && key_usage) {
        ret = mbedtls_x509write_crt_set_key_usage(
            &cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_validity(&cert, not_before, not_after);
    }
    if (ret == 0) {
        mbedtls_x509write_crt_set_issuer_key(&cert, issuer_key);
        mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
        ret = mbedtls_x509write_crt_pem(&cert, output, output_size,
                                         mbedtls_ctr_drbg_random, drbg);
    }
    mbedtls_x509write_crt_free(&cert);
    if (ret != 0) {
        logMbedtlsFailure(stage, ret);
        return false;
    }
    *pem_size = strnlen(reinterpret_cast<const char *>(output), output_size);
    if (*pem_size == output_size) {
        printf("[HCC PAIR CRYPTO] %s failed: PEM output not terminated\n", stage);
        return false;
    }
    return true;
}

bool publicKeysEqual(const mbedtls_pk_context *left, const mbedtls_pk_context *right)
{
    std::array<unsigned char, 1024> left_der{};
    std::array<unsigned char, 1024> right_der{};
    const int left_length = mbedtls_pk_write_pubkey_der(left, left_der.data(), left_der.size());
    const int right_length = mbedtls_pk_write_pubkey_der(right, right_der.data(), right_der.size());
    const bool equal = left_length > 0 && right_length > 0 && left_length == right_length &&
                       memcmp(left_der.data() + left_der.size() - left_length,
                              right_der.data() + right_der.size() - right_length,
                              static_cast<size_t>(left_length)) == 0;
    mbedtls_platform_zeroize(left_der.data(), left_der.size());
    mbedtls_platform_zeroize(right_der.data(), right_der.size());
    return equal;
}

bool validUtcNow(char *not_before, size_t not_before_size, char *not_after, size_t not_after_size)
{
    const time_t now = time(nullptr);
    if (now < kEarliestValidUtc) {
        return false;
    }
    const time_t then = now + static_cast<time_t>(60 * 60 * 24 * 365 * 10);
    struct tm utc_now = {};
    struct tm utc_then = {};
    if (gmtime_r(&now, &utc_now) == nullptr || gmtime_r(&then, &utc_then) == nullptr ||
        strftime(not_before, not_before_size, "%Y%m%d%H%M%S", &utc_now) != 14 ||
        strftime(not_after, not_after_size, "%Y%m%d%H%M%S", &utc_then) != 14) {
        return false;
    }
    return true;
}

} // namespace

HCCPairCredentials::HCCPairCredentials()
{
    _completed_queue = xQueueCreateStatic(1, sizeof(HCCPairCredentialBundle *),
                                           _completed_queue_buffer, &_completed_queue_storage);
}

HCCPairCredentialBundle *HCCPairCredentials::allocateBundle(uint32_t generation)
{
    constexpr uint32_t capabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    HCCPairCredentialBundle *bundle = static_cast<HCCPairCredentialBundle *>(
        heap_caps_calloc(1, sizeof(HCCPairCredentialBundle), capabilities));
    if (bundle == nullptr) {
        return nullptr;
    }

    bundle->generation = generation;
    HCCPairPemBuffer *buffers[] = {
        &bundle->root_private_key,
        &bundle->host_private_key,
        &bundle->root_certificate,
        &bundle->host_certificate,
        &bundle->device_certificate,
    };
    for (HCCPairPemBuffer *buffer : buffers) {
        buffer->data = static_cast<uint8_t *>(heap_caps_calloc(1, kPemBufferSize, capabilities));
        if (buffer->data == nullptr) {
            destroyBundle(bundle);
            return nullptr;
        }
        buffer->capacity = kPemBufferSize;
    }
    return bundle;
}

void HCCPairCredentials::destroyBundle(HCCPairCredentialBundle *bundle)
{
    if (bundle == nullptr) {
        return;
    }
    HCCPairPemBuffer *buffers[] = {
        &bundle->root_private_key,
        &bundle->host_private_key,
        &bundle->root_certificate,
        &bundle->host_certificate,
        &bundle->device_certificate,
    };
    for (HCCPairPemBuffer *buffer : buffers) {
        if (buffer->data != nullptr) {
            mbedtls_platform_zeroize(buffer->data, buffer->capacity);
            heap_caps_free(buffer->data);
            buffer->data = nullptr;
        }
        buffer->length = 0;
        buffer->capacity = 0;
    }
    mbedtls_platform_zeroize(bundle, sizeof(*bundle));
    heap_caps_free(bundle);
}

bool HCCPairCredentials::validateBundle(const HCCPairCredentialBundle *bundle)
{
    if (bundle == nullptr || bundle->generation == 0) {
        return false;
    }
    const HCCPairPemBuffer *buffers[] = {
        &bundle->root_private_key,
        &bundle->host_private_key,
        &bundle->root_certificate,
        &bundle->host_certificate,
        &bundle->device_certificate,
    };
    for (const HCCPairPemBuffer *buffer : buffers) {
        if (buffer->data == nullptr || buffer->length == 0 ||
            buffer->length >= buffer->capacity || buffer->data[buffer->length] != '\0') {
            return false;
        }
    }
    return true;
}

void HCCPairCredentials::discardQueuedBundles()
{
    if (_completed_queue == nullptr) {
        return;
    }
    HCCPairCredentialBundle *bundle = nullptr;
    while (xQueueReceive(_completed_queue, &bundle, 0) == pdTRUE) {
        destroyBundle(bundle);
        bundle = nullptr;
    }
}

void HCCPairCredentials::setActiveGeneration(uint32_t generation)
{
    _active_generation.store(generation, std::memory_order_release);
    discardQueuedBundles();
}

bool HCCPairCredentials::hasDeferredWork() const
{
    return _worker_active.load(std::memory_order_acquire) ||
           (_completed_queue != nullptr && uxQueueMessagesWaiting(_completed_queue) != 0);
}

HCCPairCredentialBundle *HCCPairCredentials::takeCompletedBundle(uint32_t generation)
{
    if (_completed_queue == nullptr) {
        return nullptr;
    }
    HCCPairCredentialBundle *bundle = nullptr;
    if (xQueueReceive(_completed_queue, &bundle, 0) != pdTRUE) {
        return nullptr;
    }
    if (bundle == nullptr || bundle->generation != generation ||
        generation != _active_generation.load(std::memory_order_acquire)) {
        destroyBundle(bundle);
        return nullptr;
    }
    return bundle;
}

void HCCPairCredentials::publishBundle(HCCPairCredentialBundle *bundle)
{
    if (bundle == nullptr) {
        return;
    }
    if (bundle->generation != _active_generation.load(std::memory_order_acquire) ||
        _completed_queue == nullptr || xQueueSend(_completed_queue, &bundle, 0) != pdTRUE) {
        destroyBundle(bundle);
    }
}

bool HCCPairCredentials::queueDevicePublicKey(const uint8_t *data, size_t length, uint32_t generation)
{
    if (data == nullptr || length == 0 || length > _device_public_key.size() || generation == 0 ||
        generation != _active_generation.load(std::memory_order_acquire)) {
        printf("[HCC PAIR CRYPTO] DevicePublicKey handoff rejected length=%u\n",
               static_cast<unsigned>(length));
        return false;
    }
    if (_last_started_generation.load(std::memory_order_acquire) == generation) {
        printf("[HCC PAIR CRYPTO] DevicePublicKey handoff ignored: worker already used\n");
        return true;
    }
    bool expected = false;
    if (!_worker_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    memcpy(_device_public_key.data(), data, length);
    _device_public_key_length = length;
    _worker_generation = generation;
    _last_started_generation.store(generation, std::memory_order_release);
    logCryptoCreationHeap("BEFORE CREATE");
    if (xTaskCreatePinnedToCore(HCCPairCredentials::workerTask, "hcc_pair_crypto", kWorkerStackBytes,
                    this, kWorkerPriority, nullptr, HCC_CORE_WORK) != pdPASS) {
        logCryptoCreationHeap("CREATE FAIL");
        printf("[HCC PAIR CRYPTO] worker task creation failed\n");
        mbedtls_platform_zeroize(_device_public_key.data(), _device_public_key.size());
        _device_public_key_length = 0;
        _worker_active.store(false, std::memory_order_release);
        _last_started_generation.store(0, std::memory_order_release);
        return false;
    }
    printf("[HCC HEAP] CRYPTO CREATE PASS\n");
    return true;
}

void HCCPairCredentials::workerTask(void *arg)
{
    logWorkerStack("worker-start");
    HCCPairCredentials *credentials = static_cast<HCCPairCredentials *>(arg);
    if (credentials != nullptr) {
        credentials->runWorker();
    }
    logWorkerStack("before-task-delete");
    vTaskDelete(nullptr);
}

void HCCPairCredentials::runWorker()
{
    printf("[HCC PAIR CRYPTO] worker start\n");
    logHeap("start");

    const uint32_t generation = _worker_generation;
    HCCPairCredentialBundle *bundle = allocateBundle(generation);
    char not_before[16] = {};
    char not_after[16] = {};
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context root_key;
    mbedtls_pk_context host_key;
    mbedtls_pk_context device_key;
    mbedtls_x509_crt root_cert;
    mbedtls_x509_crt host_cert;
    mbedtls_x509_crt device_cert;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&root_key);
    mbedtls_pk_init(&host_key);
    mbedtls_pk_init(&device_key);
    mbedtls_x509_crt_init(&root_cert);
    mbedtls_x509_crt_init(&host_cert);
    mbedtls_x509_crt_init(&device_cert);

    bool success = false;
    int ret = 0;
    size_t pem_size = 0;
    uint32_t verify_flags = 0;
    std::array<uint8_t, kMaxDevicePublicKeySize + 1> device_public_key_with_nul{};
    if (bundle == nullptr) {
        printf("[HCC PAIR CRYPTO] credential bundle allocation failed\n");
        goto cleanup;
    }
    if (!validUtcNow(not_before, sizeof(not_before), not_after, sizeof(not_after))) {
        printf("[HCC PAIR CRYPTO] TIME NOT VALID\n");
        goto cleanup;
    }
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                reinterpret_cast<const unsigned char *>(kDrbgPersonalization),
                                sizeof(kDrbgPersonalization) - 1);
    if (ret != 0) {
        logMbedtlsFailure("CTR_DRBG seed", ret);
        goto cleanup;
    }
    memcpy(device_public_key_with_nul.data(), _device_public_key.data(), _device_public_key_length);
    ret = mbedtls_pk_parse_public_key(&device_key, device_public_key_with_nul.data(),
                                      _device_public_key_length + 1);
    mbedtls_platform_zeroize(device_public_key_with_nul.data(), device_public_key_with_nul.size());
    if (ret != 0) {
        logMbedtlsFailure("DevicePublicKey parse", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] DevicePublicKey parsed type=%d bits=%u\n",
           static_cast<int>(mbedtls_pk_get_type(&device_key)),
           static_cast<unsigned>(mbedtls_pk_get_bitlen(&device_key)));
    logHeap("device-key-parsed");
    logWorkerStack("device-key-parsed");

    ret = mbedtls_pk_setup(&root_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret == 0) {
        ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(root_key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
    }
    if (ret != 0 || mbedtls_pk_get_bitlen(&root_key) != 2048) {
        logMbedtlsFailure("Root RSA generation", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] Root RSA generated bits=2048\n");
    logHeap("root-rsa");
    logWorkerStack("root-rsa");

    ret = mbedtls_pk_setup(&host_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret == 0) {
        ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(host_key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
    }
    if (ret != 0 || mbedtls_pk_get_bitlen(&host_key) != 2048 || publicKeysEqual(&root_key, &host_key)) {
        if (ret != 0) {
            logMbedtlsFailure("Host RSA generation", ret);
        } else {
            printf("[HCC PAIR CRYPTO] Host RSA generation failed: key relationship\n");
        }
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] Host RSA generated bits=2048\n");
    logHeap("host-rsa");
    logWorkerStack("host-rsa");

    ret = mbedtls_pk_write_key_pem(&root_key, bundle->root_private_key.data,
                                   bundle->root_private_key.capacity);
    if (ret != 0) {
        logMbedtlsFailure("RootPrivateKey PEM", ret);
        goto cleanup;
    }
    bundle->root_private_key.length = strnlen(
        reinterpret_cast<const char *>(bundle->root_private_key.data),
        bundle->root_private_key.capacity);
    if (bundle->root_private_key.length == bundle->root_private_key.capacity) {
        printf("[HCC PAIR CRYPTO] RootPrivateKey PEM not terminated\n");
        goto cleanup;
    }
    ret = mbedtls_pk_write_key_pem(&host_key, bundle->host_private_key.data,
                                   bundle->host_private_key.capacity);
    if (ret != 0) {
        logMbedtlsFailure("HostPrivateKey PEM", ret);
        goto cleanup;
    }
    bundle->host_private_key.length = strnlen(
        reinterpret_cast<const char *>(bundle->host_private_key.data),
        bundle->host_private_key.capacity);
    if (bundle->host_private_key.length == bundle->host_private_key.capacity) {
        printf("[HCC PAIR CRYPTO] HostPrivateKey PEM not terminated\n");
        goto cleanup;
    }

    logRootCertificateHeap("PRE-ROOTCERT");
    {
        void *probe = heap_caps_malloc(128, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const bool probe_succeeded = probe != nullptr;
        heap_caps_free(probe);
        printf("[HCC PAIR CRYPTO] DMA128 probe=%s\n", probe_succeeded ? "PASS" : "FAIL");
    }
    if (!writeCertificatePem(&root_key, &root_key, kRootCertificateName, kRootCertificateName,
                             not_before, not_after, true, false,
                             &drbg, bundle->root_certificate.data,
                             bundle->root_certificate.capacity, &pem_size, "RootCertificate")) {
        logRootCertificateHeap("ROOTCERT-FAIL");
        goto cleanup;
    }
    bundle->root_certificate.length = pem_size;
    logRootCertificateHeap("POST-ROOTCERT");
    ret = mbedtls_x509_crt_parse(&root_cert, bundle->root_certificate.data, pem_size + 1);
    if (ret != 0) {
        logMbedtlsFailure("RootCertificate parse", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] RootCertificate generated pem_bytes=%u\n", static_cast<unsigned>(pem_size));
    logHeap("root-cert");

    if (!writeCertificatePem(&host_key, &root_key, kHostCertificateName, kRootCertificateName,
                             not_before, not_after, false, true,
                             &drbg, bundle->host_certificate.data,
                             bundle->host_certificate.capacity, &pem_size, "HostCertificate")) {
        goto cleanup;
    }
    bundle->host_certificate.length = pem_size;
    ret = mbedtls_x509_crt_parse(&host_cert, bundle->host_certificate.data, pem_size + 1);
    if (ret != 0) {
        logMbedtlsFailure("HostCertificate parse", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] HostCertificate generated pem_bytes=%u\n", static_cast<unsigned>(pem_size));
    logHeap("host-cert");

    if (!writeCertificatePem(&device_key, &root_key, kDeviceCertificateName, kRootCertificateName,
                             not_before, not_after, false, true,
                             &drbg, bundle->device_certificate.data,
                             bundle->device_certificate.capacity, &pem_size, "DeviceCertificate")) {
        goto cleanup;
    }
    bundle->device_certificate.length = pem_size;
    ret = mbedtls_x509_crt_parse(&device_cert, bundle->device_certificate.data, pem_size + 1);
    if (ret != 0) {
        logMbedtlsFailure("DeviceCertificate parse", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] DeviceCertificate generated pem_bytes=%u\n", static_cast<unsigned>(pem_size));
    logHeap("device-cert");

    ret = mbedtls_x509_crt_verify(&root_cert, &root_cert, nullptr, nullptr, &verify_flags, nullptr, nullptr);
    if (ret != 0 || verify_flags != 0) {
        logMbedtlsFailure("Root self-verify", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] Root self-verify PASS\n");
    ret = mbedtls_x509_crt_verify(&host_cert, &root_cert, nullptr, nullptr, &verify_flags, nullptr, nullptr);
    if (ret != 0 || verify_flags != 0) {
        logMbedtlsFailure("Host verify Root", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] Host verify Root PASS\n");
    ret = mbedtls_x509_crt_verify(&device_cert, &root_cert, nullptr, nullptr, &verify_flags, nullptr, nullptr);
    if (ret != 0 || verify_flags != 0) {
        logMbedtlsFailure("Device verify Root", ret);
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] Device verify Root PASS\n");

    ret = mbedtls_pk_check_pair(&root_cert.pk, &root_key, mbedtls_ctr_drbg_random, &drbg);
    if (ret == 0) {
        ret = mbedtls_pk_check_pair(&host_cert.pk, &host_key, mbedtls_ctr_drbg_random, &drbg);
    }
    if (ret != 0 || !publicKeysEqual(&device_cert.pk, &device_key)) {
        if (ret != 0) {
            logMbedtlsFailure("key relationships", ret);
        } else {
            printf("[HCC PAIR CRYPTO] key relationships failed: device public key mismatch\n");
        }
        goto cleanup;
    }
    printf("[HCC PAIR CRYPTO] key relationships PASS\n");
    logWorkerStack("certificates-validated");
    printf("[HCC PAIR CRYPTO] OFFLINE CREDENTIALS PASS\n");
    success = validateBundle(bundle);
    if (!success) {
        printf("[HCC PAIR CRYPTO] credential bundle validation failed\n");
    }

cleanup:
    mbedtls_platform_zeroize(device_public_key_with_nul.data(), device_public_key_with_nul.size());
    mbedtls_x509_crt_free(&device_cert);
    mbedtls_x509_crt_free(&host_cert);
    mbedtls_x509_crt_free(&root_cert);
    mbedtls_pk_free(&device_key);
    mbedtls_pk_free(&host_key);
    mbedtls_pk_free(&root_key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_platform_zeroize(_device_public_key.data(), _device_public_key.size());
    _device_public_key_length = 0;
    if (success) {
        publishBundle(bundle);
        bundle = nullptr;
    } else {
        destroyBundle(bundle);
        bundle = nullptr;
    }
    _worker_active.store(false, std::memory_order_release);
    printf("[HCC PAIR CRYPTO] temporary sensitive material destroyed\n");
}
