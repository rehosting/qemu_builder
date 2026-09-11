# fastsnap: what is adopted, from where, and under what licence

Everything in `src/fastsnap/` and `src/include/fastsnap/` is
**GPL-2.0-or-later**, and each file carries an SPDX header saying so. That is
not a change in the repo's licensing posture — this whole tree is a QEMU
derivative and QEMU is GPL-2.0 — but two of these files are *adopted from a
third party*, so they get an explicit declaration rather than an inherited one.

## The adopted files

| file | origin |
|---|---|
| `device-save.c` | [qemu-libafl-bridge](https://github.com/AFLplusplus/qemu-libafl-bridge), `libafl/syx-snapshot/device-save.c` |
| `channel-buffer-writeback.c` | same repo, `libafl/syx-snapshot/channel-buffer-writeback.c` |
| `../include/fastsnap/device-save.h` | same repo, corresponding header |
| `../include/fastsnap/channel-buffer-writeback.h` | same repo, corresponding header |

- **Upstream commit:** `4df4d2dcfa0d2eecfb267cddf5ebfb8ef9f58d87` ("Add crash
  exit request (#106)", 2025-03-14)
- **Upstream QEMU base at that commit:** 9.1.1
- **Upstream licence:** the repository is GPL-2.0. The individual files carried
  no per-file licence header; the SPDX lines here are ours, asserting the
  repository licence explicitly rather than leaving it implicit.

`selftest.c` and the pristine copies of the upstream originals are ours and
were written for this port.

Adoption was whole-file with attribution, deliberately. The alternative —
folding these into the patch series — would interleave third-party GPL code
with our own edits inside patch hunks, where its provenance stops being
legible. Keeping them as whole files in `src/` means the boundary between
"adopted" and "authored" is a directory listing.

## What changed, and why

### `channel-buffer-writeback.c` — small

1. Include paths `libafl/` → `fastsnap/`; `syx-misc.h` dropped (unused here).
2. `class_init`'s `class_data` parameter became `const void *` in QEMU 11.1.0.
3. **New:** `qio_channel_buffer_writeback_new_reader()`, a read-only view over
   a caller-owned buffer. See below.
4. The `writeback_buf_capacity >= usage` assert moved inside the
   `if (writeback_buf)` guard, since a reader has neither.

### `device-save.c` — substantially rewritten

The logic is upstream's; the way it reaches QEMU is not.

1. **The handler walk.** Upstream reads `savevm_state.handlers` directly, which
   needs `SaveStateEntry`'s and `SaveState`'s layout — both private to
   `migration/savevm.c`. It gets them by hoisting the two structs into
   `migration/savevm.h` and deleting them from the `.c`.

   That is a duplicated struct definition that has to track upstream
   byte-for-byte or corrupt memory silently, and it is the worst possible thing
   to carry in a series rebased across upstream releases: it applies cleanly
   and is wrong. Replaced with two accessors added inside `savevm.c` (series
   patch *"migration: expose the savevm handler list"*), through which
   `SaveStateEntry` stays an incomplete type out here. Nothing in this file can
   go stale against a layout change.

2. **`se->is_ram` is gone**, removed in QEMU 11.x. Its name was always
   misleading: it was set for *any* handler with a `save_setup` op — every
   live/iterative handler, so `ram`, `dirty-bitmap`, `slirp`, `spapr/htab`,
   `vfio`. The accessor reports that predicate as `iterative`. Matching on
   `idstr == "ram"` instead would be a real bug: it would pull the other live
   handlers into a block with no stream to iterate them over.

3. **`vmstate_save()` gained an `Error **`.** Upstream's `extern` declares it
   with three arguments against a four-argument definition, so `errp` was an
   uninitialised register on every call. Now routed through
   `qemu_savevm_save_one()`, which has the real signature, and the error is
   actually reported.

4. **`qemu_load_device_state()` gained an `Error **`** likewise, and its return
   value is now checked.

5. **Double-free fix.** Upstream's restore path builds its channel with
   `qio_channel_buffer_new_external()` — a function it adds to
   `io/channel-buffer.c`. But upstream's own `qio_channel_buffer_finalize()`
   calls `g_free(ioc->data)` unconditionally, so unref'ing that channel frees
   the caller's buffer, leaving `DeviceSaveState` dangling and making the later
   `device_free_all()` a double free. Reproduced as a SIGSEGV.

   Fixed by not going there at all: `..._writeback_new_reader()` above keeps
   the borrowed-buffer case inside our own file, which removes the bug **and**
   two patches to `io/` from the series. The upstream footprint of this whole
   feature is now exactly `migration/savevm.{c,h}` plus one `subdir()` line.

6. `libafl_*` symbol prefix → `fastsnap_*`.

## Acceptance

`selftest.c`, run at machine-init-done when `FASTSNAP_SELFTEST` is set:

```
FASTSNAP_SELFTEST=1 ./qemu-system-aarch64 -M virt -cpu cortex-a57 -m 128 -display none -S
```

Verified on QEMU 11.1.0: **PASSED**, 17 device sections, 63029 bytes. Verified
to *fail* (exit 1) when `device_restore_all()` is neutered, so the gate has
teeth; to report SKIPPED rather than FAILED on a machine with no PL011; and to
be silent and non-terminating when the variable is unset.

## Known limitations

- **No `cpu_synchronize_all_states()` before the walk.** QEMU's own
  `qemu_savevm_state_non_iterable()` calls it. Under TCG — every Penguin
  rehosting run — it is a no-op, and omitting it is what the measured numbers
  were taken against. Under KVM the device block would capture stale CPU state.
  Add it before using this with an accelerator.
- **`FASTSNAP_DEVICE_BLOCK_LIMIT` is 32 MB, allocated per save**, against a
  real block of ~64 KB. The pages are never touched, so the cost is address
  space rather than memory, but a tight loop would still rather size this from
  the previous block than reallocate 32 MB each time.
- **The allowlist is a correctness question this code does not answer.**
  `DEVICE_SNAPSHOT_ALLOWLIST` restores only named sections, which is where the
  measured 17.5x speedup comes from — but a set derived by diffing one workload
  is a lower bound on what must be restored, not a proof of sufficiency.
