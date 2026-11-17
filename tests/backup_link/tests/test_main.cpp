#include "hcc_backup_stream.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace hcc::backup;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)

struct Output {
    std::vector<std::uint8_t> bytes;
    std::size_t limit = 1000000, calls = 0, block_every = 0;
    std::size_t close_at = std::numeric_limits<std::size_t>::max();
    bool block_after_write = false, zero_ok = false;
    static IoResult write(void* ctx, const std::uint8_t* b, std::size_t n) {
        auto& s = *static_cast<Output*>(ctx); ++s.calls;
        if (s.zero_ok) return {0, IoState::ok};
        if (s.bytes.size() >= s.close_at) return {0, IoState::closed};
        if (s.block_every && s.calls % s.block_every == 0) return {0, IoState::would_block};
        const auto take = std::min({n, s.limit, s.close_at - s.bytes.size()});
        s.bytes.insert(s.bytes.end(), b, b+take);
        return {take, s.block_after_write ? IoState::would_block : IoState::ok};
    }
    Sink sink() { return {this, write}; }
};
struct Input {
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0, limit = kObservedChunkSize;
    std::size_t fail_at = std::numeric_limits<std::size_t>::max();
    bool block_once = false, zero_ok = false;
    static SourceResult read(void* ctx, std::uint8_t* b, std::size_t cap) {
        auto& s = *static_cast<Input*>(ctx);
        if (s.block_once) { s.block_once = false; return {0,false,Error::would_block}; }
        if (s.zero_ok) return {0,false,Error::ok};
        if (s.offset >= s.fail_at) return {0,false,Error::io_error};
        const auto take = std::min({cap, s.limit, s.bytes.size()-s.offset, s.fail_at-s.offset});
        if (take) std::memcpy(b,s.bytes.data()+s.offset,take);
        s.offset += take;
        return {take,s.offset==s.bytes.size(),Error::ok};
    }
    Source source() { return {this,read}; }
};
std::vector<std::uint8_t> pattern(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i=0;i<n;++i) v[i]=std::uint8_t((i*37+11)&255);
    return v;
}
void finish(FileFrameWriter& w, Output& out) {
    for (std::size_t i=0;i<1000000;++i) {
        const auto e=w.pump(out.sink());
        if (e==Error::ok) { CHECK(w.done()); return; }
        CHECK(e==Error::would_block);
    }
    CHECK(false);
}
void golden_frames() {
    Output out; FileFrameWriter w;
    const std::uint8_t data[]={0xaa,0xbb,0xcc};
    CHECK(w.begin_data(data,3)==Error::ok); finish(w,out);
    CHECK(out.bytes==std::vector<std::uint8_t>({0,0,0,4,0x0c,0xaa,0xbb,0xcc}));
    CHECK(w.begin_file_end()==Error::ok); finish(w,out);
    CHECK(out.bytes==std::vector<std::uint8_t>({0,0,0,4,0x0c,0xaa,0xbb,0xcc,0,0,0,1,0}));
}
void short_writes_and_backpressure() {
    const auto data=pattern(1001);
    for (std::size_t limit : {1u,2u,3u,5u,127u}) {
        Output out; out.limit=limit; out.block_every=4; out.block_after_write=true;
        FileFrameWriter w; CHECK(w.begin_data(data.data(),data.size())==Error::ok);
        finish(w,out);
        CHECK(out.bytes.size()==data.size()+5);
        CHECK(read_be32(out.bytes.data())==data.size()+1);
        CHECK(out.bytes[4]==0x0c);
        CHECK(std::equal(data.begin(),data.end(),out.bytes.begin()+5));
    }
}
void sink_errors_are_sticky() {
    std::uint8_t b=9; FileFrameWriter w; Output out;
    out.close_at=3;
    CHECK(w.begin_data(&b,1)==Error::ok);
    CHECK(w.pump(out.sink())==Error::closed);
    const auto size=out.bytes.size(); CHECK(size==3);
    CHECK(w.pump(out.sink())==Error::closed); CHECK(out.bytes.size()==size);
    w.reset(); Output zero; zero.zero_ok=true;
    CHECK(w.begin_file_end()==Error::ok);
    CHECK(w.pump(zero.sink())==Error::no_progress);
}
void decoder_all_split_positions() {
    const std::vector<std::uint8_t> frame={0,0,0,4,0x0c,0xaa,0xbb,0xcc};
    for (std::size_t split=0;split<=frame.size();++split) {
        std::array<std::uint8_t,4> storage{}; FrameDecoder d(storage.data(),storage.size());
        std::size_t n=99; CHECK(d.feed(frame.data(),split,n)==Error::ok); CHECK(n==split);
        if (split<frame.size()) {
            CHECK(!d.ready()); CHECK(d.feed(frame.data()+split,frame.size()-split,n)==Error::ok);
            CHECK(n==frame.size()-split);
        }
        CHECK(d.ready()); CHECK(d.size()==4); CHECK(d.end_input()==Error::ok);
        FileFrameView v{}; CHECK(inspect_file_frame(d.data(),d.size(),v)==Error::ok);
        CHECK(v.kind==FileFrameKind::data && v.size==3 && v.data[0]==0xaa);
    }
}
void decoder_bounds_and_truncation() {
    const std::uint8_t huge[]={0xff,0xff,0xff,0xff};
    std::array<std::uint8_t,6> guard{}; guard.fill(0x6a);
    FrameDecoder d(guard.data()+1,4); std::size_t n=0;
    CHECK(d.feed(huge,4,n)==Error::too_large);
    CHECK(guard.front()==0x6a && guard.back()==0x6a);
    d.next_frame(); CHECK(d.feed(huge,1,n)==Error::too_large);
    const std::uint8_t frame[]={0,0,0,4,0x0c,1,2,3};
    for (std::size_t cut=1;cut<sizeof(frame);++cut) {
        std::array<std::uint8_t,4> b{}; FrameDecoder part(b.data(),b.size());
        CHECK(part.feed(frame,cut,n)==Error::ok); CHECK(part.end_input()==Error::truncated);
    }
}
void zero_length_and_invalid_file_frames() {
    std::uint8_t b[4]{}; FrameDecoder d(b,sizeof(b)); std::size_t n=0;
    CHECK(d.feed(b,4,n)==Error::ok); CHECK(d.ready() && d.size()==0);
    FileFrameView v{}; CHECK(inspect_file_frame(d.data(),d.size(),v)==Error::invalid_frame);
    const std::uint8_t eof[]={0}, bad[]={0,1}, empty_data[]={0x0c}, unknown[]={7};
    CHECK(inspect_file_frame(eof,1,v)==Error::ok && v.kind==FileFrameKind::end);
    CHECK(inspect_file_frame(bad,2,v)==Error::invalid_frame);
    CHECK(inspect_file_frame(empty_data,1,v)==Error::invalid_frame);
    CHECK(inspect_file_frame(unknown,1,v)==Error::invalid_frame);
}
void verify_wire(const std::vector<std::uint8_t>& wire,const std::vector<std::uint8_t>& expected) {
    std::array<std::uint8_t,kObservedChunkSize+1> b{};
    FrameDecoder d(b.data(),b.size()); std::vector<std::uint8_t> result;
    std::size_t offset=0, ends=0;
    while (offset<wire.size()) {
        std::size_t n=0;
        CHECK(d.feed(wire.data()+offset,std::min<std::size_t>(7,wire.size()-offset),n)==Error::ok);
        CHECK(n>0); offset+=n;
        if (d.ready()) {
            FileFrameView v{}; CHECK(inspect_file_frame(d.data(),d.size(),v)==Error::ok);
            if (v.kind==FileFrameKind::end) { ++ends; CHECK(offset==wire.size()); }
            else { CHECK(ends==0); result.insert(result.end(),v.data,v.data+v.size); }
            d.next_frame();
        }
    }
    CHECK(d.end_input()==Error::ok); CHECK(ends==1); CHECK(result==expected);
}
void streaming_sizes_and_short_reads() {
    for (std::size_t n : {0u,1u,31u,32767u,32768u,32769u,100001u}) {
        for (std::size_t read_limit : {13u,32768u}) {
            Input in; in.bytes=pattern(n); in.limit=read_limit; in.block_once=true;
            Output out; out.limit=103; out.block_every=7;
            std::array<std::uint8_t,kObservedChunkSize> scratch{};
            BlobSender sender(scratch.data(),scratch.size());
            CHECK(sender.start(in.source())==Error::ok);
            for (std::size_t tries=0;!sender.done() && tries<1000000;++tries) {
                const auto e=sender.pump(out.sink()); CHECK(e==Error::ok || e==Error::would_block);
            }
            CHECK(sender.done()); CHECK(sender.completed_payload_bytes()==n);
            verify_wire(out.bytes,in.bytes);
        }
    }
}
void source_failure_does_not_signal_success() {
    Input in; in.bytes=pattern(50000); in.fail_at=32768;
    Output out; std::array<std::uint8_t,kObservedChunkSize> scratch{};
    BlobSender sender(scratch.data(),scratch.size());
    CHECK(sender.start(in.source())==Error::ok); CHECK(sender.pump(out.sink())==Error::io_error);
    CHECK(!sender.done()); CHECK(out.bytes.size()==32768+5);
    CHECK(sender.pump(out.sink())==Error::io_error);
    Input zero; zero.zero_ok=true;
    CHECK(sender.start(zero.source())==Error::ok); CHECK(sender.pump(out.sink())==Error::no_progress);
}
void invalid_arguments_and_busy() {
    std::uint8_t b=1; FileFrameWriter w;
    CHECK(w.begin_data(nullptr,1)==Error::invalid_argument);
    CHECK(w.begin_data(&b,0)==Error::invalid_argument);
    CHECK(w.begin_data(&b,std::numeric_limits<std::uint32_t>::max())==Error::too_large);
    CHECK(w.begin_data(&b,1)==Error::ok); CHECK(w.begin_file_end()==Error::busy);
    Input in; BlobSender no_memory(nullptr,0); CHECK(no_memory.start(in.source())==Error::invalid_argument);
    std::array<std::uint8_t,4> buf{}; BlobSender sender(buf.data(),buf.size());
    CHECK(sender.start(in.source())==Error::ok); CHECK(sender.start(in.source())==Error::busy);
    CHECK(sender.pump({nullptr,nullptr})==Error::invalid_argument);
}
int main() {
    const struct { const char* name; void(*run)(); } tests[]={
        {"golden native-frame byte layouts",golden_frames},
        {"partial writes and backpressure",short_writes_and_backpressure},
        {"sticky sink failures",sink_errors_are_sticky},
        {"all frame split positions",decoder_all_split_positions},
        {"bounded lengths and truncated input",decoder_bounds_and_truncation},
        {"zero-length vs file-end and malformed frames",zero_length_and_invalid_file_frames},
        {"streaming boundary sizes and short reads",streaming_sizes_and_short_reads},
        {"source failures do not emit success terminator",source_failure_does_not_signal_success},
        {"invalid inputs and busy state",invalid_arguments_and_busy},
    };
    for (const auto& t:tests) { t.run(); std::cout<<"PASS: "<<t.name<<"\n"; }
    std::cout<<"9 test groups passed; no device connection performed.\n";
}
