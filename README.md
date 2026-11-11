# disk-run

Boot a disk image under QEMU, drive its serial console, run one command inside
the guest, and return the command's stdout and exit code on the host.

Generic by design: the contract contains no vocabulary of any particular guest
system. Any CI can use it standalone.

```sh
# quickstart: run one command inside a bootable login-style image
bin/disk-run --image os.qcow2 --accel tcg --timeout 300 -- echo hello
# non-login boot flows: supply your own console script
echo '[{"expect": "# ", "send": ""}]' > boot.json && bin/disk-run --image os.qcow2 --expect boot.json -- echo hello
```

## Contract

```
disk-run --image <img> [--machine m] [--cpu c] [--memory MiB] [--smp n]
         [--accel auto|kvm|tcg] [--timeout SEC] [--expect FILE.json]
         [--env NAME=VAL]... [--file host=guest]... -- <command...>
```

Behaviour:

1. **Boot** — runs `qemu-system-<arch>` with `-nographic` (serial = stdio over
   a pty). Architecture defaults to `x86_64`; `--machine` selects known
   mappings (`virt` → aarch64, `vexpress-a9` → arm, …).
2. **Write isolation** — the base image is never written. A temporary qcow2
   overlay (`qemu-img create -f qcow2 -b <base>`) is booted instead; all
   temporary files (overlay, file disk, workdir) are removed afterwards.
3. **File delivery** — each `--file host=guest` packs host files into a
   read-only raw FAT image attached as a second virtio disk. The commands that
   mount that disk inside the guest belong to each system's console script
   (see *Boundary* below), not to the runner core.
4. **Console driver** — expect/send primitives: `read_until(pattern, timeout)`
   and `send(line)` (throttled). The default script is a login-style
   bootstrap (`login:` → `root` → `Password:` → shell prompt); it is a
   replaceable policy, not part of the core.
5. **Marker protocol** — the command is wrapped as
   `stty -echo; echo @@BEGIN@@; <cmd>; rc=$?; echo @@STATUS=$rc@@`
   (`stty -echo` is sent as its own line first, so the tty stops echoing
   before the marker line arrives). Bytes between the BEGIN and STATUS
   markers are the program's stdout (CR/CRLF normalised to LF).
6. **Environment** — only `--env NAME=VAL` values are passed through, and
   only to the wrapped command (no prefix allowlist). All arguments are
   single-quote escaped for POSIX sh.

## Exit codes

| Code | Meaning | Message remedy |
|------|---------|----------------|
| 0–255 | guest program's own rc, passed through verbatim | — |
| 124   | overall timeout expired before the program finished | increase `--timeout` to cover slow boots |
| 125   | console broke (EOF) or a marker was never seen | check that the first expect pattern in your `--expect` script matches early boot output |
| 126   | runner prerequisites unusable | per message: install qemu / `qemu-img info` the image / FAT tooling below |
| 130   | interrupted (SIGINT) — qemu process group killed, temp dir cleaned | — |
| 143   | terminated (SIGTERM) — same cleanup | — |

Every framework error on stderr is one sentence of cause plus one
"To fix: …" sentence.

## `--expect FILE.json`

Replaces the default login script entirely. Format: a JSON array of steps,
executed in order:

```json
[
  {"expect": "login:", "send": "root", "timeout": 60},
  {"expect": "Password:", "send": "root", "timeout": 30},
  {"expect": "# ", "send": ""}
]
```

- `expect` — regex matched against the console stream (required)
- `send` — line written after a match, throttled (optional)
- `timeout` — per-step budget in seconds, clamped to the overall `--timeout`
  (optional, default 60)

## `--file host=guest`

Copies host file `host` into the FAT disk at guest path `guest` (leading `/`
stripped). The guest sees it on the second virtio disk, read-only. Mounting it
in the guest is the console script's job — e.g. a step whose `send` mounts
`/dev/vdb` before the command runs.

FAT volume building uses mtools (`mkfs.vfat` + `mcopy`) when installed;
otherwise, on macOS, the bundled `hdiutil` (create FAT32 → attach → copy →
detach). If neither is available the run exits 126 with both remedies in the
message (`brew install mtools` / system `hdiutil`).

## Boundary: console drivers belong to each system

The core (`diskrun.protocol`, `diskrun.console`, `diskrun.runner`) is
guest-agnostic. Everything that knows how a particular image boots — its login
prompt, its mount commands, its shell — lives in the per-system expect script
passed via `--expect`. The built-in login-style default is one such policy and
can be ignored entirely.

## Requirements

- Python 3.10+ (stdlib only: `pty`, `select`, `subprocess`, `tempfile`, `json`)
- `qemu-system-<arch>`, `qemu-img`; for `--file`: mtools (`mkfs.vfat`/`mcopy`)
  or, on macOS, the system `hdiutil`

## Development

```
python -m pytest          # mock-layer tests; no QEMU needed
bin/disk-run --help
```

Real-QEMU integration tests auto-skip unless `qemu-system-x86_64` and
`DISK_RUN_TEST_IMAGE` (path to a bootable login-style image) are available.

## License

Apache-2.0 — see [LICENSE](LICENSE).
