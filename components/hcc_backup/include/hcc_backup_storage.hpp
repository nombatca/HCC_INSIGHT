#pragma once
#include "hcc_backup_receive.hpp"
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
    Error open_session(const char* existing_parent);
    // Capture-only directory namespace: root/directories/<relative path>.
    // Payloads remain indexed .ready records, not materialized under this tree.
    // 512-byte full path, <=32 components, <=256 newly created directories/session.
    // Exclusive ownership/trusted ancestors required; no symlinks on host.
    // Existing directories succeed. Failure may leave newly made parents.
    Error create_directory(std::string_view relative_path);
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
