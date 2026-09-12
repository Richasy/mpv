Experimental native Feature18 NR
===============================

This is an original C implementation of the community experimental NGX
Feature18 path. It is not the official RTX Video Super Resolution filter.
No reference implementation, shader, plugin, NVIDIA static SDK library, or
model binary is included.

The filter is opt-in: ``@dlss5:dlssnr``. Its ``enabled`` option defaults to yes
when the filter is present; applications should leave the filter absent by
default.

Runtime boundary
----------------

The user supplies ``nvngx_dlssnr.dll`` privately. The default location is
``ngx\nvngx_dlssnr.dll`` beside the module containing libmpv, not the working
directory. An explicit relative ``model-path``, including
``ngx\nvngx_dlssnr.dll``, is also resolved against that module directory, never
the working directory. Absolute drive/UNC overrides remain supported;
ambiguous drive-relative and single-rooted paths are rejected.

NGX storage is separate from the model and installation directory.
``cache-path`` accepts an absolute writable directory; empty or omitted uses
``mpv\cache\dlssnr`` under the Windows LocalAppData known folder, even with
``--no-config``. Embedded applications should explicitly select storage owned by
their active application/package identity. Creation and write-probe failures
include a Windows error code and never fall back to the working directory or
installation. Existing cache contents are retained, and changing ``cache-path``
reloads the runtime. NGX's application data path requires filesystem write
access; GPU scratch buffers are not a memory-only substitute for this directory.
No model is downloaded by this implementation. NGX itself can perform its
normal driver-cache maintenance and telemetry.

The runtime holds ``mpv-dlssnr-cache.lock`` in this directory with read access
and ``FILE_SHARE_READ`` until actual NGX shutdown succeeds. Frontends may hold
additional shared-read leases. Cleanup must obtain an exclusive read/write
handle to this same file and preserve it while deleting cache contents.
Do not treat filter removal or an asynchronous GPU retirement request as proof
that the cache is unused. Fault-retained native runtimes also retain the lease
until process exit. A frontend can use separate per-session directories and
reclaim only inactive sessions without interfering with another player.

The installed NGX core provides parameter factories. Original C declarations
describe the public parameter interface's Microsoft x64 vtable. Neither C++
linkage nor NVIDIA SDK headers/static libraries are needed. The raw driver's
``Init_ProjectID`` takes the API version before common-info; raw ``Shutdown1``
also requires a remaining-device-count output pointer. These differ from the
SDK wrappers.

Feature18 creation/evaluation calls the explicitly loaded model's exports.
There is no requirement that the installed core's public
``GetFeatureRequirements`` or Feature18 dispatcher report support.

The user's selected public reference archive has 130832171 bytes and SHA-256
``4e4691b11b5b0d83f2458c280e24aa33cb8ba7f4a449c71d2587d4fa8f8f34de``.
Matching its release digest establishes reference-archive identity, not NVIDIA
signature validity or vendor authorization.

The privately supplied reference artifact inspected during development has:

* File version: ``310.8.0.0``.
* File size: 165840496 bytes.
* SHA-256: ``8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206``.
* Embedded NVIDIA signature status: **HashMismatch**, not Valid.

It is emphatically not a trusted, unmodified vendor binary. Its use is confined
to the user's explicit private experimental route; neither it nor its reference
archive belongs in the repository, release packages, or CI artifacts.

That exact artifact checks whether the caller-module filename contains
``nvngx.dll`` before entering five used D3D12 exports. A mismatch returns
``PlatformError`` and logs ``Not called from NGX runtime``. This particular
guard does not inspect a license key, entitlement, certificate, or project ID.

The original LGPL helper ``mpv-nvngx.dll`` contains only NGX model-call
forwarders. Its genuine filename contains the expected substring, so ordinary
``GetModuleFileNameW`` satisfies this calling convention. Compiler no-tail-call
attributes and a volatile post-call result preserve a real helper call frame
for Init/Create/Evaluate/Release/Shutdown. No provenance API, import table,
driver, application identity, model selection result, or file on disk is
modified. There are no model-specific guard addresses.

