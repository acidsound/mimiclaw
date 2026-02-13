#include "telegram_bot.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "llm/llm_stt.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "wifi/wifi_manager.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "telegram";
static const int TELEGRAM_SEND_TIMEOUT_MS = 10000;

static char s_bot_token[128] = MIMI_SECRET_TG_TOKEN;
static int64_t s_update_offset = 0;

#ifndef MIMI_BREAK_GLASS_ADMIN_ID
#define MIMI_BREAK_GLASS_ADMIN_ID 0
#endif

static bool has_chat_flag(char prefix, int64_t chat_id) {
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) != ESP_OK) {
    return false;
  }

  char key[16];
  snprintf(key, sizeof(key), "%c%" PRId64, prefix, chat_id);
  uint8_t val = 0;
  esp_err_t err = nvs_get_u8(nvs, key, &val);
  nvs_close(nvs);
  return (err == ESP_OK && val == 1);
}

static esp_err_t set_chat_flag(char prefix, int64_t chat_id, bool enabled) {
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) != ESP_OK) {
    return ESP_FAIL;
  }

  char key[16];
  snprintf(key, sizeof(key), "%c%" PRId64, prefix, chat_id);
  if (enabled) {
    nvs_set_u8(nvs, key, 1);
  } else {
    nvs_erase_key(nvs, key);
  }
  nvs_commit(nvs);
  nvs_close(nvs);
  return ESP_OK;
}

static bool is_chat_admin(int64_t chat_id) {
  if (MIMI_BREAK_GLASS_ADMIN_ID != 0 && chat_id == MIMI_BREAK_GLASS_ADMIN_ID) {
    return true;
  }
  return has_chat_flag('m', chat_id);
}

static bool is_chat_authorized(int64_t chat_id) {
  if (is_chat_admin(chat_id))
    return true;

  return has_chat_flag('a', chat_id);
}

/* HTTP response accumulator */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  http_resp_t *resp = (http_resp_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (resp->len + evt->data_len >= resp->cap) {
      size_t new_cap = resp->cap * 2;
      if (new_cap < resp->len + evt->data_len + 1) {
        new_cap = resp->len + evt->data_len + 1;
      }
      char *tmp = heap_caps_realloc(resp->buf, new_cap, MALLOC_CAP_SPIRAM);
      if (!tmp)
        return ESP_ERR_NO_MEM;
      resp->buf = tmp;
      resp->cap = new_cap;
    }
    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    resp->buf[resp->len] = '\0';
  }
  return ESP_OK;
}

static void log_wifi_not_ready_throttled(void) {
  static TickType_t s_last_warn = 0;
  TickType_t now = xTaskGetTickCount();
  if (s_last_warn == 0 || (now - s_last_warn) >= pdMS_TO_TICKS(15000)) {
    ESP_LOGW(TAG, "WiFi not connected, Telegram API call skipped");
    s_last_warn = now;
  }
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static char *tg_api_call_via_proxy(const char *path, const char *post_data,
                                   int timeout_ms) {
  proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, timeout_ms);
  if (!conn)
    return NULL;

  /* Build HTTP request */
  char header[512];
  int hlen;
  if (post_data) {
    hlen = snprintf(header, sizeof(header),
                    "POST /bot%s/%s HTTP/1.1\r\n"
                    "Host: api.telegram.org\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n",
                    s_bot_token, path, (int)strlen(post_data));
  } else {
    hlen = snprintf(header, sizeof(header),
                    "GET /bot%s/%s HTTP/1.1\r\n"
                    "Host: api.telegram.org\r\n"
                    "Connection: close\r\n\r\n",
                    s_bot_token, path);
  }

  if (proxy_conn_write(conn, header, hlen) < 0) {
    proxy_conn_close(conn);
    return NULL;
  }
  if (post_data && proxy_conn_write(conn, post_data, strlen(post_data)) < 0) {
    proxy_conn_close(conn);
    return NULL;
  }

  /* Read response — accumulate until connection close */
  size_t cap = 4096, len = 0;
  char *buf = calloc(1, cap);
  if (!buf) {
    proxy_conn_close(conn);
    return NULL;
  }

  int timeout = timeout_ms;
  while (1) {
    if (len + 1024 >= cap) {
      cap *= 2;
      char *tmp = realloc(buf, cap);
      if (!tmp)
        break;
      buf = tmp;
    }
    int n = proxy_conn_read(conn, buf + len, cap - len - 1, timeout);
    if (n <= 0)
      break;
    len += n;
  }
  buf[len] = '\0';
  proxy_conn_close(conn);

  /* Skip HTTP headers — find \r\n\r\n */
  char *body = strstr(buf, "\r\n\r\n");
  if (!body) {
    free(buf);
    return NULL;
  }
  body += 4;

  /* Return just the body */
  char *result = strdup(body);
  free(buf);
  return result;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static char *tg_api_call_direct(const char *method, const char *post_data,
                               int timeout_ms) {
  char url[256];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_bot_token,
           method);

  http_resp_t resp = {
      .buf = calloc(1, 4096),
      .len = 0,
      .cap = 4096,
  };
  if (!resp.buf)
    return NULL;

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = http_event_handler,
      .user_data = &resp,
      .timeout_ms = timeout_ms,
      .buffer_size = 2048,
      .buffer_size_tx = 2048,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    free(resp.buf);
    return NULL;
  }

  if (post_data) {
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
  }

  esp_err_t err = esp_http_client_perform(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    free(resp.buf);
    return NULL;
  }

  return resp.buf;
}

