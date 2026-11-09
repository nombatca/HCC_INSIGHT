#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include "HCCBackupOperation.hpp"
#include <iterator>
#include <new>
#include <string_view>
#include <vector>

// Use the production transport declaration; only SDK types and I/O are stubbed.
#define private public
#include "HCCUSBTransport.hpp"
#undef private

namespace bb = hcc::backup;
namespace fs = std::filesystem;
static std::string sd_root;
void* bsp_sdcard=reinterpret_cast<void*>(1);
int bsp_sdcard_mount(){return 0;}
const char* hcc_test_mount_point(){return sd_root.c_str();}
int esp_vfs_fat_info(const char*,std::uint64_t* total,std::uint64_t* available){*total=*available=1073741824;return 0;}
static uint64_t now_ms = 0;
static uint64_t mdmNowMs() { return now_ms; }
static bool allocation_fails = false;
static size_t allocations = 0;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
static void *heap_caps_malloc(size_t n, int) {
    if (allocation_fails) return nullptr;
    ++allocations;
    return std::malloc(n);
}
static void heap_caps_free(void *p) { --allocations; std::free(p); }
static void mbedtls_platform_zeroize(void *p, size_t n) { std::memset(p, 0, n); }
constexpr int MBEDTLS_ERR_SSL_WANT_READ = -101;
constexpr int MBEDTLS_ERR_SSL_WANT_WRITE = -102;
constexpr int MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY = -103;

