# ask-master BLE 协议规范 v2（Claude Hardware Buddy 兼容 + 扩展）

> 本文档是固件、PC 端（Go daemon / ble_proxy.py）与 MCP server 的共同依据。
> 基线协议 = Anthropic 官方 **Claude Hardware Buddy** 线协议（Nordic UART Service，换行分隔 UTF-8 JSON），
> 在此之上增加**向后兼容扩展**（多选项、自由文本、语音），官方 Claude 桌面应用可识别并正常使用设备。

## 1. 传输与设备名

| 项 | 值 |
|---|---|
| 服务 | Nordic UART Service（NUS） |
| 服务 UUID | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX（桌面→设备，write） | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX（设备→桌面，notify） | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |
| 广播名 | 以 `Claude` 开头（如 `Claude AskMaster`），供官方设备选择器过滤 |
| 帧格式 | UTF-8 JSON，一行一个对象，`\n` 结尾；收发双方均需按行重组 |
| 安全 | 建议 LE Secure Connections 绑定 + 6 位 passkey + 特征加密（见 §8） |

## 2. 消息模型

- **桌面 → 设备**：心跳快照（含可选 `prompt`）、命令（`cmd`）、时间/所有者一次性消息。
- **设备 → 桌面**：对 prompt 的决定（`permission`）、自由文本（`input`）、对命令的 `ack`、状态 `status`、语音帧（扩展）。
- 所有与一次交互绑定的消息都携带 `id`，与 `prompt.id` 严格一致。

## 3. 心跳快照（官方字段 + 扩展）

桌面在状态变化时发送，空闲时每 10s 发一次保活：

```json
{
  "total": 1,            // 会话总数
  "running": 0,          // 正在生成的会话数
  "waiting": 1,          // 等待人类输入的会话数
  "msg": "等待输入: git 提交信息",
  "entries": ["10:42 git push", "10:41 阅读文件..."],
  "tokens": 184502,      // 累计输出 token
  "tokens_today": 31200, // 当日输出 token
  "prompt": null         // 见 §4；无待办时为 null
}
```

无 `prompt` 时设备显示状态摘要（`msg`/`entries`），进入 IDLE。

## 4. prompt（官方 + 扩展）

`prompt` 非空表示需要人类输入，字段如下：

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string | 必填，交互唯一 id，回复时必须原样带回 |
| `tool` | string | 工具名（如 `ask-human`、`confirm`、`Bash`） |
| `hint` | string | 一句话提示（用作问题标题） |
| `context` | string | 扩展，附加上下文（可滚动） |
| `options` | string[] | 扩展，**多选**：2 个以上选项时出现（含二元确认） |
| `input` | bool | 扩展，`true` = 自由文本/语音输入（无 options 时） |
| `escalated` | bool | 扩展，`true` = 紧急提示（告警闪烁/提示音） |

**渲染规则（设备侧）：**

- `options` 存在且 `length >= 2` → 选项列表界面，数字键 `1..N` 选择
- `input == true` → 文本输入界面（拼音 IME / 语音），回车发送
- 两者都无 → 二元确认（A=同意 / B=拒绝），兼容官方桌面
- `escalated` → 高优先级提示音与闪烁

示例：

```json
{ "prompt": { "id": "req_x", "tool": "choose",
              "hint": "如何处理?",
              "options": ["推送", "跳过", "修改提交信息", "拒绝"] } }

{ "prompt": { "id": "req_y", "tool": "ask-human",
              "hint": "git commit message?",
              "input": true } }
```

## 5. 设备 → 桌面回复

### 5.1 多选 / 二元确认（`permission`，官方命令 + 扩展字段）

```json
{ "cmd": "permission", "id": "req_x", "decision": "once", "option": 3 }
{ "cmd": "permission", "id": "req_x", "decision": "deny" }
```

- 官方桌面只识别 `decision`（`once`/`deny`），`option` 为扩展字段，官方忽略；
- MCP server 读取 `option` 映射回 agent（未选择多选时 `option` 缺省）。

### 5.2 自由文本（`input`，扩展命令）

```json
{ "cmd": "input", "id": "req_y", "text": "修复BLE粘包" }
```

官方桌面不认识该命令则忽略；MCP server 将其作为 `SendAndWait` 的返回值。

### 5.3 状态上报（官方）

```json
{ "ack": "status", "ok": true, "data": {
  "name": "Claude AskMaster", "sec": true,
  "bat": { "pct": 87, "mV": 4012, "mA": -120, "usb": true },
  "sys": { "up": 8412, "heap": 84200 },
  "stats": { "appr": 42, "deny": 3 }
} }
```

缺少的字段可省略。

## 6. 命令与 ack（官方）

| 命令 | 载荷 | 设备 ack |
|---|---|---|
| `{"cmd":"status"}` | — | 见 §5.3 |
| `{"cmd":"name","name":"..."}` | 设置显示名 | `{"ack":"name","ok":true}` |
| `{"cmd":"owner","name":"..."}` | 设置所有者 | `{"ack":"owner","ok":true}` |
| `{"cmd":"unpair"}` | 清除绑定 | `{"ack":"unpair","ok":true}` |

任何带 `cmd` 的桌面消息都需要 `ack`（`ok:false` + `error` 表示失败）。

## 7. 语音扩展（自由文本的语音答复通道）

**方向：设备 → 桌面**，逐帧 base64，`\n` 行分隔：

```json
{ "evt": "audio", "seq": 1, "fmt": "adpcm", "rate": 16000, "ch": 1, "d": "<base64>" }
{ "evt": "audio_end", "seq": 12, "id": "req_y" }
```

- 仅在 `prompt.input == true` 且用户按住说话键（PTT）时发送；
- 16kHz / 16-bit / mono，IMA-ADPCM 4:1 压缩；
- PC 端收到 `audio_end` 后转写（faster-whisper 或云 ASR），文本作为 `input` 回复提交给 agent；
- 官方桌面忽略 `evt` 消息，不影响兼容。

## 8. 加密与配对（官方安全机制）

1. 设备以 **DisplayOnly** IO 能力广播，NUS 特征（RX/TX 及 TX CCCD）标记为**仅加密访问**；
2. 首次 GATT 访问触发 OS 配对，设备屏幕显示 6 位 **passkey**，桌面端确认后链路升级为 **AES-CCM**（LE Secure Connections）；
3. 重连复用已存 LTK，不再弹窗；
4. 配对完成后，`status` 的 `data.sec` 返回 `true`；
5. 收到 `{"cmd":"unpair"}` 清除绑定，下次重新配对；
6. 为兼容无配对能力的客户端，设备侧提供配置开关 `BLE_SECURE`（默认开启，可关闭后明文工作）。

## 9. 兼容矩阵

| 客户端 | 心跳显示 | 二元确认 | 多选(3+) | 自由文本 | 语音 | 加密 |
|---|---|---|---|---|---|---|
| 官方 Claude 桌面 | ✅ | ✅ | 显示 options（忽略） | 忽略 input | 忽略 | ✅ |
| ask-master MCP server | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| 旧版 ask-master（升级前固件） | — | — | — | — | — | — |

## 10. 设备状态机

```
IDLE ──收到含 prompt 的心跳──▶ RENDERING ──▶ WAITING_INPUT
  ▲                                  │
  └──────── 回复已发送 ──────────────┴──▶ SENDING ──▶ IDLE
```

- IDLE：显示心跳摘要（msg/entries），空闲保活
- WAITING_INPUT：选项界面（数字键）或输入界面（拼音/语音 PTT）
- SENDING：等待桌面 ack/下一条心跳，超时回 IDLE