static char *tg_api_call_with_timeout(const char *method, const char *post_data,
                                     int timeout_ms) {
  if (!wifi_manager_is_connected()) {
    log_wifi_not_ready_throttled();
    return NULL;
  }

  char *resp = NULL;
  if (http_proxy_is_enabled()) {
    resp = tg_api_call_via_proxy(method, post_data, timeout_ms);
  } else {
    resp = tg_api_call_direct(method, post_data, timeout_ms);
  }

  /* Short operations get one quick retry to survive transient link flaps. */
  if (!resp && timeout_ms <= TELEGRAM_SEND_TIMEOUT_MS + 2000 &&
      wifi_manager_is_connected()) {
    vTaskDelay(pdMS_TO_TICKS(300));
    if (http_proxy_is_enabled()) {
      resp = tg_api_call_via_proxy(method, post_data, timeout_ms);
    } else {
      resp = tg_api_call_direct(method, post_data, timeout_ms);
    }
  }
  return resp;
}

static char *tg_api_call(const char *method, const char *post_data) {
  return tg_api_call_with_timeout(method, post_data,
                                  (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000);
}

static esp_err_t telegram_sync_commands(void) {
  static const char *commands_payload =
      "{"
      "\"commands\":["
      "{\"command\":\"help\",\"description\":\"Show available commands\"},"
      "{\"command\":\"start\",\"description\":\"Start and show usage\"},"
      "{\"command\":\"commands\",\"description\":\"Alias of /help\"},"
      "{\"command\":\"whoami\",\"description\":\"Show your chat id\"},"
      "{\"command\":\"status\",\"description\":\"Show current status\"},"
      "{\"command\":\"user_get\",\"description\":\"(Admin) Show USER.md\"},"
      "{\"command\":\"user_set\",\"description\":\"(Admin) Set USER.md\"},"
      "{\"command\":\"soul_get\",\"description\":\"(Admin) Show SOUL.md\"},"
      "{\"command\":\"soul_set\",\"description\":\"(Admin) Set SOUL.md\"},"
      "{\"command\":\"pf_ls\",\"description\":\"List LLM profiles\"},"
      "{\"command\":\"pf_use\",\"description\":\"Switch LLM profile\"},"
      "{\"command\":\"pf_rm\",\"description\":\"Delete LLM profile\"},"
      "{\"command\":\"set_provider\",\"description\":\"Set LLM provider\"},"
      "{\"command\":\"set_model\",\"description\":\"Set LLM model\"},"
      "{\"command\":\"set_base_url\",\"description\":\"Set LLM base URL\"},"
      "{\"command\":\"set_api_key\",\"description\":\"Set LLM API key\"},"
      "{\"command\":\"set_stt_provider\",\"description\":\"Set STT provider\"},"
      "{\"command\":\"set_stt_model\",\"description\":\"Set STT model\"},"
      "{\"command\":\"set_stt_base_url\",\"description\":\"Set STT base URL\"},"
      "{\"command\":\"set_stt_key\",\"description\":\"Set STT API key\"}"
      "]"
      "}";

  char *resp = tg_api_call_with_timeout("setMyCommands", commands_payload,
                                        TELEGRAM_SEND_TIMEOUT_MS);
  if (!resp) {
    ESP_LOGW(TAG, "setMyCommands failed: no response");
    return ESP_FAIL;
  }

  esp_err_t ret = ESP_FAIL;
  cJSON *root = cJSON_Parse(resp);
  if (root) {
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (cJSON_IsTrue(ok)) {
      ret = ESP_OK;
    } else {
      cJSON *desc = cJSON_GetObjectItem(root, "description");
      ESP_LOGW(TAG, "setMyCommands rejected: %s",
               cJSON_IsString(desc) ? desc->valuestring : "unknown");
    }
    cJSON_Delete(root);
  } else {
    ESP_LOGW(TAG, "setMyCommands parse failed");
  }

  free(resp);
  return ret;
}

static const char *skip_spaces(const char *s) {
  while (s && *s && isspace((unsigned char)*s)) {
    s++;
  }
  return s;
}

static bool extract_slash_command(const char *text, char *cmd, size_t cmd_cap,
                                  const char **out_args) {
  if (!text || text[0] != '/' || cmd_cap < 2) {
    return false;
  }

  const char *p = text + 1;
  size_t n = 0;
  while (*p && !isspace((unsigned char)*p) && *p != '@') {
    if (n + 1 >= cmd_cap) {
      return false;
    }
    cmd[n++] = *p++;
  }

  if (*p == '@') {
    while (*p && !isspace((unsigned char)*p)) {
      p++;
    }
  }

  cmd[n] = '\0';
  if (out_args) {
    *out_args = skip_spaces(p);
  }
  return n > 0;
}

static void send_profile_list_message(const char *chat_id) {
  char list[256] = {0};
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(list);
    nvs_get_str(nvs, MIMI_NVS_KEY_PF_LIST, list, &len);
    nvs_close(nvs);
  }

  const char *active = llm_get_active_profile();
  char msg[768];
  int off = snprintf(msg, sizeof(msg), "Active profile: %s\nProfiles:\n",
                     active ? active : "default");

  bool has_default = false;
  if (list[0]) {
    char list_copy[256];
    strncpy(list_copy, list, sizeof(list_copy) - 1);
    list_copy[sizeof(list_copy) - 1] = '\0';

    char *token = strtok(list_copy, ",");
    while (token && off < (int)sizeof(msg) - 32) {
      bool is_active = (active && strcmp(token, active) == 0);
      if (strcmp(token, "default") == 0) {
        has_default = true;
      }
      off += snprintf(msg + off, sizeof(msg) - (size_t)off, "%s %s%s\n",
                      is_active ? "->" : " -", token,
                      is_active ? " [ACTIVE]" : "");
      token = strtok(NULL, ",");
    }
  }

  if (!has_default && off < (int)sizeof(msg) - 32) {
    bool is_active_default = (active && strcmp(active, "default") == 0);
    off += snprintf(msg + off, sizeof(msg) - (size_t)off, "%s default%s\n",
                    is_active_default ? "->" : " -",
                    is_active_default ? " [ACTIVE]" : "");
  }

  telegram_send_message(chat_id, msg);
}

