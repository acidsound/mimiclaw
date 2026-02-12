# Work Plan

## 1) System & Security Foundation
- Directory restructuring:
  - Move non-sensitive memory/config docs under `/spiffs/public/`
  - Create `/spiffs/private/` for secrets/sessions/registries
- Security hardening:
  - Implement directory allowlist enforcement in `tool_files.c` (public-only)
  - Add log redaction helper (token/cookie/auth/query masking)
- Telegram auth:
  - Implement NVS-based allowlist + dispatch gate in `telegram_bot.c`
  - Add getUpdates caps: allowed_updates + limit
  - Persist last_processed_update_id (for stable offset)
- Add `heap_info` and `restart` (restart is admin-only + cooldown)

## 2) HTTP Request Tool
- Implement `tool_http.c`:
  - Strict 8KB streaming cap, `truncated` flag, abort/close-or-discard remainder
  - Internal `{{SECRET:KEY}}` resolution via `/spiffs/private/SECRET.env`
  - SSRF guard: scheme allowlist, redirect policy, IP classification blocklist
- Implement session manager:
  - Store Set-Cookie to `/spiffs/private/SESSIONS.json`
  - Enforce cookie caps and never expose cookies to LLM

## 3) WOL & Discovery Tool (Async)
- Implement background ping-scan task (bounded stack, bounded queues)
- Implement `wol_scan_start` + `wol_scan_result` with job_id and result caps
- Implement `wol_send` (admin-only)
- Registry management in `/spiffs/private/wol_devices.json`
