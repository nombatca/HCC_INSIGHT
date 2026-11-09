#pragma once
#include "hcc_backup_receive.hpp"
#include "hcc_backup_listing.hpp"
#include <cstdio>
namespace hcc::backup {
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
    // Metadata swap rolls back ordinary rename errors; not power-loss atomic.
    Error move_file(std::string_view from,std::string_view to,ListingScratch& scratch);
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
    char root_[256]{},stage_[288]{},ready_[288]{};
    FILE* payload_=nullptr;
    bool active_=false;
    std::uint32_t sequence_=0,committed_=0,directories_=0;
};
} // namespace hcc::backup
