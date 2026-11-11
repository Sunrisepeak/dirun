"""CLI for disk-run."""

from __future__ import annotations

import argparse
import sys

from . import __version__
from .runner import (
    EXIT_NO_CONSOLE,
    EXIT_RUNNER_UNUSABLE,
    EXIT_TIMEOUT,
    RunnerError,
    run,
)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="disk-run",
        description="Boot a disk image and run a command inside it over the serial console.",
    )
    p.add_argument("--image", required=True, help="base disk image (never written; a temp qcow2 overlay is used)")
    p.add_argument("--machine", default=None, help="qemu machine type (default per architecture)")
    p.add_argument("--cpu", default=None, help="qemu cpu model")
    p.add_argument("--memory", type=int, default=512, metavar="MiB", help="guest memory in MiB (default 512)")
    p.add_argument("--smp", type=int, default=1, help="guest vcpu count (default 1)")
    p.add_argument("--accel", choices=["auto", "kvm", "tcg"], default="auto", help="acceleration (default auto)")
    p.add_argument("--timeout", type=float, default=300, metavar="SEC", help="overall wall-clock budget (default 300)")
    p.add_argument("--expect", default=None, metavar="FILE.json",
                   help="replace the default login script with a JSON array of {expect, send, timeout}")
    p.add_argument("--env", action="append", default=[], metavar="NAME=VAL",
                   help="pass NAME=VAL to the guest command (repeatable, explicit allowlist only)")
    p.add_argument("--file", action="append", default=[], metavar="host=guest",
                   help="deliver host file into a read-only FAT second disk at guest path (repeatable)")
    p.add_argument("command", nargs=argparse.REMAINDER, help="command to run (after --)")
    p.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required (use: disk-run ... -- command...)")

    env: dict[str, str] = {}
    for item in args.env:
        if "=" not in item:
            parser.error(f"--env expects NAME=VAL, got {item!r}")
        k, v = item.split("=", 1)
        env[k] = v

    files: list[tuple[str, str]] = []
    for item in args.file:
        if "=" not in item:
            parser.error(f"--file expects host=guest, got {item!r}")
        host, guest = item.split("=", 1)
        files.append((host, guest))

    try:
        rc, out = run(
            image=args.image,
            command=command,
            machine=args.machine,
            cpu=args.cpu,
            memory=args.memory,
            smp=args.smp,
            accel=args.accel,
            timeout=args.timeout,
            expect=args.expect,
            env=env,
            files=files,
        )
    except RunnerError as e:
        print(f"disk-run: {e}", file=sys.stderr)
        return e.exit_code
    sys.stdout.buffer.write(out)
    sys.stdout.buffer.flush()
    return rc


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
