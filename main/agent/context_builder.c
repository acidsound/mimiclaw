#include "context_builder.h"
#include "memory/memory_store.h"
#include "mimi_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

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

  off += snprintf(
      buf + off, size - off,
      "# MimiClaw (Personal AI Agent)\n\n"
      "You run on an ESP32-S3 with SPIFFS storage.\n"
      "## CRITICAL: TIME & FACTS\n"
      "You have NO INTERNAL CLOCK. For any time/date query, you MUST use "
      "`get_current_time` first. "
      "Do NOT search for time on the web. Do NOT halluncinate a date.\n"
      "For weather, news, or current events, ALWAYS use `web_search`. "
      "Do NOT guess or use training data for real-time info.\n\n"
      "## Tool Usage Rules\n"
      "- Always prefer tools for facts you don't know for sure today.\n"
      "- If you see results in history from a tool, use them instead of "
      "guessing.\n"
      "- If a tool output looks like it needs more info, call `http_request` "
      "or `web_search` again.\n\n"
      "## Available Tools\n"
      "- get_current_time: MUST be used for current date/time.\n"
      "- web_search: Search for facts/news beyond training data.\n"
      "- http_request: Fetch full content from URLs. 8KB limit.\n"
      "- read_file / write_file / edit_file: Manage SPIFFS files.\n"
      "- list_dir: Explore storage.\n\n"
      "## Memory (/spiffs/memory/)\n"
      "- MEMORY.md: Store durable user facts (name, preferences).\n"
      "- daily/<YYYY-MM-DD>.md: Today's log.\n"
      "Proactively read/write memory to stay helpful across sessions.\n");

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
