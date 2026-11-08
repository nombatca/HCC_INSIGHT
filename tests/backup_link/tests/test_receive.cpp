#include "hcc_backup_receive.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
namespace bb=hcc::backup;
static void number(std::vector<std::uint8_t>& out,std::uint32_t n){std::uint8_t b[4];bb::write_be32(b,n);out.insert(out.end(),b,b+4);}
static void name(std::vector<std::uint8_t>& out,const std::string& s){number(out,s.size());out.insert(out.end(),s.begin(),s.end());}
static void record(std::vector<std::uint8_t>& out,const std::string& path,const std::vector<std::uint8_t>& data,std::uint8_t end=0){
    name(out,"/opaque/device/name");name(out,path);
    if(!data.empty()){number(out,data.size()+1);out.push_back(0x0c);out.insert(out.end(),data.begin(),data.end());}
    number(out,1);out.push_back(end);
}
struct Wire {
    std::vector<std::uint8_t> input;std::size_t offset=0,chunk=7,calls=0;
    void* retry=nullptr;std::size_t length=0;bool close=false;
    static bb::IoResult read(void* ctx,std::uint8_t* p,std::size_t n){
        auto& w=*static_cast<Wire*>(ctx);++w.calls;
        if(!w.retry){w.retry=p;w.length=n;return {0,bb::IoState::would_block};}
        assert(w.retry==p&&w.length==n);
        const auto take=std::min({n,w.chunk,w.input.size()-w.offset});
        if(!take)return {0,w.close?bb::IoState::closed:bb::IoState::would_block};
        std::memcpy(p,w.input.data()+w.offset,take);w.offset+=take;w.retry=nullptr;return {take,bb::IoState::ok};
    }
    bb::Duplex io(){return {this,read,nullptr};}
};
struct Target {
    std::vector<std::vector<std::uint8_t>> committed;
    std::vector<std::uint8_t> pending;unsigned begun=0,aborted=0,calls=0;
    bool fail_write=false,fail_commit=false;
    const std::uint8_t* retry=nullptr;std::size_t length=0;
    static bb::Error begin(void* ctx,std::string_view source,std::string_view dest){auto& t=*static_cast<Target*>(ctx);assert(source=="/opaque/device/name");assert(!dest.empty());++t.begun;t.pending.clear();return bb::Error::ok;}
    static bb::IoResult write(void* ctx,const std::uint8_t* p,std::size_t n){
        auto& t=*static_cast<Target*>(ctx);++t.calls;
        if(t.fail_write)return {0,bb::IoState::error};
        if(!t.retry){t.retry=p;t.length=n;return {0,bb::IoState::would_block};}
        assert(t.retry==p&&t.length==n);t.retry=nullptr;
        const auto take=std::min(n,std::size_t(3));t.pending.insert(t.pending.end(),p,p+take);return {take,bb::IoState::ok};
    }
    static bb::Error finish(void* ctx,bool commit){auto& t=*static_cast<Target*>(ctx);if(commit){if(t.fail_commit){t.pending.clear();return bb::Error::io_error;}t.committed.push_back(t.pending);}else ++t.aborted;t.pending.clear();return bb::Error::ok;}
    bb::UploadTarget io(){return {this,begin,write,finish};}
};
static bb::Error drive(bb::UploadBatchReceiver& receiver,Wire& w,Target& t){
    for(unsigned i=0;i<100000;++i){const auto before=w.calls+t.calls;const auto e=receiver.pump(w.io(),2);assert(w.calls+t.calls-before<=2);if(e!=bb::Error::would_block)return e;}
    assert(false);return bb::Error::io_error;
}
int main(){
    std::array<std::uint8_t,13> scratch{};
    const std::vector<std::uint8_t> payload(113,0x42);
    for(auto chunk:{std::size_t(1),std::size_t(7),std::size_t(4096)}){
        Wire w;w.chunk=chunk;record(w.input,"backup/a",payload);record(w.input,"backup/empty",{});number(w.input,0);w.input.push_back(0x99);
        Target t;bb::UploadBatchReceiver r(scratch.data(),scratch.size());assert(r.start(t.io())==bb::Error::ok);
        assert(drive(r,w,t)==bb::Error::ok&&r.done()&&r.completed_files()==2);
        assert(t.committed.size()==2&&t.committed[0]==payload&&t.committed[1].empty()&&!t.aborted);
        assert(r.completed_payload_bytes()==payload.size()&&w.offset+1==w.input.size());
    }
    std::cout<<"PASS: upload wire splits, stable retries, budgets, empty files and no overread\n";
    std::vector<std::uint8_t> complete;record(complete,"backup/a",{1,2,3});number(complete,0);
    for(std::size_t cut=0;cut<complete.size();++cut){
        Wire w;w.input.assign(complete.begin(),complete.begin()+cut);w.close=true;Target t;
        bb::UploadBatchReceiver r(scratch.data(),scratch.size());assert(r.start(t.io())==bb::Error::ok);
        assert(drive(r,w,t)==bb::Error::truncated&&!r.done());
        assert(t.begun==t.committed.size()+t.aborted); // Every opened target resolved exactly once.
    }
    std::cout<<"PASS: every truncated stream position aborts any uncommitted target\n";
    for(unsigned kind=0;kind<9;++kind){
        Wire w;Target t;
        if(kind==0)record(w.input,"../escape",payload);
        if(kind==1){name(w.input,"/opaque/device/name");number(w.input,4097);}
        if(kind==2)record(w.input,"backup/a",payload,0x0b);
        if(kind==3){record(w.input,"backup/a",payload);t.fail_write=true;}
        if(kind==4){record(w.input,"backup/a",payload);t.fail_commit=true;}
        if(kind==5||kind==6||kind==7)record(w.input,"backup/a",payload);
        if(kind==8){record(w.input,"backup/a",{});record(w.input,"backup/b",{});}
        number(w.input,0);
        bb::UploadBatchReceiver r(scratch.data(),scratch.size());bb::UploadLimits limits;
        if(kind==5)limits.max_file_bytes=3;
        if(kind==6)limits.max_batch_bytes=3;
        if(kind==7)limits.max_frame_payload=3;
        if(kind==8)limits.max_files=1;
        assert(r.start(t.io(),limits)==bb::Error::ok);const auto e=drive(r,w,t);
        assert(e!=bb::Error::ok&&e!=bb::Error::would_block&&!r.done());
        assert(t.committed.size()==(kind==8?1u:0u));assert(r.pump(w.io())==e);
        if(kind==0||kind==1)assert(!t.begun);
        if(kind==2||kind==3||kind==5||kind==6||kind==7)assert(t.aborted==1);
    }
    std::cout<<"PASS: paths, bounded lengths, remote terminal, write/commit failure and byte limits\n";
    {Wire w;number(w.input,0);Target t;bb::UploadBatchReceiver r(scratch.data(),scratch.size());assert(r.start(t.io())==bb::Error::ok);assert(drive(r,w,t)==bb::Error::ok&&r.done()&&!t.begun);}
    {Wire w;record(w.input,"backup/a",payload);Target t;bb::UploadBatchReceiver r(scratch.data(),scratch.size());assert(r.start(t.io())==bb::Error::ok);while(!t.begun)assert(r.pump(w.io(),1)==bb::Error::would_block);r.cancel();r.cancel();assert(t.aborted==1&&!r.done());}
    std::cout<<"PASS: empty batch and explicit cancellation\n";
}
