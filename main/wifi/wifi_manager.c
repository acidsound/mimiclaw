#include "wifi_manager.h"
#include "mimi_config.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "wifi";

#define WIFI_PROV_DONE_BIT BIT2
#define WIFI_PROV_SCAN_LIMIT 20
#define WIFI_PROV_REQ_MAX_LEN 512
#define WIFI_PROV_CONNECT_TIMEOUT_MS 30000

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static char s_ip_str[16] = "0.0.0.0";
static bool s_connected = false;
static bool s_ap_netif_created = false;
static httpd_handle_t s_prov_httpd = NULL;

static const char *PROV_HTML =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MimiClaw Wi-Fi Setup</title>"
    "<style>body{font-family:sans-serif;max-width:520px;margin:24px auto;padding:0 12px;}"
    "button{margin:4px 0;padding:8px 12px;}input{width:100%;padding:8px;margin:6px 0;}"
    "#msg{white-space:pre-wrap;padding:8px;background:#f4f4f4;}</style></head><body>"
    "<h2>MimiClaw Wi-Fi Setup</h2><p>Select SSID, enter password, connect.</p>"
    "<button onclick='loadScan()'>Rescan</button><div id='list'></div><hr>"
    "<div>SSID</div><input id='ssid' readonly><div>Password</div>"
    "<input id='pw' type='password' placeholder='Wi-Fi password'>"
    "<button onclick='doConnect()'>Connect</button><pre id='msg'></pre>"
    "<script>"
    "async function loadScan(){"
    "let r=await fetch('/scan'); let j=await r.json(); let el=document.getElementById('list'); el.innerHTML='';"
    "(j.ssids||[]).forEach(s=>{let b=document.createElement('button'); b.textContent=s;"
    "b.onclick=()=>{document.getElementById('ssid').value=s;}; el.appendChild(b); el.appendChild(document.createElement('br'));});"
    "}"
    "async function doConnect(){"
    "let ssid=document.getElementById('ssid').value; let password=document.getElementById('pw').value;"
    "if(!ssid){document.getElementById('msg').textContent='Select SSID first'; return;}"
    "document.getElementById('msg').textContent='Connecting...';"
    "let r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid,password})});"
    "let j=await r.json(); document.getElementById('msg').textContent=JSON.stringify(j,null,2);"
    "if(j.ok){ setTimeout(()=>location.reload(),1500);}"
    "}"
    "loadScan();"
    "</script></body></html>";

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_connected = false;
    if (s_retry_count < MIMI_WIFI_MAX_RETRY) {
      /* Exponential backoff: 1s, 2s, 4s, 8s, ... capped at 30s */
      uint32_t delay_ms = MIMI_WIFI_RETRY_BASE_MS << s_retry_count;
      if (delay_ms > MIMI_WIFI_RETRY_MAX_MS) {
        delay_ms = MIMI_WIFI_RETRY_MAX_MS;
      }
      ESP_LOGW(TAG, "Disconnected, retry %d/%d in %" PRIu32 "ms",
               s_retry_count + 1, MIMI_WIFI_MAX_RETRY, delay_ms);
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
      esp_wifi_connect();
      s_retry_count++;
    } else {
      ESP_LOGE(TAG, "Failed to connect after %d retries", MIMI_WIFI_MAX_RETRY);
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Connected! IP: %s", s_ip_str);
    s_retry_count = 0;
    s_connected = true;

    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_PROV_DONE_BIT);
  }
}

static esp_err_t prov_root_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, PROV_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t prov_scan_get_handler(httpd_req_t *req) {
  wifi_scan_config_t scan_cfg = {0};
  esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan_failed\"}");
  }

  uint16_t ap_num = 0;
  esp_wifi_scan_get_ap_num(&ap_num);
  if (ap_num > WIFI_PROV_SCAN_LIMIT) {
    ap_num = WIFI_PROV_SCAN_LIMIT;
  }

  wifi_ap_record_t *records = calloc(ap_num ? ap_num : 1, sizeof(wifi_ap_record_t));
  if (!records) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_mem\"}");
  }

  err = esp_wifi_scan_get_ap_records(&ap_num, records);
  if (err != ESP_OK) {
    free(records);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan_read_failed\"}");
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "ssids");
  for (uint16_t i = 0; i < ap_num; i++) {
    if (records[i].ssid[0] != '\0') {
      cJSON_AddItemToArray(arr, cJSON_CreateString((const char *)records[i].ssid));
    }
  }

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  free(records);

  if (!json) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"json_failed\"}");
  }

  httpd_resp_set_type(req, "application/json");
  esp_err_t send_err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  free(json);
  return send_err;
}

