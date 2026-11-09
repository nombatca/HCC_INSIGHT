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
relative destination), then native data frames and a 0x00 or context-valid 0x0B terminal.
A zero source-name length ends the batch without reading subsequent bytes.
`completed_payload_bytes()` counts fully accepted data chunks, not durable bytes.

Reference: `mb2_handle_receive_files` in the libimobiledevice source linked above.
Its 0x0B handling is context-sensitive, including a non-error use after data.
The receiver accepts 0x0B after a fully delivered data frame, consumes its entire
bounded body without writing it as payload, then commits the file. A first-frame
0x0B drains its bounded body and aborts. A truncated body, unknown tag, cancellation
or storage failure never commits that file. Both bodyless and nonempty terminals
are supported. This follows the reference last-data rule; physical iPad capture
is still needed to validate device-specific sequences. No success status
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
every truncation position, invalid paths/name sizes, 0x0B context/fragmentation/truncation, write/commit
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
MDM button. Other filesystem control requests and physical bulk iPad-to-SD
testing are still outstanding.

P4 integration must allocate the handler's bounded state (including two 4096-byte
name buffers) off the small USB task stack, and schedule synchronous file I/O
without concurrent access to the USB/TLS transport. The current wrapper does not
create tasks or make HCCUSBTransport thread-safe; transport/task scheduling is
part of the still-missing operation driver.

## Backup request preparation and start state

`encode_backup_request` encodes the Backup operation only, wrapped in a DeviceLink
process message. TargetIdentifier and SourceIdentifier use the same validated
UDID (40 hex characters or 8 hex + hyphen + 16 hex). Default mode omits Options;
explicit full mode adds `ForceFullBackup: true`. No restore, encryption change,
password or management-profile options are supported.

Reference: `mobilebackup2_send_request` / `mobilebackup2_send_message` in
https://github.com/libimobiledevice/libimobiledevice/blob/master/src/mobilebackup2.c
and Backup handling in the previously linked idevicebackup2 tool source.

`BackupStartWriter` takes an observer obtained from a negotiated link and a
caller-owned output buffer. It serializes the request before any writes and
marks the observer running only after all request bytes are accepted by the
transport. Each pump makes at most one write callback and retains stable retry
arguments on would-block. Sent means local transmission, not remote acceptance
or backup success. Write failure or cancelling a prepared request makes the
observer fail; abandon that stream rather than replaying a partial operation.
Invalid identifiers, wrong initial state or insufficient encoding storage do
not start an operation. Each writer sends at most one request.

Caller must reserve storage and a complete operation handler before using this
writer with a real device. The current upload wrapper opens capture per request,
so it is not yet an operation-wide storage reservation or full preflight. A real
driver must hold the lease for the entire operation, handle other filesystem
requests, ensure adequate space, schedule file I/O and enforce deadlines.
The MDM probe does not instantiate this writer or automatically start a backup.

Tests independently decode both encoder modes with Python plistlib, test modern
and legacy IDs, invalid input and tiny buffers, partial/WANT writes, no early
running state, write closure/error and cancellation. The host end-to-end upload
flow now actually sends the encoded Backup request into simulated I/O instead
of manually setting running; final operation success still requires a separate
final result. No physical backup/restore is claimed.

## Operation-wide prototype coordinator

`HCCBackupOperation` combines negotiated link, space/mount preflight, one Backup
request, repeated UploadFiles requests, status replies and final result handling.
It opens one capture session and retains the SD lease throughout all batches,
including timeout/cancel/final state until the object is destroyed. Quotas for
file count and payload bytes are cumulative across requests, not reset per batch.
Previously committed files survive later failures; pending data is aborted.

It validates configuration/UDID/output bounds before storage preflight, then
checks free bytes with `esp_vfs_fat_info` before sending anything. Mount, free-space
query, insufficient free space or ownership failures leave the observer ready
and send no operation request. Minimum free bytes is a snapshot threshold, not
reserved capacity or a promise that the whole backup fits. Legacy filesystem
users can still consume space and actual write failures remain terminal.
The apps component explicitly depends on fatfs for this API.
Espressif API reference:
https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/fatfs.html

