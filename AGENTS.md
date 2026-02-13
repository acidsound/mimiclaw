# Repository Guidelines

## Project Structure & Module Organization
Firmware lives in `main/`, with focused modules such as `agent/` (reasoning loop), `telegram/` (bot bridge), `tools/` (LLM actions), `memory/` (SPIFFS-backed store), and `wifi/` (network bring-up). Visual assets sit in `assets/`, longer-form references in `docs/`, and upstream ESP-IDF components are vendored inside `managed_components/` with their lockfile in `dependencies.lock`. Default on-device files originate in `spiffs_data/public` and `spiffs_data/private`, mirroring the runtime `/spiffs` layout—edit those seeds instead of touching generated flash images. Build outputs land in `build/` and should be ignored by Git.

## Build, Flash & Monitor
1. `idf.py set-target esp32s3` once per clone to ensure the correct toolchain profile.
2. `cp main/mimi_secrets.h.example main/mimi_secrets.h` and fill Wi-Fi, Telegram, LLM, and optional Brave Search keys.
3. `idf.py build` for iterative development; it also regenerates SPIFFS when relevant files change.
4. `idf.py -p /dev/cu.usbmodemXXXX flash monitor` flashes, reboots, and opens a serial console (replace the port; exit with `Ctrl+]`).
5. `idf.py fullclean` when switching boards, mucking with `sdkconfig*`, or updating partitions to avoid stale artifacts.

## Coding Style & Naming Conventions
Stick to ESP-IDF C17 style: two-space indents, K&R braces, and 100-column soft limit. Gate logging through `ESP_LOGx` with meaningful `TAG`s and redact secrets using helpers in `utils/`. File-private helpers should be `static`, while shared APIs keep a `mimi_` prefix and live in the corresponding header. Configuration constants belong in `mimi_config.h`; deploy-specific secrets or URLs belong in `mimi_secrets.h` and must never be committed.

## Testing & Verification
There is no hosted unit-test suite, so verification is hardware-first. After each change, run `idf.py build`, flash a dev board, and watch `idf.py monitor` for clean boot plus Wi-Fi attach. Validate behavioral changes by messaging the Telegram bot, inspecting scheduler updates in `/spiffs/public/schedule.md`, and exercising CLI utilities (`mimi> ls public`, `mimi> memory_read`). Confirm migrations by reading the affected SPIFFS files through the CLI instead of the host filesystem.

## Commit & Pull Request Guidelines
Use the Conventional Commit verbs already in history (`feat:`, `fix:`, `docs:`) and keep each change logically scoped. PRs should summarize the feature, modules touched, and any new configuration knobs, plus link issues or design notes. Include monitor snippets or Telegram transcripts whenever networking, scheduler, or memory flows change, and call out new dependencies or secrets so reviewers can reproduce safely.
