#include "gateway/ui_bridge.h"

#include "gateway/ws_server.h"
#include "mimi_config.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_bridge";

typedef struct {
  bool active;
  bool done;
  bool ok;
  char chat_id[32];
  char request_id[32];
  char media_type[24];
  int width;
  int height;
  int rotation;
  char err[96];
  char *image_b64;
} ui_pending_req_t;

typedef struct {
  bool ready;
  char request_id[32];
  char media_type[24];
  int width;
  int height;
  int rotation;
  char *image_b64;
} ui_latest_capture_t;

typedef struct {
  bool active;
  bool done;
  bool ok;
  char chat_id[32];
  char request_id[32];
  char file_id[128];
  char err[96];
} ui_sim_req_t;

static SemaphoreHandle_t s_lock = NULL;
static ui_pending_req_t s_pending = {0};
static ui_latest_capture_t s_latest = {0};
static ui_sim_req_t s_sim = {0};

static void clear_pending_locked(void) {
  if (s_pending.image_b64) {
    free(s_pending.image_b64);
    s_pending.image_b64 = NULL;
  }
  memset(&s_pending, 0, sizeof(s_pending));
}

static void clear_latest_locked(void) {
  if (s_latest.image_b64) {
    free(s_latest.image_b64);
    s_latest.image_b64 = NULL;
  }
  memset(&s_latest, 0, sizeof(s_latest));
}

static void clear_sim_locked(void) { memset(&s_sim, 0, sizeof(s_sim)); }

static void make_request_id(char *buf, size_t size) {
  uint32_t r = esp_random();
  snprintf(buf, size, "cap_%08x", (unsigned)r);
}

static void clamp_capture_params(int *timeout_ms, int *max_width,
                                 int *jpeg_quality) {
  if (*timeout_ms < 1000)
    *timeout_ms = 1000;
  if (*timeout_ms > 30000)
    *timeout_ms = 30000;

  if (*max_width < 320)
    *max_width = 320;
  if (*max_width > 2160)
    *max_width = 2160;

  if (*jpeg_quality < 30)
    *jpeg_quality = 30;
  if (*jpeg_quality > 95)
    *jpeg_quality = 95;
}

esp_err_t ui_bridge_init(void) {
  if (s_lock) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    ESP_LOGE(TAG, "Failed to create lock");
    return ESP_ERR_NO_MEM;
  }
  memset(&s_pending, 0, sizeof(s_pending));
  memset(&s_latest, 0, sizeof(s_latest));
  memset(&s_sim, 0, sizeof(s_sim));
  ESP_LOGI(TAG, "UI bridge initialized");
  return ESP_OK;
}

static void finish_pending_with_error(const char *msg) {
  s_pending.ok = false;
  s_pending.done = true;
  snprintf(s_pending.err, sizeof(s_pending.err), "%s", msg ? msg : "capture_failed");
}

static void finish_sim_with_error(const char *msg) {
  s_sim.ok = false;
  s_sim.done = true;
  snprintf(s_sim.err, sizeof(s_sim.err), "%s", msg ? msg : "ios_sim_failed");
}

