# Changelog

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
