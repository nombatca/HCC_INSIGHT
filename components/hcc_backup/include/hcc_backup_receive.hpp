#pragma once
#include "hcc_backup_link.hpp"
namespace hcc::backup {
struct UploadTarget {
    void* context;
    // Source name is opaque metadata. Only destination is validated as relative.
    Error (*begin)(void*,std::string_view source,std::string_view destination);
    IoResult (*write)(void*,const std::uint8_t*,std::size_t);
    // Called exactly once after successful begin: commit on 0x00, abort on error.
    // Caller must stage data and atomically publish only when commit succeeds.
    Error (*finish)(void*,bool commit);
};
struct UploadLimits {
    std::uint32_t max_files=1024,max_frame_payload=1048576;
    std::uint64_t max_file_bytes=67108864,max_batch_bytes=268435456;
};
// Receives raw upload records AFTER an operation driver accepted UploadFiles.
// Owns no filesystem and sends no status. Supports native data (0x0c) and clean
// file-end (0x00) only. Remote/unknown terminal codes fail closed, never commit.
// Separate caller scratch storage (1..32768 bytes) remains exclusive while active.
class UploadBatchReceiver {
public:
    UploadBatchReceiver(std::uint8_t* scratch,std::size_t capacity):scratch_(scratch),capacity_(capacity) {}
    UploadBatchReceiver(const UploadBatchReceiver&)=delete;
    UploadBatchReceiver& operator=(const UploadBatchReceiver&)=delete;
    ~UploadBatchReceiver();
    Error start(UploadTarget,UploadLimits={});
    Error pump(Duplex,std::size_t max_io_calls=8);
    void cancel();
    bool done() const {return state_==State::done;}
    std::uint32_t completed_files() const {return files_;}
    std::uint64_t completed_payload_bytes() const {return total_;}
private:
    enum class State {idle,source_length,source,destination_length,destination,
                      frame_length,tag,data,flush,done,failed};
    Error fail(Error);
    void length(State);
    void read_complete();
    std::uint8_t* scratch_;std::size_t capacity_;
    UploadTarget target_{};UploadLimits limits_{};
    State state_=State::idle;Error error_=Error::ok;
    std::uint8_t header_[4]{},tag_=0;
    char source_[4096]{},destination_[4096]{};
    std::size_t need_=4,used_=0,source_size_=0,destination_size_=0,pending_=0,written_=0;
    std::uint32_t frame_left_=0,files_=0;
    std::uint64_t file_bytes_=0,total_=0;
    bool opened_=false;
};
} // namespace hcc::backup