bool ui_bridge_handle_ws_event(const char *chat_id, const cJSON *root) {
  if (!chat_id || !root || !s_lock) {
    return false;
  }

  const cJSON *type = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "type");
  if (!cJSON_IsString(type)) {
    return false;
  }

  if (strcmp(type->valuestring, "ios_sim_capture_result") == 0) {
    const cJSON *payload =
        cJSON_GetObjectItemCaseSensitive((cJSON *)root, "payload");
    if (!payload || !cJSON_IsObject(payload)) {
      ESP_LOGW(TAG, "ios_sim_capture_result missing payload");
      return true;
    }
    const cJSON *rid =
        cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "request_id");
    const cJSON *status =
        cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "status");
    const cJSON *file_id =
        cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "telegram_file_id");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "error");
    if (!cJSON_IsString(rid)) {
      ESP_LOGW(TAG, "ios_sim_capture_result missing request_id");
      return true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_sim.active || strcmp(s_sim.request_id, rid->valuestring) != 0 ||
        strcmp(s_sim.chat_id, chat_id) != 0) {
      xSemaphoreGive(s_lock);
      ESP_LOGW(TAG, "ios_sim_capture_result ignored (no matching pending request)");
      return true;
    }

    if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") != 0) {
      finish_sim_with_error(cJSON_IsString(err) ? err->valuestring
                                                : "ios_sim_capture_error");
      xSemaphoreGive(s_lock);
      return true;
    }
    if (!cJSON_IsString(file_id) || file_id->valuestring[0] == '\0') {
      finish_sim_with_error("missing_telegram_file_id");
      xSemaphoreGive(s_lock);
      return true;
    }

    s_sim.ok = true;
    s_sim.done = true;
    snprintf(s_sim.file_id, sizeof(s_sim.file_id), "%s", file_id->valuestring);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "ios_sim_capture_result accepted: req=%s file_id=%s",
             rid->valuestring, s_sim.file_id);
    return true;
  }

  if (strcmp(type->valuestring, "capture_result") != 0) {
    return false;
  }

  const cJSON *payload = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "payload");
  if (!payload || !cJSON_IsObject(payload)) {
    ESP_LOGW(TAG, "capture_result missing payload");
    return true;
  }

  const cJSON *rid = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "request_id");
  const cJSON *status = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "status");
  const cJSON *image_b64 = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "image_base64");
  const cJSON *media_type = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "media_type");
  const cJSON *width = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "width");
  const cJSON *height = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "height");
  const cJSON *rotation =
      cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "rotation");
  const cJSON *err = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "error");

  if (!cJSON_IsString(rid)) {
    ESP_LOGW(TAG, "capture_result missing request_id");
    return true;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);

  if (!s_pending.active || strcmp(s_pending.request_id, rid->valuestring) != 0 ||
      strcmp(s_pending.chat_id, chat_id) != 0) {
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "capture_result ignored (no matching pending request)");
    return true;
  }

  if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") != 0) {
    finish_pending_with_error(cJSON_IsString(err) ? err->valuestring : "capture_error");
    xSemaphoreGive(s_lock);
    return true;
  }

  if (!cJSON_IsString(image_b64) || image_b64->valuestring[0] == '\0') {
    finish_pending_with_error("empty_capture_data");
    xSemaphoreGive(s_lock);
    return true;
  }

  size_t b64_len = strlen(image_b64->valuestring);
  if (b64_len > MIMI_UI_CAPTURE_MAX_B64) {
    finish_pending_with_error("capture_too_large");
    xSemaphoreGive(s_lock);
    return true;
  }

  char *copy = heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM);
  if (!copy) {
    finish_pending_with_error("capture_alloc_failed");
    xSemaphoreGive(s_lock);
    return true;
  }
  memcpy(copy, image_b64->valuestring, b64_len + 1);

  if (s_pending.image_b64) {
    free(s_pending.image_b64);
    s_pending.image_b64 = NULL;
  }
  s_pending.image_b64 = copy;
  s_pending.ok = true;
  s_pending.done = true;
  s_pending.width = cJSON_IsNumber(width) ? width->valueint : 0;
  s_pending.height = cJSON_IsNumber(height) ? height->valueint : 0;
  s_pending.rotation = cJSON_IsNumber(rotation) ? rotation->valueint : 0;
  snprintf(s_pending.media_type, sizeof(s_pending.media_type), "%s",
           cJSON_IsString(media_type) ? media_type->valuestring : "image/jpeg");

  xSemaphoreGive(s_lock);
  ESP_LOGI(TAG, "capture_result accepted: req=%s size=%d", rid->valuestring, (int)b64_len);
  return true;
}

