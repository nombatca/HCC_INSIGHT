#include "HCCSdFileSystem.hpp"
#include "HCCSdFiles.hpp"
#include "HCCSdAccess.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
namespace fs=std::filesystem;
namespace files=HCCSdFileSystem;
static std::string root;
void* bsp_sdcard=nullptr;
static bool mount_ok=true,task_ok=true;
static unsigned mounts=0;
const char* hcc_test_mount_point(){return root.c_str();}
int bsp_sdcard_mount(){++mounts;if(mount_ok)bsp_sdcard=reinterpret_cast<void*>(1);return mount_ok?0:-1;}
int esp_vfs_fat_info(const char*,std::uint64_t* total,std::uint64_t* free){*total=1000000;*free=500000;return 0;}
static void (*pending)(void*)=nullptr;
int xTaskCreatePinnedToCore(void (*worker)(void*),const char*,std::uint32_t stack,void*,unsigned,void*,int core){
    assert(stack==8192&&core==0);if(!task_ok)return 0;assert(!pending);pending=worker;return 1;
}
void vTaskDelete(void*){}
void vTaskDelay(std::uint32_t){
    assert(HCCSdAccess::gate.owner()==hcc::backup::SdOwner::file_manager);
    hcc::backup::SdLease capture(HCCSdAccess::gate,hcc::backup::SdOwner::capture);
    hcc::backup::SdLease usb(HCCSdAccess::gate,hcc::backup::SdOwner::usb_disk);
    assert(!capture.acquire()&&!usb.acquire());
}
static void run(){assert(pending);auto* task=pending;pending=nullptr;task(nullptr);assert(!HCCSdFiles::busy());assert(HCCSdAccess::gate.owner()==hcc::backup::SdOwner::free);}
struct Cancel { unsigned limit;std::uint32_t count=0; };
static bool cancelled(void* context){auto& c=*static_cast<Cancel*>(context);return c.count>=c.limit;}
static void progress(void* context,std::uint32_t count){static_cast<Cancel*>(context)->count=count;}
int main(){
    char temp[]="/tmp/hcc-file-manager-XXXXXX";assert(mkdtemp(temp));root=temp;
    const auto outside=fs::path(root)/"outside";fs::create_directories(outside);std::ofstream(outside/"keep")<<"keep";
    fs::create_directories(fs::path(root)/"page");
    for(unsigned i=0;i<150;++i)std::ofstream(fs::path(root)/"page"/std::to_string(i))<<"abc";
    files::Page page;std::set<std::string> names;
    for(std::size_t offset=0;offset<192;offset+=64){
        assert(files::list(root.c_str(),"page",offset,page)==files::Error::ok);
        assert(page.count==(offset<128?64:22)&&page.more==(offset<128));
        for(std::size_t i=0;i<page.count;++i){assert(page.entries[i].bytes==3&&page.entries[i].supported);names.insert(page.entries[i].name);}
    }
    assert(names.size()==150);
    std::uint32_t removed=0;
    for(const char* path:{"","../outside","/outside","page/../outside","page//a","page/.","page/a.","page/a ","page\\a","page:a"})
        assert(files::remove(root.c_str(),path,true,{},removed)==files::Error::invalid_path);
    fs::create_directory_symlink(outside,fs::path(root)/"link");
    assert(files::list(root.c_str(),"link",0,page)!=files::Error::ok);
    assert(files::remove(root.c_str(),"link/keep",false,{},removed)!=files::Error::ok);
    assert(files::remove(root.c_str(),"link",true,{},removed)!=files::Error::ok&&fs::exists(outside/"keep"));
    assert(files::remove(root.c_str(),"page",false,{},removed)==files::Error::unsupported);
    Cancel cancel{5};
    assert(files::remove(root.c_str(),"page",true,{&cancel,cancelled,progress},removed)==files::Error::cancelled&&removed==5);
    assert(fs::exists(fs::path(root)/"page"));
    fs::create_directories(fs::path(root)/"page/deep/empty");
    assert(files::remove(root.c_str(),"page",true,{},removed)==files::Error::ok&&removed==148&&!fs::exists(fs::path(root)/"page"));
    assert(fs::exists(outside/"keep"));
    std::ofstream(fs::path(root)/"kiểm tra.txt")<<"viet";
    assert(files::remove(root.c_str(),"kiểm tra.txt",false,{},removed)==files::Error::ok&&removed==1);
    fs::create_directories(fs::path(root)/"nested");fs::create_directory_symlink(outside,fs::path(root)/"nested/escape");
    assert(files::remove(root.c_str(),"nested",true,{},removed)==files::Error::unsupported&&fs::exists(outside/"keep"));
    fs::path deep=fs::path(root)/"depth";for(unsigned i=0;i<25;++i)deep/="d";fs::create_directories(deep);
    assert(files::remove(root.c_str(),"depth",true,{},removed)==files::Error::too_deep&&fs::exists(deep));
    Cancel stop_now{0};assert(files::list(root.c_str(),"",0,page,{&stop_now,cancelled,nullptr})==files::Error::cancelled&&page.count==0);
    std::puts("PASS: paginated listing, permanent tree deletion, root/traversal/FAT aliases/type/symlink checks and partial cancellation");
    // Async API: serialize commands, mount failure, shared SD ownership and
    // progress leave USB/capture blocked until worker completion.
    hcc::backup::SdLease capture(HCCSdAccess::gate,hcc::backup::SdOwner::capture);
    assert(capture.acquire());assert(HCCSdFiles::requestList(""));assert(HCCSdFiles::busy());assert(!HCCSdFiles::requestDelete("outside",true));
    auto* task=pending;pending=nullptr;task(nullptr);assert(!HCCSdFiles::result().success&&mounts==0);capture.release();
    mount_ok=false;assert(HCCSdFiles::requestList(""));run();assert(!HCCSdFiles::result().success);
    mount_ok=true;assert(HCCSdFiles::requestList(""));run();assert(HCCSdFiles::result().success&&HCCSdFiles::result().space_valid);
    fs::create_directories(fs::path(root)/"delete");for(unsigned i=0;i<40;++i)std::ofstream(fs::path(root)/"delete"/std::to_string(i))<<i;
    assert(HCCSdFiles::requestDelete("delete",true));run();assert(HCCSdFiles::result().success&&HCCSdFiles::result().removed==41);
    assert(HCCSdFiles::requestDelete("outside",true));HCCSdFiles::cancel();run();assert(!HCCSdFiles::result().success&&HCCSdFiles::result().removed==0&&fs::exists(outside/"keep"));
    task_ok=false;assert(HCCSdFiles::requestList(""));assert(!HCCSdFiles::busy()&&!HCCSdFiles::result().success);
    assert(HCCSdAccess::gate.owner()==hcc::backup::SdOwner::free);
    fs::remove_all(root);std::puts("PASS: async mount/ownership, concurrent request refusal, real delete progress, cancel and task-creation rollback");
}
