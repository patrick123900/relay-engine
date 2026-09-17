"""Run the generated MCP capture surface against the headless runtime."""
import json
import os
from pathlib import Path
import select
import subprocess
import time

root = Path(__file__).resolve().parents[1]
env = {**os.environ, "RELAY_RUNTIME_MODE": "headless"}
process = subprocess.Popen(["node", "tools/mcp-bridge/dist/index.js"], cwd=root, env=env,
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
serial = 0

def call(method, params):
    global serial
    serial += 1
    process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": serial, "method": method, "params": params}) + "\n")
    process.stdin.flush()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], 1)[0]:
            response = json.loads(process.stdout.readline())
            if response.get("id") == serial:
                assert "error" not in response, response
                return response["result"]
    raise TimeoutError(method)

def tool(name, arguments):
    result = call("tools/call", {"name": name, "arguments": arguments})
    content = result["content"][0]["text"]
    try:
        content = json.loads(content)
    except json.JSONDecodeError:
        pass
    return result, content

try:
    call("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                        "clientInfo": {"name": "relay-capture-smoke", "version": "1"}})
    process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
    process.stdin.flush()
    names = {tool["name"] for tool in call("tools/list", {})["tools"]}
    assert len(names) == 47 and "render_capture_cancel" in names and "scene_pick" in names
    _, captured = tool("render_capture_async", {"filename": "mcp-phase-e.png", "source": "deterministic"})
    assert captured["source"] == "deterministic", captured
    for _ in range(100):
        _, status = tool("render_capture_status", {"job": captured["job"]})
        if status["state"] == "complete":
            break
        assert status["state"] != "failed", status
        time.sleep(.02)
    assert status["state"] == "complete" and status["source"] == "deterministic", status
    result, failure = tool("render_capture_async", {"filename": "mcp-no-gpu.png", "source": "vulkan"})
    assert result.get("isError") and "Vulkan" in str(failure), (result, failure)
    print("MCP capture smoke passed: 47 tools, deterministic provenance, GPU request fails closed")
finally:
    process.stdin.close()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)
