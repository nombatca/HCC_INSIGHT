#pragma once
#include "HCCSdAccess.hpp"
#include "hcc_backup_storage.hpp"
// One capture per object. Worker-task only; destroy receiver before this target.
// Lease outlives target destruction so abort cleanup finishes before USB handoff.
class HCCBackupCapture {
public:
    HCCBackupCapture():lease_(HCCSdAccess::gate,hcc::backup::SdOwner::capture) {}
    hcc::backup::Error open(std::uint64_t minimum_free_bytes=0);
    std::uint32_t committedFiles() const {return storage_.committed_files();}
    hcc::backup::Error createDirectory(std::string_view path){return storage_.create_directory(path);}
    hcc::backup::Error listDirectory(std::string_view path,hcc::backup::DirectoryListing& out){return storage_.list_directory(path,out);}
    hcc::backup::UploadTarget target(){return storage_.target();}
    const char* sessionPath() const {return storage_.session_path();}
private:
    hcc::backup::SdLease lease_;
    hcc::backup::StagedUploadTarget storage_;
};
