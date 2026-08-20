# qemu_builder

The IGLOO QEMU delta, carried as an explicit patch series applied to a pristine
upstream QEMU **release tarball** — replacing the long-lived fork of
`rehosting/qemu`, which sat on a mid-cycle upstream staging merge.

This is the second application of the pattern `linux_builder` adopted in its
PR #59; read that repo as the reference implementation.

## Layout

```
base.json               upstream release tag, URL, tarball hash, expected patched-tree hash
patches/11.1.0/         the series + its `series` file (quilt convention)
src/                    the half of the delta we authored outright — copied in, never patched
configs/                the configure feature set as data
build.sh                builds penguin-qemu from a prepared tree; reads configs/<profile>.json
nix/source.nix          tarball + series + src/ -> a source derivation
nix/qemu.nix            penguin-qemu package (libqemu-system-*.so + qemu-img + CFFI bindings)
scripts/verify-series.sh    THE GATE
scripts/import-series.sh    tarball + series -> a dev git tree
scripts/export-series.sh    dev git tree -> the committed series
scripts/probe-versions.sh   portability report across upstream refs
```

## The invariant that makes this work

**No patch in the series ever references a file we created.**

The delta splits three ways, and only the third is a patch series:

| class | files | destination |
|---|---|---|
| builder scaffolding (`build.sh`, `Dockerfile`, flake, CI, `nix/`) | 9 | this repo's root — never a patch |
| new QEMU source we authored (`system/penguin.c`, `include/system/penguin.h`, `scripts/penguin-*.py`) | 5 | `src/`, copied into the tree |
| **edits to upstream files** | **33** | **`patches/`** |

Getting this wrong is not a style question. Exporting our 38 development commits
chronologically and applying them to v11.1.0 gets **8 of 38**, because 26 of
those patches edit files we invented earlier in the same series. Enforcing the
invariant and curating by topic gets **12 of 12**.

## Why the series is curated, not chronological

`linux_builder`'s series is a numbered `format-patch` export that preserves
commit order. That costs it nothing, because its base never moves. Ours must
move, and chronology is where the conflicts come from:

- `797f446cf8` ("add multi-target system emulation hooks") touches **9 of the 12
  topics**; `d7f87bf73e` touches 5. Those two commits made the history
  unsplittable and every downstream fixup a conflict.
- Three separate "tighten hypercall match" commits each conflict with the
  original add. `0032` reverts an `rt` symlink `0030` committed by accident.

So the 38 commits were folded into 12 topical patches, each naming the original
commits it folds. The pre-conversion history is preserved as a tag; provenance is
recoverable without being load-bearing.

## Why a release tarball, and why that is easier here than for the kernel

Both verified against v11.1.0, not assumed:

- QEMU's `.gitattributes` has **no `export-ignore`**, so there is no subtractive
  tag-vs-tarball skew. `linux_builder`'s gate has to whitelist four paths; ours
  whitelists none. All 33 files the series touches are byte-identical between the
  `v11.1.0` tag and the `v11.1.0` tarball.
- The tarball ships `subprojects/{keycodemapdb,berkeley-softfloat-3,berkeley-testfloat-3}`
  already populated and build-ready. A git base needed three `fetchFromGitLab`
  pins with hand-maintained per-version hashes; `nix/qemu.nix` lost 41 lines
  when that block went away.
- QEMU has 15 git submodules (`roms/*`, `tests/lcitool`). The series touches
  none of them, and a tarball base avoids fetching them entirely.

## The gate

```bash
./scripts/verify-series.sh 11.1.0 qemu-11.1.0.tar.xz
```

Three assertions, increasing in strength:

1. the tarball is the one `base.json` pins (sha256)
2. the series applies with **strict context** — `git am`, deliberately *without*
   `--3way`, so upstream moving under us is a loud failure a human ports rather
   than a silent auto-merge
3. the patched tree hashes to `base.json`'s `patchedTree`

Assertion 3 is the one that earns its keep. `linux_builder`'s `6db7363` had to
repair a patch that was **corrupt rather than stale** — and it still applied
cleanly. Verified here: corrupting one line of `0012` still passes 12/12 in step
2 and is caught by step 3.

## Working on it

```bash
./scripts/import-series.sh 11.1.0 qemu-11.1.0.tar.xz /tmp/qemu-dev
cd /tmp/qemu-dev && git commit ...        # develop normally
./scripts/export-series.sh 11.1.0 /tmp/qemu-dev
./scripts/verify-series.sh 11.1.0 qemu-11.1.0.tar.xz --record
```

`import-series.sh` adds the vendored `src/` overlay as a final commit marked
`VENDORED:`; `export-series.sh` skips it, so the invariant holds automatically.

## Versioning

One live version, rebased forward. `patches/<version>/` is keyed by upstream
release so a second live version is *additive*, but we deliberately maintain
exactly one — unlike the kernel, nothing in our QEMU use needs two.

## Portability

`./scripts/probe-versions.sh 11.1.0 <qemu.git> v11.0.0 v10.2.0 upstream/master`

Failure classes: `ok` (strict), `drift` (3-way resolves it), `moved` (upstream
renamed a file the patch targets), `conflict` (needs a human).

| ref | ok | drift | moved | conflict |
|---|---|---|---|---|
| **v11.1.0** (base) | **12** | 0 | 0 | 0 |
| `upstream/master` | 9 | 2 | 0 | 1 |
| v11.0.0 | 6 | 3 | 1 | 2 |
| v10.2.0 | 4 | 2 | 2 | 4 |

Forward motion is cheap: on a master a week past our base, 11 of 12 patches need
no human attention. Backward degrades faster, as expected — the patches assume
post-v11.0.0 code. `moved` is a path rewrite; `drift` is auto-resolvable.

## State — read before relying on this

**Proven:** the series applies 12/12 to the pristine v11.1.0 tarball with strict
context and reproduces the recorded tree; the gate catches corruption that still
applies; the ported series is content-identical to the original 38-commit delta
(625 → 626 added lines, the one difference being a deliberate reflow in
`hw/i386/pc.c`); `configs/default.json` reproduces the same 11 targets and 11
libraries as the hardcoded arrays it replaces; the tarball/tag and subproject
claims above.

**Written but NOT yet built or booted:** `nix/source.nix`, `flake.nix` and the
config-driven `build.sh`. All three parse; none has completed a QEMU build here.
The minimal-boot gate is not written yet.

**Deliberately deferred:**
- **byok's `penguin/` tree is not vendored yet.** `origin/workspace/byok` adds
  `penguin/plugins/` + `penguin/tools/` (+1421) and edits `accel/tcg/cputlb.c`
  and `system/runstate.c`. Its `penguin/` tree belongs in `src/`, its two
  upstream-file edits belong in the series. Landing half of it would be worse
  than none, so it waits for byok.
- **`hw/i386/pc.c` owner argument.** Upstream changed the adjacent
  `ioportF0_io` region to `OBJECT(pcms)`; our `ioport88_io` still passes `NULL`.
  Preserved deliberately — a port should not absorb an unrelated ownership
  refactor — but worth revisiting.
- **The config contract gate** (declared features vs what configure actually
  recorded) is not written; `configs/` is currently data without a gate.
