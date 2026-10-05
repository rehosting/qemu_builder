# The link gate: does fastsnap still link when there is no TCG?
#
# This exists because of one specific failure, and the shape of it is worth
# stating because the fix is easy and the detection was not.
# fastsnap_ram_restore() calls tb_invalidate_phys_range() to kill translated
# blocks covering restored pages. That symbol is TCG's, and it has no stub. In
# a build with CONFIG_TCG it links; in a build without, the call has nothing to
# resolve against and the library fails to link. The idiom that fixes it --
# guarding the call with tcg_enabled(), which folds to a compile-time false --
# is what system/physmem.c already does.
#
# The reason it needed a gate rather than a code review: fastsnap-selftest.nix
# builds ONE target, aarch64-softmmu, with TCG. Every check in this repo passed
# while libqemu-kvm-x86_64.so could not be produced at all, and the first thing
# that noticed was the penguin image build -- downstream, in another repo, at
# the end of a much longer feedback loop.
#
# WHAT THIS DOES NOT CLAIM. It links; it does not run. Nothing here says the
# reset is correct under KVM, and there are concrete reasons to doubt it until
# measured: the dirty bitmap comes from the kernel rather than from TCG's
# TLB_NOTDIRTY path, and the fork oracle forks a process holding KVM vcpu file
# descriptors. Those are runtime questions and this is a compile-and-link
# check. It is scoped to the failure it was built for.
{
  lib,
  stdenv,
  src,
  pkg-config,
  ninja,
  python3,
  perl,
  git,
  glib,
  pixman,
  zlib,
  dtc,
}:

let
  pythonForBuild = python3.withPackages (ps: [
    ps.setuptools
    ps.wheel
    ps.pip
  ]);
in
stdenv.mkDerivation {
  pname = "fastsnap-kvm-link";
  version = "1";
  inherit src;

  nativeBuildInputs = [
    pkg-config
    ninja
    pythonForBuild
    perl
    git
  ];
  buildInputs = [
    glib
    pixman
    zlib
    dtc
  ];

  unpackPhase = ''
    runHook preUnpack
    cp -r ${src} qemu-src
    chmod -R u+w qemu-src
    cd qemu-src
    runHook postUnpack
  '';

  # --disable-tcg is the whole point. --enable-kvm without it would still
  # compile CONFIG_TCG in, tcg_enabled() would not fold, and the unguarded call
  # that broke the image build would link here perfectly well.
  configurePhase = ''
    runHook preConfigure
    patchShebangs scripts configure
    mkdir -p build
    cd build
    ../configure \
      --target-list=x86_64-softmmu \
      --disable-tcg \
      --enable-kvm \
      --disable-modules \
      --extra-cflags=-fPIC \
      --disable-docs \
      --disable-tools \
      --disable-guest-agent \
      --disable-vnc \
      --disable-gtk \
      --disable-sdl \
      --disable-curses \
      --disable-slirp \
      --disable-werror
    runHook postConfigure
  '';

  # --disable-modules AND --extra-cflags=-fPIC are load-bearing, and finding
  # that out is why this file is worth having. Without them the .so links and
  # exports NOTHING of the fastsnap ABI: QEMU's module support builds with
  # hidden visibility behind an explicit export list, and penguin_fastsnap_* is
  # not on it. The shipped artifact passes both (configs/default.json), so it
  # exports all 26 -- verified directly against
  # /usr/local/lib/libqemu-kvm-x86_64.so in the penguin image.
  #
  # That is the standing hazard with this file: it hand-rolls a configure
  # rather than reusing configs/default.json's configureArgs the way build.sh
  # does, so it can diverge from the shipped configuration and then fail (or
  # worse, pass) for reasons that have nothing to do with fastsnap. Reusing the
  # profile here needs the profile's nixDeps in buildInputs; until then, these
  # two flags are the ones that matter and this comment is the reason.

  # THE SHARED LIBRARY, not the executable, and the difference is the whole
  # check. The first version of this file ninja'd qemu-system-x86_64 and then
  # failed its own symbol gate -- correctly. fastsnap's objects land in
  # libsystem.a, nothing inside QEMU references penguin_fastsnap_*, and a
  # static archive contributes only the objects that resolve an undefined
  # symbol, so the ABI is absent from any executable by construction. It is
  # present in the .so because that is linked whole and exported, and the .so
  # is what penguin dlopens. build.sh names this exact target.
  #
  # Written, wired into the flake, and never run for two weeks; the first run
  # failed. That is the argument for running a check before believing it.
  buildPhase = ''
    runHook preBuild
    ninja libqemu-kvm-x86_64.so
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    lib=libqemu-kvm-x86_64.so
    test -f "$lib" || { echo "FAIL: no $lib" >&2; exit 1; }
    # Linking is the assertion, but assert the fastsnap ABI is actually IN the
    # library too: a configuration that quietly dropped src/fastsnap from the
    # build would link just as happily and prove nothing.
    #
    # nm -D: the DYNAMIC table. Penguin reaches these through dlsym, so a
    # symbol present only in .symtab is not reachable and a gate that accepted
    # one would pass a library the loader cannot use.
    for sym in penguin_fastsnap_schedule penguin_fastsnap_set_allowlist \
               penguin_fastsnap_dev_diff_sections; do
      # awk to the last field + grep -qx, the same shape check-delta-present.sh
      # uses. The obvious end-of-line anchored grep does NOT work here: inside
      # a nix indented-string literal the backslash survives into the shell,
      # double quotes then turn the escaped dollar into a literal one, and the
      # anchor becomes a dollar CHARACTER. The check failed twice on a library
      # that exports all 26 symbols, with the diagnostics printing
      # "penguin_fastsnap in .dynsym: 26" directly underneath the FAIL.
      # (Writing that explanation with the quotes spelled out closed the
      # string literal and broke the file, which is the same joke twice.)
      nm -D --defined-only "$lib" |
        awk -v s="$sym" '{n=$NF; sub(/@.*/, "", n); if (n == s) f=1}
                         END {exit !f}' || {
        echo "FAIL: $sym is not exported from a --disable-tcg $lib." >&2
        # Say WHICH of the three things went wrong, because they have three
        # different fixes and a bare FAIL cost two rebuilds to narrow down.
        echo "  penguin_fastsnap in .dynsym: $(nm -D --defined-only "$lib" \
              2>/dev/null | grep -c penguin_fastsnap)" >&2
        echo "  penguin_fastsnap in .symtab: $(nm --defined-only "$lib" \
              2>/dev/null | grep -c penguin_fastsnap)" >&2
        echo "  total .dynsym entries: $(nm -D --defined-only "$lib" \
              2>/dev/null | wc -l)" >&2
        echo "  what nm actually calls them:" >&2
        nm -D --defined-only "$lib" | grep penguin_fastsnap | head -4 >&2 || true
        echo "  version script / dynamic list on the link line:" >&2
        grep -o -- '-Wl,--\(version-script\|dynamic-list\)[^ ]*' \
             build.ninja 2>/dev/null | sort -u | head -5 >&2 || true
        echo "  NOTE: the SHIPPED libqemu-kvm-x86_64.so exports all 26, so a" >&2
        echo "  failure here is this check diverging from build.sh's" >&2
        echo "  configure, not fastsnap being unavailable under KVM." >&2
        exit 1
      }
    done
    echo "PASS: fastsnap links into $lib and is exported with CONFIG_TCG off"
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    echo ok > "$out/linked"
    runHook postInstall
  '';

  meta = {
    description = "fastsnap links in a KVM-only, TCG-less QEMU build";
    license = lib.licenses.gpl2Plus;
  };
}
