// Included once after ServiceTlsContext. USB-task-owned protocol glue.
struct HCCUSBTransport::MdmProbeContext {
    static constexpr std::size_t kListEntries = 64;
    static constexpr std::size_t kControlNodes=2048;
    uint8_t control[65536]{};
    uint8_t output[1024]{};
    hcc::plist::Node nodes[kControlNodes]{};
    char text[65536]{};
    hcc::backup::BackupLink link;
    // Backup operation driver and its bounded, PSRAM-resident working storage.
    // Idle (no SD lease, no transport use) until a backup is actually requested.
    uint8_t op_scratch[hcc::backup::kObservedChunkSize]{}; // upload receiver scratch (32 KiB)
    uint8_t op_output[8192]{};                             // operation request/reply frames
    hcc::backup::DirectoryEntry list_entries[kListEntries]{};
    uint32_t list_offsets[hcc::backup::contents_object_count(kListEntries)]{};
    hcc::backup::ListingScratch list_scratch{};
    std::optional<HCCBackupOperation> operation;
    std::uint64_t last_diagnostic_ms=0, read_progress=0;
    MdmProbeContext()
        : link({control, sizeof(control), output, sizeof(output), {nodes, kControlNodes, text, sizeof(text)}}) {}
};

bool HCCUSBTransport::requestMdmBackup()
{
    // One operation per negotiated stream. A terminal stream must be reconnected.
    if (_mdm_state.load() != MdmState::Ready) return false;
    auto expected = MdmBackupState::none;
    if (!_mdm_backup_state.compare_exchange_strong(expected, MdmBackupState::running)) return false;
    _mdm_backup_files.store(0);
    _mdm_backup_requested.store(true);
    if (_mdm_state.load() != MdmState::Ready) {
        _mdm_backup_requested.store(false);
        _mdm_backup_state.store(MdmBackupState::failed);
        return false;
    }
    return true;
}

void HCCUSBTransport::finishMdmBackup(bool success)
{
    if (_mdm_probe && _mdm_probe->operation) {
        _mdm_backup_files.store(_mdm_probe->operation->committedFiles());
        _mdm_probe->operation.reset(); // abort pending file, then release SD ownership
    }
    _mdm_backup_state.store(success ? MdmBackupState::done : MdmBackupState::failed);
    _mdm_backup_requested.store(false);
    if (!success) _mdm_state.store(MdmState::Failed); // never reuse failed TLS/DeviceLink I/O
}

void HCCUSBTransport::pollMdmBackupWatchdog()
{
    if (!_mdm_backup_requested.load() || !_mdm_probe || !_mdm_probe->operation) return;
    auto &op=*_mdm_probe->operation;
    const auto now=mdmNowMs();
    const auto result=op.checkDeadline(now);
    const bool transport_failed=_state==State::Failed || _mdm_state.load()!=MdmState::Ready ||
                                _service_tcp.state!=TcpState::Connected;
    const bool stopped=result!=HCCBackupError::would_block;
    if (stopped || transport_failed || now-_mdm_probe->last_diagnostic_ms>=5000) {
        _mdm_probe->last_diagnostic_ms=now;
        const auto *tls=_service_tls;
        printf("[HCC BACKUP] wait phase=%d elapsed_ms=%llu idle_ms=%llu usb_tx=%u tcp=%d tls_rx=%u tls_tx=%u plain_rx=%u partial_control=%u\n",
               static_cast<int>(op.phase()),static_cast<unsigned long long>(op.elapsedMilliseconds(now)),
               static_cast<unsigned long long>(op.idleMilliseconds(now)),static_cast<unsigned>(_tx_in_flight),
               static_cast<int>(_service_tcp.state),static_cast<unsigned>(tls?tls->rx_length:0),
               static_cast<unsigned>(tls&&tls->tx_length>=tls->tx_offset?tls->tx_length-tls->tx_offset:0),
               static_cast<unsigned>(_notification_rx_stream_length),static_cast<unsigned>(_mdm_probe->link.control_frame_pending()));
    }
    if (stopped || transport_failed) {
        printf("[HCC MDM] BACKUP watchdog stopped err=%d transport_failed=%u files=%u\n",
               static_cast<int>(transport_failed?HCCBackupError::transfer:result),static_cast<unsigned>(transport_failed),static_cast<unsigned>(op.committedFiles()));
        finishMdmBackup(!transport_failed && result==HCCBackupError::ok && op.phase()==HCCBackupPhase::succeeded);
    }
}

