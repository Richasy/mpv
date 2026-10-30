#!/usr/bin/env python3
"""Run the same anonymous local DASH contracts against both real transports."""
import argparse
import contextlib
import ctypes
import os
from pathlib import Path
import shutil
import tempfile
import time
import libmpv_dash_source as suite

parser = argparse.ArgumentParser()
parser.add_argument('runtime', type=Path)
parser.add_argument('--backend', choices=('curl', 'lavf'), required=True)
parser.add_argument('--quick', action='store_true')
parser.add_argument('--pause-only', action='store_true')
parser.add_argument('--case')
parser.add_argument('--allow-test-hooks', action='store_true')
args = parser.parse_args()
runtime = args.runtime.resolve()
original_client = suite.client
original_snapshot = suite.snapshot

@contextlib.contextmanager
def client(mpv, overrides=None):
    options = {'curl-enabled': 'yes' if args.backend == 'curl' else 'no'}
    options.update(overrides or {})
    with original_client(mpv, options) as handle:
        yield handle

def snapshot(mpv, handle):
    state = original_snapshot(mpv, handle)
    if state.phase == suite.BOUND and state.failure == 0:
        pointer = mpv.mpv_get_property_string(handle, b'rodel-dash-network-backend')
        if not pointer: raise AssertionError('Actual transport evidence is missing')
        try:
            actual = ctypes.string_at(pointer).decode('ascii')
            assert actual == args.backend, (actual, args.backend)
        finally: mpv.mpv_free(pointer)
    return state

suite.client = client
suite.snapshot = snapshot

def frozen_backend(mpv, server):
    server.hold_video_body = True
    server.release_video_body.clear()
    with client(mpv) as handle:
        try:
            source = suite.source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            deadline = time.monotonic() + 8
            while not server.video_body_blocked.is_set() and time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                assert event.event_id != suite.END_FILE
            assert server.video_body_blocked.is_set()
            opposite = 'no' if args.backend == 'curl' else 'yes'
            suite.command(mpv, handle, 'set', 'curl-enabled', opposite)
            assert suite.prop(mpv, handle, 'options/curl-enabled') == opposite
        finally:
            server.release_video_body.set()
        suite.await_event(mpv, handle, suite.FILE_LOADED)
        assert snapshot(mpv, handle).phase == suite.BOUND

def truncated_lavf(mpv, server, role):
    with client(mpv) as handle:
        source = suite.source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        suite.await_event(mpv, handle, suite.FILE_LOADED)
        generation = snapshot(mpv, handle).generation
        server.truncate_range_role = role
        suite.command(mpv, handle, 'seek', '15', 'absolute+exact')
        event = suite.await_event(mpv, handle, suite.END_FILE)
        assert ctypes.cast(event.data,ctypes.POINTER(suite.EndFile)).contents.reason == suite.END_ERROR
        state = snapshot(mpv, handle)
        detail = suite.failure_detail(mpv, handle)
        assert state.generation == generation and state.phase == suite.FAILED
        assert state.failed_track == (suite.VIDEO if role == 'video' else suite.AUDIO)
        assert state.failure == suite.TRANSPORT and detail.origin == suite.ORIGIN_BODY_LENGTH
        count = len(server.requests_for(role))
        time.sleep(0.2)
        assert len(server.requests_for(role)) == count, 'Truncated response was retried'


def framing_failure(mpv, server):
    with client(mpv, {'pause':'no','speed':'20','cache-pause':'no','demuxer-max-bytes':'4MiB'}) as handle:
        source = suite.source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.05).contents
            if event.event_id == suite.END_FILE:
                assert ctypes.cast(event.data,ctypes.POINTER(suite.EndFile)).contents.reason == suite.END_ERROR
                state = snapshot(mpv, handle)
                assert state.phase == suite.FAILED and state.failure == suite.TRANSPORT
                assert state.failed_track == suite.AUDIO
                assert suite.failure_detail(mpv, handle).origin == suite.ORIGIN_BODY_LENGTH
                break
        else: raise AssertionError('Invalid chunk framing was accepted')


