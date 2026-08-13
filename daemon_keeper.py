"""Keep an ask-master BLE daemon alive so the BLE link stays warm.

The daemon shuts down when its stdin closes, so this holder starts it with an
open stdin pipe and waits. Logs are kept for troubleshooting.
Kill this process to stop the daemon.
"""
import subprocess
import time

LOG = r'd:\dev\WorkBuddy-Cardputer-BLE\daemon.log'

with open(LOG, 'wb') as log:
    proc = subprocess.Popen(
        [r'd:\dev\WorkBuddy-Cardputer-BLE\ask-master.exe', '--transport', 'ble',
         '--log-level', 'debug'],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=log,
        cwd=r'd:\dev\WorkBuddy-Cardputer-BLE',
    )

    try:
        while proc.poll() is None:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.terminate()
