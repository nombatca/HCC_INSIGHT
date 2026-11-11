#pragma once
#include "hcc_backup_receive.hpp"
#include "hcc_backup_listing.hpp"
#include <cstdio>
namespace hcc::backup {
// Lazy read-only source for one validated committed payload. No handle is held
// during lookup; sequential DownloadBatchSender use opens at most one at a time.
// Caller keeps the mounted session exclusively owned until reset/destruction.
class CapturedFileSource {
public:
    CapturedFileSource()=default;
    ~CapturedFileSource(){reset();}
    CapturedFileSource(const CapturedFileSource&)=delete;
    CapturedFileSource& operator=(const CapturedFileSource&)=delete;
    void reset();
    Source source(){return {this,read_callback};}
private:
    friend class StagedUploadTarget;
    Error bind(const char* record,std::uint64_t size);
    static SourceResult read_callback(void*,std::uint8_t*,std::size_t);
    char path_[352]{};
    FILE* file_=nullptr;
    std::uint64_t expected_=0,remaining_=0;
    bool done_=false,failed_=false;
};
// Synchronous filesystem target for an ALREADY mounted, exclusively owned
// filesystem. Parent must be trusted and must outlive this object. No mount,
// USB-disk arbitration or device operation is started by this class.
class StagedUploadTarget {
public:
    StagedUploadTarget()=default;
    StagedUploadTarget(const StagedUploadTarget&)=delete;
    StagedUploadTarget& operator=(const StagedUploadTarget&)=delete;
    ~StagedUploadTarget();
    static constexpr std::uint32_t max_created_directories=1024;
    Error open_session(const char* existing_parent);
    // Capture-only directory namespace: root/directories/<relative path>.
    // Payloads remain indexed .ready records, not materialized under this tree.
    // 512-byte full path, <=32 components, <=1024 newly created directories/session.
    // Exclusive ownership/trusted ancestors required; no symlinks on host.
    // Existing directories succeed. Failure may leave newly made parents.
    Error create_directory(std::string_view relative_path);
    // Reconstructs the immediate children of a logical backup directory by
    // combining committed .ready destinations (from identity.bin) with the
    // created-directory namespace. `request` is "" for the backup root or a
    // validated relative path. A directory with no children is a valid empty
    // listing (out.count==0, Error::ok). Overflow of out.capacity, an I/O error,
    // a symlink, or corrupt metadata returns an error and never a partial or
    // silently truncated/empty-on-error listing. Internal capture names
    // (NNNNNN.ready/.part, payload.bin, identity.bin) are never exposed.
    Error list_directory(std::string_view request, DirectoryListing& out, ListingScratch& scratch) const;
    // File-only logical rename of a unique committed record. Refuses existing
    // targets, directory moves and ambiguous identities. Uses caller PSRAM scratch.
    // Returns would_block while indexing <=8 records per call; call again.
    // Metadata swap rolls back ordinary rename errors; not power-loss atomic.
    Error move_file(std::string_view from,std::string_view to,ListingScratch& scratch);
    // Remove from this session's logical namespace. Files are renamed to
    // .removed recovery records, preserving payload/identity; no space reclaimed.
    // Directory subtrees archive at most one matching file per call, then
    // rename the created-directory tree to recovery storage. May yield; caller
    // retries the SAME path until completion. Earlier changes survive failure.
    // Validated absence succeeds (ENOENT semantics); I/O/corruption still fail.
    Error remove_item(std::string_view path,ListingScratch& scratch);
    // Exact destination in current .ready records only. Repeated uploads are
    // versioned records: use the highest committed record ID. Incremental index
    // may yield; absent is found=false, while corruption/directory/I/O fail.
    Error lookup_download(std::string_view path,ListingScratch& scratch,CapturedFileSource& out,bool& found);
    UploadTarget target(){return {this,begin_callback,write_callback,finish_callback};}
    const char* session_path() const {return root_;}
    std::uint32_t committed_files() const {return committed_;}
private:
    static Error begin_callback(void*,std::string_view,std::string_view);
    static IoResult write_callback(void*,const std::uint8_t*,std::size_t);
    static Error finish_callback(void*,bool);
    Error begin(std::string_view,std::string_view);
    Error finish(bool);
    Error cleanup();
    Error prepare_move_index(ListingScratch&);
    void* move_scan_=nullptr;
    std::uint32_t revision_=0;
    char root_[256]{},stage_[288]{},ready_[288]{};
    FILE* payload_=nullptr;
    bool active_=false;
    std::uint32_t sequence_=0,committed_=0,directories_=0,removed_directories_=0;
};
} // namespace hcc::backup
