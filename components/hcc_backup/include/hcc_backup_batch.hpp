#pragma once
#include "hcc_backup_link.hpp"
#include <string_view>
namespace hcc::backup {
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
    enum class State { idle,name,data,terminator,status,done,failed };
    Error fail(Error);
    void prepare_name();
    std::uint8_t* names_;std::size_t name_capacity_,scratch_capacity_;
    std::uint8_t* scratch_;
    const DownloadFile* files_=nullptr;std::size_t count_=0,index_=0,size_=0,sent_=0;
    BlobSender blob_;
    StatusReplyWriter status_;
    State state_=State::idle;Error error_=Error::ok;
};
} // namespace hcc::backup
