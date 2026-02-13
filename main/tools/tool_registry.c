#include "tool_registry.h"
#include "tools/tool_files.h"
#include "tools/tool_get_time.h"
#include "tools/tool_memory.h"
#include "tools/tool_system.h"
#include "tools/tool_stt.h"
#include "tools/tool_web_search.h"

#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

#include "tools/tool_discovery.h"
#include "tools/tool_http.h"

extern esp_err_t tool_wol_scan_start_execute(const char *, char *, size_t);
extern esp_err_t tool_wol_scan_result_execute(const char *, char *, size_t);

static const char *TAG = "tools";

#define MAX_TOOLS 16

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL; /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool) {
  if (s_tool_count >= MAX_TOOLS) {
    ESP_LOGE(TAG, "Tool registry full");
    return;
  }
  s_tools[s_tool_count++] = *tool;
  ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void) {
  cJSON *arr = cJSON_CreateArray();

  for (int i = 0; i < s_tool_count; i++) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", s_tools[i].name);
    cJSON_AddStringToObject(tool, "description", s_tools[i].description);

    cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
    if (schema) {
      cJSON_AddItemToObject(tool, "input_schema", schema);
    }

    cJSON_AddItemToArray(arr, tool);
  }

  free(s_tools_json);
  s_tools_json = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);

  ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void) {
  s_tool_count = 0;

  /* Register web_search */
  tool_web_search_init();

  mimi_tool_t ws = {
      .name = "web_search",
      .description = "Search the web for current information. Use this when "
                     "you need up-to-date facts, news, weather, or anything "
                     "beyond your training data.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{\"query\":{\"type\":\"string\","
                           "\"description\":\"The search query\"}},"
                           "\"required\":[\"query\"]}",
      .execute = tool_web_search_execute,
  };
  register_tool(&ws);

  /* Register get_current_time */
  mimi_tool_t gt = {
      .name = "get_current_time",
      .description =
          "Get the current date and time. Also sets the system clock. Call "
          "this when you need to know what time or date it is.",
      .input_schema_json = "{\"type\":\"object\","
                           "\"properties\":{\"timezone\":{\"type\":\"string\","
                           "\"description\":\"The timezone to fetch, e.g., "
                           "'KST-9'. Optional.\"}},"
                           "\"required\":[]}",
      .execute = tool_get_time_execute,
  };
  register_tool(&gt);

  /* Register read_file */
  mimi_tool_t rf = {
      .name = "read_file",
      .description =
          "Read a file from SPIFFS storage. Path must start with /spiffs/.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
          "\"Absolute path starting with /spiffs/\"}},"
          "\"required\":[\"path\"]}",
      .execute = tool_read_file_execute,
  };
  register_tool(&rf);

  /* Register write_file */
  mimi_tool_t wf = {
      .name = "write_file",
      .description = "Write or overwrite a file on SPIFFS storage. Path must "
                     "start with /spiffs/.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
          "\"Absolute path starting with /spiffs/\"},"
          "\"content\":{\"type\":\"string\",\"description\":\"File content to "
          "write\"}},"
          "\"required\":[\"path\",\"content\"]}",
      .execute = tool_write_file_execute,
  };
  register_tool(&wf);

  /* Register edit_file */
  mimi_tool_t ef = {
      .name = "edit_file",
      .description = "Find and replace text in a file on SPIFFS. Replaces "
                     "first occurrence of old_string with new_string.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
          "\"Absolute path starting with /spiffs/\"},"
          "\"old_string\":{\"type\":\"string\",\"description\":\"Text to "
          "find\"},"
          "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement "
          "text\"}},"
          "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
      .execute = tool_edit_file_execute,
  };
  register_tool(&ef);

  /* Register list_dir */
  mimi_tool_t ld = {
      .name = "list_dir",
      .description =
          "List files on SPIFFS storage, optionally filtered by path prefix.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":"
          "\"Optional path prefix filter, e.g. /spiffs/memory/\"}},"
          "\"required\":[]}",
      .execute = tool_list_dir_execute,
  };
  register_tool(&ld);

  /* Register heap_info */
  mimi_tool_t hi = {
      .name = "heap_info",
      .description = "Show system memory usage (internal, psram, total). Use "
                     "this to check system health.",
      .input_schema_json =
          "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      .execute = tool_heap_info_execute,
  };
  register_tool(&hi);

  /* Register http_request */
  mimi_tool_t hr = {
      .name = "http_request",
      .description = "Read content from a URL. Use this to fetch full page "
                     "content, API data, or analysis from links found via "
                     "web_search. Supports GET/POST, secrets, and session "
                     "management. 8KB limit.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{"
          "\"url\":{\"type\":\"string\",\"description\":\"Target URL\"},"
          "\"method\":{\"type\":\"string\",\"enum\":[\"GET\",\"POST\"],"
          "\"description\":\"HTTP method\"},"
          "\"body\":{\"type\":\"string\",\"description\":\"Optional request "
          "body\"},"
          "\"session_key\":{\"type\":\"string\",\"description\":\"Optional key "
          "for persistent session/cookies\"}},"
          "\"required\":[\"url\"]}",
      .execute = tool_http_request_execute,
  };
  register_tool(&hr);

  mimi_tool_t stt = {
      .name = "stt_transcribe",
      .description = "Transcribe a Telegram voice note by providing its "
                     "file_id. Returns the recognized text.",
      .input_schema_json =
          "{\"type\":\"object\",\"properties\":{"
          "\"file_id\":{\"type\":\"string\",\"description\":\"Telegram "
          "file_id of the voice note\"}},\"required\":[\"file_id\"]}",
      .execute = tool_stt_execute,
  };
  register_tool(&stt);

  /* Register wake_on_lan */
  mimi_tool_t wol = {
      .name = "wake_on_lan",
      .description = "Send a Wake-on-LAN Magic Packet to a MAC address.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"mac\":{\"type\":\"string\",\"description\":\"MAC "
          "address (AA:BB:CC:DD:EE:FF)\"}},"
          "\"required\":[\"mac\"]}",
      .execute = tool_wol_execute,
  };
  register_tool(&wol);

  /* Register list_devices */
  mimi_tool_t ld_dev = {
      .name = "list_devices",
      .description = "List discovered network devices (WOL targets).",
      .input_schema_json =
          "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      .execute = tool_list_devices_execute,
  };
  register_tool(&ld_dev);

  /* Register wol_scan_start */
  mimi_tool_t wss = {
      .name = "wol_scan_start",
      .description = "Start an asynchronous network scan for WOL targets.",
      .input_schema_json =
          "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      .execute = tool_wol_scan_start_execute,
  };
  register_tool(&wss);

  /* Register wol_scan_result */
  mimi_tool_t wsr = {
      .name = "wol_scan_result",
      .description = "Get the result of the last asynchronous network scan.",
      .input_schema_json =
          "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      .execute = tool_wol_scan_result_execute,
  };
  register_tool(&wsr);

  /* Initialize background discovery */
  tool_discovery_init();

  /* Register restart (Admin-only) */
  mimi_tool_t rs = {
      .name = "restart",
      .description = "Restart the device. This is an administrative tool.",
      .input_schema_json =
          "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
      .execute = tool_restart_execute,
  };
  register_tool(&rs);

  /* Register memory_write */
  mimi_tool_t mw = {
      .name = "memory_write",
      .description = "Write to long-term memory (MEMORY.md). Overwrites "
                     "entire file. Use this for durable facts.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"content\":{\"type\":\"string\",\"description\":"
          "\"Content to write to long-term memory\"}},"
          "\"required\":[\"content\"]}",
      .execute = tool_memory_write_execute,
  };
  register_tool(&mw);

  /* Register memory_append */
  mimi_tool_t ma = {
      .name = "memory_append",
      .description = "Append a note to today's daily memory log. Use this for "
                     "tracking events or thoughts.",
      .input_schema_json =
          "{\"type\":\"object\","
          "\"properties\":{\"content\":{\"type\":\"string\",\"description\":"
          "\"Note to append\"}},"
          "\"required\":[\"content\"]}",
      .execute = tool_memory_append_execute,
  };
  register_tool(&ma);

  build_tools_json();

  ESP_LOGI(TAG, "Tool registry initialized");
  return ESP_OK;
}

const char *tool_registry_get_tools_json(void) { return s_tools_json; }

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size) {
  for (int i = 0; i < s_tool_count; i++) {
    if (strcmp(s_tools[i].name, name) == 0) {
      ESP_LOGI(TAG, "Executing tool: %s", name);
      return s_tools[i].execute(input_json, output, output_size);
    }
  }

  ESP_LOGW(TAG, "Unknown tool: %s", name);
  snprintf(output, output_size, "Error: unknown tool '%s'", name);
  return ESP_ERR_NOT_FOUND;
}
