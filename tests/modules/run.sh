#!/usr/bin/env bash
# tests/modules/run.sh BUILD SDK [OUT]: panda modules on an aarch64 engine build (no PANDA needed).
#  1. -device from a module: ext-counter on -M virt; the guest reads its ID and count.
#  2. the vmstate guard: a module device without vmstate on the configurable machine warns.
#  3. the ABI check: a module built against a different hash is refused; PANDA_MODULE_ABI=ignore
#     loads it with a warning.
#  4. errors: not a panda module; -M on a module with no machine type; loading a path twice.
#  5. -module help prints the SDK's hash.
# Prints PASS/FAIL per check; exit status 1 if any failed.
set -u
BUILD=$(cd "$1" && pwd); SDK=$(cd "$2" && pwd); OUT=${3:-$(mktemp -d)}; mkdir -p "$OUT"
H=$(cd "$(dirname "$0")" && pwd)
Q=$BUILD/qemu-system-aarch64
fail=0
ok() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

"$SDK/bin/panda-board-cc" aarch64-softmmu -o "$OUT/ext-devices.so" "$H/ext-devices.c"
as "$H/counter.S" -o "$OUT/counter.o" && ld -Ttext=0x40080000 -e _start "$OUT/counter.o" -o "$OUT/counter.elf"
run() { timeout 20 "$Q" -display none -monitor none -semihosting "$@"; }

run -module "$OUT/ext-devices.so" -M virt -cpu cortex-a53 -m 64M -device ext-counter,addr=0xb000000 \
    -kernel "$OUT/counter.elf" -serial stdio > "$OUT/1.out" 2>&1
ok "-device ext-counter from a module (guest reads ID and 3 writes)" \
   'grep -q "ID=434e5452 COUNT=00000003" "$OUT/1.out"'

run -module "$OUT/ext-devices.so" -M configurable,ram-base=0x40000000,device.0.type=pl011,device.0.mmio.0=0x9000000,device.0.prop.chardev=serial0,device.1.type=ext-nostate,device.1.id=nostate,device.1.mmio.0=0xb000000 \
    -cpu cortex-a53 -m 64M -kernel "$OUT/counter.elf" -serial stdio > "$OUT/2.out" 2>&1
ok "vmstate guard warns about a module device without vmstate" \
   'grep -q "device /machine/peripheral/nostate (ext-nostate, from .*ext-devices.so) has no vmstate" "$OUT/2.out"'
ok "... and not about ext-counter (it has vmstate)" '! grep -q "ext-counter.*no vmstate" "$OUT/2.out"'

rm -rf "$OUT/sdk-other"; cp -r "$SDK" "$OUT/sdk-other"
sed -i 's/"\([0-9a-f]\{8\}\)/"deadbeef/' "$OUT/sdk-other/src/build/panda-abi.h"
"$OUT/sdk-other/bin/panda-board-cc" aarch64-softmmu -o "$OUT/ext-other.so" "$H/ext-devices.c"
run -module "$OUT/ext-other.so" -M virt -cpu cortex-a53 -kernel "$OUT/counter.elf" -serial none > "$OUT/3.out" 2>&1
ok "a module built against another ABI hash is refused" \
   'grep -q "built against panda ABI deadbeef.*this engine is" "$OUT/3.out" && grep -q "Rebuild it with this engine" "$OUT/3.out"'
PANDA_MODULE_ABI=ignore run -module "$OUT/ext-other.so" -M virt -cpu cortex-a53 -m 64M \
    -device ext-counter,addr=0xb000000 -kernel "$OUT/counter.elf" -serial stdio > "$OUT/3b.out" 2>&1
ok "PANDA_MODULE_ABI=ignore loads it with a warning" \
   'grep -q "loading anyway" "$OUT/3b.out" && grep -q "COUNT=00000003" "$OUT/3b.out"'

z=$(ldd "$Q" | awk '$1 ~ /^libz\.so/ {print $3}')
run -module "$z" -M virt > "$OUT/4.out" 2>&1
ok "not a panda module" 'grep -q "is not a panda module" "$OUT/4.out"'
run -M "$OUT/ext-devices.so" > "$OUT/4b.out" 2>&1
ok "-M on a module with no machine type" 'grep -q "pick one with PATH:NAME; it registered: no machine types" "$OUT/4b.out"'
run -module "$OUT/ext-devices.so" -module "$OUT/ext-devices.so" -M virt -cpu cortex-a53 -m 64M \
    -device ext-counter,addr=0xb000000 -kernel "$OUT/counter.elf" -serial stdio > "$OUT/4c.out" 2>&1
ok "the same module twice is loaded once" 'grep -q "COUNT=00000003" "$OUT/4c.out"'

h=$("$Q" -module help | awk '{print $3}')
ok "-module help prints the SDK's ABI hash" '[ "$h" = "$(python3 -c "import json;print(json.load(open(\"$SDK/panda-sdk.json\"))[\"panda_abi\"])")" ]'
exit $fail
