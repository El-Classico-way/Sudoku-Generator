"""Exception model of the Python engine client.

Rule of thumb: an engine OPERATION that ran and reported a non-success outcome
(a puzzle that cannot be solved, a bounded generation that was exhausted, an
inconclusive count, ...) is NOT an exception. It is returned as a typed result
whose ``success`` is False. Exceptions mean the request could not be carried out
or the answer cannot be trusted:

==========================  ====================================================
exception                   meaning
==========================  ====================================================
InvalidArgumentError        a Python-side argument is invalid; nothing was sent
ConfigurationError          (an InvalidArgumentError) a settings file or settings value is
                            invalid; the message names the file/key
RequestRejectedError        the engine understood the request and rejected it
                            (protocol error response, process exit code 2)
EngineInternalError         the engine reported an internal failure (exit code 3,
                            error code ``internal_error``)
ExecutableNotFoundError     no usable engine executable could be selected
EngineLaunchError           the executable exists but could not be started
EngineTimeoutError          the process did not finish within the timeout; it was
                            killed
EngineProcessError          the process ended in a way the protocol does not allow
                            (unexpected exit code, or a nonzero exit with no valid
                            response); carries exit code and stderr
ResponseFormatError         the process exited normally but its output is not a
                            valid response (empty, malformed JSON, extra output,
                            schema or consistency violation)
OperationNotSuccessfulError raised only by an explicit ``require_success()`` call
                            on a typed result, never automatically
==========================  ====================================================

All of them derive from :class:`SudokuError`. Messages and ``stderr`` attributes
carry at most a bounded excerpt of process output (see :func:`excerpt`).
"""
from __future__ import annotations

from typing import Any

MAX_EXCERPT_CHARS = 2000


def excerpt(data: bytes | str | None, limit: int = MAX_EXCERPT_CHARS) -> str:
    """Bounded, printable text from process output, for use in error context."""
    if data is None:
        return ""
    text = data.decode("utf-8", errors="replace") if isinstance(data, (bytes, bytearray)) else str(data)
    if len(text) <= limit:
        return text
    return f"{text[:limit]}... [{len(text) - limit} more characters truncated]"


class SudokuError(Exception):
    """Base class of every error raised by the client."""


class InvalidArgumentError(SudokuError, ValueError):
    """A Python-side argument is invalid. No process was started."""


class ConfigurationError(InvalidArgumentError):
    """Settings are invalid: unreadable or malformed file, unknown key, wrong type or out-of-range value."""


class ExecutableNotFoundError(SudokuError):
    """No engine executable could be selected (see EngineRunner / find_executable)."""

    def __init__(self, message: str, searched: tuple[str, ...] = ()) -> None:
        super().__init__(message)
        self.searched = searched


class EngineLaunchError(SudokuError):
    """The executable exists but the operating system could not start it."""

    def __init__(self, message: str, argv: tuple[str, ...], cause: OSError | None = None) -> None:
        super().__init__(message)
        self.argv = argv
        self.cause = cause


class EngineTimeoutError(SudokuError):
    """The engine process exceeded the timeout and was killed."""

    def __init__(self, timeout: float, argv: tuple[str, ...], stderr: bytes | str | None = None) -> None:
        self.timeout = timeout
        self.argv = argv
        self.stderr = excerpt(stderr)
        super().__init__(f"engine did not finish within {timeout} s and was killed")


class EngineProcessError(SudokuError):
    """Unexpected process outcome: an exit code the protocol does not define, or a
    nonzero exit without a valid response."""

    def __init__(self, message: str, exit_code: int, stdout: bytes | str | None = None,
                 stderr: bytes | str | None = None) -> None:
        self.exit_code = exit_code
        self.stdout = excerpt(stdout)
        self.stderr = excerpt(stderr)
        suffix = f" (exit code {exit_code})"
        if self.stderr:
            suffix += f"; stderr: {excerpt(stderr, 300)}"
        super().__init__(message + suffix)


class ResponseFormatError(SudokuError):
    """The process exited normally but the response is unusable or inconsistent."""

    def __init__(self, message: str, stdout: bytes | str | None = None, stderr: bytes | str | None = None) -> None:
        self.stdout = excerpt(stdout)
        self.stderr = excerpt(stderr)
        super().__init__(message)


class RequestRejectedError(SudokuError):
    """The engine rejected the request (protocol error, exit code 2).

    ``code`` is the stable machine-readable error code (for example
    ``invalid_field``, ``unknown_operation``); ``field`` and ``position`` are set
    when the engine reported them.
    """

    def __init__(self, code: str, message: str, *, field: str | None = None, position: int | None = None,
                 request_id: str | None = None, stderr: bytes | str | None = None) -> None:
        self.code = code
        self.engine_message = message
        self.field = field
        self.position = position
        self.request_id = request_id
        self.stderr = excerpt(stderr)
        where = f" (field {field})" if field else ""
        super().__init__(f"engine rejected the request: {code}: {message}{where}")


class EngineInternalError(SudokuError):
    """The engine reported an internal failure (exit code 3). No result is available."""

    def __init__(self, message: str, *, request_id: str | None = None, stderr: bytes | str | None = None,
                 code: str = "internal_error") -> None:
        self.code = code
        self.engine_message = message
        self.request_id = request_id
        self.stderr = excerpt(stderr)
        suffix = f"; stderr: {excerpt(stderr, 300)}" if self.stderr else ""
        super().__init__(f"engine internal error: {message}{suffix}")


class OperationNotSuccessfulError(SudokuError):
    """Raised by ``result.require_success()`` when the engine reported a non-success outcome."""

    def __init__(self, result: Any) -> None:
        self.result = result
        super().__init__(f"operation did not succeed: status {getattr(result, 'status', '?')!s}")
