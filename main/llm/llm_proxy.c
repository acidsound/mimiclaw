#include "llm_proxy.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "llm";

static char s_api_key[128] = {0};
static char s_model[64] = MIMI_LLM_DEFAULT_MODEL;
static int s_provider = MIMI_LLM_DEFAULT_PROVIDER;
static char s_base_url[128] = {0};

/* ── Response buffer ──────────────────────────────────────────── */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} resp_buf_t;

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap) {
  rb->data = heap_caps_calloc(1, initial_cap, MALLOC_CAP_SPIRAM);
  if (!rb->data)
    return ESP_ERR_NO_MEM;
  rb->len = 0;
  rb->cap = initial_cap;
  return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len) {
  while (rb->len + len >= rb->cap) {
    size_t new_cap = rb->cap * 2;
    char *tmp = heap_caps_realloc(rb->data, new_cap, MALLOC_CAP_SPIRAM);
    if (!tmp)
      return ESP_ERR_NO_MEM;
    rb->data = tmp;
    rb->cap = new_cap;
  }
  memcpy(rb->data + rb->len, data, len);
  rb->len += len;
  rb->data[rb->len] = '\0';
  return ESP_OK;
}

static void resp_buf_free(resp_buf_t *rb) {
  free(rb->data);
  rb->data = NULL;
  rb->len = 0;
  rb->cap = 0;
}

