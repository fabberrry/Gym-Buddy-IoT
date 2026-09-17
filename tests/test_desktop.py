import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("desktop_gateway", ROOT / "desktop/people_counter_gateway.py")
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)
SCHEMA = json.loads((ROOT / "docs/room-state.schema.json").read_text())


def validate(state):
    # Deliberately limited to every keyword used by our small schema; no dependency.
    assert set(SCHEMA["required"]) <= set(state) <= set(SCHEMA["properties"])
    for key, value in state.items():
        rule = SCHEMA["properties"][key]
        if "const" in rule:
            assert type(value) is int and value == rule["const"]
        if "enum" in rule:
            assert value in rule["enum"]
        kind = rule.get("type")
        if kind == "integer":
            assert type(value) is int
        elif kind == "string":
            assert isinstance(value, str) and re.fullmatch(rule["pattern"], value)
        elif isinstance(kind, list):
            assert value is None or type(value) in (int, float)
        if value is not None:
            if "minimum" in rule:
                assert value >= rule["minimum"]
            if "maximum" in rule:
                assert value <= rule["maximum"]


class Integration(unittest.TestCase):
    def run_scenario(self, name, count=0):
        result = subprocess.run([str(BINARY), "--scenario", name, "--initial-count", str(count),
                                 "--epoch-ms", "1725800000000", "--session-id", "test-session"],
                                capture_output=True, text=True, check=True, timeout=10)
        states = [json.loads(line) for line in result.stdout.splitlines()]
        for state in states:
            validate(state)
        self.assertTrue(all(a["sequence"] < b["sequence"] for a, b in zip(states, states[1:])))
        return states, result.stderr.splitlines()

    def test_all_scenarios(self):
        expected = {"empty": (0, []), "entry": (1, ["ENTRY count=1"]),
                    "exit": (0, ["EXIT count=0"]), "entries": (3, ["ENTRY count=1", "ENTRY count=2", "ENTRY count=3"]),
                    "exits": (0, ["EXIT count=0"] * 3), "noise": (1, ["ENTRY count=1"]),
                    "demo": (1, ["ENTRY count=1", "ENTRY count=2", "AMBIGUOUS count=2", "EXIT count=1"])}
        for name in ("close-following", "reversal", "blockage", "overlap", "invalid"):
            expected[name] = (0, ["AMBIGUOUS count=0"])
        for name, (count, events) in expected.items():
            with self.subTest(name=name):
                states, actual = self.run_scenario(name)
                self.assertEqual(actual, events)
                self.assertEqual(states[-1]["count"], count)
        states, _ = self.run_scenario("exit", 4)
        self.assertEqual(states[-1]["count"], 3)

    def test_deterministic_json(self):
        self.assertEqual(self.run_scenario("demo"), self.run_scenario("demo"))

    def test_invalid_cli(self):
        for args in (["--scenario", "bogus"], ["--initial-count", "-1"],
                     ["--interval-ms", "0"], ["--device-id", 'bad"id']):
            self.assertNotEqual(subprocess.run([str(BINARY)] + args, capture_output=True).returncode, 0)

    def test_configuration(self):
        config = gateway.load_config(ROOT / "config/gateway.json")
        self.assertEqual(config["host"], "0.0.0.0")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            for key, value in (("port", -1), ("mode", "mqtt"), ("deviceId", ""), ("initialCount", True)):
                bad = dict(config, **{key: value})
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):
                    gateway.load_config(path)

    def test_health_endpoint(self):
        store = gateway.LatestState(5000)
        server = gateway.ThreadingHTTPServer(("127.0.0.1", 0), gateway.handler_for(store, ""))
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{server.server_port}/health", timeout=2) as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(json.load(response), {"status": "ok"})
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    def test_http_reconnect_auth_stale_and_source_failure(self):
        states, _ = self.run_scenario("demo")
        store = gateway.LatestState(5000)
        server = gateway.ThreadingHTTPServer(("127.0.0.1", 0), gateway.handler_for(store, "test-token"))
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        url = f"http://127.0.0.1:{server.server_port}/v1/state"

        def get(path=url, token="test-token"):
            request = urllib.request.Request(path, headers={"Authorization": "Bearer " + token})
            return urllib.request.urlopen(request, timeout=2)
        try:
            for token, code in (("wrong", 401), ("test-token", 503)):
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    get(token=token)
                self.assertEqual(caught.exception.code, code)
                caught.exception.close()
            store.publish(states[0])
            with get() as response:
                self.assertEqual(json.load(response), states[0])
                self.assertEqual(response.headers["Cache-Control"], "no-store")
            # Client disconnects; producer advances independently; fresh GET recovers.
            for state in states[1:]:
                store.publish(state)
            for _ in range(2):
                with get() as response:
                    self.assertEqual(json.load(response), states[-1])
            with self.assertRaises(urllib.error.HTTPError) as caught:
                get(url + "/missing")
            self.assertEqual(caught.exception.code, 404)
            caught.exception.close()
            store.received = time.monotonic() - 10
            with self.assertRaises(urllib.error.HTTPError) as caught:
                get()
            self.assertEqual(caught.exception.code, 503)
            caught.exception.close()
            store.publish(states[-1])
            with get() as response:
                self.assertEqual(response.status, 200)
            store.stopped()
            with self.assertRaises(urllib.error.HTTPError) as caught:
                get()
            self.assertEqual(caught.exception.code, 503)
            caught.exception.close()
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    def test_actual_gateway_process(self):
        # The real launcher, pipe reader, HTTP endpoint and live C++ producer.
        config = gateway.load_config(ROOT / "config/gateway.json")
        import socket
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            config["port"] = sock.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(json.dumps(config))
            process = subprocess.Popen([sys.executable, str(ROOT / "desktop/people_counter_gateway.py"),
                                        "--config", str(path), "--binary", str(BINARY)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 10
                seen = None
                while time.monotonic() < deadline:
                    try:
                        with urllib.request.urlopen(f"http://127.0.0.1:{config['port']}/v1/state", timeout=1) as response:
                            seen = json.load(response)
                            validate(seen)
                            if seen["event"] == "exit":
                                break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.05)
                self.assertIsNotNone(seen)
                self.assertEqual((seen["event"], seen["count"], seen["status"]), ("exit", 1, "uncertain"))
            finally:
                # Kill the complete process tree on Windows; SIGTERM otherwise.
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
                else:
                    process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
