#!/usr/bin/env python3
"""Report the licence of each upstream QEMU file, from the file itself.

QEMU is not one licence. Its own LICENSE file says the emulator "as a whole" is
GPL-2.0, but individual files are variously MIT, LGPL-2.1-or-later,
GPL-2.0-only and GPL-2.0-or-later, and a patch is a derivative of the files it
edits -- not of the tree's aggregate. So "the series is GPL-2.0" is a claim
about the wrong thing.

Detection order, strongest evidence first:
  1. SPDX-License-Identifier
  2. the MIT grant ("Permission is hereby granted, free of charge")
  3. LGPL, with or-later distinguished from only
  4. GPL, with or-later distinguished from only
  5. none -- no statement in the file; it inherits the tree default

`none` is reported, never guessed. A .h or a meson.build with no header is the
common case and saying "GPL" about it would be inventing evidence.
"""
import re
import sys
import json
import pathlib

SPDX = re.compile(r'SPDX-License-Identifier:\s*([A-Za-z0-9.+\-]+(?:\s+(?:OR|AND|WITH)\s+[A-Za-z0-9.+\-]+)*)')
MIT = re.compile(r'Permission is hereby granted,\s*free of charge', re.I)
LATER = re.compile(r'(or\s*\(?\s*at your option\s*\)?\s*any later version'
                   r'|version 2(?:\.1)?\s*(?:of the License)?\s*,?\s*or\s*(?:\(at your option\)\s*)?(?:any\s*)?later'
                   r'|either version 2(?:\.1)?[^.]{0,60}later)', re.I | re.S)
LGPL = re.compile(r'(Lesser General Public|Library General Public|\bLGPL\b)', re.I)
GPL = re.compile(r'(General Public License|\bGPL\b)', re.I)

HEAD_BYTES = 6000


def detect(path: pathlib.Path) -> str:
    try:
        head = path.read_text(errors="replace")[:HEAD_BYTES]
    except OSError:
        return "unreadable"

    m = SPDX.search(head)
    if m:
        return m.group(1).strip()
    if MIT.search(head):
        return "MIT"
    later = bool(LATER.search(head))
    if LGPL.search(head):
        return "LGPL-2.1-or-later" if later else "LGPL-2.1-only"
    if GPL.search(head):
        return "GPL-2.0-or-later" if later else "GPL-2.0-only"
    return "none"


def main() -> int:
    if len(sys.argv) < 3:
        sys.exit("usage: detect-file-license.py <tree> <file>... [--json]")
    as_json = "--json" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--json"]
    tree = pathlib.Path(args[0])
    out = {}
    for rel in args[1:]:
        p = tree / rel
        out[rel] = detect(p) if p.exists() else "absent"
    if as_json:
        print(json.dumps(out, indent=2, sort_keys=True))
    else:
        for k in sorted(out):
            print(f"{out[k]:22} {k}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
