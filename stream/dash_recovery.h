/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Typed media recovery policy; no transport mutation or clock reads here.
 * See consumer ADR-0099 and DOCS/rodel-dash-transfer-diagnostics.md. */
#ifndef MP_DASH_RECOVERY_H
#define MP_DASH_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>
#include <curl/curl.h>

enum mp_dash_recovery_phase {
    MP_DASH_RECOVERY_INITIAL = 1,
    MP_DASH_RECOVERY_HEADER = 2,
    MP_DASH_RECOVERY_BODY = 3,
};

enum mp_dash_recovery_reason {
    MP_DASH_RECOVERY_CANDIDATE = 1,
    MP_DASH_RECOVERY_DISABLED = 2,
    MP_DASH_RECOVERY_EXHAUSTED = 3,
    MP_DASH_RECOVERY_DEADLINE = 4,
    MP_DASH_RECOVERY_ABORTED = 5,
    MP_DASH_RECOVERY_UNSAFE = 6,
    MP_DASH_RECOVERY_PERMANENT = 7,
    MP_DASH_RECOVERY_TERMINAL = 8,
    MP_DASH_RECOVERY_UNSUPPORTED = 9,
};

static inline bool mp_dash_recovery_candidate(CURLcode code)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_PARTIAL_FILE:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
#if LIBCURL_VERSION_NUM >= 0x074200
    case CURLE_HTTP3:
#endif
#if LIBCURL_VERSION_NUM >= 0x074500
    case CURLE_QUIC_CONNECT_ERROR:
#endif
        return true;
    default:
        // Includes success, unknown/future errors, certificate/CA/client key/
        // cipher/engine/configuration failures, callbacks and allocation errors.
        return false;
    }
}

struct mp_dash_recovery_input {
    CURLcode code;
    bool safe, aborted, terminal, supported;
    int attempt, limit;
    int64_t remaining_ms; // -1: no configured deadline
    int delay_max_ms;
};

struct mp_dash_recovery_decision {
    int action; // 1 retry, 2 stop
    enum mp_dash_recovery_reason reason;
    int delay_ms;
};

static inline struct mp_dash_recovery_decision mp_dash_recovery_decide(
    struct mp_dash_recovery_input in)
{
    struct mp_dash_recovery_decision out = {.action = 2};
    if (in.aborted || in.code == CURLE_ABORTED_BY_CALLBACK)
        out.reason = MP_DASH_RECOVERY_ABORTED;
    else if (in.terminal)
        out.reason = MP_DASH_RECOVERY_TERMINAL;
    else if (!in.supported)
        out.reason = MP_DASH_RECOVERY_UNSUPPORTED;
    else if (!mp_dash_recovery_candidate(in.code))
        out.reason = MP_DASH_RECOVERY_PERMANENT;
    else if (!in.safe)
        out.reason = MP_DASH_RECOVERY_UNSAFE;
    else if (in.limit <= 0)
        out.reason = MP_DASH_RECOVERY_DISABLED;
    else if (in.attempt >= in.limit)
        out.reason = MP_DASH_RECOVERY_EXHAUSTED;
    else {
        // Bounded exponential backoff; the first reconnection is immediate.
        int exponent = in.attempt < 5 ? in.attempt : 5;
        int delay = ((1 << exponent) - 1) * 100;
        if (delay > in.delay_max_ms)
            delay = in.delay_max_ms;
        if (in.remaining_ms >= 0 && delay >= in.remaining_ms) {
            out.reason = MP_DASH_RECOVERY_DEADLINE;
        } else {
            out.action = 1;
            out.reason = MP_DASH_RECOVERY_CANDIDATE;
            out.delay_ms = delay;
        }
    }
    return out;
}

#endif
