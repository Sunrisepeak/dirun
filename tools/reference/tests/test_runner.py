import pytest

from diskrun import runner
from diskrun.console import ConsoleError
from diskrun.runner import (
    EXIT_NO_CONSOLE,
    EXIT_RUNNER_UNUSABLE,
    RunnerError,
    build_qemu_argv,
    detect_arch,
)


class TestDetectArch:
    def test_default_x86_64(self, tmp_path):
        assert detect_arch(None, str(tmp_path / "img")) == "x86_64"

    @pytest.mark.parametrize("machine,arch", [
        ("q35", "x86_64"), ("pc", "x86_64"), ("virt", "aarch64"),
        ("vexpress-a9", "arm"), ("virt,accel=kvm", "aarch64"),
    ])
    def test_machine_map(self, machine, arch, tmp_path):
        assert detect_arch(machine, str(tmp_path / "img")) == arch


class TestBuildArgv:
    def test_minimal(self):
        argv = build_qemu_argv(
            image="/i.qcow2", overlay="/o.qcow2", arch="x86_64",
            machine=None, cpu=None, memory=512, smp=1, accel="auto", files_disk=None,
        )
        assert argv[0] == "qemu-system-x86_64"
        assert "-nographic" in argv
        assert "file=/o.qcow2,if=virtio" in argv
        assert "kvm:tcg" in argv
        assert not any("readonly=on" in a for a in argv)

    def test_full(self):
        argv = build_qemu_argv(
            image="/i", overlay="/o", arch="aarch64", machine="virt", cpu="max",
            memory=1024, smp=4, accel="tcg", files_disk="/f.vfat",
        )
        assert argv[0] == "qemu-system-aarch64"
        assert argv[argv.index("-machine") + 1] == "virt"
        assert argv[argv.index("-cpu") + 1] == "max"
        assert argv[argv.index("-m") + 1] == "1024"
        assert argv[argv.index("-smp") + 1] == "4"
        assert "file=/f.vfat,format=raw,if=virtio,readonly=on" in argv


class TestExitCodes:
    def test_missing_image(self):
        with pytest.raises(RunnerError) as e:
            runner.run(image="/nonexistent/img.qcow2", command=["true"])
        assert e.value.exit_code == EXIT_RUNNER_UNUSABLE

    def test_qemu_missing_maps_to_126(self, monkeypatch, tmp_path):
        img = tmp_path / "img.qcow2"
        img.write_bytes(b"")
        monkeypatch.setattr(runner.shutil, "which", lambda name: None)
        with pytest.raises(RunnerError) as e:
            runner.run(image=str(img), command=["true"])
        assert e.value.exit_code == EXIT_RUNNER_UNUSABLE

    def test_broken_console_maps_to_125(self, monkeypatch, tmp_path):
        img = tmp_path / "img.qcow2"
        img.write_bytes(b"")
        monkeypatch.setattr(runner.shutil, "which", lambda name: "found")
        monkeypatch.setattr(
            runner.subprocess, "run",
            lambda *a, **k: type("R", (), {"returncode": 0, "stdout": '{"format": "qcow2"}', "stderr": ""})(),
        )
        monkeypatch.setattr(runner.tempfile, "mkdtemp", lambda prefix="": str(tmp_path))
        from diskrun.console import ConsoleError

        class DeadDriver:
            def __init__(self, argv, env=None):
                raise ConsoleError("no console")

        monkeypatch.setattr(runner, "PtyDriver", DeadDriver)
        with pytest.raises(RunnerError) as e:
            runner.run(image=str(img), command=["true"], timeout=10)
        assert e.value.exit_code == EXIT_NO_CONSOLE


class TestOverlayIsolation:
    def test_overlay_created_with_backing(self, monkeypatch, tmp_path):
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd)
            return type("R", (), {"returncode": 0, "stdout": '{"format": "qcow2"}', "stderr": ""})()

        monkeypatch.setattr(runner.shutil, "which", lambda name: "found")
        monkeypatch.setattr(runner.subprocess, "run", fake_run)
        monkeypatch.setattr(runner.tempfile, "mkdtemp", lambda prefix="": str(tmp_path))

        class DeadDriver:
            def __init__(self, argv, env=None):
                raise ConsoleError("stop here")

        monkeypatch.setattr(runner, "PtyDriver", DeadDriver)
        img = tmp_path / "img.qcow2"
        img.write_bytes(b"")
        with pytest.raises(RunnerError):
            runner.run(image=str(img), command=["true"], timeout=10)
        create = [c for c in calls if c[:2] == ["qemu-img", "create"]]
        assert create and "-b" in create[0]
        assert str(img) in create[0]
        # The base image itself is never passed as a writable drive to qemu.
        # (overlay only; checked via build_qemu_argv tests as well.)
