#include "HCCBackupCapture.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
void* bsp_sdcard=nullptr;
static const char* mount_point;
int bsp_sdcard_mount(){std::abort();} // Guarded configuration must never call this.
const char* hcc_test_mount_point(){return mount_point;}
int esp_vfs_fat_info(const char*,std::uint64_t* total,std::uint64_t* free){*total=*free=1048576;return 0;}
int main(){
    char temp[]="/tmp/hcc-noformat-XXXXXX";assert(mkdtemp(temp));mount_point=temp;
    {
        HCCBackupCapture capture;
        assert(capture.open()==hcc::backup::Error::invalid_argument);
        assert(HCCSdAccess::gate.owner()==hcc::backup::SdOwner::free);
    }
    bsp_sdcard=reinterpret_cast<void*>(1);
    {HCCBackupCapture mounted;assert(mounted.open(1)==hcc::backup::Error::ok);}
    assert(HCCSdAccess::gate.owner()==hcc::backup::SdOwner::free);
    std::filesystem::remove_all(temp);
}
