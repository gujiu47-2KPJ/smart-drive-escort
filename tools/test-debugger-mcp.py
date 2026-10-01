"""Smoke test: start the embedded debugger MCP server and do a real MCP handshake.

Proves the server actually speaks MCP over stdio before we rely on it.
It only starts the server, asks for its tool list, and shuts it down.
It does NOT touch any hardware or open a debug session.

Usage:
    python test-debugger-mcp.py
"""

import json
import subprocess
import sys
import threading
import time

SERVER = r"D:\Rust\bin\embedded-debugger-mcp.exe"
SERVER_ARGS = ["serve"]


def send(proc, payload):
    proc.stdin.write(json.dumps(payload) + "\n")
    proc.stdin.flush()


def main():
    print("starting server:", SERVER, SERVER_ARGS)
    proc = subprocess.Popen(
        [SERVER] + SERVER_ARGS,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    stderr_lines = []

    def drain_stderr():
        try:
            for line in proc.stderr:
                stderr_lines.append(line.rstrip())
        except Exception:
            pass

    threading.Thread(target=drain_stderr, daemon=True).start()

    def read_response(timeout=20.0):
        box = {"v": None}

        def reader():
            try:
                box["v"] = proc.stdout.readline()
            except Exception as exc:  # noqa: BLE001
                box["v"] = f"<read error: {exc}>"

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        th.join(timeout)
        return None if th.is_alive() else box["v"]

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
    print((init_line or "").strip()[:400] or "(no response)")

    if not init_line:
        print("FAIL: no initialize response")
        for line in stderr_lines[-20:]:
            print("  stderr:", line)
        proc.kill()
        return 2

    try:
        info = json.loads(init_line).get("result", {}).get("serverInfo", {})
        print("server name   :", info.get("name"))
        print("server version:", info.get("version"))
    except Exception as exc:  # noqa: BLE001
        print("WARN: could not parse initialize:", exc)

    # 2) initialized notification
    send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
    time.sleep(0.5)

    # 3) tools/list
    send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
    tools_line = read_response()
    print("--- tools/list response ---")

    names = []
    if tools_line:
        try:
            tools = json.loads(tools_line).get("result", {}).get("tools", [])
            names = [t.get("name") for t in tools]
            print("tool count:", len(names))
            for n in names:
                print("   ", n)
        except Exception as exc:  # noqa: BLE001
            print("WARN: could not parse tools/list:", exc)
            print(tools_line[:500])
    else:
        print("FAIL: no tools/list response")

    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

    if stderr_lines:
        print("--- stderr tail ---")
        for line in stderr_lines[-8:]:
            print("  ", line)

    ok = bool(names)
    print("")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
