#include "tools/tool_http.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mimi_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tool_http";

#define HTTP_MAX_RESPONSE_SIZE (8 * 1024)

/* ── Secret Substitution ───────────────────────────────────── */

static void get_secret_val(const char *key, char *val, size_t val_size) {
  FILE *f = fopen(MIMI_SECRET_FILE, "r");
  if (!f)
    return;

  char line[128];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (eq) {
      *eq = '\0';
      if (strcmp(line, key) == 0) {
        char *v = eq + 1;
        // Trim newline
        char *nl = strpbrk(v, "\r\n");
        if (nl)
          *nl = '\0';
        // Trim quotes if present
        if (*v == '"' || *v == '\'') {
          v++;
          char *end = v + strlen(v) - 1;
          if (*end == '"' || *end == '\'')
            *end = '\0';
        }
        strncpy(val, v, val_size - 1);
        val[val_size - 1] = '\0';
        break;
      }
    }
  }
  fclose(f);
}

static char *resolve_secrets(const char *input) {
  if (!input)
    return NULL;
  char *result = strdup(input);
  if (!result)
    return NULL;

  char *p;
  while ((p = strstr(result, "{{SECRET:")) != NULL) {
    char *end = strstr(p, "}}");
    if (!end)
      break;

    *end = '\0';
    const char *key = p + 9;
    char val[64] = {0};
    get_secret_val(key, val, sizeof(val));

    size_t prefix_len = p - result;
    size_t val_len = strlen(val);
    size_t suffix_len = strlen(end + 2);
    size_t new_len = prefix_len + val_len + suffix_len;

    char *new_buf = malloc(new_len + 1);
    if (!new_buf)
      break;

    memcpy(new_buf, result, prefix_len);
    memcpy(new_buf + prefix_len, val, val_len);
    memcpy(new_buf + prefix_len + val_len, end + 2, suffix_len);
    new_buf[new_len] = '\0';

    free(result);
    result = new_buf;
  }
  return result;
}

/* ── SSRF Protection ────────────────────────────────────────── */

static bool is_ip_private(struct in_addr addr) {
  uint32_t ip = ntohl(addr.s_addr);

  // Loopback: 127.0.0.0/8
  if ((ip & 0xFF000000) == 0x7F000000)
    return true;
  // Private A: 10.0.0.0/8
  if ((ip & 0xFF000000) == 0x0A000000)
    return true;
  // Private B: 172.16.0.0/12
  if ((ip & 0xFFF00000) == 0xAC100000)
    return true;
  // Private C: 192.168.0.0/16
  if ((ip & 0xFFFF0000) == 0xC0A80000)
    return true;
  // Link-local: 169.254.0.0/16
  if ((ip & 0xFFFF0000) == 0xA9FE0000)
    return true;
  // Multicast: 224.0.0.0/4
  if ((ip & 0xF0000000) == 0xE0000000)
    return true;

  return false;
}

static int validate_url_and_dns(const char *url, uint32_t *resolved_ip) {
  if (!url)
    return -1;
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
    return -1;

  char host[128];
  const char *start = strstr(url, "://") + 3;
  const char *end = strpbrk(start, ":/");
  size_t host_len = end ? (size_t)(end - start) : strlen(start);
  if (host_len >= sizeof(host))
    return -1;
  strncpy(host, start, host_len);
  host[host_len] = '\0';

  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *res;
  if (getaddrinfo(host, NULL, &hints, &res) != 0)
    return -1; /* DNS Resolution Failed */

  int result = 0; /* OK */
  struct addrinfo *p;
  for (p = res; p != NULL; p = p->ai_next) {
    if (p->ai_family == AF_INET) {
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
      if (is_ip_private(ipv4->sin_addr)) {
        if (resolved_ip) {
          *resolved_ip = ntohl(ipv4->sin_addr.s_addr);
        }
        result = -2; /* SSRF Blocked */
        break;
      }
    }
  }
  freeaddrinfo(res);
  return result;
}

/* ── Session Management (Cookies) ─────────────────────────── */

