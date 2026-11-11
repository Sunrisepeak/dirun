"""Runner: qemu invocation, write-isolated overlay, file disk, exit-code mapping.

Framework exit-code space (distinct from the guest program's own rc):

=====  =========================================================
Code   Meaning
0-255  guest program rc, passed through verbatim
124    overall timeout expired before the program finished
125    console broke or a marker was never seen
126    runner prerequisites unusable (qemu / qemu-img / FAT tooling missing)
130    interrupted (SIGINT)
143    terminated (SIGTERM)
=====  =========================================================
"""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import tempfile
import time

from .console import ConsoleError, ExpectMachine, PtyDriver, load_script, run_marker_command
from .protocol import wrap_command

EXIT_TIMEOUT = 124
EXIT_NO_CONSOLE = 125
EXIT_RUNNER_UNUSABLE = 126
EXIT_SIGINT = 130
EXIT_SIGTERM = 143

# Known machine -> qemu system architecture mapping; default is x86_64.
MACHINE_ARCH = {
    "q35": "x86_64",
    "pc": "x86_64",
    "microvm": "x86_64",
    "virt": "aarch64",
    "vexpress-a9": "arm",
}

# Remedy sentences keyed by exit code, used by exit_error().
REMEDIES = {
    EXIT_TIMEOUT: "increase --timeout to cover slow boots",
    EXIT_NO_CONSOLE: "check that the first expect pattern in your --expect script matches early boot output",
    EXIT_RUNNER_UNUSABLE: "see the specific message (install qemu, or FAT tooling)",
}


class RunnerError(Exception):
    def __init__(self, message: str, exit_code: int, remedy: str | None = None):
        super().__init__(message)
        self.exit_code = exit_code
        self.remedy = remedy if remedy is not None else REMEDIES.get(exit_code, "")


class SignalExit(Exception):
    """Raised from a signal handler so cleanup runs and the exit code is 130/143."""

    def __init__(self, signum: int):
        self.signum = signum
        super().__init__(f"interrupted by signal {signum}")

    @property
    def exit_code(self) -> int:
        return 128 + self.signum


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
        raise RunnerError(
            "qemu-img not found in PATH", EXIT_RUNNER_UNUSABLE,
            remedy="install qemu (qemu-img ships with it)",
        )
    out = subprocess.run(
        ["qemu-img", "info", "--output=json", image],
        capture_output=True, text=True, timeout=60,
    )
    if out.returncode != 0:
        raise RunnerError(
            f"qemu-img cannot read {image}: {out.stderr.strip()}", EXIT_RUNNER_UNUSABLE,
            remedy="verify the image path and format with `qemu-img info <image>`",
        )
    import json

    return json.loads(out.stdout).get("format", "raw")


