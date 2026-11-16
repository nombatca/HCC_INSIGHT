#include "hcc_restore_source.hpp"
#include <algorithm>
#include <cstring>
#include <sys/stat.h>
namespace hcc::backup {
namespace {
bool trusted_directory(const char*);
bool regular(const char* p,std::uint64_t size){
    char parent[352];const auto n=std::strlen(p);if(n>=sizeof(parent))return false;
    std::memcpy(parent,p,n+1);auto* slash=std::strrchr(parent,'/');if(!slash)return false;
    *slash=0;if(!trusted_directory(parent))return false;
    struct stat s{};
#ifdef ESP_PLATFORM
    const int result=stat(p,&s); // FAT has no symlinks; caller owns the mount.
#else
    const int result=lstat(p,&s);
#endif
    return result==0&&S_ISREG(s.st_mode)&&s.st_size>=0&&static_cast<std::uint64_t>(s.st_size)==size;
}
bool trusted_directory(const char* path){
    char copy[352];const auto len=std::strlen(path);if(!len||len>=sizeof(copy))return false;
    std::memcpy(copy,path,len+1);
    for(std::size_t i=1;i<=len;++i){
        if(copy[i]&&copy[i]!='/')continue;
        const char save=copy[i];copy[i]=0;struct stat st{};
#ifdef ESP_PLATFORM
        const int r=stat(copy,&st);
#else
        const int r=lstat(copy,&st);
#endif
        copy[i]=save;if(r||!S_ISDIR(st.st_mode))return false;
    }
    return true;
}
bool join(char* out,std::size_t cap,const char* root,const char* name){
    const int n=std::snprintf(out,cap,"%s/%s",root,name);return n>0&&static_cast<std::size_t>(n)<cap;
}
bool shard(std::string_view s){
    if(s.size()!=2)return false;
    for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}
}
std::uint32_t restore_crc32(std::uint32_t crc,const std::uint8_t* b,std::size_t n){
    crc=~crc;
    for(std::size_t i=0;i<n;++i){crc^=b[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&-(crc&1u));}
    return ~crc;
}
void RestoreFileSource::reset(){if(file_)std::fclose(file_);file_=nullptr;path_[0]=0;remaining_=0;done_=false;failed_=false;crc_=0;}
Error RestoreFileSource::bind(const char* root,const RestoreSampleEntry& item){
    reset();if(!join(path_,sizeof(path_),root,item.name)||!regular(path_,item.size))return Error::io_error;
    remaining_=item.size;expected_=item.crc;return Error::ok;
}
SourceResult RestoreFileSource::read(void* ctx,std::uint8_t* b,std::size_t n){
    auto& s=*static_cast<RestoreFileSource*>(ctx);
    if(s.failed_)return {0,false,Error::io_error};
    if(s.done_)return {0,true,Error::ok};
    if(!b||!n||!s.path_[0])return {0,false,Error::invalid_argument};
    if(!s.file_){
        if(!regular(s.path_,s.remaining_))return {0,false,Error::io_error};
        s.file_=std::fopen(s.path_,"rb");if(!s.file_)return {0,false,Error::io_error};
    }
    const auto take=static_cast<std::size_t>(std::min<std::uint64_t>(n,s.remaining_));
    const auto got=std::fread(b,1,take,s.file_);s.crc_=restore_crc32(s.crc_,b,got);s.remaining_-=got;
    if(got!=take){s.failed_=true;std::fclose(s.file_);s.file_=nullptr;return {got,false,Error::io_error};}
    if(!s.remaining_){
        const auto extra=std::fgetc(s.file_);const bool good=extra==EOF&&!std::ferror(s.file_)&&s.crc_==s.expected_;
        const auto closed=std::fclose(s.file_);s.file_=nullptr;s.done_=good&&closed==0;s.failed_=!s.done_;
        return {got,s.done_,s.done_?Error::ok:Error::io_error};
    }
    return {got,false,Error::ok};
}
Error RestoreSampleSource::begin(const char* root,std::string_view udid,bool require_full_scan){
    if(begun_)return Error::busy;
    if(!entries_||!count_||!root||std::strlen(root)>=sizeof(root_)||(udid.size()!=25&&udid.size()!=40)||!trusted_directory(root))return Error::invalid_argument;
    for(std::size_t i=0;i<udid.size();++i){
        if(udid.size()==25&&i==8){if(udid[i]!='-')return Error::invalid_argument;continue;}
        const auto c=udid[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return Error::invalid_argument;
    }
    std::memcpy(root_,root,std::strlen(root)+1);std::memcpy(udid_,udid.data(),udid.size());udid_[udid.size()]=0;
    begun_=true;ready_=!require_full_scan;return Error::ok;
}
Error RestoreSampleSource::preflight(std::uint8_t* b,std::size_t n){
    if(!begun_||!b||!n||n>kObservedChunkSize)return Error::invalid_argument;
    if(verified_)return Error::ok;
    const auto& item=entries_[checked_];
    if(!check_){
        char path[352];if(!join(path,sizeof(path),root_,item.name)||!regular(path,item.size))return Error::io_error;
        check_=std::fopen(path,"rb");if(!check_)return Error::io_error;
        checked_bytes_=0;crc_=0;
    }
    const auto got=std::fread(b,1,n,check_);crc_=restore_crc32(crc_,b,got);checked_bytes_+=got;
    if(checked_bytes_>item.size||std::ferror(check_)){std::fclose(check_);check_=nullptr;return Error::io_error;}
    if(std::feof(check_)){
        const auto closed=std::fclose(check_);check_=nullptr;
        if(closed||checked_bytes_!=item.size||crc_!=item.crc)return Error::io_error;
        ++checked_;if(checked_==count_){verified_=ready_=true;return Error::ok;}
    }
    return Error::would_block;
}
bool RestoreSampleSource::relative(std::string_view path,std::string_view& out) const {
    if(!ready_)return false;
    if(path.empty()){out=path;return true;}
    if(!valid_relative_backup_path(path)||path.find('\\')!=std::string_view::npos)return false;
    const std::string_view id(udid_);
    if(path==id){out={};return true;}
    if(path.size()>id.size()&&path.substr(0,id.size())==id&&path[id.size()]=='/')path.remove_prefix(id.size()+1);
    out=path;return true;
}
const RestoreSampleEntry* RestoreSampleSource::entry(std::string_view path) const {
    if(path=="Info.plist")path="info.plist";
    for(std::size_t i=0;i<count_;++i){
        const std::string_view name(entries_[i].name);
        if(path==name)return &entries_[i];
        if(path.size()==40&&name.size()==43&&name[2]=='/'&&name.substr(3)==path)return &entries_[i];
        if(path.size()==43&&path[2]=='/'&&shard(path.substr(0,2))&&path.substr(0,2)==path.substr(3,2)&&name.size()==40&&name==path.substr(3))return &entries_[i];
    }
    return nullptr;
}
Error RestoreSampleSource::lookup(std::string_view path,RestoreFileSource& reader,bool& found) const {
    found=false;reader.reset();std::string_view rel;
    if(!relative(path,rel))return Error::invalid_argument;
    const auto* item=entry(rel);if(!item)return Error::ok;
    const auto e=reader.bind(root_,*item);if(e==Error::ok)found=true;return e;
}
Error RestoreSampleSource::list(std::string_view path,DirectoryListing& out) const {
    out.count=0;std::string_view rel;if(!out.entries||!relative(path,rel)||(!rel.empty()&&!shard(rel)))return Error::invalid_argument;
    for(std::size_t i=0;i<count_;++i){
        const auto& item=entries_[i];std::string_view name(item.name);bool directory=false;
        const auto slash=name.find('/');
        if(rel.empty()&&slash!=std::string_view::npos){name=name.substr(0,slash);directory=true;}
        else if(!rel.empty()){
            if(slash!=std::string_view::npos){if(name.substr(0,slash)!=rel)continue;name.remove_prefix(slash+1);}
            else if(name.size()!=40||name.substr(0,2)!=rel)continue;
        }
        bool duplicate=false;for(std::size_t j=0;j<out.count;++j)if(std::string_view(out.entries[j].name,out.entries[j].name_length)==name)duplicate=true;
        if(duplicate)continue;
        if(out.count>=out.capacity||name.size()>255)return Error::too_large;
        auto& e=out.entries[out.count++];std::memcpy(e.name,name.data(),name.size());e.name[name.size()]=0;e.name_length=static_cast<std::uint16_t>(name.size());
        e.type=directory?EntryType::directory:EntryType::regular;e.size=directory?0:item.size;e.modification_time=0;e.capture_record=0;
    }
    return Error::ok;
}
}
