#include "hcc_backup_storage.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>
namespace bb=hcc::backup;
namespace fs=std::filesystem;
static std::vector<std::uint8_t> read(const fs::path& p){std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
struct Wire{
    std::vector<std::uint8_t> bytes;std::size_t pos=0;
    void number(std::uint32_t n){std::uint8_t h[4];bb::write_be32(h,n);bytes.insert(bytes.end(),h,h+4);}
    void name(const std::string& s){number(s.size());bytes.insert(bytes.end(),s.begin(),s.end());}
    void file(const std::string& dest,const std::string& body,std::uint8_t end=0){name("/device/opaque");name(dest);if(!body.empty()){number(body.size()+1);bytes.push_back(0x0c);bytes.insert(bytes.end(),body.begin(),body.end());}number(1);bytes.push_back(end);}
    static bb::IoResult receive(void* ctx,std::uint8_t* b,std::size_t n){auto& w=*static_cast<Wire*>(ctx);if(w.pos==w.bytes.size())return {0,bb::IoState::closed};const auto take=std::min({n,std::size_t(3),w.bytes.size()-w.pos});std::memcpy(b,w.bytes.data()+w.pos,take);w.pos+=take;return {take,bb::IoState::ok};}
};
static bb::Error drive(bb::UploadBatchReceiver& r,Wire& w){for(unsigned i=0;i<10000;++i){const auto e=r.pump({&w,Wire::receive,nullptr},2);if(e!=bb::Error::would_block)return e;}assert(false);return bb::Error::io_error;}
int main(){
    char temp[]="/tmp/hcc-storage-XXXXXX";assert(mkdtemp(temp));const fs::path parent(temp);
    {
        bb::StagedUploadTarget target;assert(target.open_session(temp)==bb::Error::ok);
        assert(target.open_session(temp)==bb::Error::busy);const fs::path root(target.session_path());
        Wire w;w.file("backup/a","abcdef",0x0b);w.file("backup/empty","");w.number(0);
        std::uint8_t scratch[2];bb::UploadBatchReceiver r(scratch,sizeof(scratch));
        assert(r.start(target.target())==bb::Error::ok&&drive(r,w)==bb::Error::ok&&r.done());
        assert(target.committed_files()==2&&!fs::exists(root/"000001.part"));
        const auto bytes=read(root/"000001.ready/payload.bin");assert(std::string(bytes.begin(),bytes.end())=="abcdef");
        assert(read(root/"000002.ready/payload.bin").empty());
        const auto meta=read(root/"000001.ready/identity.bin");assert(std::memcmp(meta.data(),"HCCU0001",8)==0);
        std::size_t p=8;auto n=bb::read_be32(meta.data()+p);p+=4;assert(std::string(meta.begin()+p,meta.begin()+p+n)=="/device/opaque");p+=n;n=bb::read_be32(meta.data()+p);p+=4;assert(std::string(meta.begin()+p,meta.begin()+p+n)=="backup/a"&&p+n==meta.size());
    }
    std::cout<<"PASS: real filesystem upload receiver, staged payload/identity publication and empty file\n";
    {
        bb::StagedUploadTarget target;assert(target.open_session(temp)==bb::Error::ok);const fs::path root(target.session_path());
        Wire w;w.file("backup/a","incomplete",0x0d);std::uint8_t scratch[4];bb::UploadBatchReceiver r(scratch,sizeof(scratch));
        assert(r.start(target.target())==bb::Error::ok&&drive(r,w)==bb::Error::invalid_frame);
        assert(fs::is_empty(root)&&!target.committed_files());
        auto cb=target.target();assert(cb.begin(cb.context,"source","../escape")==bb::Error::invalid_argument);
        assert(cb.begin(cb.context,"source","backup/b")==bb::Error::ok);
        const std::uint8_t data[]={1,2};assert(cb.write(cb.context,data,2).bytes==2);
        fs::create_directory(root/"000002.ready"); // Existing destination must never be overwritten.
        assert(cb.finish(cb.context,true)==bb::Error::io_error);
        assert(fs::is_directory(root/"000002.ready")&&!fs::exists(root/"000002.part"));
    }
    std::cout<<"PASS: unknown terminal aborts staged data and commit collision preserves existing directory\n";
    fs::path pending;
    {
        bb::StagedUploadTarget target;assert(target.open_session(temp)==bb::Error::ok);pending=target.session_path();
        auto cb=target.target();assert(cb.begin(cb.context,"source","backup/c")==bb::Error::ok);
    }
    assert(fs::is_empty(pending));
    {bb::StagedUploadTarget target;assert(target.open_session("/path/that/does/not/exist")==bb::Error::io_error);}
    fs::create_directory_symlink(parent,parent/"alias");
    {bb::StagedUploadTarget target;assert(target.open_session((parent/"alias").c_str())==bb::Error::io_error);}
    {
        bb::StagedUploadTarget target;assert(target.create_directory("a")==bb::Error::invalid_argument);
        assert(target.open_session(temp)==bb::Error::ok);const fs::path root(target.session_path());
        for(const auto* path:{"../escape","a/../b","/absolute","a//b","a/","a./b","a /b","a/*"}){
            assert(target.create_directory(path)==bb::Error::invalid_argument);assert(fs::is_empty(root));
        }
        assert(target.create_directory(std::string(256,'x'))==bb::Error::too_large&&fs::is_empty(root));
        assert(target.create_directory(std::string(255,'a')+"/"+std::string(255,'b'))==bb::Error::too_large&&fs::is_empty(root));
        std::string deep="a";for(unsigned i=0;i<32;++i)deep+="/a";
        assert(target.create_directory(deep)==bb::Error::too_large&&fs::is_empty(root));
        assert(target.create_directory("backup/a/b")==bb::Error::ok);
        assert(target.create_directory("backup\\a/b")==bb::Error::ok);
        assert(fs::is_directory(root/"directories/backup/a/b"));
        const auto cb=target.target();assert(cb.begin(cb.context,"source","backup/file")==bb::Error::ok);
        assert(target.create_directory("busy")==bb::Error::busy);assert(cb.finish(cb.context,false)==bb::Error::ok);
        std::ofstream(root/"directories/regular")<<"preserve";
        assert(target.create_directory("regular/child")==bb::Error::io_error);
        const auto preserved=read(root/"directories/regular");assert(std::string(preserved.begin(),preserved.end())=="preserve");
        fs::create_directory_symlink(parent,root/"directories/link");
        assert(target.create_directory("link/outside")==bb::Error::io_error&&!fs::exists(parent/"outside"));
        // A complete 00..ff shard tree must fit alongside namespace/device parents.
        for(unsigned i=0;i<256;++i){
            char shard[32];snprintf(shard,sizeof(shard),"device/%02x",i);
            assert(target.create_directory(shard)==bb::Error::ok);
            assert(fs::is_directory(root/"directories"/shard));
        }
        // Four original directories + device parent + 256 shards = 261.
        // Existing paths do not consume quota, even once the cap is reached.
        assert(target.create_directory("device/ff")==bb::Error::ok);
        for(unsigned i=261;i<bb::StagedUploadTarget::max_created_directories;++i)
            assert(target.create_directory("dir"+std::to_string(i))==bb::Error::ok);
        assert(target.create_directory("overflow")==bb::Error::too_large&&!fs::exists(root/"directories/overflow"));
        assert(target.create_directory("backup/a/b")==bb::Error::ok);
    }
    {
        bb::StagedUploadTarget target;assert(target.open_session(temp)==bb::Error::ok);const fs::path root(target.session_path());
        fs::create_directory_symlink(parent,root/"directories");
        assert(target.create_directory("outside")==bb::Error::io_error&&!fs::exists(parent/"outside"));
    }
    std::cout<<"PASS: confined directory namespace, validation before mutation, duplicates, busy, collisions, symlinks and quota\n";
    fs::remove_all(parent);
    std::cout<<"PASS: destruction cleans pending data, missing parent and symlink parent rejected\n";
}
