# Changelog

## 0.3.0 -- 2026-11-13

One package; the console over a pipe; profiles as data; a payload mode.

- **The command's words are one command.** 0.2.0 and the 0.1 reference wrote each word as its own
  command (`'echo'; 'hello'`) and the environment as a separate `env` that only printed it; the
  words are now joined by spaces and the environment precedes them (`env 'K=v' 'echo' 'hello'`).
- **An image is read with its own format.** Newer `qemu-img info` lists the file layer's format
  first; 0.2.0 took it, created an overlay whose backing was read as raw bytes, and the guest found
  no bootable disk. The top-level `format` is used.
- **The console is read over a pipe** on every host (QEMU `-nographic`, measured equal to a pseudo
  terminal); the Windows host is no longer a stub. Console driving is the test library's session
  (expect/send), not a module of this program.
- **Profiles are data** (`--profile FILE`, `--var NAME=VALUE`): the boot steps (with `optional`,
  `pace`, `line`), the command line's template, quoting and markers, the mount lines and the machine
  defaults. `--expect FILE.json` and the built-in machine-to-architecture table are replaced by the
  profile and `--arch`. The built-in profile is `profiles/posix-login.toml`.
- **A payload mode** (`--payload FILE`): the file is booted as the program, its status returned as
  the profile's `[payload]` states (`exit`, `debug-exit`, `marker`), or as the payload options state
  (`--emulator-arg`, `--place`, `--status`, `--marker`, `--after`), so that a board's runner statement
  carries the whole description. A payload whose firmware output is ended by `after` and never shows
  it did not run: an emulator that could not start reads as status 0 under `debug-exit`, and is
  reported as `no console` instead.
- **A console's line ends are one boundary each.** CRs before an LF are one boundary: a console that
  adds a CR to a line the guest ended with CR LF (a terminal's `onlcr`, a text-mode stream on
  Windows) wrote an empty line after every line.
- **A guest runs what a test runner gives it, as a user-mode emulator would.** A program of this
  machine that the command names first is delivered and run from its copy; `--pass NAME|PREFIX*`
  gives the command this machine's variables; a profile that ships with an image names it
  (`[machine] image`, beside the profile), so a runner statement is `dirun --profile
  {pkg:<image>}/dirun.toml -- {}`; and under a test runner that speaks the mtest protocol, the
  command speaks it in the guest and its events are appended to the runner's report (`[relay]`).
  Measured with a FreeBSD 14.5 image: a delivered program, a passed variable, its status and its
  event, 16.8 s under KVM.
- **A payload is described for an emulator that is not QEMU** (`[payload] machine = false`):
  launcher templates written beside it (`files`, with the program's arguments after `--` as
  `{args}`), `--file` delivered beside it, the emulator's environment (`env`), and the status and
  output read from files a launcher wrote (`status = "file"`, `status-file`, `output-file`). A
  profile that ships with its emulator names it and the files delivered beside the payload
  (`emulator`, `beside`), so a runner statement is `dirun --payload {} --profile <its profile>`.
  Measured with DOSBox Staging 0.83.0 and a DJGPP program: its output and status 42, 0.26 s.
- **A command line too long for the console is run from the files disk.** A terminal's canonical
  input bounds a typed line; a longer one reached the shell cut and the run waited for a marker
  that never came (measured: the conformance suite's environment, 600 characters, on FreeBSD). Past
  the profile's `[command] line-max` the line is written to the files disk and `[command] script`
  runs it. The command's standard input is empty.
- **The files disk is the emulator's own.** The files are gathered in a directory that QEMU presents
  as a read-only FAT disk (`fat:`): no mtools or `hdiutil` is needed on any host.
- `--emulator PROGRAM` names the emulator, which is otherwise `qemu-system-ARCH` on `PATH`: a platform's
  runner names the program of the package it requires. `--emulator-arg` adds a word of its command line.
- `--trace FILE` writes the emulator's command line and the console's transcript.
- One package instead of four workspace members; the command line is read with a command-line
  library; the tests are the test library's cases, including the contract's exit statuses against a
  console played by a child process, and a real boot when `DIRUN_TEST_IMAGE` names an image
  (measured: a FreeBSD 14 image with its profile, 19.6 s under KVM). The
  Python reference implementation and its suite are removed: the contract is these cases.

## 0.2.0 — 2026-11-12

The native implementation (PR #1); the contract is unchanged from 0.1.2.

- C++23 is the one implementation users run: a workspace of three libraries
  (`protocol`, `console`, `runner`) and the `dirun` program; the 0.1.x Python
  moves to `tools/reference/` as the frozen specification, its suite still
  runs in CI as contract regression.
- The console driver's pty is a thin native layer (openpty, poll loop,
  process-group end, EBADF/EIO normalised) with a clear "not built" face on a
  system without one; its tests skip there.
- The runner's system touch points (qemu-img, the FAT tooling, the qemu
  channel) go through declared seams, so the suite covers the whole exit-code
  space, the overlay isolation, the mtools/hdiutil file-disk paths and the
  signal cleanup without qemu installed.
- A words check (`tools/checks/words.py`) enforces the no-ecosystem-vocabulary
  contract on the sources and docs, build configuration aside.


## 0.1.2 — 2026-11-12

CI portability fix for the pty driver.

- Any OSError on the pty master (EBADF/EIO — kernel-dependent faces of a dead
  console) is normalised to `ConsoleError("console closed: <errno text>")`;
  it never escapes raw from `select`/`read`.
- Constructor no longer double-closes the slave fd when the child fails to
  spawn; a spawn error now surfaces as itself instead of a masking EBADF.
- Tests: direct EBADF (real closed master) and EIO (injected errno) forms,
  plus the spawn-failure non-masking case.

## 0.1.1 — 2026-11-12

Hardening (no contract surface changes).

- `--file` FAT volumes: mtools when present; macOS fallback via the bundled
  `hdiutil` (create FAT32 → attach → copy → detach); exit 126 with both
  remedies when neither is available. Linux path unchanged.
- Signal safety: SIGINT/SIGTERM kill the whole qemu process group
  (`start_new_session` + `killpg`), remove the overlay/workdir, and exit
  with 130/143.
- Actionable errors: every 124/125/126 message is one sentence of cause plus
  one "To fix: …" sentence; README exit-code table now lists the remedies.
- `--help` grouped (run / console / files and environment); README quickstart.

## 0.1.0 — 2026-11-12

Initial skeleton.

- `disk-run` CLI: boot an image under QEMU, run one command in the guest,
  return its stdout and exit code.
- Marker protocol (`@@BEGIN@@` / `@@STATUS=<rc>@@`) with CR/LF normalisation
  and POSIX-sh-safe argument escaping.
- Expect/send console driver over a pty (throttled writes, regex matching,
  per-step timeouts); default login-style bootstrap script, replaceable via
  `--expect FILE.json`.
- Write isolation: temporary qcow2 overlay over the base image; automatic
  cleanup of the working directory.
- `--file host=guest` delivery via a read-only FAT second virtio disk.
- `--env NAME=VAL` explicit passthrough (no prefix allowlist).
- Exit-code space: program rc passthrough; 124 timeout, 125 no console/marker,
  126 runner prerequisites unusable.
- pytest suite with a mock console driver (marker splitting, expect state
  machine, escaping, exit-code mapping); real-QEMU integration tests auto-skip.
