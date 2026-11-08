#pragma once
#include "hcc_backup_link.hpp"
#include <string_view>
namespace hcc::backup {
// One requested file. A null source.read marks the file as absent on the host:
// the sender emits a per-file "error" frame instead of data, and the final status
// becomes a DeviceLink multi-status. This is what a fresh backup returns for the
// Status/Manifest files an iPad asks to download before sending its data.
struct DownloadFile { std::string_view path; Source source; };
// Streams an explicitly supplied, validated file list. No filesystem access,
// operation request, or SessionObserver acknowledgement is performed here.
// Caller keeps entries, paths, sources and both buffers alive and exclusive.
class DownloadBatchSender {
public:
    DownloadBatchSender(std::uint8_t* name_buffer,std::size_t name_capacity,
                        std::uint8_t* scratch,std::size_t scratch_capacity);
    DownloadBatchSender(const DownloadBatchSender&)=delete;
    DownloadBatchSender& operator=(const DownloadBatchSender&)=delete;
    Error start(const DownloadFile*,std::size_t count);
    // Bounded to at most max_writes sink calls per pump (default 8).
    Error pump(Sink,std::size_t max_writes=8);
    bool done() const { return state_==State::done; }
    std::size_t completed_files() const { return index_; }
private:
    enum class State { idle,name,data,error_frame,terminator,status,done,failed };
    Error fail(Error);
    void prepare_name();
    void prepare_error(); // builds the CODE_ERROR_LOCAL frame for an absent file
    std::uint8_t* names_;std::size_t name_capacity_,scratch_capacity_;
    std::uint8_t* scratch_;
    const DownloadFile* files_=nullptr;std::size_t count_=0,index_=0,size_=0,sent_=0;
    bool any_error_=false;
    BlobSender blob_;
    StatusReplyWriter status_;
    State state_=State::idle;Error error_=Error::ok;
};
} // namespace hcc::backup
