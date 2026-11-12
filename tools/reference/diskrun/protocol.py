"""Marker protocol: wraps a guest command so its stdout and exit status can be
extracted from the serial console stream.

Layout on the wire (one line, POSIX sh)::

    stty -echo; echo @@BEGIN@@; env 'K=V' ... -- cmd arg ...; rc=$?; echo @@STATUS=$rc@@

Bytes between the BEGIN marker and the STATUS marker are the program stdout
(normalised CRLF/CR -> LF).  Nothing else in the protocol is guest-specific.
"""

from __future__ import annotations

import re

BEGIN = "@@BEGIN@@"
BEGIN_B = BEGIN.encode()
STATUS_RE = re.compile(rb"@@STATUS=(\d+)@@")


def escape_sharg(value: str) -> str:
    """Single-quote an argument for POSIX sh; safe for any byte content."""
    return "'" + value.replace("'", "'\\''") + "'"


def wrap_command(argv: list[str], env: dict[str, str] | None = None) -> str:
    """Build the marker-wrapped command line sent to the guest shell."""
    parts = ["stty -echo", "echo " + BEGIN]
    if env:
        parts.append("env " + " ".join(escape_sharg(f"{k}={v}") for k, v in env.items()))
    parts.extend(escape_sharg(a) for a in argv)
    parts.append("rc=$?")
    parts.append("echo @@STATUS=$rc@@")
    return "; ".join(parts)


def normalize_output(data: bytes) -> bytes:
    """Normalise serial line endings: CRLF and lone CR become LF."""
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def split_output(chunk: bytes) -> tuple[bytes, int | None]:
    """Split the bytes consumed up to and including a STATUS marker.

    Returns ``(stdout_bytes_normalised, rc)``.  ``rc`` is ``None`` when no
    complete STATUS marker is present in ``chunk``.
    """
    m = STATUS_RE.search(chunk)
    if m is None:
        return normalize_output(chunk), None
    head = chunk[: m.start()]
    # Drop the BEGIN marker and everything before it (command echo etc.).
    idx = head.find(BEGIN_B)
    if idx != -1:
        head = head[idx + len(BEGIN_B):]
    head = head.lstrip(b"\r\n")
    return normalize_output(head.rstrip(b"\r\n")), int(m.group(1))
