#!/usr/bin/env bash
# Assert the IGLOO delta actually compiled into the built libraries.
#
#   ./scripts/check-delta-present.sh <penguin-qemu-out-dir>
#
# WHY THIS EXISTS. The boot gate we ship is a minimal kernel+initramfs boot,
# mirroring linux_builder's nix/boot.nix. That gate has a known hole: it passes
# even if the entire IGLOO delta is inert, because it never executes a
# hypercall. Proving a round-trip needs penguin + a kernel + a rootfs, i.e. a
# cross-repo integration test, and wiring that in here would create a
# qemu_builder -> penguin -> qemu_builder CI cycle.
#
# This is the cheap middle: it cannot prove a hypercall WORKS, but it proves
# every patch that should contribute code to a given library actually did.
#
# TWO GUEST-ENTRY PATHS, and the check must know the difference. Twelve targets
# reach Penguin through a TCG helper (helper_penguin_guest_hypercall, one patch
# per arch). x86 does NOT: it has no convenient spare instruction, so the guest
# writes to I/O port 0x88 and patch 0008 registers a MemoryRegion for it. There
# is therefore no TCG helper in the x86 libraries by design -- asserting one
# universally is a false positive, which is exactly what happened before the
# full 14-arch build ran. For x86 we check the MemoryRegion's name literal
# instead, which is what proves 0008 landed.
set -euo pipefail

OUT="${1:?usage: check-delta-present.sh <penguin-qemu-out-dir>}"

# The core + callbacks patches must contribute these to EVERY system library.
CORE_SYMS=(
    penguin_handle_guest_hypercall
    penguin_guest_hypercall_registered
    penguin_clear_guest_hypercalls
    penguin_cpu_env
    penguin_handle_qmp
    penguin_invoke_reset_request_callback
)
TCG_SYM=helper_penguin_guest_hypercall   # every target EXCEPT x86
IOPORT_STR=penguin-hypercall             # x86 only: the port-0x88 MemoryRegion name

is_x86() { case "$1" in x86_64|intel64) return 0 ;; *) return 1 ;; esac; }

shopt -s nullglob
libs=("$OUT"/lib/libqemu-system-*.so)
[ ${#libs[@]} -gt 0 ] || { echo "FAIL: no libqemu-system-*.so under $OUT/lib" >&2; exit 1; }

rc=0
for lib in "${libs[@]}"; do
    name=$(basename "$lib")
    arch=${name#libqemu-system-}; arch=${arch%.so}
    syms=$(nm -D --defined-only "$lib" 2>/dev/null | awk '{print $NF}')
    missing=()

    for s in "${CORE_SYMS[@]}"; do
        grep -qx "$s" <<<"$syms" || missing+=("$s")
    done

    if is_x86 "$arch"; then
        grep -q "$IOPORT_STR" <(strings "$lib") || missing+=("<string:$IOPORT_STR>")
        path="ioport"
    else
        grep -qx "$TCG_SYM" <<<"$syms" || missing+=("$TCG_SYM")
        path="tcg-helper"
    fi

    if [ ${#missing[@]} -eq 0 ]; then
        echo "  ok    $name [$path] ($(grep -ci penguin <<<"$syms") penguin symbols)"
    else
        echo "  FAIL  $name [$path] missing: ${missing[*]}" >&2
        rc=1
    fi
done

[ $rc -eq 0 ] && echo "PASS: the IGLOO delta is present in all ${#libs[@]} system libraries"
exit $rc
