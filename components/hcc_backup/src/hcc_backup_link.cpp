#include "hcc_backup_link.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace hcc::backup {
namespace {
class BinaryWriter {
public:
    BinaryWriter(std::uint8_t* b,std::size_t n):bytes_(b),capacity_(n) {
        if(!b) { ok_=false; return; }
        const char header[]="bplist00";
        for(unsigned i=0;i<8;++i) put(std::uint8_t(header[i]));
    }
    bool object() {
        if(count_>=16 || position_>UINT16_MAX) return ok_=false;
        offsets_[count_++]=std::uint16_t(position_); return ok_;
    }
    void put(std::uint8_t v) {
        if(!ok_ || position_>=capacity_) { ok_=false; return; }
        bytes_[position_++]=v;
    }
    void number(std::uint64_t v,unsigned width) {
        for(unsigned i=width;i>0;--i) put(std::uint8_t(v>>((i-1)*8)));
    }
    void integer(std::uint64_t v) {
        unsigned power=v<=255?0:v<=65535?1:v<=UINT32_MAX?2:3;
        put(std::uint8_t(0x10|power)); number(v,1u<<power);
    }
    void string(std::string_view s) {
        if(s.size()>255) { ok_=false; return; }
        if(s.size()<15) put(std::uint8_t(0x50|s.size()));
        else { put(0x5f); integer(s.size()); }
        for(char c:s) { if(static_cast<unsigned char>(c)>=128) ok_=false; put(std::uint8_t(c)); }
    }
    void real(double v) { std::uint64_t bits; std::memcpy(&bits,&v,8); put(0x23); number(bits,8); }
    LinkError finish(std::size_t& written) {
        written=0;
        const auto table=position_;
        for(unsigned i=0;i<count_;++i) number(offsets_[i],2);
        for(unsigned i=0;i<6;++i) put(0);
        put(2); put(1); number(count_,8); number(0,8); number(table,8);
        if(!ok_) return LinkError::too_large;
        written=position_; return LinkError::ok;
    }
private:
    std::uint8_t* bytes_;std::size_t capacity_,position_=0;
    std::uint16_t offsets_[16]{};unsigned count_=0;bool ok_=true;
};
}
LinkError encode_version_reply(std::uint64_t major,std::uint8_t* b,std::size_t n,std::size_t& written) {
    written=0; if(!b || major>INT64_MAX) return LinkError::invalid_argument;
    BinaryWriter w(b,n);
    w.object();w.put(0xa3);w.put(1);w.put(2);w.put(3);
    w.object();w.string("DLMessageVersionExchange");
    w.object();w.string("DLVersionsOk");
    w.object();w.integer(major);
    return w.finish(written);
}
LinkError encode_hello(bool v20,bool v21,std::uint8_t* b,std::size_t n,std::size_t& written) {
    written=0; if(!b || (!v20&&!v21)) return LinkError::invalid_argument;
    BinaryWriter w(b,n);
    w.object();w.put(0xa2);w.put(1);w.put(2);
    w.object();w.string("DLMessageProcessMessage");
    w.object();w.put(0xd2);w.put(3);w.put(4);w.put(5);w.put(6);
    w.object();w.string("MessageName");
    w.object();w.string("SupportedProtocolVersions");
    w.object();w.string("Hello");
    w.object();w.put(v20&&v21?0xa2:0xa1);w.put(7);if(v20&&v21)w.put(8);
    w.object();w.real(v20?2.0:2.1);
    if(v20&&v21) { w.object();w.real(2.1); }
    return w.finish(written);
}
LinkError encode_backup_request(std::string_view udid,bool full,std::uint8_t* b,std::size_t n,std::size_t& written) {
    written=0;
    if(!b || (udid.size()!=25&&udid.size()!=40)) return LinkError::invalid_argument;
    for(std::size_t i=0;i<udid.size();++i) {
        if(udid.size()==25&&i==8) {if(udid[i]!='-')return LinkError::invalid_argument;continue;}
        const char c=udid[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return LinkError::invalid_argument;
    }
    BinaryWriter w(b,n);
    w.object();w.put(0xa2);w.put(1);w.put(2);
    w.object();w.string("DLMessageProcessMessage");
    w.object();w.put(full?0xd4:0xd3);w.put(3);w.put(4);w.put(5);if(full)w.put(8);
    w.put(6);w.put(7);w.put(7);if(full)w.put(9);
    w.object();w.string("MessageName");
    w.object();w.string("TargetIdentifier");
    w.object();w.string("SourceIdentifier");
    w.object();w.string("Backup");
    w.object();w.string(udid);
    if(full) {
        w.object();w.string("Options");
        w.object();w.put(0xd1);w.put(10);w.put(11);
        w.object();w.string("ForceFullBackup");
        w.object();w.put(0x09);
    }
    return w.finish(written);
}
LinkError encode_status_response(std::int64_t code,const char* message,bool dict,
    std::uint8_t* b,std::size_t n,std::size_t& written) {
    written=0;
    if(!b) return LinkError::invalid_argument;
    std::size_t len=0;
    if(message) {
        while(len<=255 && message[len]) {
            if(static_cast<unsigned char>(message[len])>=128) return LinkError::invalid_argument;
            ++len;
        }
        if(len>255) return LinkError::too_large;
    }
    BinaryWriter w(b,n);
    w.object();w.put(0xa4);w.put(1);w.put(2);w.put(3);w.put(4);
    w.object();w.string("DLMessageStatusResponse");
    // Negative status codes use a full-width two's-complement plist integer.
    w.object();w.put(0x13);w.number(static_cast<std::uint64_t>(code),8);
    w.object();w.string(message?std::string_view(message,len):"___EmptyParameterString___");
    w.object();if(dict) w.put(0xd0);else w.string("___EmptyParameterString___");
    return w.finish(written);
}
Error StatusReplyWriter::begin(std::int64_t code,const char* message,bool dict) {
    if(active_) return Error::busy;
    done_=false;size_=sent_=0;error_=Error::ok;
    if(!storage_ || capacity_<4) return error_=Error::invalid_argument;
    std::size_t body=0;
    const auto e=encode_status_response(code,message,dict,storage_+4,capacity_-4,body);
    if(e!=LinkError::ok) return error_=e==LinkError::too_large?Error::too_large:Error::invalid_argument;
    if(body>UINT32_MAX) return error_=Error::too_large;
    write_be32(storage_,static_cast<std::uint32_t>(body));size_=body+4;active_=true;
    return Error::ok;
}
Error StatusReplyWriter::pump(Sink sink) {
    if(error_!=Error::ok) return error_;
    if(done_) return Error::ok;
    if(!active_ || !sink.write) return Error::invalid_argument;
    const auto remaining=size_-sent_;
    const auto r=sink.write(sink.context,storage_+sent_,remaining);
    auto fail=[this](Error e) {active_=false;return error_=e;};
    if(r.bytes>remaining) return fail(Error::io_error);
    sent_+=r.bytes;
    if(r.state==IoState::error) return fail(Error::io_error);
    if(r.state==IoState::closed) return fail(Error::closed);
    if(sent_==size_) {active_=false;done_=true;return Error::ok;}
    if(r.state==IoState::would_block) return Error::would_block;
    if(!r.bytes) return fail(Error::no_progress);
    return Error::would_block;
}
BackupLink::BackupLink(LinkBuffers b):buffers_(b),decoder_(b.receive,b.receive_capacity) {}
LinkError BackupLink::fail(LinkError e) { phase_=LinkPhase::failed;observer_.transport_failed();return error_=e; }
LinkError BackupLink::start(std::uint64_t now,LinkConfig config) {
    if(phase_!=LinkPhase::idle&&phase_!=LinkPhase::failed&&phase_!=LinkPhase::ready) return LinkError::wrong_state;
    if(!buffers_.receive||!buffers_.receive_capacity||!buffers_.transmit||buffers_.transmit_capacity<4||
       !buffers_.plist.nodes||!buffers_.plist.node_capacity||!buffers_.plist.text||!buffers_.plist.text_capacity||
       !config.inactivity_timeout_ms||(!config.offer_20&&!config.offer_21)||config.major>INT64_MAX||config.minor>INT64_MAX)
        return LinkError::invalid_argument;
    config_=config;decoder_=FrameDecoder(buffers_.receive,buffers_.receive_capacity);observer_.reset();
    phase_=LinkPhase::await_version;error_=LinkError::ok;tx_size_=tx_sent_=0;
    last_now_=last_progress_=now;received_=sent_=0;event_partial_=false;protocol_version_=0;remote_code_=0;
    return LinkError::ok;
}
LinkError BackupLink::handle_frame() {
    plist::Document doc;
    const auto pe=plist::parse(decoder_.data(),decoder_.size(),buffers_.plist,doc);
    if(pe!=plist::Error::ok) return fail(pe==plist::Error::too_large||pe==plist::Error::no_space?LinkError::too_large:LinkError::malformed);
    SessionEvent event;
    if(observer_.consume(doc,event)!=SessionError::ok) return fail(LinkError::malformed);
    if(phase_==LinkPhase::await_version) {
        if(event.kind!=EventKind::version_offer) return fail(LinkError::malformed);
        if(std::uint64_t(event.major)>config_.major ||
           (std::uint64_t(event.major)==config_.major&&std::uint64_t(event.minor)>config_.minor)) return fail(LinkError::incompatible_version);
        std::size_t n=0;
        const auto e=encode_version_reply(config_.major,buffers_.transmit+4,buffers_.transmit_capacity-4,n);
        if(e!=LinkError::ok) return fail(e);
        write_be32(buffers_.transmit,std::uint32_t(n));tx_size_=n+4;tx_sent_=0;phase_=LinkPhase::send_version;
    } else if(phase_==LinkPhase::await_ready) {
        if(event.kind!=EventKind::device_ready) return fail(LinkError::malformed);
        std::size_t n=0;
        const auto e=encode_hello(config_.offer_20,config_.offer_21,buffers_.transmit+4,buffers_.transmit_capacity-4,n);
        if(e!=LinkError::ok) return fail(e);
        write_be32(buffers_.transmit,std::uint32_t(n));tx_size_=n+4;tx_sent_=0;phase_=LinkPhase::send_hello;
    } else if(phase_==LinkPhase::await_hello) {
        if(event.kind!=EventKind::process_message) return fail(LinkError::malformed);
        const auto dict=event.payload;
        if(doc.string(doc.find(dict,"MessageName"))!="Response") return fail(LinkError::malformed);
        const auto* code=doc.get(doc.find(dict,"ErrorCode"));
        if(!code||code->kind!=plist::Kind::integer) return fail(LinkError::malformed);
        remote_code_=code->integer;
        if(remote_code_) return fail(remote_code_==1?LinkError::incompatible_version:LinkError::remote_error);
        const auto* version=doc.get(doc.find(dict,"ProtocolVersion"));
        if(!version||version->kind!=plist::Kind::real) return fail(LinkError::malformed);
        const auto v=version->real;
        if(!((config_.offer_20&&std::abs(v-2.0)<1e-12)||(config_.offer_21&&std::abs(v-2.1)<1e-12))) return fail(LinkError::incompatible_version);
        protocol_version_=v;phase_=LinkPhase::ready;
    } else return fail(LinkError::wrong_state);
    decoder_.next_frame();return LinkError::ok;
}
LinkError BackupLink::finish_write() {
    if(phase_==LinkPhase::send_version) {
        if(observer_.acknowledge_version(true)!=SessionError::ok) return fail(LinkError::wrong_state);
        phase_=LinkPhase::await_ready;
    } else if(phase_==LinkPhase::send_hello) phase_=LinkPhase::await_hello;
    else return fail(LinkError::wrong_state);
    tx_size_=tx_sent_=0;return LinkError::ok;
}
LinkError BackupLink::poll(Duplex io,std::uint64_t now,std::size_t budget) {
    if(phase_==LinkPhase::idle) return LinkError::wrong_state;
    if(phase_==LinkPhase::failed) return error_;
    if(phase_==LinkPhase::ready) return LinkError::ok;
    if(!io.read||!io.write||!budget) return LinkError::invalid_argument;
    if(now<last_now_) return fail(LinkError::clock_error);
    last_now_=now;
    if(now-last_progress_>=config_.inactivity_timeout_ms) return fail(LinkError::timeout);
    for(std::size_t call=0;call<budget;++call) {
        const bool writing=phase_==LinkPhase::send_version||phase_==LinkPhase::send_hello;
        const auto request=writing?tx_size_-tx_sent_:std::min<std::size_t>(sizeof(io_buffer_),decoder_.bytes_needed());
        if(!request) return fail(LinkError::wrong_state);
        const auto r=writing?io.write(io.context,buffers_.transmit+tx_sent_,request):io.read(io.context,io_buffer_,request);
        if(r.bytes>request) return fail(LinkError::io_error);
        if(r.bytes) { last_progress_=now;
            if(writing) {tx_sent_+=r.bytes;sent_+=r.bytes;}
            else {received_+=r.bytes;std::size_t consumed=0;
                const auto e=decoder_.feed(io_buffer_,r.bytes,consumed);
                if(e!=Error::ok||consumed!=r.bytes) return fail(e==Error::too_large?LinkError::too_large:LinkError::malformed);
            }
        }
        if(r.state==IoState::error) return fail(LinkError::io_error);
        if(r.state==IoState::closed) return fail(!writing&&decoder_.end_input()==Error::truncated?LinkError::truncated:LinkError::closed);
        if(writing&&tx_sent_==tx_size_) {const auto e=finish_write();if(e!=LinkError::ok)return e;}
        if(!writing&&decoder_.ready()) {const auto e=handle_frame();if(e!=LinkError::ok)return e;}
        if(phase_==LinkPhase::ready) return LinkError::ok;
        if(r.state==IoState::would_block) return LinkError::would_block;
        if(!r.bytes) return fail(LinkError::no_progress);
    }
    return LinkError::yield;
}
LinkError BackupLink::receive_event(Duplex io,std::uint64_t now,plist::Document& doc,
    SessionEvent& event,bool& available,std::size_t budget) {
    available=false;doc={};event={};
    if(phase_==LinkPhase::failed) return error_;
    if(phase_!=LinkPhase::ready) return LinkError::wrong_state;
    if(!io.read || !budget) return LinkError::invalid_argument;
    if(now<last_now_) return fail(LinkError::clock_error);
    last_now_=now;
    if(event_partial_ && now-last_progress_>=config_.inactivity_timeout_ms) return fail(LinkError::timeout);
    for(std::size_t call=0;call<budget;++call) {
        const auto request=std::min<std::size_t>(sizeof(io_buffer_),decoder_.bytes_needed());
        if(!request) return fail(LinkError::wrong_state);
        const auto r=io.read(io.context,io_buffer_,request);
        if(r.bytes>request) return fail(LinkError::io_error);
        if(r.bytes) {
            received_+=r.bytes;last_progress_=now;event_partial_=true;
            std::size_t used=0;const auto e=decoder_.feed(io_buffer_,r.bytes,used);
            if(e!=Error::ok || used!=r.bytes) return fail(e==Error::too_large?LinkError::too_large:LinkError::malformed);
        }
        if(r.state==IoState::error) return fail(LinkError::io_error);
        if(r.state==IoState::closed) return fail(decoder_.end_input()==Error::truncated?LinkError::truncated:LinkError::closed);
        if(decoder_.ready()) {
            const auto e=plist::parse(decoder_.data(),decoder_.size(),buffers_.plist,doc);
            if(e!=plist::Error::ok) return fail(e==plist::Error::too_large||e==plist::Error::no_space?LinkError::too_large:LinkError::malformed);
            if(observer_.consume(doc,event)!=SessionError::ok) {doc={};event={};return fail(LinkError::malformed);}
            decoder_.next_frame();event_partial_=false;available=true;return LinkError::ok;
        }
        if(r.state==IoState::would_block) return LinkError::would_block;
        if(!r.bytes) return fail(LinkError::no_progress);
    }
    return LinkError::yield;
}
const char* link_phase_name(LinkPhase p) {
    switch(p) {
    case LinkPhase::idle:return "idle";case LinkPhase::await_version:return "await_version";
    case LinkPhase::send_version:return "send_version";case LinkPhase::await_ready:return "await_ready";
    case LinkPhase::send_hello:return "send_hello";case LinkPhase::await_hello:return "await_hello";
    case LinkPhase::ready:return "ready";case LinkPhase::failed:return "failed";
    } return "unknown";
}
const char* link_error_name(LinkError e) {
    switch(e) {
    case LinkError::ok:return "ok";case LinkError::would_block:return "would_block";
    case LinkError::yield:return "yield";case LinkError::invalid_argument:return "invalid_argument";
    case LinkError::wrong_state:return "wrong_state";case LinkError::timeout:return "timeout";
    case LinkError::closed:return "closed";case LinkError::truncated:return "truncated";
    case LinkError::io_error:return "io_error";case LinkError::no_progress:return "no_progress";
    case LinkError::too_large:return "too_large";case LinkError::malformed:return "malformed";
    case LinkError::incompatible_version:return "incompatible_version";case LinkError::remote_error:return "remote_error";
    case LinkError::clock_error:return "clock_error";
    } return "unknown";
}
} // namespace hcc::backup
