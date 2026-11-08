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
a request was received. Download batch filenames, batch termination, uploads,
filesystem operations and final remote results still need integration and tests.
No restore payload or MDM removal is implemented.

Validation: existing stream/parser/link and P4 adapter tests; status sender tests
cover signed limits, sentinel/dictionary shape, tiny buffers, invalid ASCII,
backpressure, stable retry arguments, closure, oversized callback results and
zero progress. Python plistlib independently decodes a generated negative status.
