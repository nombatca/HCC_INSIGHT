#include "hcc_backup_link.hpp"
#include "hcc_tls_callbacks.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace bb=hcc::backup;namespace pp=hcc::plist;
#define CHECK(x) do { if(!(x)) { std::cerr<<"FAIL line "<<__LINE__<<": " #x "\n";std::abort(); } }while(false)
std::vector<std::uint8_t> load(const std::string& name) {
    std::ifstream in("tests/fixtures/"+name,std::ios::binary);CHECK(bool(in));
    return {std::istreambuf_iterator<char>(in),{}};
}
void frame(std::vector<std::uint8_t>& out,const std::vector<std::uint8_t>& payload) {
    std::array<std::uint8_t,4> header{};bb::write_be32(header.data(),std::uint32_t(payload.size()));
    out.insert(out.end(),header.begin(),header.end());out.insert(out.end(),payload.begin(),payload.end());
}
std::vector<std::uint8_t> xml(const std::string& body) {
    const auto s="<plist version=\"1.0\">"+body+"</plist>";return {s.begin(),s.end()};
}
struct Memory {
    std::array<std::uint8_t,8192> receive{};std::array<std::uint8_t,512> transmit{};
    std::array<pp::Node,256> nodes{};std::array<char,16384> text{};
    bb::LinkBuffers buffers() {return {receive.data(),receive.size(),transmit.data(),transmit.size(),{nodes.data(),nodes.size(),text.data(),text.size()}};}
    pp::Document decode(const std::uint8_t* p,std::size_t n) {
        pp::Document d;CHECK(pp::parse(p,n,{nodes.data(),nodes.size(),text.data(),text.size()},d)==pp::Error::ok);return d;
    }
};
struct Wire {
    std::vector<std::uint8_t> incoming,outgoing;
    std::size_t offset=0,chunk=100000,calls=0,block_every=0;
    std::size_t close_after_write=std::numeric_limits<std::size_t>::max();
    bool block_read=false,zero_read=false,close_on_empty=false,block_after_progress=false;
    static bb::IoResult read(void* p,std::uint8_t* b,std::size_t n) {
        auto& w=*static_cast<Wire*>(p);++w.calls;
        if(w.zero_read)return {0,bb::IoState::ok};
        if(w.block_read||(w.block_every&&w.calls%w.block_every==0))return {0,bb::IoState::would_block};
        if(w.offset==w.incoming.size())return {0,w.close_on_empty?bb::IoState::closed:bb::IoState::would_block};
        const auto take=std::min({n,w.chunk,w.incoming.size()-w.offset});
        std::copy_n(w.incoming.data()+w.offset,take,b);w.offset+=take;
        return {take,w.block_after_progress?bb::IoState::would_block:bb::IoState::ok};
    }
    static bb::IoResult write(void* p,const std::uint8_t* b,std::size_t n) {
        auto& w=*static_cast<Wire*>(p);++w.calls;
        if(w.outgoing.size()>=w.close_after_write)return {0,bb::IoState::closed};
        if(w.block_every&&w.calls%w.block_every==0)return {0,bb::IoState::would_block};
        const auto take=std::min({n,w.chunk,w.close_after_write-w.outgoing.size()});
        w.outgoing.insert(w.outgoing.end(),b,b+take);
        return {take,w.block_after_progress?bb::IoState::would_block:bb::IoState::ok};
    }
    bb::Duplex io() {return {this,read,write};}
};
void input(Wire& w,const std::vector<std::uint8_t>& hello={}) {
    frame(w.incoming,load("version.bplist"));frame(w.incoming,load("ready.xml"));
    frame(w.incoming,hello.empty()?load("hello.bplist"):hello);
}
bb::LinkError drive(bb::BackupLink& link,Wire& w,std::size_t budget=2,std::uint64_t step=1) {
    for(std::uint64_t t=0;t<100000;t+=step) {
        const auto before=w.calls;const auto e=link.poll(w.io(),t,budget);CHECK(w.calls-before<=budget);
        if(e!=bb::LinkError::yield&&e!=bb::LinkError::would_block)return e;
    }
    CHECK(false);return bb::LinkError::timeout;
}
void check_outbound(Wire& w) {
    Memory m;std::size_t offset=0,count=0;
    while(offset<w.outgoing.size()) {
        CHECK(w.outgoing.size()-offset>=4);const auto n=bb::read_be32(w.outgoing.data()+offset);offset+=4;
        CHECK(n<=w.outgoing.size()-offset);const auto d=m.decode(w.outgoing.data()+offset,n);offset+=n;
        if(count==0) {
            CHECK(d.string(d.at(d.root,0))=="DLMessageVersionExchange");
            CHECK(d.string(d.at(d.root,1))=="DLVersionsOk");CHECK(d.get(d.at(d.root,2))->integer==400);
        } else {
            CHECK(count==1 && d.string(d.at(d.root,0))=="DLMessageProcessMessage");
            const auto dict=d.at(d.root,1);CHECK(d.string(d.find(dict,"MessageName"))=="Hello");
            const auto versions=d.find(dict,"SupportedProtocolVersions");CHECK(d.get(versions)->count==2);
            CHECK(d.get(d.at(versions,0))->real==2.0 && d.get(d.at(versions,1))->real==2.1);
        }
        ++count;
    }
    CHECK(count==2);
}
void encoders_bounds_and_semantics() {
    std::array<std::uint8_t,512> out{};std::size_t n=0;
    CHECK(bb::encode_version_reply(400,out.data(),out.size(),n)==bb::LinkError::ok);
    const auto need=n;CHECK(need>40);
    for(std::size_t cap=0;cap<need;++cap) {
        std::array<std::uint8_t,514> guard{};guard.fill(0xa5);std::size_t used=100;
        CHECK(bb::encode_version_reply(400,guard.data()+1,cap,used)==bb::LinkError::too_large);
        CHECK(used==0 && guard[0]==0xa5 && guard[cap+1]==0xa5);
    }
    for(const auto offer:{0,1,2}) {
        CHECK(bb::encode_hello(offer!=1,offer!=0,out.data(),out.size(),n)==bb::LinkError::ok);
        Memory m;const auto d=m.decode(out.data(),n);const auto dict=d.at(d.root,1);
        CHECK(d.string(d.find(dict,"MessageName"))=="Hello");
        CHECK(d.get(d.find(dict,"SupportedProtocolVersions"))->count==(offer==2?2u:1u));
    }
    CHECK(bb::encode_hello(false,false,out.data(),out.size(),n)==bb::LinkError::invalid_argument && n==0);
    CHECK(bb::encode_version_reply(UINT64_MAX,out.data(),out.size(),n)==bb::LinkError::invalid_argument);
}
void fragmented_handshake_and_no_overread() {
    for(const auto chunk:{1u,2u,31u,4096u}) {
        Memory m;bb::BackupLink link(m.buffers());Wire w;w.chunk=chunk;w.block_every=5;w.block_after_progress=true;
        input(w);const auto handshake_size=w.incoming.size();frame(w.incoming,load("download.bplist"));
        CHECK(link.start(0)==bb::LinkError::ok);CHECK(drive(link,w)==bb::LinkError::ok);
        CHECK(link.phase()==bb::LinkPhase::ready && link.protocol_version()==2.1);
        CHECK(w.offset==handshake_size);CHECK(link.received_bytes()==handshake_size);
        CHECK(link.sent_bytes()==w.outgoing.size());check_outbound(w);
        const auto calls=w.calls;CHECK(link.poll(w.io(),1000)==bb::LinkError::ok && w.calls==calls);
        auto* observer=link.session_observer();CHECK(observer && observer->phase()==bb::Phase::ready);
        CHECK(observer->mark_operation_started()==bb::SessionError::ok); // Simulated application handoff.
        const auto request=load("download.bplist");const auto d=m.decode(request.data(),request.size());
        bb::SessionEvent event;CHECK(observer->consume(d,event)==bb::SessionError::ok);
        CHECK(event.kind==bb::EventKind::download_files);
        if(chunk==4096) {
            std::ofstream out("build/handshake_output.bin",std::ios::binary);
            out.write(reinterpret_cast<const char*>(w.outgoing.data()),std::streamsize(w.outgoing.size()));
        }
    }
}
void clock_and_inactivity_timeouts() {
    Memory m;bb::BackupLink link(m.buffers());Wire w;w.block_read=true;
    bb::LinkConfig config;config.inactivity_timeout_ms=5;CHECK(link.start(0,config)==bb::LinkError::ok);
    CHECK(link.poll(w.io(),4)==bb::LinkError::would_block);CHECK(link.poll(w.io(),5)==bb::LinkError::timeout);
    CHECK(link.poll(w.io(),6)==bb::LinkError::timeout);
    CHECK(link.start(10)==bb::LinkError::ok);CHECK(link.poll(w.io(),9)==bb::LinkError::clock_error);
    Wire slow;input(slow);slow.chunk=1;
    CHECK(link.start(0,config)==bb::LinkError::ok);CHECK(drive(link,slow,1,3)==bb::LinkError::ok);
    CHECK(slow.calls>5); // Total duration exceeds timeout; ongoing progress remains valid.
}
void malformed_closed_and_oversized() {
    Memory m;bb::BackupLink link(m.buffers());Wire w;
    w.incoming={0xff,0xff,0xff,0xff};CHECK(link.start(0)==bb::LinkError::ok);
    CHECK(drive(link,w)==bb::LinkError::too_large);
    w=Wire{};w.incoming={0,0};w.close_on_empty=true;CHECK(link.start(0)==bb::LinkError::ok);
    CHECK(drive(link,w)==bb::LinkError::truncated);
    w=Wire{};w.zero_read=true;CHECK(link.start(0)==bb::LinkError::ok);
    CHECK(drive(link,w)==bb::LinkError::no_progress);
    w=Wire{};frame(w.incoming,load("ready.xml"));CHECK(link.start(0)==bb::LinkError::ok);
    CHECK(drive(link,w)==bb::LinkError::malformed);
    w=Wire{};input(w);w.close_after_write=3;CHECK(link.start(0)==bb::LinkError::ok);
    CHECK(drive(link,w)==bb::LinkError::closed);CHECK(w.outgoing.size()==3);
}
void version_and_response_validation() {
    Memory m;bb::BackupLink link(m.buffers());Wire w;
    frame(w.incoming,xml("<array><string>DLMessageVersionExchange</string><integer>401</integer><integer>0</integer></array>"));
    CHECK(link.start(0)==bb::LinkError::ok);CHECK(drive(link,w)==bb::LinkError::incompatible_version);
    for(const auto variant:{0,1,2,3}) {
        const std::string code=variant==0?"1":"0";
        const std::string version=variant==1?"<real>9.0</real>":variant==2?"<integer>2</integer>":"<real>2.0</real>";
        w=Wire{};input(w,xml("<array><string>DLMessageProcessMessage</string><dict><key>MessageName</key><string>Response</string><key>ErrorCode</key><integer>"+code+"</integer><key>ProtocolVersion</key>"+version+"</dict></array>"));
        bb::LinkConfig config;config.offer_21=false;CHECK(link.start(0,config)==bb::LinkError::ok);
        const auto e=drive(link,w);
        if(variant==0) CHECK(e==bb::LinkError::incompatible_version && link.remote_code()==1);
        if(variant==1) CHECK(e==bb::LinkError::incompatible_version);
        if(variant==2) CHECK(e==bb::LinkError::malformed);
        if(variant==3) CHECK(e==bb::LinkError::ok && link.protocol_version()==2.0);
    }
}
void arguments_and_output_capacity() {
    Memory m;Wire w;auto small=m.buffers();small.transmit_capacity=8;bb::BackupLink link(small);input(w);
    CHECK(link.start(0)==bb::LinkError::ok);CHECK(drive(link,w)==bb::LinkError::too_large);
    bb::BackupLink idle(m.buffers());CHECK(idle.poll(w.io(),0)==bb::LinkError::wrong_state);
    CHECK(idle.session_observer()==nullptr);
    bb::LinkConfig config;config.offer_20=config.offer_21=false;
    CHECK(idle.start(0,config)==bb::LinkError::invalid_argument);
    CHECK(idle.start(0)==bb::LinkError::ok);CHECK(idle.start(0)==bb::LinkError::wrong_state);
    CHECK(idle.poll(w.io(),0,0)==bb::LinkError::invalid_argument);
}
struct FakeTls {
    int result=1;
    static int read(void* ctx,unsigned char*,std::size_t) {return static_cast<FakeTls*>(ctx)->result;}
    static int write(void* ctx,const unsigned char*,std::size_t) {return static_cast<FakeTls*>(ctx)->result;}
};
void tls_callback_mapping() {
    FakeTls fake;bb::TlsCallbacks tls(&fake,FakeTls::read,FakeTls::write,{-101,-102,-103});auto io=tls.duplex();std::uint8_t b[4]{};
    auto r=io.read(io.context,b,4);CHECK(r.bytes==1 && r.state==bb::IoState::ok);
    fake.result=-101;r=io.write(io.context,b,4);CHECK(r.state==bb::IoState::would_block && tls.interest()==bb::IoInterest::read);
    fake.result=-102;r=io.read(io.context,b,4);CHECK(r.state==bb::IoState::would_block && tls.interest()==bb::IoInterest::write);
    fake.result=-103;CHECK(io.read(io.context,b,4).state==bb::IoState::closed);
    fake.result=0;CHECK(io.write(io.context,b,4).state==bb::IoState::closed);
    fake.result=-999;CHECK(io.read(io.context,b,4).state==bb::IoState::error && tls.last_error()==-999);
    fake.result=5;CHECK(io.write(io.context,b,4).state==bb::IoState::error);
}
struct RetryTls {
    Wire wire;unsigned char* read_pointer=nullptr;const unsigned char* write_pointer=nullptr;
    std::size_t read_size=0,write_size=0;bool read_pending=false,write_pending=false;
    static int read(void* p,unsigned char* b,std::size_t n) {
        auto& s=*static_cast<RetryTls*>(p);
        if(!s.read_pending){s.read_pointer=b;s.read_size=n;s.read_pending=true;return -102;}
        CHECK(s.read_pointer==b && s.read_size==n);s.read_pending=false;
        const auto r=Wire::read(&s.wire,b,n);return r.bytes?int(r.bytes):-101;
    }
    static int write(void* p,const unsigned char* b,std::size_t n) {
        auto& s=*static_cast<RetryTls*>(p);
        if(!s.write_pending){s.write_pointer=b;s.write_size=n;s.write_pending=true;return -101;}
        CHECK(s.write_pointer==b && s.write_size==n);s.write_pending=false;
        const auto r=Wire::write(&s.wire,b,n);return r.bytes?int(r.bytes):-102;
    }
};
void tls_retry_arguments_remain_stable() {
    Memory memory;bb::BackupLink link(memory.buffers());RetryTls transport;
    input(transport.wire);transport.wire.chunk=7;
    bb::TlsCallbacks tls(&transport,RetryTls::read,RetryTls::write,{-101,-102,-103});
    CHECK(link.start(0)==bb::LinkError::ok);
    for(std::uint64_t t=0;t<10000&&link.phase()!=bb::LinkPhase::ready;++t) {
        const auto e=link.poll(tls.duplex(),t,4);
        CHECK(e==bb::LinkError::ok||e==bb::LinkError::would_block||e==bb::LinkError::yield);
    }
    CHECK(link.phase()==bb::LinkPhase::ready);check_outbound(transport.wire);
}
int main() {
    const struct {const char* name;void(*run)();} tests[]={
        {"binary control encoders and bounded output",encoders_bounds_and_semantics},
        {"fragmented handshake, budgets and no overread",fragmented_handshake_and_no_overread},
        {"inactivity timeout, progress and monotonic clock",clock_and_inactivity_timeouts},
        {"oversized/truncated input and connection loss",malformed_closed_and_oversized},
        {"version selection and typed Response validation",version_and_response_validation},
        {"invalid arguments and undersized output",arguments_and_output_capacity},
        {"TLS callback status and readiness mapping",tls_callback_mapping},
        {"TLS retries preserve original pointers and lengths",tls_retry_arguments_remain_stable},
    };
    for(const auto& t:tests){t.run();std::cout<<"PASS: "<<t.name<<"\n";}
    std::cout<<"8 link test groups passed; transport is simulated.\n";
}
