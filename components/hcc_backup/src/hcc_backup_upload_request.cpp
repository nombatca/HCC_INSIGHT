#include "hcc_backup_upload_request.hpp"
namespace hcc::backup {
Error UploadRequestHandler::fail(Error e){receiver_.cancel();observer_.transport_failed();phase_=UploadRequestPhase::failed;return error_=e;}
void UploadRequestHandler::cancel(){if(phase_==UploadRequestPhase::receiving||phase_==UploadRequestPhase::reply)fail(Error::closed);}
Error UploadRequestHandler::start(UploadTarget target,UploadLimits limits){
    if(!can_start())return Error::invalid_argument;
    const auto e=receiver_.start(target,limits);
    if(e!=Error::ok)return fail(e);
    const auto status=status_.begin(0); // Validate output capacity before receiving data.
    if(status!=Error::ok)return fail(status);
    phase_=UploadRequestPhase::receiving;return Error::ok;
}
Error UploadRequestHandler::pump(Duplex io,std::size_t budget){
    if(phase_==UploadRequestPhase::failed)return error_;
    if(phase_==UploadRequestPhase::complete)return Error::ok;
    if(phase_==UploadRequestPhase::idle||!budget||!io.read||!io.write)return Error::invalid_argument;
    // No other consumer may change the observer while this raw stream is owned.
    if(observer_.pending_request()!=EventKind::upload_files)return fail(Error::invalid_frame);
    if(phase_==UploadRequestPhase::receiving){
        const auto e=receiver_.pump(io,budget);
        if(e==Error::would_block)return e;
        if(e!=Error::ok)return fail(e);
        phase_=UploadRequestPhase::reply;
        return Error::would_block; // Separate pump preserves the total I/O budget.
    }
    const auto e=status_.pump({io.context,io.write});
    if(e==Error::would_block)return e;
    if(e!=Error::ok)return fail(e);
    if(observer_.complete_request(true)!=SessionError::ok)return fail(Error::invalid_frame);
    phase_=UploadRequestPhase::complete;return Error::ok;
}
} // namespace hcc::backup
