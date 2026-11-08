# mobilebackup2 status replies

Hardware checkpoint reported on 2026-11-08: the P4 negotiated DeviceLink and
mobilebackup2 protocol 2.1 over paired service TLS. The log explicitly reports
`no backup/restore request sent`. This verifies negotiation, not an operation.

`StatusReplyWriter` adds an allocation-free, caller-buffered status sender.
It emits one big-endian length-prefixed binary plist containing four elements:
message name, signed status code, ASCII message or empty-parameter sentinel,
and an empty dictionary or sentinel. Negative codes use full-width integers.
This bounded subset does not encode multistatus dictionaries or free-space values.

Protocol reference: libimobiledevice `mobilebackup2_send_status_response`:
https://github.com/libimobiledevice/libimobiledevice/blob/master/src/mobilebackup2.c
Implementation is independent and uses the existing HCC binary-plist writer.

Each pump makes at most one write callback. Partial progress is retained;
would-block retries preserve buffer address and remaining length. Storage must
remain exclusively owned and unchanged until completion/error. Closed/error
results remain failures even if the callback reports the final bytes written.
`done()` only confirms local transmission, not a remote operation's success.

The firmware probe does not call this writer yet. An operation driver must first
send a real operation request, validate incoming requests, perform the requested
file operation, then send its actual result. Never report success merely because
a request was received. Download batch framing is now implemented in `DownloadBatchSender` and tested
on host. Uploads, filesystem operations and final remote results still need
integration and tests.
No restore payload or MDM removal is implemented.

Validation: existing stream/parser/link and P4 adapter tests; status sender tests
cover signed limits, sentinel/dictionary shape, tiny buffers, invalid ASCII,
backpressure, stable retry arguments, closure, oversized callback results and
zero progress. Python plistlib independently decodes a generated negative status.

## Download batch sender

`hcc_backup_batch.hpp` combines explicit caller-supplied `DownloadFile` entries,
the existing `BlobSender`, and `StatusReplyWriter`. For each entry it emits a
big-endian path byte length, exact path bytes without a NUL, native data frames,
and a file-end frame. It then emits a zero DWORD and a status reply. Empty files
and empty lists are valid. All paths and source callbacks are validated before
any output; lists are limited to 1024 entries and paths to 4096 bytes.

Wire reference: `mb2_handle_send_file` and `mb2_handle_send_files` in
https://github.com/libimobiledevice/libimobiledevice/blob/master/tools/idevicebackup2.c

Callers supply separate, nonoverlapping name and scratch buffers and retain all
paths/source contexts until done/error. One object handles one batch. Each pump
bounds sink calls (8 by default); `would_block` also means budget exhausted.
Source callbacks must be nonblocking. The source provider remains responsible
for opening/closing files and confining paths and symlinks to the backup root.
On failure the sender stops with a sticky error and does not append success;
there is no per-file multistatus recovery, so discard the failed stream.

Host tests reconstruct the complete wire sequence for multiple files, an empty
file/list, partial writes and TLS-style stable retries. They verify budgets,
path rejection, source failure, closed transport and invalid callback results.
This component is compiled into P4 but is not called by its negotiation probe.
No device transfer or restore is claimed by these tests.

## Post-Hello control monitoring on P4

After negotiation the P4 probe now calls `BackupLink::receive_event` to keep
reading control messages. Quiet idle does not expire; an incomplete frame that
stops making progress expires after the configured inactivity timeout (5 s).
The 30 s opening deadline only applies to Opening/Negotiating, not Ready.
Receive polling is bounded, preserves TLS retry buffers, parses at most one
frame per call and never writes responses or starts an operation.

DeviceLink Disconnect or clean transport close changes the UI to “phiên dịch vụ
đã đóng”; it does not mean success. Truncation, malformed input, unexpected
operation requests, reset or I/O failures produce Failed. The user may arm a
new probe after Disconnected and reconnect. Parsed document/event views are
borrowed and expire on the next receive call.

Host tests exercise fragmented disconnects, long idle, incomplete-frame timeout,
transport close/truncation, unsolicited download requests before an operation,
no output after Hello, and actual P4 glue with TLS WANT retry checks. The physical
iPad has verified negotiation only; this new monitoring needs a hardware test.
