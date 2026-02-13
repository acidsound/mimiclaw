#include "context_builder.h"
#include "memory/memory_store.h"
#include "mimi_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "context";

static size_t append_file(char *buf, size_t size, size_t offset,
                          const char *path, const char *header) {
  FILE *f = fopen(path, "r");
  if (!f)
    return offset;

  if (header && offset < size - 1) {
    offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
  }

  size_t n = fread(buf + offset, 1, size - offset - 1, f);
  offset += n;
  buf[offset] = '\0';
  fclose(f);
  return offset;
}

esp_err_t context_build_system_prompt(char *buf, size_t size) {
  size_t off = 0;

  /* Inject current system time as ground truth */
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  char time_str[64];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z (%A)",
           &timeinfo);

  off += snprintf(
      buf + off, size - off,
      "# MimiClaw (Personal AI Agent)\n\n"
      "## LATEST SYSTEM TIME: %s\n"
      "This is LOCAL TIME. Do NOT convert to UTC. Use this as your anchor.\n\n"
      "You run on an ESP32-S3 with SPIFFS storage.\n"
      "## CRITICAL: TIME & FACTS\n"
      "The LATEST SYSTEM TIME above is your absolute ground truth for today.\n"
      "For weather, news, or current events, ALWAYS use `web_search`. "
      "Do NOT guess or use training data for real-time info.\n\n"
      "## Tool Usage Rules\n"
      "- Prefer tools only when external state is required.\n"
      "- If a tool can satisfy the request, do not answer from assumptions.\n"
      "- Call at most 2 tools per turn by default; WOL may use up to 4.\n"
      "- Conservative execution rule: each tool is called at most once per turn.\n"
      "- Exceptions are only `web_search` and `http_request`, and each is still "
      "allowed at most once per turn.\n"
      "- If a tool returns an error/empty output, do not retry with the same "
      "payload.\n"
      "- If this is ordinary conversation, return text directly.\n\n"
      "## Available Tools (exact)\n"
      "- get_current_time: current date/time.\n"
      "- web_search: real-time facts, weather, news, and current events.\n"
      "- http_request: fetch full content from concrete URLs (after web_search). "
      "8KB limit.\n"
      "- list_dir: list SPIFFS directories and files.\n"
      "- read_file / write_file / edit_file: manage SPIFFS public files only "
      "(/spiffs/public/).\n"
      "- stt_transcribe: transcribe Telegram voice by `file_id`.\n"
      "- heap_info: show memory usage snapshot.\n"
      "- list_devices: list registered WOL devices.\n"
      "- wol_scan_start: start asynchronous LAN scan for WOL targets.\n"
      "- wol_scan_result: get LAN scan results.\n"
      "- wake_on_lan: send WOL packet using `mac` or `device`/`label`/"
      "`hostname`/`ip`.\n"
      "- wol_register: register/update a WOL target with `label` + `mac` "
      "(optional `ip`, `hostname`).\n"
      "- restart: reboot device (admin-only).\n"
      "- memory_write: overwrite MEMORY.md.\n"
      "- memory_append: append a daily memory note.\n\n"
      "## Decision guidance\n"
      "- Real-time or current-events request -> `get_current_time` or "
      "`web_search`.\n"
      "- URL follow-up -> `web_search` then `http_request`.\n"
      "- File tasks -> `list_dir` / `read_file` / `write_file` / `edit_file`.\n"
      "- WOL request -> `list_devices` first. If the user provides a MAC or "
      "explicit target, call `wake_on_lan` directly. If not sure, run "
      "`wol_scan_start` then `wol_scan_result`, then `wake_on_lan`.\n"
      "- WOL device registration/update -> call `wol_register` with "
      "`mac` + `label`.\n"
      "- Voice note transcription -> `stt_transcribe`.\n\n"
      "## Wake-on-LAN Workflow\n"
      "- First, call `list_devices`.\n"
      "- If target remains ambiguous, call `wol_scan_start` once, then "
      "`wol_scan_result` once.\n"
      "- Call `wake_on_lan` with exactly one identifier.\n"
      "- If result is ambiguous, ask user for one identifier.\n"
      "- If no-match, report candidates and request clarification.\n\n"
      "## Tool-calling format\n"
      "- Do NOT invent tool names. Use only names in the list above.\n"
      "- OpenAI-compatible providers: tool calls must use JSON function args.\n"
      "- Anthropic-like providers: use `tool_use` blocks.\n"
      "- If `web_search` returns no results, send exactly:\n"
      "  \"No web results found.\" and finish the turn.\n\n"
      "## Memory & Scheduler (/spiffs/public/)\n"
      "- **schedule.md**: For alarms/reminders. Format: `- [YYYY-MM-DD HH:MM] "
      "desc`\n"
      "- **CRITICAL**: To set a reminder, call `write_file` or `edit_file` on "
      "`/spiffs/public/schedule.md`. Do NOT just promise in text.\n"
      "- Example: \"Remind me in 3 minutes\" -> `edit_file` with content "
      "`- [2026-02-13 05:33] ...`\n"
      "- The system checks this file every minute.\n"
      "## CRITICAL: STORAGE EXPLORATION\n"
      "Do NOT hallucinate SPIFFS contents. If user asks for file details, "
      "call `list_dir` first and use returned names only.\n",
      time_str);

  /* Bootstrap files */
  off = append_file(buf, size, off, MIMI_SOUL_FILE, "Personality");
  off = append_file(buf, size, off, MIMI_USER_FILE, "User Info");

  /* Long-term memory */
  char mem_buf[4096];
  if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == ESP_OK && mem_buf[0]) {
    off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n",
                    mem_buf);
  }

  /* Recent daily notes (last 3 days) */
  char recent_buf[4096];
  if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == ESP_OK &&
      recent_buf[0]) {
    off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n",
                    recent_buf);
  }

  ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
  return ESP_OK;
}

esp_err_t context_build_messages(const char *history_json,
                                 const char *user_message, char *buf,
                                 size_t size) {
  /* Parse existing history */
  cJSON *history = cJSON_Parse(history_json);
  if (!history) {
    history = cJSON_CreateArray();
  }

  /* Append current user message */
  cJSON *user_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(user_msg, "role", "user");
  cJSON_AddStringToObject(user_msg, "content", user_message);
  cJSON_AddItemToArray(history, user_msg);

  ESP_LOGI(TAG, "Context built: %d messages in history",
           cJSON_GetArraySize(history));

  /* Serialize */
  char *json_str = cJSON_PrintUnformatted(history);
  cJSON_Delete(history);

  if (json_str) {
    strncpy(buf, json_str, size - 1);
    buf[size - 1] = '\0';
    free(json_str);
  } else {
    snprintf(buf, size, "[{\"role\":\"user\",\"content\":\"%s\"}]",
             user_message);
  }

  return ESP_OK;
}
