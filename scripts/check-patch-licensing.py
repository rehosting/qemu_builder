#!/usr/bin/env python3
"""Every patch declares its own licence, and the declaration is checked.

  ./scripts/check-patch-licensing.py 11.1.0 <pristine-qemu-tree>

WHY PER-PATCH. QEMU is not one licence. Its LICENSE file says the emulator "as a
whole" is GPL-2.0, and that is true of the aggregate -- but a patch is a
derivative of the specific files it edits, not of the aggregate. Measured across
the files this series touches:

    11  GPL-2.0-or-later     10  LGPL-2.1-or-later     5  MIT
     2  GPL-2.0-only          6  no statement in the file

So a repo-level "the patches are GPL-2.0" would be wrong about the MIT files,
needlessly restrictive about the LGPL ones, and would quietly hide that one
patch touches a GPL-2.0-ONLY file, which is the one place the "or later" option
is actually lost. Three patches here are pure MIT.

THE DECLARATION. Each patch's commit message carries two trailers, so they
travel inside the patch and cannot drift away from it across a git am /
format-patch round trip:

    License:   <SPDX expression>     what THIS patch is a derivative of
    Patch-Set: <set name>            which project's contribution it belongs to

PATCH SETS. A feature is rarely one patch, and a feature adopted from another
project needs its provenance stated once rather than copied onto every patch
that happens to belong to it. So the series is partitioned into named sets, each
described by patches/<version>/sets/<name>.json -- where the incoming project,
its commit, its licence and its provenance document live exactly once:

    { "name": "fastsnap",
      "origin": { "kind": "adopted", "project": "qemu-libafl-bridge",
                  "url": "...", "commit": "4df4d2dcfa...",
                  "license": "GPL-2.0-or-later",
                  "provenance": "src/fastsnap/PROVENANCE.md" },
      "srcPaths": ["src/fastsnap", "src/include/fastsnap"] }

Membership lives in the patch and description lives in the set file, which is
the split that survives editing: a patch cannot be renamed, split or reordered
out of its set without this noticing, and a set's provenance cannot be updated
in one place and go stale in thirteen others.

A set's patches must be CONTIGUOUS in the series. A project's contribution
staying in one run is what makes it reviewable, droppable and rebasable as a
unit -- and the moment it interleaves with someone else's, "which patches came
from where" becomes archaeology.

`srcPaths` is how a set covers the half of itself that is NOT a patch. fastsnap
is one series patch plus two directories of adopted source; naming both in one
place is what makes the set the feature rather than just its diff.

THE CHECK. This recomputes each patch's effective licence from the files it
touches and requires the declaration to match. Adding a file to a patch's
footprint therefore fails the gate if that file's licence changes the answer --
which is the drift this repo builds gates against everywhere else.

Combining rule, most permissive to least. A derivative of several files must be
offered under terms satisfying all of them:

    MIT  ->  LGPL-2.1-or-later  ->  GPL-2.0-or-later  ->  GPL-2.0-only

LGPL-2.1 section 3 permits conversion to GPL-2, which is why LGPL sits below
GPL. GPL-2.0-only is last because combining it with an "or later" work loses
the later-version option for the result. Files with no statement of their own
add no constraint and are reported as `inherit`.
"""
import json
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

# least to most restrictive; index is the rank used to combine
LATTICE = ["MIT", "LGPL-2.1-or-later", "GPL-2.0-or-later", "GPL-2.0-only"]
NO_CONSTRAINT = {"none"}

TRAILER = re.compile(r'^(License|Patch-Set):\s*(.+?)\s*$', re.M)
TOUCHES = re.compile(r'^\+\+\+ b/(.+)$', re.M)

SET_NAME = re.compile(r'^[a-z][a-z0-9-]*$')
SPDX_OK = re.compile(r'^[A-Za-z0-9.+-]+$')


def series(version):
    path = ROOT / "patches" / version / "series"
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            yield ROOT / "patches" / line


def load_sets(version):
    """Read patches/<version>/sets/*.json, validating each as we go."""
    d = ROOT / "patches" / version / "sets"
    sets, problems = {}, []
    if not d.is_dir():
        return sets, [f"no set directory at {d.relative_to(ROOT)}"]
    for f in sorted(d.glob("*.json")):
        try:
            spec = json.loads(f.read_text())
        except json.JSONDecodeError as e:
            problems.append(f"{f.name}: not valid JSON: {e}")
            continue
        name = spec.get("name")
        if name != f.stem:
            problems.append(f"{f.name}: name is {name!r}, should match the filename")
            continue
        if not SET_NAME.match(name):
            problems.append(f"{f.name}: set names are lowercase-with-dashes")
        if not spec.get("summary"):
            problems.append(f"{f.name}: no summary")
        origin = spec.get("origin") or {}
        kind = origin.get("kind")
        if kind not in ("authored", "adopted"):
            problems.append(f"{f.name}: origin.kind must be 'authored' or 'adopted'")
        elif kind == "adopted":
            # An adopted set is the whole reason this file exists: it must say
            # what was taken, from where, at which commit, and under what terms.
            for k in ("project", "url", "commit", "license", "provenance"):
                if not origin.get(k):
                    problems.append(f"{f.name}: adopted set needs origin.{k}")
            lic = origin.get("license")
            if lic and not SPDX_OK.match(lic):
                problems.append(f"{f.name}: origin.license {lic!r} is not an SPDX id")
            prov = origin.get("provenance")
            if prov and not (ROOT / prov).exists():
                problems.append(f"{f.name}: origin.provenance {prov} does not exist")
            commit = origin.get("commit", "")
            if commit and not re.fullmatch(r"[0-9a-f]{40}", commit):
                problems.append(f"{f.name}: origin.commit should be a full 40-char sha")
        for sp in spec.get("srcPaths", []):
            if not (ROOT / sp).exists():
                problems.append(f"{f.name}: srcPath {sp} does not exist")
        sets[name] = spec
    return sets, problems


