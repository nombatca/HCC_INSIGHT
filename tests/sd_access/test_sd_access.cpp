#include "HCCBackupCapture.hpp"
#include "HCCBackupUploadRequest.hpp"
#include "hcc_backup_start.hpp"
#include "HCCBackupOperation.hpp"
#include "HCCBackupSdSelfTest.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
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
static int space_result=0;
static std::uint64_t space_available=1073741824;
int esp_vfs_fat_info(const char*,std::uint64_t* total,std::uint64_t* available){*total=1073741824;*available=space_available;return space_result;}
static const char* mount_point;
int bsp_sdcard_mount(){++mount_calls;if(!mount_result)bsp_sdcard=reinterpret_cast<void*>(1);return mount_result;}
const char* hcc_test_mount_point(){return mount_point;}
static void (*test_worker)(void*)=nullptr;
static void* test_argument=nullptr;
static bool test_task_fails=false;
int xTaskCreatePinnedToCore(void(*entry)(void*),const char*,std::uint32_t stack,void* argument,unsigned,void*,int core){
    assert(stack==8192&&core==0);
    if(test_task_fails)return 0;
    test_worker=entry;test_argument=argument;return 1;
}
void vTaskDelete(void*){}
static void run_selftest_worker(){assert(test_worker);auto* worker=test_worker;test_worker=nullptr;worker(test_argument);}
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
struct RequestWire {
    std::vector<std::uint8_t> input,output;std::size_t offset=0,calls=0;
    const void* retry_read=nullptr;const void* retry_write=nullptr;std::size_t read_n=0,write_n=0;
    bool close_write=false;
    void number(std::uint32_t n){std::uint8_t b[4];bb::write_be32(b,n);input.insert(input.end(),b,b+4);}
    void bytes(std::string_view s){input.insert(input.end(),s.begin(),s.end());}
    void name(std::string_view s){number(s.size());bytes(s);}
    void xml(std::string_view body){std::string s="<plist version=\"1.0\">";s+=body;s+="</plist>";number(s.size());bytes(s);}
    void fixture(const char* name){std::ifstream f(std::string("../backup_link/tests/fixtures/")+name,std::ios::binary);assert(f);std::vector<std::uint8_t> b{std::istreambuf_iterator<char>(f),{}};number(b.size());input.insert(input.end(),b.begin(),b.end());}
    static bb::IoResult read(void* ctx,std::uint8_t* out,std::size_t n){
        auto& w=*static_cast<RequestWire*>(ctx);++w.calls;
        if(!w.retry_read){w.retry_read=out;w.read_n=n;return {0,bb::IoState::would_block};}
        assert(w.retry_read==out&&w.read_n==n);
        const auto take=std::min({n,std::size_t(5),w.input.size()-w.offset});
        if(!take)return {0,bb::IoState::would_block};
        std::memcpy(out,w.input.data()+w.offset,take);w.offset+=take;w.retry_read=nullptr;return {take,bb::IoState::ok};
    }
    static bb::IoResult write(void* ctx,const std::uint8_t* data,std::size_t n){
        auto& w=*static_cast<RequestWire*>(ctx);++w.calls;if(w.close_write)return {0,bb::IoState::closed};
        if(!w.retry_write){w.retry_write=data;w.write_n=n;return {0,bb::IoState::would_block};}
        assert(w.retry_write==data&&w.write_n==n);w.retry_write=nullptr;
        const auto take=std::min(n,std::size_t(7));w.output.insert(w.output.end(),data,data+take);return {take,bb::IoState::ok};
    }
    bb::Duplex io(){return {this,read,write};}
};
static void upload_request_flow(unsigned failure){
    RequestWire wire;wire.fixture("version.bplist");wire.fixture("ready.xml");wire.fixture("hello.bplist");
    wire.xml("<array><string>DLMessageUploadFiles</string><integer>0</integer><integer>0</integer><integer>5</integer></array>");
    wire.name("/device/source");wire.name("backup/test");wire.number(6);wire.input.push_back(0x0c);wire.bytes("hello");
    wire.number(1);wire.input.push_back(failure==1?0x0b:0);wire.number(0);
    const auto raw_end=wire.input.size();wire.fixture("result_ok.xml");
    std::uint8_t control[8192],output[1024],scratch[3];hcc::plist::Node nodes[128];char text[8192];
    bb::BackupLink link({control,sizeof(control),output,sizeof(output),{nodes,128,text,sizeof(text)}});
    assert(link.start(0)==bb::LinkError::ok);std::uint64_t clock=0;
    for(;clock<5000&&link.phase()!=bb::LinkPhase::ready;++clock){const auto e=link.poll(wire.io(),clock,2);assert(e==bb::LinkError::ok||e==bb::LinkError::would_block||e==bb::LinkError::yield);}
    assert(link.phase()==bb::LinkPhase::ready);auto& observer=*link.session_observer();
    {
        HCCBackupUploadRequest invalid(observer,scratch,sizeof(scratch),output,sizeof(output));
        assert(invalid.start()==bb::Error::invalid_argument);
        assert(HCCSdAccess::gate.owner()==bb::SdOwner::free&&observer.phase()==bb::Phase::ready);
    }
    // Send an actual encoded Backup request into the simulated duplex stream.
    // This is not a physical device connection or a complete backup driver.
    const auto request_start=wire.output.size();const auto before_read=wire.offset;
    bb::BackupStartWriter start(observer,output,sizeof(output));
    assert(start.begin("00008101-001234567890ABCD")==bb::Error::ok);
    for(unsigned i=0;i<10000&&start.phase()!=bb::BackupStartPhase::sent;++i){
        const auto e=start.pump({&wire,RequestWire::write});assert(e==bb::Error::ok||e==bb::Error::would_block);
        assert(observer.phase()==(start.phase()==bb::BackupStartPhase::sent?bb::Phase::running:bb::Phase::ready));
    }
    assert(start.phase()==bb::BackupStartPhase::sent&&wire.offset==before_read);
    assert(bb::read_be32(wire.output.data()+request_start)==wire.output.size()-request_start-4);
    bool available=false;hcc::plist::Document doc;bb::SessionEvent event;
    for(;clock<10000&&!available;++clock){const auto e=link.receive_event(wire.io(),clock,doc,event,available,2);assert(e==bb::LinkError::ok||e==bb::LinkError::would_block||e==bb::LinkError::yield);}
    assert(available&&event.kind==bb::EventKind::upload_files);
    HCCUsbDisk::s_state=HCCUsbDisk::State::Off;
    const auto reply_start=wire.output.size();
    if(failure==3){
        {HCCBackupUploadRequest tiny(observer,scratch,sizeof(scratch),output,8);assert(tiny.start()==bb::Error::too_large);assert(tiny.phase()==bb::UploadRequestPhase::failed);}
        assert(observer.outcome()==bb::Outcome::local_error&&wire.output.size()==reply_start&&HCCSdAccess::gate.owner()==bb::SdOwner::free);return;
    }
    if(failure==4){
        {
            HCCBackupUploadRequest cancelled(observer,scratch,sizeof(scratch),output,sizeof(output));assert(cancelled.start()==bb::Error::ok);
            for(unsigned i=0;i<10000&&cancelled.phase()!=bb::UploadRequestPhase::reply;++i)assert(cancelled.pump(wire.io(),2)==bb::Error::would_block);
            assert(cancelled.phase()==bb::UploadRequestPhase::reply&&observer.phase()==bb::Phase::request_pending);
        }
        assert(observer.outcome()==bb::Outcome::local_error&&wire.output.size()==reply_start&&HCCSdAccess::gate.owner()==bb::SdOwner::free);return;
    }
    {
        HCCBackupUploadRequest request(observer,scratch,sizeof(scratch),output,sizeof(output));
        assert(request.start()==bb::Error::ok);assert(!HCCUsbDisk::requestStart());
        for(unsigned i=0;i<10000;++i){
            if(failure==2&&request.phase()==bb::UploadRequestPhase::reply)wire.close_write=true;
            const auto before=wire.calls;const auto e=request.pump(wire.io(),2);assert(wire.calls-before<=2);
            if(e!=bb::Error::would_block){assert(e==(failure==1?bb::Error::invalid_frame:failure==2?bb::Error::closed:bb::Error::ok));break;}
        }
        if(failure){assert(request.phase()==bb::UploadRequestPhase::failed&&observer.outcome()==bb::Outcome::local_error);assert(wire.output.size()==reply_start);}
        else {
            assert(request.phase()==bb::UploadRequestPhase::complete&&observer.phase()==bb::Phase::running&&observer.outcome()==bb::Outcome::none);
            assert(wire.offset==raw_end);
            std::ifstream f(fs::path(request.sessionPath())/"000001.ready/payload.bin",std::ios::binary);assert(f);std::string body{std::istreambuf_iterator<char>(f),{}};assert(body=="hello");
            assert(bb::read_be32(wire.output.data()+reply_start)==wire.output.size()-reply_start-4);
            hcc::plist::Node reply_nodes[32];char reply_text[512];hcc::plist::Document reply;
            assert(hcc::plist::parse(wire.output.data()+reply_start+4,wire.output.size()-reply_start-4,{reply_nodes,32,reply_text,512},reply)==hcc::plist::Error::ok);
            assert(reply.string(reply.at(reply.root,0))=="DLMessageStatusResponse"&&reply.get(reply.at(reply.root,1))->integer==0);
        }
        assert(HCCSdAccess::gate.owner()==bb::SdOwner::capture);
    }
    assert(HCCSdAccess::gate.owner()==bb::SdOwner::free);
    if(!failure){available=false;for(;clock<15000&&!available;++clock){const auto e=link.receive_event(wire.io(),clock,doc,event,available,2);assert(e==bb::LinkError::ok||e==bb::LinkError::would_block||e==bb::LinkError::yield);}assert(available&&event.kind==bb::EventKind::result&&observer.outcome()==bb::Outcome::success);}
}
static void operation_flow(unsigned mode){
    RequestWire wire;wire.fixture("version.bplist");wire.fixture("ready.xml");wire.fixture("hello.bplist");
    std::size_t pause=0;
    if(mode==0||mode==3||mode==6||mode==8||mode==13){
        for(unsigned batch=0;batch<2;++batch){
            wire.xml("<array><string>DLMessageUploadFiles</string><integer>0</integer><integer>0</integer><integer>5</integer></array>");
            wire.name("/device/source");wire.name("backup/test");wire.number(6);wire.input.push_back(0x0c);wire.bytes(batch?"world":"hello");
            if(batch==0)pause=wire.input.size();
            wire.number(1);wire.input.push_back(0);wire.number(0);
        }
        wire.fixture(mode==8?"result_error.bplist":"result_ok.xml");
    }else if(mode==7)wire.xml("<array><string>DLContentsOfDirectory</string><string>backup</string></array>");
    else if(mode==9)wire.fixture("disconnect.xml");
    std::uint8_t control[8192],output[1024],scratch[3];hcc::plist::Node nodes[128];char text[8192];
    bb::BackupLink link({control,sizeof(control),output,sizeof(output),{nodes,128,text,sizeof(text)}});
    assert(link.start(0)==bb::LinkError::ok);
    for(unsigned i=0;i<5000&&link.phase()!=bb::LinkPhase::ready;++i){auto e=link.poll(wire.io(),i,2);assert(e==bb::LinkError::ok||e==bb::LinkError::would_block||e==bb::LinkError::yield);}
    auto* observer=link.session_observer();assert(observer);const auto before=wire.output.size();
    HCCBackupConfig config;
    if(mode==1)space_available=0;
    if(mode==10)space_result=2;
    if(mode==6)config.limits.max_batch_bytes=7;
    if(mode==13)config.limits.max_files=1;
    if(mode==12)config.total_timeout_ms=3;
    bb::SdLease blocker(HCCSdAccess::gate,bb::SdOwner::usb_disk);
    if(mode==2)assert(blocker.acquire());
    {
        HCCBackupOperation operation(link,scratch,sizeof(scratch),output,sizeof(output));
        auto started=operation.start(mode==14?"invalid":"00008101-001234567890ABCD",10000,config);
        if(mode==1||mode==2||mode==10||mode==14){
            assert(started==(mode==2?HCCBackupError::busy:mode==14?HCCBackupError::invalid_argument:mode==1?HCCBackupError::quota:HCCBackupError::storage));
            assert(observer->phase()==bb::Phase::ready&&wire.output.size()==before);
        }else{
            assert(started==HCCBackupError::ok&&HCCSdAccess::gate.owner()==bb::SdOwner::capture);
            HCCBackupError result=HCCBackupError::would_block;
            std::uint64_t clock=10000;
            for(unsigned i=0;i<30000;++i){
                if(mode==3&&operation.phase()==HCCBackupPhase::receiving&&wire.offset>=pause){operation.cancel();result=operation.error();break;}
                if(mode==4&&operation.phase()==HCCBackupPhase::waiting)clock+=config.idle_timeout_ms;
                if(mode==5)clock=9999;
                const auto calls=wire.calls;result=operation.poll(wire.io(),clock++,2);assert(wire.calls-calls<=2);
                if(result!=HCCBackupError::would_block)break;
            }
            if(mode==0){assert(result==HCCBackupError::ok&&operation.phase()==HCCBackupPhase::succeeded&&operation.committedFiles()==2&&observer->outcome()==bb::Outcome::success);}
            else if(mode==3){assert(result==HCCBackupError::cancelled&&operation.phase()==HCCBackupPhase::cancelled&&operation.committedFiles()==0);assert(fs::is_empty(operation.sessionPath()));}
            else if(mode==4||mode==12)assert(result==HCCBackupError::timeout);
            else if(mode==5)assert(result==HCCBackupError::clock);
            else if(mode==6||mode==13){assert(result==HCCBackupError::quota&&operation.committedFiles()==1);}
            else if(mode==7)assert(result==HCCBackupError::unsupported);
            else if(mode==8)assert(result==HCCBackupError::remote&&operation.remoteCode()==5&&observer->outcome()==bb::Outcome::remote_error);
            else if(mode==9)assert(result==HCCBackupError::disconnected);
            assert(HCCSdAccess::gate.owner()==bb::SdOwner::capture);
        }
    }
    if(mode==2){assert(HCCSdAccess::gate.owner()==bb::SdOwner::usb_disk);blocker.release();}
    space_available=1073741824;space_result=0;
    assert(HCCSdAccess::gate.owner()==bb::SdOwner::free);
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
    for(unsigned failure=0;failure<5;++failure)upload_request_flow(failure);
    std::puts("PASS: UploadFiles control -> real capture -> status -> final result, with terminal/TX failure paths");
    for(unsigned mode:{0,1,2,3,4,5,6,7,8,9,10,12,13,14})operation_flow(mode);
    std::puts("PASS: operation preflight, multi-batch SD ownership, cumulative quotas, result, cancellation and deadlines");
    {
        using R=HCCBackupSdSelfTest::Result;
        assert(HCCBackupSdSelfTest::request());assert(HCCBackupSdSelfTest::result()==R::running);
        assert(!HCCBackupSdSelfTest::request()&&!HCCBackupSdSelfTest::clear());
        run_selftest_worker();assert(HCCBackupSdSelfTest::result()==R::passed&&HCCSdAccess::gate.owner()==bb::SdOwner::free);
        assert(HCCBackupSdSelfTest::clear());
        bb::SdLease disk(HCCSdAccess::gate,bb::SdOwner::usb_disk);assert(disk.acquire());
        assert(HCCBackupSdSelfTest::request());run_selftest_worker();assert(HCCBackupSdSelfTest::result()==R::busy&&HCCSdAccess::gate.owner()==bb::SdOwner::usb_disk);disk.release();
        space_available=0;assert(HCCBackupSdSelfTest::request());run_selftest_worker();assert(HCCBackupSdSelfTest::result()==R::storage);space_available=1073741824;
        test_task_fails=true;assert(!HCCBackupSdSelfTest::request()&&HCCBackupSdSelfTest::result()==R::task);test_task_fails=false;
        assert(HCCBackupSdSelfTest::clear());
    }
    std::puts("PASS: SD readiness self-test worker (host files), verification, abort cleanup, duplicate/busy/space/task failures");
    fs::remove_all(temp);
    std::puts("PASS: startup rollback and capture mount failure release only safe ownership");
}