/* ── HTTP event handler (for esp_http_client direct path) ─────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  resp_buf_t *rb = (resp_buf_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    resp_buf_append(rb, (const char *)evt->data, evt->data_len);
  }
  return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

/* ── Helper: Trim whitespace ──────────────────────────────────── */
static void trim_inplace(char *s) {
  char *end = s + strlen(s) - 1;
  while (end >= s && ((unsigned char)*end <= 32)) {
    *end-- = '\0';
  }
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t llm_proxy_init(void) {
  /* Start with build-time defaults */
  if (MIMI_SECRET_API_KEY[0] != '\0') {
    strncpy(s_api_key, MIMI_SECRET_API_KEY, sizeof(s_api_key) - 1);
  }
  if (MIMI_SECRET_MODEL[0] != '\0') {
    strncpy(s_model, MIMI_SECRET_MODEL, sizeof(s_model) - 1);
  }
#ifdef MIMI_SECRET_PROVIDER
  s_provider = MIMI_SECRET_PROVIDER;
#endif
  if (MIMI_SECRET_BASE_URL[0] != '\0') {
    strncpy(s_base_url, MIMI_SECRET_BASE_URL, sizeof(s_base_url) - 1);
  } else {
    /* Set default URL based on provider if not specified */
    if (s_provider == MIMI_LLM_PROVIDER_ANTHROPIC) {
      strcpy(s_base_url, MIMI_LLM_API_URL_ANTHROPIC);
    } else {
      strcpy(s_base_url, MIMI_LLM_API_URL_OPENAI);
    }
  }

  /* NVS overrides take highest priority (set via CLI) */
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    char tmp[128] = {0};
    size_t len = sizeof(tmp);
    if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
      strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
    }
    len = sizeof(tmp);
    memset(tmp, 0, sizeof(tmp));
    if (nvs_get_str(nvs, MIMI_NVS_KEY_MODEL, tmp, &len) == ESP_OK && tmp[0]) {
      strncpy(s_model, tmp, sizeof(s_model) - 1);
    }

    int32_t val;
    if (nvs_get_i32(nvs, MIMI_NVS_KEY_PROVIDER, &val) == ESP_OK) {
      s_provider = (int)val;
    }

    len = sizeof(tmp);
    memset(tmp, 0, sizeof(tmp));
    if (nvs_get_str(nvs, MIMI_NVS_KEY_BASE_URL, tmp, &len) == ESP_OK &&
        tmp[0]) {
      strncpy(s_base_url, tmp, sizeof(s_base_url) - 1);
    }

    nvs_close(nvs);
  }

  /* Sanitize inputs */
  trim_inplace(s_api_key);
  trim_inplace(s_base_url);
  trim_inplace(s_model);

  if (s_api_key[0]) {
    ESP_LOGI(TAG, "LLM initialized: provider=%d, model=%s", s_provider,
             s_model);
    ESP_LOGI(TAG, "Base URL: %s", s_base_url);

    /* Timezone info check from NVS */
    char tz[64] = {0};
    size_t tz_len = sizeof(tz);
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
      if (nvs_get_str(nvs, MIMI_NVS_KEY_TIMEZONE, tz, &tz_len) == ESP_OK) {
        ESP_LOGI(TAG, "TimeZone (NVS): %s", tz);
      }
      nvs_close(nvs);
    }
  } else {
    ESP_LOGW(TAG, "No API key. Use CLI: set_api_key <KEY>");
  }
  return ESP_OK;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static esp_err_t llm_http_direct(const char *post_data, resp_buf_t *rb,
                                 int *out_status) {
  esp_http_client_config_t config = {
      .url = s_base_url,
      .event_handler = http_event_handler,
      .user_data = rb,
      .timeout_ms = 120 * 1000,
      .buffer_size = 8192,
      .buffer_size_tx = 4096,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client)
    return ESP_FAIL;

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");

  if (s_provider == MIMI_LLM_PROVIDER_ANTHROPIC) {
    esp_http_client_set_header(client, "x-api-key", s_api_key);
    esp_http_client_set_header(client, "anthropic-version",
                               MIMI_LLM_API_VERSION);
  } else {
    /* OpenAI / Kimi compatible */
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
    esp_http_client_set_header(client, "Authorization", auth);

    if (strstr(s_base_url, "openrouter.ai")) {
      esp_http_client_set_header(client, "HTTP-Referer",
                                 "https://github.com/acidsound/mimiclaw");
      esp_http_client_set_header(client, "X-Title", "MimiClaw");
    }
  }

  esp_http_client_set_post_field(client, post_data, strlen(post_data));

  esp_err_t err = esp_http_client_perform(client);
  *out_status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  return err;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static esp_err_t llm_http_via_proxy(const char *post_data, resp_buf_t *rb,
                                    int *out_status) {
  /* Basic URL parsing to separate Host and Path */
  char host[128] = {0};
  char path[128] = "/";
  int port = 443;

  /* Extract host/path from s_base_url (simple parser) */
  const char *p = s_base_url;
  if (strncmp(p, "https://", 8) == 0)
    p += 8;
  else if (strncmp(p, "http://", 7) == 0)
    p += 7;

  const char *slash = strchr(p, '/');
  if (slash) {
    size_t hlen = slash - p;
    if (hlen >= sizeof(host))
      hlen = sizeof(host) - 1;
    strncpy(host, p, hlen);
    strncpy(path, slash, sizeof(path) - 1);
  } else {
    strncpy(host, p, sizeof(host) - 1);
  }

  proxy_conn_t *conn = proxy_conn_open(host, port, 30000);
  if (!conn)
    return ESP_ERR_HTTP_CONNECT;

  int body_len = strlen(post_data);
  char header[1024];
  int hlen = 0;

  if (s_provider == MIMI_LLM_PROVIDER_ANTHROPIC) {
    hlen = snprintf(header, sizeof(header),
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Content-Type: application/json\r\n"
                    "x-api-key: %s\r\n"
                    "anthropic-version: %s\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n",
                    path, host, s_api_key, MIMI_LLM_API_VERSION, body_len);
  } else {
    char auth[140];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);

    const char *or_headers = "";
    if (strstr(s_base_url, "openrouter.ai")) {
      or_headers = "HTTP-Referer: https://github.com/acidsound/mimiclaw\r\n"
                   "X-Title: MimiClaw\r\n";
    }

    hlen = snprintf(header, sizeof(header),
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Authorization: %s\r\n"
                    "%s"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n",
                    path, host, auth, or_headers, body_len);
  }

  if (proxy_conn_write(conn, header, hlen) < 0 ||
      proxy_conn_write(conn, post_data, body_len) < 0) {
    proxy_conn_close(conn);
    return ESP_ERR_HTTP_WRITE_DATA;
  }

  /* Read full response into buffer */
  char tmp[4096];
  while (1) {
    int n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000);
    if (n <= 0)
      break;
    if (resp_buf_append(rb, tmp, n) != ESP_OK)
      break;
  }
  proxy_conn_close(conn);

  /* Parse status line */
  *out_status = 0;
  if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
    const char *sp = strchr(rb->data, ' ');
    if (sp)
      *out_status = atoi(sp + 1);
  }

  /* Strip HTTP headers, keep body only */
  char *body = strstr(rb->data, "\r\n\r\n");
  if (body) {
    body += 4;
    size_t blen = rb->len - (body - rb->data);
    memmove(rb->data, body, blen);
    rb->len = blen;
    rb->data[rb->len] = '\0';
  }

  return ESP_OK;
}

/* ── Shared HTTP dispatch ─────────────────────────────────────── */

