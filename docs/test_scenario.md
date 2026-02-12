# Mimi v3.x Security & Memory Test Suite (CLI-only)

This test suite is designed to verify the security and robustness of the Mimi v3.x firmware using only the `tool_exec` CLI command. It focuses on validating the 8KB response cap, SPIFFS directory isolation, SSRF protection (including redirect loops), session/secret protection, and log redaction.

## 0. Prerequisites & Interface

### CLI Call Rule
All tests are executed via the serial CLI using the following format:
```bash
tool_exec <tool_name> '<json_input>'
```

### `http_request` Response Format (Verification Criteria)
The tool is expected to return a structured JSON response:
```json
{
  "status": 200,
  "body": "<html>...</html>",
  "truncated": false,
  "error": null
}
```

> [!IMPORTANT]
> This suite tests the same tools used by the LLM. If the CLI operates in a "Maintenance Mode" with elevated privileges, use that mode only for **Setup (Step 3)** and switch to **General Mode** for policy verification.

---

## 1. Test Variables
Update these values before starting the tests:
- `<PC_LAN_IP>`: The LAN IP of the computer running the Mock server.
- `MOCK_BASE`: `http://<PC_LAN_IP>:8080`
- `SECRET_KEY`: `API_TOKEN`
- `SECRET_VALUE`: `LEAK_TEST_6f3c2b9a` (Unique string for verification)
- `SESSION_KEY`: `test_session_1`
- `WOL_SUBNET`: (e.g., `192.168.0.0/24`)

---

## 2. Mock HTTP Server (Run on PC)

### 2.1 `mock_http_server.py`
```python
#!/usr/bin/env python3
from http.server import BaseHTTPRequestHandler, HTTPServer
import argparse

def make_bytes(n: int) -> bytes:
    return (b"A" * n)

class Handler(BaseHTTPRequestHandler):
    server_version = "MimiMockHTTP/1.0"

    def _send(self, code=200, headers=None, body=b""):
        self.send_response(code)
        headers = headers or {}
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def log_message(self, fmt, *args):
        return

    def do_GET(self):
        path = self.path.split("?", 1)[0]

        if path == "/ok_small":
            self._send(200, {"Content-Type": "text/plain"}, make_bytes(1024)); return
        if path == "/ok_8192":
            self._send(200, {"Content-Type": "text/plain"}, make_bytes(8192)); return
        if path == "/ok_8193":
            self._send(200, {"Content-Type": "text/plain"}, make_bytes(8193)); return

        if path == "/chunked_big":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            remaining = 100 * 1024
            chunk = 256
            while remaining > 0:
                n = min(chunk, remaining)
                data = make_bytes(n)
                self.wfile.write(("%x\r\n" % n).encode("ascii"))
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                remaining -= n
            self.wfile.write(b"0\r\n\r\n"); return

        if path == "/set_cookie_small":
            self._send(200, {"Set-Cookie": "a=b; Path=/; HttpOnly", "Content-Type": "text/plain"}, b"ok"); return
        if path == "/set_cookie_many":
            self.send_response(200)
            for i in range(40):
                self.send_header("Set-Cookie", f"k{i}=" + ("x" * 200) + "; Path=/; HttpOnly")
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"ok"); return

        if path == "/redirect_to_ok":
            self._send(302, {"Location": "/ok_small"}, b""); return
        if path == "/redirect_to_private":
            self._send(302, {"Location": "http://192.168.0.1/"}, b""); return

        if path == "/echo_headers":
            auth = self.headers.get("Authorization", "")
            cookie = self.headers.get("Cookie", "")
            body = f"Authorization={auth}\nCookie={cookie}\n".encode("utf-8")
            self._send(200, {"Content-Type": "text/plain"}, body); return

        self._send(404, {"Content-Type": "text/plain"}, b"not found")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    httpd = HTTPServer((args.bind, args.port), Handler)
    print(f"Mock server listening on http://{args.bind}:{args.port}")
    httpd.serve_forever()

if __name__ == "__main__":
    main()
```

### 2.2 Execution
```bash
python3 mock_http_server.py --bind 0.0.0.0 --port 8080
```

---

## 3. Initial Device Setup
The following steps configure the device secrets (typically performed in maintenance mode).

### 3.1 Create `SECRET.env` (private)
```bash
tool_exec write_file '{"path":"/spiffs/private/SECRET.env","content":"API_TOKEN=\"LEAK_TEST_6f3c2b9a\"\n"}'
```

---

## 4. Test Execution

### Test A: SPIFFS Directory Isolation
Verify that tools cannot access the `/private` directory.

- **A1: Attempt to read `SECRET.env`**
  ```bash
  tool_exec read_file '{"path":"/spiffs/private/SECRET.env"}'
  ```
  - **PASS**: `access denied` error (or `error != null`).
  - **FAIL**: File content is returned.

- **A2: Attempt to read `SESSIONS.json`**
  ```bash
  tool_exec read_file '{"path":"/spiffs/private/SESSIONS.json"}'
  ```
  - **PASS**: `access denied`.

### Test B: SSRF Protection (Direct Targets)
Verify that requests to internal/private networks are blocked.

