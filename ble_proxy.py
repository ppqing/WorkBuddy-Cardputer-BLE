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

Voice (P1): while the device shows an ask/escalate prompt, holding Ctrl records
the built-in MEMS mic (ES8311). Audio is streamed as IMA-ADPCM in JSON frames:

    {"evt":"audio","seq":N,"data":"<base64 ADPCM>"}
    {"evt":"audio_end","seq":N,"len":<PCM samples>,"rate":16000}

On audio_end the proxy decodes ADPCM back to 16 kHz mono PCM, optionally
transcribes it with faster-whisper, and injects the result back into the daemon
as a normal input reply:

    {"cmd":"input","id":"<prompt id>","text":"<transcribed text>"}

If faster-whisper is not installed the audio is saved as a .wav file and a
fallback text reply points at it.

Requires: pip install bleak  (voice: pip install faster-whisper)
"""

import asyncio
import base64
import json
import os
import struct
import sys
import tempfile
import threading
import time
import wave

try:
    import ctypes
except ImportError:  # pragma: no cover - only on non-Windows
    ctypes = None

DEVICE_NAME = "Claude AskMaster"
RX_CHAR = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
TX_CHAR = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

SCAN_TIMEOUT = 10.0
RETRY_DELAY = 2.0
# 性能监控采样间隔（秒）。
# 收到 prompt 推送期间自动暂停（state["in_prompt"]），推送结束回到
# 主界面后恢复推送。避免 metrics 与 prompt 命令争抢 BLE 写通道。
METRICS_INTERVAL = 2.0

AUDIO_SAMPLE_RATE = 16000
AUDIO_DIR = os.path.join(tempfile.gettempdir(), "ask-master-audio")

# ---------------------------------------------------------------------------
# IMA-ADPCM decode (mirror of the firmware encoder in audio.cpp)
# ---------------------------------------------------------------------------

IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
IMA_INDEX = [-1, -1, -1, -1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]


def adpcm_decode(data):
    """Decode packed IMA-ADPCM (2 nibbles/byte, first sample in high nibble)
    back into a list of int16 samples."""
    out = []
    pred = 0
    idx = 0
    for b in data:
        for nib in ((b >> 4) & 0xF, b & 0xF):
            step = IMA_STEP[idx]
            delta = step >> 3
            if nib & 4:
                delta += step
            if nib & 2:
                delta += step >> 1
            if nib & 1:
                delta += step >> 2
            if nib & 8:
                pred -= delta
            else:
                pred += delta
            pred = max(-32768, min(32767, pred))
            idx += IMA_INDEX[nib & 7]
            idx = max(0, min(88, idx))
            out.append(pred)
    return out


def write_wav(path, samples, rate=AUDIO_SAMPLE_RATE):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


# ---------------------------------------------------------------------------
# faster-whisper transcription (lazy, cached model)
# ---------------------------------------------------------------------------

# 语音识别模型。可选 tiny/base/small/medium/large-v3。
# 模型越大中文识别越准，但转写越慢、内存/磁盘占用越大。
WHISPER_MODEL = "medium"
WHISPER_DEVICE = "cuda"       # 有 NVIDIA GPU 用 cuda，否则用 cpu
WHISPER_COMPUTE = "float16"   # GPU: float16/int8_float16；CPU: int8
WHISPER_BEAM_SIZE = 5         # 5 质量更高（GPU 下速度足够快）
WHISPER_LANGUAGE = "zh"
# 简体中文引导：Whisper 中文训练数据繁体占比高，不引导容易输出繁体。
WHISPER_INITIAL_PROMPT = "以下是普通话的句子，请使用简体中文输出。"

_whisper_model = None
_whisper_lock = threading.Lock()

# 保存 os.add_dll_directory 的返回值，防止被 GC 导致 DLL 搜索路径失效。
_cublas_dll_dirs = []


def _setup_cuda_dll_path():
    """Ensure ctranslate2 can load cuBLAS (cublas64_12.dll).

    ctranslate2's pip wheel bundles cuDNN but NOT cuBLAS. On Windows the DLL
    comes from the `nvidia-cublas-cu12` package (site-packages/nvidia/cublas/bin),
    which is not on the default DLL search path. Prefer the ctranslate2 dir
    (in case it was copied there), else add the nvidia package dir.
    """
    try:
        import ctranslate2 as _ct

        ct_dir = os.path.dirname(_ct.__file__)
        if os.path.isfile(os.path.join(ct_dir, "cublas64_12.dll")):
            return True
    except Exception:
        pass
    try:
        import site

        for sp in site.getsitepackages():
            d = os.path.join(sp, "nvidia", "cublas", "bin")
            if os.path.isfile(os.path.join(d, "cublas64_12.dll")):
                handle = os.add_dll_directory(d)
                _cublas_dll_dirs.append(handle)  # 保持引用
                return True
    except Exception:
        pass
    return False


def _load_model():
    """Import faster_whisper and load the model (call under _whisper_lock)."""
    global _whisper_model
    if _whisper_model is not None:
        return
    _setup_cuda_dll_path()
    from faster_whisper import WhisperModel

    _whisper_model = WhisperModel(
        WHISPER_MODEL, device=WHISPER_DEVICE, compute_type=WHISPER_COMPUTE
    )


def _preload_model():
    """Load the whisper model in the background at startup.

    Importing faster-whisper + loading the model takes several seconds (and
    holds the GIL/import locks). Doing it eagerly here avoids a long stall
    (and a concurrent-import deadlock) on the first voice capture.
    """
    try:
        with _whisper_lock:
            _load_model()
        emit({"event": "log", "msg": "whisper: model preloaded (%s/%s)" % (WHISPER_DEVICE, WHISPER_COMPUTE)})
    except Exception as e:
        import traceback

        emit({"event": "log", "msg": "whisper preload error: %r" % e})
        emit({"event": "log", "msg": "whisper preload traceback: %s" % traceback.format_exc()})


def transcribe(path):
    """Transcribe a wav file. Returns text, or None if whisper is unavailable."""
    try:
        with _whisper_lock:
            _load_model()
            segments, _info = _whisper_model.transcribe(
                path,
                language=WHISPER_LANGUAGE,
                beam_size=WHISPER_BEAM_SIZE,
                initial_prompt=WHISPER_INITIAL_PROMPT,
                vad_filter=True,
            )
            return "".join(s.text for s in segments).strip()
    except Exception as e:
        emit({"event": "log", "msg": "whisper error: %r" % e})
        return None


# ---------------------------------------------------------------------------
# stdio plumbing
# ---------------------------------------------------------------------------


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


# ---------------------------------------------------------------------------
# 性能监控：采集 CPU / GPU / 内存 / 网速
# ---------------------------------------------------------------------------

_metrics_state = {"time": 0.0, "dn": 0, "up": 0}
_nvml_handle = None


def _get_nvml_handle():
    """返回 NVML 句柄（首次调用时初始化），失败返回 None。"""
    global _nvml_handle
    if _nvml_handle is not None:
        return _nvml_handle
    try:
        import pynvml

        pynvml.nvmlInit()
        _nvml_handle = pynvml.nvmlDeviceGetHandleByIndex(0)
    except Exception:
        _nvml_handle = False  # 标记为不可用，避免反复初始化
    return _nvml_handle if _nvml_handle else None


def collect_metrics():
    """采集一次性能数据，返回 dict（不含 type 字段，由调用方补充）。

    - cpu: CPU 使用率 %（-1 表示不可用）
    - gpu: GPU 使用率 %（-1 表示无 GPU/不可用）
    - mem: 内存使用率 %（-1 表示不可用）
    - net_dn / net_up: 下载/上传速度 bytes/s（需要两次采样求差值）
    """
    m = {"cpu": -1, "gpu": -1, "mem": -1, "net_dn": 0, "net_up": 0}

    try:
        import psutil

        m["cpu"] = int(psutil.cpu_percent(interval=None))
        m["mem"] = int(psutil.virtual_memory().percent)

        # 网速：两次采样求差值
        now = time.time()
        io = psutil.net_io_counters()
        st = _metrics_state
        if st["time"] > 0:
            dt = now - st["time"]
            if dt > 0:
                m["net_dn"] = max(0, int((io.bytes_recv - st["dn"]) / dt))
                m["net_up"] = max(0, int((io.bytes_sent - st["up"]) / dt))
        st["time"] = now
        st["dn"] = io.bytes_recv
        st["up"] = io.bytes_sent
    except Exception:
        pass

    handle = _get_nvml_handle()
    if handle:
        try:
            import pynvml

            util = pynvml.nvmlDeviceGetUtilizationRates(handle)
            m["gpu"] = int(util.gpu)
        except Exception:
            pass

    return m


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


async def pump(client, cmd_q, state):
    """Forward queued commands to the device until the link drops.

    Also periodically samples PC performance (CPU/GPU/mem/net) and pushes it
    to the device so the standby screen can display it.
    """
    last_metrics = 0.0
    while client.is_connected:
        try:
            msg = await asyncio.wait_for(cmd_q.get(), timeout=1.0)
        except asyncio.TimeoutError:
            # 空闲时定时推送性能数据。
            # 仅在「无待发命令」且「不在 prompt 推送期间」的间隙发送，
            # 避免 metrics 与 prompt 命令争抢 BLE 写通道。
            if METRICS_INTERVAL <= 0 or state.get("in_prompt", False):
                continue
            now = time.time()
            if now - last_metrics >= METRICS_INTERVAL:
                last_metrics = now
                metrics = collect_metrics()
                metrics["type"] = "metrics"
                try:
                    await client.write_gatt_char(
                        RX_CHAR, (json.dumps(metrics, ensure_ascii=False) + "\n").encode("utf-8")
                    )
                except Exception as e:
                    emit({"event": "log", "msg": "metrics write error: %r" % e})
                    return
            continue
        if msg is None:
            raise _Shutdown()
        if msg.get("cmd") != "send":
            continue
        payload = msg.get("payload", "")
        # Track the latest prompt id so voice replies can reference it.
        try:
            p = json.loads(payload)
            pid = (p.get("prompt") or {}).get("id")
            if pid:
                state["prompt_id"] = pid
                # 推送期间暂停性能监控（推送界面无位置展示）。
                state["in_prompt"] = True
        except Exception:
            pass
        try:
            # The device splits incoming messages on '\n'; the newline is required.
            await client.write_gatt_char(RX_CHAR, (payload + "\n").encode("utf-8"))
        except Exception as e:
            emit({"event": "log", "msg": "write error: %r" % e})
            return  # treat write failure as a dropped link


def _setup_win32_types(user32, kernel32):
    """Declare Win32 API signatures.

    ctypes defaults return types to 32-bit c_int, which truncates the 64-bit
    HGLOBAL/pointer values returned by GlobalAlloc/GlobalLock, producing a
    wild pointer and an access-violation crash. Declaring c_void_p fixes it.
    """
    # HGLOBAL GlobalAlloc(UINT uFlags, SIZE_T dwBytes)
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
    # LPVOID GlobalLock(HGLOBAL hMem)
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    # BOOL GlobalUnlock(HGLOBAL hMem)
    kernel32.GlobalUnlock.restype = ctypes.c_int
    kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    # HGLOBAL GlobalFree(HGLOBAL hMem)
    kernel32.GlobalFree.restype = ctypes.c_void_p
    kernel32.GlobalFree.argtypes = [ctypes.c_void_p]

    # BOOL OpenClipboard(HWND hWndNewOwner)
    user32.OpenClipboard.restype = ctypes.c_int
    user32.OpenClipboard.argtypes = [ctypes.c_void_p]
    # BOOL EmptyClipboard(VOID)
    user32.EmptyClipboard.restype = ctypes.c_int
    # HANDLE SetClipboardData(UINT uFormat, HANDLE hMem)
    user32.SetClipboardData.restype = ctypes.c_void_p
    user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
    # BOOL CloseClipboard(VOID)
    user32.CloseClipboard.restype = ctypes.c_int


def type_text_to_focused_window(text):
    """Paste `text` into the currently focused window.

    Uses clipboard + Ctrl+V (the only reliable way to input arbitrary text,
    including CJK). Windows-only; returns True on success.
    """
    if not text:
        return False
    if ctypes is None or os.name != "nt":
        emit({"event": "log", "msg": "keyboard paste: only supported on Windows"})
        return False
    try:
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        _setup_win32_types(user32, kernel32)

        # 1) 写入剪贴板（CF_UNICODETEXT）
        CF_UNICODETEXT = 13
        GMEM_MOVEABLE = 0x0002
        if not user32.OpenClipboard(None):
            emit({"event": "log", "msg": "keyboard paste: OpenClipboard failed"})
            return False
        try:
            user32.EmptyClipboard()
            wtext = text.encode("utf-16-le") + b"\x00\x00"
            hmem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(wtext))
            if not hmem:
                raise OSError("GlobalAlloc failed")
            p = kernel32.GlobalLock(hmem)
            if not p:
                kernel32.GlobalFree(hmem)
                raise OSError("GlobalLock failed")
            try:
                ctypes.memmove(p, wtext, len(wtext))
            finally:
                kernel32.GlobalUnlock(hmem)
            if not user32.SetClipboardData(CF_UNICODETEXT, hmem):
                kernel32.GlobalFree(hmem)
                raise OSError("SetClipboardData failed")
        finally:
            user32.CloseClipboard()

        # 2) 模拟 Ctrl+V
        time.sleep(0.05)
        VK_CONTROL = 0x11
        VK_V = 0x56
        KEYEVENTF_KEYUP = 0x0002
        user32.keybd_event(VK_CONTROL, 0, 0, 0)
        user32.keybd_event(VK_V, 0, 0, 0)
        user32.keybd_event(VK_V, 0, KEYEVENTF_KEYUP, 0)
        user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)
        emit({"event": "log", "msg": "keyboard paste: %d chars typed" % len(text)})
        return True
    except Exception as e:
        emit({"event": "log", "msg": "keyboard paste error: %r" % e})
        return False


def type_key(key):
    """Simulate a single keypress on the currently focused window.

    Used by the "keyboard forwarding" feature: when the device is idle
    (BLE connected, no prompt), its Enter/Backspace keys are forwarded
    here and injected into the focused window. Windows-only.
    """
    if ctypes is None or os.name != "nt":
        return False
    VK_MAP = {
        "enter": 0x0D,
        "backspace": 0x08,
    }
    vk = VK_MAP.get(key)
    if vk is None:
        emit({"event": "log", "msg": "key: unknown key %r" % key})
        return False
    try:
        user32 = ctypes.windll.user32
        KEYEVENTF_KEYUP = 0x0002
        user32.keybd_event(vk, 0, 0, 0)
        user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)
        emit({"event": "log", "msg": "key: pressed %r" % key})
        return True
    except Exception as e:
        emit({"event": "log", "msg": "key error: %r" % e})
        return False


async def process_audio_end(adpcm, meta, state):
    """Decode the recorded ADPCM, save it, transcribe, and route the result.

    - mode == "keyboard": paste the transcription into the focused window
      (no reply back to the daemon).
    - otherwise (prompt mode): reply to the pending prompt as normal input.
    """
    samples = adpcm_decode(adpcm)
    ts = time.strftime("%Y%m%d-%H%M%S")
    wav_path = os.path.join(AUDIO_DIR, "voice-%s.wav" % ts)
    write_wav(wav_path, samples)
    emit({"event": "log", "msg": "voice captured: %d samples -> %s" % (len(samples), wav_path)})

    loop = asyncio.get_running_loop()
    emit({"event": "log", "msg": "transcribe start: mode=%s" % meta.get("mode")})
    text = await loop.run_in_executor(None, transcribe, wav_path)
    emit({"event": "log", "msg": "transcribe done: text=%r" % (text,)})

    # 键盘输入模式：转写结果直接输入电脑聚焦窗口，不回传 agent。
    if meta.get("mode") == "keyboard":
        if text:
            await loop.run_in_executor(None, type_text_to_focused_window, text)
        else:
            emit({"event": "log", "msg": "keyboard paste: no transcription"})
        return

    pid = state.get("prompt_id") or ""
    if text:
        reply = {"cmd": "input", "id": pid, "text": text}
    else:
        reply = {
            "cmd": "input",
            "id": pid,
            "text": "[voice] (未识别到语音，请重试)",
        }
    # 语音回复意味着用户已回应，推送结束，恢复性能监控。
    state["in_prompt"] = False
    emit({"event": "log", "msg": "voice reply: pid=%s text=%r" % (pid, text)})
    emit({"event": "recv", "line": json.dumps(reply, ensure_ascii=False)})


async def main():
    _force_utf8_stdio()
    emit({"event": "ready"})

    # 必须在 BLE(WinRT) 初始化之前加载 faster-whisper：
    # 它依赖的 PyTorch c10.dll 在 WinRT/BLE 组件活跃时会加载失败
    # (WinError 1114: 动态链接库初始化例程失败)。用 executor 加载并等待
    # 完成，既避免阻塞事件循环，又保证 torch 先于 BLE 加载。
    try:
        await asyncio.get_running_loop().run_in_executor(None, _preload_model)
    except Exception:
        pass

    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        emit({"event": "log", "msg": "bleak not installed; run: pip install bleak"})
        emit({"event": "connected", "connected": False})
        return

    cmd_q = asyncio.Queue()
    reader = asyncio.create_task(stdin_reader(cmd_q))
    state = {"prompt_id": "", "in_prompt": False}

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
            audio_buf = bytearray()

            def on_notify(_sender, data):
                nonlocal rxbuf, audio_buf
                rxbuf += data
                while b"\n" in rxbuf:
                    line, rxbuf = rxbuf.split(b"\n", 1)
                    s = line.decode("utf-8", "replace")
                    try:
                        obj = json.loads(s)
                    except Exception:
                        # 解析失败：可能是音频帧被 BLE 分包后残片/错乱。
                        # 合法回复（permission/input）都是完整 JSON，不会到这里。
                        # 忽略残片，避免把音频数据当作回复发给 agent。
                        emit({"event": "log", "msg": "dropped unparsable notify: %r" % s[:60]})
                        continue
                    if obj.get("evt") == "audio":
                        try:
                            audio_buf += base64.b64decode(obj.get("data", ""))
                        except Exception:
                            pass
                        continue
                    if obj.get("evt") == "audio_end":
                        payload = bytes(audio_buf)
                        audio_buf = bytearray()
                        asyncio.ensure_future(process_audio_end(payload, obj, state))
                        continue
                    if obj.get("evt") == "key":
                        # 键盘转发：Cardputer 实体键 -> 电脑聚焦窗口
                        type_key(obj.get("key", ""))
                        continue
                    # 用户已回复（permission/input），推送结束，恢复性能监控。
                    if obj.get("cmd") in ("permission", "input"):
                        state["in_prompt"] = False
                    emit({"event": "recv", "line": s})

            try:
                async with BleakClient(device.address) as client:
                    # Subscribe before announcing the link so no reply is missed.
                    await client.start_notify(TX_CHAR, on_notify)
                    emit({"event": "connected", "connected": True})
                    await pump(client, cmd_q, state)
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
