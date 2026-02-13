# MimiClaw vs Nanobot — Feature Gap Tracker

> Comparing against `nanobot/` reference implementation. Tracks features MimiClaw has not yet aligned with.
> Priority: P0 = Core missing, P1 = Important enhancement, P2 = Nice to have

---

## P0 — Core Agent Capabilities

### [x] ~~Tool Use Loop (multi-turn agent iteration)~~
- Implemented: `agent_loop.c` ReAct loop with `llm_chat_tools()`, max 10 iterations, non-streaming JSON parsing

### [ ] Memory Write via Tool Use (agent-driven memory persistence)
- **openclaw**: Agent uses standard `write`/`edit` tools to write `MEMORY.md` and `memory/YYYY-MM-DD.md`; system prompt instructs agent to persist important information; pre-compaction memory flush triggers a silent agent turn to save durable memories before context window limit
- **MimiClaw**: `memory_write_long_term` and `memory_append_today` exist but are only called from CLI; agent loop never writes memory
- **Scope**: Expose `memory_write` and `memory_append_today` as tool_use tools for Claude; add system prompt guidance on when to persist memory; optionally add pre-compaction flush (trigger memory save when session history nears `MIMI_SESSION_MAX_MSGS`)
- **Depends on**: Tool Use Loop

### [x] ~~Tool Registry + web_search Tool~~
- Implemented: `tools/tool_registry.c` — tool registration, JSON schema builder, dispatch by name
- Implemented: `tools/tool_web_search.c` — Brave Search API via HTTPS (direct + proxy support)

### [ ] More Built-in Tools
- **nanobot built-in tools** not yet ported: `read_file`, `write_file`, `edit_file`, `list_dir`, `message`
- **Recommendation**: Reasonable tool subset for ESP32: `read_file`, `write_file`, `list_dir` (SPIFFS), `message`, `memory_write`

### [ ] Subagent / Spawn Background Tasks
- **nanobot**: `subagent.py` — SubagentManager spawns independent agent instances with isolated tool sets and system prompts, announces results back to main agent via system channel
- **MimiClaw**: Not implemented
- **Recommendation**: ESP32 memory is limited; simplify to a single background FreeRTOS task for long-running work, inject result into inbound queue on completion
- [x] Design spec documented for Leader-Worker swarm orchestration: `docs/leader_worker_swarm_spec.md`
- [ ] Implementation remains pending (leader queue/state machine, worker protocol, executor integration)

---

## P1 — Important Features

### [x] ~~Telegram User Allowlist (tg_auth)~~
- Implemented: Chat ID allowlist stored in NVS, managed via Serial CLI (`tg_auth_add/list/remove`).
- "Break-glass" admin ID support.

### [ ] Telegram Markdown to HTML Conversion
- **nanobot**: `channels/telegram.py` L16-76 — `_markdown_to_telegram_html()` full converter: code blocks, inline code, bold, italic, links, strikethrough, lists
- **MimiClaw**: Uses `parse_mode: Markdown` directly; special characters can cause send failures (has fallback to plain text)
- **Recommendation**: Implement simplified Markdown-to-HTML converter, or switch to `parse_mode: HTML`

### [ ] Telegram /start Command
- **nanobot**: `telegram.py` L183-192 — handles `/start` command, replies with welcome message
- **MimiClaw**: Not handled; /start is sent to Claude as a regular message

### [ ] Telegram Media Handling (photos/voice/files)
- **nanobot**: `telegram.py` L194-289 — handles photo, voice, audio, document; downloads files; transcribes voice
- **MimiClaw**: Only processes `message.text`, ignores all media messages
- **Recommendation**: Images can be base64-encoded for Claude Vision; voice requires Whisper API (extra HTTPS request)

### [ ] Image Generation Relay (Vercel / Hugging Face)
- **Problem**: ESP32-S3 RAM budget is too small for large `b64_json` image responses (JSON parse + base64 decode buffers).
- **Approach**: Use a lightweight relay server (Vercel or Hugging Face) to:
  1. call image model API,
  2. send image to Telegram (`sendPhoto`),
  3. return only `file_id`/message metadata to device.
- **Data policy**: Delete temporary image bytes on relay immediately after Telegram upload succeeds and `file_id` is captured.
- **MimiClaw side**: Keep protocol minimal (`prompt` request → `file_id` result), no raw image payload handling on-device.
- **Spec**: `docs/image-relay-spec.md`

### [ ] Skills System (pluggable capabilities)
- **nanobot**: `agent/skills.py` — loads skills from SKILL.md files, supports always-loaded and on-demand, frontmatter metadata, requirements checking
- **MimiClaw**: Not implemented
- **Recommendation**: Simplified version: store SKILL.md files on SPIFFS, load into system prompt via context_builder

### [ ] Full Bootstrap File Alignment
- **nanobot**: Loads `AGENTS.md`, `SOUL.md`, `USER.md`, `TOOLS.md`, `IDENTITY.md` (5 files)
- **MimiClaw**: Only loads `SOUL.md` and `USER.md`
- **Recommendation**: Add AGENTS.md (behavior guidelines) and TOOLS.md (tool documentation)

### [ ] Longer Memory Lookback
- **nanobot**: `memory.py` L56-80 — `get_recent_memories(days=7)` defaults to 7 days
- **MimiClaw**: `context_builder.c` only reads last 3 days
- **Recommendation**: Make configurable, but mind token budget

