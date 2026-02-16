# iOS Simulator WS Helper (Telegram Screenshot Relay)

## Goal

Handle this single scenario from a Telegram-driven request:

1. Open Mobile Safari in iOS Simulator with the target URL.
2. Tap once (center) or skip tap.
3. Wait.
4. Capture screenshot.
5. Upload screenshot to Telegram `sendPhoto`.
6. Return `telegram_file_id` back to ESP32 through WebSocket.

## File

- Helper script: `tools/dev/ios_sim_ws_helper.mjs`

## Prerequisites

1. macOS with Xcode/iOS Simulator installed.
2. At least one iPhone Simulator booted (`booted` target must exist).
3. Telegram bot token (`TELEGRAM_BOT_TOKEN`).
4. ESP32 WebSocket server reachable from this Mac (default: `ws://mimiclaw.local:18789/`).
5. Accessibility permission for the terminal app running Node.js:
   - macOS Settings -> Privacy & Security -> Accessibility.

## Run

```bash
cd /Users/spectrum/Documents/works/_HW/esp32/mimiclaw
export MIMI_HELPER_CHAT_ID="ios_sim_helper"
export TELEGRAM_BOT_TOKEN="<YOUR_BOT_TOKEN>"
node tools/dev/ios_sim_ws_helper.mjs
```

Optional timing env vars:

- `MIMI_IOS_OPEN_WAIT_MS` (default: `2500`)
- `MIMI_IOS_WAIT_AFTER_TAP_MS` (default: `1000`)
- `MIMI_IOS_TIMEOUT_MS` (default: `30000`)
- `ESP32_IP` (optional; if set, helper uses `ws://$ESP32_IP:18789/`)
- `MIMI_WS_HOST` (optional; default: `mimiclaw.local`)
- `MIMI_WS_PORT` (optional; default: `18789`)
- `MIMI_WS_URL` (optional override; highest priority)

## WS Protocol

### Request (ESP32 -> Helper)

```json
{
  "type": "ios_sim_capture_request",
  "chat_id": "ios_sim_helper",
  "payload": {
    "request_id": "cap_ab12cd34",
    "tg_chat_id": "123456789",
    "url": "https://acidsound.github.io/ddxx7/",
    "tap_mode": "center",
    "open_wait_ms": 2500,
    "wait_after_tap_ms": 1000,
    "timeout_ms": 30000
  }
}
```

### Result (Helper -> ESP32)

Success:

```json
{
  "type": "ios_sim_capture_result",
  "chat_id": "ios_sim_helper",
  "payload": {
    "request_id": "cap_ab12cd34",
    "status": "ok",
    "telegram_file_id": "<file_id>"
  }
}
```

Failure:

```json
{
  "type": "ios_sim_capture_result",
  "chat_id": "ios_sim_helper",
  "payload": {
    "request_id": "cap_ab12cd34",
    "status": "error",
    "error": "no_booted_simulator"
  }
}
```

## Notes

- The helper registers itself using `chat_id=ios_sim_helper` at connection time.
- Temporary screenshot files are deleted after Telegram upload completes.
- Telegram upload response uses the largest photo size entry and returns its `file_id`.
- Telegram natural-language fast-path is enabled for iOS simulator capture intent
  in `main/agent/agent_loop.c` (URL + tap-to-start + capture/send cues).