static esp_err_t prov_connect_post_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > WIFI_PROV_REQ_MAX_LEN) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"invalid_body\"}");
  }

  char *body = calloc(1, req->content_len + 1);
  if (!body) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_mem\"}");
  }

  int received = 0;
  while (received < req->content_len) {
    int r = httpd_req_recv(req, body + received, req->content_len - received);
    if (r <= 0) {
      free(body);
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_set_type(req, "application/json");
      return httpd_resp_sendstr(req,
                                "{\"ok\":false,\"error\":\"recv_failed\"}");
    }
    received += r;
  }
  body[req->content_len] = '\0';

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_json\"}");
  }

  cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
  cJSON *password = cJSON_GetObjectItem(root, "password");
  if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
      ssid->valuestring[0] == '\0') {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"invalid_args\"}");
  }

  if (strlen(ssid->valuestring) > 32 || strlen(password->valuestring) > 63) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"arg_too_long\"}");
  }

  wifi_config_t sta_cfg = {0};
  strncpy((char *)sta_cfg.sta.ssid, ssid->valuestring,
          sizeof(sta_cfg.sta.ssid) - 1);
  strncpy((char *)sta_cfg.sta.password, password->valuestring,
          sizeof(sta_cfg.sta.password) - 1);
  cJSON_Delete(root);

  xEventGroupClearBits(s_wifi_event_group,
                       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_PROV_DONE_BIT);
  s_retry_count = 0;
  s_connected = false;
  snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");

  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"set_config_failed\"}");
  }

  err = esp_wifi_connect();
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"connect_start_failed\"}");
  }

  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(WIFI_PROV_CONNECT_TIMEOUT_MS));

  if (bits & WIFI_CONNECTED_BIT) {
    wifi_manager_set_credentials((const char *)sta_cfg.sta.ssid,
                                 (const char *)sta_cfg.sta.password);
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"ip\":\"%s\"}",
             s_ip_str);
    return httpd_resp_sendstr(req, resp);
  }

  httpd_resp_set_status(req, "408 Request Timeout");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req,
                            "{\"ok\":false,\"error\":\"connect_failed\"}");
}

static esp_err_t prov_httpd_start(void) {
  if (s_prov_httpd) {
    return ESP_OK;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 6144;
  cfg.max_uri_handlers = 8;

  esp_err_t err = httpd_start(&s_prov_httpd, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Provisioning HTTP start failed: %s", esp_err_to_name(err));
    return err;
  }

  httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = prov_root_get_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t scan = {
      .uri = "/scan",
      .method = HTTP_GET,
      .handler = prov_scan_get_handler,
      .user_ctx = NULL,
  };
  httpd_uri_t connect = {
      .uri = "/connect",
      .method = HTTP_POST,
      .handler = prov_connect_post_handler,
      .user_ctx = NULL,
  };

  httpd_register_uri_handler(s_prov_httpd, &root);
  httpd_register_uri_handler(s_prov_httpd, &scan);
  httpd_register_uri_handler(s_prov_httpd, &connect);
  return ESP_OK;
}

static void prov_httpd_stop(void) {
  if (!s_prov_httpd) {
    return;
  }
  httpd_stop(s_prov_httpd);
  s_prov_httpd = NULL;
}

esp_err_t wifi_manager_init(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  ESP_LOGI(TAG, "WiFi manager initialized");
  return ESP_OK;
}

esp_err_t wifi_manager_start(void) {
  wifi_config_t wifi_cfg = {0};
  bool found = false;
  bool disable_secret_fallback = false;

  /* NVS overrides take highest priority (set via CLI) */
  nvs_handle_t nvs;
  if (nvs_open(MIMI_NVS_WIFI, NVS_READONLY, &nvs) == ESP_OK) {
    uint8_t no_secret = 0;
    if (nvs_get_u8(nvs, MIMI_NVS_KEY_WIFI_NO_SECRET, &no_secret) == ESP_OK &&
        no_secret == 1) {
      disable_secret_fallback = true;
    }

    size_t len = sizeof(wifi_cfg.sta.ssid);
    if (nvs_get_str(nvs, MIMI_NVS_KEY_SSID, (char *)wifi_cfg.sta.ssid, &len) == ESP_OK &&
        wifi_cfg.sta.ssid[0] != '\0') {
      len = sizeof(wifi_cfg.sta.password);
      nvs_get_str(nvs, MIMI_NVS_KEY_PASS, (char *)wifi_cfg.sta.password, &len);
      found = true;
    }
    nvs_close(nvs);
  }

  /* Fall back to build-time secrets unless explicitly disabled by wifi_reset */
  if (!found && !disable_secret_fallback) {
    if (MIMI_SECRET_WIFI_SSID[0] != '\0') {
      strncpy((char *)wifi_cfg.sta.ssid, MIMI_SECRET_WIFI_SSID,
              sizeof(wifi_cfg.sta.ssid) - 1);
      strncpy((char *)wifi_cfg.sta.password, MIMI_SECRET_WIFI_PASS,
              sizeof(wifi_cfg.sta.password) - 1);
      found = true;
    }
  }

  if (!found) {
    ESP_LOGW(TAG,
             "No WiFi credentials. Use CLI: wifi_set <SSID> <PASS> or wifi_portal");
    return ESP_ERR_NOT_FOUND;
  }

  ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_cfg.sta.ssid);

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms) {
  TickType_t ticks =
      (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      ticks);

  if (bits & WIFI_CONNECTED_BIT) {
    return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void) { return s_connected; }

const char *wifi_manager_get_ip(void) { return s_ip_str; }

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password) {
  nvs_handle_t nvs;
  ESP_ERROR_CHECK(nvs_open(MIMI_NVS_WIFI, NVS_READWRITE, &nvs));
  ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_SSID, ssid));
  ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_PASS, password));
  ESP_ERROR_CHECK(nvs_set_u8(nvs, MIMI_NVS_KEY_WIFI_NO_SECRET, 0));
  ESP_ERROR_CHECK(nvs_commit(nvs));
  nvs_close(nvs);
  ESP_LOGI(TAG, "WiFi credentials saved for SSID: %s", ssid);
  return ESP_OK;
}