static bool require_admin_for_command(const char *chat_id, int64_t cid,
                                      const char *cmd) {
  if (is_chat_admin(cid)) {
    return true;
  }

  char msg[192];
  snprintf(msg, sizeof(msg), "Command /%s is admin-only.", cmd);
  telegram_send_message(chat_id, msg);
  return false;
}

static esp_err_t write_text_file(const char *path, const char *content) {
  if (!path || !content) {
    return ESP_ERR_INVALID_ARG;
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    return ESP_FAIL;
  }

  size_t len = strlen(content);
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  return (written == len) ? ESP_OK : ESP_FAIL;
}

static void send_file_snapshot(const char *chat_id, const char *path,
                               const char *title) {
  FILE *f = fopen(path, "r");
  if (!f) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s not found.", title);
    telegram_send_message(chat_id, msg);
    return;
  }

  size_t data_cap = 4096;
  char *data = malloc(data_cap + 1);
  if (!data) {
    fclose(f);
    telegram_send_message(chat_id, "Out of memory while reading file.");
    return;
  }

  size_t n = fread(data, 1, data_cap, f);
  bool truncated = !feof(f);
  data[n] = '\0';
  fclose(f);

  if (n == 0) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s is empty.", title);
    free(data);
    telegram_send_message(chat_id, msg);
    return;
  }

  const char *suffix = truncated ? "\n\n[truncated to 4KB]" : "";
  size_t out_cap = strlen(title) + n + strlen(suffix) + 16;
  char *out = malloc(out_cap);
  if (!out) {
    free(data);
    telegram_send_message(chat_id, "Out of memory while preparing response.");
    return;
  }

  snprintf(out, out_cap, "=== %s ===\n%s%s", title, data, suffix);
  telegram_send_message(chat_id, out);
  free(out);
  free(data);
}

