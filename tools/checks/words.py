#!/usr/bin/env python3
"""The zero-ecosystem-words contract check.

The product's contract, CLI surface, tests and docs carry no vocabulary of any
particular ecosystem. The build configuration (mcpp.toml, mcpp.lock) and the
dependency declarations inside sources (``import mcpplibs.os``, namespace
aliases) may name what they build with, and the README may say it "is built
with the mcpp build" or show its commands (``mcpp build``, ``mcpp run``).
Everything else is a violation.

Exit status: 0 = clean, 1 = violations (printed one per line).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

BANNED = re.compile(r"mcpp|mcpplibs|openkal|xim(?![a-z])|xlings", re.IGNORECASE)

# Where a build's own name may appear: the files that configure the build.
CONFIG_FILES = {"mcpp.toml", "mcpp.lock", "versions.env"}

# Where a dependency may be named in a scanned file (a dependency declaration,
# not prose): the import/alias forms and the manifest's workspace references.
SOURCE_ALLOWANCE = re.compile(
    r"import\s+mcpplibs|mcpplibs::|\bos\.workspace\s*=|mcpplibs\.\w+\s*=.*workspace"
)

# Where the build's name may appear in prose: naming the build, or its commands.
PROSE_ALLOWANCE = re.compile(r"\bmcpp (build|run|test|clean|new|toolchain|self)\b|the mcpp build")

SCANNED_SUFFIXES = {".py", ".md", ".cppm", ".cpp", ".hpp", ".toml"}


def violations(root: Path) -> list[str]:
    found: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SCANNED_SUFFIXES:
            continue
        if any(part in {"target", ".git", ".pytest_cache", "__pycache__", "node_modules"}
               for part in path.parts):
            continue
        if path.name in CONFIG_FILES:
            continue  # the build's own configuration names the build
        if path.name == "words.py":
            continue  # this rule's own definition names what it bans
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for match in BANNED.finditer(line):
                window = line[max(0, match.start() - 40) : match.end() + 40]
                if SOURCE_ALLOWANCE.search(line) or PROSE_ALLOWANCE.search(line):
                    continue
                found.append(f"{path.relative_to(root)}:{number}: ...{window.strip()}...")
    return found


def main() -> int:
    root = Path(__file__).resolve().parent.parent.parent
    found = violations(root)
    for line in found:
        print(line)
    print(f"{'FAIL' if found else 'OK'}: {len(found)} violation(s)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
