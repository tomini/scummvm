# Modifications to ScummVM

This fork carries local changes on top of upstream ScummVM (https://github.com/scummvm/scummvm),
scoped to the Neverhood engine, for the Neverhood Reklayed project
(https://github.com/tomini/neverhood-reklayed). Kept per GPLv3 §5(a) so the record survives even
if this repository is ever exported without its `.git` history (tarball, `git archive`, release zip).

Upstream ScummVM copyright/license terms are unchanged; see `COPYRIGHT` and `COPYING`.

## Log

### 2026
- `engines/neverhood/resourceman.h`, `engines/neverhood/resourceman.cpp`: added
  `ResourceMan::hasOverride(uint32 fileHash)`, a no-op hook (currently always returns `false`)
  called from `ResourceMan::loadResource`. Groundwork for an HD asset override layer — not yet
  wired to load replacement data. No behavior change yet.
- HD asset override layer, first working version (mechanism only, no real HD art shipped).
  Opt-in: with the setting below absent, the engine takes the upstream code path and output
  (CLUT8, 640x480) unchanged.
  - **Setting:** `hd_overrides_path` (ConfMan, e.g. in `scummvm.ini` under `[scummvm]` or the
    game's domain) = directory with override PNGs. If it is set and is a directory, the engine
    starts in HD mode: 1280x960 true-color output (first 32bpp format the backend offers, else
    16bpp). If it is set but invalid, a warning is printed and the engine runs normally.
  - **File naming (flat directory, hash = 8 hex digits, case-insensitive):**
    `<FILEHASH>.png` replaces a sprite (`SpriteResource`, keyed by the hash passed to `load()`);
    `<ANIMHASH>_<frameIndex>.png` (decimal index, no padding, e.g. `5420E254_0.png`) replaces one
    frame of an animation (`AnimResource`, keyed by the animation hash). Any PNG size is accepted
    and is sampled to fill the original sprite/frame rectangle at 2x; exact 2x of the original
    size maps 1:1 to output pixels. The PNG alpha channel is honored.
  - `resourceman.h/.cpp`: `initOverrides()` scans the directory once; `hasOverride()` now
    answers from that index; `getOverride(fileHash, frameIndex)` decodes lazily via
    `Image::PNGDecoder`, converts to ARGB32 and caches for the engine's lifetime.
  - `resource.h/.cpp`: `SpriteResource` remembers its requested hash;
    `SpriteResource::getHdOverride()` / `AnimResource::getHdOverride(frameIndex)`. An animation
    with active palette color replacement (`setRepl`) keeps its original frames.
  - `graphics.h/.cpp`: `BaseSurface` binds the override to its CLUT8 surface after drawing a
    sprite/anim frame and unbinds on `clear()`, `copyFrom()` (text drawn into the surface) and
    destruction. The CLUT8 surface itself is still filled exactly as upstream does.
  - `screen.h/.cpp`: optional second backbuffer `_hdScreen` (1280x960 true-color). The CLUT8
    `_backScreen` is composited exactly as before; in HD mode every render-queue blit is
    additionally mirrored into `_hdScreen` in queue order (so z-order is preserved): unbound items
    as their CLUT8 result through the current palette, pixel-doubled; bound items sampled from the
    override (with flip), alpha-blended. `_hdScreen` is what goes to the backend in HD mode. A
    palette change re-derives the whole HD frame; no hardware palette is used in HD mode (the
    CLUT8 cursor gets a cursor palette instead).
  - `palette.cpp`: whole-palette fades to/from a flat color (`startFadeToBlack/White`, fade-in
    from a uniform palette) are reported to `Screen`, which applies the same per-step arithmetic
    to override pixels. Partial-palette crossfades and palette cycling do not affect override
    pixels (they have no palette indices).
  - `neverhood.cpp`: HD mode setup; mouse event coordinates are divided by the output scale so
    game logic stays in 640x480 space. `mouse.cpp`: cursor scaled with the output in HD mode.
  - Game logic (positions, collision, hit rects, walk paths) is untouched.
- HD source export hook (`export_hd_source_path` setting), for dumping
  correctly-colored source frames to feed the upscale pipeline (later phase).
  - Same file naming as HD overrides: `<HASH>.png` / `<HASH>_<frameIndex>.png`.
  - Root cause of an early bug: a naive "export at draw-into-offscreen-surface
    time" hook silently captured wrong or missing colors for scene
    backgrounds. `Scene::setBackground()` always runs before
    `Scene::setPalette()`, and `Palette::~Palette()` (previous scene) nulls
    out `Screen::_paletteData` on teardown, so the background's first (and
    only) draw always happens in a window where the new scene's palette isn't
    active yet.
  - Fix: `BaseSurface` now binds a fileHash/frameIndex to itself (mirroring
    `bindHdOverride()`) whenever it's filled, via new
    `SpriteResource::getFileHash()` / `AnimResource::getFileHash()`. The
    actual `exportSource()` call moved to `BaseSurface::draw()`, the point
    where the surface is composited to the screen -- guaranteed to run after
    the scene's own `setPalette()`. `clear()`/`copyFrom()` clear the binding.
    Re-writes a key up to 5 times, not once, since the palette can still be
    mid-fade on the first few draws. `AnimResource::hasActiveColorReplacement()`
    (new) skips frames under `setRepl()`, same reasoning as `getHdOverride()`.
- `graphics.cpp`, `BaseSurface::copyFrom()`: exports the surface's still-clean
  pixels one last time right before they get overwritten, if it never got a
  `draw()` first. Example: text composited onto a background before any frame
  renders, like Scene1005's note paper (`0x8870A546`). The export added above
  only fires from `BaseSurface::draw()`, so that case was previously missed
  entirely. By then the scene's own `setPalette()` has normally already run.
- `resourceman.{h,cpp}`, `graphics.cpp`: two more HD source export fixes.
  - Real per-pixel alpha: `exportSource()` now takes the surface's
    `transparent`/`alphaColor` (mirroring what `Screen::blitRenderItem` would
    use for the same draw) and, when transparent, builds an RGBA32 image
    before calling `Image::writePNG()` instead of a flat RGB palette image.
    Without this, the "transparent" palette index (almost always 0) came out
    as an opaque solid color in the exported PNG, even for a single stray
    pixel inside otherwise-opaque art.
  - Mirrored draws are un-mirrored on write, not skipped: a flipped and
    unflipped draw of the same asset share one fileHash/frameIndex key (the
    HD override render path re-applies flipX/flipY at blit time on top of a
    canonical source image, same as the original engine mirroring one
    authored bitmap). `bindSourceExportKey()` now records flipX/flipY too;
    `exportSource()` reads source pixels in mirrored order when writing, so
    every write lands in the same canonical orientation regardless of which
    flip state triggered it. No asset is skipped, and none can be corrupted
    with a backwards image by an unlucky draw order.
