#pragma once
#include "hcc_backup_stream.hpp"
#include "hcc_backup_session.hpp"
#include "hcc_backup_listing.hpp"

namespace hcc::backup {
struct Duplex {
    void* context;
    IoResult (*read)(void*,std::uint8_t*,std::size_t);
    IoResult (*write)(void*,const std::uint8_t*,std::size_t);
};
enum class LinkPhase { idle, await_version, send_version, await_ready,
                       send_hello, await_hello, ready, failed };
enum class LinkError { ok, would_block, yield, invalid_argument, wrong_state,
                       timeout, closed, truncated, io_error, no_progress,
                       too_large, malformed, incompatible_version, remote_error,
                       clock_error, unsupported_operation };
struct LinkConfig {
    std::uint64_t major=400,minor=0;
    bool offer_20=true,offer_21=true;
    std::uint64_t inactivity_timeout_ms=5000;
};
struct LinkBuffers {
    std::uint8_t* receive; std::size_t receive_capacity;
    std::uint8_t* transmit; std::size_t transmit_capacity;
    plist::Workspace plist;
};

// Binary-plist control messages, WITHOUT the 4-byte transport length prefix.
// On failure, written=0; callers must not send partially constructed output.
LinkError encode_version_reply(std::uint64_t major,std::uint8_t*,std::size_t,std::size_t& written);
LinkError encode_hello(bool offer_20,bool offer_21,std::uint8_t*,std::size_t,std::size_t& written);
// Backup only; both identifiers use the same validated device UDID. Optional
// ForceFullBackup is omitted unless true. No restore/encryption options exist.
LinkError encode_backup_request(std::string_view udid,bool force_full,
                                std::uint8_t*,std::size_t,std::size_t& written);

// Bounded status reply: ASCII message (0..255 bytes), signed status code,
// and either an empty dictionary or the DeviceLink empty-parameter sentinel.
// This does not acknowledge a SessionObserver request or initiate an operation.
LinkError encode_status_response(std::int64_t code,const char* message,
    bool empty_dictionary,std::uint8_t*,std::size_t,std::size_t& written);

// Success status with a nonnegative integer capacity (bounded to INT64_MAX).
LinkError encode_free_space_response(std::uint64_t bytes,std::uint8_t*,std::size_t,std::size_t& written);

// DLContentsOfDirectory success reply: a DLMessageStatusResponse whose fourth
// item is a dictionary mapping each immediate child name to a sub-dictionary of
// DLFileType ("DLFileTypeRegular"/"DLFileTypeDirectory"), DLFileSize and
// DLFileModificationDate (Mac-epoch date). An empty listing yields an empty
// dictionary with code 0. Entry names must be nonempty basenames (no separators).
// `offset_scratch` supplies at least contents_object_count(list.count) uint32
// slots. On any failure written=0 and nothing is sent; the listing is never
// partially encoded or silently truncated.
LinkError encode_contents_of_directory_response(const DirectoryListing& list,
    std::uint32_t* offset_scratch,std::size_t offset_capacity,
    std::uint8_t*,std::size_t,std::size_t& written);

// Caller owns storage exclusively until done/error. Do not move this object or
// modify storage while pending: TLS retries require stable address and length.
class StatusReplyWriter {
public:
    StatusReplyWriter(std::uint8_t* storage,std::size_t capacity):storage_(storage),capacity_(capacity) {}
    StatusReplyWriter(const StatusReplyWriter&)=delete;
    StatusReplyWriter& operator=(const StatusReplyWriter&)=delete;
    Error begin(std::int64_t code,const char* message=nullptr,bool empty_dictionary=true);
    Error begin_free_space(std::uint64_t bytes);
    // Frames a DLContentsOfDirectory reply for `list`. `offset_scratch` must hold
    // at least contents_object_count(list.count) uint32 slots and remain unused by
    // the caller until done/error. Encoding failures keep the writer inactive.
    Error begin_contents(const DirectoryListing& list,std::uint32_t* offset_scratch,std::size_t offset_capacity);
    Error pump(Sink);
    bool done() const { return done_; }
private:
    std::uint8_t* storage_;
    std::size_t capacity_,size_=0,sent_=0;
    bool active_=false,done_=false;
    Error error_=Error::ok;
};

// Negotiates DeviceLink + mobilebackup2 application versions on an ALREADY
// connected, paired, service-specific byte stream. Does not create USBMUX/TLS,
// send restore requests. poll() stops at Hello; receive_event() is explicit.
class BackupLink {
public:
    explicit BackupLink(LinkBuffers buffers);
    LinkError start(std::uint64_t now_ms,LinkConfig config={});
    LinkError poll(Duplex,std::uint64_t now_ms,std::size_t max_io_calls=16);
    // After Hello, receive at most one control event without writing. Returned
    // document/event views remain valid only until the next receive call.
    // Quiet idle has no deadline; an incomplete frame has the configured timeout.
    LinkError receive_event(Duplex,std::uint64_t now_ms,plist::Document&,
                            SessionEvent&,bool& available,std::size_t max_io_calls=8);
    bool control_frame_pending() const { return event_partial_; }
    LinkPhase phase() const { return phase_; }
    LinkError error() const { return error_; }
    double protocol_version() const { return protocol_version_; }
    std::int64_t remote_code() const { return remote_code_; }
    std::uint64_t received_bytes() const { return received_; }
    std::uint64_t sent_bytes() const { return sent_; }
    // Borrowed state for the application operation driver after negotiation.
    // Starting an operation still requires the caller to send its real request.
    SessionObserver* session_observer() {
        return phase_==LinkPhase::ready?&observer_:nullptr;
    }
private:
    LinkError fail(LinkError);
    LinkError handle_frame();
    LinkError finish_write();
    LinkBuffers buffers_;
    LinkConfig config_{};
    FrameDecoder decoder_;
    SessionObserver observer_;
    LinkPhase phase_=LinkPhase::idle;
    LinkError error_=LinkError::ok;
    std::size_t tx_size_=0,tx_sent_=0;
    std::uint8_t io_buffer_[256]{}; // Stable address across TLS WANT_READ/WRITE retries.
    std::uint64_t last_now_=0,last_progress_=0,received_=0,sent_=0;
    bool event_partial_=false;
    double protocol_version_=0;
    std::int64_t remote_code_=0;
};
const char* link_phase_name(LinkPhase);
const char* link_error_name(LinkError);
} // namespace hcc::backup