Polling bounds transport calls and tracks successful byte progress. Configurable
idle and total deadlines default to 60 s and 10 min, with monotonic-clock checks;
the link's own partial-control-frame timeout may be stricter. Unsupported control
operations, remote error, disconnect, quota, I/O failure and cancellation remain
distinct failures. During a running operation, an integer ErrorCode marks a
terminal result (zero succeeds), even when MessageName is present. A named
process/progress message without ErrorCode is not completion. Local cancel does not send a remote
cancel packet: caller must close/abandon that stream. Calls on this object and
its transport remain serialized; there are no internal threads or task handoffs.

The prototype handles Backup/UploadFiles, GetFreeDiskSpace and CreateDirectory. DownloadFiles and the other
filesystem requests are still unsupported. Raw 0x0B follows the reference
context rule described above. Therefore
it is NOT a complete device backup workflow and is not enabled by the MDM UI.
Keep this context and its large handler outside the small USB task stack and
schedule blocking SD work appropriately before physical integration.

End-to-end host cases cover two upload batches sharing one lease/session, full
request transmission, physical temporary files and final result; preflight low
space/query error/USB owner/invalid ID with no writes; cumulative byte/file quota;
cancel during a staged file; idle/total deadlines; clock reversal; unsupported
control, remote failure and disconnect. BSP, free-space information and device
I/O are simulated, while actual production coordinator and file storage run.
Physical bulk iPad-to-SD behavior and a complete Apple-restorable output remain untested.

## Physical SD readiness test

`HCCBackupSdSelfTest` runs a pinned worker with the production capture/storage
adapter. On Apple Normal mode after MDM Ready, long-press MDM starts it; short
press retains the probe action and clears a completed test result. Android
FASTBOOT remains short-click only. UI status reads atomic result codes and the
USB task uses finite polling while the test runs. No LVGL objects or device
transport are accessed by the file worker.

The test writes 64 known binary bytes plus identity metadata, flushes/syncs/
closes and renames via the actual target, reads back exact content/EOF, then
checks abort cleanup of a second file and preservation of the first. A newly
created HCC capture directory retains the committed sample. Ownership blocks
USB DISK and is released before publishing the final result. Duplicate requests
are rejected; startup, SD busy and low-space failures report explicit states.

Host tests execute the production worker with a task-launch stub and real local
files. The user subsequently reported three physical P4 passes on 2026-11-08
(`rx_000002` through `rx_000004`, 64 bytes, commit=1, abort=1). This verifies
the small SD test, not bulk device backup. See P4_SD_SELFTEST.md for the actual hardware test procedure.

Capture refuses to mount an unmounted card when BSP auto-format-on-mount-failure
is enabled. An already mounted card may still be used. A dedicated host build
with that flag enabled asserts the mount function is never called and the SD
lease is released on rejection. This guard applies to capture/self-test calls,
not legacy code elsewhere that invokes the BSP mount API directly.

## GetFreeDiskSpace control response

The running operation accepts `DLMessageGetFreeDiskSpace`, queries FAT while
holding its SD lease, and sends a binary plist status with an integer fourth
item. The advertised capacity is the minimum of physical free space and the
remaining cumulative payload quota. It is a snapshot and excludes future
metadata overhead; storage write errors remain terminal. Zero is valid.
The observer remains request-pending through partial/TLS retry writes and only
returns to running after the whole status frame is accepted. Query failure,
invalid FAT totals, failed writes, cancel and deadlines fail the operation.
This handler is part of the prototype coordinator, not enabled in the MDM UI.

### Terminal regression tests

Host tests cover 0x0B with zero/one/multiple scratch chunks of terminal body,
with and without preceding data, fragmented reads and stable retry arguments;
every truncation before terminal completion; terminal size limits, cancellation
and commit failure. A real local staged file and the UploadFiles/status/final
result pipeline also exercise the after-data terminal. None is a physical iPad
backup test. No restore or MDM-changing operation is introduced.

## CreateDirectory capture namespace

The running operation accepts `DLMessageCreateDirectory` with a string path,
validates it, creates parents under `<session>/directories/`, then sends status
zero with the DeviceLink empty-parameter sentinel. It acknowledges the pending
request only after the complete status frame is accepted. Query/type/path/storage
errors and failed sends remain terminal for this prototype; no false success
reply is sent. Response buffer capacity is checked before creating directories.