def _disk_mib(files: list[tuple[str, str]]) -> int:
    total = 16
    for host, _ in files:
        total += max(1, (os.path.getsize(host) + 1024 * 1024 - 1) // (1024 * 1024))
    return total


def _build_files_disk_mtools(files, path, mib) -> str:
    with open(path, "wb") as f:
        f.truncate(mib * 1024 * 1024)
    r = subprocess.run(["mkfs.vfat", path], capture_output=True, text=True)
    if r.returncode != 0:
        raise RunnerError(f"mkfs.vfat failed: {r.stderr.strip()}", EXIT_RUNNER_UNUSABLE,
                          remedy="install mtools (e.g. brew install mtools / apt install mtools)")
    for host, guest in files:
        guest = guest.lstrip("/")
        r = subprocess.run(["mcopy", "-i", path, host, "::/" + guest],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RunnerError(f"mcopy {host} -> {guest} failed: {r.stderr.strip()}",
                              EXIT_RUNNER_UNUSABLE,
                              remedy="install mtools (e.g. brew install mtools / apt install mtools)")
    return path


def _build_files_disk_hdiutil(files, path, mib) -> str:
    """macOS native path: hdiutil creates the FAT volume, attach+copy+detach fills it."""
    r = subprocess.run(
        ["hdiutil", "create", "-fs", "MS-DOS FAT32", "-layout", "NONE",
         "-size", f"{mib}m", "-ov", "-o", path],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        raise RunnerError(f"hdiutil create failed: {r.stderr.strip()}", EXIT_RUNNER_UNUSABLE,
                          remedy="hdiutil ships with macOS; check disk space and permissions")
    # hdiutil may append .dmg when the given name has no recognizable image suffix.
    img = path if os.path.exists(path) else path + ".dmg"
    mount = path + ".mnt"
    os.makedirs(mount, exist_ok=True)
    try:
        r = subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", mount, img],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RunnerError(f"hdiutil attach failed: {r.stderr.strip()}", EXIT_RUNNER_UNUSABLE,
                              remedy="check that the volume is not already mounted")
        try:
            for host, guest in files:
                guest = guest.lstrip("/")
                target = os.path.join(mount, guest)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copy2(host, target)
        finally:
            subprocess.run(["hdiutil", "detach", mount], capture_output=True, text=True)
    finally:
        shutil.rmtree(mount, ignore_errors=True)
    return img


def build_files_disk(files: list[tuple[str, str]], workdir: str) -> str:
    """Pack host files into a read-only FAT second disk; guest mount commands
    belong to each system's console/expect script, not to the core runner."""
    path = os.path.join(workdir, "files.vfat")
    mib = _disk_mib(files)
    have_mtools = shutil.which("mkfs.vfat") and shutil.which("mcopy")
    have_hdiutil = shutil.which("hdiutil") is not None
    if have_mtools:
        return _build_files_disk_mtools(files, path, mib)
    if have_hdiutil:
        return _build_files_disk_hdiutil(files, path, mib)
    raise RunnerError(
        "cannot build the FAT file disk: mtools (mkfs.vfat/mcopy) not found and "
        "hdiutil is unavailable", EXIT_RUNNER_UNUSABLE,
        remedy="install mtools (e.g. brew install mtools / apt install mtools) "
               "or use the system hdiutil (bundled with macOS)",
    )


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


class _SignalGuard:
    """Converts SIGINT/SIGTERM into SignalExit so finally-blocks run cleanup."""

    def __init__(self):
        self._prev: dict[int, object] = {}

    def __enter__(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            self._prev[sig] = signal.getsignal(sig)
            signal.signal(sig, self._handle)
        return self

    def __exit__(self, *exc):
        for sig, handler in self._prev.items():
            signal.signal(sig, handler)
        return False

    @staticmethod
    def _handle(signum, frame):
        raise SignalExit(signum)


def run(*, image, command, machine=None, cpu=None, memory=512, smp=1,
        accel="auto", timeout=300, expect=None, env=None, files=None) -> tuple[int, bytes]:
    """Run ``command`` inside the image's guest; return (rc, stdout)."""
    if not os.path.exists(image):
        raise RunnerError(
            f"image not found: {image}", EXIT_RUNNER_UNUSABLE,
            remedy="pass an existing disk image via --image",
        )
    env = dict(env or {})
    files = list(files or [])

    arch = detect_arch(machine, image)
    qemu_bin = f"qemu-system-{arch}"
    if shutil.which(qemu_bin) is None:
        raise RunnerError(
            f"{qemu_bin} not found in PATH", EXIT_RUNNER_UNUSABLE,
            remedy=f"install qemu (provides {qemu_bin})",
        )

    workdir = tempfile.mkdtemp(prefix="disk-run-")
    driver = None
    try:
        with _SignalGuard():
            base_fmt = probe_format(image)
            overlay = os.path.join(workdir, "overlay.qcow2")
            r = subprocess.run(
                ["qemu-img", "create", "-f", "qcow2", "-b", os.path.abspath(image),
                 "-F", base_fmt, overlay],
                capture_output=True, text=True,
            )
            if r.returncode != 0:
                raise RunnerError(
                    f"overlay creation failed: {r.stderr.strip()}", EXIT_RUNNER_UNUSABLE,
                    remedy="check the base image format with `qemu-img info <image>`",
                )

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
                raise RunnerError(
                    f"cannot start {qemu_bin}", EXIT_RUNNER_UNUSABLE,
                    remedy=f"install qemu and ensure {qemu_bin} is executable",
                )
            except ConsoleError as e:
                raise RunnerError(str(e), EXIT_NO_CONSOLE)

            def remaining() -> float:
                return deadline - time.monotonic()

            try:
                if remaining() <= 0:
                    raise RunnerError("timeout expired before boot completed", EXIT_TIMEOUT)
                # Bootstrap phase gets at most the full timeout budget.
                boot_budget = remaining()
                for step in script.steps:
                    step = dict(step, timeout=min(step["timeout"], max(0.1, boot_budget)))
                script.run(driver)
                if remaining() <= 0:
                    raise RunnerError("timeout expired during boot script", EXIT_TIMEOUT)
                out, rc = run_marker_command(driver, cmdline, remaining())
            except ConsoleError as e:
                code = EXIT_TIMEOUT if remaining() <= 0 else EXIT_NO_CONSOLE
                raise RunnerError(f"console: {e}", code)
            return rc & 0xFF, out
    finally:
        # Kills the whole qemu process group (start_new_session) and wipes the
        # overlay/workdir — runs for normal exits, errors and signals alike.
        if driver is not None:
            driver.close()
        shutil.rmtree(workdir, ignore_errors=True)
