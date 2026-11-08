#pragma once
#include <cstddef>
#include <cstdint>

// DLContentsOfDirectory listing model.
//
// A capture stores uploaded files as indexed `<root>/NNNNNN.ready` records whose
// destination path lives only in identity.bin, plus a `<root>/directories/`
// namespace of explicitly created directories. There is no materialized backup
// tree. A directory listing therefore reconstructs the immediate children of a
// requested logical path by combining both sources. Entries only ever expose the
// reconstructed logical names (destinations / created directory names); internal
// capture names (NNNNNN.ready, payload.bin, identity.bin, .part) are never shown.
namespace hcc::backup {

enum class EntryType : std::uint8_t { directory, regular };

// One immediate child of the requested directory. `name` is a single path
// component (basename), NUL-terminated, 1..255 bytes. `size` is the payload size
// for files and 0 for directories. `modification_time` is Unix epoch seconds;
// the encoder converts it to the Mac/CoreFoundation epoch used by DeviceLink.
struct DirectoryEntry {
    char name[256];
    std::uint16_t name_length;
    EntryType type;
    std::uint64_t size;
    std::int64_t modification_time;
};

// Caller-owned, fixed-capacity collection. `entries` points at `capacity`
// elements; `count` is how many are populated. The producer never silently
// truncates: exceeding `capacity` is reported as an error instead.
struct DirectoryListing {
    DirectoryEntry* entries;
    std::size_t capacity;
    std::size_t count;
};

// Number of binary-plist objects the encoder emits for `entries` children:
// the status array, its three leading items, the directory dict, three shared
// key strings, two shared file-type strings, then four objects per entry
// (name key, value sub-dict, size integer, modification date).
constexpr std::size_t contents_object_count(std::size_t entries) {
    return 10 + 4 * entries;
}

} // namespace hcc::backup
