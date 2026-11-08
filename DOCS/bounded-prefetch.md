# Bounded next-file prefetch

The experimental `prefetch-file <url>` command prepares one HTTP(S) input in
the current player instance, without replacing the playing file, starting a
decoder, or advancing the playlist. The caller must only supply a direct-file
URL whose acquisition does not start server-side transcoding.

The speculative input uses the curl transport and the current network and
demuxer options. Its budget is independent of ordinary playback: two seconds
of demuxer readahead, at most 8 MiB of reserved HTTP response ranges, four
requests including redirects, and five seconds to become ready. Retries,
nested resources, playlists, and ordered-chapter timelines are not prepared.
The byte-range cache shares the same budget with its additional cursors.
Servers must honor byte ranges. TCP/TLS overhead and bytes already sent by a
server after cancellation are not represented by the received-body counter.

`prefetch-state` is 0 for no pending input, 1 while preparing, 2 when ready,
and 3 after preparation failed. `prefetch-bytes` and `prefetch-requests` expose
non-secret work counters. `prefetch-adoptions` counts successful transfers into
ordinary playback. These properties can be read without any URL or header
disclosure.

Once ready, loading the exact same URL with compatible effective options adopts
the prepared demuxer and removes its speculative limits. A `stop` followed by
`loadfile` on an idle-enabled instance can retain this preparation. A different
URL, incompatible options, or an unfinished/failed preparation uses the normal
loading path. `prefetch-cancel` releases only the speculative input; it does not
stop the playing file. Final instance shutdown also releases it.

The application owns the decision to prepare a next item, cancellation on route
changes, provider-session ownership, and playback reporting. In particular,
preparation is not evidence that the user started or watched the next item.

Run `python -B test/libmpv_prefetch.py <library>` against a locally built runtime
to exercise bounded redirects, cancellation, rate-limit refusal without retry,
and same-instance adoption followed by byte-exact playback.