### [x] ~~System Prompt Tool Guidance~~
- Implemented: `context_builder.c` includes tool usage guidance in system prompt

### [ ] Message Metadata (media, reply_to, metadata)
- **nanobot**: `bus/events.py` — InboundMessage has media, metadata fields; OutboundMessage has reply_to
- **MimiClaw**: `mimi_msg_t` only has channel + chat_id + content
- **Recommendation**: Extend msg struct, add media_path and metadata fields

### [ ] Outbound Subscription Pattern
- **nanobot**: `bus/queue.py` L41-49 — supports `subscribe_outbound(channel, callback)` subscription model
- **MimiClaw**: Hardcoded if-else dispatch
- **Recommendation**: Current approach is simple and reliable; not worth changing with few channels

---

## P2 — Advanced Features


### [ ] Heartbeat Service
- **nanobot**: `heartbeat/service.py` — reads HEARTBEAT.md every 30 minutes, triggers agent if tasks are found
- **MimiClaw**: Not implemented
- **Recommendation**: Simple FreeRTOS timer that periodically checks HEARTBEAT.md

### [x] ~~Multi-LLM Provider Support~~
- Implemented: Support for Anthropic, Kimi (Moonshot), and any OpenAI-compatible provider (OpenRouter, DeepSeek, etc.).
- Switchable via CLI: `set_provider`, `set_base_url`.

### [ ] Voice Transcription
- **nanobot**: `providers/transcription.py` — Groq Whisper API
- **MimiClaw**: Not implemented
- **Recommendation**: Requires extra HTTPS request to Whisper API: download Telegram voice -> forward -> get text

### [x] ~~Build-time Config File + Runtime NVS Override~~
- Implemented: `mimi_secrets.h` as build-time defaults, NVS as runtime override via CLI
- Two-layer config: build-time secrets → NVS fallback, CLI commands to set/show/reset

### [ ] WebSocket Gateway Protocol Enhancement
- **nanobot**: Gateway port 18790 + richer protocol
- **MimiClaw**: Basic JSON protocol, lacks streaming token push
- **Recommendation**: Add `{"type":"token","content":"..."}` streaming push

### [ ] Multi-Channel Manager
- **nanobot**: `channels/manager.py` — unified lifecycle management for multiple channels
- **MimiClaw**: Hardcoded in app_main()
- **Recommendation**: Not worth abstracting with few channels

### [ ] WhatsApp / Feishu Channels
- **nanobot**: `channels/whatsapp.py`, `channels/feishu.py`
- **MimiClaw**: Only Telegram + WebSocket
- **Recommendation**: Low priority, Telegram is sufficient

### [x] ~~Telegram Proxy Support (HTTP CONNECT)~~
- Implemented: HTTP CONNECT tunnel via `proxy/http_proxy.c`, configurable via `mimi_secrets.h` (`MIMI_SECRET_PROXY_HOST`/`MIMI_SECRET_PROXY_PORT`)

### [ ] Session Metadata Persistence
- **nanobot**: `session/manager.py` L136-153 — session file includes metadata line (created_at, updated_at)
- **MimiClaw**: JSONL only stores role/content/ts, no metadata header
- **Recommendation**: Low priority

---

## Completed Alignment

- [x] Telegram Bot long polling (getUpdates)
- [x] Message Bus (inbound/outbound queues)
- [x] Agent Loop with ReAct tool use (multi-turn, max 10 iterations)
- [x] Claude API (Anthropic Messages API, non-streaming, tool_use protocol)
- [x] Tool Registry + web_search tool (Brave Search API)
- [x] Context Builder (system prompt + bootstrap files + memory + tool guidance)
- [x] Memory Store (MEMORY.md + daily notes)
- [x] Session Manager (JSONL per chat_id, ring buffer history)
- [x] WebSocket Gateway (port 18789, JSON protocol)
- [x] Serial CLI (esp_console, debug/maintenance commands)
- [x] HTTP CONNECT Proxy (Telegram + Claude API + Brave Search via proxy tunnel)
- [x] OTA Update
- [x] WiFi Manager (build-time credentials, exponential backoff)
- [x] SPIFFS storage with directory isolation (`/public` vs `/private`)
- [x] Build-time config (`mimi_secrets.h`) + runtime NVS override via CLI
- [x] Log Redaction (masking secrets/tokens in logs)
- [x] Telegram User Authorization (NVS-based allowlist + CLI commands)
- [x] Multi-LLM Provider Support (Anthropic, Kimi, OpenAI-compatible)
- [x] Secure `http_request` Tool (SSRF protection, secret substitution, cookies)
- [x] Wake-On-LAN & Background Device Discovery
- [x] LLM-Powered Scheduler (NEW!)
    - Periodically checks `/spiffs/public/schedule.md` and triggers agent turns.
- [x] Dynamic Time Awareness
    - Injects current localized time (KST) into system prompt for relative time calculations.

---

## Suggested Implementation Order

```
1. [done] Tool Use Loop + Tool Registry + web_search
2. Memory Write via Tool Use         <- makes the agent actually remember
3. Built-in Tools (read_file, write_file, message)
4. Telegram Allowlist (allow_from)   <- security essential
5. Bootstrap File Completion (AGENTS.md, TOOLS.md)
6. Subagent (simplified)
7. Telegram Markdown -> HTML
8. Media Handling
9. Cron / Heartbeat
10. Other enhancements
```
