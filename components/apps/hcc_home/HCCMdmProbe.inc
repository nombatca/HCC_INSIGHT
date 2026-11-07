// Included once after ServiceTlsContext. USB-task-owned protocol glue.
struct HCCUSBTransport::MdmProbeContext {
    uint8_t control[8192]{};
    uint8_t output[1024]{};
    hcc::plist::Node nodes[128]{};
    char text[8192]{};
    hcc::backup::BackupLink link;
    MdmProbeContext() : link({control, sizeof(control), output, sizeof(output),
                             {nodes, 128, text, sizeof(text)}}) {}
};

bool HCCUSBTransport::requestMdmProbe()
{
    MdmState expected = _mdm_state.load();
    while (expected == MdmState::Idle || expected == MdmState::Failed) {
        if (_mdm_state.compare_exchange_weak(expected, MdmState::Armed)) return true;
    }
    return false;
}

bool HCCUSBTransport::mdmPollingPending() const
{
    const auto state = _mdm_state.load();
    return state == MdmState::Opening || state == MdmState::Negotiating;
}

const char *HCCUSBTransport::mdmStatusText() const
{
    switch (_mdm_state.load()) {
    case MdmState::Idle: return "MDM: chưa kiểm tra";
    case MdmState::Armed: return "MDM: rút/cắm lại thiết bị để kiểm tra";
    case MdmState::Opening: return "MDM: đang kết nối dịch vụ";
    case MdmState::Negotiating: return "MDM: đang thương lượng phiên bản";
    case MdmState::Ready: return "MDM: dịch vụ sẵn sàng - chưa gỡ MDM";
    case MdmState::Failed: return "MDM: kiểm tra thất bại - xem log USB";
    }
    return "MDM: chưa xác định";
}

bool HCCUSBTransport::beginMdmProbe()
{
    if (_mdm_state.load() != MdmState::Opening) return true;
    if (_mdm_probe != nullptr) return false;
    void *storage = heap_caps_malloc(sizeof(MdmProbeContext), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == nullptr) return false;
    _mdm_probe = new (storage) MdmProbeContext();
    if (_mdm_probe->link.start(mdmNowMs()) != hcc::backup::LinkError::ok) return false;
    _mdm_state.store(MdmState::Negotiating);
    printf("[HCC MDM] mobilebackup2 negotiation started; context=%u bytes SSL=%u\n",
           static_cast<unsigned>(sizeof(MdmProbeContext)), static_cast<unsigned>(_notification_service_ssl));
    return true;
}

void HCCUSBTransport::releaseMdmProbe()
{
    if (_mdm_probe == nullptr) return;
    _mdm_probe->~MdmProbeContext();
    mbedtls_platform_zeroize(_mdm_probe, sizeof(MdmProbeContext));
    heap_caps_free(_mdm_probe);
    _mdm_probe = nullptr;
}

bool HCCUSBTransport::appendMdmPlaintext(const uint8_t *data, size_t length)
{
    if (length > _notification_rx_stream.size() - _notification_rx_stream_length) {
        _mdm_state.store(MdmState::Failed);
        return false;
    }
    if (length != 0) memcpy(_notification_rx_stream.data() + _notification_rx_stream_length, data, length);
    _notification_rx_stream_length += length;
    return true;
}

static hcc::backup::IoResult mdmTlsResult(int result, size_t capacity)
{
    using hcc::backup::IoState;
    if (result > 0 && static_cast<size_t>(result) <= capacity) return {static_cast<size_t>(result), IoState::ok};
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return {0, IoState::would_block};
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return {0, IoState::would_block};
#endif
    if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return {0, IoState::closed};
    printf("[HCC MDM] service TLS I/O failed: %d\n", result);
    return {0, IoState::error};
}

hcc::backup::IoResult HCCUSBTransport::mdmRead(void *context, uint8_t *buffer, size_t length)
{
    auto *transport = static_cast<HCCUSBTransport *>(context);
    using hcc::backup::IoState;
    if (transport->_service_tcp.state != TcpState::Connected) return {0, IoState::closed};
    if (transport->_notification_service_ssl) {
        if (transport->_service_tls == nullptr || !transport->_service_tls->established) return {0, IoState::error};
        return mdmTlsResult(mbedtls_ssl_read(&transport->_service_tls->ssl, buffer, length), length);
    }
    const size_t used = transport->_notification_rx_stream_length;
    const size_t take = length < used ? length : used;
    if (take == 0) return {0, IoState::would_block};
    memcpy(buffer, transport->_notification_rx_stream.data(), take);
    memmove(transport->_notification_rx_stream.data(), transport->_notification_rx_stream.data() + take, used - take);
    transport->_notification_rx_stream_length -= take;
    return {take, IoState::ok};
}

hcc::backup::IoResult HCCUSBTransport::mdmWrite(void *context, const uint8_t *buffer, size_t length)
{
    auto *transport = static_cast<HCCUSBTransport *>(context);
    using hcc::backup::IoState;
    if (transport->_service_tcp.state != TcpState::Connected) return {0, IoState::closed};
    if (transport->_tx_in_flight) return {0, IoState::would_block};
    if (transport->_notification_service_ssl) {
        if (transport->_service_tls == nullptr || !transport->_service_tls->established) return {0, IoState::error};
        return mdmTlsResult(mbedtls_ssl_write(&transport->_service_tls->ssl, buffer, length), length);
    }
    const size_t take = length < kTlsTcpChunkSize ? length : kTlsTcpChunkSize;
    // sendServiceTcpPacket copies into the USB TX buffer before returning.
    if (!transport->sendServiceTcpPacket(TxKind::BackupPlaintext, 0x10, buffer, take)) return {0, IoState::error};
    return {take, IoState::ok};
}

bool HCCUSBTransport::pollMdmProbe()
{
    if (_mdm_state.load() != MdmState::Negotiating) return true;
    if (_mdm_probe == nullptr) return false;
    const auto result = _mdm_probe->link.poll({this, mdmRead, mdmWrite}, mdmNowMs(), 8);
    if (_mdm_probe->link.phase() == hcc::backup::LinkPhase::ready) {
        _mdm_state.store(MdmState::Ready);
        printf("[HCC MDM] NEGOTIATION READY protocol=%.1f; no backup/restore request sent\n",
               _mdm_probe->link.protocol_version());
        return true;
    }
    if (result == hcc::backup::LinkError::would_block || result == hcc::backup::LinkError::yield) return true;
    printf("[HCC MDM] negotiation failed: %s\n", hcc::backup::link_error_name(result));
    _mdm_state.store(MdmState::Failed);
    return false;
}
