#pragma once
#include <atomic>
namespace hcc::backup {
enum class SdOwner { free, capture, usb_disk, file_manager };
class SdLease;
class SdAccess {
public:
    SdOwner owner() const {return owner_.load();}
private:
    friend class SdLease;
    std::atomic<SdOwner> owner_{SdOwner::free};
};
// Noncopyable ownership token. No blocking, recursive acquisition or stealing.
// Callers must serialize methods on an individual token; distinct tokens may
// contend concurrently. Keep the gate alive longer than every token.
class SdLease {
public:
    SdLease(SdAccess& gate,SdOwner owner):gate_(gate),owner_(owner) {}
    ~SdLease(){release();}
    SdLease(const SdLease&)=delete;
    SdLease& operator=(const SdLease&)=delete;
    bool acquire(){
        if(held_||owner_==SdOwner::free)return false;
        auto expected=SdOwner::free;
        if(!gate_.owner_.compare_exchange_strong(expected,owner_))return false;
        held_=true;return true;
    }
    void release(){if(held_){gate_.owner_.store(SdOwner::free);held_=false;}}
    bool held() const {return held_;}
private:
    SdAccess& gate_;SdOwner owner_;bool held_=false;
};
} // namespace hcc::backup
