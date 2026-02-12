## Final Review — 6 Must-Fix Notes

1) **SSRF Guard must cover IPv6 too**
- Expand `IP blocklist (private/loopback)` to explicitly include IPv6 (e.g., `::1`, `fe80::/10`, `fc00::/7`) in addition to RFC1918 IPv4 ranges. [web:98][web:22]

2) **Redirect policy must be explicit**
- Default MUST be: redirects disabled.
- If redirects are enabled, validate every hop and cap hop count (e.g., 3–5). [web:98][web:61]

3) **8KB cap: define exact behavior at limit**
- On reaching 8192 bytes: return `truncated=true`, and then either abort/close immediately OR read-and-discard remainder, but never buffer more for LLM output. [web:61]

4) **DNS consistency / rebinding: keep as SHOULD**
- Keep “connect to validated IP + keep SNI/Host as original domain” as a SHOULD (best-effort) requirement, because TLS/SNI/library constraints may affect feasibility.
- Document TOCTOU/DNS rebinding awareness explicitly. [web:98][web:42]

5) **Bootstrap file migration must be allowlist-based**
- Any automated move into `/spiffs/public/` MUST only move an explicit allowlist of known-safe files/patterns; unknown files must never be moved to public. [web:156]

6) **Log redaction must be tested and universal**
- Ensure Verification includes tests that confirm logs never contain: Telegram bot token in URL path, `Authorization`, `Cookie/Set-Cookie`, or resolved secret values, and that all risky logs pass through `mimi_log_redact()`. [web:156][web:61]
