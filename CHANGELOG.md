# Changelog

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
