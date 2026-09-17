"""Desktop HTTP adapter. No sensor or crossing algorithm knowledge required."""
import argparse
import hmac
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]

ROOM_FIELDS = {"schemaVersion", "deviceId", "roomId", "sessionId", "sequence",
               "count", "event", "timestamp", "status"}
ROOM_IDENTIFIER = re.compile(r"[A-Za-z0-9._:-]{1,128}\Z")


def validate_room_state(state):
    """Fail closed before an untrusted producer line reaches HTTP clients."""
    if not isinstance(state, dict) or not ROOM_FIELDS <= state.keys() or not state.keys() <= ROOM_FIELDS | {"confidence"}:
        raise ValueError("Invalid RoomState fields")
    if type(state["schemaVersion"]) is not int or state["schemaVersion"] != 1:
        raise ValueError("Invalid RoomState version")
    for key in ("deviceId", "roomId", "sessionId"):
        if not isinstance(state[key], str) or not ROOM_IDENTIFIER.fullmatch(state[key]):
            raise ValueError("Invalid RoomState identity")
    for key, maximum in (("sequence", None), ("count", 4294967295), ("timestamp", None)):
        value = state[key]
        if type(value) is not int or value < 0 or (maximum is not None and value > maximum):
            raise ValueError("Invalid RoomState " + key)
    if state["event"] not in ("entry", "exit", "ambiguous", "none") or type(state["event"]) is not str:
        raise ValueError("Invalid RoomState event")
    if state["status"] not in ("valid", "uncertain") or type(state["status"]) is not str:
        raise ValueError("Invalid RoomState status")
    confidence = state.get("confidence")
    if confidence is not None and (type(confidence) not in (int, float) or
                                   not math.isfinite(confidence) or not 0 <= confidence <= 1):
        raise ValueError("Invalid RoomState confidence")


def load_config(path):
    config = json.loads(Path(path).read_text(encoding="utf-8"))
    expected = {"deviceId", "roomId", "mode", "host", "port", "authTokenEnv",
                "publishIntervalMs", "staleAfterMs", "initialCount", "scenario"}
    if set(config) != expected:
        raise ValueError("Configuration fields must match config/gateway.json")
    for key in ("deviceId", "roomId"):
        if not isinstance(config[key], str) or not re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", config[key]):
            raise ValueError("Invalid identity")
    for key, low, high in (("port", 1, 65535), ("initialCount", 0, 4294967295),
                           ("publishIntervalMs", 10, 3600000), ("staleAfterMs", 100, 86400000)):
        if type(config[key]) is not int or not low <= config[key] <= high:
            raise ValueError("Invalid " + key)
    if config["mode"] not in ("http", "console"):
        raise ValueError("Supported networking modes: console, http")
    for key in ("host", "authTokenEnv", "scenario"):
        if not isinstance(config[key], str):
            raise ValueError("Invalid " + key)
    if not config["host"] or config["staleAfterMs"] <= config["publishIntervalMs"]:
        raise ValueError("Host required; staleAfterMs must exceed publishIntervalMs")
    return config


class LatestState:
    def __init__(self, stale_ms=5000):
        self.lock = threading.Lock()
        self.payload = None
        self.received = 0
        self.alive = True
        self.stale_ms = stale_ms

    def publish(self, state):
        # Snapshot replacement is atomic and independent of slow HTTP consumers.
        validate_room_state(state)
        payload = json.dumps(state, separators=(",", ":"), allow_nan=False).encode("utf-8")
        with self.lock:
            self.payload = payload
            self.received = time.monotonic()

    def stopped(self):
        with self.lock:
            self.alive = False

    def read(self):
        with self.lock:
            if (not self.alive or self.payload is None or
                    (time.monotonic() - self.received) * 1000 > self.stale_ms):
                return None
            return self.payload


def handler_for(store, token=""):
    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(5)

        def log_message(self, *_):
            pass

        def reply(self, code, payload):
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(payload)))
            if code == 503:
                self.send_header("Retry-After", "1")
            if code == 401:
                self.send_header("WWW-Authenticate", "Bearer")
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError, TimeoutError):
                pass

        def do_GET(self):
            supplied = self.headers.get("Authorization", "").encode("utf-8")
            if token and not hmac.compare_digest(supplied, ("Bearer " + token).encode("utf-8")):
                self.reply(401, b'{"error":"unauthorized"}')
            elif self.path == "/health":
                self.reply(200, b'{"status":"ok"}')
            elif self.path != "/v1/state":
                self.reply(404, b'{"error":"not_found"}')
            else:
                payload = store.read()
                self.reply(200 if payload is not None else 503,
                           payload if payload is not None else b'{"error":"state_unavailable"}')

        def do_POST(self):
            self.reply(405, b'{"error":"method_not_allowed"}')
    return Handler


def command(config, binary, once=False):
    args = [str(binary), "--device-id", config["deviceId"], "--room-id", config["roomId"],
            "--scenario", config["scenario"], "--initial-count", str(config["initialCount"]),
            "--interval-ms", str(config["publishIntervalMs"])]
    if not once:
        args.append("--live")
    return args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default=str(ROOT / "config/gateway.json"))
    parser.add_argument("--binary", default=str(ROOT / "build" / ("people_counter_gateway.exe" if os.name == "nt" else "people_counter_gateway")))
    parser.add_argument("--scenario")
    parser.add_argument("--once", action="store_true", help="Accelerated console run; overrides mode")
    args = parser.parse_args()
    config = load_config(args.config)
    if args.scenario:
        config["scenario"] = args.scenario
    if args.once or config["mode"] == "console":
        return subprocess.call(command(config, args.binary, args.once))
    token = os.environ.get(config["authTokenEnv"], "") if config["authTokenEnv"] else ""
    if config["authTokenEnv"] and not token:
        raise ValueError("Configured authentication environment variable is empty")
    store = LatestState(config["staleAfterMs"])
    def stop(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    server = ThreadingHTTPServer((config["host"], config["port"]), handler_for(store, token))
    server.daemon_threads = True
    process = None
    try:
        process = subprocess.Popen(command(config, args.binary), stdout=subprocess.PIPE,
                                   text=True, encoding="utf-8")

        def consume():
            try:
                for line in process.stdout:
                    store.publish(json.loads(line))
            except (ValueError, OSError):
                print("Invalid or interrupted publisher stream", file=sys.stderr)
            finally:
                store.stopped()
        reader = threading.Thread(target=consume, daemon=True)
        reader.start()
        print(f"GET http://{config['host']}:{config['port']}/v1/state", flush=True)
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if process is not None:
            process.terminate()
            process.wait(timeout=5)
            reader.join(timeout=2)
            if process.stdout:
                process.stdout.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
