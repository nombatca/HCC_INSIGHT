# P4: experimental Restore from the supplied 4uKey sample

This is an explicit, device-changing experiment, not a verified MDM removal or
activation feature. Use a test iPad with a separate known-good backup. Restore
may overwrite data/settings and the iPad may reboot. Interrupting a Restore does
not roll back changes already applied. No host reboot command is added.

## Prepare the SD card on the Mac

Obtain `restoremdm.zip` from the supplied 4uKey installation/archive. This repo
contains no third-party backup payload or decrypted files. The preparation tool
requires this exact original ZIP SHA-256:

`0d1dedeeedebb9ff477a83dfdcf0ed17c5b64cf3951d1ea9523a83e57ac480c9`

From the project directory, in the ESP-IDF Python environment, run (replace both
paths with the actual ZIP and mounted card):

```sh
python3 tools/prepare_restore_sample.py "/path/to/restoremdm.zip" "/Volumes/SD_CARD"
```

The tool verifies the complete ZIP hash, then copies exactly the outer 297 files
(12,299,936 bytes), preserving the two-character hash directories and original
encrypted bytes. The duplicate nested `restoremdm/restoremdm` tree is omitted.
Each output file is checked for size/CRC, flushed and synced before the staged
folder is renamed into `/hcc_restore/restoremdm` on the card. The tool does not
contact an iOS device, change backup passwords, decrypt blobs or delete captures.
If the destination already exists it refuses to overwrite it; move the existing
folder aside yourself if a new preparation is intended. Safely eject the card.

## Firmware and UI

Build/flash this branch using the normal project workflow. Do not erase NVS or
pair records. `idf.py build flash monitor` is sufficient when the IDF environment
and serial port are already configured.

1. Insert the prepared SD, open USB HUB, connect/unlock/trust the test iPad.
2. Tap MDM to arm/negotiate; reconnect when requested by the existing probe.
3. Wait for the device information and service negotiation. The button changes
   from **MDM** to **ACTIVE MDM** only with a complete info stage, nonempty Serial,
   Ready service and no operation already started.
4. Tap **ACTIVE MDM** once to start Restore directly. This second tap is the
   explicit action requested by the user; no extra dialog or long press is
   required. The action rechecks the Serial displayed in the ready state.
5. While running, the button shows **ACTIVE MDM / Đang chạy** and ignores extra
   clicks. Completion shows **RESTORE / Hoàn tất** only after the terminal device
   result. Reconnect for another session.
6. Long press remains an advanced shortcut to the previous capture/Restore dialog.
   Preserve the serial log from `[HCC RESTORE] fast start` to the final result.
   Do not use other SD apps during the operation.

The request is serial-bound at the explicit ACTIVE MDM tap (or advanced-dialog confirmation) and accepted only once in a Ready
session. A new operation requires a new negotiated stream. Before sending any
Restore bytes, the USB owner task acquires the shared SD lease, creates a separate
capture session for inbound device uploads, opens the source root and sends Restore without scanning all 297 files. Only
requested files are opened: their size is checked before reading and their CRC32
is checked while streaming. Missing/corrupt requested files fail when encountered;
unrequested files do not delay startup. A late error cannot undo data already
sent/applied. Mount, source-root, space and SD-ownership checks remain. CRC32 is an
accidental-corruption check, not cryptographic source authentication. The Mac
preparation tool's ZIP SHA-256 establishes which supplied package was used.

Expected log boundary:

```
[HCC RESTORE] fast start: full SD scan skipped; requested files checked while sending
```

Restore operation/watchdog/control logs use `[HCC RESTORE]`; capture logs keep
`[HCC BACKUP]`. There is no phase-12 full-scan delay in the firmware path.
A successful file/status write is not operation success. Only a terminal device
`ErrorCode=0` sets **Restore: iPad báo hoàn tất**. Disconnect/reboot before a final
response, nonzero device error, deadline, media error or transport failure remains
failed/unconfirmed. Do not infer success from a reboot or number of files sent.

## Implemented scope

The request follows the observed `DeviceRestoreOptions::DeviceRemoveMDM` profile
in the supplied binary: MessageName Restore, current connected UDID as source and
target, sample password `a`, RestoreDontCopyBackup true, RemoveItemsNotRestored,
RestoreSystemFiles, RestorePreserveSettings and RestorePreserveCameraRoll false.
This records the observed request, not a claim about modern iPadOS effects.
No native legacy `DeviceRemoveMDM` metadata-rewrite helper is reproduced.

On `DLMessageDownloadFiles`, only names in the compiled sample index are served.
Both `<UDID>/<shard>/<hash>` and equivalent flat hash requests resolve to the
indexed file; the actual source tree remains sharded. `Info.plist` maps to the
sample's lowercase `info.plist`. Arbitrary SD paths, other-device prefixes,
traversal and symlinks cannot select source files. Unknown files get the existing
explicit missing-file response. Each reader lazily opens one file and checks its
size and streaming CRC again. TLS retries keep stable buffers. Up to 1024 paths
and 128 KiB of copied request names are bounded in the PSRAM operation context.