static esp_err_t llm_http_call(const char *post_data, resp_buf_t *rb,
                               int *out_status) {
  if (http_proxy_is_enabled()) {
    return llm_http_via_proxy(post_data, rb, out_status);
  } else {
    return llm_http_direct(post_data, rb, out_status);
  }
}

/* ── Parse text from JSON response ────────────────────────────── */

static void extract_text(cJSON *root, char *buf, size_t size) {
  buf[0] = '\0';

  /* Try Anthropic style (content array) */
  cJSON *content = cJSON_GetObjectItem(root, "content");
  if (content && cJSON_IsArray(content)) {
    size_t off = 0;
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
      cJSON *btype = cJSON_GetObjectItem(block, "type");
      if (!btype || strcmp(btype->valuestring, "text") != 0)
        continue;
      cJSON *text = cJSON_GetObjectItem(block, "text");
      if (!text || !cJSON_IsString(text))
        continue;
      size_t tlen = strlen(text->valuestring);
      size_t copy = (tlen < size - off - 1) ? tlen : size - off - 1;
      memcpy(buf + off, text->valuestring, copy);
      off += copy;
    }
    buf[off] = '\0';
    return;
  }

  /* Try OpenAI style (choices[0].message.content) */
  cJSON *choices = cJSON_GetObjectItem(root, "choices");
  if (choices && cJSON_IsArray(choices)) {
    cJSON *first = cJSON_GetArrayItem(choices, 0);
    if (first) {
      cJSON *msg = cJSON_GetObjectItem(first, "message");
      if (msg) {
        cJSON *text = cJSON_GetObjectItem(msg, "content");
        if (text && cJSON_IsString(text)) {
          strncpy(buf, text->valuestring, size - 1);
          return;
        }
      }
    }
  }
}

/* ── Public: simple chat (backward compat) ────────────────────── */

esp_err_t llm_chat(const char *system_prompt, const char *messages_json,
                   char *response_buf, size_t buf_size) {
  if (s_api_key[0] == '\0') {
    snprintf(response_buf, buf_size, "Error: No API key configured");
    return ESP_ERR_INVALID_STATE;
  }

  /* Build request body (non-streaming) */
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "model", s_model);
  cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);

  cJSON *messages_arr = NULL;
  cJSON *messages_arg = cJSON_Parse(messages_json);

  if (s_provider == MIMI_LLM_PROVIDER_ANTHROPIC) {
    /* Anthropic: "system" is top-level, "messages" is separate */
    cJSON_AddStringToObject(body, "system", system_prompt);
    if (messages_arg) {
      cJSON_AddItemToObject(body, "messages", messages_arg);
    } else {
      cJSON *arr = cJSON_CreateArray();
      cJSON *msg = cJSON_CreateObject();
      cJSON_AddStringToObject(msg, "role", "user");
      cJSON_AddStringToObject(msg, "content", messages_json);
      cJSON_AddItemToArray(arr, msg);
      cJSON_AddItemToObject(body, "messages", arr);
    }
  } else {
    /* OpenAI: "system" is the first message */
    messages_arr = cJSON_CreateArray();

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages_arr, sys_msg);

    if (messages_arg) {
      if (cJSON_IsArray(messages_arg)) {
        /* Append all messages */
        cJSON *item;
        cJSON_ArrayForEach(item, messages_arg) {
          cJSON_AddItemToArray(messages_arr, cJSON_Duplicate(item, 1));
        }
      }
      cJSON_Delete(messages_arg);
    } else {
      cJSON *msg = cJSON_CreateObject();
      cJSON_AddStringToObject(msg, "role", "user");
      cJSON_AddStringToObject(msg, "content", messages_json);
      cJSON_AddItemToArray(messages_arr, msg);
    }
    cJSON_AddItemToObject(body, "messages", messages_arr);
  }

  char *post_data = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (!post_data) {
    snprintf(response_buf, buf_size, "Error: Failed to build request");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "[LLM_CALL] provider=%s, model=%s, body=%d bytes",
           (s_provider == MIMI_LLM_PROVIDER_ANTHROPIC) ? "Anthropic"
                                                       : "OpenAI/Kimi",
           s_model, (int)strlen(post_data));

  resp_buf_t rb;
  if (resp_buf_init(&rb, MIMI_LLM_STREAM_BUF_SIZE) != ESP_OK) {
    free(post_data);
    snprintf(response_buf, buf_size, "Error: Out of memory");
    return ESP_ERR_NO_MEM;
  }

  int status = 0;
  esp_err_t err = llm_http_call(post_data, &rb, &status);
  free(post_data);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    resp_buf_free(&rb);
    snprintf(response_buf, buf_size, "Error: HTTP request failed (%s)",
             esp_err_to_name(err));
    return err;
  }

  /* Raw response logging */
  /* Log cleaned up in unified block later */

  if (status != 200) {
    ESP_LOGE(TAG, "API returned status %d", status);
    snprintf(response_buf, buf_size, "API error (HTTP %d): %.200s", status,
             rb.data ? rb.data : "");
    resp_buf_free(&rb);
    return ESP_FAIL;
  }

  /* Parse JSON response */
  cJSON *root = cJSON_Parse(rb.data);
  resp_buf_free(&rb);

  if (!root) {
    snprintf(response_buf, buf_size, "Error: Failed to parse response");
    return ESP_FAIL;
  }

  extract_text(root, response_buf, buf_size);
  cJSON_Delete(root);

  if (response_buf[0] == '\0') {
    snprintf(response_buf, buf_size, "No response from Claude API");
  } else {
    ESP_LOGI(TAG, "Claude response: %d bytes", (int)strlen(response_buf));
  }

  return ESP_OK;
}

