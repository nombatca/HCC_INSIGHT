#pragma once
#include "hcc_backup_receive.hpp"
namespace hcc::backup {
enum class UploadRequestPhase { idle,receiving,reply,complete,failed };
// One validated UploadFiles request after the caller sent its real operation
// request and marked the observer running. Does not initiate an operation.
// Observer, target and exclusive scratch/output buffers must outlive this object.
class UploadRequestHandler {
public:
    UploadRequestHandler(SessionObserver& observer,std::uint8_t* scratch,std::size_t capacity,
                         std::uint8_t* output,std::size_t output_capacity)
        :observer_(observer),receiver_(scratch,capacity),status_(output,output_capacity) {}
    ~UploadRequestHandler(){cancel();}
    bool can_start() const {return phase_==UploadRequestPhase::idle && observer_.pending_request()==EventKind::upload_files;}
    Error start(UploadTarget,UploadLimits={});
    Error pump(Duplex,std::size_t max_io_calls=8);
    void cancel();
    UploadRequestPhase phase() const {return phase_;}
    std::uint64_t completed_payload_bytes() const {return receiver_.completed_payload_bytes();}
    std::uint32_t completed_files() const {return receiver_.completed_files();}
private:
    Error fail(Error);
    SessionObserver& observer_;
    UploadBatchReceiver receiver_;
    StatusReplyWriter status_;
    UploadRequestPhase phase_=UploadRequestPhase::idle;
    Error error_=Error::ok;
};
} // namespace hcc::backup
