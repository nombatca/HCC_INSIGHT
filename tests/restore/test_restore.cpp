#include "hcc_restore_source.hpp"
#include "hcc_backup_start.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unistd.h>
namespace bb=hcc::backup;
namespace fs=std::filesystem;
static constexpr char id[]="00008101-001234567890ABCD";
static void write(const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f<<s;assert(f.good());}
static bb::Error verify(bb::RestoreSampleSource& s){std::uint8_t scratch[17];for(unsigned i=0;i<10000;++i){auto e=s.preflight(scratch,sizeof(scratch));if(e!=bb::Error::would_block)return e;}std::abort();}
int main(int argc,char** argv){
    if(argc==2){
        bb::RestoreSampleSource source;assert(source.begin(argv[1],id)==bb::Error::ok);
        std::array<std::uint8_t,32768> scratch{};bb::Error e=bb::Error::would_block;
        for(unsigned i=0;i<10000&&e==bb::Error::would_block;++i)e=source.preflight(scratch.data(),scratch.size());
        assert(e==bb::Error::ok&&source.checked()==bb::kRestoreSampleCount);
        std::uint64_t total=0;
        for(const auto& item:bb::kRestoreSample){
            bb::RestoreFileSource reader;bool found=false;assert(source.lookup(std::string(id)+"/"+item.name,reader,found)==bb::Error::ok&&found);
            std::uint64_t count=0;
            for(;;){auto r=reader.source().read(reader.source().context,scratch.data(),scratch.size());assert(r.error==bb::Error::ok);count+=r.bytes;if(r.eof)break;}
            assert(count==item.size);total+=count;
        }
        assert(total==12299936);std::puts("PASS: original 297-file encrypted sample preflight and exact streaming CRC checks; no device contacted");return 0;
    }
    const std::string payload(70000,'x');const std::string meta="metadata";
    const bb::RestoreSampleEntry index[]={
        {"ab11111111111111111111111111111111111111",static_cast<std::uint32_t>(payload.size()),bb::restore_crc32(0,reinterpret_cast<const std::uint8_t*>(payload.data()),payload.size())},
        {"info.plist",8,bb::restore_crc32(0,reinterpret_cast<const std::uint8_t*>(meta.data()),meta.size())}
    };
    assert(bb::restore_crc32(0,reinterpret_cast<const std::uint8_t*>("123456789"),9)==0xcbf43926);
    char temp[]="/tmp/hcc-restore-test-XXXXXX";assert(mkdtemp(temp));fs::path root=temp;
    write(root/index[0].name,payload);write(root/index[1].name,meta);
    bb::RestoreSampleSource src(index,2);assert(src.begin(temp,id)==bb::Error::ok);
    bb::RestoreFileSource reader;bool found=true;
    assert(src.lookup(index[0].name,reader,found)==bb::Error::invalid_argument&&!found);
    assert(verify(src)==bb::Error::ok&&src.checked()==2);
    for(const auto& path:{std::string(index[0].name),std::string(id)+"/"+index[0].name,std::string(id)+"/ab/"+index[0].name}){
        assert(src.lookup(path,reader,found)==bb::Error::ok&&found);
        std::vector<std::uint8_t> bytes;std::array<std::uint8_t,8192>b{};
        for(;;){auto r=reader.source().read(reader.source().context,b.data(),b.size());assert(r.error==bb::Error::ok);bytes.insert(bytes.end(),b.data(),b.data()+r.bytes);if(r.eof)break;}
        assert(std::string(bytes.begin(),bytes.end())==payload);
    }
    assert(src.lookup(std::string(id)+"/Info.plist",reader,found)==bb::Error::ok&&found);reader.reset();
    for(auto p:{"../info.plist","/etc/passwd","a/../info.plist","ab\\info.plist"})assert(src.lookup(p,reader,found)==bb::Error::invalid_argument);
    assert(src.lookup("other-device/info.plist",reader,found)==bb::Error::ok&&!found);
    write(root/"secret.txt","not part of sample");assert(src.lookup("secret.txt",reader,found)==bb::Error::ok&&!found);
    std::array<bb::DirectoryEntry,3> rows{};bb::DirectoryListing list{rows.data(),rows.size(),0};
    assert(src.list(id,list)==bb::Error::ok&&list.count==2);
    assert(src.list(std::string(id)+"/ab",list)==bb::Error::ok&&list.count==1);
    bb::DirectoryListing tiny{rows.data(),1,0};assert(src.list("",tiny)==bb::Error::too_large);
    fs::remove(root/index[1].name);fs::create_symlink(root/"secret.txt",root/index[1].name);
    assert(src.lookup("Info.plist",reader,found)==bb::Error::io_error&&!found);
    bb::RestoreSampleSource bad(index,2);assert(bad.begin(temp,id)==bb::Error::ok);assert(verify(bad)==bb::Error::io_error);
    fs::remove(root/index[1].name);write(root/index[1].name,"metadatz");
    bb::RestoreSampleSource corrupt(index,2);assert(corrupt.begin(temp,id)==bb::Error::ok);assert(verify(corrupt)==bb::Error::io_error);
    write(root/index[1].name,meta);
    assert(src.lookup("info.plist",reader,found)==bb::Error::ok&&found);write(root/index[1].name,"metadatz");
    std::uint8_t small[8];auto changed=reader.source().read(reader.source().context,small,sizeof(small));assert(changed.error==bb::Error::io_error);reader.reset();
    std::uint8_t output[1024];std::size_t n=0;
    assert(bb::encode_sample_restore_request(id,output,sizeof(output),n)==bb::LinkError::ok);
    fs::create_directories("build");std::ofstream("build/request.bplist",std::ios::binary).write(reinterpret_cast<char*>(output),n);
    n=123;assert(bb::encode_sample_restore_request("bad",output,sizeof(output),n)==bb::LinkError::invalid_argument&&n==0);
    n=123;assert(bb::encode_sample_restore_request(id,output,12,n)==bb::LinkError::too_large&&n==0);
    fs::remove_all(root);
    std::puts("PASS: immutable restore source, bounded CRC preflight, exact 70KB reads, aliases, confinement, changes, symlinks and request encoding");
}
