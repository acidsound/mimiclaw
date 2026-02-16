# UI Capture Loop MVP

## Goal

Implement a non-realtime UI automation loop:

1. Request screen capture from desktop/mobile helper.
2. Let LLM analyze screenshot.
3. Execute one input action (click/touch/type).
4. Capture again and verify progress.
5. Repeat until done or step limit reached.

## Transport

- Device ↔ helper channel: WebSocket (`/`, port `18789`)
- LLM tool: `ui_capture`
- Image handoff: base64 image (`image/jpeg`) injected into agent context as vision block

## WebSocket Protocol (MVP)

### ESP32 -> Helper (capture request)

```json
{
  "type": "capture_request",
  "chat_id": "ws_12",
  "payload": {
    "request_id": "cap_ab12cd34",
    "format": "jpeg_base64",
    "max_width": 1080,
    "jpeg_quality": 65,
    "goal": "Open settings icon"
  }
}
```

### Helper -> ESP32 (capture result)

```json
{
  "type": "capture_result",
  "chat_id": "ws_12",
  "payload": {
    "request_id": "cap_ab12cd34",
    "status": "ok",
    "media_type": "image/jpeg",
    "width": 1080,
    "height": 2340,
    "rotation": 0,
    "image_base64": "<base64-bytes>"
  }
}
```

### Error result

```json
{
  "type": "capture_result",
  "chat_id": "ws_12",
  "payload": {
    "request_id": "cap_ab12cd34",
    "status": "error",
    "error": "permission_denied"
  }
}
```

## Tool Contract: `ui_capture`

Input JSON:

```json
{
  "chat_id": "ws_12",
  "goal": "Tap login button",
  "timeout_ms": 12000,
  "max_width": 1080,
  "jpeg_quality": 65
}
```

Behavior:

- Sends `capture_request` to helper.
- Waits for matching `capture_result` (same `chat_id` + `request_id`).
- Stores latest frame for agent loop.
- Returns text summary with request id and image metadata.

## Tool Contract: `ui_action`

Input JSON:

```json
{
  "chat_id": "ws_12",
  "action": "tap",
  "x_norm": 0.73,
  "y_norm": 0.18,
  "confidence": 0.86,
  "reason": "Settings gear icon at top right"
}
```

Behavior:

- Sends one action event to helper.
- Outbound event type: `input_action`
- Returns immediate dispatch result text.

Outbound payload example:

```json
{
  "type": "input_action",
  "chat_id": "ws_12",
  "payload": {
    "action": "tap",
    "x_norm": 0.73,
    "y_norm": 0.18,
    "confidence": 0.86
  }
}
```

## Tool Contract: `ios_sim_capture_to_telegram`

Input JSON:

```json
{
  "chat_id": "ios_sim_helper",
  "tg_chat_id": "123456789",
  "url": "https://acidsound.github.io/ddxx7/",
  "tap_mode": "center",
  "open_wait_ms": 2500,
  "wait_after_tap_ms": 1000,
  "timeout_ms": 30000
}
```

Behavior:

- Sends one `ios_sim_capture_request` event to a connected iOS Simulator helper.
- Helper opens URL in Mobile Safari, captures screenshot, uploads to Telegram `sendPhoto`.
- Helper returns `ios_sim_capture_result` with `telegram_file_id`.
- Tool returns a summary string including `file_id`.

## Agent Loop Integration

- After tool results are appended, if a fresh capture exists:
  - Append synthetic user message containing:
    - text summary
    - `type=image` base64 source block
- This allows the next LLM tool-thinking step to reason over screenshot.

## Input Action JSON (recommended next step)

Use normalized coordinates (resolution-agnostic):

```json
{
  "action": "tap",
  "x_norm": 0.73,
  "y_norm": 0.18,
  "confidence": 0.86,
  "reason": "Settings gear icon at top right"
}
```

Guidelines:

- `x_norm`, `y_norm` in `[0.0, 1.0]`
- execute one action per step
- recapture and verify state change before next action

## Safety Rules

- cap image payload (`MIMI_UI_CAPTURE_MAX_B64`)
- timeout and fail closed
- one pending capture request at a time
- require explicit `chat_id` ownership for helper routing
- keep helper role scoped to capture relay scenario while testing
