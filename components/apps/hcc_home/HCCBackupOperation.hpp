#pragma once
#include "HCCBackupCapture.hpp"
#include "hcc_backup_start.hpp"
#include "hcc_backup_upload_request.hpp"
#include "hcc_backup_batch.hpp"
#include <optional>
enum class HCCBackupPhase {idle,starting,waiting,replying,receiving,downloading,succeeded,failed,cancelled,moving};
enum class HCCBackupError {ok,would_block,invalid_argument,busy,storage,quota,transfer,
                           timeout,clock,unsupported,remote,disconnected,cancelled};
struct HCCBackupConfig {
    hcc::backup::UploadLimits limits{};
    std::uint64_t minimum_free_bytes=1048576;
    std::uint64_t idle_timeout_ms=60000,total_timeout_ms=600000;
    bool force_full=false;
    bool use_sd_capacity=false;
    std::uint64_t reserve_bytes=64ULL*1024*1024;
};
// Caller-owned storage for DLContentsOfDirectory listings, kept out of this
// object so it stays small enough for the USB worker. `entries`/`entry_capacity`
// hold the reconstructed children; `offsets`/`offset_capacity` back the binary
// plist object table and must provide at least
// hcc::backup::contents_object_count(entry_capacity) slots. Leave empty to refuse
// directory listings. All buffers must outlive the operation.
struct HCCBackupContentsBuffers {
    hcc::backup::DirectoryEntry* entries=nullptr;
    std::size_t entry_capacity=0;
    std::uint32_t* offsets=nullptr;
    std::size_t offset_capacity=0;
    hcc::backup::ListingScratch* scratch=nullptr; // path working buffers, kept off-stack
};
// Bounded prototype driver for Backup, UploadFiles, GetFreeDiskSpace, CreateDirectory and
// DLContentsOfDirectory. Other filesystem commands fail explicitly. Used by the
// experimental MDM capture action; not an Apple-restorable backup. Allocate off
// the small USB task stack. Caller owns transport scheduling, independent buffers,
// link lifetime, and must close/abandon the stream on failure/cancel. No internal
// tasks are created.
class HCCBackupOperation {
public:
    HCCBackupOperation(hcc::backup::BackupLink& link,std::uint8_t* scratch,std::size_t capacity,
                       std::uint8_t* output,std::size_t output_capacity,
                       HCCBackupContentsBuffers contents={})
        :link_(link),scratch_(scratch),capacity_(capacity),output_(output),output_capacity_(output_capacity),
         contents_(contents) {}
    ~HCCBackupOperation(){cancel();}
    HCCBackupError start(std::string_view udid,std::uint64_t now,HCCBackupConfig={});
    HCCBackupError poll(hcc::backup::Duplex,std::uint64_t now,std::size_t budget=8);
    // Owner-task check independent of transport readiness. No I/O is performed.
    HCCBackupError checkDeadline(std::uint64_t now);
    std::uint64_t idleMilliseconds(std::uint64_t now) const {return now>=progress_?now-progress_:0;}
    std::uint64_t elapsedMilliseconds(std::uint64_t now) const {return now>=began_?now-began_:0;}
    void cancel();
    HCCBackupPhase phase() const {return phase_;}
    HCCBackupError error() const {return error_;}
    const char* sessionPath() const {return capture_.sessionPath();}
    std::uint32_t committedFiles() const {return capture_.committedFiles();}
    std::int64_t remoteCode() const {return remote_code_;}
private:
    HCCBackupError fail(HCCBackupError);
    struct Progress {
        HCCBackupOperation* operation;hcc::backup::Duplex io;std::uint64_t now;
        static hcc::backup::IoResult read(void*,std::uint8_t*,std::size_t);
        static hcc::backup::IoResult write(void*,const std::uint8_t*,std::size_t);
    };
    hcc::backup::BackupLink& link_;
    std::uint8_t* scratch_;std::size_t capacity_;std::uint8_t* output_;std::size_t output_capacity_;
    HCCBackupContentsBuffers contents_;
    hcc::backup::SessionObserver* observer_=nullptr;
    HCCBackupCapture capture_; // Destroy handlers before target and SD lease.
    std::optional<hcc::backup::StatusReplyWriter> reply_;
    std::optional<hcc::backup::BackupStartWriter> start_;
    std::optional<hcc::backup::UploadRequestHandler> upload_;
    std::optional<hcc::backup::DownloadBatchSender> download_;
    // Requested download paths are copied here (the plist document is transient).
    static constexpr std::size_t kMaxDownloadFiles = 24;
    hcc::backup::DownloadFile download_entries_[kMaxDownloadFiles];
    char download_arena_[2048];
    hcc::plist::Document move_doc_{};
    std::uint32_t move_key_=hcc::plist::none,moves_done_=0;
    HCCBackupConfig config_{};
    HCCBackupPhase phase_=HCCBackupPhase::idle;HCCBackupError error_=HCCBackupError::ok;
    std::uint64_t began_=0,last_now_=0,progress_=0,bytes_=0;
    std::uint32_t files_=0;
    std::int64_t remote_code_=0;
};
