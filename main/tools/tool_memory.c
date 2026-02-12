#include "tool_memory.h"
#include "cJSON.h"
#include "esp_log.h"
#include "memory/memory_store.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "tool_memory";

esp_err_t tool_memory_write_execute(const char *input_json, char *output,
                                    size_t output_size) {
  cJSON *root = cJSON_Parse(input_json);
  if (!root) {
    snprintf(output, output_size, "Error: invalid JSON");
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *content = cJSON_GetObjectItem(root, "content");
  if (!content || !cJSON_IsString(content)) {
    snprintf(output, output_size, "Error: missing content");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = memory_write_long_term(content->valuestring);
  cJSON_Delete(root);

  if (err == ESP_OK) {
    snprintf(output, output_size, "Memory updated");
  } else {
    snprintf(output, output_size, "Error: memory write failed");
  }
  return err;
}

esp_err_t tool_memory_append_execute(const char *input_json, char *output,
                                     size_t output_size) {
  cJSON *root = cJSON_Parse(input_json);
  if (!root) {
    snprintf(output, output_size, "Error: invalid JSON");
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *content = cJSON_GetObjectItem(root, "content");
  if (!content || !cJSON_IsString(content)) {
    snprintf(output, output_size, "Error: missing content");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = memory_append_today(content->valuestring);
  cJSON_Delete(root);

  if (err == ESP_OK) {
    snprintf(output, output_size, "Note appended to memory");
  } else {
    snprintf(output, output_size, "Error: memory append failed");
  }
  return err;
}
