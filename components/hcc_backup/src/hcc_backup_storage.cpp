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
StagedUploadTarget::~StagedUploadTarget(){if(move_scan_)closedir(static_cast<DIR*>(move_scan_));if(active_)finish(false);}
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
        if(directories_>=max_created_directories)return Error::too_large;
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
    active_=false;++committed_;++revision_;return Error::ok;
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
    if(ok&&(src_len==0||src_len>4096))ok=false;
    if(ok&&std::fseek(f,static_cast<long>(src_len),SEEK_CUR)!=0)ok=false;      // skip source metadata
    std::uint8_t dl[4]; ok=ok&&std::fread(dl,1,4,f)==4;
    const std::uint32_t d_len=ok?read_be32(dl):0;
    if(ok&&(d_len==0||d_len>dest_cap))ok=false;
    if(ok&&std::fread(dest,1,d_len,f)!=d_len)ok=false;
    if(ok&&(!valid_relative_backup_path({dest,d_len})||std::fgetc(f)!=EOF||std::ferror(f)))ok=false;
    if(std::fclose(f)!=0)ok=false;
    if(!ok)return false;
    dest_len=d_len;
    if(snprintf(path,sizeof(path),"%s/payload.bin",record_dir)<=0)return false;
    struct stat ps{};
#ifdef ESP_PLATFORM
    if(stat(path,&ps)!=0||!S_ISREG(ps.st_mode))return false;
#else
    if(lstat(path,&ps)!=0||!S_ISREG(ps.st_mode))return false;
