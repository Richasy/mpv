// SPDX-License-Identifier: LGPL-2.1-or-later

#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <string.h>

#include "player/paused_refresh.h"
#include "video/out/frame_result.h"

static struct mp_refresh_source ready(void)
{
    return (struct mp_refresh_source){
        .initialized = true, .paused = true, .seekable = true,
        .video = true, .position = 1.25,
    };
}

static void apply(struct mp_paused_refresh *s)
{
    mp_refresh_apply(s, s->requested.id, s->owner_epoch, s->owner_revision);
}

static void frame(struct mp_paused_refresh *s)
{
    mp_refresh_frame_done(s, s->requested.id, s->owner_epoch, s->owner_revision,
                          true);
}

static void complete(struct mp_paused_refresh *s, struct mp_refresh_source src)
{
    mp_refresh_complete(s, src, s->requested.id, s->owner_epoch, s->owner_revision);
}

static void test_admission_and_prequeued_auto(void)
{
    struct mp_paused_refresh s = {0};
    struct mp_refresh_source src = ready();
    assert(!mp_refresh_admit(&s, src, 1, 1.25)); // unknown source
    assert(strcmp(mp_refresh_phase_name(s.phase), "unknown") == 0);
    mp_refresh_new_source(&s);
    mp_refresh_new_seek(&s);
    src.pending = true; // vf refresh before a client-message FIFO barrier
    assert(!mp_refresh_admit(&s, src, 1, 1.25));
    assert(!s.requested.id);
    src.pending = false;
    src.active = true;
    assert(!mp_refresh_admit(&s, src, 1, 1.25));
    src.active = false;
    assert(!mp_refresh_admit(&s, src, 0, 1.25));
    assert(!mp_refresh_admit(&s, src, -1, 1.25));
    assert(!mp_refresh_admit(&s, src, 1, NAN));
    assert(!mp_refresh_admit(&s, src, 1, INFINITY));
    assert(!mp_refresh_admit(&s, src, 1, 1.26));
    assert(mp_refresh_admit(&s, src, 1, 1.25));
    assert(s.phase == MP_REFRESH_REQUESTED && !s.applied.id);
    assert(s.requested.position == 1.25);
    assert(src.paused); // policy never changes its caller's source
}

static void test_exact_operation_and_actual_completion(void)
{
    struct mp_paused_refresh s = {0};
    struct mp_refresh_source src = ready();
    mp_refresh_new_source(&s);
    assert(mp_refresh_admit(&s, src, 10, 1.25));
    // Arbitrary old SEEK/PLAYBACK_RESTART/new-frame signals cannot own it.
    mp_refresh_apply(&s, 9, s.owner_epoch, s.owner_revision);
    mp_refresh_apply(&s, 10, s.owner_epoch - 1, s.owner_revision);
    mp_refresh_apply(&s, 10, s.owner_epoch, s.owner_revision - 1);
    frame(&s);
    complete(&s, src);
    assert(s.phase == MP_REFRESH_REQUESTED && !s.completed.id);
    apply(&s);
    assert(s.phase == MP_REFRESH_APPLIED && s.applied.id == 10);
    mp_refresh_frame_done(&s, 9, s.owner_epoch, s.owner_revision, true);
    assert(!s.frame_done);
    frame(&s);
    src.active = true; // current seek is cleared only after owned restart
    complete(&s, src);
    assert(s.phase == MP_REFRESH_COMPLETED && s.completed.id == 10);
    assert(s.completed.position == 1.25 && src.paused);
    assert(!mp_refresh_admit(&s, ready(), 10, 1.25));
    assert(!mp_refresh_admit(&s, ready(), 9, 1.25));

    assert(mp_refresh_admit(&s, ready(), 11, 1.25));
    apply(&s);
    complete(&s, src); // restart without a completed new frame is not success
    assert(s.phase == MP_REFRESH_CANCELED && s.completed.id == 10);
    assert(s.canceled.id == 11);
    assert(mp_refresh_admit(&s, ready(), 12, 1.25));
    apply(&s);
    frame(&s);
    src.pending = true; // even a completed frame cannot bypass newer work
    complete(&s, src);
    assert(s.phase == MP_REFRESH_CANCELED && s.completed.id == 10);
    assert(s.canceled.id == 12);
}

