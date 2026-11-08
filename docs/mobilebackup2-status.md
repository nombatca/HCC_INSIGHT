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

## Raw upload receiver (host-tested subset)

`UploadBatchReceiver` handles raw records after an operation driver accepts an
UploadFiles request. A record has two length-prefixed names (source metadata and
relative destination), then native data frames and a clean 0x00 file terminator.
A zero source-name length ends the batch without reading subsequent bytes.
`completed_payload_bytes()` counts fully accepted data chunks, not durable bytes.

Reference: `mb2_handle_receive_files` in the libimobiledevice source linked above.
Its 0x0B handling is context-sensitive, including a non-error use after data.
This first receiver deliberately rejects 0x0B and unknown tags; it is not yet a
complete substitute for libimobiledevice's UploadFiles handler. No success status
is automatically sent, and no new backup/restore request is introduced.

Caller supplies scratch storage (1..32768 bytes) and a target with begin/write/
finish callbacks. Source names are opaque metadata and never used as local
paths. Destination paths use HCC's relative path validation; the target must
add root confinement and symlink protection. Target writes may be partial or
would-block and retain stable retry buffer addresses and lengths. Read/write
calls share a per-pump budget; synchronous begin/finish callbacks must return a
final result and must not return would-block.

After each successful begin, finish is called once: commit only after a clean
file-end, otherwise abort. Stage writes in temporary storage and publish
atomically on commit. If commit itself fails, finish must clean up its staging
file. Explicit cancel and receiver destruction abort a pending target; target
contexts must outlive the receiver. A failed batch can contain previously
committed files: this is a per-file transaction, not whole-batch rollback.
Discard a failed protocol stream rather than continuing with an unknown offset.

Default limits: 4096 bytes/name, 1024 files, 1 MiB/frame payload, 64 MiB/file and
256 MiB/batch. All are checked before accepting the relevant payload; no file is
opened before its complete destination name is validated. Incoming names and
all state are bounded, with no heap allocation in the receiver.

Tests cover multiple wire split sizes, long data with small scratch storage,
empty file/batch, no overread, stable TLS-style and target retries, budgets,
every truncation position, invalid paths/name sizes, 0x0B rejection, write/commit
failure, file/batch/frame/count quotas and cancellation. This API is compiled
into P4 but not wired into its negotiation probe; an operation driver, complete
UploadFiles control handling and SD-backed staging still need integration.

## Filesystem target for mounted SD/VFS

`StagedUploadTarget` supplies receiver callbacks for a trusted, existing parent
on an already mounted filesystem. `open_session(parent)` exclusively creates an
unused `rx_NNNNNN` directory; every file uses a private `NNNNNN.part` directory
containing `payload.bin` and `identity.bin`. Incoming names never become local
filesystem paths. Identity format: 8 ASCII bytes `HCCU0001`, BE32 source length,
source bytes, BE32 destination length, destination bytes (no trailing NULs).

After clean file-end, the target checks flush, file sync and close results,
then renames the staged directory to `NNNNNN.ready`. Metadata and payload are
published together by that rename. A failure/cancel removes this file's staged
pair; committed files and unrelated directories are preserved. An existing
ready destination causes failure instead of overwrite. A leftover `.part`
after crash is incomplete and must not be used as a committed file.

This is an HCC capture layout, not an Apple-restorable backup directory. It
preserves exact original names in metadata for later inspection/export. Rename
publication is not a guarantee against SD power loss: directory sync and
power-failure recovery are not implemented. Target callbacks are synchronous
and must run in an appropriate worker task, not an LVGL callback.

Caller must hold exclusive ownership of the mount and this session directory,
and retain target context until the receiver is destroyed. Parent and ancestors
must be trusted; host final-parent symlinks are rejected, while ESP-IDF FAT VFS
uses the mount ownership contract. Concurrency against renames/unmounts or
external filesystem mutation is unsupported. The class does not mount SD or
manage USB DISK handoff. Existing project USB DISK mode unmounts `/sdcard`, so
that arbitration must be implemented before enabling real captures in the UI.
An app may later use a dedicated parent such as `/sdcard/hcc_backup` after that
mount/ownership check. The negotiation probe still does not instantiate a target.

