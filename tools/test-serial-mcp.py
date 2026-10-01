"""Smoke test: start the serial MCP server and do a real MCP handshake.

This proves the server actually speaks MCP over stdio before we register it
in Codex's config.toml. It only starts the server, asks for its tool list,
and shuts it down. It does NOT open any serial port.

Usage:
    D:\\opencode\\venv-mcp1\\Scripts\\python.exe G:\\codex-workspace\\tools\\test-serial-mcp.py
"""

import json
import subprocess
import sys
import threading
import time

SERVER_PY = r"D:\opencode\venv-mcp1\Scripts\python.exe"
SERVER_ARGS = ["-m", "serial_mcp_server.server"]


def send(proc, payload):
    line = json.dumps(payload) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()


def main():
    print("starting server:", SERVER_PY, SERVER_ARGS)
    proc = subprocess.Popen(
        [SERVER_PY] + SERVER_ARGS,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )

    stderr_lines = []

    def drain_stderr():
        for line in proc.stderr:
            stderr_lines.append(line.rstrip())

    t = threading.Thread(target=drain_stderr, daemon=True)
    t.start()

    def read_response(timeout=15.0):
        result = {"value": None}

        def reader():
            try:
                result["value"] = proc.stdout.readline()
            except Exception as exc:  # noqa: BLE001
                result["value"] = f"<read error: {exc}>"

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        th.join(timeout)
        if th.is_alive():
            return None
        return result["value"]

    # 1) initialize
    send(proc, {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "codex-smoke-test", "version": "1.0"},
        },
    })
    init_line = read_response()
    print("--- initialize response ---")
    print(init_line)

    if not init_line:
        print("FAIL: no initialize response")
        print("stderr so far:")
        for line in stderr_lines[-30:]:
            print("  ", line)
        proc.kill()
        return 2

    try:
        parsed = json.loads(init_line)
        server_info = parsed.get("result", {}).get("serverInfo", {})
        print("server name   :", server_info.get("name"))
        print("server version:", server_info.get("version"))
    except Exception as exc:  # noqa: BLE001
        print("WARN: could not parse initialize response:", exc)

    # 2) initialized notification
    send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
    time.sleep(0.4)

    # 3) tools/list
    send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
    tools_line = read_response()
    print("--- tools/list response ---")

    tool_names = []
    if tools_line:
        try:
            parsed = json.loads(tools_line)
            tools = parsed.get("result", {}).get("tools", [])
            tool_names = [t.get("name") for t in tools]
            print("tool count:", len(tool_names))
            for name in tool_names:
                print("   ", name)
        except Exception as exc:  # noqa: BLE001
            print("WARN: could not parse tools/list:", exc)
            print(tools_line[:800])
    else:
        print("FAIL: no tools/list response")

    # 4) shut down cleanly
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:  # noqa: BLE001
        proc.kill()

    print("--- stderr tail ---")
    for line in stderr_lines[-15:]:
        print("  ", line)

    ok = bool(tool_names)
    print("")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
