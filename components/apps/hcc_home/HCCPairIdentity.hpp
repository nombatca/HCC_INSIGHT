#pragma once

#include <array>
#include <stddef.h>

class HCCPairIdentity {
public:
    bool initialize();

    const char *systemBuid() const;
    const char *hostId() const;

private:
    static constexpr size_t kUuidStringLength = 36;
    static constexpr size_t kUuidStorageSize = kUuidStringLength + 1;

    static bool isValidUuid(const char *value);
    static void generateUuid(char *value);

    std::array<char, kUuidStorageSize> _system_buid{};
    std::array<char, kUuidStorageSize> _host_id{};
    bool _initialized = false;
};
