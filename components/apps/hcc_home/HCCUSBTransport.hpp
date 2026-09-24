#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "mbedtls/x509_crt.h"
#include "usb/usb_host.h"
#include "HCCPairCredentials.hpp"

class HCCUSBTransport {
public:
    enum class State {
        Idle,
        WaitingVersion,
        Version1,
        Version2,
        SetupSent,
        MuxActive,
        Failed,
    };

    enum class TcpState {
        Inactive,
        SynSent,
        Connected,
        Failed,
    };

    enum class TlsState {
        PlainLockdown,
        TlsPrepare,
        TlsHandshaking,
        TlsEstablished,
        Failed,
    };

    HCCUSBTransport() = default;

    bool start(usb_device_handle_t device, uint8_t out_endpoint, uint8_t in_endpoint, uint16_t in_mps);
    bool submitPendingSetup();
    bool submitPendingTcp();
    bool needsDwcDelayedSnapshot() const;
    bool hasDwcDelayedSnapshotPending() const;
    bool isDwcDelayedSnapshotDue() const;
    TickType_t dwcDelayedSnapshotDue() const;
    bool txInFlight() const;
    uint8_t txKindForDiagnostic() const;
    void takeDwcDelayedSnapshotIfDue();
    void stopForDeviceGone();
    void stopForShutdown();
    bool hasPendingTransfers() const;
    State state() const;
    using DevicePublicKeyConsumer = bool (*)(void *context, const uint8_t *data, size_t length);
    bool handoffDevicePublicKey(DevicePublicKeyConsumer consumer, void *context);
    bool preparePairTimeWait();
    // Initializes the dedicated hcc_pair NVS partition ahead of USB sessions; lazy init remains as fallback.
    static bool initPairRecordStorage();
    bool persistPendingPairRecord();
    bool loadPairRecordForReuse(const char *udid, uint32_t generation);
    bool beginPair(HCCPairCredentialBundle *bundle, uint32_t generation,
                   const char *host_id, const char *system_buid, const char *udid);

    // Read-only device info captured during the plaintext lockdown queries (fresh-pair sessions).
    // Empty string when a value has not been captured this connection.
    const char *productVersion() const { return _product_version.data(); }
    const char *deviceClass() const { return _device_class.data(); }
    const char *wifiAddress() const { return _wifi_address_valid ? _wifi_address.data() : ""; }
    // True once the encrypted lockdown session is up (device is trusted and ready).
    bool sessionEstablished() const { return _tls_state == TlsState::TlsEstablished; }

private:
    static constexpr size_t kRxMinimumBufferSize = 8192;
    static constexpr size_t kRxTransferCount = 3;
    static constexpr size_t kVersionPacketSize = 20;
    static constexpr size_t kV2HeaderSize = 16;
    static constexpr size_t kSetupPacketSize = kV2HeaderSize + 1;
    static constexpr size_t kTcpHeaderSize = 20;
    static constexpr size_t kTcpMuxPacketSize = kV2HeaderSize + kTcpHeaderSize;
    static constexpr size_t kRxStreamCapacity = 65536;
    static constexpr size_t kLockdownRxStreamCapacity = 4096;
    static constexpr size_t kDevicePublicKeyCapacity = 2048;
    static constexpr size_t kPairXmlMaximum = 8192;
    static constexpr size_t kPairTxChunkSize = 512;
    static constexpr size_t kPairResponseMaximum = 16384;
    static constexpr size_t kEscrowBagMaximum = 4096;
    static constexpr size_t kStartSessionXmlCapacity = 1024;
    static constexpr size_t kSessionIdCapacity = 128;
    static constexpr size_t kTlsRxCapacity = 32768;
    static constexpr size_t kTlsTxCapacity = 8192;
    static constexpr size_t kTlsTcpChunkSize = 4096;
    static constexpr size_t kTlsLockdownCapacity = 4096;
    static constexpr size_t kNotificationRxCapacity = 4096;
    static constexpr uint16_t kLockdownPort = 62078;