#endif
    if(ps.st_size<0)return false;
    size=static_cast<std::uint64_t>(ps.st_size); mtime=static_cast<std::int64_t>(ps.st_mtime);
    return true;
}
}
Error StagedUploadTarget::list_directory(std::string_view request,DirectoryListing& out,ListingScratch& scratch) const {
    if(!out.entries||!out.capacity)return Error::invalid_argument;
    out.count=0;
    if(!root_[0])return Error::invalid_argument;
    if(!plain_directory(root_))return Error::io_error;
    // Normalize the requested prefix: '/' separators, no trailing slash. "" = root.
    char* prefix=scratch.prefix; std::size_t plen=0;
    if(!request.empty()){
        if(!valid_relative_backup_path(request))return Error::invalid_argument;
        if(request.size()>=sizeof(scratch.prefix))return Error::too_large;
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
        char* record=scratch.record;
        if(snprintf(record,sizeof(scratch.record),"%s/%s",root_,ent->d_name)<=0){err=Error::io_error;break;}
        if(!plain_directory(record)){err=Error::io_error;break;}
        char* norm=scratch.path; std::size_t dlen=0; std::uint64_t size=0; std::int64_t mtime=0;
        if(!read_record(record,norm,sizeof(scratch.path),dlen,size,mtime)){err=Error::io_error;break;}
        for(std::size_t i=0;i<dlen;++i)if(norm[i]=='\\')norm[i]='/'; // normalize in place
        std::size_t cs=0,cl=0; bool leaf=false;
        if(!classify(norm,dlen,cs,cl,leaf))continue;
        err=leaf?add(norm+cs,cl,EntryType::regular,size,mtime)
                :add(norm+cs,cl,EntryType::directory,0,0);
    }
    closedir(dir);
    if(err!=Error::ok)return err;
    // 2) Created-directory namespace: immediate subdirectories of the prefix.
    char* nsdir=scratch.nsdir;
    if(snprintf(nsdir,sizeof(scratch.nsdir),"%s/directories%s%s",root_,plen?"/":"",prefix)<=0)return Error::io_error;
    if(!plen){ // trim the trailing separator produced for the root case
        nsdir[std::strlen(root_)+std::strlen("/directories")]=0;
    }
    // Check every ancestor before opendir: lstat on the final path alone would
    // follow a symlink planted in an intermediate namespace component.
    const auto root_length=std::strlen(root_);
    for(std::size_t i=root_length+1;;++i){
        if(nsdir[i]!='/'&&nsdir[i]!=0)continue;
        const auto saved=nsdir[i];nsdir[i]=0;
        struct stat ancestor{};
#ifdef ESP_PLATFORM
        const auto found=stat(nsdir,&ancestor);
#else
        const auto found=lstat(nsdir,&ancestor);
#endif
        const auto lookup_error=errno;nsdir[i]=saved;
        if(found!=0)return lookup_error==ENOENT?Error::ok:Error::io_error;
        if(!S_ISDIR(ancestor.st_mode))return Error::io_error;
        if(!saved)break;
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
        char* sub=scratch.sub;
        if(snprintf(sub,sizeof(scratch.sub),"%s/%s",nsdir,ent->d_name)<=0){err=Error::io_error;break;}
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

namespace hcc::backup {
Error StagedUploadTarget::prepare_move_index(ListingScratch& scratch){
    if(scratch.move_ready&&scratch.move_revision==revision_&&std::strcmp(scratch.move_owner,root_)==0)return Error::ok;
    if(!move_scan_){
        scratch.remove_active=false;scratch.move_ready=false;scratch.move_count=0;scratch.move_used=0;
        if(!plain_directory(root_))return Error::io_error;
        move_scan_=opendir(root_);if(!move_scan_)return Error::io_error;
        std::strcpy(scratch.move_owner,root_);++scratch.move_builds;
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] MoveItems index starting\n");
#endif
    }
    auto stop=[&](Error error){closedir(static_cast<DIR*>(move_scan_));move_scan_=nullptr;return error;};
    for(unsigned step=0;step<8;++step){
        errno=0;auto* ent=readdir(static_cast<DIR*>(move_scan_));
        if(!ent){
            if(errno)return stop(Error::io_error);
            if(closedir(static_cast<DIR*>(move_scan_))!=0){move_scan_=nullptr;return Error::io_error;}
            move_scan_=nullptr;scratch.move_ready=true;scratch.move_revision=revision_;
#ifdef ESP_PLATFORM
            printf("[HCC BACKUP] MoveItems index ready records=%u path_bytes=%u\n",scratch.move_count,scratch.move_used);
#endif
            return Error::ok;
        }
        if(!ready_record(ent->d_name))continue;
        if(scratch.move_count>=1024)return stop(Error::too_large);
        if(!child(scratch.record,sizeof(scratch.record),root_,ent->d_name)||!plain_directory(scratch.record))return stop(Error::io_error);
        std::size_t length=0;std::uint64_t size=0;std::int64_t time=0;
        if(!read_record(scratch.record,scratch.path,sizeof(scratch.path),length,size,time))return stop(Error::io_error);
        if(length>sizeof(scratch.move_paths)-scratch.move_used)return stop(Error::too_large);
        unsigned record=0;for(unsigned i=0;i<6;++i)record=record*10+static_cast<unsigned>(ent->d_name[i]-'0');
        auto& item=scratch.move_entries[scratch.move_count++];item={scratch.move_used,static_cast<std::uint32_t>(length),record};
        std::memcpy(scratch.move_paths+scratch.move_used,scratch.path,length);scratch.move_used+=length;
    }
    return Error::would_block;
}
Error StagedUploadTarget::move_file(std::string_view from,std::string_view to,ListingScratch& scratch){
    if(active_)return Error::busy;
    if(!root_[0]||!valid_relative_backup_path(from)||!valid_relative_backup_path(to))return Error::invalid_argument;
    if(from.size()>=sizeof(scratch.path)||to.size()>=sizeof(scratch.prefix))return Error::too_large;
    // Compare both separator styles; no untrusted logical path is opened on disk.
    auto equal=[](std::string_view a,std::string_view b){
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i)if((a[i]=='\\'?'/':a[i])!=(b[i]=='\\'?'/':b[i]))return false;
        return true;
    };
    const auto indexed=prepare_move_index(scratch);if(indexed!=Error::ok)return indexed;
    char* source=scratch.record;source[0]=0;std::uint32_t source_index=0;bool collision=false;
    for(std::uint32_t i=0;i<scratch.move_count;++i){
        const auto& entry=scratch.move_entries[i];if(!entry.length)continue;const std::string_view dest(scratch.move_paths+entry.offset,entry.length);
        if(equal(dest,from)){
            if(source[0])return Error::io_error;
            const int n=std::snprintf(source,sizeof(scratch.record),"%s/%06u.ready",root_,entry.record);
            if(n<=0||static_cast<std::size_t>(n)>=sizeof(scratch.record))return Error::too_large;
            source_index=i;
        }
        if(equal(dest,to)&&!equal(from,to))collision=true;
        if(dest.size()>to.size()&&equal(dest.substr(0,to.size()),to)&&(dest[to.size()]=='/'||dest[to.size()]=='\\'))collision=true;
    }
    if(!source[0]||collision)return Error::io_error;
    std::size_t verified=0;std::uint64_t payload_size=0;std::int64_t modified=0;
    if(!plain_directory(source)||!read_record(source,scratch.path,sizeof(scratch.path),verified,payload_size,modified)||!equal({scratch.path,verified},from))return Error::io_error;
    if(equal(from,to))return Error::ok;
    if(to.size()>sizeof(scratch.move_paths)-scratch.move_used)return Error::too_large;
    // Reject collisions with explicit namespace directories, following no links.
    if(to.size()+std::strlen(root_)+14>=sizeof(scratch.nsdir))return Error::too_large;
    std::snprintf(scratch.nsdir,sizeof(scratch.nsdir),"%s/directories/",root_);
    auto used=std::strlen(scratch.nsdir);
    for(char c:to)scratch.nsdir[used++]=c=='\\'?'/':c;
    scratch.nsdir[used]=0;
    for(std::size_t i=std::strlen(root_)+1;i<=used;++i){
        if(scratch.nsdir[i]!='/'&&i!=used)continue;
        const auto save=scratch.nsdir[i];scratch.nsdir[i]=0;struct stat st{};
#ifdef ESP_PLATFORM
        const int found=stat(scratch.nsdir,&st);
#else
        const int found=lstat(scratch.nsdir,&st);
#endif
        const int why=errno;scratch.nsdir[i]=save;
        if(found==0&&(!S_ISDIR(st.st_mode)||i==used))return Error::io_error;
        if(found!=0){if(why!=ENOENT)return Error::io_error;break;}
    }
    char* original=scratch.nsdir;char* pending=scratch.sub;char* previous=scratch.prefix;
    if(!child(original,sizeof(scratch.nsdir),source,"identity.bin")||!child(pending,sizeof(scratch.sub),source,"identity.move")||!child(previous,sizeof(scratch.prefix),source,"identity.previous"))return Error::too_large;
    struct stat st{};
#ifdef ESP_PLATFORM
    const int prior=stat(previous,&st);
#else
    const int prior=lstat(previous,&st);
#endif
    if(prior==0||errno!=ENOENT)return Error::io_error;
    FILE* in=std::fopen(original,"rb");if(!in)return Error::io_error;
    // read_record already validated the complete original under exclusive ownership.
    std::uint8_t head[12];bool ok=std::fread(head,1,12,in)==12;
    const auto n=ok?read_be32(head+8):0;
    ok=ok&&n>0&&n<=sizeof(scratch.path)&&std::fread(scratch.path,1,n,in)==n;
    if(std::fclose(in)!=0)ok=false;
    if(!ok)return Error::io_error;
    FILE* out=exclusive_file(pending);if(!out)return Error::io_error;
    std::uint8_t size[4];write_be32(size,static_cast<std::uint32_t>(to.size()));
    ok=std::fwrite(head,1,12,out)==12&&std::fwrite(scratch.path,1,n,out)==n&&std::fwrite(size,1,4,out)==4&&std::fwrite(to.data(),1,to.size(),out)==to.size();
    const bool synced=sync_close(out);
    if(!ok||!synced){unlink(pending);return Error::io_error;}
    if(rename(original,previous)!=0){unlink(pending);return Error::io_error;}
    if(rename(pending,original)!=0){rename(previous,original);unlink(pending);return Error::io_error;}
    // Keep old identity as a recovery record; future moves are deliberately refused
    // until it is removed after the successful swap.
    if(unlink(previous)!=0){scratch.move_ready=false;return Error::io_error;}
    auto& entry=scratch.move_entries[source_index];entry.offset=scratch.move_used;entry.length=static_cast<std::uint32_t>(to.size());
    std::memcpy(scratch.move_paths+scratch.move_used,to.data(),to.size());scratch.move_used+=to.size();
    ++revision_;scratch.move_revision=revision_;
    return Error::ok;
}
} // namespace hcc::backup