This is a capture directory namespace, not a materialized Apple backup tree:
uploaded payload/identity pairs remain in indexed `.ready` records. A future
listing handler must combine that mapping with this namespace; listing is still
unsupported. No Backup or directory operation is started from the MDM UI.

Bounds: full local path at most 512 bytes including NUL, at most 32 relative
components, each at most 255 bytes, and 1024 newly created directories per session
including the namespace root/parents. Both slash styles normalize to `/`. Reject
absolute/traversal/empty components, control/FAT-reserved characters and trailing
dots/spaces. Validate the entire path before mutation. Host traversal rejects
symlinks at each component; ESP FAT has no symlinks. Trusted ancestors and
exclusive mount/session ownership remain required (no concurrent mutation).
Existing directories succeed; files/symlinks are never replaced. An error or
cancel may retain created parents; mkdir is not a rollback transaction or a
power-loss durability guarantee.

Host tests cover malformed/state/path requests, nested/repeated creation, busy
upload state, prevalidation limits, collisions/symlinks and directory quota; the
production coordinator runs create/status/upload/final-result with real local
files, fragmented I/O, failed send, cancellation and storage/path/quota failure.
Physical iPad/SD behavior for this command remains untested.

## DLContentsOfDirectory listing

The running operation now accepts `DLContentsOfDirectory` with a string path and
replies with a `DLMessageStatusResponse` whose fourth item is a dictionary mapping
each immediate child name to a sub-dictionary of `DLFileType`
(`DLFileTypeRegular`/`DLFileTypeDirectory`), `DLFileSize` and
`DLFileModificationDate` (a Mac-epoch date). An empty string lists the backup
root; any other path must be relative and traversal-free. It acknowledges the
pending request only after the whole status frame is accepted.

Wire reference: `mb2_handle_list_directory` in
https://github.com/libimobiledevice/libimobiledevice/blob/master/tools/idevicebackup2.c
The implementation is independent; the reply is serialized by a new bounded
binary-plist writer (`encode_contents_of_directory_response`) that supports an
arbitrary object count with variable object-reference and offset-integer widths,
UTF-16 names and date values — the existing 16-object BinaryWriter cannot encode a
listing. The project parser now also decodes bplist dates (exposed as reals).

Because the capture is NOT a materialized Apple backup tree, the listing is
reconstructed in `StagedUploadTarget::list_directory`: it reads every committed
`NNNNNN.ready` record's destination from identity.bin and combines those with the
`<session>/directories/` namespace. A destination directly inside the requested
path is a regular file (size and modification time taken from payload.bin); a
deeper destination contributes its first component as an implied directory; the
namespace supplies explicitly created subdirectories. Internal capture names
(NNNNNN.ready/.part, payload.bin, identity.bin) are never exposed.

Safety and bounds: the requested path is validated (no absolute/traversal
components); host enumeration rejects symlinks per record and per namespace entry
(ESP FAT has none); a name that is both a file and a directory prefix, or corrupt
metadata, is a loud error, never a dropped entry. A directory with no children is
a valid empty dictionary with status 0, but an I/O/metadata error or exceeding the
caller's bounded entry capacity fails the operation rather than truncating or
returning an empty listing on error. Listing storage (entries + the plist object
table) is caller-owned and kept outside the operation object so it stays off the
small USB task stack; the coordinator refuses listings when it is not provisioned.
Partial/TLS-retry writes keep the observer request-pending until the whole frame
is sent, matching CreateDirectory/GetFreeDiskSpace.

Host tests (`test_listing`, `test_protocol`, `test_sd_access`) cover the
reconstructed tree (nested files, implied and created directories, root
collapsing), empty and missing directories, traversal rejection, capacity
overflow, file/directory collision, corrupt identity metadata, namespace symlink
rejection, the encoded reply decoded back to the expected envelope and file
dictionary (empty and populated), and the production coordinator driving a real
empty and a real multi-entry listing to completion. All pass under
AddressSanitizer/UndefinedBehaviorSanitizer. No restore or MDM-changing operation
is introduced, and no listing is started from the MDM UI. Physical iPad/SD
behavior for this command remains untested.