/* ── Helper: Tool schema conversion (Anthropic -> OpenAI) ────── */
static cJSON *convert_anthropic_to_openai_tools(const char *tools_json) {
  cJSON *anth_tools = cJSON_Parse(tools_json);
  if (!anth_tools || !cJSON_IsArray(anth_tools)) {
    cJSON_Delete(anth_tools);
    return NULL;
  }

  cJSON *oa_tools = cJSON_CreateArray();
  cJSON *item;
  cJSON_ArrayForEach(item, anth_tools) {
    cJSON *name = cJSON_GetObjectItem(item, "name");
    cJSON *desc = cJSON_GetObjectItem(item, "description");
    cJSON *schema = cJSON_GetObjectItem(item, "input_schema");

    if (name && cJSON_IsString(name)) {
      cJSON *func_wrapper = cJSON_CreateObject();
      cJSON_AddStringToObject(func_wrapper, "type", "function");

      cJSON *func = cJSON_CreateObject();
      cJSON_AddStringToObject(func, "name", name->valuestring);
      if (desc && cJSON_IsString(desc)) {
        cJSON_AddStringToObject(func, "description", desc->valuestring);
      }
      if (schema) {
        cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));
      }

      cJSON_AddItemToObject(func_wrapper, "function", func);
      cJSON_AddItemToArray(oa_tools, func_wrapper);
    }
  }

  cJSON_Delete(anth_tools);
  return oa_tools;
}

/* ── Helper: Message conversion (Anthropic -> OpenAI) ────────── */
static cJSON *convert_anthropic_to_openai_messages(cJSON *anth_msgs) {
  if (!anth_msgs || !cJSON_IsArray(anth_msgs))
    return NULL;

  cJSON *oa_msgs = cJSON_CreateArray();
  cJSON *item;
  cJSON_ArrayForEach(item, anth_msgs) {
    cJSON *role = cJSON_GetObjectItem(item, "role");
    cJSON *content = cJSON_GetObjectItem(item, "content");

    if (!role || !cJSON_IsString(role))
      continue;

    cJSON *oa_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(oa_msg, "role", role->valuestring);

    if (cJSON_IsString(content)) {
      cJSON_AddStringToObject(oa_msg, "content", content->valuestring);
    } else if (cJSON_IsArray(content)) {
      /* Anthropic content array block */
      cJSON *block;
      cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItem(block, "type");
        if (!type || !cJSON_IsString(type))
          continue;

        if (strcmp(type->valuestring, "text") == 0) {
          cJSON *text = cJSON_GetObjectItem(block, "text");
          if (text && cJSON_IsString(text)) {
            cJSON_AddStringToObject(oa_msg, "content", text->valuestring);
          }
        } else if (strcmp(type->valuestring, "tool_use") == 0) {
          /* Convert tool_use to tool_calls array */
          cJSON *tcalls = cJSON_GetObjectItem(oa_msg, "tool_calls");
          if (!tcalls) {
            tcalls = cJSON_CreateArray();
            cJSON_AddItemToObject(oa_msg, "tool_calls", tcalls);
          }
          cJSON *tcall = cJSON_CreateObject();
          cJSON_AddStringToObject(tcall, "type", "function");
          cJSON *id = cJSON_GetObjectItem(block, "id");
          if (id)
            cJSON_AddStringToObject(tcall, "id", id->valuestring);

          cJSON *func = cJSON_CreateObject();
          cJSON *name = cJSON_GetObjectItem(block, "name");
          if (name)
            cJSON_AddStringToObject(func, "name", name->valuestring);
          cJSON *input = cJSON_GetObjectItem(block, "input");
          if (input) {
            char *args = cJSON_PrintUnformatted(input);
            cJSON_AddStringToObject(func, "arguments", args);
            free(args);
          }
          cJSON_AddItemToObject(tcall, "function", func);
          cJSON_AddItemToArray(tcalls, tcall);
        } else if (strcmp(type->valuestring, "tool_result") == 0) {
          /* Convert tool_result to role: tool */
          cJSON_ReplaceItemInObject(oa_msg, "role", cJSON_CreateString("tool"));
          cJSON *id = cJSON_GetObjectItem(block, "tool_use_id");
          if (id)
            cJSON_AddStringToObject(oa_msg, "tool_call_id", id->valuestring);
          cJSON *res_content = cJSON_GetObjectItem(block, "content");
          if (res_content)
            cJSON_AddStringToObject(oa_msg, "content",
                                    res_content->valuestring);
        }
      }
    }

    /* Strict providers (like Arcee via OpenRouter) require "content" field
     * even if null when tool_calls is present. */
    if (strcmp(role->valuestring, "assistant") == 0 &&
        !cJSON_GetObjectItem(oa_msg, "content")) {
      cJSON_AddNullToObject(oa_msg, "content");
    }

    cJSON_AddItemToArray(oa_msgs, oa_msg);
  }
  return oa_msgs;
}