static char *load_cookies(const char *session_key) {
  if (!session_key)
    return NULL;
  FILE *f = fopen(MIMI_SESSION_DB, "r");
  if (!f)
    return NULL;

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *data = malloc(size + 1);
  if (data) {
    fread(data, 1, size, f);
    data[size] = '\0';
  }
  fclose(f);

  if (!data)
    return NULL;

  cJSON *root = cJSON_Parse(data);
  free(data);
  if (!root)
    return NULL;

  cJSON *sess = cJSON_GetObjectItem(root, session_key);
  char *cookies = NULL;
  if (cJSON_IsString(sess)) {
    cookies = strdup(sess->valuestring);
  }
  cJSON_Delete(root);
  return cookies;
}

static void save_cookies(const char *session_key, const char *cookies) {
  if (!session_key || !cookies)
    return;

  char *data = NULL;
  FILE *f = fopen(MIMI_SESSION_DB, "r");
  if (f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(size + 1);
    if (data) {
      fread(data, 1, size, f);
      data[size] = '\0';
    }
    fclose(f);
  }

  cJSON *root = data ? cJSON_Parse(data) : cJSON_CreateObject();
  free(data);
  if (!root)
    root = cJSON_CreateObject();

  cJSON_ReplaceItemInObject(root, session_key, cJSON_CreateString(cookies));

  char *out = cJSON_PrintUnformatted(root);
  if (out) {
    f = fopen(MIMI_SESSION_DB, "w");
    if (f) {
      fputs(out, f);
      fclose(f);
    }
    free(out);
  }
  cJSON_Delete(root);
}

/* ── HTTP Client Execution ─────────────────────────────────── */

typedef struct {
  char *buf;
  size_t len;
  bool truncated;
  char *set_cookie;
  char *location;
} http_tool_data_t;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  http_tool_data_t *data = (http_tool_data_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (data->len + evt->data_len >= HTTP_MAX_RESPONSE_SIZE) {
      size_t remaining = HTTP_MAX_RESPONSE_SIZE - data->len - 1;
      if (remaining > 0) {
        memcpy(data->buf + data->len, evt->data, remaining);
        data->len += remaining;
      }
      data->truncated = true;
      return ESP_FAIL; /* Abort connection after 8KB */
    }
    memcpy(data->buf + data->len, evt->data, evt->data_len);
    data->len += evt->data_len;
    data->buf[data->len] = '\0';
  } else if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (strcasecmp(evt->header_key, "Set-Cookie") == 0) {
      if (data->set_cookie) {
        size_t old_len = strlen(data->set_cookie);
        size_t new_len = old_len + strlen(evt->header_value) + 3;
        char *tmp = realloc(data->set_cookie, new_len);
        if (tmp) {
          data->set_cookie = tmp;
          strcat(data->set_cookie, "; ");
          strcat(data->set_cookie, evt->header_value);
        }
      } else {
        data->set_cookie = strdup(evt->header_value);
      }
    } else if (strcasecmp(evt->header_key, "Location") == 0) {
      free(data->location);
      data->location = strdup(evt->header_value);
    }
  }
  return ESP_OK;
}

