"""Run the generated MCP capture surface against the headless runtime."""
import json
import os
from pathlib import Path
import select
import subprocess
import time

root = Path(__file__).resolve().parents[1]
env = {**os.environ, "RELAY_RUNTIME_MODE": "headless", "RELAY_AGENT_GRANTS": "scene.create,scene.copy,scene.paste,scene.destroy_many,render.capture,render.capture_async,render.capture_status,editor.camera.status,editor.camera.set,editor.camera.frame"}
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
    assert {"editor_camera_status", "editor_camera_set", "editor_camera_frame"} <= names
    for camera_tool in ("editor_camera_status", "editor_camera_set"):
        camera_result, camera_error = tool(camera_tool, {})
        assert camera_result.get("isError") and "requires a live editor" in str(camera_error), (camera_result, camera_error)
    assert len(names) == 72 and "render_capture_cancel" in names and "scene_pick" in names
    assert {"scene_duplicate", "scene_clear", "scene_copy", "scene_cut", "scene_paste",
            "scene_transform_many", "project_create", "project_open", "animation_clip",
            "scene_set_animations"} <= names
    assert {"session_grant", "session_decide", "session_revoke", "chat_submit", "bridge_poll", "bridge_publish"}.isdisjoint(names), names
    assert "session_request" in names
    _, grants = tool("session_status", {})
    assert "scene.create" in grants["grants"], grants
    result, denied = tool("scene_clear", {})
    assert result.get("isError") and "capability denied" in str(denied), (result, denied)
    _, audit = tool("session_audit", {})
    assert audit["entries"][-1]["scope"] == "scene.clear" and not audit["entries"][-1]["allowed"], audit
    _, created = tool("scene_create", {"name": "MCP clipboard probe"})
    probe = created["entity"]
    _, copied = tool("scene_copy", {"entities": [probe]})
    assert copied["copied"] == 1, copied
    _, pasted = tool("scene_paste", {})
    assert len(pasted["roots"]) == 1 and pasted["roots"][0] != probe, pasted
    _, deleted = tool("scene_destroy_many", {"entities": [probe, pasted["roots"][0]]})
    assert "history" in deleted, deleted
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
    result, failure = tool("render_capture", {"filename": "mcp-no-gpu-sync.png", "source": "vulkan"})
    assert result.get("isError") and "Vulkan" in str(failure), (result, failure)
    print(f"MCP capture smoke passed: {len(names)} tools, deterministic provenance, "
          "GPU request fails closed")
finally:
    process.stdin.close()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)

# Even with display variables present, default startup stays headless and ungranted.
default_env = {**os.environ, "RELAY_AGENT_GRANTS": "", "DISPLAY": ":65535", "WAYLAND_DISPLAY": ""}
default_env.pop("RELAY_RUNTIME_MODE", None)
default_env.pop("WAYLAND_SOCKET", None)
process = subprocess.Popen(["node", "tools/mcp-bridge/dist/index.js"], cwd=root, env=default_env,
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
try:
    call("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                        "clientInfo": {"name": "relay-session-smoke", "version": "1"}})
    process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
    process.stdin.flush()
    _, status = tool("session_status", {})
    assert status["grants"] == [], status
    result, denied = tool("runtime_status", {})
    assert result.get("isError") and "capability denied" in str(denied), (result, denied)
    result, denied = tool("render_capture", {"filename": "mcp-ungranted.png", "source": "vulkan"})
    assert result.get("isError") and "capability denied" in str(denied), (result, denied)
    print("MCP default session smoke passed: headless startup, no grants, capture cannot bypass policy")
finally:
    process.stdin.close()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)
