#include "hcc_backup_storage.hpp"
#include <cerrno>
#include <cstring>
#include <dirent.h>
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
namespace {
// `NNNNNN.ready` committed-file record (exactly six digits + ".ready").
bool ready_record(const char* name){
    if(std::strlen(name)!=12)return false;
    for(int i=0;i<6;++i)if(name[i]<'0'||name[i]>'9')return false;
    return std::strcmp(name+6,".ready")==0;
}
// Real directory that is NOT a symlink. ESP FAT VFS has no symlinks; the mount
// ownership contract is the caller's. On host lstat rejects a planted symlink.
bool plain_directory(const char* path){
    struct stat s{};
#ifdef ESP_PLATFORM
    return stat(path,&s)==0&&S_ISDIR(s.st_mode);
#else
    return lstat(path,&s)==0&&S_ISDIR(s.st_mode);
#endif
}
// Reads a committed record's destination path from identity.bin and stats its
// payload. Returns false (io_error) on any malformed/short metadata or symlink.
bool read_record(const char* record_dir,char* dest,std::size_t dest_cap,std::size_t& dest_len,
                 std::uint64_t& size,std::int64_t& mtime){
    char path[320];
    if(snprintf(path,sizeof(path),"%s/identity.bin",record_dir)<=0)return false;
#ifndef ESP_PLATFORM
    struct stat ls{}; if(lstat(path,&ls)!=0||!S_ISREG(ls.st_mode))return false;
#endif
    FILE* f=std::fopen(path,"rb"); if(!f)return false;
    std::uint8_t head[12]; bool ok=std::fread(head,1,12,f)==12&&std::memcmp(head,"HCCU0001",8)==0;
    std::uint32_t src_len=ok?read_be32(head+8):0;
    if(ok&&std::fseek(f,static_cast<long>(src_len),SEEK_CUR)!=0)ok=false;      // skip source metadata
    std::uint8_t dl[4]; ok=ok&&std::fread(dl,1,4,f)==4;
    const std::uint32_t d_len=ok?read_be32(dl):0;
    if(ok&&(d_len==0||d_len>dest_cap))ok=false;
    if(ok&&std::fread(dest,1,d_len,f)!=d_len)ok=false;
    std::fclose(f);
    if(!ok)return false;
    dest_len=d_len;
    if(snprintf(path,sizeof(path),"%s/payload.bin",record_dir)<=0)return false;
    struct stat ps{};
#ifdef ESP_PLATFORM
    if(stat(path,&ps)!=0||!S_ISREG(ps.st_mode))return false;
#else
    if(lstat(path,&ps)!=0||!S_ISREG(ps.st_mode))return false;
#endif
    size=static_cast<std::uint64_t>(ps.st_size); mtime=static_cast<std::int64_t>(ps.st_mtime);
    return true;
}
}
Error StagedUploadTarget::list_directory(std::string_view request,DirectoryListing& out) const {
    if(!out.entries||!out.capacity)return Error::invalid_argument;
    out.count=0;
    if(!root_[0])return Error::invalid_argument;
    // Normalize the requested prefix: '/' separators, no trailing slash. "" = root.
    char prefix[4096]; std::size_t plen=0;
    if(!request.empty()){
        if(!valid_relative_backup_path(request))return Error::invalid_argument;
        if(request.size()>=sizeof(prefix))return Error::too_large;
        for(char c:request)prefix[plen++]=(c=='\\')?'/':c;
        while(plen&&prefix[plen-1]=='/')--plen;
    }
    prefix[plen]=0;
    auto add=[&](const char* name,std::size_t nlen,EntryType type,std::uint64_t size,std::int64_t mtime)->Error{
        if(nlen==0||nlen>255)return Error::io_error;
        for(std::size_t k=0;k<out.count;++k){
            DirectoryEntry& ex=out.entries[k];
            if(ex.name_length==nlen&&std::memcmp(ex.name,name,nlen)==0){
                // Same name may legitimately recur only as a directory from both sources.
                if(ex.type==EntryType::directory&&type==EntryType::directory){
                    if(mtime>ex.modification_time)ex.modification_time=mtime;
                    return Error::ok;
                }
                return Error::io_error; // file/dir or file/file collision = ambiguous tree
            }
        }
        if(out.count>=out.capacity)return Error::too_large; // never truncate the listing
        DirectoryEntry& e=out.entries[out.count++];
        std::memcpy(e.name,name,nlen);e.name[nlen]=0;e.name_length=static_cast<std::uint16_t>(nlen);
        e.type=type;e.size=(type==EntryType::directory)?0:size;e.modification_time=mtime;
        return Error::ok;
    };
    // Map one normalized destination to its immediate child under the prefix.
    auto classify=[&](const char* norm,std::size_t nlen,std::size_t& cstart,std::size_t& clen,bool& leaf)->bool{
        std::size_t rstart;
        if(plen==0)rstart=0;
        else{
            if(nlen<=plen||norm[plen]!='/'||std::memcmp(norm,prefix,plen)!=0)return false;
            rstart=plen+1;
        }
        if(rstart>=nlen)return false; // destination equals the prefix, not a child
        std::size_t sep=rstart; while(sep<nlen&&norm[sep]!='/')++sep;
        cstart=rstart; clen=sep-rstart; leaf=(sep==nlen);
        return clen>0;
    };
    // 1) Committed files (.ready records): destination -> file leaf or implied dir.
    DIR* dir=opendir(root_); if(!dir)return Error::io_error;
    Error err=Error::ok;
    for(struct dirent* ent; err==Error::ok&&(ent=readdir(dir));){
        if(!ready_record(ent->d_name))continue; // skip .part, directories, anything internal
        char record[300];
        if(snprintf(record,sizeof(record),"%s/%s",root_,ent->d_name)<=0){err=Error::io_error;break;}
        if(!plain_directory(record)){err=Error::io_error;break;}
        char dest[4096]; std::size_t dlen=0; std::uint64_t size=0; std::int64_t mtime=0;
        if(!read_record(record,dest,sizeof(dest),dlen,size,mtime)){err=Error::io_error;break;}
        char norm[4096]; for(std::size_t i=0;i<dlen;++i)norm[i]=(dest[i]=='\\')?'/':dest[i];
        std::size_t cs=0,cl=0; bool leaf=false;
        if(!classify(norm,dlen,cs,cl,leaf))continue;
        err=leaf?add(norm+cs,cl,EntryType::regular,size,mtime)
                :add(norm+cs,cl,EntryType::directory,0,0);
    }
    closedir(dir);
    if(err!=Error::ok)return err;
    // 2) Created-directory namespace: immediate subdirectories of the prefix.
    char nsdir[4120];
    if(snprintf(nsdir,sizeof(nsdir),"%s/directories%s%s",root_,plen?"/":"",prefix)<=0)return Error::io_error;
    if(!plen){ // trim the trailing separator produced for the root case
        nsdir[std::strlen(root_)+std::strlen("/directories")]=0;
    }
    struct stat ns{};
#ifdef ESP_PLATFORM
    const int seen=stat(nsdir,&ns);
#else
    const int seen=lstat(nsdir,&ns);
#endif
    if(seen!=0){
        if(errno==ENOENT)return Error::ok; // no namespace dir: valid empty contribution
        return Error::io_error;
    }
    if(!S_ISDIR(ns.st_mode))return Error::io_error; // symlink or file where a dir is expected
    DIR* nd=opendir(nsdir); if(!nd)return Error::io_error;
    for(struct dirent* ent; (ent=readdir(nd));){
        if(std::strcmp(ent->d_name,".")==0||std::strcmp(ent->d_name,"..")==0)continue;
        char sub[4400];
        if(snprintf(sub,sizeof(sub),"%s/%s",nsdir,ent->d_name)<=0){err=Error::io_error;break;}
        struct stat ss{};
#ifdef ESP_PLATFORM
        if(stat(sub,&ss)!=0){err=Error::io_error;break;}
#else
        if(lstat(sub,&ss)!=0){err=Error::io_error;break;}
#endif
        if(!S_ISDIR(ss.st_mode)){err=Error::io_error;break;} // only directories live here
        err=add(ent->d_name,std::strlen(ent->d_name),EntryType::directory,0,static_cast<std::int64_t>(ss.st_mtime));
        if(err!=Error::ok)break;
    }
    closedir(nd);
    return err;
}
} // namespace hcc::backup
