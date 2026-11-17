#include "hcc_backup_start.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
namespace bb=hcc::backup;namespace pp=hcc::plist;
struct Memory {
    pp::Node nodes[64];char text[4096];pp::Document doc;
    void parse(const std::uint8_t* p,std::size_t n){assert(pp::parse(p,n,{nodes,64,text,sizeof(text)},doc)==pp::Error::ok);}
    void ready(bb::SessionObserver& observer){
        bb::SessionEvent event;
        for(const auto body:{"<array><string>DLMessageVersionExchange</string><integer>400</integer><integer>0</integer></array>","<array><string>DLMessageDeviceReady</string></array>"}){
            const std::string xml=std::string("<plist version=\"1.0\">")+body+"</plist>";parse(reinterpret_cast<const std::uint8_t*>(xml.data()),xml.size());assert(observer.consume(doc,event)==bb::SessionError::ok);
            if(event.kind==bb::EventKind::version_offer)assert(observer.acknowledge_version(true)==bb::SessionError::ok);
        }
    }
};
struct Wire {
    std::vector<std::uint8_t> output;const std::uint8_t* retry=nullptr;std::size_t n=0;
    static bb::IoResult write(void* ctx,const std::uint8_t* p,std::size_t size){
        auto& w=*static_cast<Wire*>(ctx);
        if(!w.retry){w.retry=p;w.n=size;return {0,bb::IoState::would_block};}
        assert(w.retry==p&&w.n==size);w.retry=nullptr;const auto take=std::min(size,std::size_t(5));
        w.output.insert(w.output.end(),p,p+take);return {take,bb::IoState::ok};
    }
};
int main(){
    std::array<std::uint8_t,1024> bytes{};Memory m;
    const char* modern="00008101-001234567890ABCD";const char* legacy="0123456789abcdef0123456789abcdef01234567";
    for(bool full:{false,true})for(const char* id:{modern,legacy}){
        std::size_t n=0;assert(bb::encode_backup_request(id,full,bytes.data(),bytes.size(),n)==bb::LinkError::ok);
        m.parse(bytes.data(),n);assert(m.doc.string(m.doc.at(m.doc.root,0))=="DLMessageProcessMessage");
        const auto dict=m.doc.at(m.doc.root,1);assert(m.doc.string(m.doc.find(dict,"MessageName"))=="Backup");
        assert(m.doc.string(m.doc.find(dict,"TargetIdentifier"))==id&&m.doc.string(m.doc.find(dict,"SourceIdentifier"))==id);
        const auto options=m.doc.find(dict,"Options");
        if(full){assert(m.doc.get(options)->kind==pp::Kind::dict);assert(m.doc.get(m.doc.find(options,"ForceFullBackup"))->boolean);}
        else assert(options==pp::none);
        if(id==modern){std::ofstream out(full?"build/backup_full.bplist":"build/backup_default.bplist",std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),n);}
    }
    std::cout<<"PASS: Backup-only request fields, modern/legacy IDs and explicit full option\n";
    for(auto id:{"","../escape","00008101_001234567890ABCD","00008101-001234567890ABCZ"}){
        std::size_t n=99;assert(bb::encode_backup_request(id,false,bytes.data(),bytes.size(),n)==bb::LinkError::invalid_argument&&n==0);
    }
    {std::size_t n=99;assert(bb::encode_backup_request(modern,false,bytes.data(),8,n)==bb::LinkError::too_large&&n==0);}
    std::cout<<"PASS: invalid identifiers and bounded output\n";
    {
        bb::SessionObserver observer;m.ready(observer);Wire wire;bb::BackupStartWriter writer(observer,bytes.data(),bytes.size());
        assert(writer.begin(modern)==bb::Error::ok);
        for(unsigned i=0;i<10000&&writer.phase()!=bb::BackupStartPhase::sent;++i){
            const auto e=writer.pump({&wire,Wire::write});assert(e==bb::Error::ok||e==bb::Error::would_block);
            assert(observer.phase()==(writer.phase()==bb::BackupStartPhase::sent?bb::Phase::running:bb::Phase::ready));
        }
        assert(writer.phase()==bb::BackupStartPhase::sent&&observer.outcome()==bb::Outcome::none);
        assert(bb::read_be32(wire.output.data())==wire.output.size()-4);assert(writer.begin(modern)==bb::Error::busy);
    }
    std::cout<<"PASS: partial writes and stable retries do not mark running early\n";
    for(unsigned failure=0;failure<4;++failure){
        bb::SessionObserver observer;m.ready(observer);bb::BackupStartWriter writer(observer,bytes.data(),bytes.size());assert(writer.begin(modern)==bb::Error::ok);
        auto callback=[](void* ctx,const std::uint8_t*,std::size_t n)->bb::IoResult{
            const auto mode=*static_cast<unsigned*>(ctx);
            if(mode==0)return {0,bb::IoState::closed};
            if(mode==1)return {0,bb::IoState::ok};
            if(mode==2)return {n+1,bb::IoState::ok};
            return {n,bb::IoState::closed};
        };
        const auto e=writer.pump({&failure,callback});assert(e==bb::Error::closed||e==bb::Error::no_progress||e==bb::Error::io_error);
        assert(writer.phase()==bb::BackupStartPhase::failed&&observer.outcome()==bb::Outcome::local_error);assert(writer.pump({&failure,callback})==e);
    }
    {
        bb::SessionObserver observer;m.ready(observer);{bb::BackupStartWriter writer(observer,bytes.data(),bytes.size());assert(writer.begin(modern)==bb::Error::ok);writer.cancel();}assert(observer.outcome()==bb::Outcome::local_error);
    }
    {
        bb::SessionObserver observer;bb::BackupStartWriter writer(observer,bytes.data(),bytes.size());assert(writer.begin(modern)==bb::Error::invalid_argument&&observer.phase()==bb::Phase::await_version);
    }
    std::cout<<"PASS: failed writes, close with final bytes, cancellation and wrong phase\n";
}