def idle_pause(mpv, server, role, resume_stalled=False):
    server.stall_body_after[role] = 100000
    with client(mpv, {'pause':'no', 'network-timeout':'1',
        'demuxer-readahead-secs':'30', 'demuxer-max-bytes':'1MiB',
        'demuxer-lavf-o':'use_mfra_for=0'}) as handle:
        source = suite.source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        loaded = restarted = False
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.02).contents
            assert event.event_id != suite.END_FILE, 'Playback ended before pause'
            loaded |= event.event_id == suite.FILE_LOADED
            restarted |= event.event_id == suite.PLAYBACK_RESTART
            if loaded and restarted and server.stalled_body.is_set(): break
        else: raise AssertionError('Fixture did not reach playback and a stalled read')
        generation = snapshot(mpv, handle).generation
        suite.command(mpv, handle, 'set', 'pause', 'yes')
        start = time.monotonic()
        while time.monotonic() - start < 3:
            event = mpv.mpv_wait_event(handle, 0.02).contents
            assert event.event_id != suite.END_FILE, 'Idle pause exhausted the read budget'
            state = snapshot(mpv, handle)
            assert state.phase == suite.BOUND and state.failure == 0
        assert suite.prop(mpv, handle, 'pause') == 'yes'
        if not resume_stalled: server.release_stalled_body.set()
        suite.command(mpv, handle, 'set', 'pause', 'no')
        resumed = time.monotonic()
        if resume_stalled:
            event = suite.await_event(mpv, handle, suite.END_FILE, 5)
            assert ctypes.cast(event.data,ctypes.POINTER(suite.EndFile)).contents.reason == suite.END_ERROR
            assert snapshot(mpv, handle).failure == suite.TRANSPORT
            assert time.monotonic() - resumed < 3, 'Resume did not restore a bounded budget'
        else:
            while time.monotonic() - resumed < 4:
                event = mpv.mpv_wait_event(handle, 0.02).contents
                assert event.event_id != suite.END_FILE
                position = suite.prop(mpv, handle, 'time-pos')
                if position is not None and float(position) >= 1: break
            else: raise AssertionError('Playback did not advance after a long pause')
            assert snapshot(mpv, handle).generation == generation
            suite.command(mpv, handle, 'set', 'pause', 'yes')
            suite.confirm_position(mpv, handle, 8, generation,
                lambda:suite.command(mpv, handle, 'seek', '8', 'absolute+exact'))
            paused = time.monotonic()
            while time.monotonic() - paused < 3:
                assert mpv.mpv_wait_event(handle, 0.02).contents.event_id != suite.END_FILE
                assert snapshot(mpv, handle).failure == 0
            suite.command(mpv, handle, 'stop')
            suite.await_event(mpv, handle, suite.END_FILE)


def idle_pause_http(mpv, server, role, code):
    server.short_initial_roles.add(role)
    server.short_initial_bytes = 131072
    server.hold_followup_role = role
    server.hold_followup_start_min = 190000 if role == 'video' else 130000
    server.hold_followup_before_midpoint = True
    server.range_max_bytes = 65536
    server.release_followup_headers.clear()
    with client(mpv, {'pause':'no','network-timeout':'1',
        'cache-pause':'no',
        'demuxer-readahead-secs':'30','demuxer-max-bytes':'1MiB',
        'demuxer-lavf-o':'use_mfra_for=0'}) as handle:
        source = suite.source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        loaded = restarted = False
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.02).contents
            assert event.event_id != suite.END_FILE, (role, code, loaded, restarted,
                snapshot(mpv, handle).phase, snapshot(mpv, handle).failure,
                [(r[1],r[2]) for r in server.requests_for(role)])
            loaded |= event.event_id == suite.FILE_LOADED
            restarted |= event.event_id == suite.PLAYBACK_RESTART
            if loaded and restarted and server.followup_requested.is_set(): break
        else: raise AssertionError('No continuation pending after playback restart')
        suite.command(mpv, handle, 'set', 'pause', 'yes')
        # An already admitted request must still observe real terminal HTTP.
        server.deny,server.deny_code = role,code
        server.release_followup_headers.set()
        suite.await_event(mpv, handle, suite.END_FILE, 5)
        state = snapshot(mpv, handle)
        assert state.failure == (suite.RISK if code == 412 else suite.AUTH)
