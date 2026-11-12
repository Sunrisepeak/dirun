import json

import pytest

from diskrun.console import (
    ConsoleError,
    ExpectMachine,
    PtyDriver,
    load_script,
    run_marker_command,
)
from diskrun.protocol import wrap_command


class FakeDriver:
    """Replays a scripted byte stream; mimics PtyDriver.read_until/send."""

    def __init__(self, chunks: list[bytes]):
        self.chunks = list(chunks)
        self.buffer = b""
        self.sent: list[str] = []

    def _refill(self) -> bool:
        if self.chunks:
            self.buffer += self.chunks.pop(0)
            return True
        return False

    def read_until(self, pattern, timeout):
        import re

        if hasattr(pattern, "search"):
            rx = pattern
        else:
            rx = re.compile(pattern if isinstance(pattern, bytes) else pattern.encode(), re.DOTALL)
        while True:
            m = rx.search(self.buffer)
            if m:
                consumed = self.buffer[: m.end()]
                self.buffer = self.buffer[m.end():]
                return consumed
            if not self._refill():
                raise ConsoleError(f"pattern {pattern!r} not seen before timeout")

    def send(self, line, throttle=True):
        self.sent.append(line)

    def close(self):
        pass


class TestExpectMachine:
    def test_login_flow_default_script(self):
        driver = FakeDriver([b"Welcome\r\n", b"box login: ", b"Password: ", b"# "])
        ExpectMachine(load_script(None)).run(driver)
        assert driver.sent == ["root", "root"]

    def test_custom_script_order(self):
        steps = [{"expect": "user:", "send": "alice"}, {"expect": ">", "send": "run"}]
        driver = FakeDriver([b"user: ", b"> "])
        ExpectMachine(steps).run(driver)
        assert driver.sent == ["alice", "run"]

    def test_missing_pattern_raises(self):
        steps = [{"expect": "never", "send": "x", "timeout": 0.5}]
        driver = FakeDriver([b"garbage"])
        with pytest.raises(ConsoleError):
            ExpectMachine(steps).run(driver)

    def test_load_script_json(self, tmp_path):
        p = tmp_path / "s.json"
        p.write_text(json.dumps([{"expect": "a", "send": "b", "timeout": 3}]))
        steps = load_script(str(p))
        assert steps == [{"expect": "a", "send": "b", "timeout": 3}]

    def test_load_script_rejects_bad_shape(self, tmp_path):
        p = tmp_path / "bad.json"
        p.write_text('{"expect": "a"}')
        with pytest.raises(ValueError):
            load_script(str(p))


class TestMarkerProtocolOverDriver:
    def test_output_and_rc_roundtrip(self):
        stream = [
            b"stty -echo\r\n",              # prelude echo (before BEGIN)
            b"echo @@BEGIN@@\r\n",
            b"line1\r\nline2\r\n",           # program stdout (echo now off)
            b"@@STATUS=3@@\r\n# ",
        ]
        driver = FakeDriver(stream)
        out, rc = run_marker_command(driver, wrap_command(["./app"]), 5)
        assert rc == 3
        assert out == b"line1\nline2"
        assert driver.sent[0] == "stty -echo"

    def test_begin_marker_missing_raises(self):
        driver = FakeDriver([b"no markers here"])
        with pytest.raises(ConsoleError):
            run_marker_command(driver, wrap_command(["true"]), 0)

    def test_status_marker_missing_raises(self):
        driver = FakeDriver([b"@@BEGIN@@\r\noutput but no status\r\n"])
        with pytest.raises(ConsoleError):
            run_marker_command(driver, wrap_command(["true"]), 0)


class TestPtyDriver:
    def test_eof_stream_reports_console_error(self):
        driver = PtyDriver(["/bin/true"])
        try:
            with pytest.raises(ConsoleError):
                driver.read_until("anything", 2.0)
        finally:
            driver.close()

    def test_roundtrip_over_pty(self):
        driver = PtyDriver(["/bin/cat"])
        try:
            driver.send("ping", throttle=False)
            got = driver.read_until(b"ping", 5.0)
            assert got.endswith(b"ping")
        finally:
            driver.close()

    def test_send_throttle_chunks(self, monkeypatch):
        driver = PtyDriver(["/bin/cat"])
        try:
            sleeps = []
            import diskrun.console as console_mod

            monkeypatch.setattr(console_mod.time, "sleep", lambda s: sleeps.append(s))
            driver.send("x" * 200)  # 4 chunks -> 3 throttled pauses
            assert len(sleeps) == 3
        finally:
            driver.close()

    def test_read_after_master_close_reports_console_closed(self):
        """EBADF form: master fd closed under us -> ConsoleError, never raw OSError."""
        import os

        driver = PtyDriver(["/bin/sleep", "30"])
        try:
            os.close(driver.master)  # fd goes bad while the child still runs
            with pytest.raises(ConsoleError, match=r"console closed: Bad file descriptor"):
                driver.read_until(b"anything", 2.0)
        finally:
            driver.close()  # must tolerate the already-dead fd

    def test_eio_form_reports_console_closed(self, monkeypatch):
        """EIO form: what a Linux kernel hands back once the guest pty side dies."""
        import errno

        driver = PtyDriver(["/bin/cat"])
        try:
            def raise_eio(fd, n):
                raise OSError(errno.EIO, "Input/output error")

            monkeypatch.setattr("diskrun.console.os.read", raise_eio)
            driver.send("x", throttle=False)  # make the master readable first
            with pytest.raises(ConsoleError, match=r"console closed: Input/output error"):
                driver.read_until(b"anything", 2.0)
        finally:
            driver.close()

    def test_popen_failure_is_not_masked_by_double_close(self):
        """Spawn failure must surface as itself, not as a spurious EBADF."""
        missing = "/nonexistent/disk-run-probe-binary"
        with pytest.raises(FileNotFoundError):
            PtyDriver([missing])


