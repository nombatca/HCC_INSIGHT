#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <string_view>
#include <vector>

// Use the production transport declaration; only SDK types and I/O are stubbed.
#define private public
#include "HCCUSBTransport.hpp"
#undef private

namespace bb = hcc::backup;
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
    assert(allocations == 0);
    std::puts("5 firmware-glue groups passed using host SDK/I/O stubs; not a firmware build.");
}
