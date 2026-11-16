# Profiles

A profile is a TOML file that states how a disk image boots and runs a command, or how a payload
boots and returns its status. It ships with the image or the board it describes; dirun carries one,
`posix-login` ([profiles/posix-login.toml](../profiles/posix-login.toml)), used when `--profile` is
not given. A key a profile does not have is an error. `{name}` is replaced by `--var name=...`
(`{{` and `}}` are one brace). `{profile-dir}` is the directory of the profile's file, so a profile
names a file shipped beside it, such as a firmware image, as `{profile-dir}/<name>`; `--var` does
not replace it.

```toml
name = "freebsd-14"

[machine]                    # defaults; the command line overrides
arch   = "x86_64"            # qemu-system-<arch>
type   = ""                  # -machine
cpu    = ""                  # -cpu
memory = 1024                # MiB
smp    = 1
args   = []                  # further emulator words
image  = "disk.qcow2"        # the disk when --image is not given; relative: beside this file

[[boot]]                     # in order
expect  = "Autoboot in"      # a regular expression (ECMAScript) awaited on the console
send    = " 3"               # written after it; omitted: nothing is written
line    = false              # whether a newline follows `send` (default true)
pace    = 300                # milliseconds between characters: a loader that polls its line drops faster input
timeout = 120                # seconds (default 300), within the run's limit

[[boot]]
expect   = "Password:"
send     = "root"
optional = true              # passed over when the next step's pattern appears first

[command]                    # guest mode
prepare = ["stty -echo"]     # lines written before the command, each awaited at the prompt
line    = "echo @@BEGIN@@; {env}{argv} < /dev/null; echo @@STATUS=$?@@"
line-max = 200               # a longer line is written to the files disk and run from there
script   = "sh {script}"     # how; {script} is its name in the guest
env     = "env {assignments} "   # how NAME=VALUE words precede the command
quote   = "posix"            # posix, uefi-shell or none: how each word is written (below)
begin   = "@@BEGIN@@"
status  = "@@STATUS=(\\d+)@@"

[files]                      # the read-only FAT disk of the files (one partition)
mount = ["mkdir -p /mnt/in", "mount -r -t msdosfs /dev/vtbd1s1 /mnt/in"]
at    = "/mnt/in"            # where `mount` puts it: a delivered program runs from its copy there

[relay]                      # the test protocol's events (docs/contract.md, "The test protocol")
report = "/tmp/dirun-mtest.ndjson"   # the guest file the command writes them to
read   = ["cat"]             # the words that print a file, its name appended

[payload]                    # payload mode
args   = ["-bios", "{firmware}", "-drive", "format=raw,file=fat:rw:{dir}",
          "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
place  = "EFI/BOOT/BOOTX64.EFI"
status = "debug-exit"        # exit, debug-exit, marker or file
marker = ""                  # status = "marker": a pattern whose first group is the status
after  = ""                  # output before this pattern's first match is boot noise
machine = true               # false: the emulator is not QEMU and is given only `args`
quote   = "none"             # how each argument is written into {args}: posix, uefi-shell or none (0.5.0)
# files       = { "RUN.BAT" = "PROG.EXE {args} > OUT.TXT" }   # templates written into {dir}
# status-file = "RC.TXT"     # status = "file": the number a launcher wrote
# output-file = "OUT.TXT"    # the program's output, read from {dir}
# stderr-file = "ERR.TXT"    # its standard error, read from {dir} and written to dirun's (0.4.0)
# env         = { SDL_VIDEODRIVER = "dummy" }                 # the emulator's environment
# emulator    = "bin/dosbox" # when --emulator is not given; relative: beside this file
# beside      = ["CWSDPMI.EXE"]   # files beside this file, delivered beside the payload
```

A guest reached by commands (0.4.0) has `[reach]` in place of `[machine]`, `[[boot]]` and `[files]`,
and `[start]` when this program starts its machine. Its `[command]` is the guest's command line, as
for a console:

```toml
[reach]
shell = ["{adb}", "-s", "{serial}", "shell", "{line}"]          # runs one command line; {line}
push  = ["{adb}", "-s", "{serial}", "push", "{host}", "{guest}"]  # delivers one file
at    = "/data/local/tmp/dirun"                                  # where delivered files are placed

[start]                      # omitted: the guest is already running (a remote host)
command = ["{emulator}", "-avd", "{avd}", "-no-window", "-no-audio", "-no-snapshot-save"]
ready   = ["{adb}", "-s", "{serial}", "shell", "getprop sys.boot_completed | grep -q 1"]
ready-timeout = 600          # seconds; `ready' is run once a second until it succeeds
stop    = ["{adb}", "-s", "{serial}", "emu", "kill"]   # omitted: the started command is ended
```

The two parts are independent. A machine's lifecycle (`[start]`) may be absent; the way into the
guest (`[reach]`) is always stated. A virtual machine with a serial console, an emulated device
behind a bridge and a running host behind a remote shell are three combinations of the two
([examples](examples)).

An emulator of another system's interface rather than of a machine --- DOSBox, which runs a DOS
program in a directory it mounts as `C:` --- is a payload profile with `machine = false`: the
program is placed in `{dir}`, a launcher written beside it runs it with `{args}` and writes its
output and status as files, and the status is read from them, because DOSBox's own exit status is
not the program's and its command interpreter has no `%ERRORLEVEL%` (the status is read one digit at
a time with `if errorlevel`).

A word reaches the guest through a line another program reads: a POSIX shell, the UEFI Shell, DOS's
command interpreter. The quoting style is therefore the profile's, for its command line (`[command]
quote`) and for the arguments written into a payload's launcher or emulator words (`[payload]
quote`). `posix` single-quotes each word and is safe for any byte. `uefi-shell` writes a word that
is empty, or holds a blank or one of `"`, `^`, `%`, `#`, in double quotes, with `^` before each of
those four; the UEFI Shell splits a line at blanks outside quotes and reads `^` as an escape (UEFI
Shell Specification 2.2). `none` writes the words as given: a reader with no quoting, such as
COMMAND.COM, cannot take a blank in a word.

The last boot step's pattern is the shell's prompt. A guest whose command interpreter has no
`$?` states its own `line` and `status`: what the command line looks like is the profile's, not the
program's.
