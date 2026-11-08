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
};
struct HCCUSBTransport::ServiceTlsContext { MockTls ssl; bool established = true; };
static int mbedtls_ssl_read(MockTls *s, unsigned char *b, size_t n) {
    if (s->close) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
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
    for(unsigned mode=0;mode<7;++mode){
        HCCUSBTransport t;HCCUSBTransport::ServiceTlsContext tls;tls.ssl.input=handshake();
        t._service_tls=&tls;t._notification_service_ssl=true;prepare(t);
        assert(!t.requestMdmBackup()); // negotiation is not enough to claim an operation
        for(unsigned i=0;i<2000&&t.mdmState()!=State::Ready;++i){++now_ms;assert(t.pollMdmProbe());}
        std::strcpy(t._session_udid.data(),mode==1?"invalid":"00008101-001234567890ABCD");
        if(mode==0||mode==5){
            auto& bytes=tls.ssl.input;
            auto length=[&](std::uint32_t n){std::uint8_t h[4];bb::write_be32(h,n);bytes.insert(bytes.end(),h,h+4);};
            auto name=[&](const char* s){length(std::strlen(s));bytes.insert(bytes.end(),s,s+std::strlen(s));};
            const char* xml="<plist><array><string>DLMessageUploadFiles</string><integer>0</integer><integer>0</integer><integer>5</integer></array></plist>";
            name(xml);name("/device/source");name("backup/glue.bin");
            length(6);bytes.push_back(0x0c);for(char c:std::string_view("hello"))bytes.push_back(c);
            if(mode==0){length(1);bytes.push_back(0x0b);length(0);add_frame(bytes,"result_ok.xml");}
        }
        assert(t.requestMdmBackup()&&!t.requestMdmBackup());
        if(mode==4)tls.ssl.close=true;
        for(unsigned i=0;i<5000&&t.mdmBackupState()==HCCUSBTransport::MdmBackupState::running;++i){
            ++now_ms;const auto ok=t.pollMdmProbe();
            if(mode==2&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::waiting)now_ms+=60000;
            if(mode==6&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::waiting){
                t._notification_rx_stream_length=t._notification_rx_stream.size();
                const std::uint8_t byte=0;assert(!t.appendMdmPlaintext(&byte,1));break;
            }
            if(mode==5&&t._mdm_probe->operation&&t._mdm_probe->operation->phase()==HCCBackupPhase::receiving){
                bool staged=false;for(const auto& e:fs::recursive_directory_iterator(sd_root))if(e.path().extension()==".part")staged=true;
                if(staged){t.releaseMdmProbe();break;}
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
    std::puts("PASS: backup completion, preflight/transport/timeout failure and teardown release SD; no terminal stream restart");
    fs::remove_all(sd_root);
    assert(allocations == 0);
    std::puts("8 firmware-glue groups passed using host SDK/I/O stubs; not a firmware build.");
}
