ISO and BDMV streaming investigation
====================================

Scope and conclusion
--------------------

Investigation date: 2026-10-30. Implementation base:
``4adb56e5dedcc74bbba2edfcb6ee77a9cc86d6d3``.

An ISO is a random-access filesystem image, not a linear media container.
Efficient online playback requires reading its directory and navigation metadata
without downloading the image, then mapping playlist time to the correct clip
and byte range. Increasing a sequential download buffer alone does not achieve
this. BDMV has the same navigation problem, but its files need separate URLs
and directory enumeration.

Keep libudfread/libbluray for filesystem interpretation, playlist selection,
CLPI time-to-packet indexes, clip cuts, angles, chapters and track metadata.
Adapt their I/O to mpv's existing transport and sparse cache. The resulting
paths support local ISO, HTTP(S) Blu-ray ISO, local BDMV entries, and remotely
mapped BDMV. Source-provider enumeration remains at the host boundary.

Community evidence and applicability
------------------------------------

* `libbluray's public API
  <https://github.com/ShiftMediaProject/libbluray/blob/master/src/libbluray/bluray.h>`__
  provides ``bd_open_stream`` for image block reads and ``bd_open_files`` for
  instance-owned directory/file callbacks. Its filesystem interface explicitly
  accommodates network filesystems. A new disc parser or global filesystem
  hook is unnecessary.

* `Kodi's Blu-ray input implementation
  <https://github.com/xbmc/xbmc/blob/master/xbmc/cores/VideoPlayer/DVDInputStreams/DVDInputStreamBluray.cpp>`__
  uses block mode for images and file callbacks for virtual filesystems.
  `Its UDF context
  <https://github.com/xbmc/xbmc/blob/master/xbmc/filesystem/UDFContext.cpp>`__
  retains a mount while readers share it. This demonstrates the benefit of
  preserving filesystem state rather than reopening an ISO for every inner
  file; it is not a universal startup-latency benchmark. mpv's existing
  ``bd_open_stream`` already keeps one libbluray/UDF context per playback.

* `libudfread's block interface
  <https://github.com/firecore/libudfread/blob/master/src/blockinput.h>`__
  reads 2048-byte blocks through caller-owned input. Image size does not imply
  an obligation to download all image bytes. Working random byte access is
  the prerequisite.

* `faststrm's routing documentation
  <https://github.com/wabisabi926/faststrm/wiki/STRM-%E8%B7%AF%E7%94%B1%E7%AD%96%E7%95%A5>`__
  describes proxying original-disc traffic to handle CDN Range, User-Agent and
  signature restrictions. This is a server-side compatibility solution. It
  does not replace UDF/MPLS/CLPI handling in the player, and a proxy can still
  be useful when a provider does not expose a usable direct byte-range URL.

* `HTTP range semantics
  <https://www.rfc-editor.org/rfc/rfc9110.html#section-14>`__
  distinguish partial responses from a server ignoring Range. Playback must
  validate the returned range rather than interpret a full response starting
  at zero as bytes from a later image offset. The existing curl transport
  supplies that validation and bounded recovery.

An upstream `mpv issue from 2023/2024
<https://github.com/mpv-player/mpv/issues/13195>`__ documents that ordinary
upstream builds did not support online ISO at that time. This fork already had
an implementation; the work here improves that implementation rather than
assuming an upstream option enables remote disc access.

Local findings and changes
--------------------------

The old ISO adapter synchronously filled at least 1 MiB on every cache miss,
kept 64 FIFO chunks, and only checked whether one chunk covered the entire
request. That duplicated the stream LRU, delayed small metadata reads and
could re-fetch overlapping ranges. ISO opens also forced the FFmpeg backend,
bypassing the fork's normal curl network policy. Generic tail-prefetch and
read-through policies could add unrelated synchronous I/O.

The adapter now reads demanded bytes through the existing sparse stream LRU.
HTTP disc files select curl when compiled in, otherwise FFmpeg. They retain
normal headers, cookies, proxy settings, network timeouts, recovery and
cancellation. Disc opens disable synchronous tail prefetch and cap cursor
read-through at 64 KiB; ordinary media streams retain their configured policy.
Disc curl receive buffers, including additional region cursors, are capped at
256 KiB. An intermediate A/B exposed increased response read-ahead with curl's
ordinary 4-MiB buffer; the smaller disc buffer addresses that measured cost.
The outer disc stream is correctly marked as network-backed, so normal
automatic demuxer caching can operate. Invalid range transports fail the
Blu-ray probe rather than being scanned as an ordinary transport stream.

Title duration and playlist IDs are retained from the initial title-info pass.
Building the edition list no longer reopens title information twice per title.
The required initial title scan remains: guessing the main feature from the
largest M2TS would break multi-clip titles, branching and clip in/out points.

Local ``index.bdmv`` and five-digit MPLS entries now resolve to the disc root;
the requested MPLS remains a playlist, not a media-file shortcut. Root handling
preserves filesystem and Windows drive roots.

