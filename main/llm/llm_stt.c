#include "llm_stt.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mimi_config.h"
#include "nvs.h"
#include "proxy/http_proxy.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "stt";
static const char *STT_API_PATH = MIMI_STT_TRANSCRIBE_PATH;
static const char *STT_BOUNDARY = "----MimiSttBoundary7MA4YWxkTrZu0gW";
static char s_stt_key[128] = {0};
static char s_stt_model[64] = MIMI_STT_DEFAULT_MODEL;
static char s_stt_base_url[160] = {0};
static int s_stt_provider = MIMI_STT_DEFAULT_PROVIDER;

static void trim_inplace(char *s) {
  if (!s || s[0] == '\0')
    return;
  char *end = s + strlen(s) - 1;
  while (end >= s && ((unsigned char)*end <= 32)) {
    *end-- = '\0';
  }
}

static const char *stt_provider_name(int provider) {
  if (provider == MIMI_STT_PROVIDER_GROQ)
    return "groq";
  return "unknown";
}

static esp_err_t stt_config_set_provider(int provider) {
  if (provider != MIMI_STT_PROVIDER_GROQ) {
    return ESP_ERR_INVALID_ARG;
  }
  s_stt_provider = provider;
  return ESP_OK;
}

static esp_err_t stt_build_endpoint_url(char *out, size_t out_size) {
  if (out_size == 0 || !out || !s_stt_base_url[0])
    return ESP_ERR_INVALID_ARG;

  if (strstr(s_stt_base_url, STT_API_PATH) != NULL) {
    snprintf(out, out_size, "%s", s_stt_base_url);
    return ESP_OK;
  }

  size_t base_len = strlen(s_stt_base_url);
  while (base_len > 0 && s_stt_base_url[base_len - 1] == '/') {
    base_len--;
  }

  const char *path = STT_API_PATH;
  while (*path == '/') {
    path++;
  }

  size_t need;
  need = base_len + 1 + strlen(path) + 1;
  if (need > out_size)
    return ESP_ERR_NO_MEM;

  snprintf(out, out_size, "%.*s/%s", (int)base_len, s_stt_base_url, path);
  return ESP_OK;
}

esp_err_t llm_stt_init(void) {
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(s_stt_base_url);
    nvs_get_str(nvs, MIMI_NVS_KEY_STT_BASE_URL, s_stt_base_url, &len);
    memset(s_stt_model, 0, sizeof(s_stt_model));
    len = sizeof(s_stt_key);
    nvs_get_str(nvs, MIMI_NVS_KEY_STT_KEY, s_stt_key, &len);
    len = sizeof(s_stt_model);
    nvs_get_str(nvs, MIMI_NVS_KEY_STT_MODEL, s_stt_model, &len);
    int32_t provider = MIMI_STT_DEFAULT_PROVIDER;
    if (nvs_get_i32(nvs, MIMI_NVS_KEY_STT_PROVIDER, &provider) == ESP_OK) {
      stt_config_set_provider((int)provider);
    }
    if (s_stt_key[0] == '\0') {
      len = sizeof(s_stt_key);
      nvs_get_str(nvs, "groq_key", s_stt_key, &len); // legacy compatibility
    }
    nvs_close(nvs);
  }

  if (s_stt_key[0] == '\0' && MIMI_SECRET_STT_KEY[0] != '\0') {
    strncpy(s_stt_key, MIMI_SECRET_STT_KEY, sizeof(s_stt_key) - 1);
  }
  if (s_stt_base_url[0] == '\0' && MIMI_SECRET_STT_BASE_URL[0] != '\0') {
    strncpy(s_stt_base_url, MIMI_SECRET_STT_BASE_URL, sizeof(s_stt_base_url) - 1);
  }
  if (s_stt_model[0] == '\0' && MIMI_SECRET_STT_MODEL[0] != '\0') {
    strncpy(s_stt_model, MIMI_SECRET_STT_MODEL, sizeof(s_stt_model) - 1);
  }

  trim_inplace(s_stt_key);
  trim_inplace(s_stt_model);
  trim_inplace(s_stt_base_url);

  if (s_stt_base_url[0] == '\0') {
    snprintf(s_stt_base_url, sizeof(s_stt_base_url), "%s",
             MIMI_SECRET_STT_BASE_URL);
  }

  ESP_LOGI(TAG, "STT initialized: provider=%s, model=%s", stt_provider_name(s_stt_provider), s_stt_model);
  ESP_LOGI(TAG, "STT API URL: %s", s_stt_base_url);
  return ESP_OK;
}