static void test_supersession_and_reset(void)
{
    for (int phase = 0; phase < 3; phase++) {
        struct mp_paused_refresh s = {0};
        mp_refresh_new_source(&s);
        assert(mp_refresh_admit(&s, ready(), 20, 1.25));
        if (phase > 0)
            apply(&s);
        if (phase > 1) {
            frame(&s);
            complete(&s, ready());
        }
        int64_t revision = s.owner_revision;
        mp_refresh_new_seek(&s); // ordinary, auto-refresh or seek cancel
        assert(s.phase == MP_REFRESH_CANCELED && s.canceled.id == 20);
        assert(s.revision == revision + 1);
        apply(&s);
        frame(&s);
        complete(&s, ready());
        assert(s.phase == MP_REFRESH_CANCELED);
        assert(!mp_refresh_admit(&s, ready(), 20, 1.25));
        assert(mp_refresh_admit(&s, ready(), 21, 1.25));
        mp_refresh_cancel(&s); // reset/reconfiguration
        assert(s.canceled.id == 21 && !s.frame_done);
    }
}

static void test_source_eof_unpause_and_shutdown(void)
{
    for (int reason = 0; reason < 6; reason++) {
        struct mp_paused_refresh s = {0};
        struct mp_refresh_source src = ready();
        mp_refresh_new_source(&s);
        assert(mp_refresh_admit(&s, src, 30, 1.25));
        apply(&s);
        frame(&s);
        int64_t epoch = s.source_epoch;
        switch (reason) {
        case 0: mp_refresh_new_source(&s); break;
        case 1: src.initialized = false; break;
        case 2: src.eof = true; break;
        case 3: src.paused = false; break;
        case 4: src.stopped = true; break; // stop/quit/shutdown
        case 5: src.video = false; break;
        }
        mp_refresh_validate(&s, src);
        mp_refresh_complete(&s, src, 30, epoch, s.owner_revision);
        assert(s.phase == MP_REFRESH_CANCELED && !s.completed.id);
        assert(!mp_refresh_admit(&s, src, 30, 1.25));
        assert(s.requested.position == 1.25);
    }
    struct mp_paused_refresh s = {0};
    mp_refresh_new_source(&s);
    struct mp_refresh_source src = ready();
    src.seekable = false;
    assert(!mp_refresh_admit(&s, src, 1, 1.25));
    src = ready();
    src.pending = true;
    assert(!mp_refresh_admit(&s, src, 1, 1.25));
}

static void test_counter_exhaustion(void)
{
    struct mp_paused_refresh s = {.source_epoch = INT64_MAX};
    mp_refresh_new_source(&s);
    assert(s.exhausted && s.source_epoch == INT64_MAX);
    assert(!mp_refresh_admit(&s, ready(), 1, 1.25));
    s = (struct mp_paused_refresh){.source_epoch = 1, .revision = INT64_MAX};
    assert(!mp_refresh_admit(&s, ready(), 1, 1.25));
    mp_refresh_new_seek(&s);
    assert(s.exhausted && s.revision == INT64_MAX);
}

