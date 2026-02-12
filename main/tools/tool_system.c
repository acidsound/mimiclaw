#include "tools/tool_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "tool_system";

esp_err_t tool_heap_info_execute(const char *input_json, char *output,
                                 size_t output_size) {
  size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t total_free = esp_get_free_heap_size();
  size_t min_free = esp_get_minimum_free_heap_size();

  snprintf(output, output_size,
           "Heap Info:\n- Internal Free: %zu bytes\n- PSRAM Free: %zu bytes\n- "
           "Total Free: %zu bytes\n- Lifetime Min Free: %zu bytes",
           free_internal, free_psram, total_free, min_free);

  return ESP_OK;
}

static void restart_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
}

esp_err_t tool_restart_execute(const char *input_json, char *output,
                               size_t output_size) {
  snprintf(output, output_size, "OK: Restarting device in 2 seconds...");
  ESP_LOGI(TAG, "Restart triggered via tool");

  /* Create a separate task to delay and restart */
  xTaskCreate(restart_task, "restart_task", 2048, NULL, 10, NULL);

  return ESP_OK;
}
