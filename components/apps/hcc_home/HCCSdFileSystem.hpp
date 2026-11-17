#pragma once
#include <cstddef>
#include <cstdint>
namespace HCCSdFileSystem {
constexpr std::size_t page_size=64, path_size=512;
enum class Error { ok, invalid_path, io, unsupported, cancelled, too_deep };
struct Entry { char name[256]{}; std::uint64_t bytes=0; bool directory=false,supported=false; };
struct Page { Entry entries[page_size]{}; std::size_t count=0; bool more=false; };
struct Hooks { void* context=nullptr; bool (*cancel)(void*)=nullptr; void (*progress)(void*,std::uint32_t)=nullptr; };
// Trusted mounted root and exclusive caller ownership. All requests are relative;
// root browsing is allowed, root deletion is not. No host symlinks are followed.
Error list(const char* root,const char* relative,std::size_t offset,Page& page,Hooks hooks={});
// Permanent, incremental DFS with bounded depth/path. On cancellation/error,
// already removed entries stay removed and the exact count is returned.
Error remove(const char* root,const char* relative,bool expected_directory,Hooks hooks,std::uint32_t& removed);
}