esp_err_t llm_stt_set_key(const char *key) {
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_str(nvs, MIMI_NVS_KEY_STT_KEY, key);
    nvs_set_str(nvs, "groq_key", key); // keep old key for compatibility
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  memset(s_stt_key, 0, sizeof(s_stt_key));
  strncpy(s_stt_key, key, sizeof(s_stt_key) - 1);
  return ESP_OK;
}

esp_err_t llm_stt_set_model(const char *model) {
  if (!model || !model[0]) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_str(nvs, MIMI_NVS_KEY_STT_MODEL, model);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  memset(s_stt_model, 0, sizeof(s_stt_model));
  strncpy(s_stt_model, model, sizeof(s_stt_model) - 1);
  return ESP_OK;
}

esp_err_t llm_stt_set_base_url(const char *url) {
  if (!url || !url[0]) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_str(nvs, MIMI_NVS_KEY_STT_BASE_URL, url);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  memset(s_stt_base_url, 0, sizeof(s_stt_base_url));
  strncpy(s_stt_base_url, url, sizeof(s_stt_base_url) - 1);
  return ESP_OK;
}

esp_err_t llm_stt_set_provider(int provider) {
  if (stt_config_set_provider(provider) != ESP_OK) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_i32(nvs, MIMI_NVS_KEY_STT_PROVIDER, provider);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  return ESP_OK;
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} stt_resp_t;

struct llm_stt_stream {
  esp_http_client_handle_t client;
  size_t expected;
  size_t sent;
  char *footer;
  size_t footer_len;
  char endpoint[192];
  stt_resp_t resp;
};

static esp_err_t stt_response_append(stt_resp_t *resp, const uint8_t *data,
                                    size_t len) {
  if (!resp || (!data && len > 0))
    return ESP_ERR_INVALID_ARG;

  if (resp->len + len + 1 > resp->cap) {
    size_t need = resp->len + len + 1;
    size_t new_cap = resp->cap ? resp->cap : 64;
    while (new_cap < need) {
      new_cap *= 2;
    }

    char *tmp = heap_caps_realloc(resp->buf, new_cap, MALLOC_CAP_SPIRAM);
    if (!tmp)
      return ESP_ERR_NO_MEM;
    resp->buf = tmp;
    resp->cap = new_cap;
  }

  if (len > 0) {
    memcpy(resp->buf + resp->len, data, len);
    resp->len += len;
    resp->buf[resp->len] = '\0';
  }
  return ESP_OK;
}

