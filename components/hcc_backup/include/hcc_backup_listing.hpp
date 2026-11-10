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

// Caller-owned working buffers for list_directory, kept off the caller's stack
// (the USB task has only a few KiB of headroom). One instance may be reused
// across calls. Sized for the 4096-byte relative-path limit plus the session
// root prefix and a 255-byte component.
struct MoveIndexEntry { std::uint32_t offset=0,length=0,record=0; };
struct ListingScratch {
    // Caller-owned PSRAM arena. No filesystem scan is repeated for each move.
    MoveIndexEntry move_entries[1024]{};
    char move_paths[262144]{};
    char move_owner[256]{};
    std::uint32_t move_count=0,move_used=0,move_revision=0,move_builds=0;
    bool move_ready=false;
    bool remove_active=false;
    std::uint32_t remove_done=0;
    char remove_path[4096]{};
    char prefix[4096];   // normalized requested path
    char path[4096];     // a record's destination, normalized in place
    char nsdir[4400];    // <root>/directories/<prefix>
    char sub[4700];      // a namespace child's full path
    char record[320];    // <root>/NNNNNN.ready
};

// Number of binary-plist objects the encoder emits for `entries` children:
// the status array, its three leading items, the directory dict, three shared
// key strings, two shared file-type strings, then four objects per entry
// (name key, value sub-dict, size integer, modification date).
constexpr std::size_t contents_object_count(std::size_t entries) {
    return 10 + 4 * entries;
}

} // namespace hcc::backup
