#pragma once
#include "hcc_backup_stream.hpp"
#include "hcc_backup_session.hpp"

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
                       clock_error };
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

// Negotiates DeviceLink + mobilebackup2 application versions on an ALREADY
// connected, paired, service-specific byte stream. Does not create USBMUX/TLS,
// send restore requests or consume messages beyond the Hello response.
class BackupLink {
public:
    explicit BackupLink(LinkBuffers buffers);
    LinkError start(std::uint64_t now_ms,LinkConfig config={});
    LinkError poll(Duplex,std::uint64_t now_ms,std::size_t max_io_calls=16);
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
    double protocol_version_=0;
    std::int64_t remote_code_=0;
};
const char* link_phase_name(LinkPhase);
const char* link_error_name(LinkError);
} // namespace hcc::backup