esp_err_t tool_http_request_execute(const char *input_json, char *output,
                                    size_t output_size) {
  ESP_LOGI(TAG, "Executing http_request");
  cJSON *root = cJSON_Parse(input_json);
  if (!root) {
    snprintf(output, output_size, "Error: invalid JSON");
    return ESP_ERR_INVALID_ARG;
  }

  const char *url_raw = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
  const char *method =
      cJSON_GetStringValue(cJSON_GetObjectItem(root, "method"));
  const char *body_raw =
      cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
  const char *session_key =
      cJSON_GetStringValue(cJSON_GetObjectItem(root, "session_key"));

  char *url = resolve_secrets(url_raw);
  char *body = resolve_secrets(body_raw);

  uint32_t resolved_ip = 0;
  int validation_result = validate_url_and_dns(url, &resolved_ip);

  if (validation_result != 0) {
    if (validation_result == -1) {
      snprintf(output, output_size, "Error: DNS resolution failed for %s",
               url_raw);
    } else if (validation_result == -2) {
      char ip_str[16];
      struct in_addr addr;
      addr.s_addr = htonl(resolved_ip);
      inet_ntoa_r(addr, ip_str, sizeof(ip_str));
      snprintf(output, output_size,
               "Error: SSRF protection blocked request to %s (Resolved: %s)",
               url_raw, ip_str);
    } else {
      snprintf(output, output_size, "Error: Invalid URL format");
    }

    free(url);
    free(body);
    cJSON_Delete(root);
    return ESP_ERR_INVALID_ARG;
  }

  http_tool_data_t tool_data = {.buf = calloc(1, HTTP_MAX_RESPONSE_SIZE),
                                .len = 0,
                                .truncated = false,
                                .set_cookie = NULL,
                                .location = NULL};

  char *current_url = strdup(url);
  int redirect_count = 0;
  esp_err_t err = ESP_OK;
  int status_code = 0;

  while (redirect_count <= 5) {
    uint32_t next_ip = 0;
    int val_res = validate_url_and_dns(current_url, &next_ip);
    if (val_res != 0) {
      status_code = 0;
      if (val_res == -1) {
        snprintf(output, output_size,
                 "Error: DNS resolution failed for redirect: %s", current_url);
      } else {
        snprintf(output, output_size,
                 "Error: SSRF protection blocked redirect to %s", current_url);
      }
      err = ESP_ERR_ADMISSION_CONTROL;
      break;
    }

    esp_http_client_config_t config = {
        .url = current_url,
        .event_handler = _http_event_handler,
        .user_data = &tool_data,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method =
            (redirect_count == 0 && method && strcasecmp(method, "POST") == 0)
                ? HTTP_METHOD_POST
                : HTTP_METHOD_GET,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char *cookies = load_cookies(session_key);
    if (cookies) {
      esp_http_client_set_header(client, "Cookie", cookies);
      free(cookies);
    }

    if (redirect_count == 0 && body) {
      esp_http_client_set_post_field(client, body, strlen(body));
      esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    /* Reset buffers for each hop if we want only final content,
       but here we normally only get content on 200. */
    tool_data.len = 0;
    if (tool_data.buf)
      tool_data.buf[0] = '\0';
    free(tool_data.location);
    tool_data.location = NULL;

    err = esp_http_client_perform(client);
    status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status_code >= 300 && status_code < 400 &&
        tool_data.location && redirect_count < 5) {
      redirect_count++;
      char *next_url = strdup(tool_data.location);
      free(current_url);
      current_url = next_url;
      continue;
    }
    break;
  }

  if (session_key && tool_data.set_cookie) {
    save_cookies(session_key, tool_data.set_cookie);
  }
  free(tool_data.set_cookie);
  free(tool_data.location);

  cJSON *out_root = cJSON_CreateObject();
  if (err != ESP_OK && !tool_data.truncated) {
    cJSON_AddNumberToObject(out_root, "status", status_code);
    cJSON_AddStringToObject(out_root, "body", "");
    cJSON_AddBoolToObject(out_root, "truncated", false);
    cJSON_AddStringToObject(out_root, "error", esp_err_to_name(err));
  } else {
    cJSON_AddNumberToObject(out_root, "status", status_code);
    cJSON_AddStringToObject(out_root, "body", tool_data.buf);
    cJSON_AddBoolToObject(out_root, "truncated", tool_data.truncated);
    cJSON_AddNullToObject(out_root, "error");
  }

  char *json_str = cJSON_PrintUnformatted(out_root);
  if (json_str) {
    snprintf(output, output_size, "%s", json_str);
    free(json_str);
  } else {
    snprintf(output, output_size, "{\"error\":\"Internal JSON error\"}");
  }
  cJSON_Delete(out_root);

  free(tool_data.buf);
  free(url);
  free(current_url);
  free(body);
  cJSON_Delete(root);
  return ESP_OK;
}
