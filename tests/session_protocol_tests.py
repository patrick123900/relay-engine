"""Background transport regression: no SDL, display, OS input or project writes."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
serial = 0


def call(channel, method, **fields):
    global serial
    serial += 1
    channel.write(json.dumps({"id": serial, "method": method, **fields}) + "\n")
    channel.flush()


def stdio_check(directory, grants, approved):
    env = {**os.environ, "RELAY_AGENT_GRANTS": grants}
    process = subprocess.Popen([binary, "--agent-stdio"], cwd=directory, env=env,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    requests = [dict(id=1, method="session.status"), dict(id=2, method="scene.create"),
                dict(id=3, method="scene.clear"), dict(id=4, method="session.audit")]
    try:
        output, _ = process.communicate("".join(json.dumps(r) + "\n" for r in requests), timeout=10)
        responses = [json.loads(line) for line in output.splitlines()]
        assert responses.pop(0)["event"] == "relay.ready"
        assert responses[0]["result"]["grants"] == (["scene.create"] if approved else [])
        assert responses[1]["ok"] == approved, responses
        assert not responses[2]["ok"], responses
        audit = responses[3]["result"]["entries"]
        assert audit[0]["scope"] == "session.grants"
        assert audit[-2]["allowed"] == approved and audit[-2]["succeeded"] == approved
        assert audit[-1]["destructive"] and not audit[-1]["allowed"]
        assert process.returncode == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


with tempfile.TemporaryDirectory(prefix="relay-session-") as directory:
    stdio_check(directory, "", False)
    stdio_check(directory, "scene.create", True)
    stdio_check(directory, "scene.create,*", False)
    stdio_check(directory, "scene.create,", False)
    stdio_check(directory, "trace.replay", False)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen([binary, "--agent-port", str(port)], cwd=directory,
                               env={**os.environ, "RELAY_AGENT_GRANTS": "scene.create,scene.list", "RELAY_AGENT_TOKEN": "a" * 64},
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                connection = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("loopback server did not start")
                time.sleep(.02)
        # Unauthenticated requests fail and do not reach native dispatch.
        with connection, connection.makefile("rw") as channel:
            call(channel, "scene.create", name="Unauthorized")
            assert not json.loads(channel.readline())["ok"]
        with socket.create_connection(("127.0.0.1", port), timeout=2) as rejected:
            with rejected.makefile("rw") as channel:
                call(channel, "session.authenticate", token="wrong-token")
                assert not json.loads(channel.readline())["ok"]
        connection = socket.create_connection(("127.0.0.1", port), timeout=2)
        with connection, connection.makefile("rw") as channel:
            call(channel, "session.authenticate", token="a" * 64)
            assert json.loads(channel.readline())["result"]["authenticated"]
            call(channel, "scene.create", name="Socket scope probe")
            assert json.loads(channel.readline())["ok"]
            call(channel, "scene.clear")
            assert not json.loads(channel.readline())["ok"]
            call(channel, "session.audit")
            assert len(json.loads(channel.readline())["result"]["entries"]) == 3
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            with connection.makefile("rw") as channel:
                call(channel, "session.authenticate", token="a" * 64)
                assert json.loads(channel.readline())["ok"]
                call(channel, "session.audit")
                audit = json.loads(channel.readline())["result"]
                assert len(audit["entries"]) == 1 and audit["entries"][0]["scope"] == "session.grants"
                call(channel, "scene.list")
                listed = json.loads(channel.readline())
                assert listed["ok"] and "Unauthorized" not in json.dumps(listed), listed
                # No wire request can approve or replace host grants.
                call(channel, "session.grant", method_name="scene.clear")
                assert not json.loads(channel.readline())["ok"]
        assert not list(Path(directory).rglob("*.relay.json"))
    finally:
        process.terminate()
        process.wait(timeout=5)
    missing_token = subprocess.run([binary, "--agent-port", str(port)], cwd=directory,
        env={**os.environ, "RELAY_AGENT_TOKEN": ""}, capture_output=True, timeout=5)
    assert missing_token.returncode != 0 and b"requires" in missing_token.stderr
print("Session stdio/loopback tests passed: default denial, exact grants, invalid config, connection isolation")
