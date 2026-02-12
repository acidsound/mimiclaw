## Appendix B — `mimi_log_redact()` Requirements (Normative, Final)

This appendix defines the required behavior of `mimi_log_redact()` and the logging policy to prevent leakage of secrets, tokens, and session material via logs.

### B.1 Goals
- Logs **MUST NOT** become a secondary secret store (credentials/session material must never be written). [web:156]
- Prefer **allowlist logging**: log only known-safe, minimal fields; avoid raw payload logging by default. [web:156]
- Ensure consistent redaction across subsystems (Telegram, HTTP, file tools, WOL).

### B.2 Absolute Prohibitions (MUST NOT)
The system **MUST NOT** log any of the following (directly or indirectly):
- Session identification values, access tokens, authentication passwords. [web:156]
- Encryption keys and other primary/master secrets. [web:156]
- HTTP `Authorization` header values, cookies, or `Set-Cookie` values (session IDs). [web:156]
- Telegram Bot API token (embedded in the Bot API URL path). [web:156]

### B.3 Mandatory Enforcement (MUST)
- Any log statement that may include user-controlled or network-controlled content (Telegram message text, URLs, headers, JSON bodies, error payloads) **MUST**:
  - Either avoid logging the content entirely, OR
  - Pass the content through `mimi_log_redact()` before emitting the log. [web:156]
- Redaction **MUST** occur *before* emission (no “log raw then sanitize later” designs).
- Redaction **MUST** use **full replacement** (`[REDACTED]`) for secrets/tokens; partial masking is not sufficient for tokens.

### B.4 Allowlist Logging (SHOULD)
- Logging **SHOULD** be structured (key/value fields) and allowlist-based.
- Recommended safe fields:
  - Component name, operation name (e.g., `http_request`)
  - HTTP method, scheme, host (without credentials), port, path (optional)
  - Status code, error code (no raw server payload)
  - Byte counts (received_total, returned_to_llm)
  - Flags (`truncated`, `redirect_count`, `ssrf_blocked`)
- Raw request/response bodies **SHOULD NOT** be logged in production.

### B.5 Required Redaction Coverage (MUST)

#### 1) Telegram Bot API URL (MUST)
- Any URL containing the Telegram Bot API token in its path **MUST** be redacted so the token is not present in logs.

#### 2) HTTP headers (MUST)
- If headers are serialized into a log string, the values for these headers **MUST** be replaced with `[REDACTED]`:
  - `Authorization`, `Proxy-Authorization`, `Cookie`, `Set-Cookie` [web:156]
- Header names may be logged, but sensitive header values must not.

#### 3) URL query strings (SHOULD)
- Full query strings **SHOULD NOT** be logged.
- If query logging is necessary, the system **SHOULD** log only:
  - Parameter count, total query length,
  - And/or allowlisted parameter keys with redacted values.

#### 4) JSON bodies (SHOULD)
- If JSON body logging is enabled for debugging, the following keys **SHOULD** be treated as sensitive and redacted:
  - `token`, `access_token`, `refresh_token`, `id_token`, `api_key`, `secret`, `password`, `authorization`, `cookie`
- Default behavior **SHOULD** be: do not log bodies; log only byte counts and parse success/failure.

#### 5) Secret placeholders (MUST)
- Resolved secret values from `{{SECRET:KEY}}` **MUST NOT** appear in logs.
- Logs **SHOULD** avoid revealing secret key names; placeholders may be normalized to `{{SECRET:*}}`.

### B.6 Bounded Output & Memory Safety (MUST)
- `mimi_log_redact()` **MUST** be memory-bounded:
  - It MUST NOT allocate unbounded memory.
  - It MUST cap output length (e.g., 512 bytes recommended).
  - If the input exceeds the cap, it MUST produce a truncated redacted output and include a boolean `redaction_truncated=true` for internal telemetry (not necessarily shown to LLM).

### B.7 Anti-Log-Injection (SHOULD)
- The system **SHOULD** sanitize or encode CR/LF and delimiter characters in logged fields to reduce log injection risks. [web:156]

### B.8 Verification Tests (MUST)
- Inject a log candidate containing:
  - Telegram Bot API URL with token -> ensure token is absent post-redaction.
  - `Authorization: Bearer ...` -> ensure value becomes `[REDACTED]`. [web:156]
  - `Set-Cookie: session=...` -> ensure cookie value becomes `[REDACTED]`. [web:156]
  - A long string (> cap) containing token-like patterns -> ensure bounded output + `redaction_truncated=true`.
- Verify no code path logs raw request/response bodies in production configuration.
