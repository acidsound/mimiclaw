#include "tool_get_time.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "tool_time";

static const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* Parse "Sat, 01 Feb 2025 10:25:00 GMT" → set system clock, return formatted
 * string */
static bool parse_and_set_time(const char *date_str, char *out,
                               size_t out_size) {
  int day, year, hour, min, sec;
  char mon_str[4] = {0};

  if (sscanf(date_str, "%*[^,], %d %3s %d %d:%d:%d", &day, mon_str, &year,
             &hour, &min, &sec) != 6) {
    return false;
  }

  int mon = -1;
  for (int i = 0; i < 12; i++) {
    if (strcmp(mon_str, MONTHS[i]) == 0) {
      mon = i;
      break;
    }
  }
  if (mon < 0)
    return false;

  struct tm tm = {
      .tm_sec = sec,
      .tm_min = min,
      .tm_hour = hour,
      .tm_mday = day,
      .tm_mon = mon,
      .tm_year = year - 1900,
  };

  /* Convert UTC to epoch — mktime expects local, so temporarily set UTC */
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(&tm);

  /* Restore timezone from NVS or default */
  char tz[64] = MIMI_TIMEZONE;
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(tz);
    nvs_get_str(nvs, MIMI_NVS_KEY_TIMEZONE, tz, &len);
    nvs_close(nvs);
  }
  setenv("TZ", tz, 1);
  tzset();

  if (t < 0)
    return false;

  struct timeval tv = {.tv_sec = t};
  settimeofday(&tv, NULL);

  /* Format in local time */
  struct tm local;
  localtime_r(&t, &local);
  strftime(out, out_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local);

  return true;
}

#include "esp_sntp.h"

/* Initialize SNTP */
void tool_time_init(void) {
  ESP_LOGI(TAG, "Initializing SNTP");
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  /* Load TZ from NVS or use default */
  char tz[64] = MIMI_TIMEZONE;
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(tz);
    if (nvs_get_str(nvs, MIMI_NVS_KEY_TIMEZONE, tz, &len) == ESP_OK) {
      ESP_LOGI(TAG, "TimeZone (NVS): %s", tz);
    } else {
      ESP_LOGI(TAG, "TimeZone (Default): %s", tz);
    }
    nvs_close(nvs);
  } else {
    ESP_LOGI(TAG, "TimeZone (Default): %s", tz);
  }

  setenv("TZ", tz, 1);
  tzset();
}

static void wait_for_sntp_sync(void) {
  int retry = 0;
  const int retry_count = 10;
  while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET &&
         ++retry < retry_count) {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
             retry_count);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

/* Fetch time via proxy: HEAD request to api.telegram.org, parse Date header */
static esp_err_t fetch_time_via_proxy(char *out, size_t out_size) {
  proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, 10000);
  if (!conn)
    return ESP_ERR_HTTP_CONNECT;

  const char *req = "HEAD / HTTP/1.1\r\n"
                    "Host: api.telegram.org\r\n"
                    "Connection: close\r\n\r\n";

  if (proxy_conn_write(conn, req, strlen(req)) < 0) {
    proxy_conn_close(conn);
    return ESP_ERR_HTTP_WRITE_DATA;
  }

  char buf[1024];
  int total = 0;
  while (total < (int)sizeof(buf) - 1) {
    int n = proxy_conn_read(conn, buf + total, sizeof(buf) - 1 - total, 10000);
    if (n <= 0)
      break;
    total += n;
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n"))
      break;
  }
  proxy_conn_close(conn);

  /* Find Date header */
  char *date_hdr = strcasestr(buf, "\r\nDate: ");
  if (!date_hdr)
    return ESP_ERR_NOT_FOUND;
  date_hdr += 8;

  char *eol = strstr(date_hdr, "\r\n");
  if (!eol)
    return ESP_ERR_NOT_FOUND;

  char date_val[64];
  size_t dlen = eol - date_hdr;
  if (dlen >= sizeof(date_val))
    return ESP_ERR_NOT_FOUND;
  memcpy(date_val, date_hdr, dlen);
  date_val[dlen] = '\0';

  if (!parse_and_set_time(date_val, out, out_size))
    return ESP_FAIL;
  return ESP_OK;
}

/* Fetch time via direct HTTPS */
static esp_err_t fetch_time_direct(char *out, size_t out_size) {
  esp_http_client_config_t config = {
      .url = "https://www.google.com/generate_204",
      .method = HTTP_METHOD_HEAD,
      .timeout_ms = 10000,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client)
    return ESP_FAIL;

  esp_err_t err = esp_http_client_perform(client);
  int status = 0;
  if (err == ESP_OK) {
    status = esp_http_client_get_status_code(client);
  }

  /* Get Date header */
  char *date_ptr = NULL;
  esp_http_client_get_header(client, "Date", &date_ptr);

  char date_val[64] = {0};
  if (date_ptr) {
    strncpy(date_val, date_ptr, sizeof(date_val) - 1);
    ESP_LOGI(TAG, "Fetched Date Header: '%s' (Status: %d)", date_val, status);
  } else {
    ESP_LOGW(TAG, "No Date header found (Status: %d, Err: %s)", status,
             esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);

  if (date_val[0] == '\0')
    return ESP_ERR_NOT_FOUND;

  if (!parse_and_set_time(date_val, out, out_size)) {
    ESP_LOGE(TAG, "Failed to parse date string: %s", date_val);
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t tool_get_time_execute(const char *input_json, char *output,
                                size_t output_size) {
  /* Check if system time is already set (e.g. via SNTP) */
  time_t now = 0;
  struct tm timeinfo = {0};
  time(&now);
  localtime_r(&now, &timeinfo);

  ESP_LOGI(TAG, "Current internal year: %d (tm_year: %d)",
           timeinfo.tm_year + 1900, timeinfo.tm_year);

  /* If year > 2025, assume valid time (we are in 2026) */
  if (timeinfo.tm_year > (2025 - 1900)) {
    strftime(output, output_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &timeinfo);
    ESP_LOGI(TAG, "Time (System): %s", output);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "System time not set (year <= 2025), fetching from network...");

  /* Try SNTP sync first if not synced */
  wait_for_sntp_sync();

  time(&now);
  localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year > (2025 - 1900)) {
    strftime(output, output_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &timeinfo);
    ESP_LOGI(TAG, "Time (SNTP): %s", output);
    return ESP_OK;
  }

  /* Fallback to HTTP */
  esp_err_t err;
  if (http_proxy_is_enabled()) {
    err = fetch_time_via_proxy(output, output_size);
  } else {
    err = fetch_time_direct(output, output_size);
  }

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Time (HTTP): %s", output);
  } else {
    snprintf(output, output_size, "Error: failed to fetch time (%s)",
             esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", output);
  }
  return err;
}
