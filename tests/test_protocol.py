import pytest

from diskrun.protocol import (
    BEGIN,
    escape_sharg,
    normalize_output,
    split_output,
    wrap_command,
)


def test_escape_plain():
    assert escape_sharg("hello") == "'hello'"


def test_escape_embedded_quote():
    assert escape_sharg("it's") == "'it'\\''s'"


def test_escape_injection_is_inert():
    evil = "'; rm -rf /; echo '"
    out = wrap_command(["sh", "-c", evil])
    # The payload survives only inside single-quote escaping, so the shell
    # never parses a bare rm command.
    assert "'\\''; rm -rf /; echo '\\''" in out


def test_wrap_command_shape():
    line = wrap_command(["./app", "--x", "1"], {"FOO": "bar"})
    assert line.startswith("stty -echo; echo @@BEGIN@@;")
    assert line.endswith("echo @@STATUS=$rc@@")
    assert "'env'" not in line
    assert "'FOO=bar'" in line
    assert "'./app'" in line and "'--x'" in line and "'1'" in line


def test_wrap_command_no_env():
    line = wrap_command(["true"])
    assert line == f"stty -echo; echo {BEGIN}; 'true'; rc=$?; echo @@STATUS=$rc@@".replace("echo @@BEGIN", "echo @@BEGIN")


def test_split_output_basic():
    chunk = b"echo @@BEGIN@@\r\nhello\r\nworld\r\n@@STATUS=7@@\r\n"
    out, rc = split_output(chunk)
    assert rc == 7
    assert out == b"hello\nworld"


def test_split_output_no_status():
    out, rc = split_output(b"junk @@STATUS=4")
    assert rc is None
    assert b"junk" in out


def test_normalize_output():
    assert normalize_output(b"a\r\nb\rc\nd") == b"a\nb\nc\nd"


def test_split_output_binary_payload():
    chunk = b"x @@BEGIN@@\x00\x01\xff\r@@STATUS=255@@"
    out, rc = split_output(chunk)
    assert rc == 255
    assert out == b"\x00\x01\xff"
