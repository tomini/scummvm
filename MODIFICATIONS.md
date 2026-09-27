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
