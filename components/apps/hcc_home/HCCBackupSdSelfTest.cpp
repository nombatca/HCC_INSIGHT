#include "HCCBackupSdSelfTest.hpp"
#include "HCCBackupCapture.hpp"
#include "hcc_cores.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
namespace HCCBackupSdSelfTest {
namespace {
std::atomic<Result> state{Result::idle};
bool matches(const char* path,const void* expected,std::size_t size){
    if(size>128)return false;
    FILE* f=fopen(path,"rb");if(!f)return false;
    unsigned char data[128];const auto n=fread(data,1,size,f);
    const bool ok=n==size&&std::memcmp(data,expected,size)==0&&fgetc(f)==EOF&&!ferror(f);
    const bool closed=fclose(f)==0;return ok&&closed;
}
bool path(char* out,std::size_t capacity,const char* root,const char* name){
    const int n=snprintf(out,capacity,"%s/%s",root,name);
    return n>0&&static_cast<std::size_t>(n)<capacity;
}
Result execute(){
    using hcc::backup::Error;
    printf("[HCC SDTEST] START version=1 bytes=64\n");
    HCCBackupCapture capture;const auto opened=capture.open(1048576);
    if(opened!=Error::ok)return opened==Error::busy?Result::busy:Result::storage;
    const auto target=capture.target();
    unsigned char data[64];for(unsigned i=0;i<sizeof(data);++i)data[i]=static_cast<unsigned char>(i^0xa5);
    constexpr char source[]="HCC_SD_SELFTEST_V1",destination[]="selftest/payload.bin";
    if(target.begin(target.context,source,destination)!=Error::ok)return Result::begin;
    auto written=target.write(target.context,data,sizeof(data));
    if(written.state!=hcc::backup::IoState::ok||written.bytes!=sizeof(data))return Result::write;
    if(target.finish(target.context,true)!=Error::ok)return Result::commit;
    char full[320];
    if(!path(full,sizeof(full),capture.sessionPath(),"000001.ready/payload.bin")||!matches(full,data,sizeof(data)))return Result::verify;
    unsigned char identity[128];std::size_t offset=0;
    std::memcpy(identity,"HCCU0001",8);offset=8;
    hcc::backup::write_be32(identity+offset,sizeof(source)-1);offset+=4;
    std::memcpy(identity+offset,source,sizeof(source)-1);offset+=sizeof(source)-1;
    hcc::backup::write_be32(identity+offset,sizeof(destination)-1);offset+=4;
    std::memcpy(identity+offset,destination,sizeof(destination)-1);offset+=sizeof(destination)-1;
    if(!path(full,sizeof(full),capture.sessionPath(),"000001.ready/identity.bin")||!matches(full,identity,offset))return Result::verify;
    if(target.begin(target.context,source,"selftest/abort.bin")!=Error::ok)return Result::begin;
    written=target.write(target.context,data,sizeof(data));
    if(written.state!=hcc::backup::IoState::ok||written.bytes!=sizeof(data))return Result::write;
    if(target.finish(target.context,false)!=Error::ok)return Result::abort_cleanup;
    if(!path(full,sizeof(full),capture.sessionPath(),"000002.part"))return Result::abort_cleanup;
    struct stat info{};
    if(stat(full,&info)==0||errno!=ENOENT||capture.committedFiles()!=1)return Result::abort_cleanup;
    if(!path(full,sizeof(full),capture.sessionPath(),"000001.ready/payload.bin")||!matches(full,data,sizeof(data)))return Result::abort_cleanup;
    printf("[HCC SDTEST] PASS bytes=64 commit=1 abort=1 session=%s\n",capture.sessionPath());
    return Result::passed;
}
const char* stepName(Result r){
    switch(r){
    case Result::idle:return "idle";case Result::running:return "running";case Result::passed:return "passed";
    case Result::busy:return "busy";case Result::storage:return "storage";case Result::begin:return "begin";
    case Result::write:return "write";case Result::commit:return "commit";case Result::verify:return "verify";
    case Result::abort_cleanup:return "abort_cleanup";case Result::task:return "task";
    }
    return "unknown";
}
void worker(void*){
    const auto done=execute(); // capture/lease destruction completes before publication.
    if(done!=Result::passed)printf("[HCC SDTEST] FAIL step=%s\n",stepName(done));
    state.store(done);vTaskDelete(nullptr);
}
}
Result result(){return state.load();}
bool request(){
    auto previous=state.load();
    while(previous!=Result::running){
        if(state.compare_exchange_weak(previous,Result::running)){
            if(xTaskCreatePinnedToCore(worker,"hcc_sd_selftest",8192,nullptr,4,nullptr,HCC_CORE_WORK)==pdPASS)return true;
            state.store(Result::task);printf("[HCC SDTEST] FAIL step=task\n");return false;
        }
    }
    return false;
}
bool clear(){
    auto previous=state.load();
    while(previous!=Result::running){if(state.compare_exchange_weak(previous,Result::idle))return true;}
    return false;
}
const char* statusText(){
    switch(state.load()){
    case Result::idle:return "SD: chưa kiểm tra";
    case Result::running:return "SD: ĐANG KIỂM TRA";
    case Result::passed:return "SD: PASS - ghi/chốt/đọc/dọn OK";
    case Result::busy:return "SD: BẬN - tắt USB DISK rồi thử lại";
    case Result::storage:return "SD: lỗi mount/dung lượng - xem log";
    case Result::begin:return "SD: không tạo được file - xem log";
    case Result::write:return "SD: lỗi ghi - xem log";
    case Result::commit:return "SD: lỗi chốt file - xem log";
    case Result::verify:return "SD: đọc lại không khớp - xem log";
    case Result::abort_cleanup:return "SD: lỗi dọn file tạm - xem log";
    case Result::task:return "SD: không tạo được task";
    }
    return "SD: chưa xác định";
}
}