const char *HCCUSBTransport::mdmBackupStatusText()
{
    const unsigned files = _mdm_backup_files.load();
    switch (_mdm_backup_state.load()) {
    case MdmBackupState::none:    return "Backup: chưa chạy";
    case MdmBackupState::running: snprintf(_mdm_backup_text, sizeof(_mdm_backup_text), "Backup: đang nhận dữ liệu... (%u tệp)", files); break;
    case MdmBackupState::done:    snprintf(_mdm_backup_text, sizeof(_mdm_backup_text), "Capture xong: %u tệp -> SD", files); break;
    case MdmBackupState::failed:  snprintf(_mdm_backup_text, sizeof(_mdm_backup_text), "Backup dừng/lỗi: %u tệp đã lưu", files); break;
    }
    return _mdm_backup_text;
}

bool HCCUSBTransport::requestMdmProbe()
{
    MdmState expected = _mdm_state.load();
    while (expected == MdmState::Idle || expected == MdmState::Failed || expected == MdmState::Disconnected) {
        if (_mdm_state.compare_exchange_weak(expected, MdmState::Armed)) return true;
    }
    return false;
}

bool HCCUSBTransport::mdmPollingPending() const
{
    const auto state = _mdm_state.load();
    return state == MdmState::Opening || state == MdmState::Negotiating ||
           (state == MdmState::Ready && _mdm_backup_state.load() != MdmBackupState::done);
}

