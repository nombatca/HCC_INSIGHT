#include "HCCBackupCapture.hpp"
#include "HCCUsbDisk.hpp"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unistd.h>
namespace bb=hcc::backup;
namespace fs=std::filesystem;
void* bsp_sdcard=nullptr;
static int mount_result=0,mount_calls=0;
static const char* mount_point;
int bsp_sdcard_mount(){++mount_calls;if(!mount_result)bsp_sdcard=reinterpret_cast<void*>(1);return mount_result;}
const char* hcc_test_mount_point(){return mount_point;}
namespace HCCUsbDisk {
static std::atomic<State> s_state{State::Off};
static bb::SdLease s_sd_lease(HCCSdAccess::gate,bb::SdOwner::usb_disk);
static bool s_sd_returned=false,launch_ok=true,start_ok=true,remount_ok=true,requested_start=false;
static char s_error[96];
static bool doStart(){return start_ok;}
static bool teardown(){s_sd_returned=remount_ok;return remount_ok;}
static void vTaskDelete(void*){}
#define ESP_LOGI(...) ((void)0)
#include "HCCUsbDiskWorker.inc"
#undef ESP_LOGI
static bool launch(bool start){requested_start=start;return launch_ok;}
#include "HCCUsbDiskRequests.inc"
static void run(){worker(requested_start?reinterpret_cast<void*>(1):nullptr);}
}
int main(){
    {
        bb::SdAccess gate;std::atomic<int> inside{0};std::atomic<unsigned> acquired{0};
        auto contend=[&](bb::SdOwner owner){bb::SdLease lease(gate,owner);for(unsigned i=0;i<20000;++i){if(lease.acquire()){assert(inside.fetch_add(1)==0);++acquired;assert(!lease.acquire());assert(inside.fetch_sub(1)==1);lease.release();lease.release();}}};
        std::thread a(contend,bb::SdOwner::capture),b(contend,bb::SdOwner::usb_disk);a.join();b.join();
        assert(acquired>0&&gate.owner()==bb::SdOwner::free);
    }
    std::puts("PASS: capture/USB ownership contention never overlaps");
    char temp[]="/tmp/hcc-sd-access-XXXXXX";assert(mkdtemp(temp));mount_point=temp;
    {
        HCCBackupCapture capture;assert(capture.open()==bb::Error::ok);assert(mount_calls==1);
        assert(!HCCUsbDisk::requestStart());assert(HCCUsbDisk::s_state==HCCUsbDisk::State::Off);
        assert(capture.open()==bb::Error::busy);
        auto target=capture.target();assert(target.begin(target.context,"source","backup/a")==bb::Error::ok);
        assert(fs::exists(fs::path(capture.sessionPath())/"000001.part"));
    }
    assert(HCCSdAccess::gate.owner()==bb::SdOwner::free);
    assert(fs::is_empty(fs::path(temp)/"hcc_backup/rx_000001"));
    std::puts("PASS: mounted capture blocks USB start and cleans staging before releasing ownership");
    using State=HCCUsbDisk::State;
    {
        assert(HCCUsbDisk::requestStart());assert(!HCCUsbDisk::requestStart());
        HCCBackupCapture capture;assert(capture.open()==bb::Error::busy);
        HCCUsbDisk::run();assert(HCCUsbDisk::s_state==State::Ready);
        HCCUsbDisk::launch_ok=false;assert(!HCCUsbDisk::requestStop());assert(capture.open()==bb::Error::busy);
        HCCUsbDisk::launch_ok=true;assert(HCCUsbDisk::requestStop());
        HCCUsbDisk::remount_ok=false;HCCUsbDisk::run();assert(HCCUsbDisk::s_state==State::Error);
        assert(capture.open()==bb::Error::busy);assert(!HCCUsbDisk::requestStart());
        HCCUsbDisk::remount_ok=true;assert(HCCUsbDisk::requestStop());HCCUsbDisk::run();
        assert(HCCUsbDisk::s_state==State::Off);assert(capture.open()==bb::Error::ok);
    }
    std::puts("PASS: USB start/stop, task launch failure and failed remount retain correct ownership");
    {
        HCCUsbDisk::launch_ok=false;assert(!HCCUsbDisk::requestStart());assert(HCCSdAccess::gate.owner()==bb::SdOwner::free);
        HCCUsbDisk::launch_ok=true;HCCUsbDisk::start_ok=false;HCCUsbDisk::s_sd_returned=true;
        assert(HCCUsbDisk::requestStart());HCCUsbDisk::run();assert(HCCSdAccess::gate.owner()==bb::SdOwner::free);
        HCCUsbDisk::s_sd_returned=false;assert(HCCUsbDisk::requestStart());HCCUsbDisk::run();assert(HCCSdAccess::gate.owner()==bb::SdOwner::usb_disk);
        assert(HCCUsbDisk::requestStop());HCCUsbDisk::run();
    }
    {
        bsp_sdcard=nullptr;mount_result=2;HCCBackupCapture failed;assert(failed.open()==bb::Error::io_error);assert(HCCSdAccess::gate.owner()==bb::SdOwner::free);
        mount_result=0;assert(failed.open()==bb::Error::ok);
        HCCUsbDisk::s_state=State::Error;assert(!HCCUsbDisk::requestStop());
    }
    assert(HCCSdAccess::gate.owner()==bb::SdOwner::free);
    fs::remove_all(temp);
    std::puts("PASS: startup rollback and capture mount failure release only safe ownership");
}