/* ── Helper: Parse pseudo-tag tool calls [name(args)] or [name] ── */
static void parse_pseudotag_tool_calls(llm_response_t *resp) {
  if (!resp->text || resp->text_len == 0)
    return;

  const char *p = resp->text;
  while (resp->call_count < MIMI_MAX_TOOL_CALLS) {
    const char *start = strchr(p, '[');
    if (!start)
      break;
    const char *end = strchr(start, ']');
    if (!end)
      break;

    /* Check if it's strictly [NAME] or [NAME(ARGS)] */
    if (end - start < 3) {
      p = end + 1;
      continue;
    }

    llm_tool_call_t *call = &resp->calls[resp->call_count];

    const char *lp = strchr(start, '(');
    if (lp && lp < end) {
      /* name(args) */
      size_t nlen = lp - start - 1;
      char full_name[64] = {0};
      if (nlen >= sizeof(full_name))
        nlen = sizeof(full_name) - 1;
      strncpy(full_name, start + 1, nlen);

      const char *n = full_name;
      if (strncmp(n, "functions.", 10) == 0)
        n += 10;
      char *colon = strchr(n, ':');
      if (colon)
        *colon = '\0';
      strncpy(call->name, n, sizeof(call->name) - 1);

      const char *rp = strchr(lp, ')');
      if (rp && rp <= end) {
        size_t alen = rp - lp - 1;
        call->input = calloc(1, alen + 1);
        if (call->input) {
          memcpy(call->input, lp + 1, alen);
          call->input_len = alen;
        }
      }
    } else {
      /* [name] */
      size_t nlen = end - start - 1;
      char full_name[64] = {0};
      if (nlen >= sizeof(full_name))
        nlen = sizeof(full_name) - 1;
      strncpy(full_name, start + 1, nlen);

      const char *n = full_name;
      if (strncmp(n, "functions.", 10) == 0)
        n += 10;
      char *colon = strchr(n, ':');
      if (colon)
        *colon = '\0';
      strncpy(call->name, n, sizeof(call->name) - 1);

      call->input = strdup("{}");
      call->input_len = 2;
    }

    snprintf(call->id, sizeof(call->id), "pseudo_%d", resp->call_count);
    resp->call_count++;
    resp->tool_use = true;
    p = end + 1;
  }
}

/* ── Helper: Parse tag-based tool calls (Arcee / Trinity style) ── */
/* Detects
 * <|tool_call_begin|>name<|tool_call_argument_begin|>json<|tool_call_end|> */
