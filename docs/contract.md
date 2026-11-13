# The contract

## The command line

```
dirun [--image IMG] [--profile FILE] [--var NAME=VALUE]... [MACHINE] [--env NAME=VALUE]...
      [--pass NAME|PREFIX*]... [--file HOST=GUEST]... [--timeout SEC] [--trace FILE] -- COMMAND...
dirun --payload FILE [--profile FILE] [PAYLOAD] [--var NAME=VALUE]... [MACHINE] [--file HOST=GUEST]...
      [--timeout SEC] [--trace FILE] [-- ARGS...]

PAYLOAD: [--emulator-arg WORD]... [--place PATH] [--status exit|debug-exit|marker] [--marker REGEX] [--after REGEX]

MACHINE: [--arch ARCH] [--emulator PROGRAM] [--machine M] [--cpu C] [--memory MiB] [--smp N] [--accel auto|kvm|tcg]
```

Exactly one of `--image` and `--payload` is given. A machine option overrides the profile's
`[machine]`. `--emulator` names the emulator; without it, `qemu-system-ARCH` is found on `PATH` (a
runner that installs its emulator as a package names the package's program). `--accel auto` asks for the host's accelerator (kvm, hvf, whpx) and falls back to tcg.
`--trace` writes the emulator's command line and the console's transcript, with what was sent
marked, however the run ends.

## Guest mode

1. The image is `--image`, else the profile's `[machine] image` (a profile that ships with an image
   names it). Its format is read (`qemu-img info`, the top-level `format`); a qcow2 overlay backed
   by the image is created in a temporary directory and booted. The image is never written.
2. The files: each `--file`, a program of this machine that `COMMAND` names first (it is run from
   its copy, at the profile's `[files] at`), and the test protocol's selection (below). They are
   gathered in a directory the emulator presents as a read-only FAT disk with one partition
   (QEMU's `fat:` driver; no tool of the host's), the second virtio disk. The profile's
   `[files] mount` lines mount it.
3. The emulator runs with `-nographic`: its serial line is its standard input and output, and the
   firmware's text console is mirrored onto it. The console is read and written over pipes.
4. The profile's `[[boot]]` steps run in order; then its `[command] prepare` lines, then the mount
   lines, each awaited at the last boot step's prompt.
5. The command line is written: the words of `COMMAND` joined by spaces, quoted as `[command] quote`
   says, preceded by the environment as `[command] env` says, within `[command] line`. The
   environment is `--env`, the variables of this machine `--pass` names (`NAME`, or every one
   beginning `PREFIX` for `PREFIX*`), and the test protocol's. The command's standard input is
   empty (the built-in profile's line reads it from `/dev/null`): the console is the conversation's,
   and a command that read it would wait for input that never comes. A line longer than the
   profile's `[command] line-max` is not typed --- a terminal's canonical input bounds a line, and a
   longer one reaches the shell cut --- but written to the files disk and run by `[command] script`
   (`sh {script}` in the built-in profile).
6. The output is what lies between the last begin marker before the status marker and the status
   marker, carriage returns normalised, one line boundary removed at each end. The status is the
   status pattern's first group, modulo 256.

## The test protocol

A program started by a test runner that speaks the mtest protocol (`MTEST_PROTOCOL` and
`MTEST_REPORT` set) writes its events to a file. When dirun is that program and its profile has a
`[relay]`, the command speaks the protocol in the guest: it is given `MTEST_PROTOCOL`,
`MTEST_MODE`, `MTEST_REPORT` as the profile's `[relay] report`, and `MTEST_SELECT` as the
selection's copy on the files disk. After the command, `[relay] read` and the report's name are run
as a second command, and each line of its output that is an event (it begins with `{`) is
appended to the runner's `MTEST_REPORT`. A test of another system runs there as it would here.

## Payload mode

A payload's description is a profile's `[payload]`, or the payload options, which add to it: a
board states how its payload boots and how its status returns in its runner statement, where a file
of its own has no name to be reached by.

The payload is the program: the profile's `[payload] args` boot it (`{payload}` is the file;
`{dir}` a directory holding it at `[payload] place`, with each `--file` at its name and each
`[payload] files` template written there; `{args}` the program's arguments after `--`, joined by
spaces). The emulator is given the machine's QEMU words first, unless `[payload] machine = false`
(an emulator that is not QEMU, given only `args`, in the environment `[payload] env` adds); the
emulator is `--emulator`, else `[payload] emulator`, and the files `[payload] beside` names are
delivered beside the payload (a profile that ships with an emulator names both beside itself). Its
status is the emulator's exit status (`exit`), that status as a debug-exit device gives it,
`(s - 1) / 2` (`debug-exit`), the first group of a pattern on the console (`marker`), or the number
in `[payload] status-file` that a launcher in `{dir}` wrote (`file`: an emulator whose own status
is not the program's). The output is the console's, after the first match of `[payload] after`
when the profile states one, or the contents of `[payload] output-file`.

## Exit statuses

| Status | Meaning |
|---|---|
| 0-255 | the command's or the payload's own status |
| 2 | the command line is wrong |
| 124 | the time limit expired first |
| 125 | the console ended, or a pattern never came |
| 126 | a prerequisite is unusable |
| 128 + n | ended by signal n; the emulator is ended and the temporary files removed |

Every error is one line on standard error: the cause, then `To fix:` and the remedy.
