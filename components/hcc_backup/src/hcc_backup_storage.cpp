#include "hcc_backup_storage.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace hcc::backup {
namespace {
bool directory(const char* path){
    struct stat s{};
#ifdef ESP_PLATFORM
    // ESP-IDF FAT VFS has no symlinks. Mount ownership is the caller's contract.
    return stat(path,&s)==0&&S_ISDIR(s.st_mode);
#else
    return lstat(path,&s)==0&&S_ISDIR(s.st_mode);
#endif
}
FILE* exclusive_file(const char* path){
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0)return nullptr;
    FILE* f=fdopen(fd,"wb");if(!f){close(fd);unlink(path);}return f;
}
bool sync_close(FILE*& f){
    if(!f)return true;
    bool ok=fflush(f)==0;if(ok)ok=fsync(fileno(f))==0;
    if(fclose(f)!=0)ok=false;
    f=nullptr;return ok;
}
bool child(char* out,std::size_t capacity,const char* parent,const char* name){
    const int n=snprintf(out,capacity,"%s/%s",parent,name);return n>0&&static_cast<std::size_t>(n)<capacity;
}
}
StagedUploadTarget::~StagedUploadTarget(){if(active_)finish(false);}
Error StagedUploadTarget::open_session(const char* parent){
    if(root_[0]||active_)return Error::busy;
    if(!parent||!parent[0]||std::strlen(parent)>230)return Error::invalid_argument;
    if(!directory(parent))return Error::io_error;
    for(unsigned i=1;i<=10000;++i){
        char path[256];const int n=snprintf(path,sizeof(path),"%s/rx_%06u",parent,i);
        if(n<0||static_cast<std::size_t>(n)>=sizeof(path))return Error::too_large;
        if(mkdir(path,0700)==0){std::memcpy(root_,path,static_cast<std::size_t>(n)+1);return Error::ok;}
        if(errno!=EEXIST)return Error::io_error;
    }
    return Error::too_large;
}
Error StagedUploadTarget::create_directory(std::string_view relative){
    if(active_)return Error::busy;
    if(!root_[0]||!valid_relative_backup_path(relative))return Error::invalid_argument;
    // Validate the WHOLE path before any mutation; normalize both separators.
    std::size_t start=0,depth=0;
    for(std::size_t i=0;i<=relative.size();++i){
        if(i<relative.size()&&relative[i]!='/'&&relative[i]!='\\'){
            const auto c=static_cast<unsigned char>(relative[i]);
            if(c==127||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return Error::invalid_argument;
            continue;
        }
        const auto part=relative.substr(start,i-start);
        if(part.size()>255||++depth>32)return Error::too_large;
        if(part.back()=='.'||part.back()==' ')return Error::invalid_argument;
        start=i+1;
    }
    char path[512];
    if(!child(path,sizeof(path),root_,"directories"))return Error::too_large;
    const auto base=std::strlen(path);
    if(relative.size()+base+1>=sizeof(path))return Error::too_large;
    if(!directory(root_))return Error::io_error;
    auto ensure=[&](){
        if(directory(path))return Error::ok;
        // Reject regular files, symlinks, and all lookup errors except absence.
        struct stat existing{};
#ifdef ESP_PLATFORM
        const auto found=stat(path,&existing);
#else
        const auto found=lstat(path,&existing);
#endif
        if(found==0||errno!=ENOENT)return Error::io_error;
        if(directories_>=256)return Error::too_large;
        if(mkdir(path,0700)!=0)return Error::io_error;
        ++directories_;return Error::ok;
    };
    auto result=ensure();if(result!=Error::ok)return result;
    auto length=base;path[length++]='/';
    for(std::size_t i=0;i<relative.size();++i){
        path[length++]=(relative[i]=='\\')?'/':relative[i];
        if(i+1==relative.size()||relative[i+1]=='/'||relative[i+1]=='\\'){
            path[length]=0;result=ensure();if(result!=Error::ok)return result;
        }
    }
    return Error::ok;
}
Error StagedUploadTarget::begin_callback(void* p,std::string_view s,std::string_view d){return static_cast<StagedUploadTarget*>(p)->begin(s,d);}
IoResult StagedUploadTarget::write_callback(void* p,const std::uint8_t* bytes,std::size_t n){
    auto& self=*static_cast<StagedUploadTarget*>(p);
    if(!self.active_||!self.payload_||(n&&!bytes))return {0,IoState::error};
    const auto written=fwrite(bytes,1,n,self.payload_);
    return {written,written==n&&!ferror(self.payload_)?IoState::ok:IoState::error};
}
Error StagedUploadTarget::finish_callback(void* p,bool commit){return static_cast<StagedUploadTarget*>(p)->finish(commit);}
Error StagedUploadTarget::begin(std::string_view source,std::string_view destination){
    if(active_)return Error::busy;
    if(!root_[0]||source.empty()||source.size()>4096||!valid_relative_backup_path(destination))return Error::invalid_argument;
    for(char c:source)if(static_cast<unsigned char>(c)<32)return Error::invalid_argument;
    if(sequence_>=999999)return Error::too_large;
    ++sequence_;
    snprintf(stage_,sizeof(stage_),"%s/%06u.part",root_,static_cast<unsigned>(sequence_));
    snprintf(ready_,sizeof(ready_),"%s/%06u.ready",root_,static_cast<unsigned>(sequence_));
    if(mkdir(stage_,0700)!=0)return Error::io_error;
    active_=true;char path[320];
    if(!child(path,sizeof(path),stage_,"identity.bin")){cleanup();return Error::too_large;}
    FILE* meta=exclusive_file(path);
    if(!meta){cleanup();return Error::io_error;}
    const char magic[]="HCCU0001";std::uint8_t size[4];
    bool ok=fwrite(magic,1,8,meta)==8;
    write_be32(size,static_cast<std::uint32_t>(source.size()));
    ok=ok&&fwrite(size,1,4,meta)==4&&fwrite(source.data(),1,source.size(),meta)==source.size();
    write_be32(size,static_cast<std::uint32_t>(destination.size()));
    ok=ok&&fwrite(size,1,4,meta)==4&&fwrite(destination.data(),1,destination.size(),meta)==destination.size();
    const bool closed=sync_close(meta);
    if(!ok||!closed){cleanup();return Error::io_error;}
    if(!child(path,sizeof(path),stage_,"payload.bin")){cleanup();return Error::too_large;}
    payload_=exclusive_file(path);
    if(!payload_){cleanup();return Error::io_error;}
    return Error::ok;
}
Error StagedUploadTarget::cleanup(){
    bool ok=true;if(payload_){if(fclose(payload_)!=0)ok=false;payload_=nullptr;}
    char path[320];
    for(const char* name:{"payload.bin","identity.bin"}){
        if(!child(path,sizeof(path),stage_,name)){ok=false;continue;}
        if(unlink(path)!=0&&errno!=ENOENT)ok=false;
    }
    if(rmdir(stage_)!=0&&errno!=ENOENT)ok=false;
    active_=false;return ok?Error::ok:Error::io_error;
}
Error StagedUploadTarget::finish(bool commit){
    if(!active_)return Error::invalid_argument;
    if(!commit)return cleanup();
    if(!sync_close(payload_)){cleanup();return Error::io_error;}
    struct stat existing{};
    // This directory is private to this target; concurrent mutation is forbidden.
    // Never overwrite a previous published directory, including an empty one.
    if(stat(ready_,&existing)==0||errno!=ENOENT){cleanup();return Error::io_error;}
    if(rename(stage_,ready_)!=0){cleanup();return Error::io_error;}
    active_=false;++committed_;return Error::ok;
}
} // namespace hcc::backup