static bool handle_local_command(const char *chat_id, int64_t cid,
                                 const char *text) {
  char cmd[32];
  const char *args = NULL;
  if (!extract_slash_command(text, cmd, sizeof(cmd), &args)) {
    return false;
  }

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "start") == 0 ||
      strcmp(cmd, "commands") == 0) {
    telegram_send_message(
        chat_id,
        "Available commands:\n"
        "/help - Show this help\n"
        "/start - Show this help\n"
        "/commands - Alias of /help\n"
        "/whoami - Show your sender chat id\n"
        "/status - Show system status\n"
        "/user_get - (Admin) show USER.md\n"
        "/user_set <text> - (Admin) overwrite USER.md\n"
        "/soul_get - (Admin) show SOUL.md\n"
        "/soul_set <text> - (Admin) overwrite SOUL.md\n"
        "/pf_ls - List LLM profiles\n"
        "/pf_use <name> - Switch active profile\n"
        "/pf_rm <name> - Delete profile (default blocked)\n"
        "/pf_del <name> - Alias of /pf_rm\n"
        "/set_provider <anthropic|openai|0|1> - Set LLM provider\n"
        "/set_model <model> - Set LLM model\n"
        "/set_base_url <url> - Set LLM base URL\n"
        "/set_api_key <key> - Set LLM API key\n"
        "/set_stt_provider <groq|0> - Set STT provider\n"
        "/set_stt_model <model> - Set STT model\n"
        "/set_stt_base_url <url> - Set STT base URL\n"
        "/set_stt_key <key> - Set STT API key\n\n"
        "Admin setup: use serial CLI `tg_admin_add <chat_id>` first.\n\n"
        "Usage examples:\n"
        "/user_get\n"
        "/user_set Name: Alice\\nLang: ko\\nTone: concise\n"
        "/soul_get\n"
        "/soul_set You are practical and concise.\n"
        "/pf_use prod\n"
        "/set_provider anthropic\n"
        "/set_model claude-3-5-sonnet-20241022\n"
        "/set_base_url https://api.anthropic.com/v1/messages\n"
        "/set_stt_provider groq\n"
        "/set_stt_model whisper-large-v3\n"
        "/set_stt_base_url https://api.groq.com/openai/v1\n\n"
        "Any non-command text is forwarded to the agent.");
    return true;
  }

  if (strcmp(cmd, "whoami") == 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "Your chat_id is %" PRId64, cid);
    telegram_send_message(chat_id, buf);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    char buf[320];
    snprintf(buf, sizeof(buf),
             "Status: running\n"
             "Active profile: %s\n"
             "Free heap: %u bytes\n"
             "Free psram: %u bytes\n"
             "Update offset: %" PRId64,
             llm_get_active_profile(),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             s_update_offset);
    telegram_send_message(chat_id, buf);
    return true;
  }

  if (strcmp(cmd, "user_get") == 0) {
    if (!require_admin_for_command(chat_id, cid, cmd)) {
      return true;
    }
    send_file_snapshot(chat_id, MIMI_USER_FILE, "USER.md");
    return true;
  }

  if (strcmp(cmd, "soul_get") == 0) {
    if (!require_admin_for_command(chat_id, cid, cmd)) {
      return true;
    }
    send_file_snapshot(chat_id, MIMI_SOUL_FILE, "SOUL.md");
    return true;
  }

  if (strcmp(cmd, "user_set") == 0) {
    if (!require_admin_for_command(chat_id, cid, cmd)) {
      return true;
    }
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /user_set <text>");
      return true;
    }
    if (write_text_file(MIMI_USER_FILE, args) == ESP_OK) {
      telegram_send_message(chat_id, "USER.md updated.");
    } else {
      telegram_send_message(chat_id, "Failed to update USER.md.");
    }
    return true;
  }

  if (strcmp(cmd, "soul_set") == 0) {
    if (!require_admin_for_command(chat_id, cid, cmd)) {
      return true;
    }
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /soul_set <text>");
      return true;
    }
    if (write_text_file(MIMI_SOUL_FILE, args) == ESP_OK) {
      telegram_send_message(chat_id, "SOUL.md updated.");
    } else {
      telegram_send_message(chat_id, "Failed to update SOUL.md.");
    }
    return true;
  }

  if (strcmp(cmd, "pf_ls") == 0) {
    send_profile_list_message(chat_id);
    return true;
  }

  if (strcmp(cmd, "pf_use") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /pf_use <name>");
      return true;
    }
    char name[16] = {0};
    size_t i = 0;
    while (args[i] && !isspace((unsigned char)args[i]) && i < sizeof(name) - 1) {
      name[i] = args[i];
      i++;
    }
    name[i] = '\0';
    if (name[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /pf_use <name>");
      return true;
    }
    esp_err_t err = llm_profile_use(name);
    if (err == ESP_OK) {
      char buf[96];
      snprintf(buf, sizeof(buf), "Active profile switched to: %s", name);
      telegram_send_message(chat_id, buf);
    } else {
      telegram_send_message(chat_id,
                            "pf_use failed. Name must be <=12 chars and "
                            "alnum/_/- only.");
    }
    return true;
  }

  if (strcmp(cmd, "pf_rm") == 0 || strcmp(cmd, "pf_del") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /pf_rm <name>");
      return true;
    }
    char name[16] = {0};
    size_t i = 0;
    while (args[i] && !isspace((unsigned char)args[i]) && i < sizeof(name) - 1) {
      name[i] = args[i];
      i++;
    }
    name[i] = '\0';
    if (name[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /pf_rm <name>");
      return true;
    }

    esp_err_t err = llm_profile_del(name);
    if (err == ESP_OK) {
      char buf[96];
      snprintf(buf, sizeof(buf), "Profile deleted: %s", name);
      telegram_send_message(chat_id, buf);
    } else if (err == ESP_ERR_INVALID_ARG) {
      telegram_send_message(chat_id,
                            "pf_rm failed. 'default' cannot be deleted.");
    } else {
      telegram_send_message(chat_id, "pf_rm failed. Profile not found.");
    }
    return true;
  }

  if (strcmp(cmd, "set_provider") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id,
                            "Usage: /set_provider <anthropic|openai|0|1>");
      return true;
    }
    int provider = -1;
    if (strcmp(args, "anthropic") == 0 || strcmp(args, "0") == 0) {
      provider = MIMI_LLM_PROVIDER_ANTHROPIC;
    } else if (strcmp(args, "openai") == 0 || strcmp(args, "1") == 0) {
      provider = MIMI_LLM_PROVIDER_OPENAI;
    }
    if (provider < 0) {
      telegram_send_message(
          chat_id,
          "Invalid provider. Use anthropic/openai (or 0/1).");
      return true;
    }
    if (llm_set_provider(provider) == ESP_OK) {
      telegram_send_message(chat_id, "LLM provider updated for active profile.");
    } else {
      telegram_send_message(chat_id, "Failed to set LLM provider.");
    }
    return true;
  }

  if (strcmp(cmd, "set_model") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /set_model <model>");
      return true;
    }
    if (llm_set_model(args) == ESP_OK) {
      telegram_send_message(chat_id, "LLM model updated for active profile.");
    } else {
      telegram_send_message(chat_id, "Failed to set LLM model.");
    }
    return true;
  }

  if (strcmp(cmd, "set_base_url") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /set_base_url <url>");
      return true;
    }
    if (llm_set_base_url(args) == ESP_OK) {
      telegram_send_message(chat_id, "LLM base URL updated for active profile.");
    } else {
      telegram_send_message(chat_id, "Failed to set LLM base URL.");
    }
    return true;
  }

  if (strcmp(cmd, "set_api_key") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /set_api_key <key>");
      return true;
    }
    if (llm_set_api_key(args) == ESP_OK) {
      telegram_send_message(chat_id, "LLM API key updated for active profile.");
    } else {
      telegram_send_message(chat_id, "Failed to set LLM API key.");
    }
    return true;
  }

  if (strcmp(cmd, "set_stt_provider") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /set_stt_provider <groq|0>");
      return true;
    }
    int provider = -1;
    if (strcmp(args, "groq") == 0 || strcmp(args, "0") == 0) {
      provider = MIMI_STT_PROVIDER_GROQ;
    }
    if (provider < 0) {
      telegram_send_message(chat_id,
                            "Invalid provider. Supported: groq (0)");
      return true;
    }
    if (llm_stt_set_provider(provider) == ESP_OK) {
      telegram_send_message(chat_id, "STT provider set to groq (0).");
    } else {
      telegram_send_message(chat_id, "Failed to set STT provider.");
    }
    return true;
  }

  if (strcmp(cmd, "set_stt_model") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /set_stt_model <model>");
      return true;
    }
    if (llm_stt_set_model(args) == ESP_OK) {
      telegram_send_message(chat_id, "STT model updated.");
    } else {
      telegram_send_message(chat_id, "Failed to set STT model.");
    }
    return true;
  }

  if (strcmp(cmd, "set_stt_base_url") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /set_stt_base_url <url>");
      return true;
    }
    if (llm_stt_set_base_url(args) == ESP_OK) {
      telegram_send_message(chat_id, "STT base URL updated.");
    } else {
      telegram_send_message(chat_id, "Failed to set STT base URL.");
    }
    return true;
  }

  if (strcmp(cmd, "set_stt_key") == 0) {
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
      telegram_send_message(chat_id, "Usage: /set_stt_key <key>");
      return true;
    }
    if (llm_stt_set_key(args) == ESP_OK) {
      telegram_send_message(chat_id, "STT API key updated.");
    } else {
      telegram_send_message(chat_id, "Failed to set STT API key.");
    }
    return true;
  }

  char buf[160];
  snprintf(buf, sizeof(buf), "Unknown command: /%s\nUse /help.", cmd);
  telegram_send_message(chat_id, buf);
  return true;
}

