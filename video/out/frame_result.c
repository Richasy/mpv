// SPDX-License-Identifier: LGPL-2.1-or-later

#include "frame_result.h"

void mp_frame_receipt_begin(struct mp_frame_receipt *s, uint64_t id)
{
    *s = (struct mp_frame_receipt){id, id ? MP_FRAME_PENDING : MP_FRAME_UNKNOWN};
}

void mp_frame_receipt_report(struct mp_frame_receipt *s, uint64_t id,
                             enum mp_frame_result result)
{
    if (id && s->id == id && s->result == MP_FRAME_PENDING &&
        (result == MP_FRAME_SUCCEEDED || result == MP_FRAME_FAILED))
        s->result = result;
}

void mp_frame_receipt_finish(struct mp_frame_receipt *s, uint64_t id,
                             enum mp_frame_result backend, bool dropped)
{
    mp_frame_receipt_report(s, id,
                           !dropped && backend == MP_FRAME_SUCCEEDED
                               ? MP_FRAME_SUCCEEDED : MP_FRAME_FAILED);
}

void mp_frame_receipt_invalidate(struct mp_frame_receipt *s)
{
    if (s->id)
        s->result = MP_FRAME_FAILED;
}

enum mp_frame_result mp_frame_receipt_get(struct mp_frame_receipt *s, uint64_t id)
{
    return id && s->id == id ? s->result : MP_FRAME_UNKNOWN;
}
