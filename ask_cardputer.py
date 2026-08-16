#!/usr/bin/env python3
"""Send a question to the Cardputer via the ask-master daemon's MCP TCP server
and wait for a reply.  Call this from the command line when the AI assistant
needs human input.

Usage:
    python ask_cardputer.py confirm "Are you sure?" "It will delete files"
    python ask_cardputer.py choose "How to handle?" "Push" "Skip" "Edit"
    python ask_cardputer.py ask-human "What commit message?" --context "..."
    python ask_cardputer.py escalate "URGENT: Server is down!" --context "..."

Exit code: 0 on success, 1 on error/timeout/offline.
The reply (or a [CARDPUTER ...] fallback message) is printed to stdout.
"""

import sys
import json
import socket
import uuid
import time

DAEMON_ADDR = "127.0.0.1"
DAEMON_PORT = 51937


def _send_recv(conn, msg, timeout=120):
    """Send a JSON-RPC message and read the corresponding response.

    Handles notifications (JSON objects without matching id) by skipping them.
    """
    data = json.dumps(msg).encode("utf-8")
    conn.sendall(data + b"\n")
    conn.settimeout(timeout)

    # Read until we get a complete JSON-RPC response matching our id.
    buf = ""
    resp_id = msg.get("id")
    while True:
        try:
            chunk = conn.recv(4096)
        except socket.timeout:
            raise TimeoutError("timed out waiting for response")
        if not chunk:
            raise ConnectionError("daemon closed connection")
        buf += chunk.decode("utf-8", "replace")
        # Try to extract complete JSON objects from the buffer.
        # Multiple objects may arrive in one chunk (e.g. notification + response).
        while True:
            try:
                obj, end = json.JSONDecoder().raw_decode(buf)
                buf = buf[end:]  # remove parsed object from buffer
                if obj.get("id") == resp_id:
                    return obj
                # Otherwise it's a notification (or wrong id); skip it.
            except (json.JSONDecodeError, UnicodeDecodeError):
                break  # need more data


def call_tool(tool_name, arguments, timeout_ms=60000):
    """Connect to the daemon, initialize MCP session, call a tool, and return
    the result text."""
    conn = socket.create_connection((DAEMON_ADDR, DAEMON_PORT), timeout=10)
    try:
        # 1. Initialize
        init_id = str(uuid.uuid4())
        init_req = {
            "jsonrpc": "2.0",
            "id": init_id,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "ask-cardputer", "version": "1.0"},
            },
        }
        _send_recv(conn, init_req)

        # 2. Send initialized notification
        notif = {
            "jsonrpc": "2.0",
            "method": "notifications/initialized",
        }
        conn.sendall(json.dumps(notif).encode("utf-8") + b"\n")

        # 3. Call the tool
        call_id = str(uuid.uuid4())
        call_req = {
            "jsonrpc": "2.0",
            "id": call_id,
            "method": "tools/call",
            "params": {
                "name": tool_name,
                "arguments": {**arguments, "timeout": timeout_ms},
            },
        }
        resp = _send_recv(conn, call_req)

        # 4. Parse result
        if "error" in resp:
            print(f"[CARDPUTER ERROR] {resp['error']['message']}", file=sys.stderr)
            return None

        result = resp.get("result", {})
        if result.get("isError"):
            text = (result.get("content") or [{}])[0].get("text", "unknown error")
            print(text, file=sys.stderr)
            return None

        text = "".join(
            c.get("text", "")
            for c in (result.get("content") or [])
            if c.get("type") == "text"
        )
        return text

    finally:
        conn.close()


def _parse_flag(name, default=""):
    """Return the value of --<name> from sys.argv, or default."""
    if name in sys.argv:
        idx = sys.argv.index(name)
        if idx + 1 < len(sys.argv):
            return sys.argv[idx + 1]
    return default


_KNOWN_FLAGS = {"--context"}


def _collect_positional(start):
    """Return positional args from sys.argv[start:] skipping known flags and their values."""
    result = []
    skip = 0
    for i, a in enumerate(sys.argv[start:]):
        if skip > 0:
            skip -= 1
            continue
        if a in _KNOWN_FLAGS:
            skip = 1
            continue
        result.append(a)
    return result


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    tool = sys.argv[1]
    args = {}
    context = _parse_flag("--context")

    if tool == "confirm":
        args["statement"] = sys.argv[2]
        if len(sys.argv) > 3:
            args["consequence"] = sys.argv[3]
        if context:
            args["context"] = context
    elif tool == "choose":
        args["question"] = sys.argv[2]
        if context:
            args["context"] = context
        opts = _collect_positional(3)
        if not opts:
            print("error: choose requires at least 2 options", file=sys.stderr)
            sys.exit(1)
        args["options"] = opts
    elif tool in ("ask-human", "escalate"):
        args["question"] = sys.argv[2]
        if context:
            args["context"] = context
    else:
        print(f"unknown tool: {tool}", file=sys.stderr)
        print("Valid: confirm, choose, ask-human, escalate", file=sys.stderr)
        sys.exit(1)

    result = call_tool(tool, args)
    if result is None:
        sys.exit(1)

    print(result)


if __name__ == "__main__":
    main()