#include "hcc_backup_start.hpp"
namespace hcc::backup {
Error BackupStartWriter::fail(Error e){observer_.transport_failed();phase_=BackupStartPhase::failed;return error_=e;}
void BackupStartWriter::cancel(){if(phase_==BackupStartPhase::sending)fail(Error::closed);}
Error BackupStartWriter::begin(std::string_view udid,bool full){
    if(phase_!=BackupStartPhase::idle)return Error::busy;
    if(observer_.phase()!=Phase::ready||!output_||capacity_<4)return Error::invalid_argument;
    std::size_t body=0;
    const auto e=encode_backup_request(udid,full,output_+4,capacity_-4,body);
    if(e!=LinkError::ok)return e==LinkError::too_large?Error::too_large:Error::invalid_argument;
    write_be32(output_,static_cast<std::uint32_t>(body));size_=body+4;sent_=0;
    phase_=BackupStartPhase::sending;return Error::ok;
}
Error BackupStartWriter::pump(Sink sink){
    if(phase_==BackupStartPhase::failed)return error_;
    if(phase_==BackupStartPhase::sent)return Error::ok;
    if(phase_==BackupStartPhase::idle||!sink.write)return Error::invalid_argument;
    if(observer_.phase()!=Phase::ready)return fail(Error::invalid_frame);
    const auto remaining=size_-sent_;
    const auto r=sink.write(sink.context,output_+sent_,remaining);
    if(r.bytes>remaining)return fail(Error::io_error);
    sent_+=r.bytes;
    if(r.state==IoState::closed)return fail(Error::closed);
    if(r.state==IoState::error)return fail(Error::io_error);
    if(sent_==size_){
        if(observer_.mark_operation_started()!=SessionError::ok)return fail(Error::invalid_frame);
        phase_=BackupStartPhase::sent;return Error::ok;
    }
    if(r.state==IoState::would_block)return Error::would_block;
    if(!r.bytes)return fail(Error::no_progress);
    return Error::would_block;
}
} // namespace hcc::backup