static void process_updates(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root)
    return;

  cJSON *ok = cJSON_GetObjectItem(root, "ok");
  if (!cJSON_IsTrue(ok)) {
    cJSON_Delete(root);
    return;
  }

  cJSON *result = cJSON_GetObjectItem(root, "result");
  if (!cJSON_IsArray(result)) {
    cJSON_Delete(root);
    return;
  }

  cJSON *update;
  cJSON_ArrayForEach(update, result) {
    /* Track offset */
    cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
    if (cJSON_IsNumber(update_id)) {
      int64_t uid = (int64_t)update_id->valuedouble;
      if (uid >= s_update_offset) {
        s_update_offset = uid + 1;
        /* Persist offset periodically or after processing */
        nvs_handle_t nvs;
        if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) == ESP_OK) {
          nvs_set_i64(nvs, "offset", s_update_offset);
          nvs_commit(nvs);
          nvs_close(nvs);
        }
      }
    }

    /* Extract message */
    cJSON *message = cJSON_GetObjectItem(update, "message");
    if (!message)
      continue;

    cJSON *chat = cJSON_GetObjectItem(message, "chat");
    if (!chat)
      continue;

    cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
    if (!chat_id)
      continue;

    char chat_id_str[32];
    snprintf(chat_id_str, sizeof(chat_id_str), "%.0f", chat_id->valuedouble);
    int64_t cid = (int64_t)chat_id->valuedouble;

    if (!is_chat_authorized(cid)) {
      ESP_LOGW(TAG, "Unauthorized message from chat %s", chat_id_str);
      continue;
    }

    /* Message contents */
    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id_str, sizeof(msg.chat_id) - 1);

    cJSON *text = cJSON_GetObjectItem(message, "text");
    cJSON *photo = cJSON_GetObjectItem(message, "photo");
    cJSON *voice = cJSON_GetObjectItem(message, "voice");
    cJSON *caption = cJSON_GetObjectItem(message, "caption");

    if (text && cJSON_IsString(text) &&
        handle_local_command(chat_id_str, cid, text->valuestring)) {
      continue;
    }

    if (photo && cJSON_IsArray(photo)) {
      /* Pick the largest photo (last element in array) */
      int size = cJSON_GetArraySize(photo);
      cJSON *best = cJSON_GetArrayItem(photo, size - 1);
      cJSON *fid = cJSON_GetObjectItem(best, "file_id");
      cJSON *fsize = cJSON_GetObjectItem(best, "file_size");
      if (fid && cJSON_IsString(fid)) {
        msg.type = MIMI_MSG_TYPE_PHOTO;
        msg.media_id = strdup(fid->valuestring);
        msg.content = strdup(caption ? caption->valuestring : "");
        if (cJSON_IsNumber(fsize)) {
          msg.media_size = (size_t)fsize->valuedouble;
        }
      }
    } else if (voice) {
      cJSON *fid = cJSON_GetObjectItem(voice, "file_id");
      cJSON *fsize = cJSON_GetObjectItem(voice, "file_size");
      cJSON *dur = cJSON_GetObjectItem(voice, "duration");
      if (fid && cJSON_IsString(fid)) {
        msg.type = MIMI_MSG_TYPE_VOICE;
        msg.media_id = strdup(fid->valuestring);
        msg.content = strdup(caption ? caption->valuestring : "");
        if (cJSON_IsNumber(fsize))
          msg.media_size = (size_t)fsize->valuedouble;
        if (cJSON_IsNumber(dur))
          msg.media_duration = (int)dur->valuedouble;
      }
    } else if (text && cJSON_IsString(text)) {
      msg.type = MIMI_MSG_TYPE_TEXT;
      msg.content = strdup(text->valuestring);
    }

    if (msg.content) {
      ESP_LOGI(TAG, "Authorized %s from chat %s: %.40s...",
               (msg.type == MIMI_MSG_TYPE_PHOTO)   ? "photo"
               : (msg.type == MIMI_MSG_TYPE_VOICE) ? "voice"
                                                   : "message",
               chat_id_str, msg.content);
      message_bus_push_inbound(&msg);
    } else {
      if (msg.media_id)
        free(msg.media_id);
    }
  }

  cJSON_Delete(root);
}

