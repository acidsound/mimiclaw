# MimiClaw: Pocket AI Assistant on a $5 Chip

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![DeepWiki](https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg)](https://deepwiki.com/memovai/mimiclaw)
[![Discord](https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white)](https://discord.gg/r8ZxSvB8Yr)
[![X](https://img.shields.io/badge/X-@ssslvky-black?logo=x)](https://x.com/ssslvky)

**English | [한국어](README.md) | [中文](README_CN.md)**

> [!IMPORTANT]
> **New in v0.2.0:**
> - Support for **Kimi (Moonshot AI)** and **OpenAI-compatible** providers!
> - **Telegram media support:** voice notes can be transcribed via STT and photos can be passed to vision flow.
> - **LLM-Powered Scheduler (NEW!):** Simply say "Remind me in 3 minutes" on Telegram, and Mimi will automatically manage `/spiffs/public/schedule.md` to trigger alarms.
> - **Real-time Time Injection:** Enhanced time awareness by injecting the current system time (KST) directly into the LLM context.
> - **Robust Error Recovery:** Added specialized handling for non-standard tool-calling errors (e.g., Gemini's array-wrapped errors) to ensure stable agent turn recovery.

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="480" />
</p>

**The world's first AI assistant(OpenClaw) on a $5 chip. No Linux. No Node.js. Just pure C**

MimiClaw turns a tiny ESP32-S3 board into a personal AI assistant. Plug it into USB power, connect to WiFi, and talk to it through Telegram — it handles any task you throw at it and evolves over time with local memory — all on a chip the size of a thumb.

## Meet MimiClaw

- **Tiny** — No Linux, no Node.js, no bloat — just pure C
- **Handy** — Message it from Telegram, it handles the rest
- **Loyal** — Learns from memory, remembers across reboots
- **Energetic** — USB power, 0.5 W, runs 24/7
- **Lovable** — One ESP32-S3 board, $5, nothing else

## How It Works

![](assets/mimiclaw.png)

You send a message on Telegram. The ESP32-S3 picks it up over WiFi, feeds it into an agent loop — Claude thinks, calls tools, reads memory — and sends the reply back. Everything runs on a single $5 chip with all your data stored locally on flash.

## Quick Start

### What You Need

- An **ESP32-S3 dev board** with 16 MB flash and 8 MB PSRAM (e.g. Xiaozhi AI board, ~$10)
- A **USB Type-C cable**
- A **Telegram bot token** — talk to [@BotFather](https://t.me/BotFather) on Telegram to create one
- An **Anthropic API key** (Claude) OR **Moonshot API key** (Kimi)
- An optional **STT API key** for voice notes (e.g., Groq)

### Install

```bash
# You need ESP-IDF v5.5+ installed first:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

# Optimized for 4MB Flash / 2MB PSRAM devices
# (Pre-configured in sdkconfig.defaults.esp32s3 and partitions.csv)
idf.py set-target esp32s3
```

### Configure

MimiClaw uses a **two-layer config** system: build-time defaults in `mimi_secrets.h`, with runtime overrides via the serial CLI. CLI values are stored in NVS flash and take priority over build-time values.

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

Edit `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_WIFI_SSID       "YourWiFiName"
#define MIMI_SECRET_WIFI_PASS       "YourWiFiPassword"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"

/* Choose Provider: MIMI_LLM_PROVIDER_ANTHROPIC or MIMI_LLM_PROVIDER_OPENAI */
#define MIMI_SECRET_PROVIDER        MIMI_LLM_PROVIDER_ANTHROPIC 
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_BASE_URL        "https://api.anthropic.com/v1/messages" // or https://api.moonshot.cn/v1/chat/completions

#define MIMI_SECRET_SEARCH_KEY      ""              // optional: Brave Search API key
#define MIMI_SECRET_PROXY_HOST      ""              // optional: e.g. "10.0.0.1"
#define MIMI_SECRET_PROXY_PORT      ""              // optional: e.g. "7897"

/* STT (Groq/Whisper) */
#define MIMI_SECRET_STT_PROVIDER    MIMI_STT_PROVIDER_GROQ
#define MIMI_SECRET_STT_KEY         ""
#define MIMI_SECRET_STT_BASE_URL    "https://api.groq.com/openai/v1"
#define MIMI_SECRET_STT_MODEL       "whisper-large-v3"
```

Then build and flash:

```bash
# Clean build (required after any mimi_secrets.h change)
idf.py fullclean && idf.py build

# Find your serial port
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# Flash and monitor (replace PORT with your port)
# USB adapter: likely /dev/cu.usbmodem11401 (macOS) or /dev/ttyACM0 (Linux)
idf.py -p PORT flash monitor
```

### CLI Commands

Connect via serial to configure or debug. **Config commands** let you change settings without recompiling — just plug in a USB cable anywhere.

**Runtime config** (saved to NVS, overrides build-time defaults):

```
mimi> wifi_set MySSID MyPassword   # change WiFi network
mimi> wifi_reset                   # reset WiFi creds (NVS + disable secret fallback)
mimi> wifi_portal 600              # start SoftAP portal for 10 minutes (seconds)
mimi> set_tg_token 123456:ABC...   # change Telegram bot token

# LLM Configuration
mimi> set_provider kimi            # switch to Kimi (Moonshot AI)
mimi> set_base_url https://api.moonshot.cn/v1/chat/completions
mimi> set_api_key sk-xxxxxxxx...   # set Kimi API key
mimi> set_model moonshot-v1-8k     # set Kimi model

mimi> set_proxy 127.0.0.1 7897     # set HTTP proxy
mimi> clear_proxy                  # remove proxy
mimi> set_search_key BSA...        # set Brave Search API key
mimi> set_stt_provider groq        # set STT provider (currently: groq)
mimi> set_stt_base_url https://api.groq.com/openai/v1
mimi> set_stt_key gsk_...         # set STT API key
mimi> set_stt_model whisper-large-v3
mimi> set_media_limits 1024 400 10 # photo KB / voice KB / max seconds
mimi> tg_auth_add 12345            # authorize a Telegram user (chat_id)
mimi> tg_auth_remove 12345         # deauthorize a user
mimi> tg_auth_list                 # list authorized users
mimi> config_show                  # show all config (masked)
mimi> config_reset                 # clear NVS, revert to build-time defaults
```

### Post-Flash Onboarding (Serial + SoftAP + Telegram)

Use this flow after flashing:

1. Check current network status:
```bash
mimi> wifi_status
```

2. If you already know WiFi credentials, set them directly:
```bash
mimi> wifi_set MySSID MyPassword
mimi> restart
```

3. If WiFi changed or you do not know credentials, use SoftAP provisioning:
```bash
mimi> wifi_reset
mimi> wifi_portal 600
```
- SoftAP SSID: `MimiClaw-XXXX` (random suffix)
- SoftAP password: `mimiclaw1`
- Portal URL: `http://192.168.4.1`
- After successful STA connect, SoftAP/portal are stopped automatically.

4. Set Telegram token:
```bash
mimi> set_tg_token 123456:ABC...
mimi> restart
```

5. Authorize Telegram chat IDs:
```bash
mimi> tg_auth_add <chat_id>
mimi> tg_auth_list
```

6. In Telegram, run `/help` to verify command routing.

**Debug & maintenance:**

```
mimi> ls [path]                # list files (e.g., ls public)
mimi> cat <path>               # read file (paging supported)
mimi> rm <path>                # delete file (e.g., rm public/temp.md)
mimi> wifi_status              # am I connected?
mimi> memory_read              # see what the bot remembers
mimi> memory_write "content"   # write to MEMORY.md
mimi> heap_info                # how much RAM is free?
mimi> session_list             # list all chat sessions
mimi> session_clear 12345      # wipe a conversation
mimi> restart                  # reboot
```

## Security & Storage

MimiClaw enforces strict **directory-based isolation** to protect your data.

- **Public (`/spiffs/public/`)**: Accessible by LLM tools. Contains memories and personality files.
- **Private (`/spiffs/private/`)**: Hidden from LLM tools. Contains secrets, sessions, and discovery data.
- **Log Redaction**: sensitive tokens and keys are automatically masked in system logs.

### File Structure

| File | Access | Description |
|------|--------|-------------|
| `SOUL.md` | Public | The bot's personality — edit this to change how it behaves |
| `USER.md` | Public | Info about you — name, preferences, language |
| `MEMORY.md` | Public | Long-term memory — things the bot should always remember |
| `SESSIONS.json` | Private | Persistent HTTP session cookies |
| `wol_devices.json` | Private | Registered WOL devices (`label`, `name`, `ip`, `hostname`, `mac`) |
| `SECRET.env` | Private | External API keys for the `http_request` tool |
| `tg_12345.jsonl` | Private | Chat history — your conversation with the bot |

## Tools

MimiClaw uses Anthropic's tool use protocol (ReAct pattern). Claude can call tools to interact with the world.

| Tool | Description |
|------|-------------|
| `web_search` | Search the web via Brave Search API for current information |
| `http_request` | **(Secure)** GET/POST with SSRF protection and secret substitution |
| `wake_on_lan` | Send Magic Packets to wake up devices in your home lab. You can call with mac or device label/hostname/ip from `list_devices`. |
| `wol_register` | Register or update a WOL target by `mac` with optional `label`, `hostname`, or `ip`. |
| `list_devices` | List hosts found via background ICMP subnet scanning |
| `get_current_time` | Fetch current date/time via HTTP and set the system clock |

### Secure HTTP Tool
The `http_request` tool is hardened for security:
- **SSRF Protection**: Blocks requests to local networks and private IPs.
- **Secret Substitution**: Use `{{SECRET:KEY}}` in URLs or bodies to inject keys from `SECRET.env`.
- **Session Support**: Automatically persists `Set-Cookie` headers for stateful interactions.
- **8KB Limit**: Prevents memory exhaustion on large responses.

To enable web search, set a [Brave Search API key](https://brave.com/search/api/) via `MIMI_SECRET_SEARCH_KEY` in `mimi_secrets.h`.

## Telegram media handling

- **Voice notes (STT):** Telegram voice messages are streamed and transcribed before being sent to the LLM.
  - Default limits: photo 1 MB, voice 400 KB, max 10 seconds. Use `set_media_limits` to change.
- **Photo input (Vision):** Telegram photos are downloaded by stream and included as image blocks for model input.
- **Safety behavior:** If STT fails or limits are exceeded, the bot responds with a short helpful message instead of crashing.
- **Render safety:** Search tags like `[web_search]` are handled safely in Telegram output rendering.

## Also Included

- **WebSocket gateway** on port 18789 — connect from your LAN with any WebSocket client
- **OTA updates** — flash new firmware over WiFi, no USB needed
- **Dual-core** — network I/O and AI processing run on separate CPU cores
- **HTTP proxy** — CONNECT tunnel support for restricted networks
- **Tool use** — ReAct agent loop with Anthropic tool use protocol

## Hardware Optimization

MimiClaw is tuned for low-cost ESP32-S3 boards (e.g. 4MB Flash / 2MB PSRAM):

- **Partition Table** (`partitions.csv`): Custom layout designed to fit the AI agent application within **4MB Flash**.
- **Memory Config** (`sdkconfig.defaults.esp32s3`):
    - **Flash Mode**: Optimized for **QIO 4MB**.
    - **PSRAM**: Configured for **Quad SPI** (more compatible than Octal) with 2MB support.
    - **TLS/Network**: Tuned buffer sizes to minimize RAM usage without sacrificing stability.

## OpenAI & Compatible Providers

MimiClaw supports any provider that follows the OpenAI Chat Completions API format.

**Example:- **OpenRouter (Kimi/DeepSeek/GLM)**:
  ```bash
  set_provider openai
  set_base_url https://openrouter.ai/api/v1/chat/completions
  set_api_key YOUR_OPENROUTER_KEY
  set_model moonshotai/kimi-k2 # or deepseek/deepseek-r1:free, z-ai/glm-4.5-air:free
  ```

**Example: OpenRouter (Claude)**
```bash
mimi> set_provider openai
mimi> set_base_url https://openrouter.ai/api/v1/chat/completions
mimi> set_api_key sk-or-v1-...
mimi> set_model anthropic/claude-3-opus
```

**Example: DeepSeek (V2)**
```bash
mimi> set_provider openai
mimi> set_base_url https://api.deepseek.com/chat/completions
mimi> set_api_key sk-ds-...
mimi> set_model deepseek-chat
```

## For Developers

Technical details live in the `docs/` folder:

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — system design, module map, task layout, memory budget, protocols, flash partitions
- **[docs/TODO.md](docs/TODO.md)** — feature gap tracker and roadmap

## License

MIT

## Acknowledgments

Inspired by [OpenClaw](https://github.com/openclaw/openclaw) and [Nanobot](https://github.com/HKUDS/nanobot). MimiClaw reimplements the core AI agent architecture for embedded hardware — no Linux, no server, just a $5 chip.

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
