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
travel inside the patch and cannot drift away from it:

    License: <SPDX expression>
    Origin:  authored-by-igloo | <url>@<sha>

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

TRAILER = re.compile(r'^(License|Origin):\s*(.+?)\s*$', re.M)
TOUCHES = re.compile(r'^\+\+\+ b/(.+)$', re.M)

ORIGIN_OK = re.compile(r'^(authored-by-igloo|https?://\S+@[0-9a-f]{7,40})$')


def series(version):
    path = ROOT / "patches" / version / "series"
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            yield ROOT / "patches" / line


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

    rc = 0
    width = max(len(p.name) for p in patches)
    for p in patches:
        n = p.name
        want = combine([lic[f] for f in touched[n]])
        got = declared[n].get("License")
        origin = declared[n].get("Origin")
        problems = []
        if got is None:
            problems.append("no License: trailer")
        elif got != want:
            problems.append(f"declares {got}, files require {want}")
        if origin is None:
            problems.append("no Origin: trailer")
        elif not ORIGIN_OK.match(origin):
            problems.append(f"Origin {origin!r} is neither authored-by-igloo nor <url>@<sha>")
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
            print(f"  ok    {n[:width]}  {got}{note}")

    if rc == 0:
        print(f"PASS: all {len(patches)} patches declare the licence their files require")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
