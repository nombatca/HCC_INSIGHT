// Shared with the host burst-receive regression; uses production ring code.
bool HCCUSBTransport::appendServiceTlsRx(const uint8_t *data, size_t length)
{
    ServiceTlsContext *tls = _service_tls;
    if (tls != nullptr && tls->established && !tls->closed && _mdm_service_active &&
        !pumpMdmServiceRx()) return false;
    if (tls == nullptr || data == nullptr || length > ServiceTlsContext::kRxCapacity - tls->rx_length) {
        printf("[HCC SERVICE TLS] RX ciphertext overflow len=%u buffered=%u\n",
               static_cast<unsigned>(length), static_cast<unsigned>(tls != nullptr ? tls->rx_length : 0));
        return false;
    }
    const size_t tail = (tls->rx_head + tls->rx_length) % ServiceTlsContext::kRxCapacity;
    const size_t first = length < (ServiceTlsContext::kRxCapacity - tail)
                             ? length : (ServiceTlsContext::kRxCapacity - tail);
    memcpy(tls->rx_fifo + tail, data, first);
    if (length > first) memcpy(tls->rx_fifo, data + first, length - first);
    tls->rx_length += length;
    tls->handshake_step_requested = true;
    // Decrypt right away once established so the ciphertext ring never fills while the task
    // is still inside handle_events; any ciphertext mbedTLS stages is sent by processServiceTls.
    if (tls->established && !tls->closed && _mdm_service_active) return pumpMdmServiceRx();
    return drainServiceTls();
}