esp_err_t ui_bridge_request_capture(const char *chat_id, const char *goal,
                                    int timeout_ms, int max_width,
                                    int jpeg_quality, char *output,
                                    size_t output_size) {
  if (!chat_id || chat_id[0] == '\0' || !output || output_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_lock) {
    esp_err_t init_err = ui_bridge_init();
    if (init_err != ESP_OK) {
      snprintf(output, output_size, "Error: ui bridge unavailable");
      return init_err;
    }
  }
  if (!ws_server_is_started()) {
    snprintf(output, output_size,
             "Error: websocket server is not ready. Wait for Wi-Fi connect and retry.");
    return ESP_ERR_INVALID_STATE;
  }

  clamp_capture_params(&timeout_ms, &max_width, &jpeg_quality);

  char request_id[32];
  make_request_id(request_id, sizeof(request_id));

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_pending.active) {
    xSemaphoreGive(s_lock);
    snprintf(output, output_size,
             "Capture busy: another request is already in progress.");
    return ESP_ERR_INVALID_STATE;
  }
  clear_pending_locked();
  s_pending.active = true;
  s_pending.done = false;
  s_pending.ok = false;
  snprintf(s_pending.chat_id, sizeof(s_pending.chat_id), "%s", chat_id);
  snprintf(s_pending.request_id, sizeof(s_pending.request_id), "%s", request_id);
  xSemaphoreGive(s_lock);

  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "request_id", request_id);
  cJSON_AddStringToObject(payload, "format", "jpeg_base64");
  cJSON_AddNumberToObject(payload, "max_width", max_width);
  cJSON_AddNumberToObject(payload, "jpeg_quality", jpeg_quality);
  if (goal && goal[0]) {
    cJSON_AddStringToObject(payload, "goal", goal);
  } else {
    cJSON_AddStringToObject(payload, "goal", "Capture current screen for UI automation.");
  }
  char *payload_json = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);
  if (!payload_json) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_pending_locked();
    xSemaphoreGive(s_lock);
    snprintf(output, output_size, "Error: failed to build capture payload.");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t send_err = ws_server_send_event(chat_id, "capture_request", payload_json);
  free(payload_json);
  if (send_err != ESP_OK) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_pending_locked();
    xSemaphoreGive(s_lock);
    snprintf(output, output_size, "Error: failed to send capture request (%s)",
             esp_err_to_name(send_err));
    return send_err;
  }

  int elapsed = 0;
  const int poll_ms = 100;
  while (elapsed < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(poll_ms));
    elapsed += poll_ms;

    bool done = false;
    bool ok = false;
    char err[96] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    done = s_pending.active && s_pending.done;
    ok = done && s_pending.ok;
    if (done && !ok) {
      snprintf(err, sizeof(err), "%s", s_pending.err);
      clear_pending_locked();
      xSemaphoreGive(s_lock);
      snprintf(output, output_size, "Capture failed: %s", err[0] ? err : "unknown");
      return ESP_FAIL;
    }
    if (ok) {
      clear_latest_locked();
      s_latest.ready = true;
      snprintf(s_latest.request_id, sizeof(s_latest.request_id), "%s",
               s_pending.request_id);
      snprintf(s_latest.media_type, sizeof(s_latest.media_type), "%s",
               s_pending.media_type[0] ? s_pending.media_type : "image/jpeg");
      s_latest.width = s_pending.width;
      s_latest.height = s_pending.height;
      s_latest.rotation = s_pending.rotation;
      s_latest.image_b64 = s_pending.image_b64;
      s_pending.image_b64 = NULL;
      clear_pending_locked();
      xSemaphoreGive(s_lock);

      snprintf(output, output_size,
               "Capture OK: request_id=%s, media_type=%s, size=%dx%d, rotation=%d",
               s_latest.request_id, s_latest.media_type, s_latest.width,
               s_latest.height, s_latest.rotation);
      return ESP_OK;
    }
    xSemaphoreGive(s_lock);
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  clear_pending_locked();
  xSemaphoreGive(s_lock);
  snprintf(output, output_size, "Capture timeout after %d ms", timeout_ms);
  return ESP_ERR_TIMEOUT;
}