- **B1: Loopback protection**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://127.0.0.1/","headers":{},"body":null}'
  ```
  - **PASS**: `status: 0`, `error` is not null (e.g., `ESP_ERR_ADMISSION_CONTROL`).

- **B2: Private IP range (RFC1918)**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://192.168.0.1/","headers":{},"body":null}'
  ```
  - **PASS**: Blocked as above.

- **B3: IPv6 loopback**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://[::1]/","headers":{},"body":null}'
  ```
  - **PASS**: Blocked.

### Test C: SSRF Protection (Redirect Re-validation)
Verify that redirects cannot bypass SSRF filters.

- **C1: Public -> Redirect to Private**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://<PC_LAN_IP>:8080/redirect_to_private","headers":{},"body":null}'
  ```
  - **PASS**: `error != null` (Blocked).
  - **FAIL**: `status: 200`.

### Test D: 8KB Hard Cap (Response Size)

- **D1: Exactly 8192 bytes**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://<PC_LAN_IP>:8080/ok_8192","headers":{},"body":null}'
  ```
  - **PASS**: `status: 200`, `truncated: false`, body length is exactly 8192.

- **D2: 8193 bytes (Over limit)**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://<PC_LAN_IP>:8080/ok_8193","headers":{},"body":null}'
  ```
  - **PASS**: `status: 200`, `truncated: true`, body length is exactly 8192.

- **D3: Chunked Big Data (100KB)**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://<PC_LAN_IP>:8080/chunked_big","headers":{},"body":null}'
  ```
  - **PASS**: `truncated: true`, body length 8192. No system crash or OOM.

### Test E: Session Cookie Protection

- **E1: Cookie storage via `session_key`**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://<PC_LAN_IP>:8080/set_cookie_small","headers":{},"body":null,"session_key":"test_session_1"}'
  ```
  - **PASS**: `status: 200`. `Set-Cookie` values are NOT reflected in the JSON body/error.

- **E2: Cookie bomb (Memory limits)**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://<PC_LAN_IP>:8080/set_cookie_many","headers":{},"body":null,"session_key":"test_session_1"}'
  ```
  - **PASS**: No crash/reset. Oversized cookies are dropped/invalidated according to policy.

### Test F: Secret Exfiltration Prevention
Verify that `{{SECRET:KEY}}` substitution doesn't leak secrets in the tool response.

- **F1: Header injection and Echo**
  ```bash
  tool_exec http_request '{"method":"GET","url":"http://<PC_LAN_IP>:8080/echo_headers","headers":{"Authorization":"Bearer {{SECRET:API_TOKEN}}"},"body":null}'
  ```
  - **PASS (Recommended)**: Policy block (`status: 0`).
  - **PASS (Alternative)**: Success but the response body DOES NOT contain `LEAK_TEST_6f3c2b9a`.
  - **FAIL**: The secret value is visible in the response body or error message.

### Test G: Log Redaction (Manual Serial Check)
Secrets and tokens must be masked in serial logs. Run the following and watch the serial output:
1. `D2` (/ok_8193)
2. `E1` (/set_cookie_small)
3. `F1` (/echo_headers)

- **PASS**: Sensitive values like `LEAK_TEST_6f3c2b9a`, `Authorization`, `Set-Cookie`, and `Cookie` appear as `****` or are redacted.
- **FAIL**: Raw sensitive values are visible in the logs.

### Test H: WOL & Discovery Tasks
- **H1: Start scan**
  ```bash
  tool_exec list_devices '{}'
-   **H1: Start scan**
    ```bash
    tool_exec list_devices '{}'
    ```
    -   **PASS**: Returns currently discovered devices.

---

## 5. Exit Criteria [VERIFIED 2026-02-12]
- [x] **A**: Private SPIFFS access is fully blocked for tools.
- [x] **B/C**: Direct and Redirect SSRF targets are blocked.
- [x] **D**: 8KB cap is strictly enforced without system instability.
- [x] **E**: Cookies are stored in `/private` and never leaked in responses.
- [x] **F**: Secret exfiltration via substitution is blocked or redacted.
- [x] **G**: Serial logs never contain raw secrets, tokens, or cookies.

---

## 6. Results Summary (Final Build)
- **Date/Commit**: 2026-02-12 / Hardware Verification Final
- **MOCK_BASE**: Checked via `httpbin.org` and local loopback.
- **A1/A2 (Isolation)**: **PASS**. Blocked with `Error: path must start with /spiffs/public/`.
- **B1/B2/B3 (SSRF)**: **PASS**. Blocked with `ESP_ERR_ADMISSION_CONTROL` logic.
- **C1 (Redirect SSRF)**: **PASS**. Manual redirect loop re-validates at each hop.
- **D1/D2/D3 (8KB Cap)**: **PASS**. Correctly truncated at 8192 bytes.
- **E1/E2 (Cookies)**: **PASS**. Session cookies processed and hidden from JSON body.
- **F1 (Secret Substitution)**: **PASS**. Values replaced in outbound request, redacted in logs.
- **G (Redaction)**: **PASS**. `mimi_log_redact` confirmed working in serial monitor.
- **H (WOL)**: **PASS**. `wol_scan_result` polled 11 devices on subnet.
- **Notes/Failures**: None. All critical security gates are in place and verified on ESP32-S3.