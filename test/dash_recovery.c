/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include "stream/dash_recovery.h"

int main(void)
{
    // Independent numeric oracle: every libcurl completion enum is intentional.
    const int candidates[] = {5, 6, 7, 16, 18, 28, 35, 52, 55, 56, 92, 95, 96};
    int cases = 0;
    for (int phase = 1; phase <= 3; phase++) {
        for (int code = -1; code <= CURL_LAST + 1; code++) {
            bool expected = false;
            for (unsigned n = 0; n < sizeof(candidates) / sizeof(*candidates); n++)
                expected |= code == candidates[n];
            assert(mp_dash_recovery_candidate(code) == expected);
            struct mp_dash_recovery_input in = {
                .code = code, .safe = true, .supported = true,
                .limit = 5, .remaining_ms = -1, .delay_max_ms = 30000,
            };
            struct mp_dash_recovery_decision out = mp_dash_recovery_decide(in);
            assert(out.action == (expected ? 1 : 2));
            assert(out.reason == (expected ? MP_DASH_RECOVERY_CANDIDATE :
                code == CURLE_ABORTED_BY_CALLBACK ? MP_DASH_RECOVERY_ABORTED :
                                                  MP_DASH_RECOVERY_PERMANENT));
            if (!expected)
                continue;
            in.limit = 0;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_DISABLED);
            in.limit = 5; in.attempt = 5;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_EXHAUSTED);
            in.attempt = 2; in.remaining_ms = 300;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_DEADLINE);
            in.remaining_ms = 301;
            assert(mp_dash_recovery_decide(in).delay_ms == 300);
            in.remaining_ms = 0;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_DEADLINE);
            in.remaining_ms = -1; in.safe = false;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_UNSAFE);
            in.safe = true; in.terminal = true;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_TERMINAL);
            in.aborted = true;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_ABORTED);
            in.aborted = in.terminal = false; in.supported = false;
            assert(mp_dash_recovery_decide(in).reason == MP_DASH_RECOVERY_UNSUPPORTED);
            in.supported = true; in.attempt = 99; in.limit = 100;
            assert(mp_dash_recovery_decide(in).delay_ms == 3100);
            in.delay_max_ms = 10;
            assert(mp_dash_recovery_decide(in).delay_ms == 10);
            cases++;
        }
    }
    printf("DASH_RECOVERY_POLICY_PASS candidates_and_phases=%d curl_last=%d\n",
           cases, CURL_LAST);
    return 0;
}
