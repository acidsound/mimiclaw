Implementation Plan - Expanding Mimi's Capabilities (Memory-first + Remote-trigger Safe)

Scope
- Add 3 tool categories: HTTP Request (secret-safe + SSRF-guard + 8KB response cap),
  Wake-On-LAN (registered devices + capped discovery), and System Debugging (rate-limited).
- Runtime environment: ESP-class MCU with tight SRAM budget; prevent OOM by hard caps and streaming.

User Review Required
IMPORTANT
Remote Trigger Model
- Tool calls are triggered via Telegram getUpdates long-polling.
- Therefore, ALL tool execution must be gated by Telegram allowlist (chat_id and/or from.id), and
  high-risk tools (restart, wol_scan, arbitrary http_request) must be admin-only.

Credential Security
- Secrets MUST NOT enter LLM context.
- Secrets are resolved internally by the http_request tool via {{SECRET:KEY}} placeholders.
- File tool access control MUST prevent LLM from reading any secret-bearing storage (see "File Access Policy").

Response Memory Cap (Global Rule)
- Any tool response returned to LLM MUST be bounded.
- http_request: maximum response body returned to LLM is 8192 bytes (8KB) hard cap; set truncated=true if exceeded.
- Logs must never contain secrets/tokens/cookies (see "Log Redaction").

NOTE
HTTP Streaming Requirement
- HTTP response handling must use esp_http_client event callbacks (HTTP_EVENT_ON_DATA) to stream data.
- Never allocate response-sized buffers; copy only up to 8KB into a fixed buffer, then abort or discard remaining bytes.

WOL Discovery
- STA mode: no AP STA list.
- wol_scan is optional and must be capped (subnet size limit, rate limit, result count limit).
- Default discovery mode is "registered devices only"; scanning is disabled unless explicitly enabled.

Proposed Changes

1) Telegram Gate (Tool Authorization Layer)
[MODIFY] telegram_bot.c (message dispatch)
- Enforce allowlist:
  - Allowed chat_id list (required)
  - Allowed from.id list (optional)
- Add role separation:
  - "admin" can run http_request (arbitrary), wol_send, wol_scan, restart
  - "user" can run safe read-only tools only (e.g., heap_info)
- getUpdates tuning:
  - Use offset = last_processed_update_id + 1
  - Set limit (e.g., 10~20) to cap processing load
  - Set allowed_updates=["message"] to reduce payload types

2) System Tools
[NEW] tool_system.h
[NEW] tool_system.c
- heap_info: returns bounded, fixed-format info (no huge strings).
- restart: triggers esp_restart(), but admin-only + cooldown (e.g., 60s minimum between restarts).

3) HTTP Request Tool (SSRF-guard + Secret substitution + 8KB cap)
[NEW] tool_http.h
[NEW] tool_http.c
Tool: http_request
- Supports all HTTP methods.
- Handles headers and body JSON.
- Streaming receive:
  - Accumulate response into fixed 8KB buffer only
  - If response exceeds cap:
    - Set truncated=true
    - Abort the connection OR read-and-discard remainder without buffering
- Secret Substitution:
  - Replace {{SECRET:KEY}} placeholders internally (LLM never sees resolved values).
- Redirect handling (SSRF safety):
  - Default: disable automatic redirects.
  - If redirects are allowed for usability, validate every hop:
    - Normalize URL
    - DNS resolve A/AAAA
    - Block private/loopback/link-local/multicast/reserved ranges
    - Block scheme other than http/https
    - Reject redirects that change to unsafe IP ranges
- Timeouts:
  - Set explicit connect/read timeouts to avoid long stalls.
- Return format (minimal):
  - status_code
  - content_type (optional)
  - body_prefix (<= 8192 bytes)
  - truncated (bool)
  - error (short string)

