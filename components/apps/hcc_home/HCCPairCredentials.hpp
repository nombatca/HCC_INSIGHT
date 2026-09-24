#pragma once

#include <array>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct HCCPairPemBuffer {
    uint8_t *data = nullptr;
    size_t length = 0;
    size_t capacity = 0;
};

struct HCCPairCredentialBundle {
    uint32_t generation = 0;
    HCCPairPemBuffer root_private_key;
    HCCPairPemBuffer host_private_key;
    HCCPairPemBuffer root_certificate;
    HCCPairPemBuffer host_certificate;
    HCCPairPemBuffer device_certificate;
};

class HCCPairCredentials {
public:
    static constexpr size_t kMaxDevicePublicKeySize = 2048;

    HCCPairCredentials();

    // Called by the USB client task after Lockdown has fully validated the
    // DevicePublicKey response. This method copies the public data and starts
    // a separate worker; it never performs cryptographic generation itself.
    bool queueDevicePublicKey(const uint8_t *data, size_t length, uint32_t generation);

    void setActiveGeneration(uint32_t generation);
    bool hasDeferredWork() const;
    HCCPairCredentialBundle *takeCompletedBundle(uint32_t generation);
    static bool validateBundle(const HCCPairCredentialBundle *bundle);
    static void destroyBundle(HCCPairCredentialBundle *bundle);

private:
    static void workerTask(void *arg);
    void runWorker();
    static HCCPairCredentialBundle *allocateBundle(uint32_t generation);
    void publishBundle(HCCPairCredentialBundle *bundle);
    void discardQueuedBundles();

    std::array<uint8_t, kMaxDevicePublicKeySize> _device_public_key{};
    size_t _device_public_key_length = 0;
    uint32_t _worker_generation = 0;
    std::atomic<uint32_t> _active_generation{0};
    std::atomic<uint32_t> _last_started_generation{0};
    std::atomic<bool> _worker_active{false};
    StaticQueue_t _completed_queue_storage{};
    uint8_t _completed_queue_buffer[sizeof(HCCPairCredentialBundle *)]{};
    QueueHandle_t _completed_queue = nullptr;
};