static void parse_tag_based_tool_calls(llm_response_t *resp) {
  if (!resp->text || resp->text_len == 0)
    return;

  const char *p = resp->text;
  while (resp->call_count < MIMI_MAX_TOOL_CALLS) {
    const char *start_tag = "<|tool_call_begin|>";
    const char *arg_tag = "<|tool_call_argument_begin|>";
    const char *end_tag = "<|tool_call_end|>";

    const char *start = strstr(p, start_tag);
    if (!start)
      break;

    const char *name_start = start + strlen(start_tag);
    const char *arg_start = strstr(name_start, arg_tag);
    if (!arg_start)
      break;

    const char *arg_val_start = arg_start + strlen(arg_tag);
    const char *end = strstr(arg_val_start, end_tag);
    if (!end)
      break;

    llm_tool_call_t *call = &resp->calls[resp->call_count];

    /* Parse Name (ignore functions. prefix and :N suffix) */
    char full_name[64] = {0};
    size_t nlen = arg_start - name_start;
    if (nlen > sizeof(full_name) - 1)
      nlen = sizeof(full_name) - 1;
    strncpy(full_name, name_start, nlen);

    const char *n = full_name;
    if (strncmp(n, "functions.", 10) == 0)
      n += 10;
    char *colon = strchr(n, ':');
    if (colon)
      *colon = '\0';
    strncpy(call->name, n, sizeof(call->name) - 1);

    /* Parse Arguments */
    size_t alen = end - arg_val_start;
    call->input = calloc(1, alen + 1);
    if (call->input) {
      memcpy(call->input, arg_val_start, alen);
      call->input_len = alen;
    }

    /* Assign a dummy ID if missing */
    snprintf(call->id, sizeof(call->id), "tag_%d", resp->call_count);

    resp->call_count++;
    resp->tool_use = true;
    p = end + strlen(end_tag);
  }
}

/* ── Helper: Strip reasoning tags (DeepSeek-R1 style) ────────── */
static void strip_reasoning(char *s) {
  if (!s)
    return;
  char *start;
  while ((start = strstr(s, "<think>")) != NULL) {
    char *end = strstr(start, "</think>");
    if (end) {
      end += 8; /* length of </think> */
      memmove(start, end, strlen(end) + 1);
    } else {
      /* Unclosed tag - just truncate for safety or leave as is?
         Let's just remove the start tag to avoid mess. */
      memmove(start, start + 7, strlen(start + 7) + 1);
    }
  }
}

/* ── Public: chat with tools (non-streaming) ──────────────────── */

void llm_response_free(llm_response_t *resp) {
  free(resp->text);
  resp->text = NULL;
  resp->text_len = 0;
  for (int i = 0; i < resp->call_count; i++) {
    free(resp->calls[i].input);
    resp->calls[i].input = NULL;
  }
  resp->call_count = 0;
  resp->tool_use = false;
}