Session Management (Cookies) - Optional
- Parameter: session_key (string)
- Cookie store:
  - Save Set-Cookie internally under (session_key + scheme + host + port) to prevent cookie mixing.
  - Enforce caps: max cookies per session, max total bytes per session.
- LLM exposure:
  - LLM only knows session_key; cookie content is never returned.
- Storage:
  - /spiffs/private/SESSIONS.json (LLM file tools must not access)

4) WOL & Discovery Tool
[NEW] tool_wol.h
[NEW] tool_wol.c
Tools:
- wol_send: sends magic packet to a registered MAC address (admin-only).
- wol_scan: optional discovery (admin-only; disabled by default).
- wol_status: ping a device to check if it's awake (admin-only or user, depending on risk posture).

Device Registry
- Store registered devices in /spiffs/private/wol_devices.json
- Hard caps:
  - Max devices (e.g., 32)
  - Fixed record size where possible (IP, MAC; hostname optional and length-capped)

Discovery Constraints (wol_scan)
- Subnet size cap (e.g., /24 only) unless explicitly overridden by admin config.
- Rate limit (pings per second).
- Result cap (max found devices).
- Memory-safe output:
  - Return only N results to LLM; store full results internally if needed.

Security Hardening

1) File Access Policy (Stronger than single-file deny)
[MODIFY] tool_files.c
- Replace single deny (/spiffs/SECRET.md) with directory allowlist:
  - LLM file tools may access only /spiffs/public/*
  - Deny all /spiffs/private/* always
- Ensure the policy applies to ALL file-related tools consistently (read/edit/list/delete/rename/copy).

2) Log Redaction (Mandatory)
- Never log:
  - Telegram bot token (present in Bot API URL path)
  - Authorization headers
  - Cookies / Set-Cookie
  - URL query tokens
- Provide a helper to mask secrets before any ESP_LOG* output.

3) Outbound Request Controls (SSRF Defense)
- Enforce scheme restrictions: http/https only.
- Block unsafe destinations by resolved IP classification.
- Handle DNS rebinding and redirect chains with re-validation on each hop.
- Restrict ports (optional): allow 80/443 by default.

Integration
[MODIFY] tool_registry.c
- Register all new tools:
  - heap_info, restart
  - http_request
  - wol_send, wol_scan, wol_status
- Ensure Telegram authorization layer is checked BEFORE tool dispatch.

Verification Plan

Automated Tests (CLI / unit-style)
- Telegram gate:
  - Unauthorized chat_id/from.id cannot execute tools.
- HTTP tool:
  - Response size tests:
    - 8KB exactly => truncated=false
    - 8KB+1 => truncated=true, no crash, bounded memory
    - Chunked large response => truncated=true, no heap growth
  - Redirect tests:
    - Redirect to private IP => blocked
    - Multi-hop redirect => every hop validated
  - Secret tests:
    - {{SECRET:KEY}} resolves internally, but never appears in returned data or logs.
  - Cookie tests:
    - Set-Cookie stored internally, not returned; session caps enforced.

Manual Verification
- Real Telegram chat:
  - Confirm allowed_updates/limit reduce update load.
  - Confirm admin vs user role behaviors.
- WOL:
  - wol_send wakes a real PC on same LAN.
  - wol_scan behaves under caps; no long scan / memory spikes.

Non-goals / Assumptions
- Physical access (serial/flash dump) is considered allowed; protection focuses on Wi-Fi/Telegram remote abuse.
- If later physical threat model changes, move secrets/cookies to encrypted storage and enable secure boot/flash encryption.

## Security & Memory Policy (Normative)

### Telegram 권한 관리 (저장/운영)
- The system **MUST** enforce tool execution authorization using a Telegram allowlist (chat_id and/or from.id) before any tool dispatch occurs. [page:2]
- The allowlist **SHOULD** be stored in NVS and managed via CLI (create/update/delete), to avoid firmware rebuilds for operational changes.
- The firmware **MUST** include at least one compile-time “break-glass admin” identifier to recover from misconfiguration (e.g., empty/erased NVS).
- The bot polling logic **MUST** update `offset` as `last_processed_update_id + 1` based on the `update_id` field to avoid re-processing updates. [page:2]
- The bot **SHOULD** set `allowed_updates=["message"]` and a bounded `limit` to reduce payload and processing load. [page:2]

### SSRF 보호: Proxy 정책
- Proxy support **MUST** be disabled by default. [web:22]
- If proxy support is enabled, the proxy endpoint **MUST** be selected only from a fixed allowlist (host + port), and user-provided proxy endpoints **MUST NOT** be accepted. [web:22]
- When proxy is enabled, destination filtering at the device **MUST** still be applied to the requested URL (scheme restrictions, redirect controls, and hostname normalization), even though the proxy may be able to reach private networks. [web:22]
- The system **MUST** explicitly document that, with proxy enabled, complete prevention of server-side access to private networks depends on the proxy server’s own egress ACLs, and the device cannot guarantee it alone. [web:22]
- HTTP redirect following **MUST** be disabled by default; if enabled, each redirect hop **MUST** be re-validated under the same SSRF rules. [web:22][web:85]

### HTTP 8KB 제한 및 Truncation
- The http_request tool **MUST** enforce a hard cap of 8192 bytes on the response body returned to the LLM. [page:4]
- If the response exceeds the cap, the tool **MUST** set `content_truncated: true` (or `truncated: true`) in the returned JSON and return only the first 8192 bytes as `body_prefix`.
- The tool **MUST NOT** allocate memory proportional to the response size; it **MUST** stream data via the HTTP client data callbacks and copy at most the capped number of bytes into a fixed-size buffer. [page:3][page:4]
- The tool **SHOULD** abort the connection (or read-and-discard the remainder) immediately after the cap is reached to reduce network time and avoid internal buffering. [page:3]

### 파일 접근 정책 (/spiffs/public/*)
- All file tools exposed to the LLM **MUST** enforce a directory allowlist policy (e.g., only `/spiffs/public/*` is accessible) and **MUST NOT** rely on single-file deny rules. [web:10]
- All sensitive data stores (secrets, sessions, device registries) **MUST** be placed under `/spiffs/private/*` and **MUST NOT** be accessible by any LLM file tool. [web:10]
- Moving existing files (e.g., `MEMORY.md`, `SOUL.md`) into `/spiffs/public/` **IS ACCEPTABLE** if and only if they are confirmed to contain no secrets/tokens/cookies/session material; otherwise they **MUST** remain in `/spiffs/private/` (or be replaced with a sanitized public copy). [web:10]

### WOL Scan 속도/타임아웃 처리
- The `wol_scan` operation **MUST** be admin-only and disabled by default. [web:9]
- For `/24` scans, the implementation **SHOULD** be asynchronous: `wol_scan_start` returns a `job_id`, and `wol_scan_status/wol_scan_result` retrieves progress and bounded results.
- The scan **MUST** enforce caps (subnet size cap, rate limit, max results returned to LLM) to prevent long blocking operations and memory spikes.
- If synchronous scan is retained, the system **MUST** restrict the scan range further than `/24` (e.g., configurable smaller range) to avoid Telegram/LLM timeouts.

### SECRET 저장 포맷 ({{SECRET:KEY}} 치환)
- Secrets **MUST** be stored in a non-LLM-accessible location (e.g., `/spiffs/private/SECRET.env`) and **MUST NOT** be readable by any LLM file tool. [web:10]
- The secret file format **MAY** be `KEY=VALUE`, but the parser rules **MUST** be explicitly defined (whitespace trimming, comments, escaping/quoting rules, and duplicate key handling) to avoid ambiguous resolution.
- The http_request tool **MUST** resolve `{{SECRET:KEY}}` placeholders internally and **MUST NOT** return resolved secret values in tool outputs or logs. [web:10]
