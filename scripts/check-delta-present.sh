#!/usr/bin/env bash
# Assert the IGLOO delta actually compiled into the built libraries.
#
#   ./scripts/check-delta-present.sh <penguin-qemu-out-dir>
#
# WHY THIS EXISTS. The boot gate we ship is a minimal kernel+initramfs boot,
# mirroring linux_builder's nix/boot.nix. That gate has a known hole: it passes
# even if the entire IGLOO delta is inert, because it never executes a
# hypercall. Proving a hypercall round-trips needs penguin + a kernel + a
# rootfs, i.e. a cross-repo integration test, and wiring that into this repo
# would create a qemu_builder -> penguin -> qemu_builder CI cycle.
#
# This is the cheap middle: it cannot prove a hypercall WORKS, but it does prove
# every patch that should contribute code to the binary actually did. A patch
# that silently stopped applying to a hunk, or a target whose translate hook was
# dropped, fails here rather than being discovered in a rehost weeks later.
set -euo pipefail

OUT="${1:?usage: check-delta-present.sh <penguin-qemu-out-dir>}"

# Symbols the core + callbacks patches must contribute to every system library.
CORE_SYMS=(
    penguin_handle_guest_hypercall
    penguin_guest_hypercall_registered
    penguin_clear_guest_hypercalls
    penguin_cpu_env
    penguin_handle_qmp
    penguin_invoke_reset_request_callback
)
# The per-arch TCG hypercall entry. Every TCG system library must define this;
# its absence means that target's hypercall-entry patch contributed nothing.
ARCH_SYM=helper_penguin_guest_hypercall

shopt -s nullglob
libs=("$OUT"/lib/libqemu-system-*.so)
[ ${#libs[@]} -gt 0 ] || { echo "FAIL: no libqemu-system-*.so under $OUT/lib" >&2; exit 1; }

rc=0
for lib in "${libs[@]}"; do
    name=$(basename "$lib")
    syms=$(nm -D --defined-only "$lib" 2>/dev/null | awk '{print $NF}')
    missing=()
    for s in "${CORE_SYMS[@]}" "$ARCH_SYM"; do
        grep -qx "$s" <<<"$syms" || missing+=("$s")
    done
    if [ ${#missing[@]} -eq 0 ]; then
        echo "  ok    $name ($(grep -ci penguin <<<"$syms") penguin symbols)"
    else
        echo "  FAIL  $name missing: ${missing[*]}" >&2
        rc=1
    fi
done

[ $rc -eq 0 ] && echo "PASS: the IGLOO delta is present in all ${#libs[@]} system libraries"
exit $rc
