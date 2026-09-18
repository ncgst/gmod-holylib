"""Exercise the production summary/curl helper against isolated loopback HTTP.

Run with Python 3 and curl; LUAJIT may override the bundled interpreter path.
No telemetry service, GitHub job, or game server is contacted.
"""

from contextlib import contextmanager
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("HOLYLIB_PREBUILDTOOLS", ROOT / "source/_prebuildtools"))
LUAJIT = Path(os.environ.get("LUAJIT", ROOT / "source/_prebuildtools" / ("luajit.exe" if os.name == "nt" else "luajit_64")))


def entry(calls=100, seconds=1, padding=""):
    return json.dumps({"totalCalls": calls, "totalTime": seconds, "gmodBranch": "fixture",
                       "name": "measured", "padding": padding}, separators=(",", ":")).encode()


def framed(*records):
    return b"".join(str(len(record)).encode() + b"\0" + record + b"\0" for record in records)


@dataclass
class Response:
    body: bytes
    status: int = 200
    split: int = 0
    delay: float = 0
    extra_length: int = 0


@contextmanager
def serving(responses):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            key = self.headers.get("entryIndex", self.path)
            response = responses.get(key, Response(b"", 404))
            self.send_response(response.status)
            self.send_header("Content-Length", str(len(response.body) + response.extra_length))
            self.end_headers()
            try:
                if response.split:
                    self.wfile.write(response.body[:response.split])
                    self.wfile.flush()
                    time.sleep(response.delay)
                    self.wfile.write(response.body[response.split:])
                else:
                    self.wfile.write(response.body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass  # A timeout/short-transfer test intentionally closes early.

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield "http://127.0.0.1:" + str(server.server_address[1])
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


class TelemetryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix=".telemetry-", dir=ROOT / "tests")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        # utils.lua's http directory reset is confined to this new test directory.
        self.assertTrue(self.directory.resolve().is_relative_to((ROOT / "tests").resolve()))
        for name in ("http.lua", "utils.lua", "json.lua", "_workflow_summary.lua"):
            shutil.copy2(TOOLS / name, self.directory / name)
        self.environment = dict(os.environ, NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")

    def run_lua(self, script, *arguments):
        return subprocess.run([str(LUAJIT.resolve()), script, *arguments], cwd=self.directory,
                              env=self.environment, capture_output=True, text=True, timeout=12)

    def summary(self, current, previous, *, automatic=False, older=None):
        responses = {"fixture/repo___304": current, "fixture/repo___303": previous}
        if older is not None:
            responses["fixture/repo___302"] = older
        with serving(responses) as host:
            result = self.run_lua("_workflow_summary.lua", "fixture/repo", host, "", "", "304",
                                  "0" if automatic else "303", "push")
        output = self.directory / "generated_summary.md"
        return result, output.read_text() if output.exists() else None

    def assert_failed(self, result, output, diagnostic):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(diagnostic, result.stdout + result.stderr)
        self.assertTrue(output is None or "| measured |" not in output, "failed input produced performance rows")

    def helper(self, code, responses):
        script = self.directory / "helper.lua"
        script.write_text('dofile("utils.lua")\njson = require("json")\nrequire("http")\nlocal host = ...\n' + code)
        with serving(responses) as host:
            return self.run_lua(script.name, host)

    def test_valid_summary_merges_all_records(self):
        result, output = self.summary(Response(framed(entry(100, 1), entry(300, 3))), Response(framed(entry(100, 2))))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("| measured | 4.00s | 400 | 0.01 | 2x |", output)

    def test_delayed_previous_response_is_read_after_curl_finishes(self):
        previous = framed(entry(100, 2, "x" * 32768))
        result, output = self.summary(Response(framed(entry())), Response(previous, split=16384, delay=0.8))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("| measured | 1.00s | 100 | 0.01 | 2x |", output)

    def test_missing_current_is_a_failure(self):
        result, output = self.summary(Response(b""), Response(framed(entry())))
        self.assert_failed(result, output, "Failed to fetch results")

    def test_missing_explicit_previous_is_a_failure(self):
        result, output = self.summary(Response(framed(entry())), Response(b""))
        self.assert_failed(result, output, "Failed to fetch results")

    def test_automatic_search_can_skip_absent_history(self):
        result, output = self.summary(Response(framed(entry())), Response(b""), automatic=True,
                                      older=Response(framed(entry(100, 2))))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Previous run: 302", output)
        self.assertIn("| measured |", output)

    def test_truncated_json_is_not_skipped(self):
        result, output = self.summary(Response(framed(entry())), Response(framed(b'{"name":"unfinished')))
        self.assert_failed(result, output, "expected closing quote")

    def test_truncated_framed_entry_is_a_failure(self):
        result, output = self.summary(Response(framed(entry())[:-8]), Response(framed(entry())))
        self.assert_failed(result, output, "Incomplete telemetry entry")

    def test_missing_terminator_cannot_publish_valid_json_prefix(self):
        result, output = self.summary(Response(framed(entry())[:-1]), Response(framed(entry())))
        self.assert_failed(result, output, "Incomplete telemetry entry")

    def test_invalid_length_is_a_failure(self):
        result, output = self.summary(Response(b"invalid\0" + entry() + b"\0"), Response(framed(entry())))
        self.assert_failed(result, output, "Invalid telemetry length")

    def test_http_error_cannot_publish_even_valid_telemetry(self):
        result, output = self.summary(Response(framed(entry()), status=503), Response(framed(entry())))
        self.assert_failed(result, output, "curl did not complete successfully")

    def test_short_http_transfer_cannot_publish_valid_body(self):
        result, output = self.summary(Response(framed(entry()), extra_length=50), Response(framed(entry())))
        self.assert_failed(result, output, "curl did not complete successfully")

    def test_sync_request_completes_once(self):
        result = self.helper('''
local successes = 0
HTTP({url = host .. "/value", mode = "sync",
    success = function(body) successes = successes + 1 assert(json.decode(body).value == 7) end,
    failed = function(reason) error(reason) end})
assert(successes == 1)
HTTP_WaitForAll()
assert(successes == 1)
''', {"/value": Response(b'{"value":7}')})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_async_callback_can_enqueue_another_request(self):
        result = self.helper('''
local successes = 0
local function complete(body) successes = successes + 1 assert(json.decode(body).value == 7) end
HTTP({url = host .. "/value", success = function(body)
    complete(body)
    HTTP({url = host .. "/value", success = complete, failed = error})
end, failed = error})
HTTP({url = host .. "/value", success = complete, failed = error})
HTTP_WaitForAll()
assert(successes == 3)
''', {"/value": Response(b'{"value":7}')})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_timeout_rejects_partial_output(self):
        result = self.helper('''
HTTP({url = host .. "/value", timeout = 1,
    success = function() error("partial output was accepted") end,
    failed = function(reason) error(reason) end})
HTTP_WaitForAll()
''', {"/value": Response(b'{"value":7}', split=4, delay=1.5)})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("curl did not complete successfully", result.stdout + result.stderr)
        self.assertNotIn("partial output was accepted", result.stdout + result.stderr)

    def test_failed_download_does_not_accept_stale_file(self):
        result = self.helper('''
WriteFile("download.json", '{"value":999}')
local successes, failures = 0, 0
HTTPDownload({url = host .. "/value", file = "download.json",
    success = function() successes = successes + 1 end,
    failed = function() failures = failures + 1 end})
HTTP_WaitForAll()
assert(successes == 0 and failures == 1)
''', {"/value": Response(b'{"value":7}', status=503)})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