Directory replies are reconstructed directly from the known index, avoiding
capture metadata scans. Uploaded data is staged separately in `hcc_backup`.
Source create/move/remove requests get an explicit negative read-only response;
the template is never mutated or silently acknowledged. Unsupported protocol
requests fail. No firmware update/DFU or full-device erase command is added.

## Limits and validation

The sample metadata describes an old source device; broad iOS/iPadOS compatibility
and the exact on-device effect are unknown. Previous analysis found that direct
SHA-1 comparisons did not match stored Digest fields; this change neither edits
those fields nor claims to have resolved their semantics. The original bytes are
preserved, rather than manufacturing a modified backup.

Host validation covers exact request decoding with independent plistlib, original
297-file preflight/streaming offline (optional source API), fast startup without
reading unrequested files, missing/corrupt/changed requested sources, path aliases,
sharded tree preparation, source immutability, symlink confinement, partial TLS
writes, exact 70 KiB+metadata transmission, explicit final result, cancellation
before the first request write, closure and deadlines. SDK/TLS stubs do not prove real USB/TLS or
Restore compatibility. The P4 context is about 2.72 MB in PSRAM; allocation or
real FAT errors still need hardware confirmation. Legacy SD users do not all
participate in the shared lease.

## Hardware checkpoint and fast-start change (2026-11-16)

The user supplied a terminal `DLMessageProcessMessage`, MessageName Response,
ErrorCode 0 followed by `RESTORE complete files=56` for the 120fed3 baseline. This
confirms that Restore session's completion, not persistent MDM removal. The
preceding wait log showed elapsed_ms=228717; it is not an exact final duration.
At the user's request, subsequent firmware skips the full SD preflight; the same
SD sample remains compatible. Fast-start duration still requires a hardware run.


## Separate sample17 experiment button (iOS 27 trial)

The USB screen now has five action buttons. First use MDM to negotiate the service and wait for complete device information / ACTIVE MDM. The separate **RESTORE iOS27 / THỬ (bộ 17)** button selects the supplied `restoremdm17.7z` sample explicitly. It is disabled before Ready and during any operation. It uses the displayed Serial guard and one-operation-per-stream gate. Reconnect after a terminal result to run another experiment.

This is a trial of the existing HCC Restore protocol with a different backup, **not an implementation or verification of 4uKey's iOS 27 algorithm**. No automatic OS-version selection is made. The old ACTIVE MDM button retains the legacy 297-file source.

Extract `restoremdm17.7z` on the computer. Copy the resulting `restoremdm17` folder into `hcc_restore` at the SD root, preserving shard directories. The P4 does not decompress 7z. Expected source:

```
/sdcard/hcc_restore/restoremdm17/Info.plist
/sdcard/hcc_restore/restoremdm17/Manifest.plist
/sdcard/hcc_restore/restoremdm17/Manifest.db
/sdcard/hcc_restore/restoremdm17/Status.plist
/sdcard/hcc_restore/restoremdm17/<shard>/<hash>
```

The exact supplied archive SHA-256 is `e432e664de8080f2454e5ed88d8cbdbae1d5db3c67b878c2d173157656bd1d6f`. The immutable index contains 857 files / 21,074,143 bytes (853 blobs and four metadata files). The existing request password independently opened its keybag and produced a SQLite Manifest header. This confirms sample readability, not device compatibility. All requested files retain size / streaming CRC / EOF checks, and both Info.plist case aliases work. Full SD scanning is still skipped on P4. A missing selected root fails before request; there is no fallback to the other sample.

Optional computer-side verification and staging (standard Python, no device connection):

```sh
python3 tools/prepare_restore_sample17.py "/path/extracted/restoremdm17" "/Volumes/SD_CARD"
```

This verifies all indexed sizes/CRCs before publishing the prepared folder, refuses existing destinations and leaves the legacy folder alone. No third-party backup payload is distributed in Git. Logs identify selection:

```
[HCC RESTORE] selected sample=17 files=857 root=/sdcard/hcc_restore/restoremdm17; experimental, OS compatibility unverified
```

The selected source uses the same Restore request/options as the baseline. Success still requires a terminal device ErrorCode=0; restart/disconnect does not mean success or persistent MDM removal. No hardware or iOS 27 result is claimed by host tests.


## Diagnose a transfer stop

A stop with phase=5 / reason=6 is a sender failure, not a specific device rejection. Before cleanup, the operation now logs `DownloadFiles send failed` with the bounded requested path, batch item, original error, error origin (`source_read`, `sink_write`, `status_write`, or framing), and completed payload bytes for the current blob. Source failures add `source failure` with the SD path and step (`stat_type_size`, `open`, `read`, `extra_data`, `eof_read`, `crc`, or `close`), sizes/CRC and relevant errno values. Logs are emitted on errors only; transfer bytes, checks and request options are unchanged. `completed_batches_files` still counts prior completed responses, not files applied on the device.
