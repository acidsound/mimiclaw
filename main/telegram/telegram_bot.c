#include "telegram_bot.h"
#include "bus/message_bus.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "telegram";

static char s_bot_token[128] = MIMI_SECRET_TG_TOKEN;
static int64_t s_update_offset = 0;

#ifndef MIMI_BREAK_GLASS_ADMIN_ID
#define MIMI_BREAK_GLASS_ADMIN_ID 0
#endif

static bool is_chat_authorized(int64_t chat_id) {
  if (chat_id == MIMI_BREAK_GLASS_ADMIN_ID)
    return true;

  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) == ESP_OK) {
    char key[32];
    snprintf(key, sizeof(key), "a%" PRId64, chat_id); // NVS keys max 15 chars
    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(nvs, key, &val);
    nvs_close(nvs);
    if (err == ESP_OK && val == 1)
      return true;
  }
  return false;
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

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static char *tg_api_call_via_proxy(const char *path, const char *post_data) {
  proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443,
                                       (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000);
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

  int timeout = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000;
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

static char *tg_api_call_direct(const char *method, const char *post_data) {
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
      .timeout_ms = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000,
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
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    free(resp.buf);
    return NULL;
  }

  return resp.buf;
}

static char *tg_api_call(const char *method, const char *post_data) {
  if (http_proxy_is_enabled()) {
    return tg_api_call_via_proxy(method, post_data);
  }
  return tg_api_call_direct(method, post_data);
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
  BaseType_t ret =
      xTaskCreatePinnedToCore(telegram_poll_task, "tg_poll", MIMI_TG_POLL_STACK,
                              NULL, MIMI_TG_POLL_PRIO, NULL, MIMI_TG_POLL_CORE);

  return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

static bool is_md_word_or_hyphen_char(char c) {
  return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-');
}

/* Simple Markdown to HTML converter for Telegram */
static char *tg_markdown_to_html(const char *md) {
  if (!md)
    return NULL;

  size_t in_len = strlen(md);
  /* HTML can be larger than markdown due to tags and escaping,
   * Allocate 2x buffer up front to avoid frequent reallocs */
  size_t cap = in_len * 2 + 128;
  char *out = malloc(cap);
  if (!out)
    return NULL;

  const char *src = md;
  char *dst = out;
  bool bold = false, italic = false, code = false, pre = false;

  while (*src) {
    /* Safety check for buffer space */
    if ((dst - out) + 16 >= cap) {
      size_t offset = dst - out;
      cap *= 2;
      char *tmp = realloc(out, cap);
      if (!tmp) {
        free(out);
        return NULL;
      }
      out = tmp;
      dst = out + offset;
    }

    if (!code && !pre && strncmp(src, "```", 3) == 0) {
      pre = true;
      strcpy(dst, "<pre>");
      dst += 5;
      src += 3;
    } else if (pre && strncmp(src, "```", 3) == 0) {
      pre = false;
      strcpy(dst, "</pre>");
      dst += 6;
      src += 3;
    } else if (!pre && strncmp(src, "`", 1) == 0) {
      if (!code) {
        strcpy(dst, "<code>");
        dst += 6;
      } else {
        strcpy(dst, "</code>");
        dst += 7;
      }
      code = !code;
      src += 1;
    } else if (!code && !pre &&
               (strncmp(src, "**", 2) == 0 || strncmp(src, "__", 2) == 0)) {
      if (!bold) {
        strcpy(dst, "<b>");
        dst += 3;
      } else {
        strcpy(dst, "</b>");
        dst += 4;
      }
      bold = !bold;
      src += 2;
    } else if (!code && !pre && *src == '[') {
      const char *close = strchr(src, ']');
      if (close && close > src + 1) {
        bool word = true;
        bool has_underscore = false;
        for (const char *p = src + 1; p < close; p++) {
          if (*p == '_')
            has_underscore = true;
          if (!is_md_word_or_hyphen_char(*p)) {
            word = false;
            break;
          }
        }

        if (word && has_underscore) {
          for (const char *p = src; p <= close; p++) {
            if (*p == '<') {
              strcpy(dst, "&lt;");
              dst += 4;
            } else if (*p == '>') {
              strcpy(dst, "&gt;");
              dst += 4;
            } else if (*p == '&') {
              strcpy(dst, "&amp;");
              dst += 5;
            } else {
              *dst++ = *p;
            }
          }
          src = close + 1;
          continue;
        }
      }
    } else if (!code && !pre && strncmp(src, "*", 1) == 0) {
      if (!italic) {
        strcpy(dst, "<i>");
        dst += 3;
      } else {
        strcpy(dst, "</i>");
        dst += 4;
      }
      italic = !italic;
      src += 1;
    } else if (!code && !pre && strncmp(src, "_", 1) == 0) {
      const char prev = (src == md) ? '\0' : src[-1];
      const char next = src[1];
      bool prev_word = (prev != '\0') &&
                       ((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
                        (prev >= '0' && prev <= '9') || prev == '_');
      bool next_word = (next != '\0') &&
                       ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                        (next >= '0' && next <= '9') || next == '_');

      if (prev_word && next_word) {
        *dst++ = *src++;
      } else {
        if (!italic) {
          strcpy(dst, "<i>");
          dst += 3;
        } else {
          strcpy(dst, "</i>");
          dst += 4;
        }
        italic = !italic;
        src += 1;
      }
    } else {
      /* Literal character with HTML escaping if not in code/pre */
      if (*src == '<') {
        strcpy(dst, "&lt;");
        dst += 4;
      } else if (*src == '>') {
        strcpy(dst, "&gt;");
        dst += 4;
      } else if (*src == '&') {
        strcpy(dst, "&amp;");
        dst += 5;
      } else {
        *dst++ = *src;
      }
      src++;
    }
  }

  /* Close any hanging tags */
  if (bold) {
    strcpy(dst, "</b>");
    dst += 4;
  }
  if (italic) {
    strcpy(dst, "</i>");
    dst += 4;
  }
  if (code) {
    strcpy(dst, "</code>");
    dst += 7;
  }
  if (pre) {
    strcpy(dst, "</pre>");
    dst += 6;
  }

  *dst = '\0';
  return out;
}

esp_err_t telegram_send_message(const char *chat_id, const char *text) {
  if (s_bot_token[0] == '\0') {
    ESP_LOGW(TAG, "Cannot send: no bot token");
    return ESP_ERR_INVALID_STATE;
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

    /* Convert Markdown to HTML */
    char *html_text = tg_markdown_to_html(segment);
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
      char *resp = tg_api_call("sendMessage", json_str);
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
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) != ESP_OK)
    return ESP_FAIL;
  char key[16];
  snprintf(key, sizeof(key), "a%" PRId64, chat_id);
  nvs_set_u8(nvs, key, 1);
  nvs_commit(nvs);
  nvs_close(nvs);
  ESP_LOGI(TAG, "Authorized chat %" PRId64, chat_id);
  return ESP_OK;
}

esp_err_t telegram_auth_remove(int64_t chat_id) {
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) != ESP_OK)
    return ESP_FAIL;
  char key[16];
  snprintf(key, sizeof(key), "a%" PRId64, chat_id);
  nvs_erase_key(nvs, key);
  nvs_commit(nvs);
  nvs_close(nvs);
  ESP_LOGI(TAG, "Deauthorized chat %" PRId64, chat_id);
  return ESP_OK;
}

void telegram_auth_list(void) {
  printf("Authorized Telegram Chats (NVS):\n");
  /* Simple list via iterator if supported, or just print break-glass */
  printf("  [Static] Break-glass Admin: %" PRId64 "\n",
         (int64_t)MIMI_BREAK_GLASS_ADMIN_ID);
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