## Current P4 integration and lifecycle review

The latest branch includes a prototype capture action: after DeviceLink Ready,
long-press MDM for the SD self-test; after PASS, long-press again to request a
bounded device-to-SD Backup. Labels distinguish these steps. DownloadFiles
currently reports requested files absent only while the fresh capture has no
committed files; after an upload, nonempty download requests fail explicitly
until lookup/serving existing records is implemented. Listing, capture and final
result handling do not make this an Apple-restorable backup or an MDM change.

Each negotiated stream accepts one operation. Operation state is constructed on
demand in PSRAM. Completion/failure/disconnect destroys the operation, aborts any
pending staged file, preserves committed files/count and releases its SD lease.
Completed streams are parked; failed streams stop protocol I/O. Neither silently
restarts; reconnect and negotiate before another capture. Cancelling DownloadFiles
now follows the same terminal path as upload/reply, with no further writes.

The firmware-glue host runner now links the real coordinator, storage and core
sources with BSP/I/O stubs. Tests include real staged payload/metadata files,
TLS-style partial retries, final result, invalid ID/preflight, timeout, closed
transport, teardown while waiting and teardown during an open upload. Successful
completion and every failure release the SD lease; a second request on the same
stream is rejected. Tests do not replace firmware/hardware validation.

Listing review also rejects symlinks in namespace ancestors, out-of-range source
metadata lengths, invalid stored destinations and trailing identity bytes. These
conditions fail the operation instead of exposing an ambiguous listing.

Hardware evidence so far: protocol 2.1 negotiation and the small 64-byte SD test.
The supplied log ends with no Backup/Restore request sent; it does not establish
that this capture action has run on P4. Synchronous FAT work is still on the USB
owner task; physical bulk-transfer timing and unsupported control requests need
the next hardware test. Never erase flash/NVS as part of this test.

## DownloadFiles missing-file response correction

A physical P4 log reached `DLMessageDownloadFiles` and sent TLS data, but did not
show the next control request or an operation result. This narrows the observed
stop to the download-response boundary; it does not prove a transport failure.
Review against `mb2_handle_send_file`, `mb2_handle_send_files`,
`mb2_multi_status_add_file_error` and `errno_to_device_error` in the primary
libimobiledevice source found a concrete wire mismatch: the previous sender
returned -13 with an empty dictionary and continued after absent files.

The corrected sender stops at the first absent file after its length-prefixed
path and raw 0x06 error frame, sends the zero DWORD batch terminator, then sends
`[DLMessageStatusResponse, -13, Multi status, {path: {DLFileErrorString:
No such file or directory, DLFileErrorCode: -6}}]`. All-success batches still
send status zero. `completed_files()` counts attempted entries including the
missing one; later source callbacks are never read after the first missing file.
The full multi-status capacity/encoding is validated before emitting raw bytes.
UTF-8 paths are encoded as ASCII or UTF-16 as appropriate.

Tests check the raw frame sequence, exact path-keyed dictionary and signed codes,
stopping before a later source, mixed success/absent batches, bounded output,
long/Unicode paths, malformed input, partial writes and stable retries. Python
plistlib independently decodes ASCII/Unicode multi-status fixtures. New device
logs mark `DownloadFiles reply complete ... waiting for next control` and failure
phase/reason. This fixes the identified mismatch; another iPad/P4 capture test
is required to establish whether the device proceeds past this boundary.

## ProcessMessage result classification and diagnostics

The latest physical log contains `event kind=7 msg=DLMessageProcessMessage`.
That outer tag alone does not reveal whether the device reports progress, a
terminal error, or completion. The previous observer ignored every dictionary
with MessageName before checking ErrorCode. Primary idevicebackup2 handles the
integer ErrorCode in the operation loop irrespective of MessageName; ignoring
that field can leave a named terminal result waiting until timeout.

The observer now keeps Hello/negotiation nonterminal while phase is ready, but
while running an integer ErrorCode becomes a result even with MessageName. Zero
means success and nonzero preserves the device error code. Named messages with
no ErrorCode remain nonterminal; malformed code types are not coerced. Request-
pending states still reject an out-of-order result. The protocol demo/test
handshake order was corrected to begin the simulated operation after Hello.

