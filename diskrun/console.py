"""Console driving: expect/send primitives over a byte channel (pty or mock).

The *default* login script below is just one policy: a login-style interactive
image (login: -> root -> Password: -> shell prompt).  Any system can supply a
completely different script via ``--expect FILE.json`` (array of
``{"expect": ..., "send": ..., "timeout": ...}``); the console driver itself
knows nothing about any specific guest.
"""

from __future__ import annotations

import os
import re
import select
import subprocess
import time

from .protocol import BEGIN_B, STATUS_RE

# Default login-style bootstrap script (replaceable per system via --expect).
DEFAULT_LOGIN_SCRIPT: list[dict] = [
    {"expect": r"login:", "send": "root"},
    {"expect": r"Password:", "send": "root"},
    {"expect": r"[#$] $", "send": ""},
]


class ConsoleError(Exception):
    """Console channel broke (EOF) or a pattern never matched."""


class PtyDriver:
    """Drives a child process whose stdio is a serial console on a pty."""

    SEND_DELAY = 0.02  # throttle: pause between output chunks

    def __init__(self, argv: list[str], env: dict[str, str] | None = None):
        import pty

        self._buffer = b""
        try:
            master, slave = pty.openpty()
        except OSError as e:  # no pty available
            raise ConsoleError(f"cannot allocate pty: {e}") from e
        try:
            self.proc = subprocess.Popen(
                argv,
                stdin=slave,
                stdout=slave,
                stderr=slave,
                env=env,
                close_fds=True,
            )
        except OSError as e:
            os.close(master)
            os.close(slave)
            raise
        finally:
            os.close(slave)
        self.master = master

    # -- low level ---------------------------------------------------------
    def _read_more(self, timeout: float) -> bool:
        """Read once; return True if bytes arrived. False on EOF/timeout."""
        if self.proc.poll() is not None:
            return False
        try:
            r, _, _ = select.select([self.master], [], [], max(0.0, timeout))
        except (OSError, ValueError):
            return False
        if not r:
            return False
        try:
            data = os.read(self.master, 4096)
        except OSError:
            return False
        if not data:
            return False
        self._buffer += data
        return True

    def read_until(self, pattern: bytes | str, timeout: float) -> bytes:
        """Accumulate until ``pattern`` (bytes or regex source) matches.

        Returns all bytes consumed up to and including the match.
        Raises ConsoleError on EOF or timeout without a match.
        """
        if isinstance(pattern, str):
            pattern = pattern.encode()
        rx = re.compile(pattern, re.DOTALL)
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            m = rx.search(self._buffer)
            if m is not None:
                consumed = self._buffer[: m.end()]
                self._buffer = self._buffer[m.end():]
                return consumed
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self._read_more(remaining):
                # One final chance: flush pending output even if child died.
                m = rx.search(self._buffer)
                if m is not None:
                    consumed = self._buffer[: m.end()]
                    self._buffer = self._buffer[m.end():]
                    return consumed
                raise ConsoleError(f"pattern {pattern!r} not seen before timeout")

    def send(self, line: str, throttle: bool = True) -> None:
        """Write a line to the console; chunked with a small delay (throttle)."""
        data = (line + "\n").encode()
        try:
            for i in range(0, len(data), 64):
                os.write(self.master, data[i : i + 64])
                if throttle and i + 64 < len(data):
                    time.sleep(self.SEND_DELAY)
        except OSError as e:
            raise ConsoleError(f"console write failed: {e}") from e

    def close(self) -> None:
        try:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
        finally:
            try:
                os.close(self.master)
            except OSError:
                pass


class ExpectMachine:
    """Runs a sequence of {expect, send, timeout} steps against a driver."""

    def __init__(self, steps: list[dict]):
        self.steps = []
        for step in steps:
            self.steps.append(
                {
                    "expect": step["expect"],
                    "send": step.get("send", ""),
                    "timeout": float(step.get("timeout", 60.0)),
                }
            )

    def run(self, driver) -> None:
        for step in self.steps:
            driver.read_until(step["expect"], step["timeout"])
            if step["send"]:
                time.sleep(0.1)
                driver.send(step["send"])


def load_script(path: str | None) -> list[dict]:
    """Load the expect script: JSON array from ``path`` or the default one."""
    import json

    if path is None:
        return list(DEFAULT_LOGIN_SCRIPT)
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, list) or not all(isinstance(s, dict) and "expect" in s for s in data):
        raise ValueError("expect script must be a JSON array of {expect, send, timeout} objects")
    return data


def run_marker_command(driver, command_line: str, timeout: float) -> tuple[bytes, int]:
    """Send the wrapped command, capture stdout between markers and the rc.

    The ``stty -echo`` prelude goes out first as its own line so the tty stops
    echoing before the marker line arrives (otherwise the command echo would
    land between BEGIN and the program output).  Raises ConsoleError if either
    marker is not seen within ``timeout``.
    """
    driver.send("stty -echo")
    time.sleep(0.2)
    driver.send(command_line)
    deadline = time.monotonic() + max(0.0, timeout)

    def remaining() -> float:
        return deadline - time.monotonic()

    driver.read_until(BEGIN_B, remaining())
    consumed = driver.read_until(STATUS_RE, remaining())
    from .protocol import split_output

    out, rc = split_output(consumed)
    if rc is None:  # pragma: no cover - read_until(STATUS_RE) guarantees a match
        raise ConsoleError("status marker missing")
    return out, rc