    enum class TxKind {
        None,
        Version,
        Setup,
        TcpSyn,
        TcpAck,
        LockdownPrefix,
        LockdownBody,
        TcpDataAck,
        GetValueProductVersionPrefix,
        GetValueProductVersionBody,
        GetValueDeviceClassPrefix,
        GetValueDeviceClassBody,
        GetValueDevicePublicKeyPrefix,
        GetValueDevicePublicKeyBody,
        GetValueWiFiAddressPrefix,
        GetValueWiFiAddressBody,
        PairPrefix,
        PairChunk,
        StartNotificationServicePrefix,
        StartNotificationServiceBody,
        StartSessionPrefix,
        StartSessionBody,
        TlsCiphertext,
        ServiceTcpSyn,
        ServiceTcpAck,
        PrimaryTrustFin,
        PrimaryTrustCloseAck,
        NotificationPrefix,
        NotificationBody,
    };

    enum class LockdownResponse {
        None,
        QueryType,
        GetValueProductVersion,
        GetValueDeviceClass,
        GetValueDevicePublicKey,
        GetValueWiFiAddress,
        Pair,
        StartNotificationService,
        StartSession,
    };

    enum class PairTrustState {
        Inactive,
        WaitingStartNotificationService,
        StartNotificationServicePending,
        NotificationServiceReady,
        Failed,
    };

    enum class PrimaryConnectionPurpose {
        InitialDiscovery,
        PairReconnect,
        SessionReconnect,
    };

    static void onRxComplete(usb_transfer_t *transfer);
    static void onTxComplete(usb_transfer_t *transfer);

    bool submitRx(size_t slot);
    bool sendSetup();
    bool sendTcpSyn();
    bool sendTcpAck();
    bool sendServiceTcpSyn();
    bool sendServiceTcpAck();
    bool sendPrimaryTrustFin();
    bool sendPrimaryTrustCloseAck();
    bool sendNotificationPrefix();
    bool sendNotificationBody();
    bool sendLockdownQueryTypePrefix();
    bool sendLockdownQueryTypeBody();
    bool sendTcpDataAck();
    bool sendGetValueProductVersionPrefix();
    bool sendGetValueProductVersionBody();
    bool sendGetValueDeviceClassPrefix();
    bool sendGetValueDeviceClassBody();
    bool sendGetValueDevicePublicKeyPrefix();
    bool sendGetValueDevicePublicKeyBody();
    bool sendGetValueWiFiAddressPrefix();
    bool sendGetValueWiFiAddressBody();
    bool sendPairPrefix();
    bool sendPairChunk();
    bool sendStartNotificationServicePrefix();
    bool sendStartNotificationServiceBody();
    bool sendStartSessionPrefix();
    bool sendStartSessionBody();
    bool processTlsDeferred();
    bool processTlsApplication();
    bool prepareTls();
    bool loadTlsCredentials();
    bool sendTlsCiphertext();
    bool appendTlsRx(const uint8_t *data, size_t length);
    void cleanupTls();
    static int tlsBioSend(void *context, const unsigned char *buffer, size_t length);
    static int tlsBioRecv(void *context, unsigned char *buffer, size_t length);
    static int tlsVerifyPeer(void *context, mbedtls_x509_crt *certificate, int depth, uint32_t *flags);
    bool buildStartSessionXml();
    bool buildPairXml(const char *host_id, const char *system_buid);
    void destroyPairState();
    bool handlePairResponse(const uint8_t *body, size_t length);
    bool handleStartNotificationServiceResponse(const uint8_t *body, size_t length);
    bool handleStartSessionResponse(const uint8_t *body, size_t length);
    bool sendTcpPacket(TxKind tx_kind, uint8_t flags, const uint8_t *payload = nullptr, size_t payload_length = 0);
    bool sendServiceTcpPacket(TxKind tx_kind, uint8_t flags,
                              const uint8_t *payload = nullptr, size_t payload_length = 0);
    bool parseVersionResponse(const uint8_t *data, size_t length);
    bool appendRxStream(const uint8_t *data, size_t length);
    bool processRxStream();
    bool dispatchV2Frame(const uint8_t *data, size_t length);
    bool handleInboundTcp(const uint8_t *data, size_t length);
    bool handleInboundServiceTcp(uint32_t seq, uint32_t ack, uint8_t flags,
                                 uint16_t window, const uint8_t *payload,
                                 size_t payload_length);
    bool appendNotificationRx(const uint8_t *data, size_t length);
    bool processNotificationRx();
    bool appendLockdownRxStream(const uint8_t *data, size_t length);
    bool processLockdownRxStream();
    bool handleLockdownResponse(const uint8_t *body, size_t length);
    void resetPrimaryTcpForPairReconnect();
    void clearStartSessionConnectionState(bool keep_pair_record);
    bool pairReadyForCurrentConnection() const;
    void authorizePairForCurrentConnection();
    uint16_t allocateSourcePort();
    uint16_t allocateServiceSourcePort();
    void resetServiceTcpContext();
    static const char *txKindName(TxKind tx_kind);
    void beginDwcSetupDiagnostic();
    void takeDwcSnapshot(const char *snapshot_name);
    bool findDwcOutChannel();
    void retireTransfer(usb_transfer_t *transfer, bool is_rx);
    int findRxSlot(const usb_transfer_t *transfer) const;
    bool hasRxTransfersInFlight() const;