The helper exposes a versioned C ABI through ``mpv_ngx_bridge_get_api``. It is
loaded only from beside libmpv and is retained until all model calls finish.
The known reference artifact's signature mismatch is identified read-only,
logged, and exposed separately in metadata; its digest is not an execution gate.
Unknown models retain their ordinary checks and receive the same genuine
mpv bridge caller.

Only one native NR runtime instance per process is supported. A model already
loaded by another component is rejected to preserve isolation. A native SDK
exception latches the runtime unavailable until process restart; potentially
live code/resources are retained rather than unsafely freed.

GPU pipeline and ownership
--------------------------

* Uses the incoming D3D11 decoder device and creates D3D12 on the same adapter.
  The adapter LUID is checked; there is no migration to another GPU.
* Copies one decoder array slice to shared GPU-local staging. A D3D11 signal
  and D3D12 queue wait establish the producer dependency.
* Converts NV12/P010 matrix, range and chroma siting on GPU to original-size
  RGBAF16. SDR BT.601, BT.709 and SMPTE 240M matrices are supported.
* Uses original separable Lanczos2 reduction and the model's BGRA8 input/output.
  The low-resolution model difference is retained in FP16 and reconstructed
  with an original Catmull-Rom implementation.
* Composes ``original + controlled model difference`` at original size. It
  does not substitute an upscaled low-resolution model frame. This preserves
  source detail and P010 precision even though model input/output are eight-bit.
* Copies the shared F16 result to an ordinary D3D11 F16 texture for gpu-next.
  Bounded output slots are tied to image buffer references. Recycling
  requires both release of downstream references and completion of a D3D11
  consumer fence placed after their queued reads. This fence is separate from
  the inference handoff fence. Consumer values are assigned under immediate-
  context serialization in actual submission order, never preassigned by
  frame/slot index, so out-of-order reference release cannot satisfy an older
  slot's unfinished reads.
* Retired output pools can outlive filter destruction. GPU waits and module
  pins protect asynchronous retirement. Resources with unproven completion
  are retained on exceptional failure.
  D3D11 is unloaded synchronously only after its COM objects are released;
  the callback's sole deferred DLL-unload registration is reserved for libmpv.

Resource backpressure
---------------------

Four slots are insufficient for the real player: ``vo_gpu_next.c`` requests at
least two future frames and declares retained past/future frames separately.
``vo.h`` bounds requested frames at ten and retained frames at twenty.
``vd_lavc.c`` already sizes fixed decoder pools with a six-surface baseline and
three additional in-flight surfaces above VO retention.

The NR pool follows that contract: six initial output textures, lazy growth
when additional references are retained, and a hard maximum of twenty-three
textures (twenty retained plus three in flight). A compile-time assertion
checks this bound against ``VO_MAX_REQ_FRAMES``. Released textures whose GPU
reads are still pending are waited on instead of causing growth.

At the bound, the worker retains the same input frame and waits on consumer
reference-release/fence-completion events. This is normal backpressure, not a
runtime failure or an unrequested passthrough. Seek/reset, disable and destruction
signal a cancellation event before waiting for the worker; queued GPU work still
uses the existing completion/retirement rules. The filter requests at most one
worker job for outstanding output demand. ``backpressure-waits``,
``output-slots``, ``output-slot-capacity`` and ``last-slot-wait-ms`` expose the
resulting resource behavior. Slot-wait time is not included in inference timing.

``zero-copy=yes`` means **no CPU pixel upload/readback inside this hardware
filter**. It does not mean zero GPU copies. All model creation, allocation and
processing run on a filter worker, not the playback/UI thread. That worker
waits for actual GPU completion. ``last-infer-ms`` is wall time from pipeline
submission/recording through GPU completion, including conversion, inference,
composition and output copy; it is not a GPU timestamp or isolated NN duration.

PTS, duration, SAR, crop, rotation, ICC and other side data are copied through.
Only the representation fields needed to describe RGB output are replaced.
PQ/HLG/Dolby Vision sources bypass the filter. BT.2020 SDR, software frames and
transparent RGB are currently unsupported and pass through unchanged.
NR receives video frames, not VO subtitles/OSD.

Controls and status
-------------------

The complete option table is in ``vf_dlssnr.c``; defaults and validation are in
``params.c``. Runtime changes are finite, bounded, transactional snapshots::

    vf-command dlss5 intensity 0.65
    vf-command dlss5 style 2