static void test_backend_processing_drop_timeout_and_failure(void)
{
    for (int failure = 0; failure < 6; failure++) {
        struct mp_paused_refresh s = {0};
        struct mp_frame_receipt backend = {0};
        struct mp_frame_receipt core = {0};
        mp_refresh_new_source(&s);
        assert(mp_refresh_admit(&s, ready(), 1, 1.25));
        apply(&s);
        mp_frame_receipt_begin(&backend, 100);
        mp_frame_receipt_begin(&core, 100);
        switch (failure) {
        case 0: // timeout followed by a late successful render
            mp_frame_receipt_report(&backend, 100, MP_FRAME_FAILED);
            mp_frame_receipt_report(&backend, 100, MP_FRAME_SUCCEEDED);
            break;
        case 1: // exact successful render but the VO reports a drop
            mp_frame_receipt_report(&backend, 100, MP_FRAME_SUCCEEDED);
            break;
        case 2: // unrelated frame completes
            mp_frame_receipt_report(&backend, 99, MP_FRAME_SUCCEEDED);
            break;
        case 3: // render/submit/swap failed
            mp_frame_receipt_report(&backend, 100, MP_FRAME_FAILED);
            break;
        case 4: // reset/teardown after successful processing
            mp_frame_receipt_report(&backend, 100, MP_FRAME_SUCCEEDED);
            mp_frame_receipt_invalidate(&backend);
            break;
        case 5: // dequeued, but still processing / unsupported feedback
            break;
        }
        mp_frame_receipt_finish(&core, 100,
                                mp_frame_receipt_get(&backend, 100), failure == 1);
        mp_refresh_frame_done(&s, 1, s.owner_epoch, s.owner_revision,
                              mp_frame_receipt_get(&core, 100) ==
                                  MP_FRAME_SUCCEEDED);
        complete(&s, ready());
        assert(s.phase == MP_REFRESH_CANCELED && !s.frame_done);
        assert(!s.completed.id && s.canceled.id == 1);
        mp_frame_receipt_report(&core, 100, MP_FRAME_SUCCEEDED);
        assert(mp_frame_receipt_get(&core, 100) == MP_FRAME_FAILED);
    }

    struct mp_frame_receipt s = {0};
    mp_frame_receipt_begin(&s, 100);
    mp_frame_receipt_finish(&s, 100, MP_FRAME_SUCCEEDED, false);
    assert(mp_frame_receipt_get(&s, 100) == MP_FRAME_SUCCEEDED);
    assert(mp_frame_receipt_get(&s, 99) == MP_FRAME_UNKNOWN);
    mp_frame_receipt_begin(&s, 101);
    mp_frame_receipt_finish(&s, 100, MP_FRAME_SUCCEEDED, false);
    assert(mp_frame_receipt_get(&s, 101) == MP_FRAME_PENDING);
    mp_frame_receipt_invalidate(&s);
    assert(mp_frame_receipt_get(&s, 101) == MP_FRAME_FAILED);
}

static void test_automatic_refresh_coalesces_pending_owned_seek(void)
{
    for (int pending = 0; pending < 2; pending++) {
        struct mp_paused_refresh s = {0};
        mp_refresh_new_source(&s);
        assert(mp_refresh_admit(&s, ready(), 1, 1.25));
        if (!pending)
            apply(&s);
        int64_t id = s.requested.id;
        int64_t epoch = s.owner_epoch;
        int64_t revision = s.owner_revision;
        int64_t old_revision = revision;
        // This production seam is shared by pending coalescing, current
        // repetition and ordinary queueing. It cannot change seek parameters.
        mp_refresh_supersede_seek(&s, &id, &epoch, &revision);
        assert(!id && !epoch && !revision);
        assert(s.phase == MP_REFRESH_CANCELED && s.canceled.id == 1);
        assert(s.revision == old_revision + 1);
        assert(s.owner_revision == old_revision);
        mp_refresh_apply(&s, id, epoch, revision);
        mp_refresh_frame_done(&s, 1, s.owner_epoch, old_revision, true);
        complete(&s, ready());
        assert(!s.completed.id && !s.frame_done);
        // Even unrelated ordinary pending work has no inherited owner tag.
        mp_refresh_supersede_seek(&s, &id, &epoch, &revision);
        assert(!id && !epoch && !revision && s.revision == old_revision + 2);
        assert(mp_refresh_admit(&s, ready(), 2, 1.25));
        apply(&s);
        frame(&s);
        complete(&s, ready());
        assert(s.phase == MP_REFRESH_COMPLETED && s.completed.id == 2);
    }
}

int main(void)
{
    test_admission_and_prequeued_auto();
    test_exact_operation_and_actual_completion();
    test_supersession_and_reset();
    test_source_eof_unpause_and_shutdown();
    test_counter_exhaustion();
    test_backend_processing_drop_timeout_and_failure();
    test_automatic_refresh_coalesces_pending_owned_seek();
    return 0;
}
