#pragma once
#include "hcc_plist.hpp"
#include <cstdint>
#include <string_view>

namespace hcc::backup {
enum class Phase { await_version, version_decision, await_ready, ready,
                   running, request_pending, completed, disconnected, failed };
enum class Outcome { none, success, remote_error, local_error };
enum class SessionError { ok, malformed_message, wrong_state, unsupported_message,
                          unsupported_operation, invalid_path, incompatible_version };
enum class EventKind { version_offer, device_ready, download_files, upload_files, free_disk_space, create_directory,
                       contents_of_directory, process_message, result, disconnect, unsupported, purge_disk_space, move_files };
struct SessionEvent {
    EventKind kind=EventKind::unsupported;
    std::uint32_t payload=plist::none;
    std::int64_t major=0,minor=0,result_code=0;
    std::string_view message;
};
bool valid_relative_backup_path(std::string_view);

// Receive-side state machine only. It does not send a version reply or a
// restore command. Caller acknowledges actions only AFTER actually completing
// its outbound reply/file transfer/status response in the transport layer.
class SessionObserver {
public:
    SessionError consume(const plist::Document&,SessionEvent&);
    SessionError acknowledge_version(bool accepted);
    SessionError mark_operation_started();
    SessionError complete_request(bool success);
    void transport_failed();
    void reset() { *this=SessionObserver{}; }
    Phase phase() const { return phase_; }
    Outcome outcome() const { return outcome_; }
    EventKind pending_request() const {return phase_==Phase::request_pending?pending_:EventKind::unsupported;}
private:
    Phase phase_=Phase::await_version;
    Outcome outcome_=Outcome::none;
    EventKind pending_=EventKind::unsupported;
};
// Bounded diagnostic of selected response fields; control characters are
// sanitized. Content is described by type/size only, never dumped. No ownership.
std::size_t format_process_message_diagnostic(const plist::Document&,std::uint32_t payload,char*,std::size_t capacity);
const char* phase_name(Phase);
const char* session_error_name(SessionError);
} // namespace hcc::backup
