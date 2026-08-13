"""
ask-master BLE transport proxy.

Bridges the Go daemon to the Cardputer over BLE (Nordic UART Service).

Protocol (line-delimited JSON on stdin/stdout):

  Go -> proxy (stdin):
    {"cmd":"send","payload":"<ask-master JSON payload>"}

  proxy -> Go (stdout):
    {"event":"ready"}                                  # proxy process alive
    {"event":"connected","connected":true|false}       # BLE link state
    {"event":"recv","line":"<one reply line from device>"}
    {"event":"log","msg":"..."}

The Cardputer reply protocol mirrors ask-master's WebSocket transport: the
device answers with a single line terminated by '\\n'.

The proxy reconnects indefinitely: the Cardputer stops advertising while it is
connected and resumes advertising after a disconnect (NimBLE default), so a
device reset or a dropped link must not kill this process.

Requires: pip install bleak
"""

import asyncio
import json
import sys

DEVICE_NAME = "Claude AskMaster"
RX_CHAR = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
TX_CHAR = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

SCAN_TIMEOUT = 10.0
RETRY_DELAY = 2.0


class _Shutdown(Exception):
    """Raised when the daemon closes stdin."""


def _force_utf8_stdio():
    """Pipes must be UTF-8 regardless of the platform's default encoding.

    On Windows the default stdio encoding is the ANSI code page (e.g. cp936),
    so a UTF-8 payload from the daemon would be mis-decoded and rejected as a
    bad command. Text is exchanged as UTF-8 in both directions.
    """
    for stream in (sys.stdin, sys.stdout):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def emit(obj):
    # ensure_ascii keeps every line pure ASCII, so the daemon always receives
    # parsable JSON even if the console encoding is something else entirely.
    sys.stdout.write(json.dumps(obj, ensure_ascii=True) + "\n")
    sys.stdout.flush()


async def stdin_reader(cmd_q):
    """Read commands from the daemon forever, independent of BLE link state."""
    loop = asyncio.get_running_loop()
    while True:
        raw = await loop.run_in_executor(None, sys.stdin.readline)
        if not raw:
            await cmd_q.put(None)  # stdin closed -> shut down
            return
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except Exception:
            emit({"event": "log", "msg": "bad cmd: %r" % raw[:80]})
            continue
        await cmd_q.put(msg)


async def pump(client, cmd_q):
    """Forward queued commands to the device until the link drops."""
    while client.is_connected:
        try:
            msg = await asyncio.wait_for(cmd_q.get(), timeout=1.0)
        except asyncio.TimeoutError:
            continue
        if msg is None:
            raise _Shutdown()
        if msg.get("cmd") != "send":
            continue
        payload = msg.get("payload", "")
        try:
            # The device splits incoming messages on '\n'; the newline is required.
            await client.write_gatt_char(RX_CHAR, (payload + "\n").encode("utf-8"))
        except Exception as e:
            emit({"event": "log", "msg": "write error: %r" % e})
            return  # treat write failure as a dropped link


async def main():
    _force_utf8_stdio()
    emit({"event": "ready"})

    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        emit({"event": "log", "msg": "bleak not installed; run: pip install bleak"})
        emit({"event": "connected", "connected": False})
        return

    cmd_q = asyncio.Queue()
    reader = asyncio.create_task(stdin_reader(cmd_q))

    try:
        while True:
            if reader.done():
                return

            emit({"event": "log", "msg": "scanning for %s" % DEVICE_NAME})
            device = None
            try:
                device = await BleakScanner.find_device_by_name(
                    DEVICE_NAME, timeout=SCAN_TIMEOUT
                )
            except Exception as e:
                emit({"event": "log", "msg": "scan error: %r" % e})

            if device is None:
                emit({"event": "connected", "connected": False})
                await asyncio.sleep(RETRY_DELAY)
                continue

            emit({"event": "log", "msg": "found %s" % device.address})

            rxbuf = b""

            def on_notify(_sender, data):
                nonlocal rxbuf
                rxbuf += data
                while b"\n" in rxbuf:
                    line, rxbuf = rxbuf.split(b"\n", 1)
                    emit({"event": "recv", "line": line.decode("utf-8", "replace")})

            try:
                async with BleakClient(device.address) as client:
                    # Subscribe before announcing the link so no reply is missed.
                    await client.start_notify(TX_CHAR, on_notify)
                    emit({"event": "connected", "connected": True})
                    await pump(client, cmd_q)
            except _Shutdown:
                emit({"event": "connected", "connected": False})
                return
            except Exception as e:
                emit({"event": "log", "msg": "connection error: %r" % e})

            emit({"event": "connected", "connected": False})
            await asyncio.sleep(RETRY_DELAY)
    finally:
        reader.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except Exception as e:
        emit({"event": "log", "msg": "fatal: %r" % e})
        emit({"event": "connected", "connected": False})