    usb_device_handle_t _device = nullptr;
    uint8_t _out_endpoint = 0;
    uint8_t _in_endpoint = 0;
    size_t _rx_buffer_size = 0;
    std::array<usb_transfer_t *, kRxTransferCount> _rx_transfers{};
    usb_transfer_t *_tx_transfer = nullptr;
    std::array<bool, kRxTransferCount> _rx_in_flight{};
    bool _tx_in_flight = false;
    bool _stopping = false;
    bool _v2_framing = false;
    bool _setup_requested = false;
    bool _tcp_syn_requested = false;
    bool _tcp_ack_requested = false;
    bool _lockdown_prefix_requested = false;
    bool _lockdown_body_requested = false;
    bool _tcp_data_ack_requested = false;
    bool _getvalue_product_version_after_ack = false;
    bool _getvalue_product_version_prefix_requested = false;
    bool _getvalue_product_version_body_requested = false;
    bool _getvalue_device_class_after_ack = false;
    bool _getvalue_device_class_prefix_requested = false;
    bool _getvalue_device_class_body_requested = false;
    bool _getvalue_device_public_key_after_ack = false;
    bool _getvalue_device_public_key_prefix_requested = false;
    bool _getvalue_device_public_key_body_requested = false;
    bool _getvalue_wifi_address_after_ack = false;
    bool _getvalue_wifi_address_prefix_requested = false;
    bool _getvalue_wifi_address_body_requested = false;
    bool _pair_prefix_requested = false;
    bool _pair_chunk_requested = false;
    bool _pair_after_querytype_ack = false;
    bool _pair_destroy_after_response = false;
    bool _start_notification_service_after_ack = false;
    bool _start_notification_service_prefix_requested = false;
    bool _start_notification_service_body_requested = false;
    bool _start_session_after_ack = false;
    bool _start_session_after_querytype_ack = false;
    bool _start_session_prefix_requested = false;
    bool _start_session_body_requested = false;
    bool _session_reconnect_pending = false;
    bool _session_reconnect_attempted = false;
    // InvalidHostID recovery: the reused record was rejected; reconnect once for fresh pairing.
    bool _pair_record_rejected = false;
    bool _repair_reconnect_pending = false;
    bool _repair_attempted = false;
    uint16_t _tx_seq = 0;
    uint16_t _rx_seq = 0;
    uint16_t _next_source_port = 1;
    uint16_t _tcp_source_port = 0;
    uint16_t _tcp_destination_port = kLockdownPort;
    uint32_t _tcp_tx_seq = 0;
    uint32_t _tcp_ack_seq = 0;
    uint32_t _tcp_peer_seq = 0;
    uint32_t _tcp_peer_ack = 0;
    uint32_t _tcp_peer_window = 0;
    uint32_t _primary_connection_epoch = 0;
    uint32_t _query_type_epoch = 0;
    uint32_t _pair_authorized_epoch = 0;
    bool _primary_tcp_closed = false;
    bool _primary_trust_fin_requested = false;
    bool _primary_trust_fin_dispatched = false;
    bool _primary_trust_close_ack_requested = false;
    bool _primary_reconnect_pending = false;
    bool _primary_reconnect_attempted = false;
    bool _trust_pair_retry_requested = false;
    std::array<uint8_t, kRxStreamCapacity> _rx_stream{};
    size_t _rx_stream_length = 0;
    std::array<uint8_t, kLockdownRxStreamCapacity> _lockdown_rx_stream{};
    size_t _lockdown_rx_stream_length = 0;
    uint8_t *_pair_response_stream = nullptr;
    size_t _pair_response_stream_length = 0;
    size_t _pair_response_stream_capacity = 0;
    HCCPairCredentialBundle *_pair_bundle = nullptr;
    uint8_t *_pair_xml = nullptr;
    size_t _pair_xml_length = 0;
    size_t _pair_xml_offset = 0;
    uint32_t _pair_generation = 0;
    uint32_t _notification_service_generation = 0;
    uint32_t _notification_service_epoch = 0;
    uint16_t _notification_service_port = 0;
    bool _notification_service_ssl = false;
    bool _notification_tcp_after_ack = false;
    struct ServiceTcpContext {
        TcpState state = TcpState::Inactive;
        uint16_t source_port = 0;
        uint16_t destination_port = 0;
        uint32_t tx_seq = 0;
        uint32_t ack_seq = 0;
        uint32_t peer_seq = 0;
        uint32_t peer_ack = 0;
        uint32_t peer_window = 0;
        uint32_t generation = 0;
        uint32_t epoch = 0;
        bool syn_requested = false;
        bool ack_requested = false;
        bool notification_prefix_requested = false;
        bool notification_body_requested = false;
        uint8_t notification_index = 0;
    } _service_tcp;
    std::array<uint8_t, kNotificationRxCapacity> _notification_rx_stream{};
    size_t _notification_rx_stream_length = 0;
    std::array<char, 128> _pair_udid{};
    std::array<char, 37> _pair_host_id{};
    std::array<char, 37> _pair_system_buid{};
    uint8_t *_pair_escrow_bag = nullptr;
    size_t _pair_escrow_bag_length = 0;
    bool _pair_record_persist_pending = false;
    uint32_t _pair_success_epoch = 0;
    std::array<char, 37> _session_host_id{};
    std::array<char, 37> _session_system_buid{};
    std::array<uint8_t, kStartSessionXmlCapacity> _start_session_xml{};
    size_t _start_session_xml_length = 0;
    std::array<char, kSessionIdCapacity> _session_id{};
    uint32_t _session_generation = 0;
    uint32_t _session_epoch = 0;
    bool _session_record_loaded = false;
    bool _session_ssl_required = false;
    bool _session_ready = false;
    bool _tls_after_ack = false;
    bool _tls_handshake_step_requested = false;
    uint32_t _tls_generation = 0;
    uint32_t _tls_epoch = 0;
    std::array<char, 128> _session_udid{};
    struct TlsContext;
    TlsContext *_tls = nullptr;
    TlsState _tls_state = TlsState::PlainLockdown;
    std::array<char, 64> _wifi_address{};
    bool _wifi_address_valid = false;
    // Captured lockdown GetValue results for on-screen device info.
    std::array<char, 32> _product_version{};
    std::array<char, 32> _device_class{};
    std::array<uint8_t, kDevicePublicKeyCapacity> _device_public_key{};
    size_t _device_public_key_length = 0;
    bool _device_public_key_pending = false;
    bool _device_public_key_wait_logged = false;
    bool _device_public_key_wakeup_logged = false;
    bool _device_public_key_utc_ready_logged = false;
    int _dwc_out_channel = -1;
    uint8_t _dwc_device_address = 0;
    TickType_t _dwc_delayed_snapshot_due = 0;
    bool _dwc_delayed_snapshot_pending = false;
    TxKind _tx_kind = TxKind::None;
    LockdownResponse _lockdown_expected_response = LockdownResponse::None;
    PairTrustState _pair_trust_state = PairTrustState::Inactive;
    PrimaryConnectionPurpose _primary_connection_purpose = PrimaryConnectionPurpose::InitialDiscovery;
    State _state = State::Idle;
    TcpState _tcp_state = TcpState::Inactive;
};