struct MockTls {
    std::vector<uint8_t> input, output;
    size_t offset = 0;
    const void *retry_read = nullptr, *retry_write = nullptr;
    size_t read_size = 0, write_size = 0;
    bool close = false;
    uint8_t* ring=nullptr; size_t *ring_head=nullptr,*ring_length=nullptr;
};
struct HCCUSBTransport::ServiceTlsContext {
    MockTls ssl; bool established = true;
    static constexpr size_t kRxCapacity=64*1024;
    uint8_t rx_fifo[kRxCapacity]{};
    std::size_t rx_head=0,rx_length=0,tx_length=0,tx_offset=0;
    bool closed=false,handshake_step_requested=false;
};
static int mbedtls_ssl_read(MockTls *s, unsigned char *b, size_t n) {
    if (s->close) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    if(s->ring){
        const auto take=std::min({n,size_t(4096),*s->ring_length});
        if(!take)return MBEDTLS_ERR_SSL_WANT_READ;
        for(size_t i=0;i<take;++i)b[i]=s->ring[(*s->ring_head+i)%(64*1024)];
        *s->ring_head=(*s->ring_head+take)%(64*1024);*s->ring_length-=take;
        return static_cast<int>(take);
    }
    if (!s->retry_read) {
        s->retry_read = b; s->read_size = n;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    assert(s->retry_read == b && s->read_size == n);
    const auto take = std::min({n, size_t(7), s->input.size() - s->offset});
    if (!take) return MBEDTLS_ERR_SSL_WANT_READ;
    std::memcpy(b, s->input.data() + s->offset, take);
    s->offset += take; s->retry_read = nullptr;
    return static_cast<int>(take);
}
static int mbedtls_ssl_write(MockTls *s, const unsigned char *b, size_t n) {
    if (!s->retry_write) {
        s->retry_write = b; s->write_size = n;
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    assert(s->retry_write == b && s->write_size == n);
    const size_t take = std::min(n, size_t(9));
    s->output.insert(s->output.end(), b, b + take); s->retry_write = nullptr;
    return static_cast<int>(take);
}
static std::vector<uint8_t> plain_output;
static bool send_fails = false;
bool HCCUSBTransport::sendServiceTcpPacket(TxKind kind, uint8_t flags,
                                         const uint8_t *data, size_t n) {
    assert(kind == TxKind::BackupPlaintext && flags == 0x10 && !_tx_in_flight);
    if (send_fails) return false;
    plain_output.insert(plain_output.end(), data, data + n);
    _tx_in_flight = true;
    return true;
}

// Compile and exercise the actual firmware glue, without compiling the rest of
// the USB controller or pretending that these SDK stubs are real TLS/USB.
#include "HCCMdmProbe.inc"
// Non-MDM draining is outside this host adapter; burst tests use the actual ring
// and operation parser with a transparent TLS read stub, not real decryption.
bool HCCUSBTransport::drainServiceTls(){return true;}
#include "HCCServiceTlsRx.inc"

static void add_frame(std::vector<uint8_t> &out, const char *name) {
    std::ifstream file(std::string("../backup_link/tests/fixtures/") + name, std::ios::binary);
    assert(file);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    uint8_t prefix[4]; bb::write_be32(prefix, static_cast<uint32_t>(bytes.size()));
    out.insert(out.end(), prefix, prefix + 4); out.insert(out.end(), bytes.begin(), bytes.end());
}
static std::vector<uint8_t> handshake() {
    std::vector<uint8_t> b;
    add_frame(b, "version.bplist"); add_frame(b, "ready.xml"); add_frame(b, "hello.bplist");
    return b;
}
static void prepare(HCCUSBTransport &t) {
    t._mdm_selected = t._mdm_service_active = true;
    t._service_tcp.state = HCCUSBTransport::TcpState::Connected;
    t._mdm_state.store(HCCUSBTransport::MdmState::Opening);
    assert(t.beginMdmProbe());
}
static void check_output(const std::vector<uint8_t> &out) {
    size_t offset = 0, count = 0;
    while (offset < out.size()) {
        assert(out.size() - offset >= 4);
        const auto n = bb::read_be32(out.data() + offset);
        assert(n <= out.size() - offset - 4);
        assert(n >= 8 && std::memcmp(out.data() + offset + 4, "bplist00", 8) == 0);
        offset += 4 + n; ++count;
    }
    assert(count == 2); // version reply and Hello only.
}
int main() {
    char temp[]="/tmp/hcc-mdm-glue-XXXXXX";assert(mkdtemp(temp));sd_root=temp;
    using State = HCCUSBTransport::MdmState;
    {
        HCCUSBTransport t;
        assert(t.requestMdmProbe() && t.mdmState() == State::Armed);
        assert(!t.requestMdmProbe() && !t.mdmPollingPending());
        t._mdm_state.store(State::Negotiating);
        assert(!t.requestMdmProbe() && t.mdmPollingPending());
        t._mdm_state.store(State::Failed); assert(t.requestMdmProbe());
    }
    std::puts("PASS: atomic arming and duplicate request handling");
    {
        HCCUSBTransport t; prepare(t); plain_output.clear();
        const auto input = handshake();
        assert(t.appendMdmPlaintext(input.data(), input.size()));
        const uint8_t sentinel[] = {0x11, 0x22};
        assert(t.appendMdmPlaintext(sentinel, sizeof(sentinel)));
        for (now_ms = 0; now_ms < 100 && t.mdmState() != State::Ready; ++now_ms) {
            t._tx_in_flight = false; assert(t.pollMdmProbe());
        }
        assert(t.mdmState() == State::Ready && t._notification_rx_stream_length == 2);
        assert(t._notification_rx_stream[0] == 0x11); check_output(plain_output);
        t.releaseMdmProbe(); t.releaseMdmProbe();
    }
    std::puts("PASS: plaintext service negotiation and no read beyond Hello");
    {
        HCCUSBTransport t;
        HCCUSBTransport::ServiceTlsContext tls; tls.ssl.input = handshake();
        tls.ssl.input.push_back(0x77); t._service_tls = &tls;
        t._notification_service_ssl = true; prepare(t);
        for (uint64_t i = 0; i < 2000 && t.mdmState() != State::Ready; ++i) {
            ++now_ms; assert(t.pollMdmProbe());
        }
        assert(t.mdmState() == State::Ready && tls.ssl.offset + 1 == tls.ssl.input.size());
        check_output(tls.ssl.output); t.releaseMdmProbe(); t._service_tls = nullptr;
    }
    std::puts("PASS: TLS bridge partial I/O and stable WANT retry arguments");
    {
        HCCUSBTransport t; prepare(t); now_ms += 5000;
        assert(!t.pollMdmProbe() && t.mdmState() == State::Failed); t.releaseMdmProbe();
        t._mdm_state.store(State::Opening); allocation_fails = true;
        assert(!t.beginMdmProbe() && t._mdm_probe == nullptr); allocation_fails = false;
        prepare(t); std::vector<uint8_t> oversized(t._notification_rx_stream.size() + 1);
        assert(!t.appendMdmPlaintext(oversized.data(), oversized.size())); t.releaseMdmProbe();
    }
    std::puts("PASS: timeout, PSRAM allocation failure and bounded plaintext input");
    {
        HCCUSBTransport t; prepare(t); auto input = handshake();
        assert(t.appendMdmPlaintext(input.data(), input.size())); send_fails = true;
        assert(!t.pollMdmProbe() && t.mdmState() == State::Failed); send_fails = false;
        t.releaseMdmProbe();
        HCCUSBTransport::ServiceTlsContext tls; tls.ssl.close = true;
        t._service_tls = &tls; t._notification_service_ssl = true; prepare(t);
        assert(!t.pollMdmProbe() && t.mdmState() == State::Failed);
        t.releaseMdmProbe(); t._service_tls = nullptr;
    }
    std::puts("PASS: send failure and TLS close never report success");
    {
        HCCUSBTransport t;prepare(t);plain_output.clear();auto bytes=handshake();
        assert(t.appendMdmPlaintext(bytes.data(),bytes.size()));
        for(unsigned i=0;i<100&&t.mdmState()!=State::Ready;++i){++now_ms;t._tx_in_flight=false;assert(t.pollMdmProbe());}
        assert(t.mdmState()==State::Ready&&t.mdmPollingPending());
        const auto sent=plain_output.size();now_ms+=60000;
        assert(t.pollMdmProbe()&&t.mdmState()==State::Ready);
        bytes.clear();add_frame(bytes,"disconnect.xml");
        for(auto byte:bytes){assert(t.appendMdmPlaintext(&byte,1));++now_ms;assert(t.pollMdmProbe());}
        assert(t.mdmState()==State::Disconnected&&!t.mdmPollingPending());
        assert(plain_output.size()==sent);assert(t.requestMdmProbe());t.releaseMdmProbe();
    }
    std::puts("PASS: P4 post-Hello idle, fragmented disconnect, no writes and rearming");
    {
        HCCUSBTransport t;HCCUSBTransport::ServiceTlsContext tls;tls.ssl.input=handshake();
        t._service_tls=&tls;t._notification_service_ssl=true;prepare(t);
        for(unsigned i=0;i<2000&&t.mdmState()!=State::Ready;++i){++now_ms;assert(t.pollMdmProbe());}
        assert(t.mdmState()==State::Ready);
        add_frame(tls.ssl.input,"disconnect.xml");
        for(unsigned i=0;i<2000&&t.mdmState()==State::Ready;++i){++now_ms;assert(t.pollMdmProbe());}
        assert(t.mdmState()==State::Disconnected);check_output(tls.ssl.output);
        t.releaseMdmProbe();t._service_tls=nullptr;
    }
    std::puts("PASS: P4 TLS post-Hello control reads preserve WANT retry arguments");
    for(unsigned mode=0;mode<13;++mode){
        HCCUSBTransport t;HCCUSBTransport::ServiceTlsContext tls;tls.ssl.input=handshake();
        t._service_tls=&tls;t._notification_service_ssl=true;prepare(t);
        assert(!t.requestMdmBackup()); // negotiation is not enough to claim an operation
        for(unsigned i=0;i<2000&&t.mdmState()!=State::Ready;++i){++now_ms;assert(t.pollMdmProbe());}
        std::strcpy(t._session_udid.data(),mode==1?"invalid":"00008101-001234567890ABCD");
        if(mode==0||mode==5||mode==12){
            auto& bytes=tls.ssl.input;
            auto length=[&](std::uint32_t n){std::uint8_t h[4];bb::write_be32(h,n);bytes.insert(bytes.end(),h,h+4);};
            auto name=[&](const char* s){length(std::strlen(s));bytes.insert(bytes.end(),s,s+std::strlen(s));};
            const char* xml="<plist><array><string>DLMessageUploadFiles</string><integer>0</integer><integer>0</integer><integer>5</integer></array></plist>";
            name(xml);name("/device/source");name("backup/glue.bin");
            length(6);bytes.push_back(0x0c);for(char c:std::string_view("hello"))bytes.push_back(c);
            if(mode==0){length(1);bytes.push_back(0x0b);length(0);name("<plist><array><string>DLMessageProcessMessage</string><dict><key>MessageName</key><string>Response</string><key>ErrorCode</key><integer>0</integer></dict></array></plist>");}
        }
        if(mode==7){
            const std::string result="<plist><array><string>DLMessageProcessMessage</string><dict><key>MessageName</key><string>Response</string><key>ErrorCode</key><integer>42</integer><key>ErrorDescription</key><string>synthetic error</string></dict></array></plist>";
            std::uint8_t length[4];bb::write_be32(length,result.size());tls.ssl.input.insert(tls.ssl.input.end(),length,length+4);tls.ssl.input.insert(tls.ssl.input.end(),result.begin(),result.end());
        }
        assert(t.requestMdmBackup()&&!t.requestMdmBackup());
        if(mode==4)tls.ssl.close=true;
        for(unsigned i=0;i<5000&&t.mdmBackupState()==HCCUSBTransport::MdmBackupState::running;++i){
            ++now_ms;const auto ok=t.pollMdmProbe();
            if((mode==8||mode==9||mode==10||mode==11)&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::waiting){
                const auto sent=tls.ssl.output.size();const auto read=tls.ssl.offset;
                t._tx_in_flight=true; // submitPendingTcp would skip all protocol I/O
                if(mode==9){now_ms+=5001;t.pollMdmBackupWatchdog();assert(t.mdmBackupState()==HCCUSBTransport::MdmBackupState::running);}
                if(mode==10)--now_ms;
                else if(mode==11)t._state=HCCUSBTransport::State::Failed;
                else now_ms+=60000;
                t.pollMdmBackupWatchdog();
                assert(tls.ssl.output.size()==sent&&tls.ssl.offset==read);break;
            }
            if(mode==2&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::waiting)now_ms+=60000;
            if(mode==6&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::waiting){
                t._notification_rx_stream_length=t._notification_rx_stream.size();
                const std::uint8_t byte=0;assert(!t.appendMdmPlaintext(&byte,1));break;
            }
            if((mode==5||mode==12)&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::receiving){
                bool staged=false;for(const auto& e:fs::recursive_directory_iterator(sd_root))if(e.path().extension()==".part")staged=true;
                if(staged){
                    if(mode==5)t.releaseMdmProbe();
                    else {t._tx_in_flight=true;now_ms+=60000;t.pollMdmBackupWatchdog();}
                    break;
                }
            }
            if(mode==3&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::waiting){t.releaseMdmProbe();break;}
            if(!ok)break;
        }
        assert(t.mdmBackupState()==(mode==0?HCCUSBTransport::MdmBackupState::done:HCCUSBTransport::MdmBackupState::failed));
        assert(HCCSdAccess::gate.owner()==bb::SdOwner::free&&!t._mdm_backup_requested.load());
        assert(!t.requestMdmBackup());
        for(const auto& e:fs::recursive_directory_iterator(sd_root))assert(e.path().extension()!=".part");
        if(mode==0){
            assert(t.mdmState()==State::Ready&&!t.mdmPollingPending());
            assert(t.mdmBackupFiles()==1);
            bool found=false;
            for(const auto& entry:fs::recursive_directory_iterator(sd_root)){
                if(entry.path().filename()=="payload.bin"){
                    std::ifstream f(entry.path(),std::ios::binary);std::string body{std::istreambuf_iterator<char>(f),{}};assert(body=="hello");found=true;
                }
            }
            assert(found);
            const auto before=tls.ssl.offset;assert(t.pollMdmProbe());assert(tls.ssl.offset==before);
        }
        t.releaseMdmProbe();t._service_tls=nullptr;
    }
    std::puts("PASS: backup results, teardown and independent deadlines release SD, including stalled TX; no stream restart");
    {
        HCCUSBTransport t;HCCUSBTransport::ServiceTlsContext tls;tls.ssl.input=handshake();
        t._service_tls=&tls;t._notification_service_ssl=true;prepare(t);
        for(unsigned i=0;i<2000&&t.mdmState()!=State::Ready;++i){++now_ms;assert(t.pollMdmProbe());}
        std::strcpy(t._session_udid.data(),"00008101-001234567890ABCD");
        assert(t.requestMdmBackup());
        for(unsigned i=0;i<2000;++i){assert(t.pollMdmProbe());if(t._mdm_probe->operation->phase()==HCCBackupPhase::waiting)break;}
        assert(t._mdm_probe->operation->phase()==HCCBackupPhase::waiting);
        tls.ssl.ring=tls.rx_fifo;tls.ssl.ring_head=&tls.rx_head;tls.ssl.ring_length=&tls.rx_length;
        std::vector<uint8_t> bytes;
        auto length=[&](uint32_t n){uint8_t h[4];bb::write_be32(h,n);bytes.insert(bytes.end(),h,h+4);};
        auto name=[&](const char* text){length(std::strlen(text));bytes.insert(bytes.end(),text,text+std::strlen(text));};
        // Large nonterminal control: exceeds both old 8KiB and 128-node arenas.
        std::string progress="<plist><array><string>DLMessageProcessMessage</string><dict><key>MessageName</key><string>Progress</string><key>Content</key><array>";
        for(unsigned i=0;i<300;++i)progress+="<string>synthetic-control-entry-012345678901234567890123456789</string>";
        progress+="</array></dict></array></plist>";
        assert(progress.size()>8192);name(progress.c_str());
        name("<plist><array><string>DLMessageUploadFiles</string><integer>0</integer><integer>0</integer><integer>0</integer></array></plist>");
        name("/device/source");name("backup/burst.bin");
        const size_t payload_size=512*1024;
        const auto transfer_began=now_ms;
        length(payload_size+1);bytes.push_back(0x0c);
        for(size_t i=0;i<payload_size;++i)bytes.push_back(static_cast<uint8_t>(i));
        length(1);bytes.push_back(0x00);length(0);
        // Same owner task remains inside RX delivery with shared USB TX busy.
        // No outer poll between back-to-back 32728-byte payloads (>64KiB total).
        t._tx_in_flight=true;
        for(size_t pos=0;pos<bytes.size();){
            const auto n=std::min(size_t(32728),bytes.size()-pos);
            now_ms+=40000; // Active reads every 40 seconds, lasting beyond ten minutes.
            assert(t.appendServiceTlsRx(bytes.data()+pos,n));pos+=n;
            assert(tls.rx_length<32728);
        }
        assert(now_ms-transfer_began>600000);
        assert(t.mdmBackupFiles()==1&&t.mdmBackupState()==HCCUSBTransport::MdmBackupState::running);
        t._tx_in_flight=false;
        for(unsigned i=0;i<1000&&t._mdm_probe->operation->phase()!=HCCBackupPhase::waiting;++i)assert(t.pollMdmProbe());
        assert(t._mdm_probe->operation->phase()==HCCBackupPhase::waiting);
        bytes.clear();name("<plist><array><string>DLMessageProcessMessage</string><dict><key>ErrorCode</key><integer>0</integer></dict></array></plist>");
        assert(t.appendServiceTlsRx(bytes.data(),bytes.size()));
        assert(t.mdmBackupState()==HCCUSBTransport::MdmBackupState::done);
        bool found=false;
        for(const auto& e:fs::recursive_directory_iterator(sd_root))if(e.path().filename()=="payload.bin"&&fs::file_size(e.path())==payload_size){
            std::ifstream f(e.path(),std::ios::binary);for(size_t i=0;i<payload_size;++i)assert(f.get()==static_cast<uint8_t>(i));found=true;
        }
        assert(found&&HCCSdAccess::gate.owner()==bb::SdOwner::free);
        t.releaseMdmProbe();t._service_tls=nullptr;
    }
    std::puts("PASS: production RX ring drains 512KiB past ten minutes with progress while USB TX busy; exact file, status and final result");
    {
        HCCUSBTransport t;HCCUSBTransport::ServiceTlsContext tls;
        t._service_tls=&tls;tls.established=false;
        std::vector<uint8_t> full(tls.kRxCapacity,0x5a);
        assert(t.appendServiceTlsRx(full.data(),full.size()));
        const uint8_t extra=0;
        assert(!t.appendServiceTlsRx(&extra,1)&&tls.rx_length==tls.kRxCapacity);
        assert(tls.rx_fifo[0]==0x5a&&tls.rx_fifo[tls.kRxCapacity-1]==0x5a);
        t._service_tls=nullptr;
    }
    std::puts("PASS: unconsumed RX remains bounded and overflow never overwrites ring data");
    fs::remove_all(sd_root);
    assert(allocations == 0);
    std::puts("10 firmware-glue groups passed using host SDK/I/O stubs; not a firmware build.");
}
