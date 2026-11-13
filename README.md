# dirun

dirun runs one command inside a booted disk image and returns the command's output and exit status;
it is to a virtual machine what a user-mode emulator is to a program of another architecture. It
also boots a payload -- a firmware application, a bare-metal program -- and returns its status. It
does not build images, manage machines or configure guests.

```sh
dirun --image os.qcow2 -- uname -a                       # the built-in POSIX login profile
dirun --image os.qcow2 --profile os.toml --env K=v --file data.txt=/data.txt -- ./check
dirun --profile freebsd/dirun.toml --pass 'TEST_*' -- ./test-program      # the image the profile names
dirun --payload app.efi --profile uefi.toml --var firmware=/path/to/code.fd
```

How an image boots -- its prompts, its shell's quoting, how it mounts the file disk -- is a
**profile**, data that ships with the image ([docs/profiles.md](docs/profiles.md)). The program
knows no system.

| Exit status | Meaning |
|---|---|
| 0-255 | the command's (or the payload's) own status |
| 124 | the time limit (`--timeout`, default 300 s) expired first |
| 125 | the console ended, or a marker never came |
| 126 | a prerequisite is unusable: qemu, the image, the profile, a file to deliver |
| 130, 143 | interrupted (SIGINT, SIGTERM); the emulator is ended |

The base image is never written: a temporary overlay is booted and removed. The console is read
over a pipe, the same on Linux, macOS and Windows hosts.

- [docs/contract.md](docs/contract.md) -- the command line, the marker protocol, the exit statuses
- [docs/profiles.md](docs/profiles.md) -- the profile format, with the guest and payload forms
- Requires `qemu-system-<arch>` and `qemu-img`

```sh
mcpp build && mcpp test        # DIRUN_TEST_IMAGE=<img> also boots one for real
```

Apache-2.0.
