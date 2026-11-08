#include "hcc_backup_batch.hpp"
#include <cstring>
namespace hcc::backup {
DownloadBatchSender::DownloadBatchSender(std::uint8_t* names,std::size_t nc,std::uint8_t* scratch,std::size_t sc)
    :names_(names),name_capacity_(nc),scratch_capacity_(sc),scratch_(scratch),blob_(scratch,sc),status_(names,nc) {}
Error DownloadBatchSender::fail(Error e) {state_=State::failed;return error_=e;}
// CODE_ERROR_LOCAL frame body for an absent file: one code byte + ASCII reason.
static constexpr std::uint8_t kCodeErrorLocal=0x06;
static constexpr char kAbsentMessage[]="No such file or directory";
void DownloadBatchSender::prepare_name() {
    const auto path=files_[index_].path;
    write_be32(names_,static_cast<std::uint32_t>(path.size()));
    std::memcpy(names_+4,path.data(),path.size());size_=path.size()+4;sent_=0;state_=State::name;
}
void DownloadBatchSender::prepare_error() {
    const std::size_t msg=sizeof(kAbsentMessage)-1;
    write_be32(names_,static_cast<std::uint32_t>(1+msg)); // length covers the code byte
    names_[4]=kCodeErrorLocal;
    std::memcpy(names_+5,kAbsentMessage,msg);size_=5+msg;sent_=0;state_=State::error_frame;
}
Error DownloadBatchSender::start(const DownloadFile* files,std::size_t count) {
    if(state_!=State::idle) return Error::busy; // One sender per batch, including failures.
    if(!names_ || name_capacity_<256 || !scratch_ || !scratch_capacity_ ||
       scratch_capacity_>kObservedChunkSize || (count&&!files)) return Error::invalid_argument;
    if(count>1024) return Error::too_large;
    // Validate every entry before emitting any bytes. A null source marks an
    // absent file (an error frame is emitted). Filesystem providers must
    // additionally jail paths and resolve symlinks inside their backup root.
    for(std::size_t i=0;i<count;++i) {
        if(!valid_relative_backup_path(files[i].path)) return Error::invalid_argument;
        if(files[i].path.size()>name_capacity_-4) return Error::too_large;
    }
    files_=files;count_=count;index_=0;error_=Error::ok;any_error_=false;
    if(count) prepare_name();
    else {write_be32(names_,0);size_=4;sent_=0;state_=State::terminator;}
    return Error::ok;
}
Error DownloadBatchSender::pump(Sink sink,std::size_t budget) {
    if(state_==State::failed) return error_;
    if(state_==State::done) return Error::ok;
    if(state_==State::idle || !sink.write || !budget) return Error::invalid_argument;
    struct Limited {
        Sink sink;std::size_t remaining;
        static IoResult write(void* context,const std::uint8_t* p,std::size_t n) {
            auto& self=*static_cast<Limited*>(context);
            if(!self.remaining) return {0,IoState::would_block};
            --self.remaining;return self.sink.write(self.sink.context,p,n);
        }
    } limited{sink,budget};
    const Sink bounded{&limited,Limited::write};
    while(limited.remaining) {
        if(state_==State::data) {
            const auto e=blob_.pump(bounded);
            if(e==Error::would_block) return e;
            if(e!=Error::ok) return fail(e);
            ++index_;
            if(index_<count_) prepare_name();
            else {write_be32(names_,0);size_=4;sent_=0;state_=State::terminator;}
        } else if(state_==State::status) {
            const auto e=status_.pump(bounded);
            if(e==Error::would_block) return e;
            if(e!=Error::ok) return fail(e);
            state_=State::done;return Error::ok;
        } else {
            const auto remaining=size_-sent_;
            const auto r=bounded.write(bounded.context,names_+sent_,remaining);
            if(r.bytes>remaining) return fail(Error::io_error);
            sent_+=r.bytes;
            if(r.state==IoState::error) return fail(Error::io_error);
            if(r.state==IoState::closed) return fail(Error::closed);
            if(sent_==size_) {
                if(state_==State::name) {
                    if(files_[index_].source.read) {
                        const auto e=blob_.start(files_[index_].source);
                        if(e!=Error::ok) return fail(e);
                        state_=State::data;
                    } else {
                        any_error_=true;prepare_error(); // absent file -> error frame
                    }
                } else if(state_==State::error_frame) {
                    ++index_;
                    if(index_<count_) prepare_name();
                    else {write_be32(names_,0);size_=4;sent_=0;state_=State::terminator;}
                } else {
                    // All-success -> status 0; any absent file -> DeviceLink multi-status.
                    const auto e=any_error_?status_.begin(-13,"Multi status"):status_.begin(0);
                    if(e!=Error::ok) return fail(e);
                    state_=State::status;
                }
            }
            if(r.state==IoState::would_block) return Error::would_block;
            if(!r.bytes) return fail(Error::no_progress);
        }
    }
    return Error::would_block;
}
} // namespace hcc::backup