namespace hcc::backup {
Error StagedUploadTarget::remove_item(std::string_view path,ListingScratch& scratch){
    if(active_)return Error::busy;
    if(!root_[0]||!valid_relative_backup_path(path))return Error::invalid_argument;
    if(path.size()>=sizeof(scratch.path))return Error::too_large;
    // Namespace paths must also be unambiguous on FAT (no trailing dot/space).
    std::size_t component=0;
    for(std::size_t i=0;i<=path.size();++i){
        if(i<path.size()&&path[i]!='/'&&path[i]!='\\'){
            const auto c=static_cast<unsigned char>(path[i]);
            if(c==127||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return Error::invalid_argument;
            continue;
        }
        if(i-component>255||path[i-1]=='.'||path[i-1]==' ')return Error::invalid_argument;
        component=i+1;
    }
    const auto indexed=prepare_move_index(scratch);if(indexed!=Error::ok)return indexed;
    auto equal=[](std::string_view a,std::string_view b){
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i)if((a[i]=='\\'?'/':a[i])!=(b[i]=='\\'?'/':b[i]))return false;
        return true;
    };
    if(scratch.remove_active&&!equal(path,scratch.remove_path))return Error::busy;
    auto fail=[&](const char* reason){
        scratch.remove_active=false;
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] RemoveItems failed reason=%s errno=%d\n",reason,errno);
#else
        (void)reason;
#endif
        return Error::io_error;
    };
    std::uint32_t found=UINT32_MAX,child_index=UINT32_MAX;
    for(std::uint32_t i=0;i<scratch.move_count;++i){
        const auto& item=scratch.move_entries[i];if(!item.length)continue;
        const std::string_view dest(scratch.move_paths+item.offset,item.length);
        if(equal(dest,path)){if(found!=UINT32_MAX)return fail("ambiguous");found=i;}
        if(dest.size()>path.size()&&equal(dest.substr(0,path.size()),path)&&(dest[path.size()]=='/'||dest[path.size()]=='\\'))child_index=i;
    }
    const int count=std::snprintf(scratch.nsdir,sizeof(scratch.nsdir),"%s/directories/",root_);
    if(count<=0||static_cast<std::size_t>(count)+path.size()>=sizeof(scratch.nsdir))return Error::too_large;
    auto used=static_cast<std::size_t>(count);
    for(char c:path)scratch.nsdir[used++]=c=='\\'?'/':c;
    scratch.nsdir[used]=0;bool explicit_dir=false;
    for(std::size_t i=std::strlen(root_)+1;i<=used;++i){
        if(scratch.nsdir[i]!='/'&&i!=used)continue;
        const auto saved=scratch.nsdir[i];scratch.nsdir[i]=0;struct stat st{};
#ifdef ESP_PLATFORM
        const int seen=stat(scratch.nsdir,&st);
#else
        const int seen=lstat(scratch.nsdir,&st);
#endif
        const int why=errno;scratch.nsdir[i]=saved;
        if(seen!=0){if(why!=ENOENT)return fail("namespace_lookup");break;}
        if(!S_ISDIR(st.st_mode))return fail("namespace_type");
        if(i==used)explicit_dir=true;
    }
    if(found!=UINT32_MAX&&(explicit_dir||child_index!=UINT32_MAX))return fail("file_directory_collision");
    const bool subtree=child_index!=UINT32_MAX||scratch.remove_active;
    if(subtree&&!scratch.remove_active){
        std::memcpy(scratch.remove_path,path.data(),path.size());scratch.remove_path[path.size()]=0;
        scratch.remove_active=true;scratch.remove_done=0;
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] RemoveItems subtree starting\n");
#endif
    }
    const auto record_index=found!=UINT32_MAX?found:child_index;
    if(record_index!=UINT32_MAX){
        auto& item=scratch.move_entries[record_index];
        const std::string_view expected(scratch.move_paths+item.offset,item.length);
        std::snprintf(scratch.record,sizeof(scratch.record),"%s/%06u.ready",root_,item.record);
        std::size_t length=0;std::uint64_t size=0;std::int64_t time=0;
        if(!plain_directory(scratch.record)||!read_record(scratch.record,scratch.path,sizeof(scratch.path),length,size,time)||!equal({scratch.path,length},expected))return fail("record_validation");
        std::snprintf(scratch.sub,sizeof(scratch.sub),"%s/%06u.removed",root_,item.record);
        struct stat prior{};
#ifdef ESP_PLATFORM
        const int exists=stat(scratch.sub,&prior);
#else
        const int exists=lstat(scratch.sub,&prior);
#endif
        if(exists==0||errno!=ENOENT)return fail("recovery_collision");
        if(rename(scratch.record,scratch.sub)!=0)return fail("record_rename");
        item.length=0;++revision_;scratch.move_revision=revision_;
#ifdef ESP_PLATFORM
        if(subtree&&++scratch.remove_done%8==0)printf("[HCC BACKUP] RemoveItems subtree archived=%u\n",scratch.remove_done);
#else
        if(subtree)++scratch.remove_done;
#endif
        // Return control after each file; keep request pending until whole subtree ends.
        return subtree?Error::would_block:Error::ok;
    }
    if(explicit_dir){
        // Archive the whole namespace tree, including empty descendants. No walk,
        // recursive unlink or traversal into namespace children is performed.
        const auto number=++removed_directories_;
        std::snprintf(scratch.sub,sizeof(scratch.sub),"%s/removed_directory_%06u",root_,number);
        std::snprintf(scratch.record,sizeof(scratch.record),"%s/removed_directory_%06u.path",root_,number);
        struct stat prior{};
#ifdef ESP_PLATFORM
        const int exists=stat(scratch.sub,&prior);
#else
        const int exists=lstat(scratch.sub,&prior);
#endif
        if(exists==0||errno!=ENOENT)return fail("directory_recovery_collision");
        FILE* mapping=exclusive_file(scratch.record);if(!mapping)return fail("directory_mapping_open");
        const bool wrote=std::fwrite(path.data(),1,path.size(),mapping)==path.size();
        const bool synced=sync_close(mapping);
        if(!wrote||!synced){unlink(scratch.record);return fail("directory_mapping_write");}
        if(rename(scratch.nsdir,scratch.sub)!=0){const auto why=errno;unlink(scratch.record);errno=why;return fail("directory_rename");}
    }else if(!scratch.remove_active){
        // idevicebackup2 ignores ENOENT for RemoveItems: an old index or
        // previously removed item already satisfies the requested final state.
        // Only reach this after the record index and namespace lookup validated.
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] RemoveItems already_absent result=ok\n");
#endif
        return Error::ok;
    }
    scratch.remove_active=false;++revision_;scratch.move_revision=revision_;
    return Error::ok;
}
} // namespace hcc::backup

