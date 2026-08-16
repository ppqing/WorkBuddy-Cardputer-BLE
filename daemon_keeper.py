"""Keep an ask-master BLE daemon alive so the BLE link stays warm.

The daemon shuts down when its stdin closes, so this holder starts it with an
open stdin pipe and waits. Logs are kept for troubleshooting.
Kill this process to stop the daemon.
"""
import os
import subprocess
import time

PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(PROJECT_DIR, 'daemon.log')

# 状态文件（锁文件）默认落在 %LOCALAPPDATA%\ask-master。某些受限环境
# 不允许写入该目录，这里允许通过 XDG_RUNTIME_DIR 覆盖为项目内目录。
STATE_DIR = os.path.join(PROJECT_DIR, '.state')
os.makedirs(STATE_DIR, exist_ok=True)
os.environ.setdefault('XDG_RUNTIME_DIR', STATE_DIR)

with open(LOG, 'wb') as log:
    proc = subprocess.Popen(
        [os.path.join(PROJECT_DIR, 'ask-master.exe'), '--transport', 'ble',
         '--log-level', 'debug'],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=log,
        cwd=PROJECT_DIR,
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