static esp_err_t stt_read_response_all(esp_http_client_handle_t client,
                                      stt_resp_t *resp) {
  if (!client || !resp || !resp->buf)
    return ESP_ERR_INVALID_ARG;

  uint8_t chunk[256];
  while (1) {
    int rlen = esp_http_client_read_response(client, (char *)chunk,
                                            (int)sizeof(chunk));
    if (rlen < 0) {
      return ESP_FAIL;
    }
    if (rlen == 0) {
      break;
    }
    esp_err_t err = stt_response_append(resp, chunk, (size_t)rlen);
    if (err != ESP_OK)
      return err;
  }

  return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  stt_resp_t *resp = (stt_resp_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (resp->len + evt->data_len >= resp->cap) {
      size_t new_cap = resp->cap * 2;
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

static esp_err_t http_write_all(esp_http_client_handle_t client,
                                const uint8_t *data, size_t len) {
  size_t written = 0;
  while (written < len) {
    int w = esp_http_client_write(client, (const char *)data + written,
                                  len - written);
    if (w <= 0)
      return ESP_FAIL;
    written += (size_t)w;
  }
  return ESP_OK;
}

static void llm_stt_stream_cleanup(llm_stt_stream_t *stream) {
  if (!stream)
    return;
  if (stream->client)
    esp_http_client_cleanup(stream->client);
  free(stream->footer);
  free(stream->resp.buf);
  free(stream);
}

esp_err_t llm_stt_transcribe(const uint8_t *audio_data, size_t audio_len,
                             char *out_text, size_t max_out) {
  if (s_stt_key[0] == '\0') {
    return ESP_ERR_INVALID_STATE;
  }

  char url[192];
  if (stt_build_endpoint_url(url, sizeof(url)) != ESP_OK) {
    return ESP_ERR_INVALID_STATE;
  }
  const char *boundary = STT_BOUNDARY;

  /* Build multipart/form-data manually */
int hlen =
      snprintf(NULL, 0,
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"file\"; "
                      "filename=\"voice.ogg\"\r\n"
                      "Content-Type: audio/ogg; codecs=opus\r\n\r\n",
                      boundary);
  if (hlen < 0)
    return ESP_ERR_INVALID_ARG;
  size_t header_len = (size_t)hlen;
  char *header_part = malloc(header_len + 1);
  if (!header_part)
    return ESP_ERR_NO_MEM;
  snprintf(header_part, header_len + 1,
           "--%s\r\n"
           "Content-Disposition: form-data; name=\"file\"; "
           "filename=\"voice.ogg\"\r\n"
           "Content-Type: audio/ogg; codecs=opus\r\n\r\n",
           boundary);

  int flen =
      snprintf(NULL, 0,
               "\r\n--%s\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
               "%s\r\n"
               "--%s--\r\n",
               boundary, s_stt_model, boundary);
  if (flen < 0) {
    free(header_part);
    return ESP_ERR_INVALID_ARG;
  }
  size_t footer_len = (size_t)flen;
  char *footer_part = malloc(footer_len + 1);
  if (!footer_part) {
    free(header_part);
    return ESP_ERR_NO_MEM;
  }
  snprintf(footer_part, footer_len + 1,
           "\r\n--%s\r\n"
           "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
           "%s\r\n"
           "--%s--\r\n",
           boundary, s_stt_model, boundary);

  size_t total_len = header_len + audio_len + footer_len;
  char *body = heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM);
  if (!body) {
    free(header_part);
    free(footer_part);
    return ESP_ERR_NO_MEM;
  }

  memcpy(body, header_part, hlen);
  memcpy(body + hlen, audio_data, audio_len);
  memcpy(body + hlen + audio_len, footer_part, flen);
  free(header_part);
  free(footer_part);

  stt_resp_t resp = {.buf = heap_caps_calloc(1, 1024, MALLOC_CAP_SPIRAM),
                     .len = 0,
                     .cap = 1024};

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = http_event_handler,
      .user_data = &resp,
      .timeout_ms = 30000,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  if (http_proxy_is_enabled()) {
    config.host = http_proxy_get_host();
    config.port = http_proxy_get_port();
    config.transport_type = HTTP_TRANSPORT_OVER_TCP;
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_method(client, HTTP_METHOD_POST);

  char ctypes[128];
  snprintf(ctypes, sizeof(ctypes), "multipart/form-data; boundary=%s",
           boundary);
  esp_http_client_set_header(client, "Content-Type", ctypes);

  char auth[192];
  snprintf(auth, sizeof(auth), "Bearer %s", s_stt_key);
  esp_http_client_set_header(client, "Authorization", auth);

  esp_http_client_set_post_field(client, body, total_len);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "STT request failed: err=%s status=%d", esp_err_to_name(err),
             status);
    free(body);
    free(resp.buf);
    return ESP_FAIL;
  }
  free(body);

  if (status != 200) {
    ESP_LOGE(TAG, "STT failed: status=%d, response=%.240s", status, resp.buf);
    free(resp.buf);
    return ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(resp.buf);
  if (root) {
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text)) {
      strncpy(out_text, text->valuestring, max_out - 1);
      out_text[max_out - 1] = '\0';
      err = ESP_OK;
    } else {
      ESP_LOGE(TAG, "STT parse error: no text field in %s", resp.buf);
      err = ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(root);
  } else {
    ESP_LOGE(TAG, "STT parse error: %s", resp.buf);
    err = ESP_ERR_INVALID_RESPONSE;
  }

  if (err != ESP_OK) {
    free(resp.buf);
    return err;
  }

  free(resp.buf);
  return ESP_OK;
}

esp_err_t llm_stt_stream_begin(llm_stt_stream_t **handle, size_t audio_size) {
  if (!handle)
    return ESP_ERR_INVALID_ARG;
  if (s_stt_key[0] == '\0')
    return ESP_ERR_INVALID_STATE;

  llm_stt_stream_t *stream = calloc(1, sizeof(*stream));
  if (!stream)
    return ESP_ERR_NO_MEM;

  stream->resp.buf = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM);
  if (!stream->resp.buf) {
    llm_stt_stream_cleanup(stream);
    return ESP_ERR_NO_MEM;
  }
  stream->resp.cap = 4096;

  int hlen =
      snprintf(NULL, 0,
               "--%s\r\n"
               "Content-Disposition: form-data; name=\"file\"; "
               "filename=\"voice.ogg\"\r\n"
               "Content-Type: audio/ogg; codecs=opus\r\n\r\n",
               STT_BOUNDARY);
  if (hlen < 0)
    return ESP_ERR_INVALID_ARG;
  size_t header_len = (size_t)hlen;
  char *header = malloc(header_len + 1);
  if (!header) {
    llm_stt_stream_cleanup(stream);
    return ESP_ERR_NO_MEM;
  }
  snprintf(header, header_len + 1,
           "--%s\r\n"
           "Content-Disposition: form-data; name=\"file\"; "
           "filename=\"voice.ogg\"\r\n"
           "Content-Type: audio/ogg; codecs=opus\r\n\r\n",
           STT_BOUNDARY);

  int footer_len =
      snprintf(NULL, 0,
               "\r\n--%s\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
               "%s\r\n"
               "--%s--\r\n",
               STT_BOUNDARY, s_stt_model, STT_BOUNDARY);
  if (footer_len < 0) {
    free(header);
    llm_stt_stream_cleanup(stream);
    return ESP_ERR_INVALID_ARG;
  }
  stream->footer_len = (size_t)footer_len;
  stream->footer = malloc(stream->footer_len + 1);
  if (!stream->footer) {
    free(header);
    llm_stt_stream_cleanup(stream);
    return ESP_ERR_NO_MEM;
  }
  snprintf(stream->footer, stream->footer_len + 1,
           "\r\n--%s\r\n"
           "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
           "%s\r\n"
           "--%s--\r\n",
           STT_BOUNDARY, s_stt_model, STT_BOUNDARY);

  char endpoint[192];
  if (stt_build_endpoint_url(endpoint, sizeof(endpoint)) != ESP_OK) {
    free(header);
    llm_stt_stream_cleanup(stream);
    return ESP_ERR_INVALID_STATE;
  }
  snprintf(stream->endpoint, sizeof(stream->endpoint), "%s", endpoint);
  esp_http_client_config_t config = {
      .url = endpoint,
      .event_handler = http_event_handler,
      .user_data = &stream->resp,
      .timeout_ms = 30000,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  if (http_proxy_is_enabled()) {
    config.host = http_proxy_get_host();
    config.port = http_proxy_get_port();
    config.transport_type = HTTP_TRANSPORT_OVER_TCP;
  }

  stream->client = esp_http_client_init(&config);
  if (!stream->client) {
    free(header);
    llm_stt_stream_cleanup(stream);
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_method(stream->client, HTTP_METHOD_POST);

  char ctypes[128];
  snprintf(ctypes, sizeof(ctypes), "multipart/form-data; boundary=%s",
           STT_BOUNDARY);
  esp_http_client_set_header(stream->client, "Content-Type", ctypes);

  char auth[192];
  snprintf(auth, sizeof(auth), "Bearer %s", s_stt_key);
  esp_http_client_set_header(stream->client, "Authorization", auth);

  size_t total_len = header_len + audio_size + stream->footer_len;
  esp_err_t err = esp_http_client_open(stream->client, (int)total_len);
  if (err != ESP_OK) {
    free(header);
    llm_stt_stream_cleanup(stream);
    return err;
  }

  err = http_write_all(stream->client, (const uint8_t *)header, (size_t)hlen);
  free(header);
  if (err != ESP_OK) {
    llm_stt_stream_cleanup(stream);
    return err;
  }

  stream->expected = audio_size;
  stream->sent = 0;
  ESP_LOGI(TAG, "STT stream begin: endpoint=%s model=%s audio_size=%d total_len=%d",
           stream->endpoint, s_stt_model, (int)stream->expected,
           (int)total_len);
  *handle = stream;
  return ESP_OK;
}

esp_err_t llm_stt_stream_write(llm_stt_stream_t *handle, const uint8_t *data,
                               size_t len) {
  if (!handle || !data)
    return ESP_ERR_INVALID_ARG;
  if (handle->sent + len > handle->expected)
    return ESP_ERR_INVALID_SIZE;

  esp_err_t err = http_write_all(handle->client, data, len);
  if (err == ESP_OK)
    handle->sent += len;
  return err;
}

esp_err_t llm_stt_stream_complete(llm_stt_stream_t *handle, char *out_text,
                                  size_t max_out) {
  if (!handle)
    return ESP_ERR_INVALID_ARG;

  if (handle->sent != handle->expected) {
    ESP_LOGW(TAG, "STT stream byte mismatch: sent=%d expected=%d",
             (int)handle->sent, (int)handle->expected);
    llm_stt_stream_cleanup(handle);
    return ESP_ERR_INVALID_SIZE;
  }

  esp_err_t err = http_write_all(handle->client, (const uint8_t *)handle->footer,
                                 handle->footer_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "STT stream footer write failed: %s", esp_err_to_name(err));
    llm_stt_stream_cleanup(handle);
    return err;
  }

  err = esp_http_client_fetch_headers(handle->client);
  int status = esp_http_client_get_status_code(handle->client);
  int clen = esp_http_client_get_content_length(handle->client);
  if (err == ESP_FAIL && status == 200) {
    ESP_LOGW(TAG,
             "STT stream fetch headers returned %s for 200 response; continue read",
             esp_err_to_name(err));
  } else if (err != ESP_OK && err != ESP_ERR_HTTP_EAGAIN) {
    ESP_LOGE(TAG,
             "STT stream fetch headers failed: %s status=%d content_length=%d endpoint=%s",
             esp_err_to_name(err), status, clen,
             handle->endpoint[0] ? handle->endpoint : "n/a");
    llm_stt_stream_cleanup(handle);
    return err;
  }

  esp_err_t read_err = stt_read_response_all(handle->client, &handle->resp);
  if (read_err != ESP_OK) {
    ESP_LOGE(TAG, "STT stream response read failed: %s status=%d content_length=%d endpoint=%s",
             esp_err_to_name(read_err), status, clen,
             handle->endpoint[0] ? handle->endpoint : "n/a");
    llm_stt_stream_cleanup(handle);
    return read_err;
  }

  if (status != 200) {
    ESP_LOGE(TAG, "STT stream failed: status=%d, response=%.240s", status,
             handle->resp.buf);
    llm_stt_stream_cleanup(handle);
    return ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(handle->resp.buf);
  if (!root) {
    ESP_LOGE(TAG, "STT stream parse failed: %.240s", handle->resp.buf);
    llm_stt_stream_cleanup(handle);
    return ESP_ERR_INVALID_RESPONSE;
  }

  cJSON *text = cJSON_GetObjectItem(root, "text");
  if (cJSON_IsString(text)) {
    strncpy(out_text, text->valuestring, max_out - 1);
    out_text[max_out - 1] = '\0';
    err = ESP_OK;
  } else {
    ESP_LOGW(TAG, "STT stream response missing text field: %s",
             handle->resp.buf);
    err = ESP_FAIL;
  }
  cJSON_Delete(root);
  llm_stt_stream_cleanup(handle);
  return err;
}

void llm_stt_stream_abort(llm_stt_stream_t *handle) {
  llm_stt_stream_cleanup(handle);
}
