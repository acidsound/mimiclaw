# Telegram Media Handling Plan

## Current Status (2026-02-13)

- `telegram_bot.c` can now detect voice and photo attachments and pass through metadata to agent flow.
- `llm_proxy.c` supports text/image content blocks for vision-capable providers.
- `tool_stt.c` implements Groq Whisper STT tool integration; voice is transcribed via Telegram `file_id`.
- WOL workflow now supports explicit device registration by tool: `wol_register` in addition to discovery tools.
- `tool_files.c` now normalizes legacy `wol_devices` paths and allows `/spiffs/private/wol_devices.json` updates through file tools.
- `context_builder.c` was tightened to reduce hallucinated tool use, enforce one tool call pattern, and improve no-result guidance.

## Completed

- Tool set in runtime now includes: `web_search`, `get_current_time`, `read_file`, `write_file`, `edit_file`, `list_dir`, `heap_info`, `http_request`, `stt_transcribe`, `wake_on_lan`, `list_devices`, `wol_scan_start`, `wol_scan_result`, `restart`, `memory_write`, `memory_append`, `wol_register`.
- System prompt documents exact tool names and conservative execution policy (single-call-per-tool, avoid retries with same payload).
- WOL registry persistence is robust against legacy path use (`/spiffs/wol_devices.json` alias -> `/spiffs/private/wol_devices.json`).
- STT response handling now surfaces explicit failure cases and zero-result search responses.

## Design Principles
1. **Stream, don't store**: Telegram media should be downloaded and consumed in bounded chunks to stay within heap limits.
2. **Single media surface**: keep one streaming path for Telegram media download and adapt downstream consumers.
3. **Composable tools**: STT is available as an explicit tool (`stt_transcribe`) with strict JSON input.
4. **Fail-closed safety**: reject oversized/invalid payloads before costly processing and explain limits to users.

## Tool-calling Alignment Checklist (must hold before release)
- [x] Tool prompt mirrors runtime registry exactly (`get_current_time`, `stt_transcribe`, `wol_scan_*`, `memory_*` etc. included).
- [x] Explicit instruction for `tool_use` vs `tool_calls` is present.
- [x] Non-tool response path exists for ordinary chat (no unnecessary tool invocations).
- [x] WOL flow: `list_devices` before `wake_on_lan` and one-identifier-at-a-time.
- [x] Voice request path uses `stt_transcribe` with `file_id` and includes user-facing failure notice when STT unavailable.
- [ ] Vision/photo path still needs full migration to bounded in-memory/base64 streaming.
- [ ] STT upload path should avoid fallback full-buffer flow and use direct streaming.
- [ ] Add reproducible end-to-end stack/memory regression checks under `idf.py monitor`.
- [ ] Add a long-run stress test that alternates media + tool bursts to guard against Task WDT regressions.

## Work Breakdown
1. **Chunked Telegram Fetch (Owner: Telegram module)**
   - Remaining: introduce a bounded fetch callback API and remove full buffering from photo/voice pipelines.
2. **Vision Path Refactor (Owner: Agent + LLM)**
   - Remaining: move to streaming base64 path and keep only caption metadata in RAM.
3. **STT Upload Hardening (Owner: Tools + LLM)**
   - Remaining: enforce true streaming upload path and remove fallback buffering dependency.
4. **Config & Secrets (Owner: CLI)**
   - Add examples for Groq profile migration and STT-specific env usage.
5. **Verification (Owner: QA)**
   - Keep using live Telegram and monitor-based validation; add scripted stress cases for stack overflow and tool loops.

## Media Constraints & User Guidance
- **Photos**: enforce a `file_size` cap (e.g., 1 MB). If Telegram reports a larger file or the stream exceeds chunk limits, abort and reply with a hint to resend via the "Quick way" option so the app auto-resizes/compresses the image.
- **Voice notes**: check both `duration` and `file_size` (`voice.duration`, `voice.file_size`). Reject long or oversized notes with `VOICE_LIMIT_NOTICE` and request retry.
- **LLM image generation**: when the model returns an image URL, simply embed that link in the Telegram reply (Markdown `[보기](URL)` or plain URL). No raw image upload needed.
- **Runtime tuning**: keep `set_media_limits <photo_kb> <voice_kb> <voice_secs>` available for on-device policy changes without rebuild.

## Memory & Throughput Safeguards
- Maintain bounded buffers and avoid full in-memory passthrough where possible.
- Abort early when free PSRAM falls below the watchdog safety threshold.
- Apply per-request timeout and retry caps to prevent task stalls.

## User Messaging Patterns
> Actual responses should follow repository locale policy; examples here are English.
- Oversized media (photo): `"That photo is too large for this device. Please resend it using Telegram's quick option to compress/rescale."`
- Oversize/invalid voice: `"I can only process short voice notes (about 10 seconds)."`
- If a request demands high-bandwidth output (e.g., asking for generated images), reply with the URL plus brief instructions that they can open it in their Telegram client.

## Risk & Mitigation
- **RAM fragmentation**: dedicate PSRAM allocations (`MALLOC_CAP_SPIRAM`) and free them immediately after use to reduce fragmentation. Use static arenas where possible.
- **Network stalls**: add chunk-level timeouts and watchdog logging. If a download exceeds the limit, terminate cleanly and inform the user.
- **Provider variance**: wrap STT and future media providers behind interfaces so switching to another API requires only adapter changes.
