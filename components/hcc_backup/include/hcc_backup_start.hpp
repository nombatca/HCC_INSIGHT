#pragma once
#include "hcc_backup_link.hpp"
namespace hcc::backup {
enum class BackupStartPhase {idle,sending,sent,failed};
// Use an observer obtained from a negotiated BackupLink. Caller must reserve
// storage and a transfer handler before invoking. Owns no SD/USB connection.
// Buffer is exclusively borrowed until sent/error. One request per object.
class BackupStartWriter {
public:
    BackupStartWriter(SessionObserver& observer,std::uint8_t* output,std::size_t capacity)
        :observer_(observer),output_(output),capacity_(capacity) {}
    BackupStartWriter(const BackupStartWriter&)=delete;
    BackupStartWriter& operator=(const BackupStartWriter&)=delete;
    ~BackupStartWriter(){cancel();}
    Error begin(std::string_view udid,bool force_full=false);
    Error pump(Sink);
    void cancel();
    BackupStartPhase phase() const {return phase_;}
    std::size_t sent_bytes() const {return sent_;}
private:
    Error fail(Error);
    SessionObserver& observer_;
    std::uint8_t* output_;
    std::size_t capacity_,size_=0,sent_=0;
    BackupStartPhase phase_=BackupStartPhase::idle;
    Error error_=Error::ok;
};
} // namespace hcc::backup