static void telegram_poll_task(void *arg) {
  ESP_LOGI(TAG, "Telegram polling task started");

  while (1) {
    if (s_bot_token[0] == '\0') {
      ESP_LOGW(TAG, "No bot token configured, waiting...");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    char params[192];
    snprintf(params, sizeof(params),
             "getUpdates?offset=%" PRId64
             "&timeout=%d&limit=10&allowed_updates=[\"message\"]",
             s_update_offset, MIMI_TG_POLL_TIMEOUT_S);

    char *resp = tg_api_call(params, NULL);
    if (resp) {
      process_updates(resp);
      free(resp);
    } else {
      /* Back off on error */
      vTaskDelay(pdMS_TO_TICKS(3000));
    }
  }
}

/* --- Public API --- */

esp_err_t telegram_bot_init(void) {
  /* NVS overrides take highest priority (set via CLI) */
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) == ESP_OK) {
    char tmp[128] = {0};
    size_t len = sizeof(tmp);
    if (nvs_get_str(nvs, MIMI_NVS_KEY_TG_TOKEN, tmp, &len) == ESP_OK &&
        tmp[0]) {
      strncpy(s_bot_token, tmp, sizeof(s_bot_token) - 1);
    }
    nvs_get_i64(nvs, "offset", &s_update_offset);
    nvs_close(nvs);
  }

  /* s_bot_token is already initialized from MIMI_SECRET_TG_TOKEN as fallback */

  if (s_bot_token[0]) {
    ESP_LOGI(TAG, "Telegram bot token loaded (len=%d)",
             (int)strlen(s_bot_token));
  } else {
    ESP_LOGW(TAG, "No Telegram bot token. Use CLI: set_tg_token <TOKEN>");
  }
  return ESP_OK;
}

