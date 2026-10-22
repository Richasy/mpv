# Typed DASH transfer diagnostics

`rodel-dash-transfer` is a read-only numeric string property. It is unavailable
until the first typed DASH failure has a matching transfer-completion sample.
It never contains a URL, header value, body, user identifier or native log text.

Version 1 has exactly 13 ASCII space-separated decimal fields:

1. Version (`1`).
2. Source generation (`uint64`).
3. Track (`1` video, `2` audio).
4. Track response sequence (`uint32`).
5. HTTP status (`int32`, zero means unavailable).
6. libcurl completion code (`int32`, zero means `CURLE_OK`).
7. Declared response length known (`0` or `1`).
8. Expected response bytes (`uint64`; only meaningful when field 7 is `1`).
9. Bytes accepted by the body callback (`uint64`, not a raw-wire byte count).
10. Absolute request start (`uint64`).
11. Exclusive consumer end offset (`uint64`; zero means unbounded).
12. Response headers accepted (`0` or `1`).
13. Caller cancellation observed (`0` or `1`).

The completion and its first failure are recorded under the same source lock,
before the failure wakeup. Another track or a later cancellation cannot overwrite it.
Header/auth/risk failures do not wait for transfer completion and may have no
matching sample.

The property is observational: it does not change status handling, media
admission, byte validation, recovery, cancellation or the terminal latch.
