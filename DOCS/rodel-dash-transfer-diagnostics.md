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

## Bounded same-request recovery

Consumer ADR-0099 centralizes curl recovery classification and admission in
`stream/dash_recovery.h`. Initial headerless connections, later headerless
continuations and interrupted identity bodies use the same policy. DNS/proxy-DNS,
connect, send/receive, empty reply, timeout, partial-file, HTTP2/HTTP2-stream,
HTTP3 and QUIC-connect errors are candidates. SSL-connect (35) is a bounded
same-policy re-handshake candidate, **not** evidence that TLS failed transiently.
Certificate verification/loading, CA, cipher, engine, client key, configuration,
allocation, callback and every unlisted/unknown completion error stop.
TLS configuration is never changed. HTTP statuses, including 5xx, never admit
same-response recovery; Desktop owns its separately bounded backup URL policy.

Initial retries require no status, headers or accepted bytes. Header continuation
requires previously validated range coordinates. Body retries require a known,
incomplete identity response, no Transfer-Encoding, and a strictly interior
accepted-byte offset. Every new response passes existing status/range/total/length
validation again. Accepted ring bytes are retained. At least 1 MiB accepted in
one response replenishes consecutive retries; tiny failures never accumulate
progress credit. This does not replenish the recovery episode's absolute deadline.

`curl-max-retries` (default 5, zero disabled) bounds consecutive attempts.
Backoff is 0, 100, 300, 700, 1500, then at most 3100 ms, capped by
`curl-retry-delay-max`. Open, outstanding read and recovery episode monotonic
deadlines are intersected, never extended by scheduling or reconstruction.
Cancellation and source-wide terminal failures (including the other track)
stop before submission and after curl multi admission. There is no API reread,
URL/backend/account switch or automatic UI Reload.

The observed lavf path deliberately does not implement same-offset error
reconstruction. Its HTTP observer admits exactly one response and explicitly
sets FFmpeg `reconnect=0`; interrupted operations fail closed. Explicit completed
range progression is not error recovery.

### Private-safe warning grammar

The curl module emits exactly:

```
DASH recovery: stream=%llu track=%d phase=%d action=%d reason=%d curl=%d attempt=%d limit=%d delay_ms=%d remaining_ms=%lld
```

No URL, offset, TLS text, identifier or header value is included. Track is 1 video
or 2 audio. Stream is the nonzero per-core curl request-sequence identifier at
recovery episode entry, unchanged across its retries. A later episode following
a completed response/seek captures the new request ID. It is a correlation key,
not a content identity. The unsupported lavf event uses its separate monotonic
stream-open sequence in the high-bit namespace, and its normal stream log module.
Phase: 1 initial, 2 continuation headers, 3 partial body.
Action: 1 scheduled admission, 2 stop, 3 actual curl-multi admission after stop/
deadline rechecks. Scheduled is not proof of execution or success; action 3 is
not proof that headers/body arrived. No steady-state success event is emitted.
Reason: 1 candidate, 2 disabled, 3 exhausted, 4 deadline, 5 aborted, 6 unsafe byte/
response prerequisites, 7 permanent/unclassified, 8 source terminal, 9 unsupported
backend. Attempts count admitted retries (not original request); limit is the
configured maximum. Delay is the admitted wait, zero on a refused admission.
Remaining is -1 only without a configured deadline, otherwise nonnegative ms.
libcurl 0 means actual CURLE_OK, never unknown; unavailable backend code is -1.