esp_err_t llm_chat_tools(const char *system_prompt, cJSON *messages,
                         const char *tools_json, llm_response_t *resp) {
  memset(resp, 0, sizeof(*resp));

  if (s_api_key[0] == '\0')
    return ESP_ERR_INVALID_STATE;

  /* Build request body (non-streaming) */
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "model", s_model);
  cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);

  /* Provider is now explicit (no auto-detection) */
  int provider = s_provider;

  if (provider == MIMI_LLM_PROVIDER_ANTHROPIC) {
    cJSON_AddStringToObject(body, "system", system_prompt);
    cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
    cJSON_AddItemToObject(body, "messages", msgs_copy);

    if (tools_json) {
      cJSON *tools = cJSON_Parse(tools_json);
      if (tools)
        cJSON_AddItemToObject(body, "tools", tools);
    }
  } else {
    /* OpenAI Tools format */
    cJSON *messages_arr = cJSON_CreateArray();

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages_arr, sys_msg);

    if (messages) {
      cJSON *oa_msgs = convert_anthropic_to_openai_messages(messages);
      if (oa_msgs) {
        cJSON *item;
        cJSON_ArrayForEach(item, oa_msgs) {
          cJSON_AddItemToArray(messages_arr, cJSON_Duplicate(item, 1));
        }
        cJSON_Delete(oa_msgs);
      }
    }
    cJSON_AddItemToObject(body, "messages", messages_arr);
    cJSON_AddStringToObject(body, "tool_choice", "auto");
    /* OpenAI Tools format conversion */
    if (tools_json) {
      cJSON *oa_tools = convert_anthropic_to_openai_tools(tools_json);
      if (oa_tools) {
        cJSON_AddItemToObject(body, "tools", oa_tools);
      }
    }
  }

  char *post_data = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (!post_data)
    return ESP_ERR_NO_MEM;

  ESP_LOGI(TAG, "[LLM_CALL] provider=%s, model=%s, body=%d bytes, tools=%d",
           (provider == MIMI_LLM_PROVIDER_ANTHROPIC) ? "Anthropic"
                                                     : "OpenAI/Compatible",
           s_model, (int)strlen(post_data),
           tools_json ? cJSON_GetArraySize(cJSON_Parse(tools_json)) : 0);

  /* HTTP call */
  resp_buf_t rb;
  if (resp_buf_init(&rb, MIMI_LLM_STREAM_BUF_SIZE) != ESP_OK) {
    free(post_data);
    return ESP_ERR_NO_MEM;
  }

  int status = 0;
  esp_err_t err = llm_http_call(post_data, &rb, &status);
  free(post_data);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    resp_buf_free(&rb);
    return err;
  }

  /* Raw response logging */
  /* Log cleaned up in unified block later */

  if (status != 200) {
    ESP_LOGE(TAG, "API error %d: %.500s", status, rb.data ? rb.data : "");
    resp_buf_free(&rb);
    return ESP_FAIL;
  }

  /* Parse full JSON response */
  cJSON *root = cJSON_Parse(rb.data);
  resp_buf_free(&rb);

  if (!root) {
    ESP_LOGE(TAG, "Failed to parse API response JSON");
    return ESP_FAIL;
  }

  /* stop_reason */
  cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
  if (stop_reason && cJSON_IsString(stop_reason)) {
    resp->tool_use = (strcmp(stop_reason->valuestring, "tool_use") == 0);
  }

  /* Iterate content blocks */
  cJSON *content = cJSON_GetObjectItem(root, "content");
  if (content && cJSON_IsArray(content)) {
    /* Accumulate total text length first */
    size_t total_text = 0;
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
      cJSON *btype = cJSON_GetObjectItem(block, "type");
      if (btype && strcmp(btype->valuestring, "text") == 0) {
        cJSON *text = cJSON_GetObjectItem(block, "text");
        if (text && cJSON_IsString(text)) {
          total_text += strlen(text->valuestring);
        }
      }
    }

    /* Allocate and copy text */
    if (total_text > 0) {
      resp->text = calloc(1, total_text + 1);
      if (resp->text) {
        cJSON_ArrayForEach(block, content) {
          cJSON *btype = cJSON_GetObjectItem(block, "type");
          if (!btype || strcmp(btype->valuestring, "text") != 0)
            continue;
          cJSON *text = cJSON_GetObjectItem(block, "text");
          if (!text || !cJSON_IsString(text))
            continue;
          size_t tlen = strlen(text->valuestring);
          memcpy(resp->text + resp->text_len, text->valuestring, tlen);
          resp->text_len += tlen;
        }
        resp->text[resp->text_len] = '\0';
      }
    }

    /* Extract tool_use blocks */
    cJSON_ArrayForEach(block, content) {
      cJSON *btype = cJSON_GetObjectItem(block, "type");
      if (!btype || strcmp(btype->valuestring, "tool_use") != 0)
        continue;
      if (resp->call_count >= MIMI_MAX_TOOL_CALLS)
        break;

      llm_tool_call_t *call = &resp->calls[resp->call_count];

      cJSON *id = cJSON_GetObjectItem(block, "id");
      if (id && cJSON_IsString(id)) {
        strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
      }

      cJSON *name = cJSON_GetObjectItem(block, "name");
      if (name && cJSON_IsString(name)) {
        strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
      }

      cJSON *input = cJSON_GetObjectItem(block, "input");
      if (input) {
        char *input_str = cJSON_PrintUnformatted(input);
        if (input_str) {
          call->input = input_str;
          call->input_len = strlen(input_str);
        }
      }

      resp->call_count++;
    }
  } else {
    /* Try OpenAI style fallback (choices[0].message.content) */
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices && cJSON_IsArray(choices)) {
      cJSON *first = cJSON_GetArrayItem(choices, 0);
      if (first) {
        cJSON *msg = cJSON_GetObjectItem(first, "message");
        if (msg) {
          /* Content (Text) */
          cJSON *text = cJSON_GetObjectItem(msg, "content");
          if (text && cJSON_IsString(text)) {
            resp->text = strdup(text->valuestring);
            if (resp->text) {
              resp->text_len = strlen(resp->text);
            }
          }

          /* Tool calls */
          cJSON *tcalls = cJSON_GetObjectItem(msg, "tool_calls");
          if (tcalls && cJSON_IsArray(tcalls)) {
            resp->tool_use = true;
            cJSON *tc;
            cJSON_ArrayForEach(tc, tcalls) {
              if (resp->call_count >= MIMI_MAX_TOOL_CALLS)
                break;
              llm_tool_call_t *call = &resp->calls[resp->call_count];

              cJSON *id = cJSON_GetObjectItem(tc, "id");
              if (id && cJSON_IsString(id)) {
                strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
              }

              cJSON *func = cJSON_GetObjectItem(tc, "function");
              if (func) {
                cJSON *fname = cJSON_GetObjectItem(func, "name");
                if (fname && cJSON_IsString(fname)) {
                  strncpy(call->name, fname->valuestring,
                          sizeof(call->name) - 1);
                }
                cJSON *fargs = cJSON_GetObjectItem(func, "arguments");
                if (fargs && cJSON_IsString(fargs)) {
                  call->input = strdup(fargs->valuestring);
                  call->input_len = strlen(call->input);
                }
              }
              resp->call_count++;
            }
          }
        }

        /* finish_reason indicator for tool_use */
        cJSON *freason = cJSON_GetObjectItem(first, "finish_reason");
        if (freason && cJSON_IsString(freason)) {
          if (strcmp(freason->valuestring, "tool_calls") == 0) {
            resp->tool_use = true;
          }
        }
      }
    }
  }

  /* Post-process: Strip reasoning tags and trim */
  if (resp->text) {
    strip_reasoning(resp->text);
    trim_inplace(resp->text);
    resp->text_len = strlen(resp->text);
  }

  /* Always log first 500 chars of raw response while debugging */
  const char *raw_ptr = rb.data ? rb.data : "NULL";
  if (rb.data) {
    while (*raw_ptr && (unsigned char)*raw_ptr <= 32) {
      raw_ptr++;
    }
  }
  ESP_LOGI(TAG, "[LLM_RAW] %.*s", 500, raw_ptr);

  if (resp->call_count == 0) {
    parse_tag_based_tool_calls(resp);
    if (resp->call_count == 0) {
      parse_pseudotag_tool_calls(resp);
    }
  }

  cJSON_Delete(root);
  resp_buf_free(&rb);

  ESP_LOGI(TAG, "Response: %d bytes text, %d tool calls, stop=%s",
           (int)resp->text_len, resp->call_count,
           resp->tool_use ? "tool_use" : "end_turn");

  return ESP_OK;
}

