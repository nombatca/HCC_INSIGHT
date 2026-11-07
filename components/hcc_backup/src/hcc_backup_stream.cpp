#include "hcc_backup_stream.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace hcc::backup {
std::uint32_t read_be32(const std::uint8_t* b) {
    return (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) |
           (std::uint32_t(b[2]) << 8) | std::uint32_t(b[3]);
}
void write_be32(std::uint8_t* b, std::uint32_t v) {
    b[0] = std::uint8_t(v >> 24); b[1] = std::uint8_t(v >> 16);
    b[2] = std::uint8_t(v >> 8); b[3] = std::uint8_t(v);
}
void FileFrameWriter::reset() { *this = FileFrameWriter{}; }
Error FileFrameWriter::begin(const std::uint8_t* b, std::size_t n, std::uint8_t tag) {
    if (started_ && !done_) return Error::busy;
    if (n && !b) return Error::invalid_argument;
    if (n > std::numeric_limits<std::uint32_t>::max() - 1u) return Error::too_large;
    reset(); started_ = true; payload_ = b; size_ = n;
    write_be32(prefix_, std::uint32_t(n + 1)); prefix_[4] = tag;
    return Error::ok;
}
Error FileFrameWriter::begin_data(const std::uint8_t* b, std::size_t n) {
    if (!n) return Error::invalid_argument;
    return begin(b, n, 0x0c);
}
Error FileFrameWriter::begin_file_end() { return begin(nullptr, 0, 0x00); }
Error FileFrameWriter::pump(Sink sink) {
    if (!started_ || !sink.write) return Error::invalid_argument;
    if (error_ != Error::ok) return error_;
    while (!done_) {
        const bool prefix = prefix_sent_ < sizeof(prefix_);
        const auto* data = prefix ? prefix_ + prefix_sent_ : payload_ + payload_sent_;
        const auto remaining = prefix ? sizeof(prefix_) - prefix_sent_ : size_ - payload_sent_;
        if (!remaining) { done_ = true; return Error::ok; }
        const auto result = sink.write(sink.context, data, remaining);
        if (result.bytes > remaining) return error_ = Error::io_error;
        (prefix ? prefix_sent_ : payload_sent_) += result.bytes;
        if (result.state == IoState::closed) return error_ = Error::closed;
        if (result.state == IoState::error) return error_ = Error::io_error;
        if (prefix_sent_ == sizeof(prefix_) && payload_sent_ == size_) done_ = true;
        if (result.state == IoState::would_block)
            return done_ ? Error::ok : Error::would_block;
        if (!result.bytes) return error_ = Error::no_progress;
    }
    return Error::ok;
}

FrameDecoder::FrameDecoder(std::uint8_t* b, std::size_t n) : storage_(b), capacity_(n) {
    if (!b && n) error_ = Error::invalid_argument;
}
void FrameDecoder::next_frame() {
    header_size_ = payload_size_ = length_ = 0; ready_ = false;
    // Failures are sticky; construct a fresh decoder for a fresh connection.
}
Error FrameDecoder::feed(const std::uint8_t* b, std::size_t n, std::size_t& consumed) {
    consumed = 0;
    if (error_ != Error::ok) return error_;
    if (!b && n) return Error::invalid_argument;
    if (ready_) return Error::busy;
    while (consumed < n) {
        if (header_size_ < 4) {
            const auto k = std::min(n - consumed, 4 - header_size_);
            std::memcpy(header_ + header_size_, b + consumed, k);
            header_size_ += k; consumed += k;
            if (header_size_ != 4) continue;
            length_ = read_be32(header_);
            if (length_ > capacity_) return error_ = Error::too_large;
            if (!length_) { ready_ = true; return Error::ok; }
        }
        const auto k = std::min(n - consumed, std::size_t(length_) - payload_size_);
        if (k) std::memcpy(storage_ + payload_size_, b + consumed, k);
        payload_size_ += k; consumed += k;
        if (payload_size_ == length_) { ready_ = true; return Error::ok; }
    }
    return Error::ok;
}
Error FrameDecoder::end_input() const {
    if (error_ != Error::ok) return error_;
    return !ready_ && (header_size_ || payload_size_) ? Error::truncated : Error::ok;
}
Error inspect_file_frame(const std::uint8_t* b, std::size_t n, FileFrameView& v) {
    if (!b || !n) return Error::invalid_frame;
    if (b[0] == 0 && n == 1) { v = {FileFrameKind::end, nullptr, 0}; return Error::ok; }
    if (b[0] == 0x0c && n > 1) { v = {FileFrameKind::data, b+1, n-1}; return Error::ok; }
    return Error::invalid_frame;
}

BlobSender::BlobSender(std::uint8_t* b, std::size_t n) : scratch_(b), capacity_(n) {}
Error BlobSender::fail(Error e) { state_ = State::failed; return error_ = e; }
Error BlobSender::start(Source s) {
    if (state_ != State::idle && state_ != State::done && state_ != State::failed) return Error::busy;
    if (!scratch_ || !capacity_ || capacity_ > kObservedChunkSize || !s.read) return Error::invalid_argument;
    source_ = s; writer_.reset(); state_ = State::fill;
    error_ = Error::ok; completed_ = 0; pending_size_ = 0; eof_after_data_ = false;
    return Error::ok;
}
Error BlobSender::pump(Sink sink) {
    if (state_ == State::idle) return Error::invalid_argument;
    if (state_ == State::failed) return error_;
    if (!sink.write) return fail(Error::invalid_argument);
    for (;;) {
        if (state_ == State::done) return Error::ok;
        if (state_ == State::fill) {
            const auto r = source_.read(source_.context, scratch_, capacity_);
            if (r.bytes > capacity_) return fail(Error::io_error);
            if (r.error == Error::would_block) {
                if (r.bytes || r.eof) return fail(Error::io_error);
                return Error::would_block;
            }
            if (r.error != Error::ok) return fail(r.error);
            if (!r.bytes && !r.eof) return fail(Error::no_progress);
            pending_size_ = r.bytes; eof_after_data_ = r.eof;
            const auto e = r.bytes ? writer_.begin_data(scratch_, r.bytes) : writer_.begin_file_end();
            if (e != Error::ok) return fail(e);
            state_ = r.bytes ? State::data : State::end;
        }
        const auto e = writer_.pump(sink);
        if (e == Error::would_block) return e;
        if (e != Error::ok) return fail(e);
        if (state_ == State::end) { state_ = State::done; return Error::ok; }
        completed_ += pending_size_;
        if (eof_after_data_) {
            const auto end = writer_.begin_file_end();
            if (end != Error::ok) return fail(end);
            state_ = State::end;
        } else state_ = State::fill;
    }
}
const char* error_name(Error e) {
    switch (e) {
    case Error::ok: return "ok"; case Error::invalid_argument: return "invalid_argument";
    case Error::busy: return "busy"; case Error::would_block: return "would_block";
    case Error::no_progress: return "no_progress"; case Error::closed: return "closed";
    case Error::io_error: return "io_error"; case Error::too_large: return "too_large";
    case Error::truncated: return "truncated"; case Error::invalid_frame: return "invalid_frame";
    }
    return "unknown";
}
} // namespace hcc::backup
