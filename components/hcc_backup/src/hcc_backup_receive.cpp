#include "hcc_backup_receive.hpp"
#include <algorithm>
namespace hcc::backup {
UploadBatchReceiver::~UploadBatchReceiver(){cancel();}
void UploadBatchReceiver::cancel(){if(opened_){opened_=false;target_.finish(target_.context,false);}if(state_!=State::idle&&state_!=State::done&&state_!=State::failed){state_=State::failed;error_=Error::closed;}}
Error UploadBatchReceiver::fail(Error e){cancel();state_=State::failed;return error_=e;}
void UploadBatchReceiver::length(State state){state_=state;need_=4;used_=0;}
Error UploadBatchReceiver::start(UploadTarget target,UploadLimits limits){
    if(state_!=State::idle)return Error::busy;
    if(!scratch_||!capacity_||capacity_>kObservedChunkSize||!target.begin||!target.write||!target.finish||
       !limits.max_files||!limits.max_frame_payload||!limits.max_file_bytes||!limits.max_batch_bytes)return Error::invalid_argument;
    target_=target;limits_=limits;length(State::source_length);return Error::ok;
}
void UploadBatchReceiver::finish_file(){
    opened_=false;const auto e=target_.finish(target_.context,true);
    if(e!=Error::ok){fail(e==Error::would_block?Error::io_error:e);return;}
    ++files_;length(State::source_length);
}
void UploadBatchReceiver::read_complete(){
    if(state_==State::source_length||state_==State::destination_length){
        const auto n=read_be32(header_);
        if(!n){if(state_==State::source_length)state_=State::done;else fail(Error::invalid_frame);return;}
        if(n>4096){fail(Error::too_large);return;}
        if(state_==State::source_length){if(files_>=limits_.max_files){fail(Error::too_large);return;}source_size_=n;state_=State::source;}
        else {destination_size_=n;state_=State::destination;}
        need_=n;used_=0;
    }else if(state_==State::source){
        for(std::size_t i=0;i<source_size_;++i)if(static_cast<unsigned char>(source_[i])<32){fail(Error::invalid_frame);return;}
        length(State::destination_length);
    }else if(state_==State::destination){
        if(!valid_relative_backup_path({destination_,destination_size_})){fail(Error::invalid_frame);return;}
        const auto e=target_.begin(target_.context,{source_,source_size_},{destination_,destination_size_});
        if(e!=Error::ok){fail(e==Error::would_block?Error::io_error:e);return;}
        opened_=true;had_data_=false;file_bytes_=0;length(State::frame_length);
    }else if(state_==State::frame_length){
        const auto n=read_be32(header_);
        if(!n){fail(Error::invalid_frame);return;}
        frame_left_=n-1;
        if(frame_left_>limits_.max_frame_payload){fail(Error::too_large);return;}
        state_=State::tag;need_=1;used_=0;
    }else if(state_==State::tag){
        if(tag_==0 && !frame_left_){
            finish_file();
        }else if(tag_==0x0c && frame_left_){
            if(frame_left_>limits_.max_file_bytes-file_bytes_||frame_left_>limits_.max_batch_bytes-total_){fail(Error::too_large);return;}
            state_=State::data;need_=std::min<std::size_t>(capacity_,frame_left_);used_=0;
        }else if(tag_==0x0b){
            if(!frame_left_){if(had_data_)finish_file();else fail(Error::invalid_frame);}
            else {state_=State::terminal;need_=std::min<std::size_t>(capacity_,frame_left_);used_=0;}
        }else fail(Error::invalid_frame);
    }else if(state_==State::terminal){
        frame_left_-=static_cast<std::uint32_t>(need_);
        if(!frame_left_){if(had_data_)finish_file();else fail(Error::invalid_frame);}
        else {need_=std::min<std::size_t>(capacity_,frame_left_);used_=0;}
    }else if(state_==State::data){pending_=need_;written_=0;state_=State::flush;}
}
Error UploadBatchReceiver::pump(Duplex io,std::size_t budget){
    if(state_==State::failed)return error_;
    if(state_==State::done)return Error::ok;
    if(state_==State::idle||!io.read||!budget)return Error::invalid_argument;
    for(std::size_t call=0;call<budget;++call){
        if(state_==State::flush){
            const auto remaining=pending_-written_;
            const auto r=target_.write(target_.context,scratch_+written_,remaining);
            if(r.bytes>remaining)return fail(Error::io_error);
            written_+=r.bytes;
            if(r.state==IoState::error)return fail(Error::io_error);
            if(r.state==IoState::closed)return fail(Error::closed);
            if(written_==pending_){
                frame_left_-=static_cast<std::uint32_t>(pending_);file_bytes_+=pending_;total_+=pending_;
                if(!frame_left_){had_data_=true;length(State::frame_length);}
                else {state_=State::data;need_=std::min<std::size_t>(capacity_,frame_left_);used_=0;}
            }
            if(r.state==IoState::would_block)return Error::would_block;
            if(!r.bytes)return fail(Error::no_progress);
        }else{
            auto* bytes=state_==State::source?reinterpret_cast<std::uint8_t*>(source_):
                        state_==State::destination?reinterpret_cast<std::uint8_t*>(destination_):
                        state_==State::tag?&tag_:(state_==State::data||state_==State::terminal)?scratch_:header_;
            const auto remaining=need_-used_;
            const auto r=io.read(io.context,bytes+used_,remaining);
            if(r.bytes>remaining)return fail(Error::io_error);
            used_+=r.bytes;
            if(r.state==IoState::error)return fail(Error::io_error);
            if(r.state==IoState::closed)return fail(Error::truncated);
            if(used_==need_)read_complete();
            if(state_==State::failed)return error_;
            if(state_==State::done)return Error::ok;
            if(r.state==IoState::would_block)return Error::would_block;
            if(!r.bytes)return fail(Error::no_progress);
        }
    }
    return Error::would_block;
}
} // namespace hcc::backup
