# Licensing

**QEMU is not one licence, so this series is not one licence either.** That is
the whole content of this file; everything below is the consequence.

QEMU's own `LICENSE` says the emulator *as a whole* is GPL-2.0, and that is true
of the aggregate. But a patch is a derivative of the specific files it edits, not
of the aggregate — and measured across the 34 upstream files this series touches:

| | count |
|---|---|
| `GPL-2.0-or-later` | 11 |
| `LGPL-2.1-or-later` | 10 |
| no statement in the file | 6 |
| **`MIT`** | **5** |
| **`GPL-2.0-only`** | **2** |

`migration/savevm.c`, `system/runstate.c`, `system/vl.c`, `hw/i386/pc.c` and
`hw/mips/malta.c` are MIT. The whole `target/*/translate.c` and `op_helper.c`
family is LGPL-2.1-or-later. So a repo-level "the patches are GPL-2.0" would be
wrong about a seventh of them, needlessly restrictive about a third, and would
quietly hide the one place the "or later" option is actually lost.

## So the licence follows the patch, and the patch belongs to a set

Each patch carries two trailers in its own commit message, which means they
travel *inside* the patch and cannot drift away from it across a `git am` /
`format-patch` round trip:

```
License:   <SPDX expression>     what THIS patch is a derivative of
Patch-Set: <set name>            which project's contribution it belongs to
```

### Patch sets

A feature is rarely one patch, and a feature adopted from another project needs
its provenance stated once rather than copied onto every patch that belongs to
it. So the series is partitioned into named sets, each described by
`patches/<version>/sets/<name>.json` — where the incoming project, its commit,
its licence and its provenance document live exactly once:

```json
{ "name": "fastsnap",
  "summary": "Device state in a block ...",
  "origin": { "kind": "adopted",
              "project": "qemu-libafl-bridge",
              "url": "https://github.com/AFLplusplus/qemu-libafl-bridge",
              "commit": "4df4d2dcfa0d2eecfb267cddf5ebfb8ef9f58d87",
              "upstreamQemuBase": "9.1.1",
              "license": "GPL-2.0-or-later",
              "provenance": "src/fastsnap/PROVENANCE.md" },
  "srcPaths": ["src/fastsnap", "src/include/fastsnap"] }
```

Membership lives in the patch; description lives in the set file. That split is
what survives editing — a patch cannot be renamed, split or reordered out of its
set without the gate noticing, and a set's provenance cannot be updated in one
place and go stale in thirteen others.

**A set's patches must be contiguous in the series.** A project's contribution
staying in one run is what makes it reviewable, droppable and rebasable as a
unit; the moment it interleaves with another set's, "which patches came from
where" becomes archaeology. The gate enforces it.

**`srcPaths` is how a set covers the half of itself that is not a patch.**
`fastsnap` is one series patch plus two directories of adopted source. Naming
both in one place is what makes the set the *feature* rather than just its diff
— and it is the reason the adopted code can live in `src/` as whole files,
where its provenance stays legible, without becoming detached from the patch
that needs it.

Today:

| set | patches | origin |
|---|---|---|
| `igloo-core` | `0001`–`0013` | authored here |
| `fastsnap` | `0014` | adopted, `qemu-libafl-bridge@4df4d2dcfa`, GPL-2.0-or-later, `src/fastsnap/PROVENANCE.md` |

### The gate

`scripts/check-patch-licensing.py` **recomputes** each patch's effective licence
from the files it touches and fails if the declaration disagrees. It runs in
`series.yml`, the fast gate. Adding a file to a patch's footprint therefore
fails CI if that file's licence changes the answer — the same
no-parallel-list-that-can-drift discipline `configs/*.json` uses for `nixDeps`.

It also fails when a patch names no set, names one with no description, when a
set's patches interleave with another's, when a set file is described but no
patch claims it, when an adopted set omits its project, url, commit, licence or
provenance, or when a `srcPath` does not exist. Each of those was verified to
fail; none of them is a convention anyone has to remember.

Combining rule, most permissive to least — a derivative of several files must be
offered under terms satisfying all of them:

```
MIT  ->  LGPL-2.1-or-later  ->  GPL-2.0-or-later  ->  GPL-2.0-only
```

LGPL-2.1 §3 permits conversion to GPL-2, which is why LGPL sits below GPL.
`GPL-2.0-only` is last because combining it with an "or later" work loses the
later-version option for the result. Files carrying no statement of their own
add no constraint and are reported as `inherit`, never guessed at.

Where that lands today:

| licence | patches |
|---|---|
| `MIT` | `0001`, `0012`, `0013` |
| `inherit` | `0002` |
| `LGPL-2.1-or-later` | `0003`, `0005`, `0008`, `0011` |
| `GPL-2.0-or-later` | `0004`, `0006`, `0007`, `0009`, `0014` |
| `GPL-2.0-only` | `0010` |

**`0010` is the one to know about.** It touches
`hw/virtio/vhost-user-vsock.c`, which is GPL-2.0-**only** — the single place in
this series where downstream loses the option of a later GPL version.

## The rest of the tree

| what | where | licence |
|---|---|---|
| builder scaffolding — `build.sh`, `flake.nix`, `nix/`, `scripts/`, `configs/`, CI | repo root | **MIT**, `LICENSE`. Same choice `rehosting/linux_builder` made for the same kind of code, and it is genuinely our own work rather than a derivative of anything. |
| QEMU source we authored | `src/system/penguin.c`, `src/include/system/penguin.h`, `src/scripts/penguin-*.py` | ours; compiled into QEMU, so the *built artifact* is GPL-2.0-or-later as a whole, which `nix/qemu.nix`'s `meta.license` already declares |
| QEMU source we **adopted** | `src/fastsnap/`, `src/include/fastsnap/` | **GPL-2.0-or-later**, third-party in origin, SPDX header on every file. See `src/fastsnap/PROVENANCE.md`. |
| edits to upstream QEMU | `patches/` | per patch, as above |

## The adopted code, specifically

`src/fastsnap/device-save.c` and `src/fastsnap/channel-buffer-writeback.c`, plus
their headers, come from
[qemu-libafl-bridge](https://github.com/AFLplusplus/qemu-libafl-bridge) at
`4df4d2dcfa0d2eecfb267cddf5ebfb8ef9f58d87`. That repo is GPL-2.0. The files
carried no per-file licence header upstream; the SPDX lines on our copies assert
the repository licence explicitly rather than leaving it implicit.

They are kept as **whole files in `src/`** rather than folded into the patch
series, deliberately. Interleaving third-party code into patch hunks is where
provenance stops being legible — the boundary between adopted and authored
should be a directory listing, not a diff review.

## Not settled here

`LICENSE` is copied verbatim from `rehosting/linux_builder`, copyright line
included, on the assumption that the same org making the same kind of build
tooling wants the same terms. That is an inference from precedent, not a
decision this file is entitled to make — confirm it before relying on it.
