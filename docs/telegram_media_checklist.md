# Telegram Media Handling Checklist

## 1. Chunked Telegram Fetch
- [x] Implement `telegram_stream_file(file_id, on_chunk, ctx)` that resolves `file_id` to `file_path` and streams data in ≤16 KB chunks.
- [x] Integrate proxy-aware streaming (CONNECT + direct HTTPS) without buffering entire files.
- [x] Add early-abort handling when callbacks signal `ESP_FAIL` or memory low-watermarks.

## 2. Vision Path Refactor
- [x] Introduce `utils/base64_stream.c(h)` with chunk-based encoder APIs.
- [x] Update `agent_loop.c` to call the streaming encoder (caption only in RAM).
- [x] Ensure encoded payload is appended to the Anthropic content array without holding duplicate buffers.

## 3. STT Toolization & Streaming Upload
- [x] Create `tools/tool_stt.c` with schema `{ "file_id": string }` and register in `tool_registry`.
- [x] Add `llm_stt_stream_upload` (or equivalent) that accepts chunk callbacks and performs multipart chunked transfer to Groq.
- [x] Refactor `agent_loop` voice handling to enqueue/use the STT tool (internal auto-trigger) instead of inline HTTP posts.
- [x] Add a small-file fallback: retry transcribe through bounded buffered upload (`<= 128 KB`) when streaming path fails.

## 4. Config & Limits
- [x] Add SPIFFS/CLI knobs for photo/voice size & duration caps; place defaults in `mimi_secrets.h`.
- [x] Enforce caps using Telegram `file_size` and `voice.duration` before streaming begins.
- [x] Define English user-facing messages for oversized media + voice duration failures.

## 5. Testing & Verification
- [x] Extend `docs/test_scenario.md` with JPEG/OGA streaming cases and chunk size validation.
- [x] Provide host-side mock server or script to replay chunked media for regression tests.
- [x] Document manual Telegram steps (photo "Quick way", short voice note) for QA.
