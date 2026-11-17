#include "HCCSdFileSystem.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
namespace HCCSdFileSystem {
namespace {
int inspect(const char* path,struct stat& st){
#ifdef ESP_PLATFORM
    return stat(path,&st); // FAT VFS has no symlinks.
#else
    return lstat(path,&st);
#endif
}
bool valid(const char* path){
    if(!path)return false;
    const auto n=std::strlen(path);if(n>=path_size||path[0]=='/')return false;
    std::size_t start=0;
    for(std::size_t i=0;i<=n;++i){
        const auto c=static_cast<unsigned char>(path[i]);
        if(i<n&&c!='/'){
            if(c<32||c==127||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;
            continue;
        }
        const auto length=i-start;
        if(n&&(length==0||length>255||path[i-1]=='.'||path[i-1]==' '))return false;
        start=i+1;
    }
    return true;
}
Error resolve(const char* root,const char* relative,char* full){
    if(!root||!root[0]||!valid(relative))return Error::invalid_path;
    const int n=std::snprintf(full,path_size,"%s%s%s",root,relative[0]?"/":"",relative);
    if(n<=0||static_cast<std::size_t>(n)>=path_size)return Error::invalid_path;
    const auto base=std::strlen(root);struct stat st{};
    if(inspect(root,st)!=0||!S_ISDIR(st.st_mode))return Error::io;
    for(std::size_t i=base+1;i<static_cast<std::size_t>(n);++i){
        if(full[i]!='/')continue;
        full[i]=0;const int seen=inspect(full,st);full[i]='/';
        if(seen!=0||!S_ISDIR(st.st_mode))return Error::io;
    }
    return Error::ok;
}
bool dot(const char* name){return std::strcmp(name,".")==0||std::strcmp(name,"..")==0;}
}
Error list(const char* root,const char* relative,std::size_t offset,Page& page,Hooks hooks){
    page.count=0;page.more=false;char full[path_size];
    const auto check=resolve(root,relative,full);if(check!=Error::ok)return check;
    struct stat st{};if(inspect(full,st)!=0||!S_ISDIR(st.st_mode))return Error::io;
    DIR* dir=opendir(full);if(!dir)return Error::io;
    Error result=Error::ok;std::size_t index=0;
    const auto prefix=std::strlen(full);
    for(;;){
        if(hooks.cancel&&hooks.cancel(hooks.context)){result=Error::cancelled;break;}
        errno=0;auto* item=readdir(dir);if(!item){if(errno)result=Error::io;break;}
        if(dot(item->d_name))continue;
        if(index++<offset)continue;
        if(page.count==page_size){page.more=true;break;}
        auto& entry=page.entries[page.count++];entry={};
        const auto length=std::strlen(item->d_name);
        if(length>=sizeof(entry.name)){result=Error::unsupported;break;}
        std::memcpy(entry.name,item->d_name,length+1);
        if(prefix+1+length>=sizeof(full))continue;
        full[prefix]='/';std::memcpy(full+prefix+1,item->d_name,length+1);
        if(inspect(full,st)!=0){result=Error::io;break;}
        entry.directory=S_ISDIR(st.st_mode);entry.supported=(entry.directory||S_ISREG(st.st_mode))&&valid(entry.name);
        entry.bytes=S_ISREG(st.st_mode)?static_cast<std::uint64_t>(st.st_size):0;
        full[prefix]=0;
    }
    if(closedir(dir)!=0)result=Error::io;
    if(result!=Error::ok){page.count=0;page.more=false;}
    return result;
}
Error remove(const char* root,const char* relative,bool expected_directory,Hooks hooks,std::uint32_t& removed){
    removed=0;if(!relative||!relative[0])return Error::invalid_path;
    char full[path_size];const auto check=resolve(root,relative,full);if(check!=Error::ok)return check;
    auto cancelled=[&](){return hooks.cancel&&hooks.cancel(hooks.context);};
    auto progress=[&](){++removed;if(hooks.progress)hooks.progress(hooks.context,removed);};
    if(cancelled())return Error::cancelled;
    struct stat st{};if(inspect(full,st)!=0)return Error::io;
    if(expected_directory!=static_cast<bool>(S_ISDIR(st.st_mode)))return Error::unsupported;
    if(S_ISREG(st.st_mode)){if(unlink(full)!=0)return Error::io;progress();return Error::ok;}
    if(!S_ISDIR(st.st_mode))return Error::unsupported;
    struct Frame { DIR* dir;std::size_t length; } frames[24]{};
    std::size_t depth=1;frames[0]={opendir(full),std::strlen(full)};
    if(!frames[0].dir)return Error::io;
    Error result=Error::ok;
    while(depth){
        if(cancelled()){result=Error::cancelled;break;}
        auto& frame=frames[depth-1];full[frame.length]=0;
        errno=0;auto* item=readdir(frame.dir);
        if(!item){
            if(errno){result=Error::io;break;}
            const int closed=closedir(frame.dir);frame.dir=nullptr;
            if(closed!=0||rmdir(full)!=0){result=Error::io;break;}
            --depth;progress();continue;
        }
        if(dot(item->d_name))continue;
        const auto length=std::strlen(item->d_name);
        if(frame.length+1+length>=sizeof(full)){result=Error::invalid_path;break;}
        full[frame.length]='/';std::memcpy(full+frame.length+1,item->d_name,length+1);
        if(inspect(full,st)!=0){result=Error::io;break;}
        if(S_ISDIR(st.st_mode)){
            if(depth==24){result=Error::too_deep;break;}
            DIR* child=opendir(full);if(!child){result=Error::io;break;}
            frames[depth++]={child,std::strlen(full)};
        }else if(S_ISREG(st.st_mode)){
            if(cancelled()){result=Error::cancelled;break;}
            if(unlink(full)!=0){result=Error::io;break;}
            progress();
        }else{result=Error::unsupported;break;}
    }
    for(std::size_t i=0;i<depth;++i)if(frames[i].dir)closedir(frames[i].dir);
    return result;
}
}
