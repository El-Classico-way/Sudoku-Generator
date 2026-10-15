"""Raw protocol layer: build requests and interpret process output.

This module knows the JSON protocol of the C++ engine (``sudoku.request/1`` /
``sudoku.response/1``, see the README) and nothing about subprocesses or typed
results. Everything here is a pure function, so it can be tested without an
executable. The C++ engine remains the authority on validation: the checks done
here only give early, clear errors for obviously wrong Python arguments.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any, Mapping

from .errors import (
    EngineInternalError, EngineProcessError, InvalidArgumentError, RequestRejectedError, ResponseFormatError,
)

REQUEST_SCHEMA = "sudoku.request/1"
RESPONSE_SCHEMA = "sudoku.response/1"

OPERATIONS = (
    "capabilities", "generate_grid", "generate_puzzle", "solve_search", "count_solutions",
    "solve_logical", "verify", "classify", "generate_targeted",
)

# Early-validation ranges. They mirror the engine's documented limits; the engine
# is authoritative (its `capabilities` operation reports the real values).
MAX_SEED = 2**64 - 1
MAX_NODE_BUDGET = 2**53 - 1
MAX_COUNT_LIMIT = 100000
MAX_ATTEMPTS = 1000000
MIN_CLUES, MAX_CLUES = 17, 81
MAX_ID_LENGTH = 128
BOARD_LENGTH = 81

# Exit codes of the engine process.
EXIT_SUCCESS, EXIT_NON_SUCCESS, EXIT_PROTOCOL_ERROR, EXIT_INTERNAL_ERROR = 0, 1, 2, 3

_BOARD_INPUT = re.compile(r"[1-9.0]{81}")
_BOARD_OUTPUT = re.compile(r"[1-9.]{81}")
_DECIMAL = re.compile(r"0|[1-9][0-9]{0,19}")


# ------------------------------------------------------------- validation

def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def check_int(value: Any, name: str, lo: int, hi: int) -> int:
    if not _is_int(value):
        raise InvalidArgumentError(f"{name} must be an int, got {type(value).__name__}")
    if not lo <= value <= hi:
        raise InvalidArgumentError(f"{name} must be in {lo}..{hi}, got {value}")
    return value


def encode_seed(seed: Any) -> str:
    """The protocol writes 64-bit seeds as decimal strings (never as JSON numbers)."""
    return str(check_int(seed, "seed", 0, MAX_SEED))


def decode_seed(text: Any, where: str) -> int:
    """Parses a decimal-string seed from a response, exactly."""
    if not isinstance(text, str) or not _DECIMAL.fullmatch(text) or int(text) > MAX_SEED:
        raise ResponseFormatError(f"{where} is not a valid decimal 64-bit seed: {text!r}")
    return int(text)


def check_board(board: Any, name: str) -> str:
    """Boards are strings of 81 characters: 1-9, with '.' or '0' for empty cells."""
    if not isinstance(board, str):
        raise InvalidArgumentError(f"{name} must be a str of {BOARD_LENGTH} characters, got {type(board).__name__}")
    if len(board) != BOARD_LENGTH:
        raise InvalidArgumentError(f"{name} must have exactly {BOARD_LENGTH} characters, got {len(board)}")
    if not _BOARD_INPUT.fullmatch(board):
        bad = next(i for i, c in enumerate(board) if c not in "123456789.0")
        raise InvalidArgumentError(f"{name} has an invalid character {board[bad]!r} at index {bad} (allowed: 1-9, '.', '0')")
    return board


def check_response_board(board: Any, where: str, *, complete: bool) -> str:
    if not isinstance(board, str) or not _BOARD_OUTPUT.fullmatch(board):
        raise ResponseFormatError(f"{where} is not a valid board string")
    if complete and "." in board:
        raise ResponseFormatError(f"{where} must be a complete board")
    return board


def check_request_id(request_id: Any) -> str | None:
    if request_id is None:
        return None
    # The engine measures the id in UTF-8 bytes (equal to characters for ASCII), so the early check does too.
    try:
        size = len(request_id.encode("utf-8")) if isinstance(request_id, str) else 0
    except UnicodeEncodeError:   # a lone surrogate cannot be sent as UTF-8
        size = 0
    if not 1 <= size <= MAX_ID_LENGTH:
        raise InvalidArgumentError(f"request_id must be a str of 1..{MAX_ID_LENGTH} UTF-8 bytes (valid Unicode)")
    return request_id


# ---------------------------------------------------------------- requests

def build_request(op: str, params: Mapping[str, Any] | None = None, request_id: str | None = None) -> dict[str, Any]:
    """The request object for ``op``. Only the given params are included, so engine defaults apply."""
    if op not in OPERATIONS:
        raise InvalidArgumentError(f"unknown operation {op!r}; supported: {', '.join(OPERATIONS)}")
    request: dict[str, Any] = {"schema": REQUEST_SCHEMA, "op": op}
    if check_request_id(request_id) is not None:
        request["id"] = request_id
    if params is not None:
        request["params"] = dict(params)
    return request


def serialize_request(request: Mapping[str, Any]) -> bytes:
    """One JSON document as UTF-8. NaN/Infinity cannot be serialized; non-ASCII is escaped."""
    try:
        return json.dumps(request, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise InvalidArgumentError(f"request cannot be serialized as JSON: {exc}") from exc


# --------------------------------------------------------------- responses

@dataclass(frozen=True)
class Envelope:
    """A validated successful-envelope response (``ok`` true)."""

    op: str
    request_id: str | None
    success: bool
    status: str
    result: dict[str, Any]
    exit_code: int
    stderr: str = ""


def _reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)):
        raise ValueError("duplicate key in JSON object")
    return dict(pairs)


def _no_constant(name: str) -> Any:
    raise ValueError(f"non-standard JSON constant {name}")


def loads_strict(text: str) -> Any:
    """json.loads that rejects duplicate keys and NaN/Infinity, as the engine does."""
    return json.loads(text, object_pairs_hook=_reject_duplicates, parse_constant=_no_constant)


def parse_process_output(*, exit_code: int, stdout: bytes, stderr: bytes, op: str,
                         request_id: str | None) -> Envelope:
    """Interprets one finished engine process.

    Returns the validated :class:`Envelope` for any ``ok`` response (including
    non-success engine outcomes, exit code 1). Raises for everything else:
    :class:`RequestRejectedError` (exit 2 with a protocol error),
    :class:`EngineInternalError` (exit 3), :class:`EngineProcessError`
    (undefined exit code, or nonzero exit without a valid response) and
    :class:`ResponseFormatError` (a normally-exiting process with an unusable
    or inconsistent response).
    """
    if exit_code not in (EXIT_SUCCESS, EXIT_NON_SUCCESS, EXIT_PROTOCOL_ERROR, EXIT_INTERNAL_ERROR):
        raise EngineProcessError("engine ended with an exit code the protocol does not define", exit_code, stdout, stderr)

    def unusable(message: str) -> Exception:
        # A nonzero exit without a valid response is a process failure; a clean
        # exit with a bad response is a format violation.
        if exit_code != EXIT_SUCCESS:
            return EngineProcessError(message, exit_code, stdout, stderr)
        return ResponseFormatError(message, stdout, stderr)

    if not stdout.strip():
        raise unusable("engine produced no output")
    try:
        text = stdout.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise unusable(f"engine output is not valid UTF-8 ({exc.reason})") from exc
    body = text[:-1] if text.endswith("\n") else text
    if "\n" in body or "\r" in body:
        raise unusable("engine output is not exactly one JSON line")
    try:
        doc = loads_strict(body)
    except ValueError as exc:
        raise unusable(f"engine output is not valid JSON ({exc})") from exc
    except RecursionError as exc:   # absurdly deep nesting is malformed output, not a crash of the client
        raise unusable("engine output is nested too deeply to be a response") from exc

    # From here on the output is JSON; any inconsistency is a format violation.
    def bad(message: str) -> ResponseFormatError:
        return ResponseFormatError(message, stdout, stderr)

    if not isinstance(doc, dict):
        raise bad("response is not a JSON object")
    if doc.get("schema") != RESPONSE_SCHEMA:
        raise bad(f"unexpected response schema {doc.get('schema')!r} (expected {RESPONSE_SCHEMA!r})")
    ok = doc.get("ok")
    if not isinstance(ok, bool):
        raise bad("response has no boolean 'ok'")
    sent_id = request_id
    got_id = doc.get("id")
    if got_id is not None and not isinstance(got_id, str):
        raise bad("response 'id' is not a string")
    stderr_text = stderr.decode("utf-8", errors="replace")

    if not ok:
        allowed = {"schema", "op", "id", "ok", "error"}
        if set(doc) - allowed or "error" not in doc:
            raise bad("error response has missing or unexpected fields")
        error = doc["error"]
        if (not isinstance(error, dict) or not isinstance(error.get("code"), str) or not isinstance(error.get("message"), str)
                or set(error) - {"code", "message", "field", "position"}):
            raise bad("malformed 'error' object")
        field, position = error.get("field"), error.get("position")
        if (field is not None and not isinstance(field, str)) or (position is not None and not _is_int(position)):
            raise bad("malformed 'error' object")
        if sent_id is not None and got_id is not None and got_id != sent_id:
            raise bad(f"response id {got_id!r} does not match the request id {sent_id!r}")
        if sent_id is None and got_id is not None:
            raise bad("error response contains an id although the request had none")
        if "op" in doc and doc["op"] != op:   # the engine omits 'op' when it could not read one
            raise bad(f"error response op {doc['op']!r} does not match the request op {op!r}")
        code = error["code"]
        if code == "internal_error":
            if exit_code != EXIT_INTERNAL_ERROR:
                raise bad("internal_error response with an inconsistent exit code")
            raise EngineInternalError(error["message"], request_id=got_id or sent_id, stderr=stderr)
        if exit_code != EXIT_PROTOCOL_ERROR:
            raise bad("protocol error response with an inconsistent exit code")
        raise RequestRejectedError(code, error["message"], field=field, position=position,
                                   request_id=got_id or sent_id, stderr=stderr)

    # ok == True
    if set(doc) - {"schema", "op", "id", "ok", "success", "status", "result"}:
        raise bad("response has unexpected fields")
    success, status, result = doc.get("success"), doc.get("status"), doc.get("result")
    if not isinstance(success, bool) or not isinstance(status, str) or not isinstance(result, dict):
        raise bad("response lacks a boolean 'success', string 'status' or object 'result'")
    if doc.get("op") != op:
        raise bad(f"response op {doc.get('op')!r} does not match the request op {op!r}")
    if sent_id is not None and got_id != sent_id:
        raise bad(f"response id {got_id!r} does not match the request id {sent_id!r}")
    if sent_id is None and got_id is not None:
        raise bad("response contains an id although the request had none")
    if exit_code not in (EXIT_SUCCESS, EXIT_NON_SUCCESS) or (exit_code == EXIT_SUCCESS) != success:
        raise bad("exit code is inconsistent with the response's 'success'")
    return Envelope(op=op, request_id=got_id, success=success, status=status, result=result,
                    exit_code=exit_code, stderr=stderr_text)
