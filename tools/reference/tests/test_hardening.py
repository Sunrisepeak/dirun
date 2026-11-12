import os
import signal

import pytest

from diskrun import runner
from diskrun.cli import main
from diskrun.runner import (
    EXIT_RUNNER_UNUSABLE,
    RunnerError,
    SignalExit,
    _build_files_disk_hdiutil,
    _build_files_disk_mtools,
    build_files_disk,
)


class _RunResult:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _which_map(present):
    def which(name):
        return "found" if name in present else None

    return which


class TestFilesDiskPaths:
    def test_mtools_path_linux(self, monkeypatch, tmp_path):
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd)
            return _RunResult()

        monkeypatch.setattr(runner.shutil, "which", _which_map({"mkfs.vfat", "mcopy"}))
        monkeypatch.setattr(runner.shutil, "copy2", lambda a, b: None)
        monkeypatch.setattr(runner.subprocess, "run", fake_run)
        host = tmp_path / "f.txt"
        host.write_bytes(b"x")
        out = build_files_disk([(str(host), "/g/f.txt")], str(tmp_path))
        assert out.endswith("files.vfat")
        assert calls[0][0] == "mkfs.vfat"
        assert calls[1][0] == "mcopy"

    def test_hdiutil_path_darwin(self, monkeypatch, tmp_path):
        calls = []
        mounts = []

        def fake_run(cmd, **kw):
            calls.append(cmd[0])
            if cmd[0] == "hdiutil" and cmd[1] == "attach":
                mounts.append(cmd[-2])
                os.makedirs(cmd[-2], exist_ok=True)  # simulate mountpoint appearing
            return _RunResult()

        copies = []
        # No mtools; hdiutil present -> Darwin fallback.
        monkeypatch.setattr(runner.shutil, "which", _which_map({"hdiutil"}))
        monkeypatch.setattr(runner.subprocess, "run", fake_run)
        monkeypatch.setattr(runner.os.path, "exists", lambda p: True)  # img created at exact path
        monkeypatch.setattr(runner.shutil, "copy2", lambda a, b: copies.append((a, b)))
        monkeypatch.setattr(runner.os, "makedirs", lambda p, exist_ok=True: None)
        host = tmp_path / "f.txt"
        host.write_bytes(b"x")
        out = build_files_disk([(str(host), "/d/f.txt")], str(tmp_path))
        assert calls[:3] == ["hdiutil", "hdiutil", "hdiutil"]  # create, attach, detach
        assert copies and copies[0][0] == str(host) and copies[0][1].endswith("d/f.txt")

    def test_both_missing_126_with_remedy(self, monkeypatch, tmp_path):
        monkeypatch.setattr(runner.shutil, "which", _which_map(set()))
        host = tmp_path / "f.txt"
        host.write_bytes(b"x")
        with pytest.raises(RunnerError) as e:
            build_files_disk([(str(host), "/f.txt")], str(tmp_path))
        assert e.value.exit_code == EXIT_RUNNER_UNUSABLE
        assert "brew install mtools" in e.value.remedy
        assert "hdiutil" in e.value.remedy


class TestSignalSafety:
    def _fake_env(self, monkeypatch, tmp_path, script_run):
        monkeypatch.setattr(runner.shutil, "which", _which_map({"qemu-img", "qemu-system-x86_64"}))
        monkeypatch.setattr(runner.subprocess, "run",
                            lambda *a, **k: _RunResult(stdout='{"format": "qcow2"}'))
        monkeypatch.setattr(runner.tempfile, "mkdtemp", lambda prefix="": str(tmp_path / "wd"))
        monkeypatch.setattr(runner.ExpectMachine, "run", script_run)
        closed = []

        class FakeDriver:
            def __init__(self, argv, env=None):
                pass

            def close(self):
                closed.append(True)

        monkeypatch.setattr(runner, "PtyDriver", FakeDriver)
        return closed

    def test_sigint_kills_driver_and_cleans_workdir(self, monkeypatch, tmp_path):
        os.makedirs(tmp_path / "wd", exist_ok=True)
        (tmp_path / "wd" / "overlay.qcow2").write_bytes(b"junk")

        def raise_sigint(self, driver):
            raise SignalExit(signal.SIGINT)

        closed = self._fake_env(monkeypatch, tmp_path, raise_sigint)
        img = tmp_path / "img.qcow2"
        img.write_bytes(b"")
        with pytest.raises(SignalExit) as e:
            runner.run(image=str(img), command=["true"], timeout=10)
        assert e.value.exit_code == 130
        assert closed == [True]                       # qemu group killed
        assert not (tmp_path / "wd").exists()          # no overlay residue

    def test_sigterm_maps_to_143(self, monkeypatch, tmp_path):
        os.makedirs(tmp_path / "wd", exist_ok=True)

        def raise_sigterm(self, driver):
            raise SignalExit(signal.SIGTERM)

        self._fake_env(monkeypatch, tmp_path, raise_sigterm)
        img = tmp_path / "img.qcow2"
        img.write_bytes(b"")
        with pytest.raises(SignalExit) as e:
            runner.run(image=str(img), command=["true"], timeout=10)
        assert e.value.exit_code == 143

    def test_cli_maps_signal_exit(self, monkeypatch, capsys):
        def fake_run(**kw):
            raise SignalExit(signal.SIGINT)

        monkeypatch.setattr("diskrun.cli.run", fake_run)
        assert main(["--image", "i", "--", "true"]) == 130


class TestActionableErrors:
    def test_remedy_printed_to_stderr(self, monkeypatch, capsys):
        def fake_run(**kw):
            raise RunnerError("timeout expired during boot script", 124,
                              remedy="increase --timeout to cover slow boots")

        monkeypatch.setattr("diskrun.cli.run", fake_run)
        rc = main(["--image", "i", "--", "true"])
        err = capsys.readouterr().err
        assert rc == 124
        assert "To fix: increase --timeout" in err

    def test_default_remedies_registered(self):
        assert "expect" in runner.REMEDIES[125]
        assert runner.REMEDIES[124].startswith("increase --timeout")


class TestPtyProcessGroup:
    def test_qemu_spawned_in_own_session(self, monkeypatch):
        """The driver must isolate qemu in its own process group for signal safety."""
        import subprocess

        spawned = {}

        class FakeProc:
            pid = 4242

            def poll(self):
                return None

            def wait(self, timeout=None):
                return 0

            def terminate(self):
                pass

            def kill(self):
                pass

        import diskrun.console as console_mod

        real_popen = subprocess.Popen

        def spy_popen(argv, **kw):
            spawned.update(kw)
            return FakeProc()

        import pty as pty_mod

        monkeypatch.setattr(subprocess, "Popen", spy_popen)
        driver = console_mod.PtyDriver(["/bin/sleep", "60"])
        try:
            assert spawned.get("start_new_session") is True
        finally:
            driver.close()
