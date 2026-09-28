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
- Headless scene walker (`walk_all_scenes` setting), so the HD source export
  covers the whole game without anyone playing it. Needed for the "Model 3"
  distribution plan: extraction has to run unattended on the end user's own
  copy of the game.
  - **Settings** (ConfMan / `scummvm.ini`, same pattern as the other HD hooks):
    `walk_all_scenes=true` together with `export_hd_source_path` (the walker
    refuses to start without it). With the walker on, `NeverhoodEngine::run()`
    runs the walk instead of normal play, then exits. Optional tuning:
    `walk_all_scenes_frames` (drawn frames per full job, default 200),
    `walk_all_scenes_settle_frames` (update-only frames before drawing,
    default 16), `walk_all_scenes_max_variants` (default 48 per scene).
  - **Enumeration:** `engines/neverhood/scenewalker_table.h` is generated by
    `devtools/neverhood_scenewalker_table.py` from the actual
    `GameModule::createModule()` and `ModuleNNNN::createScene()` switches: 21
    modules, 173 sprite-bearing scenes (cases that only create a Smacker
    video or a NavigationScene are dropped, since video goes through ffmpeg),
    413 `(module, sceneNum, which)` combinations (save-restore path `-1`
    plus every literal `createScene(N, W)` in each module), plus 6 MenuModule
    screens. Re-run the generator after changing any `createScene()` switch.
  - **Construction:** the same path as loading a save game.
    `GameModule::createModule(module, -1)` with `gameState().sceneNum` set;
    for a specific `which`, the restored scene is then replaced via the new
    `Module::walkerCreateScene()`. `module.h`: `createScene()` is now
    virtual in `Module` (every module already declares it with the same
    signature, so no module header changed). Only the scene is updated
    per frame, not its module, so a scene that "finishes" never triggers the
    module's own navigation. Each job starts from cleared game vars (a new
    game), with all sound muted and stopped between jobs.
  - **Frames:** 16 update-only frames first, so palette fades settle before
    the export hook's first writes. Then 200 frames of update/draw/
    `Screen::update()`. That covers short loops and Klaymen's idle animations,
    but not every random idle or anything that needs input.
  - **Global-var branches:** found at runtime instead of by hand.
    `GameVars` gained an optional read trace. Every var a scene's
    construction reads while it is 0 becomes a variant: flipped to 1
    (or to each value 1..N for the few multi-valued selectors like
    `V_TELEPORTER_WHICH`, `V_PROJECTOR_LOCATION`, `V_KEY3_LOCATION`,
    `V_MATCH_STATUS`). The walker also runs all flags at once, plus pairs for
    vars that only get read after another one was flipped (nested
    branches). `ResourceMan::queryResource()` gained an optional trace of
    bitmap/palette/animation hashes. A variant that asks for nothing new
    stops after the settle frames, so dead-end flips cost ~10-20 ms.
    Bookkeeping and hash-valued vars are never flipped: `V_MODULE_NAME`,
    `V_COLUMN_TEXT_NAME` and the rest (a hash of 1 crashes the engine).
  - **Menus:** `originalsaveload` is forced on (transient ConfMan domain,
    restored afterwards) for the walk. Without it the save/load/delete
    screens open ScummVM's modal dialog instead of building the original art
    (`30084E25`, `98620234`, `4080E01C`, overwrite prompt `043692C4`).
  - **Crash resilience:** `scenewalker_progress.txt` next to the exports
    records the running job before it is constructed. If the process dies
    (segfault, or `error()` from a scene that can't be built headlessly),
    the next launch marks that job `FAILED`, skips it and resumes. A driver
    just relaunches until the file says `COMPLETE`.
    `scenewalker_log.txt` logs every job.
  - **Known gaps:** mouse cursors (drawn through CursorMan, not
    `BaseSurface`); art only reachable through interaction or randomized
    puzzle state (e.g. the one test-tube level combination the random init
    picked); pairs beyond the per-scene variant cap (hit once, Scene1405);
    assets drawn under several palettes keep the first-captured one (existing
    one-PNG-per-key design). Some resources are referenced by no engine code
    at all (e.g. `00004011`, a menu-style box with one button), so neither
    the walker nor a playthrough can reach them.
- Extra-hash export for puzzle-piece / randomized-selection sprites
  (`scenewalker.{h,cpp}`, `devtools/neverhood_scenewalker_table.py`).
  Comparing a full walk's output against an older third-party static
  extraction turned up hashes the walk never captured that are still
  referenced by engine code, not dead resources: each module's own
  `k...FileHash...[]` arrays (e.g. a code-lock's digit sprites, a
  matching-puzzle's pieces), which the game selects between at runtime by
  random or puzzle state, so a given walk job only ever sees whichever one
  its state happened to pick.
  - The generator now also scans each module's `.cpp`/`_sprites.cpp`/
    `_sprites.h` files for these arrays and collects every literal hash into
    `kSceneWalkerExtraHashes[]` (410 found), keyed by the owning module.
  - `SceneWalker::exportExtraHashes(moduleNum)` runs once per module, right
    after that module's first full job finishes (still mid-scene, so
    `Screen::_paletteData` holds that module's real palette). Getting a
    correct, non-crashing version of this function took three rounds of
    rebuild-and-gdb before landing on its current design:
    - **First attempt:** loaded each hash as a stack-local `SpriteResource`
      and drew it into a throwaway `BaseSurface`, calling
      `Screen::update()` to flush the blit `draw()` queues. That actually
      *composited* each arbitrarily-sized throwaway surface onto the real,
      shared 640x480 `_backScreen` at (0,0), corrupting whatever the real
      scene had drawn there and, for larger sprites, memory past it.
    - **Second attempt:** kept every loaded `SpriteResource` alive
      (deliberately leaked) instead of letting each one unload itself at
      the end of its loop iteration, on the theory that unloading was
      dropping a shared fileHash's refcount to 0 and letting
      `ResourceMan::purgeResources()` free data a live scene still expected.
      Also removed the `Screen::update()` call, keeping only
      `clearRenderQueue()` to discard the queued blit unprocessed. The
      crash (`ResourceMan::purgeResources()` dereferencing a garbage
      pointer, caught with gdb both times) persisted identically,
      disproving both theories -- confirmed with a clean isolated re-test
      that removing the whole feature made the walker complete with 0
      failures, so the bug really was here, just not where either fix
      assumed.
    - **Final design:** bypasses `SpriteResource`/`ResourceMan::loadResource()`
      entirely, since going through it at all -- load and unload alike --
      was the common thread across both failed attempts, and its shared
      `_data` cache was never fully cleared of suspicion as the actual
      corruption site. `queryResource()` (no caching side effects) checks
      the hash is a bitmap; the new `ResourceMan::readResourceUncached()`
      decompresses the entry into a fresh, caller-owned buffer (the same
      `BlbArchive`/`NhcArchive::load()` decompression `loadResource()` uses,
      just never touching its shared cache) -- an earlier version of this
      read raw bytes via the existing `createStream()` instead, which turned
      out to hand back the on-disk bytes undecompressed (`BlbArchive::load()`
      DCL-decompresses; `createStream()` is a raw substream, meant for
      Smacker video decoding it as it streams), so only the handful of
      entries that happened to be stored uncompressed decoded correctly.
      `parseBitmapResource()`/`unpackSpriteRle()` (both already used
      elsewhere, just not normally called directly like this) decode by
      hand into a throwaway `BaseSurface`, which is exported (`transparent`
      = true, matching how `BaseSurface::drawSpriteResource()` always draws)
      and then freed immediately. No caching, no refcounting, nothing shared
      with any live scene's own resource handles at all.
  - **Verified:** a full walk now completes in ~70s with 0 crashed jobs
    (previously 0, matching the walker's own pre-existing baseline), and
    exports 301 of the 410 candidate hashes across 10 modules (the rest are
    non-bitmap entries the array-name pattern also picked up, e.g. a stray
    sound or animation id, correctly skipped by the `kResTypeBitmap` check).
