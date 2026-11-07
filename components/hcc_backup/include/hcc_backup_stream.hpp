#pragma once
#include <cstddef>
#include <cstdint>

namespace hcc::backup {

constexpr std::size_t kObservedChunkSize = 32768;
enum class Error { ok, invalid_argument, busy, would_block, no_progress,
                   closed, io_error, too_large, truncated, invalid_frame };
enum class IoState { ok, would_block, closed, error };
struct IoResult { std::size_t bytes; IoState state; };
struct Sink {
    void* context;
    IoResult (*write)(void*, const std::uint8_t*, std::size_t);
};
struct SourceResult { std::size_t bytes; bool eof; Error error; };
struct Source {
    void* context;
    SourceResult (*read)(void*, std::uint8_t*, std::size_t);
};

std::uint32_t read_be32(const std::uint8_t* bytes);
void write_be32(std::uint8_t* bytes, std::uint32_t value);

// Payload storage belongs to the caller and remains valid until done/error.
// A would_block result preserves offsets; call pump again when writable.
class FileFrameWriter {
public:
    Error begin_data(const std::uint8_t* bytes, std::size_t size);
    Error begin_file_end();
    Error pump(Sink sink);
    bool done() const { return done_; }
    Error error() const { return error_; }
    void reset();
private:
    Error begin(const std::uint8_t*, std::size_t, std::uint8_t);
    std::uint8_t prefix_[5]{};
    const std::uint8_t* payload_ = nullptr;
    std::size_t size_ = 0, prefix_sent_ = 0, payload_sent_ = 0;
    bool started_ = false, done_ = false;
    Error error_ = Error::ok;
};

// Incremental, allocation-free length-prefixed frame decoder. Stops at one
// complete frame; callers use consumed to process remaining input separately.
class FrameDecoder {
public:
    FrameDecoder(std::uint8_t* storage, std::size_t capacity);
    Error feed(const std::uint8_t*, std::size_t, std::size_t& consumed);
    Error end_input() const;
    void next_frame();
    bool ready() const { return ready_; }
    std::uint32_t size() const { return length_; }
    std::size_t bytes_needed() const {
        return ready_ ? 0 : header_size_ < 4 ? 4-header_size_ : length_-payload_size_;
    }
    const std::uint8_t* data() const { return storage_; }
private:
    std::uint8_t header_[4]{};
    std::uint8_t* storage_;
    std::size_t capacity_, header_size_ = 0, payload_size_ = 0;
    std::uint32_t length_ = 0;
    bool ready_ = false;
    Error error_ = Error::ok;
};

enum class FileFrameKind { data, end };
struct FileFrameView {
    FileFrameKind kind;
    const std::uint8_t* data;
    std::size_t size;
};
Error inspect_file_frame(const std::uint8_t*, std::size_t, FileFrameView&);

// Streams ONE source file using caller-owned scratch memory. Does not open
// a connection, parse a backup, choose a device, or initiate a restore.
class BlobSender {
public:
    BlobSender(std::uint8_t* scratch, std::size_t capacity);
    Error start(Source source);
    Error pump(Sink sink);
    bool done() const { return state_ == State::done; }
    std::uint64_t completed_payload_bytes() const { return completed_; }
private:
    enum class State { idle, fill, data, end, done, failed };
    Error fail(Error error);
    std::uint8_t* scratch_;
    std::size_t capacity_, pending_size_ = 0;
    Source source_{};
    FileFrameWriter writer_;
    State state_ = State::idle;
    Error error_ = Error::ok;
    bool eof_after_data_ = false;
    std::uint64_t completed_ = 0;
};

const char* error_name(Error error);
} // namespace hcc::backup