Host tests run the real upload receiver against real temporary files: payload
and identity layout, empty files, abort cleanup, commit collision preservation,
destruction cleanup, missing parent and symlink rejection. Hardware FAT/SD
behavior and interruption during physical writes remain untested.

## Capture / USB DISK ownership

`HCCBackupCapture` obtains a nonblocking `SdLease` before mounting/creating the
capture parent. The target is destroyed before the lease, so pending-file abort
cleanup finishes before USB DISK may take ownership. A capture object is one
session; it must outlive its receiver. It uses `/sdcard/hcc_backup` via the BSP
mount point. Mount/open failures return an error and release acquired ownership.

USB DISK now uses the same gate before unmounting. Its start/stop requests use
atomic state transitions; final error state is published after worker ownership
cleanup. A task launch failure releases only newly acquired ownership. A running
USB disk or failed remount retains ownership until a later successful stop and
BSP remount with a non-null card. Attach/detach callbacks use compare/exchange
so they do not overwrite a concurrent Stopping state.

This protects the new capture path against USB DISK; older panic/music/EEPROM
users have not been migrated and this is NOT a project-wide SD lock. No capture
operation is started from the UI/probe yet. Call capture and file callbacks on a
worker task. Keep each lease token's method calls serialized; separate tokens
may contend across tasks. Never unmount through an uncoordinated API while a
capture exists.

Tests include real capture staging, 40,000 competing gate-acquisition attempts,
the production USB DISK request/worker code with SDK operations stubbed, startup
rollback, task launch failures, failed remount retaining ownership, recovery,
and capture mount failure. Host SDK stubs do not verify physical MSC teardown,
BSP mount behavior, PHY routing or FAT durability. Hardware tests remain needed.

## Accepted UploadFiles request driver

`SessionObserver` now recognizes UploadFiles only after an operation has been
explicitly marked running. A known request has at least four array elements;
its optional size hint is not trusted as a quota. `pending_request()` distinguishes
upload from download and prevents attaching the upload receiver to the wrong
raw stream. The passive P4 probe still rejects unsolicited UploadFiles at Ready.

`UploadRequestHandler` handles one accepted upload: prepare a bounded status
buffer, receive raw records, finish target commits, transmit status, and only
then complete the observer request. It never reads the next control frame while
raw records/status are pending, and operation outcome remains none until a real
final result is received. Core handler failures/cancellation mark transport
failure; they do not continue from an unknown raw offset. Already committed
files remain available even if status transmission fails.

`HCCBackupUploadRequest` connects that handler to the actual mounted capture
adapter and SD lease. Its handler is destroyed before its capture target/lease.
Caller supplies independent scratch/output buffers and retains the negotiated
link/observer until destruction. Destroy the wrapper after completion or failure
to release SD. The handler processes only one request; an operation driver must
create another for subsequent requests and enforce an operation deadline/cancel
policy. Synchronous SD callbacks belong on a worker task.

End-to-end host test uses the actual BackupLink, observer, P4 capture wrapper,
filesystem target, raw receiver and status writer with BSP/TLS-style I/O stubs.
It verifies handshake, explicit simulated operation state, UploadFiles control,
real payload/metadata file creation, partial status reply, no overread into final
control result, and final operation success only after that result. Negative
cases cover attempts before operation start, unsupported terminal codes and
status transport closure, undersized output storage and cancellation before
status transmission, with no false local success.

This does not send Backup/Restore and is not wired to start captures from the
MDM button. Full operation initiation, other filesystem control requests, 0x0B
semantics and physical iPad/SD testing are still outstanding.

P4 integration must allocate the handler's bounded state (including two 4096-byte
name buffers) off the small USB task stack, and schedule synchronous file I/O
without concurrent access to the USB/TLS transport. The current wrapper does not
create tasks or make HCCUSBTransport thread-safe; transport/task scheduling is
part of the still-missing operation driver.