namespace hcc::backup {
void CapturedFileSource::reset(){
    if(file_)std::fclose(file_);
    file_=nullptr;path_[0]=0;expected_=remaining_=0;done_=failed_=false;
}
Error CapturedFileSource::bind(const char* record,std::uint64_t size){
    reset();const int n=std::snprintf(path_,sizeof(path_),"%s/payload.bin",record);
    if(n<=0||static_cast<std::size_t>(n)>=sizeof(path_)){reset();return Error::too_large;}
    expected_=remaining_=size;return Error::ok;
}
SourceResult CapturedFileSource::read_callback(void* context,std::uint8_t* bytes,std::size_t capacity){
    auto& self=*static_cast<CapturedFileSource*>(context);
    auto error=[&](Error e){if(self.file_)std::fclose(self.file_);self.file_=nullptr;self.failed_=true;return SourceResult{0,false,e};};
    if(self.failed_)return {0,false,Error::io_error};
    if(!bytes||!capacity||!self.path_[0])return error(Error::invalid_argument);
    if(self.done_)return {0,true,Error::ok};
    if(!self.file_){
        auto* slash=std::strrchr(self.path_,'/');if(!slash)return error(Error::invalid_argument);
        *slash=0;const bool parent=plain_directory(self.path_);*slash='/';
        struct stat st{};
#ifdef ESP_PLATFORM
        const int seen=stat(self.path_,&st);
#else
        const int seen=lstat(self.path_,&st);
#endif
        if(!parent||seen!=0||!S_ISREG(st.st_mode)||st.st_size<0||static_cast<std::uint64_t>(st.st_size)!=self.expected_)return error(Error::io_error);
        self.file_=std::fopen(self.path_,"rb");if(!self.file_)return error(Error::io_error);
    }
    const auto take=self.remaining_<capacity?static_cast<std::size_t>(self.remaining_):capacity;
    const auto got=take?std::fread(bytes,1,take,self.file_):0;
    if(std::ferror(self.file_))return error(Error::io_error);
    if(got!=take)return error(Error::truncated);
    self.remaining_-=got;
    if(!self.remaining_){
        const bool extra=std::fgetc(self.file_)!=EOF||std::ferror(self.file_);
        const int closed=std::fclose(self.file_);self.file_=nullptr;
        if(extra||closed!=0)return error(Error::io_error);
        self.done_=true;
    }
    return {got,self.done_,Error::ok};
}
Error StagedUploadTarget::lookup_download(std::string_view path,ListingScratch& scratch,CapturedFileSource& out,bool& found){
    out.reset();found=false;
    if(active_)return Error::busy;
    if(!root_[0]||!valid_relative_backup_path(path))return Error::invalid_argument;
    const auto indexed=prepare_move_index(scratch);if(indexed!=Error::ok)return indexed;
    if(scratch.remove_active)return Error::busy;
    auto io_failure=[&](const char* stage,int why=0){
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] DownloadFiles storage stage=%s errno=%d\n",stage,why);
#else
        (void)stage;(void)why;
#endif
        return Error::io_error;
    };
    auto equal=[](std::string_view a,std::string_view b){
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i)if((a[i]=='\\'?'/':a[i])!=(b[i]=='\\'?'/':b[i]))return false;
        return true;
    };
    std::uint32_t record=UINT32_MAX,matches=0;
    for(std::uint32_t i=0;i<scratch.move_count;++i){
        const auto& item=scratch.move_entries[i];if(!item.length)continue;
        const std::string_view dest(scratch.move_paths+item.offset,item.length);
        if(equal(dest,path)){
            ++matches;
            // Every committed upload has a monotonically assigned record ID.
            // Repeated uploads of a destination are versions: serve the newest.
            if(record==UINT32_MAX||item.record>record)record=item.record;
        }
        if(dest.size()>path.size()&&equal(dest.substr(0,path.size()),path)&&(dest[path.size()]=='/'||dest[path.size()]=='\\'))return io_failure("logical_directory");
    }
