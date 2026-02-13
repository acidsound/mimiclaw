# MimiClaw: $5 芯片上的口袋 AI 助理

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![DeepWiki](https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg)](https://deepwiki.com/memovai/mimiclaw)
[![Discord](https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white)](https://discord.gg/r8ZxSvB8Yr)
[![X](https://img.shields.io/badge/X-@ssslvky-black?logo=x)](https://x.com/ssslvky)

**[English](README.md) | [中文](README_CN.md)**

> [!IMPORTANT]
> **v0.2.0 新功能：**
> - 支持 **Kimi (Moonshot AI)** 和 **OpenAI 兼容** 的提供商！
> - **新增 Telegram 多媒体支持：** 语音消息自动转写 STT，图片消息支持 vision 输入。
> - **LLM 驱动调度器 (NEW!):** 在 Telegram 上说“3 分钟后提醒我”，Mimi 会自动管理 `/spiffs/public/schedule.md` 来触发闹钟。
> - **实时时间注入:** 通过将当前系统时间（KST）直接注入 LLM 上下文，增强了时间感知能力。

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="480" />
</p>

**$5 芯片上的 AI 助理（OpenClaw）。没有 Linux，没有 Node.js，纯 C。**

MimiClaw 把一块小小的 ESP32-S3 开发板变成你的私人 AI 助理。插上 USB 供电，连上 WiFi，通过 Telegram 跟它对话 — 它能处理你丢给它的任何任务，还会随时间积累本地记忆不断进化 — 全部跑在一颗拇指大小的芯片上。

## 认识 MimiClaw

- **小巧** — 没有 Linux，没有 Node.js，没有臃肿依赖 — 纯 C
- **好用** — 在 Telegram 发消息，剩下的它来搞定
- **忠诚** — 从记忆中学习，跨重启也不会忘
- **能干** — USB 供电，0.5W，24/7 运行
- **可爱** — 一块 ESP32-S3 开发板，$5，没了

## 工作原理

![](assets/mimiclaw.png)

你在 Telegram 发一条消息，ESP32-S3 通过 WiFi 收到后送进 Agent 循环 — Claude 思考、调用工具、读取记忆 — 再把回复发回来。一切都跑在一颗 $5 的芯片上，所有数据存在本地 Flash。

## 快速开始

### 你需要

- 一块 **ESP32-S3 开发板**，16MB Flash + 8MB PSRAM（如小智 AI 开发板，~¥30）
- 一根 **USB Type-C 数据线**
- 一个 **Telegram Bot Token** — 在 Telegram 找 [@BotFather](https://t.me/BotFather) 创建
- 一个 **Anthropic API Key** (Claude) 或 **Moonshot API Key** (Kimi)
- 一个可选的 **STT API Key**（用于语音消息，例如 Groq）

### 安装

```bash
# 需要先安装 ESP-IDF v5.5+:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3

# 针对 4MB Flash / 2MB PSRAM 设备优化
# (已在 sdkconfig.defaults.esp32s3 和 partitions.csv 中预配置)
```

### 配置

MimiClaw 使用**两层配置**：`mimi_secrets.h` 提供编译时默认值，串口 CLI 可在运行时覆盖。CLI 设置的值存在 NVS Flash 中，优先级高于编译时值。

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

编辑 `main/mimi_secrets.h`：

```c
#define MIMI_SECRET_WIFI_SSID       "你的WiFi名"
#define MIMI_SECRET_WIFI_PASS       "你的WiFi密码"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"

/* 选择提供商: MIMI_LLM_PROVIDER_ANTHROPIC 或 MIMI_LLM_PROVIDER_OPENAI */
#define MIMI_SECRET_PROVIDER        MIMI_LLM_PROVIDER_ANTHROPIC 
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_BASE_URL        "https://api.anthropic.com/v1/messages" // 或 https://api.moonshot.cn/v1/chat/completions

#define MIMI_SECRET_SEARCH_KEY      ""              // 可选：Brave Search API key
#define MIMI_SECRET_PROXY_HOST      ""              // 可选：代理地址
#define MIMI_SECRET_PROXY_PORT      ""              // 可选：代理端口

/* STT (Groq/Whisper) */
#define MIMI_SECRET_STT_PROVIDER    MIMI_STT_PROVIDER_GROQ
#define MIMI_SECRET_STT_KEY         ""
#define MIMI_SECRET_STT_BASE_URL    "https://api.groq.com/openai/v1"
#define MIMI_SECRET_STT_MODEL       "whisper-large-v3"
```

然后编译烧录：

```bash
# 完整编译（修改 mimi_secrets.h 后必须 fullclean）
idf.py fullclean && idf.py build

# 查找串口
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# 烧录并监控（将 PORT 替换为你的串口）
# USB 转接器：大概率是 /dev/cu.usbmodem11401（macOS）或 /dev/ttyACM0（Linux）
idf.py -p PORT flash monitor
```

### 代理配置（国内用户）

在国内需要代理才能访问 Telegram 和 Anthropic API。MimiClaw 内置 HTTP CONNECT 隧道支持。

**前提**：局域网内有一个支持 HTTP CONNECT 的代理（Clash Verge、V2Ray 等），并开启了「允许局域网连接」。

可以在 `mimi_secrets.h` 中编译时设置，也可以通过串口 CLI 随时修改：

```
mimi> set_proxy 192.168.1.83 7897   # 设置代理
mimi> clear_proxy                    # 清除代理
```

> **提示**：确保 ESP32-S3 和代理机器在同一局域网。Clash Verge 在「设置 → 允许局域网」中开启。

### CLI 命令

通过串口连接即可配置和调试。**配置命令**让你无需重新编译就能修改设置 — 随时随地插上 USB 线就能改。

**运行时配置**（存入 NVS，覆盖编译时默认值）：

```
mimi> wifi_set MySSID MyPassword   # 换 WiFi
mimi> wifi_reset                   # 重置 WiFi 凭据（清 NVS + 禁用编译时回退）
mimi> wifi_portal 600              # 启动 SoftAP 配网门户（单位：秒）
mimi> set_tg_token 123456:ABC...   # 换 Telegram Bot Token

# LLM 配置
mimi> set_provider kimi            # 切换到 Kimi (Moonshot AI)
mimi> set_base_url https://api.moonshot.cn/v1/chat/completions
mimi> set_api_key sk-xxxxxxxx...   # 设置 Kimi API Key
mimi> set_model moonshot-v1-8k     # 设置 Kimi 模型

mimi> set_proxy 192.168.1.83 7897  # 设置代理
mimi> clear_proxy                  # 清除代理
mimi> set_search_key BSA...        # 设置 Brave Search API Key
mimi> set_stt_provider groq         # 设置 STT 提供商（当前支持: groq）
mimi> set_stt_base_url https://api.groq.com/openai/v1
mimi> set_stt_key gsk_...           # 设置 STT API Key
mimi> set_stt_model whisper-large-v3
mimi> set_media_limits 1024 400 10   # 图片KB/语音KB/秒数
mimi> config_show                  # 查看所有配置（脱敏显示）
mimi> config_reset                 # 清除 NVS，恢复编译时默认值
```

### 烧录后上手流程（串口 + SoftAP + Telegram）

推荐按以下顺序操作：

1. 先看网络状态：
```bash
mimi> wifi_status
```

2. 已知 WiFi 账号密码时，直接设置：
```bash
mimi> wifi_set MySSID MyPassword
mimi> restart
```

3. 不知道密码或环境变化时，用 SoftAP 配网：
```bash
mimi> wifi_reset
mimi> wifi_portal 600
```
- SoftAP SSID：`MimiClaw-XXXX`（随机后缀）
- SoftAP 密码：`mimiclaw1`
- 门户地址：`http://192.168.4.1`
- STA 连接成功后，SoftAP/门户会自动关闭。

4. 设置 Telegram token：
```bash
mimi> set_tg_token 123456:ABC...
mimi> restart
```

5. 授权 Telegram chat_id：
```bash
mimi> tg_auth_add <chat_id>
mimi> tg_auth_list
```

6. 在 Telegram 里发送 `/help`，确认命令路由正常。

**调试与运维：**

```
mimi> ls [path]                # 查看文件列表 (例如: ls public)
mimi> cat <path>               # 查看文件内容 (支持分页)
mimi> rm <path>                # 删除文件 (例如: rm public/temp.md)
mimi> wifi_status              # 连上了吗？
mimi> memory_read              # 看看它记住了什么
mimi> memory_write "内容"       # 写入 MEMORY.md
mimi> heap_info                # 还剩多少内存？
mimi> session_list             # 列出所有会话
mimi> session_clear 12345      # 删除一个会话
mimi> restart                  # 重启
```

## 记忆

MimiClaw 把所有数据存为纯文本文件，可以直接读取和编辑：

| 文件 | 说明 |
|------|------|
| `SOUL.md` | 机器人的人设 — 编辑它来改变行为方式 |
| `USER.md` | 关于你的信息 — 姓名、偏好、语言 |
| `MEMORY.md` | 长期记忆 — 它应该一直记住的事 |
| `wol_devices.json` | WOL 设备注册表（`label`,`name`,`ip`,`hostname`,`mac`） |
| `tg_12345.jsonl` | 聊天记录 — 你和它的对话 |

## 工具

MimiClaw 使用 Anthropic 的 tool use 协议 — Claude 在对话中可以调用工具，循环执行直到任务完成（ReAct 模式）。

| 工具 | 说明 |
|------|------|
| `web_search` | 通过 Brave Search API 搜索网页，获取实时信息 |
| `http_request` | 安全的 GET/POST 请求（含 SSRF 防护与密钥替换） |
| `wake_on_lan` | 发送 Wake-on-LAN 魔法包，可基于 mac 或 `list_devices` 中的 label/hostname/ip |
| `list_devices` | 通过后台 ICMP 子网扫描返回设备列表 |
| `wol_register` | 使用 `mac` 注册/更新 WOL 目标，支持可选 `label`、`hostname`、`ip` |
| `get_current_time` | 通过 HTTP 获取当前日期和时间，并设置系统时钟 |

启用网页搜索需要在 `mimi_secrets.h` 中设置 [Brave Search API key](https://brave.com/search/api/)（`MIMI_SECRET_SEARCH_KEY`）。

## Telegram 多媒体处理

- **语音转写（STT）**：Telegram 语音消息会流式提交到 STT（默认 Groq + Whisper），转写完成后加入会话上下文。
  - 默认限制：图片 1 MB、语音 400 KB、语音 10 秒。可通过 `set_media_limits` 调整。
- **图片输入（Vision）**：Telegram 图片会以流式下载，并以 image block 形式提供给模型。
- **失败与保护**：超限或转写失败时返回清晰提示文本，不触发任务崩溃。
- **展示安全**：`[web_search]` 같은标签现在会在 Telegram 渲染中安全显示，不会被误当作 Markdown 解析。

## 其他功能

- **WebSocket 网关** — 端口 18789，局域网内用任意 WebSocket 客户端连接
- **OTA 更新** — WiFi 远程刷固件，无需 USB
- **双核** — 网络 I/O 和 AI 处理分别跑在不同 CPU 核心
- **HTTP 代理** — CONNECT 隧道，适配受限网络
- **工具调用** — ReAct Agent 循环，Anthropic tool use 协议

## 硬件优化

MimiClaw 专为低成本 ESP32-S3 开发板（如 4MB Flash / 2MB PSRAM）进行了优化：

- **分区表** (`partitions.csv`)：定制布局，将 AI Agent 应用适配到 **4MB Flash**。
- **内存配置** (`sdkconfig.defaults.esp32s3`)：
    - **Flash 模式**：优化为 **QIO 4MB**。
    - **PSRAM**：配置为 **Quad SPI**（兼容性优于 Octal），支持 2MB。
    - **TLS/网络**：调整缓冲区大小，在不牺牲稳定性的前提下最小化 RAM 占用。

## OpenAI 及兼容提供商

MimiClaw 支持任何遵循 OpenAI Chat Completions API 格式的提供商。

**示例：OpenRouter (Kimi/DeepSeek/GLM)**:
  ```bash
  set_provider openai
  set_base_url https://openrouter.ai/api/v1/chat/completions
  set_api_key YOUR_OPENROUTER_KEY
  set_model moonshotai/kimi-k2 # 或 deepseek/deepseek-r1:free, z-ai/glm-4.5-air:free
  ```

**示例：OpenRouter (Claude)**
```bash
mimi> set_provider openai
mimi> set_base_url https://openrouter.ai/api/v1/chat/completions
mimi> set_api_key sk-or-v1-...
mimi> set_model anthropic/claude-3-opus
```

**示例：DeepSeek (V2)**
```bash
mimi> set_provider openai
mimi> set_base_url https://api.deepseek.com/chat/completions
mimi> set_api_key sk-ds-...
mimi> set_model deepseek-chat
```

## 开发者

技术细节在 `docs/` 文件夹：

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — 系统设计、模块划分、任务布局、内存分配、协议、Flash 分区
- **[docs/TODO.md](docs/TODO.md)** — 功能差距和路线图

## 许可证

MIT

## 致谢

灵感来自 [OpenClaw](https://github.com/openclaw/openclaw) 和 [Nanobot](https://github.com/HKUDS/nanobot)。MimiClaw 为嵌入式硬件重新实现了核心 AI Agent 架构 — 没有 Linux，没有服务器，只有一颗 $5 的芯片。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
