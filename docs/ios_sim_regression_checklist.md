# iOS Simulator Capture Relay Regression Checklist

## Scope

Validate Telegram text request -> iOS Simulator Safari action -> screenshot ->
Telegram image delivery (`file_id` returned).

## Preflight

1. ESP32 flashed with latest firmware and connected to Wi-Fi.
2. iOS Simulator has at least one booted iPhone device.
3. Helper process is running:
   - `ESP32_IP=<ESP32_IP>` or `MIMI_WS_URL=ws://<ESP32_IP>:18789/`
   - `MIMI_HELPER_CHAT_ID=ios_sim_helper`
   - `TELEGRAM_BOT_TOKEN=<bot token>`
4. macOS Accessibility permission is granted for terminal app.
5. Telegram `chat_id` is authorized on device (`tg_auth_add`/`tg_admin_add`).

## Golden Test Sentence

`아이폰에서 브라우저를 띄워서 https://acidsound.github.io/ddxx7/ 사이트로 접근 Tap to start 버튼을 누른 후 1초후의 화면을 찍어서 전송해줘`

## Expected Device Logs

1. Telegram inbound accepted for target `chat_id`.
2. `agent`: `Processing message from telegram:<chat_id>`.
3. `tools`: `Executing tool: ios_sim_capture_to_telegram`.
4. `ui_bridge`: `ios_sim_capture_result accepted: req=... file_id=...`.
5. `agent`: `Fast-path iOS sim capture executed: ESP_OK`.
6. `mimi`: `Dispatching response to telegram:<chat_id>`.

## Expected Helper Logs

1. `connected` on WebSocket.
2. `request <id> start`.
3. `request <id> ok`.

## Pass Criteria

1. Telegram chat receives an actual image message.
2. Device response includes `file_id`.
3. `getFile` API for returned `file_id` responds with `ok=true`.

## Failure Triage

1. `no_booted_simulator`
   - Boot an iPhone simulator and retry.
2. `Received_network_error_or_non-101_status_code`
   - Check ESP32 IP/port and Wi-Fi reachability.
3. `missing_telegram_file_id` or `telegram_sendPhoto_failed:*`
   - Check bot token validity, bot permissions, and target chat access.
4. `osascript_failed:*` / tap not applied
   - Re-enable Accessibility permission and keep Simulator frontmost.

## Security Check

1. Do not store raw bot tokens in docs/commits.
2. Keep examples as placeholders (`<YOUR_TELEGRAM_BOT_TOKEN>`).
3. Re-run secret scan before release.