esp_err_t ui_bridge_request_ios_sim_capture(const char *helper_chat_id,
                                            const char *tg_chat_id,
                                            const char *url, int open_wait_ms,
                                            int wait_after_tap_ms,
                                            const char *tap_mode, int timeout_ms,
                                            char *output, size_t output_size) {
  if (!helper_chat_id || helper_chat_id[0] == '\0' || !tg_chat_id ||
      tg_chat_id[0] == '\0' || !url || url[0] == '\0' || !output ||
      output_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_lock) {
    esp_err_t init_err = ui_bridge_init();
    if (init_err != ESP_OK) {
      snprintf(output, output_size, "Error: ui bridge unavailable");
      return init_err;
    }
  }
  if (!ws_server_is_started()) {
    snprintf(output, output_size,
             "Error: websocket server is not ready. Wait for Wi-Fi connect and retry.");
    return ESP_ERR_INVALID_STATE;
  }

  if (timeout_ms < 1000)
    timeout_ms = 1000;
  if (timeout_ms > 60000)
    timeout_ms = 60000;
  if (open_wait_ms < 500)
    open_wait_ms = 500;
  if (open_wait_ms > 15000)
    open_wait_ms = 15000;
  if (wait_after_tap_ms < 200)
    wait_after_tap_ms = 200;
  if (wait_after_tap_ms > 15000)
    wait_after_tap_ms = 15000;

  char request_id[32];
  make_request_id(request_id, sizeof(request_id));

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_sim.active) {
    xSemaphoreGive(s_lock);
    snprintf(output, output_size,
             "iOS simulator scenario busy: another request is in progress.");
    return ESP_ERR_INVALID_STATE;
  }
  clear_sim_locked();
  s_sim.active = true;
  s_sim.done = false;
  s_sim.ok = false;
  snprintf(s_sim.chat_id, sizeof(s_sim.chat_id), "%s", helper_chat_id);
  snprintf(s_sim.request_id, sizeof(s_sim.request_id), "%s", request_id);
  xSemaphoreGive(s_lock);

  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "request_id", request_id);
  cJSON_AddStringToObject(payload, "tg_chat_id", tg_chat_id);
  cJSON_AddStringToObject(payload, "url", url);
  cJSON_AddStringToObject(payload, "tap_mode",
                          (tap_mode && tap_mode[0]) ? tap_mode : "center");
  cJSON_AddNumberToObject(payload, "open_wait_ms", open_wait_ms);
  cJSON_AddNumberToObject(payload, "wait_after_tap_ms", wait_after_tap_ms);
  cJSON_AddNumberToObject(payload, "timeout_ms", timeout_ms);
  char *payload_json = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);
  if (!payload_json) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_sim_locked();
    xSemaphoreGive(s_lock);
    snprintf(output, output_size, "Error: failed to build ios_sim payload.");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t send_err =
      ws_server_send_event(helper_chat_id, "ios_sim_capture_request", payload_json);
  free(payload_json);
  if (send_err != ESP_OK) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_sim_locked();
    xSemaphoreGive(s_lock);
    snprintf(output, output_size, "Error: failed to send ios_sim request (%s)",
             esp_err_to_name(send_err));
    return send_err;
  }

  int elapsed = 0;
  const int poll_ms = 100;
  while (elapsed < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(poll_ms));
    elapsed += poll_ms;

    bool done = false;
    bool ok = false;
    char err[96] = {0};
    char fid[128] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    done = s_sim.active && s_sim.done;
    ok = done && s_sim.ok;
    if (done && !ok) {
      snprintf(err, sizeof(err), "%s", s_sim.err);
      clear_sim_locked();
      xSemaphoreGive(s_lock);
      snprintf(output, output_size, "iOS sim capture failed: %s",
               err[0] ? err : "unknown");
      return ESP_FAIL;
    }
    if (ok) {
      snprintf(fid, sizeof(fid), "%s", s_sim.file_id);
      clear_sim_locked();
      xSemaphoreGive(s_lock);
      snprintf(output, output_size, "iOS sim capture sent to Telegram. file_id=%s",
               fid);
      return ESP_OK;
    }
    xSemaphoreGive(s_lock);
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  clear_sim_locked();
  xSemaphoreGive(s_lock);
  snprintf(output, output_size, "iOS sim capture timeout after %d ms", timeout_ms);
  return ESP_ERR_TIMEOUT;
}

bool ui_bridge_take_latest_capture(ui_capture_frame_t *out) {
  if (!out || !s_lock) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_latest.ready || !s_latest.image_b64) {
    xSemaphoreGive(s_lock);
    return false;
  }

  snprintf(out->request_id, sizeof(out->request_id), "%s", s_latest.request_id);
  snprintf(out->media_type, sizeof(out->media_type), "%s", s_latest.media_type);
  out->width = s_latest.width;
  out->height = s_latest.height;
  out->rotation = s_latest.rotation;
  out->image_b64 = s_latest.image_b64;

  s_latest.image_b64 = NULL;
  s_latest.ready = false;
  xSemaphoreGive(s_lock);
  return true;
}

void ui_bridge_free_capture(ui_capture_frame_t *frame) {
  if (!frame) {
    return;
  }
  if (frame->image_b64) {
    free(frame->image_b64);
  }
  memset(frame, 0, sizeof(*frame));
}
