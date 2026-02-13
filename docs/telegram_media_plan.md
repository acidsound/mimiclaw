# Telegram Media Handling Plan

## Goal
Deliver Phase 2 of the "Telegram Media Handling" initiative: accept Telegram photo/voice updates, forward photos to the LLM as image blocks, and transcribe voice notes via Groq Whisper while keeping ESP32-S3 RAM usage under 32 KB per streaming buffer.

## Current State Recap
- Incoming updates already differentiate text/photo/voice (`telegram_bot.c`).
- Media downloads still allocate full buffers in PSRAM; no chunked streaming exists.
- Vision pathway encodes photos to base64 inside `agent_loop.c`, but the helper lives in `llm_proxy.c` and needs hardening for large inputs.
- Voice STT works end-to-end via Groq, yet it loads the full .oga before posting and is tightly coupled to the agent loop rather than an isolated tool interface.

## Design Principles
1. **Stream, don't store**: reuse the HTTP proxy layer to pull Telegram file content in ≤32 KB chunks and push directly into the consumer (base64 encoder or STT uploader).
2. **Single media surface**: introduce a `telegram_media_stream` API that hides getFile + download plumbing and exposes callbacks for chunk consumption.
3. **Composable tools**: treat STT as an optional tool exposed through the tool registry so other contexts (e.g., scheduled jobs) can trigger transcriptions.
4. **Back-pressure awareness**: chunked callbacks must be able to abort early if the downstream detects quota overruns.

## Work Breakdown
1. **Chunked Telegram Fetch (Owner: Telegram module)**
   - Implement `telegram_stream_file(file_id, on_chunk, ctx)` that internally resolves `file_id` → `file_path`, issues HTTPS GET, and feeds chunks (size parameterized, default 16 KB) to the callback. Abort on callback != `ESP_OK`.
   - Extend proxy path to support CONNECT + streaming reads without storing whole payloads.
2. **Vision Path Refactor (Owner: Agent + LLM)**
   - Move base64 encoding helpers to `utils/base64_stream.c` (new) that can consume chunk callbacks.
   - Update `agent_loop.c` to invoke the streaming encoder; keep only captions in RAM.
3. **STT Toolization (Owner: Tools + LLM)**
   - Create `tools/tool_stt.c` exposing `stt_transcribe` with JSON schema `{ "file_id": string }`.
   - Internally leverage the Telegram stream API and forward chunks to a new `llm_stt_stream_upload` that builds a multipart request incrementally (use chunked transfer or pre-sized buffers via temporary SPIFFS file if necessary).
   - Preserve current auto-STT path by calling the tool internally after enqueueing a synthetic tool request.
4. **Config & Secrets (Owner: CLI)**
   - Surface `set_stt_key` (already exists) in docs + add SECRET.env placeholder for `GROQ_API_KEY`.
5. **Testing & Verification (Owner: QA)**
   - Scripted tests that stream mock JPEG/OGA payloads (≤100 KB) through a host-side HTTP server to ensure chunk callbacks never exceed 32 KB and that total transcription latency stays under 10 s.
   - Manual Telegram validation: send photo → ask "What is this?"; send a voice note → verify transcription reply.

## Media Constraints & User Guidance
- **Photos**: enforce a `file_size` cap (e.g., 1 MB). If Telegram reports a larger file or the stream exceeds chunk limits, abort and reply with a hint to resend via the "Quick way" option so the app auto-resizes/compresses the image.
- **Voice notes**: check both `duration` and `file_size` (`voice.duration`, `voice.file_size`). Reject anything longer than ~10 seconds or >350 KB with a courteous notice (e.g., "짧은 음성으로 다시 보내 주세요"). Allow per-device overrides via `mimi_secrets.h` or CLI.
- **LLM image generation**: when the model returns an image URL, simply embed that link in the Telegram reply (Markdown `[보기](URL)` or plain URL). No raw image upload needed.
- **Runtime tuning**: expose a CLI helper (`set_media_limits <photo_kb> <voice_kb> <voice_secs>`) backed by NVS so operators can relax/tighten limits without rebuilding.

## Memory & Throughput Safeguards
- Single media worker task drains a queue to avoid concurrent 4 KB TLS buffers multiplying. Each chunk callback logs free PSRAM before processing and aborts if it falls below ~50 KB.
- Base64 encoder writes directly into a streaming builder (or staged SPIFFS temp file) so raw + encoded payloads are never in memory simultaneously.
- STT uploader uses chunked transfer: multipart header/footers stay in RAM, while audio bytes stream straight from Telegram into the TLS socket. If `esp_http_client` cannot honor chunking, fall back to a bounded circular buffer flushed via `esp_http_client_write`.
- Apply backoff + timeout per chunk to prevent stuck downloads, and cap retries to keep the agent loop responsive.

## User Messaging Patterns
> Actual implementation must return English strings per existing prompt policy; the lines below are illustrative only.
- Oversized media → respond with "Please resend the photo using Telegram's quick method so it stays under the device memory limit." (photos) or "I can only process short voice notes (≈10 seconds)." (voice).
- If a request demands high-bandwidth output (e.g., asking for generated images), reply with the URL plus brief instructions that they can open it in their Telegram client.

## Risk & Mitigation
- **RAM fragmentation**: dedicate PSRAM allocations (`MALLOC_CAP_SPIRAM`) and free them immediately after use to reduce fragmentation. Use static arenas where possible.
- **Network stalls**: add chunk-level timeouts and watchdog logging. If a download exceeds the limit, terminate cleanly and inform the user.
- **Provider variance**: wrap STT and future media providers behind interfaces so switching to another API requires only adapter changes.
