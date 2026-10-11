#!/usr/bin/env python3
"""KEX four-view codemod for Notepad++.

Notepad++ applies most per-view settings with copy-pasted pairs of lines, e.g.

    _mainEditView.execute(SCI_SETCARETPERIOD, rate);
    _subEditView.execute(SCI_SETCARETPERIOD, rate);

For four views each such pair must also run for views 3 and 4. Instead of carrying those
~300 lines as a diff (which would conflict whenever upstream edits a neighbouring line),
this script finds every block of "main" lines that is immediately followed by the same
block for "sub", and appends the view-3 and view-4 copies. It is re-run on each new
Notepad++ release, so new pairs added upstream are picked up automatically.

Usage: codemod.py <notepad-plus-plus repo root> [--check]
Idempotent: blocks that already have their view-3 copy are skipped.
"""
import re
import sys
from pathlib import Path

FILES = [
    "PowerEditor/src/Notepad_plus.cpp",
    "PowerEditor/src/NppBigSwitch.cpp",
    "PowerEditor/src/NppCommands.cpp",
    "PowerEditor/src/NppNotification.cpp",
    "PowerEditor/src/NppIO.cpp",
]

# main token -> (sub token, view-3 token, view-4 token)
TOKENS = {
    "_mainEditView": ("_subEditView", "_view3EditView", "_view4EditView"),
    "_mainDocTab": ("_subDocTab", "_view3DocTab", "_view4DocTab"),
    "_autoCompleteMain": ("_autoCompleteSub", "_autoCompleteView3", "_autoCompleteView4"),
}
MAIN_RE = re.compile("|".join(map(re.escape, TOKENS)))
OTHER_MAIN_VIEW_TOKENS = re.compile(r"\bMAIN_VIEW\b|\bSUB_VIEW\b|isFromPrimary|isFromSecondary")


def subst(line: str, idx: int) -> str:
    return MAIN_RE.sub(lambda m: TOKENS[m.group(0)][idx], line)


def is_main_line(line: str) -> bool:
    # A "main" line uses a main token and nothing that already names the other view.
    if not MAIN_RE.search(line):
        return False
    if any(t[0] in line or t[1] in line or t[2] in line for t in TOKENS.values()):
        return False
    return not OTHER_MAIN_VIEW_TOKENS.search(line)


def transform(lines):
    out = []
    i, n, added = 0, len(lines), 0
    while i < n:
        matched = False
        for k in range(6, 0, -1):  # longest block first
            block = lines[i:i + k]
            if len(block) < k or not all(is_main_line(l) for l in block):
                continue
            for gap in (0, 1):
                j = i + k + gap
                if gap and (j - 1 >= n or lines[j - 1].strip() != ""):
                    continue
                sub_block = lines[j:j + k]
                if sub_block != [subst(l, 0) for l in block]:
                    continue
                # already transformed?
                after = lines[j + k + gap:j + 2 * k + gap]
                if after == [subst(l, 1) for l in block]:
                    continue
                out.extend(lines[i:j + k])
                if gap:
                    out.append(lines[j - 1])
                out.extend(subst(l, 1) for l in block)
                if gap:
                    out.append(lines[j - 1])
                out.extend(subst(l, 2) for l in block)
                added += 2 * k
                i = j + k
                matched = True
                break
            if matched:
                break
        if not matched:
            out.append(lines[i])
            i += 1
    return out, added


def main():
    root = Path(sys.argv[1])
    check = "--check" in sys.argv
    total = 0
    for rel in FILES:
        p = root / rel
        raw = p.read_bytes()
        text = raw.decode("utf-8")
        newline = "\r\n" if "\r\n" in text else "\n"
        lines = text.split(newline)
        new_lines, added = transform(lines)
        total += added
        print(f"{rel}: +{added} lines")
        if added and not check:
            p.write_bytes(newline.join(new_lines).encode("utf-8"))
    print(f"total: +{total} lines")


if __name__ == "__main__":
    main()