esp_err_t telegram_bot_start(void) {
  if (s_bot_token[0]) {
    if (telegram_sync_commands() == ESP_OK) {
      ESP_LOGI(TAG, "Telegram commands synced");
    } else {
      ESP_LOGW(TAG, "Telegram commands sync failed (continuing)");
    }
  }

  BaseType_t ret =
      xTaskCreatePinnedToCore(telegram_poll_task, "tg_poll", MIMI_TG_POLL_STACK,
                              NULL, MIMI_TG_POLL_PRIO, NULL, MIMI_TG_POLL_CORE);

  return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

static char *telegram_escape_html(const char *text) {
  if (!text)
    return NULL;

  size_t in_len = strlen(text);
  size_t cap = in_len * 6 + 1; /* worst-case: "&" -> "&amp;" */
  char *out = malloc(cap);
  if (!out)
    return NULL;

  const char *src = text;
  char *dst = out;
  while (*src) {
    if ((size_t)(dst - out) + 6 >= cap) {
      cap *= 2;
      size_t offset = dst - out;
      char *tmp = realloc(out, cap);
      if (!tmp) {
        free(out);
        return NULL;
      }
      out = tmp;
      dst = out + offset;
    }

    if (*src == '<') {
      memcpy(dst, "&lt;", 4);
      dst += 4;
    } else if (*src == '>') {
      memcpy(dst, "&gt;", 4);
      dst += 4;
    } else if (*src == '&') {
      memcpy(dst, "&amp;", 5);
      dst += 5;
    } else {
      *dst++ = *src;
    }
    src++;
  }
  *dst = '\0';
  return out;
}

esp_err_t telegram_send_message(const char *chat_id, const char *text) {
  if (s_bot_token[0] == '\0') {
    ESP_LOGW(TAG, "Cannot send: no bot token");
    return ESP_ERR_INVALID_STATE;
  }
  if (!text) {
    ESP_LOGW(TAG, "Cannot send: text is NULL");
    return ESP_ERR_INVALID_ARG;
  }

  /* Split long messages at 4096-char boundary */
  size_t text_len = strlen(text);
  size_t offset = 0;

  while (offset < text_len) {
    size_t chunk = text_len - offset;
    if (chunk > MIMI_TG_MAX_MSG_LEN) {
      chunk = MIMI_TG_MAX_MSG_LEN;
    }

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "chat_id", chat_id);

    /* Create null-terminated chunk */
    char *segment = malloc(chunk + 1);
    if (!segment) {
      cJSON_Delete(body);
      return ESP_ERR_NO_MEM;
    }
    memcpy(segment, text + offset, chunk);
    segment[chunk] = '\0';

    /* Escape HTML entities and keep plain message format */
    char *html_text = telegram_escape_html(segment);
    if (!html_text) {
      cJSON_Delete(body);
      free(segment);
      return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "text", html_text);
    cJSON_AddStringToObject(body, "parse_mode", "HTML");
    free(html_text);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(segment);

    if (json_str) {
      char *resp = tg_api_call_with_timeout(
          "sendMessage", json_str, TELEGRAM_SEND_TIMEOUT_MS);
      free(json_str);
      if (resp) {
        cJSON *root = cJSON_Parse(resp);
        if (root) {
          cJSON *ok_field = cJSON_GetObjectItem(root, "ok");
          if (!cJSON_IsTrue(ok_field)) {
            cJSON *desc = cJSON_GetObjectItem(root, "description");
            ESP_LOGW(TAG, "HTML send failed: %s",
                     desc ? desc->valuestring : "unknown");
          }
          cJSON_Delete(root);
        }
        free(resp);
      }
    }

    offset += chunk;
  }

  return ESP_OK;
}

esp_err_t telegram_set_token(const char *token) {
  nvs_handle_t nvs;
  ESP_ERROR_CHECK(nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs));
  ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_TG_TOKEN, token));
  ESP_ERROR_CHECK(nvs_commit(nvs));
  nvs_close(nvs);

  strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
  ESP_LOGI(TAG, "Telegram bot token saved");
  return ESP_OK;
}

esp_err_t telegram_auth_add(int64_t chat_id) {
  esp_err_t err = set_chat_flag('a', chat_id, true);
  if (err != ESP_OK)
    return err;
  ESP_LOGI(TAG, "Authorized chat %" PRId64, chat_id);
  return ESP_OK;
}

esp_err_t telegram_auth_remove(int64_t chat_id) {
  esp_err_t err = set_chat_flag('a', chat_id, false);
  if (err != ESP_OK)
    return err;
  ESP_LOGI(TAG, "Deauthorized chat %" PRId64, chat_id);
  return ESP_OK;
}

void telegram_auth_list(void) {
  printf("Authorized Telegram Chats:\n");
  printf("  - CLI list is not enumerated; use tg_auth_add/remove for management.\n");
}

esp_err_t telegram_admin_add(int64_t chat_id) {
  esp_err_t err = set_chat_flag('m', chat_id, true);
  if (err != ESP_OK)
    return err;
  ESP_LOGI(TAG, "Admin chat added %" PRId64, chat_id);
  return ESP_OK;
}

esp_err_t telegram_admin_remove(int64_t chat_id) {
  esp_err_t err = set_chat_flag('m', chat_id, false);
  if (err != ESP_OK)
    return err;
  ESP_LOGI(TAG, "Admin chat removed %" PRId64, chat_id);
  return ESP_OK;
}

void telegram_admin_list(void) {
  printf("Telegram Admin Source:\n");
  if (MIMI_BREAK_GLASS_ADMIN_ID != 0) {
    printf("  - break_glass_admin: %" PRId64 "\n",
           (int64_t)MIMI_BREAK_GLASS_ADMIN_ID);
  } else {
    printf("  - break_glass_admin: (not set)\n");
  }
  printf("  - NVS admins are enabled (managed by tg_admin_add/remove).\n");
}

