#pragma once
#include "HCCBackupCapture.hpp"
#include "hcc_backup_upload_request.hpp"
// Worker-task adapter: one accepted UploadFiles request, mounted capture target,
// ownership lease, raw receiver and status reply. No Backup/Restore is sent.
// Destroy after completion/failure to release SD; its session directory remains.
class HCCBackupUploadRequest {
public:
    HCCBackupUploadRequest(hcc::backup::SessionObserver& observer,
        std::uint8_t* scratch,std::size_t capacity,std::uint8_t* output,std::size_t output_capacity)
        :handler_(observer,scratch,capacity,output,output_capacity) {}
    hcc::backup::Error start(hcc::backup::UploadLimits limits={}) {
        if(!handler_.can_start())return hcc::backup::Error::invalid_argument;
        const auto e=capture_.open();
        if(e!=hcc::backup::Error::ok)return e;
        return handler_.start(capture_.target(),limits);
    }
    hcc::backup::Error pump(hcc::backup::Duplex io,std::size_t budget=8){return handler_.pump(io,budget);}
    hcc::backup::UploadRequestPhase phase() const {return handler_.phase();}
    const char* sessionPath() const {return capture_.sessionPath();}
private:
    // Destruction order: handler aborts first, target cleanup second, lease last.
    HCCBackupCapture capture_;
    hcc::backup::UploadRequestHandler handler_;
};
