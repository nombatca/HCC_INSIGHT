#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hcc::plist {
constexpr std::uint32_t none = UINT32_MAX;
enum class Kind { null_value, boolean, integer, real, string, data, array, dict };
enum class Error { ok, invalid_argument, malformed, unsupported, too_large,
                   too_deep, no_space, cycle, duplicate_key, invalid_text };
struct Node {
    Kind kind = Kind::null_value;
    std::uint32_t first = none, next = none, count = 0, offset = 0, length = 0;
    std::int64_t integer = 0;
    double real = 0;
    bool boolean = false;
};
struct Workspace {
    Node* nodes; std::size_t node_capacity;
    char* text; std::size_t text_capacity;
};
struct Limits {
    std::size_t max_input = 262144;
    std::uint32_t max_depth = 32;
    std::uint64_t max_binary_objects = 65536;
};
// Borrowed view; lifetime ends when input workspace is reused. Node/text memory
// is supplied by the caller. Input bytes need not remain after parse returns.
struct Document {
    const Node* nodes = nullptr; std::size_t size = 0;
    const char* text = nullptr; std::size_t text_size = 0;
    std::uint32_t root = none;
    const Node* get(std::uint32_t id) const;
    std::string_view string(std::uint32_t id) const;
    std::uint32_t at(std::uint32_t id, std::size_t index) const;
    std::uint32_t find(std::uint32_t id, std::string_view key) const;
};
Error parse(const std::uint8_t*, std::size_t, Workspace, Document&, Limits = {});
const char* error_name(Error);
} // namespace hcc::plist
