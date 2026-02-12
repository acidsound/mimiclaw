## Appendix A — `ssrf_is_safe()` Requirements (Normative)

This appendix defines the required behavior of `ssrf_is_safe()` and related HTTP redirect handling for arbitrary-URL outbound requests.

### A.1 Goals
- Prevent outbound requests from being used as an SSRF pivot into internal/unsafe network ranges. [web:22][web:123]
- Ensure consistent validation across direct URLs and redirect chains (hop-by-hop). [web:36][web:22]
- Keep memory usage bounded (validation must be lightweight and non-allocating where possible).

### A.2 Inputs / Outputs
- Input: user-provided URL string (may be hostile/malformed/encoded).
- Output: `safe=true/false` plus a short reason code (for telemetry, not for leaking sensitive details).

### A.3 Mandatory Validation Steps (in order)

1) URL parsing & normalization (MUST)
- The URL **MUST** be parsed using a strict URL parser (not substring checks).
- The implementation **MUST** normalize before any security decision:
  - Lowercase scheme/host,
  - Remove surrounding whitespace,
  - Reject invalid percent-encoding,
  - Strip IPv6 zone identifiers (e.g., `%eth0`) if present.
- The implementation **MUST NOT** use prefix matching (e.g., `startsWith`) as a security check. [web:123]

2) Scheme allowlist (MUST)
- Only `http` and `https` schemes **MUST** be allowed.
- All other schemes (including `file:`, `ftp:`, `gopher:`, `ws:`) **MUST** be rejected. [web:123]

3) Credential-in-URL handling (SHOULD)
- URLs containing userinfo (`http://user:pass@host/`) **SHOULD** be rejected to reduce parser ambiguity and logging risk.

4) Port policy (SHOULD)
- The implementation **SHOULD** restrict outbound ports to a small allowlist (default 80/443).
- If non-standard ports are allowed, they **MUST** still be validated and logged (redacted).

5) Hostname resolution (MUST)
- If the host is a domain name, the system **MUST** perform DNS resolution for all address families used (A and AAAA).
- If DNS returns multiple IPs, **ALL** returned IPs **MUST** be validated; if any IP is unsafe, the request **MUST** be rejected. [web:36]

6) IP classification blocklist (MUST)
- The request **MUST** be rejected if the destination is any of:
  - RFC1918 private IPv4 ranges,
  - loopback (127.0.0.0/8, ::1),
  - IPv4 link-local (169.254.0.0/16) and IPv6 link-local (fe80::/10),
  - multicast/broadcast/reserved ranges,
  - (Recommended) cloud metadata endpoints (commonly 169.254.169.254) treated as unsafe. [web:36][web:22]
- Note: OWASP explicitly warns that denylist-only defenses are bypassable; therefore denylist **MUST** be combined with scheme/redirect/DNS-consistency controls. [web:123]

7) Rare IP formats & encoded host bypasses (MUST)
- The validation **MUST** operate on canonicalized IPs, not raw host strings.
- The implementation **MUST** reject hosts that cannot be canonicalized reliably.
- Rationale: attackers may encode internal IPs using alternative formats (decimal, octal, hex, overflow forms) to bypass string-based filters. [web:106]

8) URL consistency / DNS rebinding consideration (SHOULD)
- The implementation **SHOULD** avoid “time of check vs time of use” inconsistencies:
  - Validate the resolved IP(s),
  - Then connect using the validated resolved IP (not by re-resolving the hostname later).
- Rationale: DNS rebinding can change a hostname’s resolution between validation and connection. [web:123][web:36]

### A.4 Redirect Handling Requirements (MUST)
- Automatic redirect following **MUST** be disabled by default. [web:123]
- If redirects are enabled for usability:
  - Each redirect hop **MUST** be treated as a new request:
    - Normalize URL,
    - Validate scheme,
    - Resolve host,
    - Classify IPs,
    - Enforce port policy,
    - Reject if unsafe. [web:36]
  - Redirect hop count **MUST** be capped (e.g., max 3–5 hops) to prevent loops and resource exhaustion.

### A.5 Error Handling & Observability (MUST)
- On rejection, the system **MUST** return a generic error to the LLM (e.g., `"URL blocked by SSRF policy"`).
- Logs **MUST** avoid sensitive data and **MUST** be passed through `mimi_log_redact()`:
  - Never log full URLs with tokens,
  - Never log Authorization/Cookies/Set-Cookie,
  - Never log resolved secret values. [web:122]

### A.6 Recommended Test Cases (for Verification Plan)
- Direct URL to private IP: `http://192.168.0.1/` => blocked.
- Localhost: `http://127.0.0.1/`, `http://[::1]/` => blocked.
- Link-local: `http://169.254.1.1/`, `http://[fe80::1]/` => blocked.
- Encoded/rare IP forms representing localhost/private ranges (decimal/octal/hex) => blocked. [web:106]
- Safe public host redirecting to private IP => blocked with hop-by-hop validation. [web:36]
- DNS returns multiple IPs (one public, one private) => blocked. [web:36]
- Redirect loop or excessive hops => blocked.
