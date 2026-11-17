#include "HCCSdFiles.hpp"
#include "HCCSdAccess.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
void* bsp_sdcard=nullptr;
static unsigned mounts=0;
const char* hcc_test_mount_point(){return "/unused";}
int bsp_sdcard_mount(){++mounts;return 0;}
int esp_vfs_fat_info(const char*,std::uint64_t*,std::uint64_t*){return -1;}
static void (*pending)(void*)=nullptr;
int xTaskCreatePinnedToCore(void(*worker)(void*),const char*,std::uint32_t,void*,unsigned,void*,int){pending=worker;return 1;}
void vTaskDelete(void*){}
void vTaskDelay(std::uint32_t){}
int main(){
    assert(HCCSdFiles::requestList(""));assert(pending);pending(nullptr);
    assert(!HCCSdFiles::result().success&&mounts==0);
    assert(HCCSdAccess::gate.owner()==hcc::backup::SdOwner::free);
    std::puts("PASS: file manager refuses mount when automatic formatting is configured");
}
