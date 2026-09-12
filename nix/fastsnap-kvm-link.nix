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

  buildPhase = ''
    runHook preBuild
    ninja qemu-system-x86_64
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    test -x qemu-system-x86_64 || { echo "FAIL: no binary" >&2; exit 1; }
    # Linking is the assertion, but assert the fastsnap ABI is actually IN the
    # binary too: a configuration that quietly dropped src/fastsnap from the
    # build would link just as happily and prove nothing.
    for sym in penguin_fastsnap_schedule penguin_fastsnap_set_allowlist \
               penguin_fastsnap_dev_diff_sections; do
      nm -C qemu-system-x86_64 | grep -q " $sym\$" || {
        echo "FAIL: $sym is not in a --disable-tcg build; this gate would" >&2
        echo "      pass on a build that does not contain fastsnap at all" >&2
        exit 1
      }
    done
    echo "PASS: fastsnap links and is present with CONFIG_TCG off"
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