/* ── NVS helpers ──────────────────────────────────────────────── */

esp_err_t llm_set_api_key(const char *api_key) {
  nvs_handle_t nvs;
  ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
  ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
  ESP_ERROR_CHECK(nvs_commit(nvs));
  nvs_close(nvs);

  strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
  trim_inplace(s_api_key);
  ESP_LOGI(TAG, "API key saved");
  return ESP_OK;
}

esp_err_t llm_set_model(const char *model) {
  nvs_handle_t nvs;
  ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
  ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_MODEL, model));
  ESP_ERROR_CHECK(nvs_commit(nvs));
  nvs_close(nvs);

  strncpy(s_model, model, sizeof(s_model) - 1);
  trim_inplace(s_model);
  ESP_LOGI(TAG, "Model set to: %s", s_model);
  return ESP_OK;
}

esp_err_t llm_set_provider(int provider) {
  nvs_handle_t nvs;
  ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
  ESP_ERROR_CHECK(nvs_set_i32(nvs, MIMI_NVS_KEY_PROVIDER, provider));
  ESP_ERROR_CHECK(nvs_commit(nvs));
  nvs_close(nvs);

  s_provider = provider;

  /* Update default URL if not manually set */
  if (s_base_url[0] == '\0') {
    if (s_provider == MIMI_LLM_PROVIDER_ANTHROPIC) {
      strcpy(s_base_url, MIMI_LLM_API_URL_ANTHROPIC);
    } else {
      strcpy(s_base_url, MIMI_LLM_API_URL_OPENAI);
    }
  }

  ESP_LOGI(TAG, "Provider set to: %d", s_provider);
  return ESP_OK;
}

esp_err_t llm_set_base_url(const char *url) {
  nvs_handle_t nvs;
  ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
  ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_BASE_URL, url));
  ESP_ERROR_CHECK(nvs_commit(nvs));
  nvs_close(nvs);

  strncpy(s_base_url, url, sizeof(s_base_url) - 1);
  trim_inplace(s_base_url);
  ESP_LOGI(TAG, "Base URL set to: %s", s_base_url);
  return ESP_OK;
}

esp_err_t llm_set_timezone(const char *tz) {
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs) != ESP_OK) {
    return ESP_FAIL;
  }
  nvs_set_str(nvs, MIMI_NVS_KEY_TIMEZONE, tz);
  nvs_commit(nvs);
  nvs_close(nvs);

  ESP_LOGI(TAG, "Timezone saved: %s", tz);
  return ESP_OK;
}
