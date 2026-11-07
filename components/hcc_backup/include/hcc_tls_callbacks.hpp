#pragma once
#include "hcc_backup_link.hpp"

namespace hcc::backup {
enum class IoInterest { none, read, write };
struct TlsCodes { int want_read; int want_write; int peer_close_notify; };
// Bridges an existing TLS/service context. Supply wrappers matching these
// signatures; this class does not initialize TLS or change authentication.
class TlsCallbacks {
public:
    using Read=int(*)(void*,unsigned char*,std::size_t);
    using Write=int(*)(void*,const unsigned char*,std::size_t);
    TlsCallbacks(void* context,Read read,Write write,TlsCodes codes)
        :context_(context),read_(read),write_(write),codes_(codes) {}
    Duplex duplex() { return {this,read_bridge,write_bridge}; }
    IoInterest interest() const { return interest_; }
    int last_error() const { return last_error_; }
private:
    IoResult map(int result,std::size_t requested) {
        interest_=IoInterest::none;
        if(result>0) {
            if(std::size_t(result)>requested) return {0,IoState::error};
            return {std::size_t(result),IoState::ok};
        }
        if(result==0 || (codes_.peer_close_notify<0&&result==codes_.peer_close_notify)) return {0,IoState::closed};
        if(codes_.want_read<0&&result==codes_.want_read) {interest_=IoInterest::read;return {0,IoState::would_block};}
        if(codes_.want_write<0&&result==codes_.want_write) {interest_=IoInterest::write;return {0,IoState::would_block};}
        last_error_=result;return {0,IoState::error};
    }
    static IoResult read_bridge(void* p,std::uint8_t* b,std::size_t n) {
        auto& self=*static_cast<TlsCallbacks*>(p);
        if(!self.read_) return {0,IoState::error};
        return self.map(self.read_(self.context_,b,n),n);
    }
    static IoResult write_bridge(void* p,const std::uint8_t* b,std::size_t n) {
        auto& self=*static_cast<TlsCallbacks*>(p);
        if(!self.write_) return {0,IoState::error};
        return self.map(self.write_(self.context_,b,n),n);
    }
    void* context_;Read read_;Write write_;TlsCodes codes_;
    IoInterest interest_=IoInterest::none;int last_error_=0;
};
} // namespace hcc::backup
