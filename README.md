<div align="center">

# ask-master (BLE)

**桌上的物理确认按钮 —— 给 AI 编程 agent 的人类侧信道。**
When the model needs you, it pings a tiny screen on your desk — not your chat.

[![Go](https://img.shields.io/badge/go-1.22%2B-00ADD8?style=flat-square&logo=go)](https://go.dev)
[![License](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)

</div>

---

> **本分支将原 WiFi/WebSocket 传输改为 BLE，并新增中文拼音输入法、电量显示、中英双语界面、词语联想等功能。**
>
> **This fork replaces WiFi/WebSocket with BLE transport, adding a pinyin IME, battery indicator, bilingual UI, and phrase prediction.**

---

## 目录 / Table of Contents

- [简介 / Overview](#简介--overview)
- [工作原理 / How It Works](#工作原理--how-it-works)
- [功能特性 / Features](#功能特性--features)
- [快速开始 / Quick Start](#快速开始--quick-start)
- [让任意 Agent 接入 / Connecting Any Agent](#让任意-agent-接入--connecting-any-agent)
- [MCP 工具 / MCP Tools](#mcp-工具--mcp-tools)
- [中文拼音输入法 / Pinyin IME](#中文拼音输入法--pinyin-ime)
- [固件构建 / Firmware Build](#固件构建--firmware-build)
- [CLI 参数 / CLI Flags](#cli-参数--cli-flags)
- [故障排查 / Troubleshooting](#故障排查--troubleshooting)
- [项目结构 / Project Layout](#项目结构--project-layout)
- [许可证 / License](#许可证--license)

---

## 简介 / Overview

**中文：** `ask-master` 是一个 [MCP](https://modelcontextprotocol.io) 服务器，让 Claude Code、Cursor、Windsurf 等 AI 编程 agent 通过你桌上的一台 [M5Stack Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3) 与你沟通。agent 在设备上提问，你用设备键盘回答，聊天历史保持干净。

**English:** `ask-master` is an [MCP](https://modelcontextprotocol.io) server that gives AI coding agents (Claude Code, Cursor, Windsurf, etc.) a side-channel to you via an [M5Stack Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3) on your desk. The agent asks questions on the device, you answer with its keyboard, and your chat history stays clean.

**适用场景 / Use cases:**
- 长时间运行的 agent 循环，你可能走开 / Long-running agent loops you walk away from
- 配对编程时不想让确认弹窗干扰聊天 / Pair-programming without approval clutter
- 关键决策需要蜂鸣提醒 / Critical decisions that need a buzzer

---

## 工作原理 / How It Works

```
┌──────────────────────────────────────────────────────────────┐
│  Agent A (CodeBuddy)  ──┐                                     │
│  Agent B (Claude Code) ──┤── ask-master.exe (stdio, JSON-RPC) │
│  Agent C (Cursor)     ──┘                                     │
│         │                                                     │
│         ▼  Unix Socket (多 agent 共享 / multi-agent sharing)   │
├──────────────────────────────────────────────────────────────┤
│  ask-master daemon (Go)                                       │
│  ├── ble_bridge.go    — BLE NUS 桥接 / BLE NUS bridge         │
│  ├── daemon.go        — 多会话 Unix socket + lock             │
│  └── tools.go         — confirm / choose / ask / escalate     │
│         │                                                     │
│         ▼  stdin/stdout (line-delimited JSON)                 │
├──────────────────────────────────────────────────────────────┤
│  ble_proxy.py (Python + bleak)                                │
│  ├── 永久重连循环 / permanent reconnect loop                  │
│  ├── GATT 连接 Nordic UART Service                            │
│  └── UTF-8 管道 / UTF-8 pipe (ASCII-escaped JSON)             │
│         │                                                     │
│         ▼  BLE NUS (6E400001 / RX:6E400002 / TX:6E400003)    │
├──────────────────────────────────────────────────────────────┤
│  Cardputer 固件 (ESP32-S3, Arduino / C++)                     │
│  ├── BLE 广播设备名 "ask-master"                              │
│  ├── NUS 收发 + portMUX 临界区保护                            │
│  ├── M5GFX canvas 渲染 (efontCN 中文字体)                     │
│  ├── 拼音输入法 (单字 + 词组联想)                              │
│  ├── 电量显示 (ADC 轮询, 10s 缓存)                            │
│  ├── 中英双语界面 (NVS 持久化, 按 L 切换)                     │
│  └── 键盘扫描 (Fn+方向键滚动, 长按连发)                       │
└──────────────────────────────────────────────────────────────┘
```

**关键设计 / Key design points:**

1. **BLE 取代 WiFi / BLE replaces WiFi** — 设备无需连 WiFi、无需 IP 配置。daemon 通过 BLE NUS 直连设备。/ No WiFi needed, no IP configuration. The daemon connects directly via BLE NUS.

2. **多 Agent 共享 / Multi-agent sharing** — 第一个启动的 exe 成为 daemon 并绑定 BLE 连接，后续 exe 自动作为 client 连接到同一个 Unix socket。/ The first launched exe becomes the daemon and holds the BLE connection; subsequent exes auto-detect it and proxy via the same Unix socket.

3. **单文件分发 / Single-file distribution** — `ble_proxy.py` 通过 `go:embed` 打入二进制，启动时释放到临时目录。单个 exe 拷到任何地方都能跑。/ `ble_proxy.py` is embedded via `go:embed`; a single exe works anywhere.

---

## 功能特性 / Features

| 特性 / Feature | 说明 / Description |
|---|---|
| BLE 传输 / BLE transport | 蓝牙直连，无需 WiFi / Direct Bluetooth, no WiFi needed |
| 中文拼音输入法 / Pinyin IME | 410 音节 + 8461 词组，支持单字和词组联想 / 410 syllables + 8461 phrases, single-char and phrase prediction |
| GB2312 过滤 / GB2312 filtering | 候选只含字体可显示的字 / Candidates limited to font-covered characters |
| 词语输入 / Phrase input | 输入 `nihao` 直接出"你好" / Type `nihao` → "你好" |
| 电量显示 / Battery indicator | 状态栏右上角百分比 + 图标，颜色分级 / Status bar percentage + icon, color-coded |
| 中英双语 / Bilingual UI | 休眠界面按 L 切换语言，NVS 持久化 / Press L on sleep screen, persisted in NVS |
| 自动换行 / Auto word-wrap | UTF-8 感知，CJK 任意断行，拉丁文整词换行 / UTF-8 aware, CJK breaks anywhere, Latin keeps words intact |
| 长按滚动 / Long-press scroll | 方向键长按 350ms 后每 90ms 滚一行 / Hold 350ms then scroll every 90ms |
| 闪烁告警 / Flashing alert | escalate 界面每 200ms 自主闪烁 / Escalate screen auto-flashes every 200ms |
| UTF-8 安全 / UTF-8 safe | Go→Python 管道 ASCII 转义，Windows GBK 不损坏中文 / Go→Python pipe ASCII-escaped, safe on Windows GBK |
| 超时与离线区分 / Timeout vs offline | `[CARDPUTER TIMEOUT]` ≠ `[CARDPUTER OFFLINE]` ≠ 人按 N / Distinct return values |

---

## 快速开始 / Quick Start

### 1. 构建服务端 / Build the server

**中文：** 需要 Go 1.22+ 和 Python 3.8+（安装 `bleak` 库）。

**English:** Requires Go 1.22+ and Python 3.8+ (install `bleak`).

```bash
# 安装 Python BLE 依赖 / Install Python BLE dependency
pip install bleak

# 构建 / Build
go build -o ask-master.exe .
```

### 2. 烧录固件 / Flash the firmware

**中文：** 使用 `fwbuild.ps1`（Windows）或 arduino-cli 编译，然后用 esptool 全量烧录。

**English:** Use `fwbuild.ps1` (Windows) or arduino-cli to compile, then flash with esptool.

```powershell
# 编译 / Compile
.\fwbuild.ps1

# 全量烧录 (bootloader + 分区表 + 固件) / Full flash
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash `
  0x0      firmware/ask_master_ble/build/ask_master_ble.ino.bootloader.bin `
  0x8000   firmware/ask_master_ble/build/ask_master_ble.ino.partitions.bin `
  0x10000  firmware/ask_master_ble/build/ask_master_ble.ino.bin
```

> **警告 / Warning:** M5Launcher 不兼容此固件 —— 它会用自己的分区表覆盖 app，导致黑屏。请用 esptool 烧录。/ M5Launcher is NOT compatible — it overwrites the partition table and causes a black screen. Use esptool.

### 3. 启动 daemon / Start the daemon

**中文：** 推荐用 `daemon_keeper.py` 常驻托管，BLE 保持连接。

**English:** Use `daemon_keeper.py` for persistent daemon management.

```bash
python daemon_keeper.py
```

或手动运行 / Or run manually:
```bash
./ask-master.exe --transport ble --log-level debug
```

### 4. 配置 MCP 客户端 / Configure MCP client

**中文：** 在你的 agent 的 MCP 配置文件中添加：

**English:** Add to your agent's MCP config file:

```json
{
  "mcpServers": {
    "ask-master": {
      "command": "d:\\dev\\WorkBuddy-Cardputer-BLE\\ask-master.exe",
      "args": ["--transport", "ble"]
    }
  }
}
```

---

## 让任意 Agent 接入 / Connecting Any Agent

**中文：** ask-master 是标准 MCP server，任何支持 MCP 的 agent 都能接入。daemon 模式自动处理多 agent 并发——请求排队串行处理，不会冲突。

**English:** ask-master is a standard MCP server. Any MCP-capable agent can connect. The daemon mode handles multi-agent concurrency automatically — requests are queued and processed serially.

### CodeBuddy

`~/.codebuddy/mcp.json`:
```json
{
  "mcpServers": {
    "ask-master": {
      "command": "/path/to/ask-master.exe",
      "args": ["--transport", "ble"],
      "type": "stdio"
    }
  }
}
```

### Claude Code

`~/.claude/settings.json`:
```json
{
  "mcpServers": {
    "ask-master": {
      "command": "/path/to/ask-master.exe",
      "args": ["--transport", "ble"]
    }
  }
}
```

### Cursor

`~/.cursor/mcp.json`:
```json
{
  "mcpServers": {
    "ask-master": {
      "command": "/path/to/ask-master.exe",
      "args": ["--transport", "ble"]
    }
  }
}
```

### Windsurf

`~/.codeium/windsurf/mcp_config.json`:
```json
{
  "mcpServers": {
    "ask-master": {
      "command": "/path/to/ask-master.exe",
      "args": ["--transport", "ble"]
    }
  }
}
```

> **中文：** 多个 agent 可以同时配置，共享同一个 daemon 和 BLE 连接。
>
> **English:** Multiple agents can be configured simultaneously, sharing the same daemon and BLE connection.

---

## MCP 工具 / MCP Tools

| 工具 / Tool | 用途 / Purpose | 设备显示 / Device shows | 回复方式 / Reply |
|---|---|---|---|
| `confirm` | 是/否确认 / Yes/no decision | 确认框 + Y/N / Confirm box + Y/N | `true` / `false` |
| `choose` | 多选一 / Multiple choice | 编号列表 / Numbered list | 选项文本 / Option text |
| `ask-human` | 文本输入 / Free-text input | 问题 + 输入行 / Question + input row | 用户输入 / User's text |
| `escalate-to-human` | 紧急确认 / Urgent alert | 闪烁标题 + 输入行 / Flashing title + input | 用户输入 / User's text |

**中文：** 设备离线时返回安全兜底值（`[CARDPUTER OFFLINE]`），超时返回 `[CARDPUTER TIMEOUT]`，两者与人按 N 的 `false` 明确区分。agent 永远不会卡住。

**English:** When the device is offline, tools return safe fallbacks (`[CARDPUTER OFFLINE]`). On timeout, `[CARDPUTER TIMEOUT]`. Both are distinct from a human pressing N (`false`). The agent never hangs.

---

## 中文拼音输入法 / Pinyin IME

**中文：** 在 `ask-human` 和 `escalate-to-human` 界面中，默认启用拼音输入法。

**English:** The pinyin IME is enabled by default in `ask-human` and `escalate-to-human` screens.

### 操作 / Key bindings

| 按键 / Key | 功能 / Action |
|---|---|
| `a`–`z` | 输入拼音字母 / Type pinyin letters |
| `1`–`9` | 选择候选 / Select candidate |
| `空格 / Space` | 选第一个候选 / Select first candidate |
| `opt` | 切换中/英文模式 / Toggle CN/EN mode |
| `backspace` | 删拼音字母 / 删已确认字 / Delete pinyin / delete committed char |
| `回车 / Enter` | 发送 / Send |

### 候选优先级 / Candidate priority

1. 单字精确匹配 / Single-char exact match（`ni` → `你`）
2. 词组精确匹配 / Phrase exact match（`nihao` → `你好`）
3. 词组前缀匹配 / Phrase prefix match（`ni` → `你好`, `你们`...）
4. 单字后缀匹配 / Single-char suffix match（`nihao` → 匹配 `hao` → `好`）

### 生成词库 / Regenerating dictionaries

```bash
python gen_pinyin_dict.py       # 单字表 (GB2312 过滤, 字频排序)
python gen_pinyin_phrases.py    # 词组表 (jieba 词频, ≥500)
```

---

## 固件构建 / Firmware Build

**中文：** 固件使用 `huge_app` 分区方案（3MB app 分区），容纳中文字体和拼音词库。

**English:** The firmware uses the `huge_app` partition scheme (3MB app partition) to fit CJK fonts and the pinyin dictionary.

### 依赖 / Dependencies

```bash
arduino-cli core install m5stack:esp32@2.1.4
arduino-cli lib install "M5Unified@0.2.19" "M5GFX@0.2.26" "NimBLE-Arduino@1.4.0" "ArduinoJson@7.0.4"
# 将 M5CardputerBLE/ 安装为 Arduino 库 / Install M5CardputerBLE/ as Arduino library
```

### 编译 / Compile

```powershell
.\fwbuild.ps1    # Windows
```

或手动 / Or manually:
```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=huge_app \
  --build-path firmware/ask_master_ble/build firmware/ask_master_ble
```

### 烧录 / Flash

```bash
# 首次烧录需全量写入 / First flash requires full write
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash \
  0x0      firmware/ask_master_ble/build/ask_master_ble.ino.bootloader.bin \
  0x8000   firmware/ask_master_ble/build/ask_master_ble.ino.partitions.bin \
  0x10000  firmware/ask_master_ble/build/ask_master_ble.ino.bin

# 后续更新只需刷 app / Subsequent updates only need app partition
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash \
  0x10000 firmware/ask_master_ble/build/ask_master_ble.ino.bin
```

### 固件特性 / Firmware features

| 特性 / Feature | 说明 / Description |
|---|---|
| 分区方案 / Partition | `huge_app` (3MB app, no OTA) |
| Flash 占用 / Flash usage | ~42% (1.27MB / 3MB) |
| 字体 / Fonts | `efontCN_12` (chrome) + `efontCN_16` (body), GB2312 范围 |
| 拼音词库 / Pinyin dict | ~6700 单字 (64KB) + 8461 词组 (124KB) |
| 电量 / Battery | ADC1_GPIO10, 10s 缓存, 颜色分级 |
| 语言 / Language | 中/英双语, NVS 持久化, 按 L 切换 |
| 接收缓冲 / RX buffer | 4KB (支持长中文消息) |

---

## CLI 参数 / CLI Flags

| 参数 / Flag | 默认值 / Default | 说明 / Description |
|---|---|---|
| `--transport` | `ws` | `ws` (WebSocket) 或 `ble` (BLE NUS, 推荐) / `ws` or `ble` (recommended) |
| `--timeout` | `300` | 默认工具超时（秒）/ Default tool timeout (seconds) |
| `--log-level` | `info` | `debug` / `info` / `warn` / `error` |
| `--version` | — | 打印版本并退出 / Print version and exit |

---

## 故障排查 / Troubleshooting

<details>
<summary><b>设备显示 OFFLINE / Device shows OFFLINE</b></summary>

**中文：**
- 确认 daemon 正在运行：`Get-Process ask-master,python`
- 确认 `ble_proxy.py` 进程存在（由 daemon 自动拉起）
- 手动运行 `python ble_proxy.py` 查看扫描与连接日志
- BLE 设备被连接后停止广播——扫不到不代表离线

**English:**
- Verify daemon is running: `Get-Process ask-master,python`
- Verify `ble_proxy.py` process exists (auto-spawned by daemon)
- Run `python ble_proxy.py` manually to see scan/connect logs
- BLE devices stop advertising while connected — not found ≠ offline
</details>

<details>
<summary><b>中文显示为方块 / Chinese shows as boxes</b></summary>

**中文：** efontCN 只覆盖 GB2312（6763 字）。词库已过滤，但如果 daemon 发送了 GB2312 以外的字，仍会显示方块。

**English:** efontCN only covers GB2312 (6763 chars). The dictionary is filtered, but characters outside GB2312 sent by the daemon will still show as boxes.
</details>

<details>
<summary><b>按 N 与超时返回值相同 / N and timeout return same value</b></summary>

**中文：** 已修复。按 N 返回 `false`，超时返回 `[CARDPUTER TIMEOUT]`，离线返回 `[CARDPUTER OFFLINE]`。

**English:** Fixed. Pressing N returns `false`, timeout returns `[CARDPUTER TIMEOUT]`, offline returns `[CARDPUTER OFFLINE]`.
</details>

<details>
<summary><b>多个 CodeBuddy 进程冲突 / Multiple CodeBuddy processes conflict</b></summary>

**中文：** daemon 模式自动处理。第一个 exe 成为 daemon，后续 exe 作为 client 连接同一 Unix socket。请求排队串行处理。

**English:** Daemon mode handles this automatically. The first exe becomes the daemon; subsequent exes connect as clients to the same Unix socket. Requests are queued serially.
</details>

<details>
<summary><b>M5Launcher 烧入后黑屏 / Black screen after M5Launcher flash</b></summary>

**中文：** M5Launcher 用自己的分区表覆盖了 app 分区。请用 esptool 全量烧录（bootloader + partitions + firmware）。

**English:** M5Launcher overwrites the partition table. Use esptool for a full flash (bootloader + partitions + firmware).
</details>

---

## 项目结构 / Project Layout

```
.
├── ask-master.exe               — 编译产物 / compiled binary
├── ble_bridge.go                — BLE NUS 桥接 (go:embed ble_proxy.py)
├── ble_proxy.py                 — Python BLE central (bleak, 永久重连)
├── daemon_keeper.py             — daemon 常驻托管脚本
├── bridge.go                    — WebSocket 桥接 (原版, 保留)
├── daemon.go                    — 多会话 Unix socket daemon
├── tools.go                     — MCP 工具注册 (confirm/choose/ask/escalate)
├── config.go                    — CLI 参数
├── main.go                      — 入口, daemon/client 路由
├── stdio_filter.go              — stdio JSON-RPC 过滤
├── internal/truncate/           — 字符串截断 (UTF-8 安全)
├── gen_pinyin_dict.py           — 单字拼音词库生成器
├── gen_pinyin_phrases.py        — 词组拼音词库生成器
├── fwbuild.ps1                  — 固件编译脚本 (Windows)
├── firmware/
│   └── ask_master_ble/          — BLE 固件源码
│       ├── ask_master_ble.ino   — 主程序 (状态机, BLE, 键盘)
│       ├── ui.cpp / ui.h        — UI 渲染 (canvas, 字体, 换行, 电池)
│       ├── pinyin_ime.cpp / .h  — 拼音输入法引擎
│       ├── pinyin_dict.h        — 单字词库 (自动生成)
│       ├── pinyin_phrases.h     — 词组词库 (自动生成)
│       ├── config.h             — 固件配置
│       └── ui_theme.h           — 颜色主题
├── M5CardputerBLE/              — BLE 适配版 M5Cardputer 库
├── plugin/                      — Claude Code 插件 (原版)
├── skill/                       — 升级策略文档
└── README.md                    — 本文件
```

---

## 许可证 / License

[MIT](LICENSE)

**中文：** 本项目基于 [mhrsntrk/ask-master](https://github.com/mhrsntrk/ask-master) 修改，原许可证为 MIT。

**English:** This project is forked from [mhrsntrk/ask-master](https://github.com/mhrsntrk/ask-master) under the MIT license.
