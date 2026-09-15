# Native subtitle positioning regression

The Windows-only `libmpv-test-subtitle-positioning` target exercises the public
libmpv API with local synthetic video and subtitle fixtures. It uses `vo=null`,
`ao=null`, and software `screenshot-raw` frames, not desktop capture, a GPU, or a
translation provider.

Configure mpv with `-Dtests=true -Dlibmpv=true`, then build the target:

```text
meson compile -C build libmpv-test-subtitle-positioning
```

Stage `build\test\libmpv-test-subtitle-positioning.exe` beside the exact
`libmpv-2.dll` being verified and its complete runtime dependency closure.
The staged directory must be writable for the test's temporary fixtures.
Run in an isolated Windows test environment:

```text
libmpv-test-subtitle-positioning.exe --selftest-relative
```

Each scenario owns a fresh libmpv client. Position, visibility, and restoration
transitions within one scenario stay on that client, while independent shape
references use a separate client. The synthetic video uses full-resolution
4:4:4 chroma so a subtitle's placement cannot change the shape oracle through
chroma subsampling.
Software color conversion uses swscale rather than zimg's default random
dithering, keeping exact pixel comparisons independent of absolute placement.
The relative gate uses the default LIBASS software compositor, including its
source/destination clipping. The legacy comparison forces RGBA subtitle
conversion because older direct LIBASS compositors cannot safely consume
post-layout bitmaps outside the output.

Only `--selftest-relative` is the relative-position regression gate. `--selftest`
is a comparison probe for the renderer-specific `auto` behavior and can report
non-moving or coupled positions. Both forms emit JSON Lines with geometry and
cleanup facts followed by a `selftest:` summary compatible with the public
Sprout.DevTools VM selftest command. A nonzero exit is never a passing gate.
