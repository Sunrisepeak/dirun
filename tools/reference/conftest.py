import sys

# The frozen reference's posix faces (pty, termios, sessions) cannot import on
# Windows; the C++ suite covers the same contract there with its own skips.
collect_ignore = (
    ["tests/test_console.py", "tests/test_hardening.py"]
    if sys.platform == "win32"
    else []
)
