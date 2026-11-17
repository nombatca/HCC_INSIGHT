#pragma once
#include "HCCSdFileSystem.hpp"
#include <cstdint>
namespace HCCSdFiles {
enum class State { idle, busy, done };
struct Result {
    HCCSdFileSystem::Page page;
    char message[192]{};
    std::uint64_t total=0,free=0;
    bool space_valid=false,deletion=false,success=false;
    std::uint32_t removed=0;
};
State state();
bool busy();
std::uint32_t progress();
void cancel();
// Result may only be read while !busy(), until the next accepted request.
const Result& result();
bool requestList(const char* relative,std::size_t offset=0);
bool requestDelete(const char* relative,bool directory);
}