The TEXT command is the option name and its single argument is the value.
For example, libmpv receives ``["vf-command", "dlss5", "intensity", "0.65"]``.
The optional fourth argument after ``vf-command`` is a filter target, never
another option value. There is no ``set`` subcommand. An empty ``model-path``
argument restores module-adjacent model resolution.

Style and strength changes do not recreate the model or feature. Preset
changes recreate only the feature. Internal-resolution changes rebuild the
inference resources/feature while retaining original-resolution output state.
Changes apply on the next video frame; paused output is not re-evaluated.
Requested/applied generations distinguish pending changes from completed work.

Paused refresh and A/B capture
-----------------------------

The frontend must explicitly obtain a new decoded frame after changing
controls while paused, especially after preset, model, scaling, or resolution
transitions. Preserve the pause state, save ``time-pos``, send the direct
``vf-command``, then seek to that saved position with ``absolute+exact``.
For example, the libmpv command arguments are::

    ["vf-command", "dlss5", "preset", "1"]
    ["seek", "<saved time-pos, invariant round-trip precision>", "absolute+exact"]

If the frontend serializes commands behind a gate, the exact seek must be
issued inside the apply transaction **before** waiting for native confirmation.
Queuing a later seek behind that confirmation wait cannot refresh a paused/EOS
decoder and will time out. Alternatively, start playback before applying.

A redraw request alone only repaints the previously filtered texture. Do not
take the "after" A/B capture just because the mutation command succeeded.
Wait for seek completion/new-frame presentation and, for NR-on, an increased
``processed-frames`` count, matching requested/applied generations and
``active=yes``. For NR-off, wait for a newly presented passthrough frame instead
of waiting for an applied NR generation. Keep both captures at the same source
position; do not use frame-step to simulate a same-frame comparison.
If NR-off removes the filter, its metadata no longer exists: verify removal
and a newly presented decoded frame rather than waiting for ``dlss5``
passthrough metadata.

Residual controls
-----------------

Residual saturation/lightness controls operate on the model-induced change,
not absolute source color. Shadow/glow controls shape signed luminance changes
in dark/bright regions. Neutral controls reproduce the original model
difference. These original algorithms are not claimed to be bit-identical to
the reference implementation.

NVOF is **not implemented**. ``motion-quality`` values above zero and
``nvof-follow-scaling=yes`` are rejected transactionally, not silently ignored.
The default path supplies real GPU textures containing zero motion/depth
guidance and reports ``disabled-zero-guidance``.

``vf-metadata/dlss5`` contains string values for requested state, active state,
status, configured controls, frame counters, processing dimensions, adapter,
timing and errors. ``active=yes`` requires successful real evaluation, completed
GPU work and matching applied/requested settings. Runtime failures remain
visible and video passes through instead of failing the host.
All option values are emitted even before processing starts, including defaults:
the nineteen non-path controls plus ``model-path`` and ``cache-path``.
Worker completion increments ``evaluated-frames`` only. A queued result retains
its settings, GPU result and seek epoch; delivered counters, applied generation
and new active proof are committed only after the output pin accepts the frame.
Reset-discarded, rejected and failed results cannot acknowledge a fresh setting.

Build integration
-----------------

Production sources:

* ``video\filter\vf_dlssnr.c``
* ``video\filter\dlssnr\params.c``
* ``video\filter\dlssnr\cache.c``
* ``video\filter\dlssnr\shaders.c``
* ``video\filter\dlssnr\gpu.c``
* ``video\filter\dlssnr\runtime.c``
* ``video\filter\dlssnr\model_identity.c``

Separately build ``video\filter\dlssnr\bridge.c`` as ``mpv-nvngx.dll``, with no
additional ``lib`` filename prefix, and install/package it beside libmpv.
This helper is original LGPL code and is part of the application distribution,
unlike the user-supplied model. Its sole export is ``mpv_ngx_bridge_get_api``;
``bridge.h`` defines ABI version 1, the version/size fields, and the five C
forwarder signatures.

Requires Win64, D3D11 hardware decoding, current Windows D3D11.4/D3D12 headers,
and Clang with ``-fms-extensions`` for SEH. MSVC additionally requires C atomics
support. Do not link the resulting image with ``--no-seh``. Windows DLLs are
loaded dynamically; no NVIDIA import/static library is linked.