esp_err_t wifi_manager_reset_credentials(void) {
  nvs_handle_t nvs;
  ESP_ERROR_CHECK(nvs_open(MIMI_NVS_WIFI, NVS_READWRITE, &nvs));
  nvs_erase_key(nvs, MIMI_NVS_KEY_SSID);
  nvs_erase_key(nvs, MIMI_NVS_KEY_PASS);
  ESP_ERROR_CHECK(nvs_set_u8(nvs, MIMI_NVS_KEY_WIFI_NO_SECRET, 1));
  ESP_ERROR_CHECK(nvs_commit(nvs));
  nvs_close(nvs);

  esp_err_t err = esp_wifi_restore();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_wifi_restore failed during wifi reset: %s",
             esp_err_to_name(err));
  }

  s_connected = false;
  snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  ESP_LOGI(TAG, "WiFi credentials reset complete (secret fallback disabled)");
  return ESP_OK;
}

EventGroupHandle_t wifi_manager_get_event_group(void) { return s_wifi_event_group; }

esp_err_t wifi_manager_run_provisioning_portal(uint32_t timeout_ms) {
  if (!s_ap_netif_created) {
    esp_netif_create_default_wifi_ap();
    s_ap_netif_created = true;
  }

  uint32_t suffix = (uint32_t)(esp_random() & 0xFFFF);
  char ap_ssid[33] = {0};
  snprintf(ap_ssid, sizeof(ap_ssid), "%s-%04X", MIMI_WIFI_PROV_AP_PREFIX,
           (unsigned)suffix);

  wifi_config_t ap_cfg = {0};
  strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
  strncpy((char *)ap_cfg.ap.password, MIMI_WIFI_PROV_AP_PASS,
          sizeof(ap_cfg.ap.password) - 1);
  ap_cfg.ap.ssid_len = strlen(ap_ssid);
  ap_cfg.ap.channel = 1;
  ap_cfg.ap.max_connection = MIMI_WIFI_PROV_AP_MAX_CONN;
  ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

  xEventGroupClearBits(s_wifi_event_group,
                       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_PROV_DONE_BIT);
  s_retry_count = 0;
  s_connected = false;

  esp_err_t err = esp_wifi_stop();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT &&
      err != ESP_ERR_WIFI_NOT_STARTED) {
    ESP_LOGW(TAG, "esp_wifi_stop before provisioning failed: %s",
             esp_err_to_name(err));
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  err = prov_httpd_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Provisioning portal failed to start");
    esp_wifi_set_mode(WIFI_MODE_STA);
    return err;
  }

  ESP_LOGW(TAG, "Provisioning AP started: SSID=%s PASS=%s", ap_ssid,
           MIMI_WIFI_PROV_AP_PASS);
  ESP_LOGW(TAG, "Connect to AP and open: http://192.168.4.1");

  TickType_t start = xTaskGetTickCount();
  while (1) {
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (bits & WIFI_PROV_DONE_BIT) {
      break;
    }

    if (timeout_ms != UINT32_MAX) {
      uint32_t elapsed_ms =
          (uint32_t)(((xTaskGetTickCount() - start) * 1000) / configTICK_RATE_HZ);
      if (elapsed_ms >= timeout_ms) {
        ESP_LOGW(TAG, "Provisioning portal timed out");
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  prov_httpd_stop();

  if (s_connected) {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "Provisioning complete. STA connected: %s", s_ip_str);
    return ESP_OK;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  return ESP_ERR_TIMEOUT;
}
