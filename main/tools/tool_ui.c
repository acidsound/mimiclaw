#include "tools/tool_ui.h"

#include "cJSON.h"
#include "esp_log.h"
#include "gateway/ui_bridge.h"
#include "gateway/ws_server.h"
#include "mimi_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tool_ui";

esp_err_t tool_ui_capture_execute(const char *input_json, char *output,
                                  size_t output_size) {
  if (!output || output_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  output[0] = '\0';

  cJSON *in = cJSON_Parse(input_json ? input_json : "{}");
  if (!in || !cJSON_IsObject(in)) {
    if (in)
      cJSON_Delete(in);
    snprintf(output, output_size,
             "Error: invalid input JSON. Expected object.");
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *chat_id = cJSON_GetObjectItem(in, "chat_id");
  cJSON *goal = cJSON_GetObjectItem(in, "goal");
  cJSON *timeout_ms = cJSON_GetObjectItem(in, "timeout_ms");
  cJSON *max_width = cJSON_GetObjectItem(in, "max_width");
  cJSON *jpeg_quality = cJSON_GetObjectItem(in, "jpeg_quality");

  char chat_buf[32] = {0};
  char goal_buf[160] = {0};
  if (cJSON_IsString(chat_id)) {
    snprintf(chat_buf, sizeof(chat_buf), "%s", chat_id->valuestring);
  }
  if (cJSON_IsString(goal)) {
    snprintf(goal_buf, sizeof(goal_buf), "%s", goal->valuestring);
  }

  int timeout = cJSON_IsNumber(timeout_ms) ? timeout_ms->valueint
                                           : MIMI_UI_CAPTURE_TIMEOUT_MS;
  int width = cJSON_IsNumber(max_width) ? max_width->valueint
                                        : MIMI_UI_CAPTURE_MAX_WIDTH;
  int quality = cJSON_IsNumber(jpeg_quality) ? jpeg_quality->valueint
                                             : MIMI_UI_CAPTURE_JPEG_QUALITY;
  cJSON_Delete(in);

  if (chat_buf[0] == '\0') {
    snprintf(output, output_size,
             "Error: chat_id is required for ui_capture.");
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = ui_bridge_request_capture(chat_buf, goal_buf, timeout, width,
                                            quality, output, output_size);
  ESP_LOGI(TAG, "ui_capture chat=%s ret=%s", chat_buf, esp_err_to_name(err));
  return err;
}

esp_err_t tool_ui_action_execute(const char *input_json, char *output,
                                 size_t output_size) {
  if (!output || output_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  output[0] = '\0';

  cJSON *in = cJSON_Parse(input_json ? input_json : "{}");
  if (!in || !cJSON_IsObject(in)) {
    if (in)
      cJSON_Delete(in);
    snprintf(output, output_size,
             "Error: invalid input JSON. Expected object.");
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *chat_id = cJSON_GetObjectItem(in, "chat_id");
  cJSON *action = cJSON_GetObjectItem(in, "action");
  if (!cJSON_IsString(chat_id) || chat_id->valuestring[0] == '\0') {
    cJSON_Delete(in);
    snprintf(output, output_size, "Error: chat_id is required for ui_action.");
    return ESP_ERR_INVALID_ARG;
  }
  if (!cJSON_IsString(action) || action->valuestring[0] == '\0') {
    cJSON_Delete(in);
    snprintf(output, output_size, "Error: action is required.");
    return ESP_ERR_INVALID_ARG;
  }
  char chat_id_buf[32];
  char action_buf[24];
  snprintf(chat_id_buf, sizeof(chat_id_buf), "%s", chat_id->valuestring);
  snprintf(action_buf, sizeof(action_buf), "%s", action->valuestring);

  cJSON *payload = cJSON_CreateObject();
  if (!payload) {
    cJSON_Delete(in);
    snprintf(output, output_size, "Error: payload alloc failed.");
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddStringToObject(payload, "action", action_buf);

  cJSON *x = cJSON_GetObjectItem(in, "x_norm");
  cJSON *y = cJSON_GetObjectItem(in, "y_norm");
  cJSON *x2 = cJSON_GetObjectItem(in, "x2_norm");
  cJSON *y2 = cJSON_GetObjectItem(in, "y2_norm");
  cJSON *text = cJSON_GetObjectItem(in, "text");
  cJSON *key = cJSON_GetObjectItem(in, "key");
  cJSON *confidence = cJSON_GetObjectItem(in, "confidence");
  cJSON *reason = cJSON_GetObjectItem(in, "reason");

  if ((cJSON_IsNumber(x) && (x->valuedouble < 0.0 || x->valuedouble > 1.0)) ||
      (cJSON_IsNumber(y) && (y->valuedouble < 0.0 || y->valuedouble > 1.0)) ||
      (cJSON_IsNumber(x2) && (x2->valuedouble < 0.0 || x2->valuedouble > 1.0)) ||
      (cJSON_IsNumber(y2) && (y2->valuedouble < 0.0 || y2->valuedouble > 1.0))) {
    cJSON_Delete(payload);
    cJSON_Delete(in);
    snprintf(output, output_size,
             "Error: normalized coordinates must be in range [0.0, 1.0].");
    return ESP_ERR_INVALID_ARG;
  }

  if (cJSON_IsNumber(x))
    cJSON_AddNumberToObject(payload, "x_norm", x->valuedouble);
  if (cJSON_IsNumber(y))
    cJSON_AddNumberToObject(payload, "y_norm", y->valuedouble);
  if (cJSON_IsNumber(x2))
    cJSON_AddNumberToObject(payload, "x2_norm", x2->valuedouble);
  if (cJSON_IsNumber(y2))
    cJSON_AddNumberToObject(payload, "y2_norm", y2->valuedouble);
  if (cJSON_IsString(text))
    cJSON_AddStringToObject(payload, "text", text->valuestring);
  if (cJSON_IsString(key))
    cJSON_AddStringToObject(payload, "key", key->valuestring);
  if (cJSON_IsNumber(confidence))
    cJSON_AddNumberToObject(payload, "confidence", confidence->valuedouble);
  if (cJSON_IsString(reason))
    cJSON_AddStringToObject(payload, "reason", reason->valuestring);

  char *payload_json = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);
  cJSON_Delete(in);
  if (!payload_json) {
    snprintf(output, output_size, "Error: failed to serialize action payload.");
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = ws_server_send_event(chat_id_buf, "input_action", payload_json);
  free(payload_json);
  if (err != ESP_OK) {
    snprintf(output, output_size, "Error: failed to send action (%s)",
             esp_err_to_name(err));
    return err;
  }

  snprintf(output, output_size, "Action sent: %s", action_buf);
  return ESP_OK;
}

esp_err_t tool_ios_sim_capture_to_telegram_execute(const char *input_json,
                                                   char *output,
                                                   size_t output_size) {
  if (!output || output_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  output[0] = '\0';

  cJSON *in = cJSON_Parse(input_json ? input_json : "{}");
  if (!in || !cJSON_IsObject(in)) {
    if (in)
      cJSON_Delete(in);
    snprintf(output, output_size, "Error: invalid input JSON. Expected object.");
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *chat_id = cJSON_GetObjectItem(in, "chat_id");
  cJSON *tg_chat_id = cJSON_GetObjectItem(in, "tg_chat_id");
  cJSON *url = cJSON_GetObjectItem(in, "url");
  cJSON *tap_mode = cJSON_GetObjectItem(in, "tap_mode");
  cJSON *open_wait_ms = cJSON_GetObjectItem(in, "open_wait_ms");
  cJSON *wait_after_tap_ms = cJSON_GetObjectItem(in, "wait_after_tap_ms");
  cJSON *timeout_ms = cJSON_GetObjectItem(in, "timeout_ms");

  char helper_chat[32] = {0};
  char telegram_chat[32] = {0};
  char url_buf[256] = {0};
  char tap_mode_buf[16] = {0};
  if (cJSON_IsString(chat_id)) {
    snprintf(helper_chat, sizeof(helper_chat), "%s", chat_id->valuestring);
  }
  if (cJSON_IsString(tg_chat_id)) {
    snprintf(telegram_chat, sizeof(telegram_chat), "%s", tg_chat_id->valuestring);
  }
  if (cJSON_IsString(url)) {
    snprintf(url_buf, sizeof(url_buf), "%s", url->valuestring);
  }
  if (cJSON_IsString(tap_mode)) {
    snprintf(tap_mode_buf, sizeof(tap_mode_buf), "%s", tap_mode->valuestring);
  }
  int open_wait = cJSON_IsNumber(open_wait_ms) ? open_wait_ms->valueint : 2500;
  int wait_after_tap =
      cJSON_IsNumber(wait_after_tap_ms) ? wait_after_tap_ms->valueint : 1000;
  int timeout = cJSON_IsNumber(timeout_ms) ? timeout_ms->valueint : 30000;
  cJSON_Delete(in);

  if (helper_chat[0] == '\0') {
    snprintf(output, output_size,
             "Error: chat_id(helper id) is required for ios_sim_capture_to_telegram.");
    return ESP_ERR_INVALID_ARG;
  }
  if (telegram_chat[0] == '\0') {
    snprintf(output, output_size,
             "Error: tg_chat_id is required for ios_sim_capture_to_telegram.");
    return ESP_ERR_INVALID_ARG;
  }
  if (url_buf[0] == '\0') {
    snprintf(output, output_size,
             "Error: url is required for ios_sim_capture_to_telegram.");
    return ESP_ERR_INVALID_ARG;
  }

  if (strncmp(url_buf, "http://", 7) != 0 && strncmp(url_buf, "https://", 8) != 0) {
    snprintf(output, output_size, "Error: url must start with http:// or https://");
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = ui_bridge_request_ios_sim_capture(
      helper_chat, telegram_chat, url_buf, open_wait, wait_after_tap,
      tap_mode_buf[0] ? tap_mode_buf : "center", timeout, output, output_size);
  ESP_LOGI(TAG, "ios_sim_capture_to_telegram helper=%s tg=%s ret=%s", helper_chat,
           telegram_chat, esp_err_to_name(err));
  return err;
}
