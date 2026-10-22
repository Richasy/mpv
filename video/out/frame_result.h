// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef MP_FRAME_RESULT_H
#define MP_FRAME_RESULT_H

#include <stdbool.h>
#include <stdint.h>

enum mp_frame_result {
    MP_FRAME_UNKNOWN,
    MP_FRAME_PENDING,
    MP_FRAME_SUCCEEDED,
    MP_FRAME_FAILED,
};

struct mp_frame_receipt {
    uint64_t id;
    enum mp_frame_result result;
};

void mp_frame_receipt_begin(struct mp_frame_receipt *s, uint64_t id);
void mp_frame_receipt_report(struct mp_frame_receipt *s, uint64_t id,
                             enum mp_frame_result result);
void mp_frame_receipt_finish(struct mp_frame_receipt *s, uint64_t id,
                             enum mp_frame_result backend, bool dropped);
void mp_frame_receipt_invalidate(struct mp_frame_receipt *s);
enum mp_frame_result mp_frame_receipt_get(struct mp_frame_receipt *s, uint64_t id);

#endif
