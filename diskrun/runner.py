"""Runner: qemu invocation, write-isolated overlay, file disk, exit-code mapping.

Framework exit-code space (distinct from the guest program's own rc):

=====  =========================================================
Code   Meaning
=====  =========================================================
0-255  guest program rc, passed through verbatim
124    overall timeout expired before the program finished
125    console broke or a marker was never seen
126    runner prerequisites unusable (qemu/qemu-img/mkfs missing)
=====  =========================================================
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time

from .console import ConsoleError, ExpectMachine, PtyDriver, load_script, run_marker_command
from .protocol import wrap_command

EXIT_TIMEOUT = 124
EXIT_NO_CONSOLE = 125
EXIT_RUNNER_UNUSABLE = 126

# Known machine -> qemu system architecture mapping; default is x86_64.
MACHINE_ARCH = {
    "q35": "x86_64",
    "pc": "x86_64",
    "microvm": "x86_64",
    "virt": "aarch64",
    "vexpress-a9": "arm",
}


class RunnerError(Exception):
    def __init__(self, message: str, exit_code: int):
        super().__init__(message)
        self.exit_code = exit_code


def detect_arch(machine: str | None, image: str) -> str:
    if machine:
        base = machine.split(",")[0]
        return MACHINE_ARCH.get(base, "x86_64")
    # Image format probing cannot tell the CPU architecture; x86_64 is the
    # default and --machine is the explicit override.
    return "x86_64"


def probe_format(image: str) -> str:
    """Return the base image format via qemu-img, so the overlay chain is right."""
    if shutil.which("qemu-img") is None:
        raise RunnerError("qemu-img not found in PATH", EXIT_RUNNER_UNUSABLE)
    out = subprocess.run(
        ["qemu-img", "info", "--output=json", image],
        capture_output=True, text=True, timeout=60,
    )
    if out.returncode != 0:
        raise RunnerError(f"qemu-img cannot read {image}: {out.stderr.strip()}", EXIT_RUNNER_UNUSABLE)
    import json

    return json.loads(out.stdout).get("format", "raw")


def build_files_disk(files: list[tuple[str, str]], workdir: str) -> str:
    """Pack host files into a read-only FAT second disk; guest mount commands
    belong to each system's console/expect script, not to the core runner."""
    path = os.path.join(workdir, "files.vfat")
    total = 16
    for host, _ in files:
        total += max(1, (os.path.getsize(host) + 1024 * 1024 - 1) // (1024 * 1024))
    with open(path, "wb") as f:
        f.truncate(int(total) * 1024 * 1024)
    for tool in ("mkfs.vfat", "mcopy"):
        if shutil.which(tool) is None:
            raise RunnerError(f"{tool} not found in PATH", EXIT_RUNNER_UNUSABLE)
    r = subprocess.run(["mkfs.vfat", path], capture_output=True, text=True)
    if r.returncode != 0:
        raise RunnerError(f"mkfs.vfat failed: {r.stderr.strip()}", EXIT_RUNNER_UNUSABLE)
    for host, guest in files:
        guest = guest.lstrip("/")
        r = subprocess.run(
            ["mcopy", "-i", path, host, "::/" + guest],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            raise RunnerError(f"mcopy {host} -> {guest} failed: {r.stderr.strip()}", EXIT_RUNNER_UNUSABLE)
    return path


def build_qemu_argv(*, image, overlay, arch, machine, cpu, memory, smp, accel, files_disk):
    argv = [f"qemu-system-{arch}", "-nographic", "-no-reboot"]
    if machine:
        argv += ["-machine", machine]
    if cpu:
        argv += ["-cpu", cpu]
    argv += ["-m", str(memory), "-smp", str(smp)]
    argv += ["-accel", {"auto": "kvm:tcg", "kvm": "kvm", "tcg": "tcg"}[accel]]
    argv += ["-drive", f"file={overlay},if=virtio"]
    if files_disk:
        argv += ["-drive", f"file={files_disk},format=raw,if=virtio,readonly=on"]
    return argv


def run(*, image, command, machine=None, cpu=None, memory=512, smp=1,
        accel="auto", timeout=300, expect=None, env=None, files=None) -> tuple[int, bytes]:
    """Run ``command`` inside the image's guest; return (rc, stdout)."""
    if not os.path.exists(image):
        raise RunnerError(f"image not found: {image}", EXIT_RUNNER_UNUSABLE)
    env = dict(env or {})
    files = list(files or [])

    arch = detect_arch(machine, image)
    qemu_bin = f"qemu-system-{arch}"
    if shutil.which(qemu_bin) is None:
        raise RunnerError(f"{qemu_bin} not found in PATH", EXIT_RUNNER_UNUSABLE)

    workdir = tempfile.mkdtemp(prefix="disk-run-")
    driver = None
    try:
        base_fmt = probe_format(image)
        overlay = os.path.join(workdir, "overlay.qcow2")
        r = subprocess.run(
            ["qemu-img", "create", "-f", "qcow2", "-b", os.path.abspath(image),
             "-F", base_fmt, overlay],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            raise RunnerError(f"overlay creation failed: {r.stderr.strip()}", EXIT_RUNNER_UNUSABLE)

        files_disk = build_files_disk(files, workdir) if files else None
        argv = build_qemu_argv(
            image=image, overlay=overlay, arch=arch, machine=machine, cpu=cpu,
            memory=memory, smp=smp, accel=accel, files_disk=files_disk,
        )

        script = ExpectMachine(load_script(expect))
        cmdline = wrap_command(command, env)

        deadline = time.monotonic() + timeout
        try:
            driver = PtyDriver(argv)
        except FileNotFoundError:
            raise RunnerError(f"cannot start {qemu_bin}", EXIT_RUNNER_UNUSABLE)
        except ConsoleError as e:
            raise RunnerError(str(e), EXIT_NO_CONSOLE)

        def remaining() -> float:
            return deadline - time.monotonic()

        try:
            if remaining() <= 0:
                raise RunnerError("timeout expired", EXIT_TIMEOUT)
            # Bootstrap phase gets at most the full timeout budget.
            boot_budget = remaining()
            for step in script.steps:
                step = dict(step, timeout=min(step["timeout"], max(0.1, boot_budget)))
            script.run(driver)
            if remaining() <= 0:
                raise RunnerError("timeout expired", EXIT_TIMEOUT)
            out, rc = run_marker_command(driver, cmdline, remaining())
        except ConsoleError as e:
            code = EXIT_TIMEOUT if remaining() <= 0 else EXIT_NO_CONSOLE
            raise RunnerError(f"console: {e}", code)
        return rc & 0xFF, out
    finally:
        if driver is not None:
            driver.close()
        shutil.rmtree(workdir, ignore_errors=True)