Focused host tests
------------------

``test_params.c`` tests option bounds, NaN/infinity rejection, processing
dimensions and NV12/P010 color matrices. Link it with ``params.c``.

``test_paths.c`` verifies omitted, explicit-relative and absolute model paths,
including that module-relative resolution is independent of the working
directory. Link it with ``runtime.c``, ``cache.c``, ``model_identity.c`` and
``params.c``, plus the ordinary Windows ``shell32``, ``ole32`` and ``uuid``
libraries used by the cache helper.

``test_cache.c`` checks the per-user default, absolute Unicode overrides,
working-directory independence, nested creation, retained contents, write-probe
cleanup, shared/exclusive leases and explicit creation/write failures.
It includes ``cache.c`` to inject
denied writes without changing filesystem permissions. Link it with ``params.c``
and the same three Windows libraries. It does not initialize a GPU or NGX and
creates only an owned temporary fixture tree.

``test_runtime_cache.c`` includes ``runtime.c`` and proves that fault-retained
native state keeps its cache lease until successful teardown, without loading
a GPU or vendor runtime. Link it with ``cache.c``, ``model_identity.c`` and
``params.c`` plus the same Windows libraries.

``test_bridge.c`` checks ABI version negotiation, argument forwarding, and the
genuine caller-module identity at all five optimized call boundaries, including
exception traversal back to the host's SEH guard. Place the original helper DLL
beside this test executable.

``test_shaders.c`` compiles all five original HLSL entrypoints with strict
diagnostics. Link it with ``shaders.c``.

``test_ngx_binding.c`` independently tests the installed core's original C
binding. Its optional ``--create18`` argument tests direct core dispatch;
it never calls ``GetFeatureRequirements``.

``test_gpu_failures.c`` injects failure of GPU-context allocation and checks that
repeated calls preserve an already-failed context's actionable diagnostic, plus
early cancellation/ownership release. It
includes ``gpu.c`` itself; link the other backend sources, not a second copy of
``gpu.c``. It does not initialize a GPU or vendor runtime.

``test_delivery.c`` tests the filter's actual staging and delivery-accounting
functions with pending, rejected, stale-epoch, stale-setting, failed and accepted
results. Build with the configured mpv headers, optimization, function/data
sections, ``-Wno-unused-function`` for this standalone translation unit, and
dead-section elimination, linking ``params.c``. Its
``DLSSNR_DELIVERY_TEST`` guard omits registration only for this standalone test.

``test_gpu.c`` links the six backend sources (not ``vf_dlssnr.c`` or ``bridge.c``)
and loads the separately built helper beside the executable. It takes
one absolute private model path. It performs real NR evaluation, checks changed,
finite F16 output, verifies live changes do not reload the model, checks
NV12/P010 decoder-array slice handling at quarter resolution, and verifies
bounded output ownership across later frames and engine destruction.
It also releases slots out of order behind a deliberately blocked GPU consumer
queue, verifies cancellable starvation and same-frame resumption without error,
and verifies reuse only after the consumer fence completes. Sustained tests
retain five and twenty-two downstream frames while producing additional frames,
covering both ordinary and maximum declared retention instead of treating a
failed fifth allocation as sufficient proof.
CPU upload/readback is confined to this diagnostic, not production code.
Its optional ``--nv12-1080p`` argument instead runs only a cold-context
1920x1080 BT.709 limited NV12 acceptance case with a padded two-slice decoder-like
texture and the default NR controls. This is a synthetic format-matching fixture,
not a claim that the user's original clip was decoded by this diagnostic.

For DLL-host retirement proof, compile ``test_gpu.c`` with ``DLSSNR_TEST_DLL``
and the backend sources into ``dlssnr-test-host.dll``. ``test_dll_host.c`` loads
that DLL, runs the real GPU ownership regressions with non-null module pins,
then verifies final module unloading. Keep ``mpv-nvngx.dll`` beside the test.

Compile with Clang C17 and Windows SDK headers. Keep test executables, logs and
caches under an ignored project build directory. Never package the supplied
model, SDK libraries, private cache, or reference archive into CI artifacts.