#ifdef ESP_PLATFORM
    if(matches>1)printf("[HCC BACKUP] DownloadFiles versions=%u selected_record=%u\n",matches,record);
#else
    (void)matches;
#endif
    const int n=std::snprintf(scratch.nsdir,sizeof(scratch.nsdir),"%s/directories/",root_);
    if(n<=0||static_cast<std::size_t>(n)+path.size()>=sizeof(scratch.nsdir))return Error::too_large;
    auto used=static_cast<std::size_t>(n);for(char c:path)scratch.nsdir[used++]=c=='\\'?'/':c;scratch.nsdir[used]=0;
    for(std::size_t i=std::strlen(root_)+1;i<=used;++i){
        if(scratch.nsdir[i]!='/'&&i!=used)continue;
        const auto saved=scratch.nsdir[i];scratch.nsdir[i]=0;struct stat st{};
#ifdef ESP_PLATFORM
        const int seen=stat(scratch.nsdir,&st);
#else
        const int seen=lstat(scratch.nsdir,&st);
#endif
        const int why=errno;scratch.nsdir[i]=saved;
        if(seen!=0){if(why!=ENOENT)return io_failure("namespace_lookup",why);break;}
        if(!S_ISDIR(st.st_mode))return io_failure("namespace_type");
        if(i==used)return io_failure("namespace_directory");
    }
    if(record==UINT32_MAX)return Error::ok;
    std::snprintf(scratch.record,sizeof(scratch.record),"%s/%06u.ready",root_,record);
    std::size_t length=0;std::uint64_t size=0;std::int64_t time=0;
    if(!plain_directory(scratch.record))return io_failure("record_directory",errno);
    if(!read_record(scratch.record,scratch.path,sizeof(scratch.path),length,size,time))return io_failure("record_metadata_or_payload",errno);
    if(!equal({scratch.path,length},path))return io_failure("identity_changed");
    const auto bound=out.bind(scratch.record,size);if(bound!=Error::ok)return bound;
    found=true;return Error::ok;
}
}
