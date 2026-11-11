import io
import json
import shutil
import subprocess
import sys

import pytest

from diskrun.cli import main


class TestCli:
    def test_help_usable(self, capsys):
        with pytest.raises(SystemExit) as e:
            main(["--help"])
        assert e.value.code == 0
        out = capsys.readouterr().out
        for flag in ("--image", "--machine", "--cpu", "--memory", "--smp",
                     "--accel", "--timeout", "--expect", "--env", "--file"):
            assert flag in out

    def test_command_required(self, capsys):
        with pytest.raises(SystemExit) as e:
            main(["--image", "x.img"])
        assert e.value.code == 2

    def test_missing_image_maps_to_126(self, capsys):
        rc = main(["--image", "/nonexistent/img", "--", "true"])
        assert rc == 126

    def test_bad_env_rejected(self):
        with pytest.raises(SystemExit) as e:
            main(["--image", "x", "--env", "NOEQ", "--", "true"])
        assert e.value.code == 2

    def test_bad_file_rejected(self):
        with pytest.raises(SystemExit) as e:
            main(["--image", "x", "--file", "noeq", "--", "true"])
        assert e.value.code == 2

    def test_dash_dash_stripped(self, monkeypatch):
        seen = {}

        def fake_run(**kw):
            seen.update(kw)
            return 0, b""

        monkeypatch.setattr("diskrun.cli.run", fake_run)
        rc = main(["--image", "i", "--env", "A=B", "--file", "h=/g", "--", "cmd", "x"])
        assert rc == 0
        assert seen["command"] == ["cmd", "x"]
        assert seen["env"] == {"A": "B"}
        assert seen["files"] == [("h", "/g")]


BIN = shutil.which("qemu-system-x86_64")
def test_integration_boot_login_image(tmp_path):
    """Real-qemu integration: requires a bootable login-style image.
    Skipped unless qemu and DISK_RUN_TEST_IMAGE are available."""
    import os

    if BIN is None:
        pytest.skip("qemu-system-x86_64 not installed")
    image = os.environ.get("DISK_RUN_TEST_IMAGE")
    if not image:
        pytest.skip("DISK_RUN_TEST_IMAGE not set")
    script = tmp_path / "expect.json"
    script.write_text(json.dumps([
        {"expect": r"login:", "send": "root", "timeout": 120},
        {"expect": r"#", "send": ""},
    ]))
    rc = main([
        "--image", image, "--machine", "q35", "--memory", "256",
        "--accel", "tcg", "--timeout", "300", "--expect", str(script),
        "--", "sh", "-c", "echo hi; exit 5",
    ])
    assert rc == 5