with os.add_dll_directory(str(runtime)):
    mpv = ctypes.CDLL(str(runtime/'libmpv-2.dll'))
    suite.configure_library(mpv)
    if hasattr(mpv, 'mpv_dash_test_control') and not args.allow_test_hooks:
        raise AssertionError('Production DLL exposes test-only hooks')
    directory = Path(tempfile.mkdtemp(prefix='dash-backends-', dir=runtime))
    try:
        tracks = suite.generate_tracks(directory)
        def run(name, callback, setup=None):
            if args.case and args.case not in name: return
            print(f'[{args.backend}] {name}', flush=True)
            with suite.serve(tracks) as server:
                if setup: setup(server)
                callback(mpv, server)
        for role in ('video','audio'):
            run('idle-pause-resume-'+role, lambda mpv,s,role=role:idle_pause(mpv,s,role))
            run('idle-pause-stall-bounded-'+role, lambda mpv,s,role=role:idle_pause(mpv,s,role,True))
            for code in (401,403,412):
                run(f'idle-pause-terminal-{role}-{code}',
                    lambda mpv,s,role=role,code=code:idle_pause_http(mpv,s,role,code))
        if args.pause_only:
            print('DASH_PAUSE_PASS', args.backend, flush=True)
            raise SystemExit(0)
        run('dual-track-and-seek', suite.test_playback)
        run('frozen-backend-after-option-change', frozen_backend)
        for role in ('audio','video'):
            run('short-206-'+role, lambda mpv,s,role=role: suite.test_short_first_range(mpv,s,role))
            run('full-200-'+role, lambda mpv,s,role=role: suite.test_full_body_playback(mpv,s,role))
        run('unknown-length-200', lambda mpv,s: suite.test_full_body_playback(mpv,s,'audio'),
            lambda s: s.full_body_lengths.update(audio=None))
        if not args.quick:
            run('controls', suite.test_control_lifecycle)
            run('seek-capability', suite.test_observed_seek_capability)
            for role,code,failure in (('video',401,suite.AUTH),('audio',403,suite.AUTH),
                                      ('audio',412,suite.RISK),('video',412,suite.RISK)):
                run('status-'+role+'-'+str(code),
                    lambda mpv,s,role=role,failure=failure: suite.test_failure(mpv,s,role,failure),
                    lambda s,role=role,code=code: (setattr(s,'deny',role),setattr(s,'deny_code',code)))
            run('no-redirect', lambda mpv,s: suite.test_failure(mpv,s,'video',suite.HTTP_STATUS,
                expected_origin=suite.ORIGIN_REDIRECT), lambda s: setattr(s,'redirect','video'))
            run('bad-range', lambda mpv,s: suite.test_failure(mpv,s,'video',suite.HTTP_RANGE,
                expected_origin=suite.ORIGIN_PARTIAL),lambda s:setattr(s,'bad_range','video'))
            run('early-hints',suite.test_playback,lambda s:s.early_hints_roles.update(('video','audio')))
            run('interim-fields-cannot-configure-final',suite.test_playback,
                lambda s:(s.early_hints_roles.update(('video','audio')),
                          s.interim_pollution_roles.update(('video','audio'))))
            run('valid-chunked-206',suite.test_playback,lambda s:s.chunked_roles.update(('video','audio')))
            if args.backend == 'lavf':
                run('unknown-chunked-missing-terminator',framing_failure,
                    lambda s:(s.full_body_roles.add('audio'),s.full_body_lengths.update(audio=None),
                        s.chunked_roles.add('audio'),s.chunk_endings.update(audio=b'')))
                for field in ('chunk_extra_roles','chunk_oversized_roles'):
                    run('full-range-'+field,framing_failure,
                        lambda s,field=field:(s.chunked_roles.add('audio'),getattr(s,field).add('audio')))
                for ending in (b'garbage\r\n\r\n',b'0x0\r\n\r\n',b'+0\r\n\r\n',
                               b'-0\r\n\r\n',b'10000000000000000\r\n\r\n',
                               b'0;bad=\r\n\r\n',b'0\r\nContent-Length: 0\r\n\r\n',b'0\r\n',b''):
                    run('invalid-chunk-framing-'+repr(ending),
                        framing_failure,
                        lambda s,ending=ending:(s.chunked_roles.add('audio'),
                            s.short_initial_roles.add('audio'),s.chunk_endings.update(audio=ending)))
                run('valid-chunk-extensions',suite.test_playback,
                    lambda s:(s.chunked_roles.add('audio'),s.chunk_endings.update(audio=b'0;checksum="valid\\\"text"\r\nX-Checksum: ok\r\n\r\n')))
            run('risk-before-stop',suite.test_risk_latched_before_stop)
            run('stop-before-risk',suite.test_stop_before_late_risk)
            run('queued-stop',suite.test_stop_queued)
            for role in ('audio','video'):
                run('stop-open-'+role,lambda mpv,s,role=role:suite.test_stop_while_opening(mpv,s,role))
                callback = suite.test_truncated_seek_response if args.backend == 'curl' else truncated_lavf
                run('truncate-seek-'+role,lambda mpv,s,role=role,callback=callback:callback(mpv,s,role))
            for code in (404,500,416):
                run('ordinary-media-'+str(code),lambda mpv,s,code=code:suite.test_full_body_playback(mpv,s,'audio',http_status=code))
        print('DASH_BACKEND_PASS',args.backend,flush=True)
    finally:
        shutil.rmtree(directory)
