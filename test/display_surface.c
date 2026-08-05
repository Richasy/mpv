#include <stdatomic.h>

#include "mpv_talloc.h"
#include "osdep/threads.h"
#include "test_utils.h"
#include "video/out/display_surface.h"

struct race_context;

struct fake_surface {
    atomic_int refs;
    struct race_context *race;
};

struct race_context {
    struct vo_display_surface_state *state;
    struct fake_surface *surface;
    struct vo_display_surface_snapshot acquired;
    mp_mutex lock;
    mp_cond cond;
    bool retain_entered;
    bool allow_retain;
    bool publish_started;
    bool publish_done;
};

static void retain_surface(void *ptr)
{
    struct fake_surface *surface = ptr;
    int previous = atomic_fetch_add(&surface->refs, 1);
    assert_true(previous > 0);
}

static void retain_surface_blocked(void *ptr)
{
    struct fake_surface *surface = ptr;
    struct race_context *race = surface->race;

    mp_mutex_lock(&race->lock);
    race->retain_entered = true;
    mp_cond_broadcast(&race->cond);
    while (!race->allow_retain)
        mp_cond_wait(&race->cond, &race->lock);
    mp_mutex_unlock(&race->lock);

    int previous = atomic_fetch_add(&surface->refs, 1);
    assert_true(previous > 0);
}

static MP_THREAD_VOID acquire_surface(void *ptr)
{
    struct race_context *race = ptr;
    race->acquired = vo_display_surface_acquire(race->state);
    MP_THREAD_RETURN();
}

static MP_THREAD_VOID publish_surface_loss(void *ptr)
{
    struct race_context *race = ptr;

    mp_mutex_lock(&race->lock);
    race->publish_started = true;
    mp_cond_broadcast(&race->cond);
    mp_mutex_unlock(&race->lock);

    vo_display_surface_publish(race->state, NULL, NULL);
    atomic_fetch_sub(&race->surface->refs, 1);

    mp_mutex_lock(&race->lock);
    race->publish_done = true;
    mp_cond_broadcast(&race->cond);
    mp_mutex_unlock(&race->lock);
    MP_THREAD_RETURN();
}

static void test_epoch_and_replacement(void)
{
    void *root = talloc_new(NULL);
    struct vo_display_surface_state *state =
        vo_display_surface_state_create(root);
    struct fake_surface first = {.refs = 1};
    struct fake_surface second = {.refs = 1};

    struct vo_display_surface_snapshot snapshot =
        vo_display_surface_acquire(state);
    assert_true(!snapshot.surface);
    assert_int_equal(snapshot.epoch, 0);

    assert_true(vo_display_surface_publish(state, &first, retain_surface));
    assert_false(vo_display_surface_publish(state, &first, retain_surface));
    snapshot = vo_display_surface_acquire(state);
    assert_true(snapshot.surface == &first);
    assert_int_equal(snapshot.epoch, 1);
    assert_int_equal(atomic_load(&first.refs), 2);

    assert_true(vo_display_surface_publish(state, &second, retain_surface));
    atomic_fetch_sub(&first.refs, 1);
    assert_int_equal(atomic_load(&first.refs), 1);
    snapshot = vo_display_surface_acquire(state);
    assert_true(snapshot.surface == &second);
    assert_int_equal(snapshot.epoch, 2);
    assert_int_equal(atomic_load(&second.refs), 2);

    assert_true(vo_display_surface_publish(state, NULL, NULL));
    atomic_fetch_sub(&second.refs, 1);
    assert_int_equal(atomic_load(&second.refs), 1);
    snapshot = vo_display_surface_acquire(state);
    assert_true(!snapshot.surface);
    assert_int_equal(snapshot.epoch, 3);

    talloc_free(root);
    atomic_fetch_sub(&first.refs, 1);
    atomic_fetch_sub(&second.refs, 1);
    assert_int_equal(atomic_load(&first.refs), 0);
    assert_int_equal(atomic_load(&second.refs), 0);
}

static void test_acquire_is_atomic_with_loss(void)
{
    void *root = talloc_new(NULL);
    struct vo_display_surface_state *state =
        vo_display_surface_state_create(root);
    struct fake_surface surface = {.refs = 1};
    struct race_context race = {
        .state = state,
        .surface = &surface,
    };
    surface.race = &race;
    mp_mutex_init(&race.lock);
    mp_cond_init(&race.cond);
    vo_display_surface_publish(state, &surface, retain_surface_blocked);

    mp_thread acquire_thread;
    assert_int_equal(mp_thread_create(&acquire_thread, acquire_surface, &race), 0);

    mp_mutex_lock(&race.lock);
    while (!race.retain_entered)
        mp_cond_wait(&race.cond, &race.lock);
    mp_mutex_unlock(&race.lock);

    mp_thread publish_thread;
    assert_int_equal(mp_thread_create(&publish_thread, publish_surface_loss, &race), 0);

    mp_mutex_lock(&race.lock);
    while (!race.publish_started)
        mp_cond_wait(&race.cond, &race.lock);
    assert_false(race.publish_done);
    race.allow_retain = true;
    mp_cond_broadcast(&race.cond);
    mp_mutex_unlock(&race.lock);

    assert_int_equal(mp_thread_join(acquire_thread), 0);
    assert_int_equal(mp_thread_join(publish_thread), 0);
    assert_true(race.acquired.surface == &surface);
    assert_int_equal(race.acquired.epoch, 1);
    assert_true(race.publish_done);
    assert_int_equal(atomic_load(&surface.refs), 1);

    struct vo_display_surface_snapshot current =
        vo_display_surface_acquire(state);
    assert_true(!current.surface);
    assert_int_equal(current.epoch, 2);

    talloc_free(root);
    atomic_fetch_sub(&surface.refs, 1);
    assert_int_equal(atomic_load(&surface.refs), 0);
    mp_cond_destroy(&race.cond);
    mp_mutex_destroy(&race.lock);
}

int main(void)
{
    test_epoch_and_replacement();
    test_acquire_is_atomic_with_loss();
    return 0;
}
