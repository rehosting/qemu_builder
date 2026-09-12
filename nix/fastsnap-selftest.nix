# The behavioural gate: does a device snapshot actually round-trip on this
# build of QEMU?
#
# Everything else in this repo's CI is static. `verify-series.sh` proves the
# patches apply and hash right; `check-delta-present.sh` proves the symbols
# landed in the libraries. Neither can tell a working device_restore_all() from
# one that silently restores nothing -- and that failure mode is invisible,
# because a no-op restore and a correct restore produce the same "it ran"
# signal. See src/fastsnap/selftest.c for the control that separates them.
#
# This builds ONE target (aarch64-softmmu) rather than the shipped fourteen,
# and a real qemu-system-aarch64 binary rather than the embedding libraries, so
# there is something to execute. That is a second QEMU compile, which is why it
# is a check and not part of the release build -- but a single target with
# features off is a small fraction of the full matrix.
#
# It is deliberately NOT the boot gate the README still owes. A boot gate
# proves the machine comes up; this proves one specific mechanism moves state.
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
  libslirp,
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
  pname = "fastsnap-selftest";
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
    libslirp
    dtc
  ];

  unpackPhase = ''
    runHook preUnpack
    cp -r ${src} qemu-src
    chmod -R u+w qemu-src
    cd qemu-src
    runHook postUnpack
  '';

  configurePhase = ''
    runHook preConfigure
    patchShebangs scripts configure
    mkdir -p build
    cd build
    ../configure \
      --target-list=aarch64-softmmu \
      --disable-docs \
      --disable-tools \
      --disable-guest-agent \
      --disable-vnc \
      --disable-gtk \
      --disable-sdl \
      --disable-curses \
      --disable-werror
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    ninja qemu-system-aarch64
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck

    echo ">>> fastsnap device-snapshot round trip"
    # NOT -S. Phase 1 runs at machine-init-done, but phase 2 asserts that the
    # restore never enters RUN_STATE_RESTORE_VM -- and vm_stop() on an
    # already-stopped VM returns without notifying change-state handlers, so
    # under -S that assertion cannot fire and passes even when a
    # vm_stop(RUN_STATE_RESTORE_VM) is deliberately injected. It was inert
    # exactly once, and its own negative control is what caught it.
    FASTSNAP_SELFTEST=1 ./qemu-system-aarch64 \
      -M virt -cpu cortex-a57 -m 128 -display none 2>&1 | tee selftest.log

    # tee eats the exit status, so assert on the verdict line. SKIPPED must not
    # pass here: on -M virt the PL011 exists, so a SKIPPED verdict means the
    # section list came back wrong, which is a failure of this gate.
    grep -q '^fastsnap: SELFTEST PASSED$' selftest.log || {
        echo "FAIL: no PASSED verdict; see above" >&2; exit 1; }
    grep -q '^fastsnap: control OK' selftest.log || {
        echo "FAIL: the positive control did not fire, so the verdict above " \
             "is not evidence of anything" >&2; exit 1; }
    # Phase 2 must have actually run. If the binary reverts to a build where it
    # does not, the PASSED line alone would not notice.
    grep -q '^fastsnap: control OK - state digest sees a RAM change$' selftest.log || {
      echo "the state-digest control did not pass; a digest that cannot see a"
      echo "RAM change makes every oracle comparison built on it meaningless"
      exit 1
    }
    grep -q '^fastsnap: state digest is reproducible$' selftest.log || {
      echo "state digest is not a pure function of guest state"
      exit 1
    }
    # The dirty-page counter. Three separate controls, and the third is the
    # one that matters for using it in a loop: a tracker that measures the
    # first interval and silently under-counts every one after it passes the
    # other two. See phase 5 in src/fastsnap/selftest.c.
    grep -q '^fastsnap: control OK - 8 poked pages' selftest.log || {
      echo "the dirty-page counter did not see 8 known writes, or did not name"
      echo "the pages it saw. A dirty-set number from it would be unfalsifiable."
      exit 1
    }
    grep -q '^fastsnap: control OK - dirty tracking re-arms' selftest.log || {
      echo "dirty tracking does not re-arm after a count, so only the first"
      echo "interval is measured and every later one under-counts"
      exit 1
    }
    grep -q '^fastsnap: dirty set returns to baseline' selftest.log || {
      echo "the dirty set is not consumed by a count, so an interval's number"
      echo "is not that interval's"
      exit 1
    }
    # The complete reset, judged by an oracle that shares no code with it:
    # the reference is a forked child read back with process_vm_readv(), the
    # restore reads an in-process copy. Neither can launder the other.
    grep -q '^fastsnap: LOOP OK' selftest.log || {
      echo "a complete reset did not return the guest to the forked reference."
      echo "Restoring devices without RAM rewinds the CPU page-table base into"
      echo "RAM that was never rewound; on real firmware that kills the guest."
      exit 1
    }
    # The form a running guest needs: reset and oracle in ONE bottom half.
    # Split across two, the guest executes in the gap and a correct reset
    # reports hundreds of differing pages -- the oracle would be usable only on
    # a machine that is not running.
    grep -q '^fastsnap: VERIFY OK' selftest.log || {
      echo "reset+verify did not pass. Either it compared less than the whole"
      echo "snapshot, or it charged the oracle's tens of milliseconds to the"
      echo "reset -- which would make every verified reset number wrong by two"
      echo "orders of magnitude, and look plausible while doing it."
      exit 1
    }
    grep -q 'no RUN_STATE_RESTORE_VM transition' selftest.log || {
        echo "FAIL: the no-tb_flush assertion did not run" >&2; exit 1; }
    grep -q '^fastsnap: scheduled restore ' selftest.log || {
        echo "FAIL: the scheduled path did not run" >&2; exit 1; }

    echo "PASS: device state round-tripped, and the control proves the "
    echo "      instrument could have seen a difference."
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    cp selftest.log "$out/selftest.log"
    # TEMPORARY (profiling lane): keep the binary so the profile harness can be
    # driven from the host with env vars instead of a rebuild per experiment.
    mkdir -p "$out/bin" "$out/share"
    cp qemu-system-aarch64 "$out/bin/"
    cp -r ../pc-bios "$out/share/qemu"
    runHook postInstall
  '';

  meta = {
    description = "Behavioural gate for fastsnap's device-state round trip";
    license = lib.licenses.gpl2Plus;
  };
}
