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
3. When the service is Ready, hold MDM to show the operation dialog.
4. Check the Serial and source path. Choose **TÔI ĐỒNG Ý RESTORE THỬ** only if the
   displayed test device may have its data/settings changed. **NHẬN BACKUP**
   retains the capture action; **HỦY** sends neither request.
5. Preserve the full serial log from `[HCC RESTORE] fast start` to the final
   response/failure. Do not repeatedly press actions or use other SD apps.

The request is serial-bound at confirmation and accepted only once in a Ready
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