const char *HCCUSBTransport::mdmStatusText() const
{
    switch (_mdm_state.load()) {
    case MdmState::Idle: return "MDM: chưa kiểm tra";
    case MdmState::Armed: return "MDM: rút/cắm lại thiết bị để kiểm tra";
    case MdmState::Opening: return "MDM: đang kết nối dịch vụ";
    case MdmState::Negotiating: return "MDM: đang thương lượng phiên bản";
    case MdmState::Ready: return "MDM: dịch vụ sẵn sàng - chưa gỡ MDM";
    case MdmState::Disconnected: return "MDM: phiên dịch vụ đã đóng";
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
    _mdm_backup_requested.store(false);
    _mdm_backup_files.store(0);
    _mdm_backup_state.store(MdmBackupState::none);
    if (_mdm_probe->link.start(mdmNowMs()) != hcc::backup::LinkError::ok) return false;
    _mdm_state.store(MdmState::Negotiating);
    printf("[HCC MDM] mobilebackup2 negotiation started; context=%u bytes SSL=%u\n",
           static_cast<unsigned>(sizeof(MdmProbeContext)), static_cast<unsigned>(_notification_service_ssl));
    return true;
}

void HCCUSBTransport::releaseMdmProbe()
{
    if (_mdm_backup_requested.load()) finishMdmBackup(false);
    if (_mdm_probe == nullptr) return;
    _mdm_probe->~MdmProbeContext();
    mbedtls_platform_zeroize(_mdm_probe, sizeof(MdmProbeContext));
    heap_caps_free(_mdm_probe);
    _mdm_probe = nullptr;
}

bool HCCUSBTransport::appendMdmPlaintext(const uint8_t *data, size_t length)
{
    if (length > _notification_rx_stream.size() - _notification_rx_stream_length) {
        if (_mdm_backup_requested.load()) finishMdmBackup(false);
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
        const auto result=mdmTlsResult(mbedtls_ssl_read(&transport->_service_tls->ssl, buffer, length), length);
        if (transport->_mdm_probe) transport->_mdm_probe->read_progress+=result.bytes;
        return result;
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
    if (_mdm_state.load() == MdmState::Ready) {
        if (_mdm_probe == nullptr) return false;
        if (_mdm_backup_requested.load()) {
            // Owner-consented device->SD data pull. Prototype: bounded partial
            // backup (coordinator caps files/bytes/time). No restore, no MDM change.
            if (!_mdm_probe->operation) {
                auto &ctx = *_mdm_probe;
                ctx.operation.emplace(ctx.link, ctx.op_scratch, sizeof(ctx.op_scratch),
                                      ctx.op_output, sizeof(ctx.op_output),
                                      HCCBackupContentsBuffers{ctx.list_entries, ctx.kListEntries,
                                      ctx.list_offsets, hcc::backup::contents_object_count(ctx.kListEntries), &ctx.list_scratch});
            }
            auto &op = *_mdm_probe->operation;
            if (op.phase() == HCCBackupPhase::idle) {
                const char *udid = _session_udid.data();
                const size_t ulen = strnlen(udid, _session_udid.size());
                HCCBackupConfig config; // 1 MiB min free, <=1024 files; bounded P4 capture
                // Real P4 transfer was still progressing at the old 10-minute cap.
                config.total_timeout_ms=60ULL*60*1000; // 1 hour; idle remains 60 seconds
                const auto started = op.start(std::string_view(udid, ulen), mdmNowMs(), config);
                if (started != HCCBackupError::ok) {
                    printf("[HCC MDM] BACKUP start failed: %d\n", static_cast<int>(started));
                    finishMdmBackup(false);
                    return false;
                }
                _mdm_probe->last_diagnostic_ms=mdmNowMs();
                _mdm_backup_state.store(MdmBackupState::running);
                printf("[HCC MDM] BACKUP started (prototype partial pull to SD)\n");
            }
            if (op.phase() != HCCBackupPhase::idle) {
                op.poll({this, mdmRead, mdmWrite}, mdmNowMs(), 8);
                _mdm_backup_files.store(op.committedFiles());
                const auto ph = op.phase();
                if (ph == HCCBackupPhase::succeeded) {
                    printf("[HCC MDM] BACKUP complete files=%u path=%s\n",
                           static_cast<unsigned>(op.committedFiles()), op.sessionPath());
                    finishMdmBackup(true);
                    return true;
                } else if (ph == HCCBackupPhase::failed || ph == HCCBackupPhase::cancelled) {
                    printf("[HCC MDM] BACKUP stopped err=%d files=%u\n",
                           static_cast<int>(op.error()), static_cast<unsigned>(op.committedFiles()));
                    finishMdmBackup(false);
                    return false;
                }
            }
            return true;
        }
        // Completed captures park the negotiated stream; do not read a final-result
        // observer again or silently restart the same operation.
        if (_mdm_backup_state.load() == MdmBackupState::done) return true;
        hcc::plist::Document doc;
        hcc::backup::SessionEvent event;
        bool available = false;
        const auto result = _mdm_probe->link.receive_event({this, mdmRead, mdmWrite}, mdmNowMs(), doc, event, available, 8);
        if (result == hcc::backup::LinkError::closed) {
            _mdm_state.store(MdmState::Disconnected);
            printf("[HCC MDM] service closed; no operation result inferred\n");
            return true;
        }
        if (result == hcc::backup::LinkError::would_block || result == hcc::backup::LinkError::yield) return true;
        if (result != hcc::backup::LinkError::ok) {
            printf("[HCC MDM] control receive failed: %s\n", hcc::backup::link_error_name(result));
            _mdm_state.store(MdmState::Failed);
            return false;
        }
        if (available) {
            // Only log the type, never file contents or unsolicited identifiers.
            printf("[HCC MDM] control message=%.*s; no operation request sent\n",
                   static_cast<int>(event.message.size()), event.message.data());
            if (event.kind == hcc::backup::EventKind::disconnect) _mdm_state.store(MdmState::Disconnected);
        }
        return true;
    }
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

// Called on the same USB owner task, including from RX callbacks. No USB submit:
// mdmWrite stages TLS output only, and preserves WANT retry ownership. Bound the
// work so a peer cannot keep the event callback running indefinitely.
bool HCCUSBTransport::pumpMdmServiceRx()
{
    for (unsigned step=0;step<32 && _mdm_probe;++step) {
        const auto before=_mdm_probe->read_progress;
        const auto phase=_mdm_probe->operation ? _mdm_probe->operation->phase() : HCCBackupPhase::idle;
        const auto state=_mdm_state.load();
        if (state!=MdmState::Ready && state!=MdmState::Negotiating) return state!=MdmState::Failed;
        if (!pollMdmProbe()) return false;
        if (!_mdm_probe) return true;
        const auto after_phase=_mdm_probe->operation ? _mdm_probe->operation->phase() : HCCBackupPhase::idle;
        if (before==_mdm_probe->read_progress && phase==after_phase && state==_mdm_state.load()) break;
    }
    return true;
}
