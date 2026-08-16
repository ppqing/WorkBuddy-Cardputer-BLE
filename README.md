<div align="center">

# ask-master (BLE)

**桌上的物理确认按钮 —— 给 AI 编程 agent 的人类侧信道。**
When the model needs you, it pings a tiny screen on your desk — not your chat.

[![Go](https://img.shields.io/badge/go-1.22%2B-00ADD8?style=flat-square&logo=go)](https://go.dev)
[![License](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)

</div>

---

> **本分支将原 WiFi/WebSocket 传输改为 BLE，并新增中文拼音输入法、电量显示、中英双语界面、语音输入、键盘转发、电脑性能监控等功能。**
>
> **This fork replaces WiFi/WebSocket with BLE transport, adding a pinyin IME, battery indicator, bilingual UI, voice input, keyboard forwarding, and PC performance monitoring.**

---

## 目录 / Table of Contents

- [简介 / Overview](#简介--overview)
- [工作原理 / How It Works](#工作原理--how-it-works)
- [功能特性 / Features](#功能特性--features)
- [快速开始 / Quick Start](#快速开始--quick-start)
- [让任意 Agent 接入 / Connecting Any Agent](#让任意-agent-接入--connecting-any-agent)
- [MCP 工具 / MCP Tools](#mcp-工具--mcp-tools)
- [语音输入 & 键盘转发 / Voice & Keyboard](#语音输入--键盘转发--voice--keyboard)
- [性能监控 / Performance Monitor](#性能监控--performance-monitor)
- [中文拼音输入法 / Pinyin IME](#中文拼音输入法--pinyin-ime)
- [固件构建 / Firmware Build](#固件构建--firmware-build)
- [CLI 参数 / CLI Flags](#cli-参数--cli-flags)
- [故障排查 / Troubleshooting](#故障排查--troubleshooting)
- [项目结构 / Project Layout](#项目结构--project-layout)
- [许可证 / License](#许可证--license)

---

## 简介 / Overview

**中文：** `ask-master` 是一个 [MCP](https://modelcontextprotocol.io) 服务器，让 Claude Code、Cursor、CodeBuddy、Windsurf 等 AI 编程 agent 通过你桌上的一台 [M5Stack Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3) 与你沟通。agent 在设备上提问，你用设备键盘（或语音）回答，聊天历史保持干净。

**English:** `ask-master` is an [MCP](https://modelcontextprotocol.io) server that gives AI coding agents (Claude Code, Cursor, CodeBuddy, Windsurf, etc.) a side-channel to you via an [M5Stack Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3) on your desk. The agent asks questions on the device, you answer with its keyboard or voice, and your chat history stays clean.

**适用场景 / Use cases:**
- 长时间运行的 agent 循环，你可能走开 / Long-running agent loops you walk away from
- 配对编程时不想让确认弹窗干扰聊天 / Pair-programming without approval clutter
- 关键决策需要蜂鸣提醒 / Critical decisions that need a buzzer
- 把 Cardputer 当「语音键盘」给电脑任意输入框打字 / Use the Cardputer as a voice keyboard for any PC input field

---

## 工作原理 / How It Works

```
┌──────────────────────────────────────────────────────────────┐
│  Agent A (CodeBuddy)  ──┐                                     │
│  Agent B (Claude Code) ──┤── ask-master.exe (stdio, JSON-RPC) │
│  Agent C (Cursor)     ──┘                                     │
│         │                                                     │
│         ▼  TCP localhost:51937 (多 agent 共享 / multi-agent)  │
├──────────────────────────────────────────────────────────────┤
│  ask-master daemon (Go)                                       │
│  ├── ble_bridge.go    — BLE NUS 桥接 / BLE NUS bridge         │
│  ├── daemon.go        — 多会话 TCP daemon + lock              │
│  └── tools.go         — confirm / choose / ask / escalate     │
│         │                                                     │
│         ▼  stdin/stdout (line-delimited JSON)                 │
├──────────────────────────────────────────────────────────────┤
│  ble_proxy.py (Python + bleak + faster-whisper)               │
│  ├── 永久重连循环 / permanent reconnect loop                  │
│  ├── GATT 连接 Nordic UART Service                            │
│  ├── 语音转写 / voice transcription (Whisper, GPU)            │
│  ├── 键盘转发 / keyboard forwarding (剪贴板 + Ctrl+V)         │
│  └── 性能监控采集 / metrics (psutil + pynvml)                 │
│         │                                                     │
│         ▼  BLE NUS (6E400001 / RX:6E400002 / TX:6E400003)    │
├──────────────────────────────────────────────────────────────┤
│  Cardputer 固件 (ESP32-S3, Arduino / C++)                     │
│  ├── BLE 广播设备名 "Claude AskMaster"                        │
│  ├── NUS 收发 + portMUX 临界区保护                            │
│  ├── M5GFX canvas 渲染 (efontCN 中文字体)                     │
│  ├── 拼音输入法 (单字 + 词组联想)                              │
│  ├── 电量显示 (ADC 轮询, 10s 缓存)                            │
│  ├── 中英双语界面 (NVS 持久化, 按 L 切换)                     │
│  ├── 麦克风录音 (按住 Ctrl 说话, ES8311 + IMA-ADPCM)          │
│  └── 键盘扫描 (Fn+方向键滚动, 长按连发)                       │
└──────────────────────────────────────────────────────────────┘
```

**关键设计 / Key design points:**

1. **BLE 取代 WiFi / BLE replaces WiFi** — 设备无需连 WiFi、无需 IP 配置。/ No WiFi needed, no IP configuration.

2. **多 Agent 共享 / Multi-agent sharing** — 第一个启动的 exe 成为 daemon 并持有 BLE 连接，后续 exe 自动作为 client 通过 TCP localhost 连接。/ The first exe becomes the daemon and holds the BLE connection; later exes connect as clients over TCP localhost.

3. **单文件分发 / Single-file distribution** — `ble_proxy.py` 通过 `go:embed` 打入二进制。/ `ble_proxy.py` is embedded via `go:embed`; a single exe works anywhere.

---

## 功能特性 / Features

| 特性 / Feature | 说明 / Description |
|---|---|
| BLE 传输 / BLE transport | 蓝牙直连，无需 WiFi / Direct Bluetooth, no WiFi needed |
| 中文拼音输入法 / Pinyin IME | 410 音节 + 8461 词组，支持单字和词组联想 |
| 语音输入 / Voice input | 按住 Ctrl 说话，faster-whisper (small, 本地模型, GPU) 转写 |
| 键盘转发 / Keyboard forwarding | Cardputer 的 Enter/Backspace 直接控制电脑聚焦窗口 |
| 性能监控 / Performance monitor | 待机界面显示 CPU/GPU/内存/网速 |
| 电量显示 / Battery indicator | 状态栏百分比 + 图标，颜色分级 |
| 中英双语 / Bilingual UI | 待机界面按 L 切换语言，NVS 持久化 |
| 自动换行 / Auto word-wrap | UTF-8 感知，CJK 任意断行，拉丁文整词换行 |
| 长按滚动 / Long-press scroll | 方向键长按 350ms 后每 90ms 滚一行 |
| 闪烁告警 / Flashing alert | escalate 界面每 200ms 自主闪烁 |
| 超时与离线区分 / Timeout vs offline | `[CARDPUTER TIMEOUT]` ≠ `[CARDPUTER OFFLINE]` ≠ 人按 N |

---

## 快速开始 / Quick Start

### 1. 安装依赖 / Install dependencies

```bash
# Python BLE + 语音 + 性能监控依赖
pip install bleak faster-whisper pynvml psutil nvidia-cublas-cu12
```

### 2. 下载服务端 / Download the server

从 [Releases](https://github.com/ppqing/WorkBuddy-Cardputer-BLE/releases) 下载最新版本：

- `ask-master.exe` —— 预编译服务端（已内置 `ble_proxy.py`，单文件即可运行）
- `ask-master-ble-merged.bin` —— Cardputer 固件（设备从未烧录过本固件时才需要）

无需安装 Go，也无需编译。

#### 语音转写模型（单独下载，不进 Release）

模型需放到 `models/faster-whisper-small/`（faster-whisper 的 CTranslate2 格式，约 486MB）。

**镜像渠道（国内直连，推荐）** — [ModelScope Systran/faster-whisper-small](https://modelscope.cn/models/Systran/faster-whisper-small)：

```powershell
$dir = "models\faster-whisper-small"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
foreach ($f in @("config.json","model.bin","tokenizer.json","vocabulary.txt")) {
  curl.exe -L -o "$dir\$f" "https://modelscope.cn/api/v1/models/Systran/faster-whisper-small/repo?Revision=master&FilePath=$f"
}
```

**官方渠道** — [Hugging Face Systran/faster-whisper-small](https://huggingface.co/Systran/faster-whisper-small)：

```bash
pip install huggingface_hub
huggingface-cli download Systran/faster-whisper-small --local-dir models/faster-whisper-small
```

### 3. 烧录固件 / Flash the firmware

```powershell
.\fwbuild.ps1    # 编译 / Compile
```

然后用 esptool 烧录（详见[固件构建](#固件构建--firmware-build)）。

### 4. 启动 daemon / Start the daemon

```bash
python daemon_keeper.py
```

### 5. 配置 MCP 客户端 / Configure MCP client

在你的 agent 的 MCP 配置文件中添加（把 `<项目目录>` 换成实际路径）：

```json
{
  "mcpServers": {
    "ask-master": {
      "command": "<项目目录>\\ask-master.exe",
      "args": ["--transport", "ble"]
    }
  }
}
```

---

## 让任意 Agent 接入 / Connecting Any Agent

**中文：** ask-master 是标准 MCP server，任何支持 MCP 的 agent 都能接入。下面给出各平台配置方法，以及一份**可直接复制给 Agent 的 Prompt**。

**English:** ask-master is a standard MCP server. Any MCP-capable agent can connect. Below are platform configs, plus a ready-to-paste prompt.

### 各平台配置 / Platform configs

**CodeBuddy** — `~/.codebuddy/mcp.json`：

```json
{
  "mcpServers": {
    "ask-master": {
      "command": "<项目目录>\\ask-master.exe",
      "args": ["--transport", "ble"],
      "type": "stdio"
    }
  }
}
```

**Claude Code** — `~/.claude/settings.json`：

```json
{
  "mcpServers": {
    "ask-master": {
      "command": "<项目目录>\\ask-master.exe",
      "args": ["--transport", "ble"]
    }
  }
}
```

**Cursor** — `~/.cursor/mcp.json`：

```json
{
  "mcpServers": {
    "ask-master": {
      "command": "<项目目录>\\ask-master.exe",
      "args": ["--transport", "ble"]
    }
  }
}
```

> 多个 agent 可以同时配置，共享同一个 daemon 和 BLE 连接，请求自动排队串行处理。

---

### 给 Agent 的接入 Prompt（直接复制）

把下面这段文本发给你的 AI agent（作为 system prompt 或对话首条消息），它就会知道如何通过 ask-master 与你沟通：

````text
你现在要接入一个名为 ask-master 的 MCP 服务器，它通过一台 M5Stack Cardputer 蓝牙设备与我（真人用户）沟通。下面包含完整的搭建、运行和使用说明。如果环境还没搭好，先按「环境搭建」和「运行」操作；如果已搭好，直接看「可用工具」和「使用规范」。

## 环境搭建（一次性，仅当环境未就绪时执行）

1. 安装 Python 依赖（BLE + 语音转写 + 性能监控）：
   pip install bleak faster-whisper pynvml psutil nvidia-cublas-cu12

2. 下载预编译服务端：从 GitHub Releases 下载 `ask-master.exe` 放到 `<项目目录>`。
   无需安装 Go，也无需编译。

3. 下载语音转写模型到 `<项目目录>/models/faster-whisper-small/`（约 486MB）：
   - 国内镜像（推荐）：ModelScope Systran/faster-whisper-small，命令见项目 README「语音转写模型」节
   - 官方渠道：`huggingface-cli download Systran/faster-whisper-small --local-dir models/faster-whisper-small`
   - 无模型时语音输入不可用，但 confirm/choose/ask-human 键盘输入不受影响

## 运行

1. 启动 daemon（常驻，自动扫描并连接 Cardputer 蓝牙设备 "Claude AskMaster"）
   cd <项目目录>
   python daemon_keeper.py

2. 探测 BLE 是否可用：检查 daemon 日志（daemon.log）是否出现
   "BLE connected: true"，或用以下命令手动扫描：
   python -c "import asyncio; from bleak import BleakScanner; \
     asyncio.run(BleakScanner.discover(timeout=10.0))" | findstr /i "Claude AskMaster"

   - 能搜到 "Claude AskMaster" → 设备固件已就绪，直接跳到第 4 步使用，无需烧录。
   - 搜不到 → 按顺序排查：
     a) 提示用户：确认 Cardputer 已开机（屏幕亮着，显示待机画面）。
     b) 确认设备已配对/在蓝牙范围内（设备被连接后会停止广播，属正常）。
     c) 若确认是「从未烧录过本固件」的新设备，才走下面「烧录固件」流程。

3. （可选，仅当设备从未烧录过本固件时）烧录固件到 Cardputer
   # 安装 Arduino 依赖
   arduino-cli core install m5stack:esp32@2.1.4
   arduino-cli lib install "M5Unified@0.2.19" "M5GFX@0.2.26" "NimBLE-Arduino@1.4.0" "ArduinoJson@7.0.4"
   # 将项目里的 M5CardputerBLE/ 目录安装为 Arduino 库

   # 编译（Windows）
   .\fwbuild.ps1

   # 烧录（首次需全量写入）
   esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash \
     0x0      firmware/ask_master_ble/build/ask_master_ble.ino.bootloader.bin \
     0x8000   firmware/ask_master_ble/build/ask_master_ble.ino.partitions.bin \
     0x10000  firmware/ask_master_ble/build/ask_master_ble.ino.bin

   注意：不要用 M5Launcher 烧录，它会覆盖分区表导致黑屏。

4. 配置 MCP 客户端：把 ask-master 注册到 MCP 配置文件
   （CodeBuddy: ~/.codebuddy/mcp.json；Claude Code: ~/.claude/settings.json；Cursor: ~/.cursor/mcp.json）
   {
     "mcpServers": {
       "ask-master": {
         "command": "<项目目录>/ask-master.exe",
         "args": ["--transport", "ble"]
       }
     }
   }

## 可用工具

1. `confirm` —— 二元确认（是/否）
   - statement（必填）：要确认的陈述，简洁明确
   - consequence（可选）：确认后的后果
   - timeout（可选）：超时毫秒数，默认 60000
   - 返回：`true`（我按了 Y）或 `false`（我按了 N）

2. `choose` —— 多选一
   - question（必填）：问题
   - options（必填）：2~5 个选项
   - context（可选）：补充上下文
   - 返回：我选择的选项文本，或我的自定义输入
   - 注意：设备会自动追加第 6 个「自定义输入」选项，我可以语音或键盘输入自定义回复

3. `ask-human` —— 自由文本输入
   - question（必填）：问题
   - context（可选）：补充上下文
   - 返回：我输入的文本（我可以用拼音输入法或按住 Ctrl 语音输入）

4. `escalate-to-human` —— 紧急升级
   - question（必填）：问题
   - context（可选）：补充上下文
   - chat_wait_time_seconds（可选）：你在聊天里已经等待的秒数
   - 返回：我输入的文本
   - 仅在以下情况使用：(a) 你已在聊天里问了但 2 分钟以上没收到回复；(b) 问题紧急需要立即引起注意

## 使用规范

- 默认优先在聊天里问我。只有当你在聊天里等了 2 分钟以上没有回复，或决策非常关键/紧急时，才调用这些工具。
- 每次只发一个请求，不要并发调用（设备串行处理）。
- question/statement 要简洁（设备屏幕有限，中文约 200 字以内，option 约 80 字以内）。
- 返回值处理：
  - `[CARDPUTER OFFLINE]` = 设备离线，我不在电脑旁。不要卡住，按默认/安全值继续，并在最终回复里说明你做了这个假设。
  - `[CARDPUTER TIMEOUT]` = 设备在线但我没在超时内回复。按超时兜底继续。
  - 这两个值都 ≠ 我按 N 的 `false`，不要混淆。
- 得到我的回复后，先明确复述我的决定，再继续执行。
- 不要为了刷存在感而频繁调用工具；只在真正需要人类决策时才用。

## 其他设备能力（无需你调用，仅供了解）

- 语音输入：我可以按住设备上的 Ctrl 键说话，转写结果自动输入到电脑当前聚焦的输入框。
- 性能监控：设备待机界面会显示电脑的 CPU/GPU/内存/网速。
- 键盘转发：设备待机时，Enter/Backspace 键直接控制电脑聚焦窗口。
````

---

## MCP 工具 / MCP Tools

| 工具 / Tool | 用途 / Purpose | 设备显示 / Device shows | 回复方式 / Reply |
|---|---|---|---|
| `confirm` | 是/否确认 | 确认框 + Y/N | `true` / `false` |
| `choose` | 多选一（2~5 项 + 自动追加「自定义输入」） | 编号列表 | 选项文本 / 自定义输入 |
| `ask-human` | 自由文本输入 | 问题 + 输入行 | 用户文本（拼音或语音） |
| `escalate-to-human` | 紧急确认 | 闪烁标题 + 输入行 | 用户文本 |

**返回值语义 / Return value semantics:**

| 返回值 | 含义 | Agent 应如何处理 |
|---|---|---|
| `true` / `false` | 用户按 Y / N | 正常分支 |
| 选项文本 / 自定义文本 | 用户的选择或输入 | 正常分支 |
| `[CARDPUTER OFFLINE]` | 设备离线 | 按默认/安全值继续 |
| `[CARDPUTER TIMEOUT]` | 超时未回复 | 按超时兜底继续 |

---

## 语音输入 & 键盘转发 / Voice & Keyboard

### 语音输入（两种模式）

**1. 键盘模式（IDLE 待机界面）**
- 电脑上点任意输入框获得焦点
- 按住 Cardputer 的 **Ctrl** 键说话，松开
- 转写结果自动粘贴到电脑聚焦窗口

**2. 回复模式（收到 prompt 时）**
- `ask-human` / `escalate` / `choose` 界面直接按住 Ctrl 说话
- 转写结果作为回复返回给 agent

**语音模型配置**（`ble_proxy.py`）：

```python
WHISPER_MODEL = _resolve_model_path()   # 默认 <项目目录>/models/faster-whisper-small/
WHISPER_DEVICE = "cuda"     # 有 NVIDIA GPU 用 cuda，否则 cpu
WHISPER_COMPUTE = "float16" # GPU: float16；CPU: int8
WHISPER_LANGUAGE = "zh"
WHISPER_INITIAL_PROMPT = "以下是普通话的句子，请使用简体中文输出。"
```

- 模型优先从本地 `models/faster-whisper-small/` 加载（随 Release 分发，无需联网下载）
- 可用环境变量 `ASK_MASTER_WHISPER_MODEL` 覆盖为其他本地路径
- 如需用别的尺寸，把 `_resolve_model_path()` 改成 `"tiny"` / `"base"` / `"small"` / `"medium"` 等，
  由 faster-whisper 自动从 Hugging Face 下载（需能访问 huggingface.co）

### 键盘转发（IDLE 待机界面）

| Cardputer 按键 | 电脑上的效果 |
|---|---|
| Enter | 输入回车 |
| Backspace（短按） | 删除 1 个字符 |
| Backspace（长按） | 连续删除（350ms 后每 90ms 删一个） |

---

## 性能监控 / Performance Monitor

待机界面显示电脑的 CPU/GPU/内存/网速（每 2 秒刷新）。

- **采集**：`ble_proxy.py` 的 `collect_metrics()`，用 `psutil`（CPU/内存/网速）+ `pynvml`（GPU）
- **暂停机制**：收到 prompt 推送期间自动暂停（推送界面无位置展示），回复后回到待机界面自动恢复
- **依赖**：`pip install psutil pynvml`

---

## 中文拼音输入法 / Pinyin IME

在 `ask-human`、`escalate-to-human` 界面中默认启用拼音输入法。

### 操作 / Key bindings

| 按键 | 功能 |
|---|---|
| `a`–`z` | 输入拼音字母 |
| `1`–`9` | 选择候选 |
| `空格` | 选第一个候选 |
| `opt` | 切换中/英文模式 |
| `backspace` | 删拼音字母 / 删已确认字 |
| `回车` | 发送 |

### 候选优先级 / Candidate priority

1. 单字精确匹配（`ni` → `你`）
2. 词组精确匹配（`nihao` → `你好`）
3. 词组前缀匹配（`ni` → `你好`, `你们`...）
4. 单字后缀匹配

### 生成词库 / Regenerating dictionaries

```bash
python gen_pinyin_dict.py       # 单字表 (GB2312 过滤, 字频排序)
python gen_pinyin_phrases.py    # 词组表 (jieba 词频, ≥500)
```

---

## 固件构建 / Firmware Build

固件使用 `huge_app` 分区方案（3MB app 分区），容纳中文字体和拼音词库。

### 依赖 / Dependencies

```bash
arduino-cli core install m5stack:esp32@2.1.4
arduino-cli lib install "M5Unified@0.2.19" "M5GFX@0.2.26" "NimBLE-Arduino@1.4.0" "ArduinoJson@7.0.4"
# 将 M5CardputerBLE/ 安装为 Arduino 库
```

### 编译 / Compile

```powershell
.\fwbuild.ps1    # Windows
```

### 烧录 / Flash

```bash
# 首次烧录需全量写入
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash \
  0x0      firmware/ask_master_ble/build/ask_master_ble.ino.bootloader.bin \
  0x8000   firmware/ask_master_ble/build/ask_master_ble.ino.partitions.bin \
  0x10000  firmware/ask_master_ble/build/ask_master_ble.ino.bin

# 后续更新只需刷 app
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash \
  0x10000 firmware/ask_master_ble/build/ask_master_ble.ino.bin
```

> **警告：** M5Launcher 不兼容此固件，请用 esptool 烧录。

---

## CLI 参数 / CLI Flags

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--transport` | `ws` | `ws` (WebSocket) 或 `ble` (BLE NUS, 推荐) |
| `--timeout` | `300` | 默认工具超时（秒） |
| `--log-level` | `info` | `debug` / `info` / `warn` / `error` |
| `--version` | — | 打印版本并退出 |

---

## 故障排查 / Troubleshooting

<details>
<summary><b>设备显示 OFFLINE / Device shows OFFLINE</b></summary>

- 确认 daemon 运行：`Get-Process ask-master,python`
- 确认 `ble_proxy.py` 进程存在（daemon 自动拉起）
- 手动运行 `python ble_proxy.py` 查看扫描日志
- **BLE 设备被连接后停止广播——扫不到不代表离线**
- Windows 蓝牙扫描失效时：设备管理器里禁用再启用蓝牙适配器，或用 PowerShell `Disable-PnpDevice`/`Enable-PnpDevice`
</details>

<details>
<summary><b>中文显示为方块 / Chinese shows as boxes</b></summary>

efontCN 只覆盖 GB2312（6763 字）。词库已过滤，但 daemon 发送 GB2312 以外的字仍会显示方块。
</details>

<details>
<summary><b>语音转写不工作 / Voice transcription fails</b></summary>

- 确认 `faster-whisper` 已安装
- 确认模型已就位：`<项目目录>/models/faster-whisper-small/`（从 ModelScope 镜像或 Hugging Face 下载，命令见 README「语音转写模型」节）
- GPU 模式下若报 `cublas64_12.dll` 缺失，安装 `pip install nvidia-cublas-cu12`
- 模型加载必须在 BLE 初始化之前（`ble_proxy.py` 已处理，勿改动顺序）
- 检查日志 `daemon.log` 中 `whisper: model preloaded (cuda/float16)` 是否出现
</details>

<details>
<summary><b>多个 CodeBuddy 进程冲突 / Multiple CodeBuddy processes conflict</b></summary>

daemon 模式自动处理。第一个 exe 成为 daemon，后续 exe 作为 client 连接 TCP localhost:51937，请求排队串行处理。
</details>

<details>
<summary><b>M5Launcher 烧入后黑屏 / Black screen after M5Launcher flash</b></summary>

M5Launcher 用自己的分区表覆盖了 app 分区。请用 esptool 全量烧录。
</details>

---

## 项目结构 / Project Layout

```
.
├── ble_bridge.go                — BLE NUS 桥接 (go:embed ble_proxy.py)
├── ble_proxy.py                 — Python BLE central (bleak + faster-whisper)
├── daemon_keeper.py             — daemon 常驻托管脚本
├── daemon.go                    — 多会话 TCP daemon
├── tools.go                     — MCP 工具注册 (confirm/choose/ask/escalate)
├── config.go                    — CLI 参数
├── main.go                      — 入口, daemon/client 路由
├── bridge.go                    — WebSocket 桥接 (原版, 保留)
├── internal/truncate/           — 字符串截断 (UTF-8 安全)
├── gen_pinyin_dict.py           — 单字拼音词库生成器
├── gen_pinyin_phrases.py        — 词组拼音词库生成器
├── fwbuild.ps1                  — 固件编译脚本 (Windows)
├── libraries/                   — 本地 Arduino 库 (ArduinoJson, 随项目分发)
├── models/faster-whisper-small/ — 语音转写模型 (随 Release 分发, 不入库)
├── firmware/ask_master_ble/     — BLE 固件源码
│   ├── ask_master_ble.ino       — 主程序 (状态机, BLE, 键盘, 语音)
│   ├── ui.cpp / ui.h            — UI 渲染 (canvas, 字体, 换行, 电池, 性能监控)
│   ├── audio.cpp / audio.h      — 麦克风录音 (ES8311, IMA-ADPCM)
│   ├── pinyin_ime.cpp / .h      — 拼音输入法引擎
│   ├── pinyin_dict.h            — 单字词库 (自动生成)
│   ├── pinyin_phrases.h         — 词组词库 (自动生成)
│   └── config.h                 — 固件配置
├── M5CardputerBLE/              — BLE 适配版 M5Cardputer 库
├── plugin/                      — Claude Code 插件 (原版)
├── skill/                       — 升级策略文档
└── README.md                    — 本文件
```

---

## 许可证 / License

[MIT](LICENSE)

本项目基于 [mhrsntrk/ask-master](https://github.com/mhrsntrk/ask-master) 修改，原许可证为 MIT。