P4 logs now include selected ProcessMessage fields before handling the result:
MessageName (up to 64 bytes), ErrorCode, ErrorDescription (up to 240 bytes).
Control characters are replaced and output is bounded/NUL-terminated. Content
is reported only by type/size; its raw value is never dumped. Formatting is
portable and tested independently of ESP_PLATFORM.

Regression tests cover named zero/nonzero results, progress without a code,
Hello before the operation, malformed codes, output capacities, sanitized text
and Content omission. Coordinator and P4 glue tests exercise named results
through the actual cleanup path, preserving committed files and releasing SD.
A physical rerun with these diagnostics is needed to identify the latest iPad
response; its ErrorCode/description cannot be recovered from the encrypted
USB bytes in the supplied log.

## Independent capture deadline and wait diagnostics

The latest physical log confirms SD self-test PASS (`rx_000022`), Backup request
start, and completion of one attempted DownloadFiles response. No next control
message or terminal result appears in that excerpt. Minute-only UI clock lines
do not establish the actual idle interval, and USB TX completion alone does not
prove the iPad accepted the application response. The cause is still unresolved.

Review found that `submitPendingTcp` returns early while a USB TX is in flight,
while operation deadlines were checked only inside protocol `poll`. A prolonged
TX can therefore skip the capture deadline check. `checkDeadline` now provides
a no-I/O, owner-task deadline check, and the transport invokes it before its
TX/failed-state early returns. Monotonic, idle (60 s default), and total (10 min
default) limits are unchanged. Failure cancels any staged file and releases SD
through the existing terminal path. This is not an ISR/hardware watchdog and
cannot run while the owner task itself is blocked in synchronous FAT work.

While a capture is active, the same hook logs a snapshot at most every five
seconds (plus a terminal snapshot): phase, elapsed/idle milliseconds, USB TX
in-flight flag, service TCP state, buffered TLS RX/TX bytes, plaintext RX bytes,
and whether a partial DeviceLink control frame is pending. No payload contents
are logged. Heartbeats do not reset progress/deadlines or perform transport I/O.
A wait snapshot is diagnostic, not proof of successful backup.

Host adapter tests cover timeout with a permanently in-flight TX, a nonterminal
heartbeat that does not consume/send bytes, clock reversal, failed outer
transport and timeout during an open staged upload. They assert .part cleanup,
preserved committed count, released SD lease and no same-stream restart. SDK
callbacks/physical USB/SD timing still require a rerun on P4.


Physical P4 directory quota correction (2026-11-09):
The device continued beyond DownloadFiles into GetFreeDiskSpace and repeated
CreateDirectory requests, then stopped with local quota error 5 and zero files.
The previous 256-directory cap counted namespace and parent directories too,
preventing a 256-shard tree from fitting. Raise the bounded cap to 1024; path,
depth, payload quotas and deadlines remain enforced. Host storage regression
creates all 00..ff children plus parents, fills the remaining allowance, rejects
the next new directory and accepts existing directories at the cap. This does
not establish that file transfer or a complete backup succeeds on hardware.


Physical P4 TLS burst correction (2026-11-09):
After directory preparation, the device sent consecutive 32764-byte USBMUX
packets (32728-byte TCP payloads). The 64KiB service ciphertext ring overflowed
with 33185 buffered bytes before another 32728-byte append. MDM intentionally
owned ssl_read, but only polled after the USB event handler returned.

The RX append path now pumps that same MDM reader before/after buffering when
TLS is established. Each pump has a 32-poll cap and stops on no read/phase
progress, preserving WANT buffer ownership. TLS output is staged only; USB
submission remains deferred, and pending USB TX does not block upload reads.
The ciphertext ring remains 64KiB and still fails closed if no consumer can
make room. This is owner-task callback work, not a concurrent TLS reader.

Host regression uses the production ring append and real coordinator/storage
with a transparent TLS stub: back-to-back 32728-byte chunks containing a 256KiB
upload, with USB TX busy and no outer protocol polls between appends. It checks
the exact committed file, deferred status and terminal result. It also checks
nonconsuming ring overflow does not overwrite data. Real TLS/USB/SD timing still
requires the P4 test; synchronous SD callback latency remains a limitation.
