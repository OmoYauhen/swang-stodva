#!/usr/bin/env python3
"""
Bump the version in version.mk.

  VERSION_STRING — user-visible SemVer (e.g. 0.0.1-alpha.3)
  VERSION_NUM    — monotonic integer for the DFU bootloader; +1 every release

Usage:
    bump-version.py --type prerelease [--label alpha] [--write]

--type:
    prerelease  0.0.1-alpha    -> 0.0.1-alpha.1 -> 0.0.1-alpha.2 ...
                (switching --label, e.g. alpha->beta, restarts at .1)
    finalize    0.0.1-alpha.3  -> 0.0.1          (drop the pre-release)
    patch       0.0.1(-x)      -> 0.0.2
    minor       0.1.3(-x)      -> 0.2.0
    major       1.4.2(-x)      -> 2.0.0

Prints "<new-version> <new-build>" to stdout. With --write, updates the file.
"""
import argparse
import re
import sys
from pathlib import Path

MAKEFILE = Path(__file__).resolve().parent.parent / "version.mk"
SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$")


def read_current():
    text = MAKEFILE.read_text()
    ver = re.search(r"^VERSION_STRING\s*:=\s*(.+?)\s*$", text, re.M)
    num = re.search(r"^VERSION_NUM\s*:=\s*(\d+)\s*$", text, re.M)
    if not ver or not num:
        sys.exit("error: could not find VERSION_STRING / VERSION_NUM in version.mk")
    return text, ver.group(1), int(num.group(1))


def bump(cur, typ, label):
    m = SEMVER_RE.match(cur)
    if not m:
        sys.exit(f"error: current VERSION_STRING '{cur}' is not valid SemVer")
    major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    pre = m.group(4)

    if typ == "major":
        return f"{major + 1}.0.0"
    if typ == "minor":
        return f"{major}.{minor + 1}.0"
    if typ == "patch":
        return f"{major}.{minor}.{patch + 1}"
    if typ == "finalize":
        if not pre:
            sys.exit(f"error: '{cur}' has no pre-release to finalize")
        return f"{major}.{minor}.{patch}"
    if typ == "prerelease":
        if pre:
            parts = pre.split(".")
            cur_label = parts[0]
            n = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
            if cur_label == label:
                return f"{major}.{minor}.{patch}-{label}.{n + 1}"
            return f"{major}.{minor}.{patch}-{label}.1"
        # promoting a final release into a new pre-release line
        return f"{major}.{minor}.{patch + 1}-{label}.1"
    sys.exit(f"error: unknown bump type '{typ}'")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type", required=True,
                    choices=["prerelease", "finalize", "patch", "minor", "major"])
    ap.add_argument("--label", default="alpha")
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    text, cur_ver, cur_num = read_current()
    new_ver = bump(cur_ver, args.type, args.label)
    new_num = cur_num + 1

    if args.write:
        text = re.sub(r"^VERSION_STRING\s*:=.*$",
                      f"VERSION_STRING := {new_ver}", text, count=1, flags=re.M)
        text = re.sub(r"^VERSION_NUM\s*:=.*$",
                      f"VERSION_NUM := {new_num}", text, count=1, flags=re.M)
        MAKEFILE.write_text(text)

    print(f"{new_ver} {new_num}")


# ---- self-test: python3 bump-version.py --selftest ----
def _selftest():
    cases = [
        ("0.0.1-alpha",   "prerelease", "alpha", "0.0.1-alpha.1"),
        ("0.0.1-alpha.1", "prerelease", "alpha", "0.0.1-alpha.2"),
        ("0.0.1-alpha.3", "prerelease", "beta",  "0.0.1-beta.1"),
        ("0.0.1-alpha.3", "finalize",   "alpha", "0.0.1"),
        ("0.0.1-alpha",   "patch",      "alpha", "0.0.2"),
        ("0.1.3",         "minor",      "alpha", "0.2.0"),
        ("1.4.2-rc.1",    "major",      "alpha", "2.0.0"),
        ("1.0.0",         "prerelease", "alpha", "1.0.1-alpha.1"),
    ]
    for cur, typ, label, want in cases:
        got = bump(cur, typ, label)
        assert got == want, f"bump({cur},{typ},{label}) = {got}, want {want}"
    print("all bump cases pass")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        _selftest()
    else:
        main()
