#pragma once
#include "HCCSdAccess.hpp"
#include "hcc_backup_storage.hpp"
// One capture per object. Worker-task only; destroy receiver before this target.
// Lease outlives target destruction so abort cleanup finishes before USB handoff.
class HCCBackupCapture {
public:
    HCCBackupCapture():lease_(HCCSdAccess::gate,hcc::backup::SdOwner::capture) {}
    hcc::backup::Error open();
    hcc::backup::UploadTarget target(){return storage_.target();}
    const char* sessionPath() const {return storage_.session_path();}
private:
    hcc::backup::SdLease lease_;
    hcc::backup::StagedUploadTarget storage_;
};
