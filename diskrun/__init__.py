"""disk-run: boot a disk image, drive its console, return program output and rc."""

__version__ = "0.1.0"

from .console import ConsoleError, ExpectMachine, PtyDriver, load_script, run_marker_command
from .protocol import BEGIN, escape_sharg, normalize_output, split_output, wrap_command
from .runner import (
    EXIT_NO_CONSOLE,
    EXIT_RUNNER_UNUSABLE,
    EXIT_TIMEOUT,
    RunnerError,
    run,
)

__all__ = [
    "BEGIN", "ConsoleError", "ExpectMachine", "PtyDriver", "RunnerError",
    "EXIT_TIMEOUT", "EXIT_NO_CONSOLE", "EXIT_RUNNER_UNUSABLE",
    "escape_sharg", "load_script", "normalize_output", "run", "run_marker_command",
    "split_output", "wrap_command", "__version__",
]