typedef struct {
  telegram_media_chunk_cb_t cb;
  void *ctx;
  esp_err_t status;
} tg_stream_ctx_t;

static esp_err_t telegram_stream_event_handler(esp_http_client_event_t *evt) {
  tg_stream_ctx_t *sctx = (tg_stream_ctx_t *)evt->user_data;
  if (!sctx || sctx->status != ESP_OK)
    return sctx ? sctx->status : ESP_FAIL;

  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
    sctx->status = sctx->cb((const uint8_t *)evt->data, evt->data_len,
                            sctx->ctx);
  }
  return sctx->status;
}

esp_err_t telegram_get_file_info(const char *file_id,
                                 telegram_file_info_t *info) {
  if (!file_id || !info || s_bot_token[0] == '\0')
    return ESP_ERR_INVALID_ARG;

  memset(info, 0, sizeof(*info));

  char params[256];
  snprintf(params, sizeof(params), "getFile?file_id=%s", file_id);
  char *resp = tg_api_call(params, NULL);
  if (!resp)
    return ESP_FAIL;

  cJSON *root = cJSON_Parse(resp);
  free(resp);
  if (!root)
    return ESP_FAIL;

  esp_err_t ret = ESP_FAIL;
  cJSON *ok = cJSON_GetObjectItem(root, "ok");
  if (cJSON_IsTrue(ok)) {
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *fpath = cJSON_GetObjectItem(result, "file_path");
    cJSON *fsize = cJSON_GetObjectItem(result, "file_size");
    if (cJSON_IsString(fpath)) {
      info->path = strdup(fpath->valuestring);
      if (info->path) {
        if (cJSON_IsNumber(fsize)) {
          info->size = (size_t)fsize->valuedouble;
        }
        ret = ESP_OK;
      }
    } else {
      cJSON *desc = cJSON_GetObjectItem(root, "description");
      ESP_LOGW(TAG, "No file_path for file_id=%s: %s",
               file_id, desc ? desc->valuestring : "unknown");
    }
  } else {
    cJSON *desc = cJSON_GetObjectItem(root, "description");
    ESP_LOGW(TAG, "getFile failed for file_id=%s: %s", file_id,
             desc ? desc->valuestring : "unknown");
  }
  cJSON_Delete(root);
  return ret;
}

void telegram_file_info_free(telegram_file_info_t *info) {
  if (!info)
    return;
  free(info->path);
  info->path = NULL;
  info->size = 0;
}

static esp_err_t telegram_stream_file_internal(const char *file_path,
                                               size_t chunk_size,
                                               telegram_media_chunk_cb_t cb,
                                               void *ctx) {
  if (!file_path || !cb || s_bot_token[0] == '\0')
    return ESP_ERR_INVALID_ARG;

  char url[256];
  snprintf(url, sizeof(url), "https://api.telegram.org/file/bot%s/%s",
           s_bot_token, file_path);

  if (chunk_size == 0 || chunk_size > MIMI_MEDIA_STREAM_CHUNK)
    chunk_size = MIMI_MEDIA_STREAM_CHUNK;

  tg_stream_ctx_t sctx = {.cb = cb, .ctx = ctx, .status = ESP_OK};

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = telegram_stream_event_handler,
      .user_data = &sctx,
      .timeout_ms = 30000,
      .buffer_size = chunk_size,
      .buffer_size_tx = chunk_size,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  if (http_proxy_is_enabled()) {
    config.host = http_proxy_get_host();
    config.port = http_proxy_get_port();
    config.transport_type = HTTP_TRANSPORT_OVER_TCP;
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client)
    return ESP_ERR_NO_MEM;

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  if (err != ESP_OK || status != 200 || sctx.status != ESP_OK) {
    ESP_LOGE(TAG, "telegram stream failed path=%s status=%d cb=%s err=%s", file_path,
             status, esp_err_to_name(sctx.status), esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);

  if (err != ESP_OK)
    return err;
  if (sctx.status != ESP_OK)
    return sctx.status;
  if (status != 200)
    return ESP_FAIL;
  return ESP_OK;
}

esp_err_t telegram_stream_file(const telegram_file_info_t *info,
                               size_t chunk_size,
                               telegram_media_chunk_cb_t cb, void *ctx) {
  if (!info || !info->path)
    return ESP_ERR_INVALID_ARG;
  return telegram_stream_file_internal(info->path, chunk_size, cb, ctx);
}

esp_err_t telegram_stream_file_by_id(const char *file_id, size_t chunk_size,
                                     telegram_media_chunk_cb_t cb,
                                     void *ctx) {
  telegram_file_info_t info;
  esp_err_t err = telegram_get_file_info(file_id, &info);
  if (err != ESP_OK)
    return err;
  err = telegram_stream_file(&info, chunk_size, cb, ctx);
  telegram_file_info_free(&info);
  return err;
}