def detect(tree, files):
    out = subprocess.run(
        [sys.executable, str(HERE / "detect-file-license.py"), str(tree), *files, "--json"],
        capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def combine(licences):
    """Effective licence of a work derived from files under `licences`."""
    constraining = [l for l in licences if l not in NO_CONSTRAINT]
    if not constraining:
        return "inherit"
    unknown = [l for l in constraining if l not in LATTICE]
    if unknown:
        return "UNKNOWN:" + ",".join(sorted(set(unknown)))
    return max(constraining, key=LATTICE.index)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.splitlines()[2].strip())
    version, tree = sys.argv[1], pathlib.Path(sys.argv[2])
    if not (tree / "migration" / "savevm.c").exists():
        sys.exit(f"not a QEMU tree: {tree}")

    patches = list(series(version))
    allfiles = set()
    touched = {}
    declared = {}
    for p in patches:
        text = p.read_text()
        touched[p.name] = sorted(set(TOUCHES.findall(text)))
        allfiles |= set(touched[p.name])
        # trailers live in the commit message, i.e. above the first diff
        msg = text.split("\ndiff --git ", 1)[0]
        declared[p.name] = dict(TRAILER.findall(msg))

    lic = detect(tree, sorted(allfiles))
    sets, set_problems = load_sets(version)

    rc = 0
    for msg in set_problems:
        rc = 1
        print(f"  FAIL  sets/  {msg}", file=sys.stderr)

    width = max(len(p.name) for p in patches)
    membership = []            # series order, for the contiguity check
    for p in patches:
        n = p.name
        want = combine([lic[f] for f in touched[n]])
        got = declared[n].get("License")
        pset = declared[n].get("Patch-Set")
        membership.append(pset)
        problems = []
        if got is None:
            problems.append("no License: trailer")
        elif got != want:
            problems.append(f"declares {got}, files require {want}")
        if pset is None:
            problems.append("no Patch-Set: trailer")
        elif pset not in sets:
            problems.append(f"Patch-Set {pset!r} has no patches/{version}/sets/{pset}.json")
        if want.startswith("UNKNOWN"):
            problems.append(f"undetectable file licence: {want}")

        if problems:
            rc = 1
            print(f"  FAIL  {n[:width]}  " + "; ".join(problems), file=sys.stderr)
            for f in touched[n]:
                print(f"          {lic[f]:22} {f}", file=sys.stderr)
        else:
            mixed = len({lic[f] for f in touched[n]} - NO_CONSTRAINT) > 1
            note = "  (combined from " + ", ".join(
                sorted({lic[f] for f in touched[n]})) + ")" if mixed else ""
            print(f"  ok    {n[:width]}  [{pset}] {got}{note}")

    # A set's patches must be one unbroken run. Interleaving is what turns
    # "which patches came from where" into archaeology, and it is what makes a
    # set impossible to drop or rebase as a unit.
    seen, runs = set(), []
    for m in membership:
        if not runs or runs[-1] != m:
            runs.append(m)
    for m in runs:
        if m in seen:
            rc = 1
            print(f"  FAIL  sets/  patch set {m!r} is not contiguous in the series; "
                  f"order is {' -> '.join(str(r) for r in runs)}", file=sys.stderr)
        seen.add(m)

    # An unused set file is a set that was dropped without its description going
    # with it -- the stale-metadata case this whole scheme exists to prevent.
    for name in sorted(sets):
        if name not in seen:
            rc = 1
            print(f"  FAIL  sets/{name}.json  described, but no patch claims it",
                  file=sys.stderr)

    if rc == 0:
        print()
        for name in [r for r in runs if r]:
            spec = sets[name]
            o = spec["origin"]
            n_in = sum(1 for m in membership if m == name)
            where = (f"{o['project']} @ {o['commit'][:12]} ({o['license']})"
                     if o["kind"] == "adopted" else "authored here")
            src = ", ".join(spec.get("srcPaths", [])) or "-"
            print(f"  set {name:12} {n_in:2} patches  {o['kind']:9} {where}")
            print(f"      {'':12} src: {src}")
        print(f"\nPASS: {len(patches)} patches in {len([r for r in runs if r])} "
              f"contiguous sets, each declaring the licence its files require")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