For remote BDMV, ``bluray-files`` supplies an immutable per-session map from
disc-relative paths to local files or HTTP(S) URLs. libbluray enumerates virtual
directories from the keys and opens each mapped file through mpv. Keys reject
absolute paths, traversal and invalid components. Missing entries are not
guessed. A libmpv node map preserves signed URLs containing commas and equals
signs. No new public ABI, application proxy or filesystem driver is required.

Validation and reproducibility
-------------------------------

The native probes run on Windows with ``vo=null``, ``ao=null``, software decode,
and private handles. They create no window and do not use a physical audio
device. These are Windows Host results, not VM or packaged application proof.

``test/disc_io.c`` checks demanded-read size, 80-GiB addressing, EOF versus
error, cancellation, seek overflow, directory deduplication, immutable file
maps, missing paths, invalid components and disabled reference access.
The existing disc-track-switch fixture now initializes the DASH state required
by the current player seek path; its previous null global caused a crash when
linked with curl enabled.

``test/libmpv_disc_streaming.py`` authors synthetic ISO/BDMV data with ffmpeg
and tsMuxeR. Its 15 cases include redirects with required headers, a typed URL
map with signed-query punctuation, explicit and automatic rejection of missing
or wrong ranges, canceled opens, local index/MPLS entry points, and a two-clip
playlist with different nonzero in/out cuts. The latter is 36.083 seconds with
four chapters; seek targets cross the clip boundary and return to it.

The existing seven-case disc track suite covers audio/subtitle changes,
paused selection, and MKV/MP4 controls. Existing curl keepalive, retry,
range-retry, per-file isolation, cancel and concurrent-client checks also pass.

To run the synthetic suite, keep the built runtime's DLL dependencies together::

    python -B test/libmpv_disc_streaming.py <libmpv-library> \
        --ffmpeg <ffmpeg> --tsmuxer <tsMuxeR>

To compare an existing read-only image::

    python -B test/libmpv_disc_streaming.py <libmpv-library> \
        --iso <image.iso> --baseline

``--baseline`` selects only the common local/HTTP/redirect cases, allowing the
same driver to exercise both implementations. It does not change caching or
relax correctness assertions. Output omits media filenames and source URLs.

Measured result
----------------

Median of three fresh-handle runs, with identical compiler, dependencies and
build options for the base and candidate. Each uncached-seek median aggregates
the four new-region targets per run; the repeated 65% target measures cache reuse.
The per-response bandwidth cap permits the same configured two region cursors
in both builds.

.. list-table:: Real Blu-ray ISO, seconds
   :header-rows: 1

   * - Operation
     - Base
     - Candidate
   * - HTTP open
     - 1.469
     - 1.015
   * - HTTP uncached exact seek
     - 1.368
     - 1.289
   * - HTTP cached return seek
     - 0.203
     - 0.094
   * - HTTP + 302 open
     - 1.531
     - 1.203
   * - HTTP + 302 uncached exact seek
     - 1.352
     - 1.290
   * - HTTP + 302 cached return seek
     - 0.188
     - 0.094

Before the initial playback restart, the server sends a median 9.17 MiB
with the base and 7.39 MiB with the final candidate over direct HTTP.
With redirects, the corresponding amounts are 9.17 and 7.94 MiB. These
counts include transport read-ahead; they are not just the bytes required to
decode the first frame.

The synthetic 24-second BDMV map opens in about 0.30 seconds under the same
fixture. Its trimmed two-clip variant opens in about 0.83 seconds. These small
authored fixtures establish navigation correctness, not representative movie
compression or a guaranteed network startup budget.

Each load has a fresh libmpv handle and memory cache. The HTTP fixture binds
only loopback, delays each request by 20 ms, and limits each response to
8 MiB/s. The real sample is an approximately 40-GB Blu-ray. Startup means
load admission to playback restart with a decoded video stream; seek means
``absolute+exact`` completion within 0.5 seconds of the requested timeline
position. Filesystem caches are not forcibly cleared. This is a reproducible
client comparison, not a real CDN or cold physical-disk benchmark. All runs
retain the same chapter list and track types/languages as local ISO playback.

Remaining integration and limits
--------------------------------

* A host must enumerate a WebDAV/cloud BDMV directory and provide its map.
  A bare HTTP file URL cannot provide directory enumeration or sibling signed
  URLs. The new mpv capability is ready for that host integration; this change
  does not implement the Rodel source-provider/UI wiring.
* CDN redirects, signed-link refresh, TLS, proxies and provider throttling need
  acceptance against the actual service. Authenticated loopback HTTP and 302
  are tested here; no public-cloud speed claim is made.
* HDMV/BD-J behavior still belongs to libbluray and the existing menu path.
  BD-J additionally requires a compatible Java runtime and libbluray.jar;
  arbitrary Java menus were not exercised. Encryption capability is unchanged.
* Large exact seeks still need the destination GOP and decoder preroll.
  Throughput, GOP length, playlist count and server latency set a real lower
  bound. A measured one-second open on this sample is not an all-disc SLA.
* The delivered development runtime and source diff are for user acceptance.
  They are not a Store/native acquisition receipt, an application package,
  a pin update or an automatic merge.
