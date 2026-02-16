#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

#include "agent/agent_loop.h"
#include "agent/scheduler.h"
#include "bus/message_bus.h"
#include "cli/serial_cli.h"
#include "gateway/ws_server.h"
#include "gateway/ui_bridge.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "media/media_limits.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_get_time.h"
#include "tools/tool_registry.h"
#include "wifi/wifi_manager.h"

#include "utils/log_redact.h"
#include <sys/stat.h>

static const char *TAG = "mimi";

static void restart_after_wifi_fail(const char *reason) {
  ESP_LOGE(TAG, "WiFi recovery restart: %s (delay=%dms)", reason,
           MIMI_WIFI_RESTART_DELAY_MS);
  vTaskDelay(pdMS_TO_TICKS(MIMI_WIFI_RESTART_DELAY_MS));
  esp_restart();
}

static void migrate_spiffs_files(void) {
  const char *safe_files[] = {"MEMORY.md", "SOUL.md", "USER.md"};
  const char *old_prefixes[] = {"/spiffs/memory/", "/spiffs/config/"};

  for (int i = 0; i < 3; i++) {
    char new_path[64];
    snprintf(new_path, sizeof(new_path), "%s/%s", MIMI_SPIFFS_PUBLIC_DIR,
             safe_files[i]);

    struct stat st;
    if (stat(new_path, &st) == 0)
      continue; // Already exists

    for (int j = 0; j < 2; j++) {
      char old_path[64];
      snprintf(old_path, sizeof(old_path), "%s%s", old_prefixes[j],
               safe_files[i]);
      if (stat(old_path, &st) == 0) {
        ESP_LOGI(TAG, "Migrating %s -> %s", old_path, new_path);
        rename(old_path, new_path);
        break;
      }
    }
  }
}

static esp_err_t init_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition truncated, erasing...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  return ret;
}

static esp_err_t init_spiffs(void) {
  esp_vfs_spiffs_conf_t conf = {
      .base_path = MIMI_SPIFFS_BASE,
      .partition_label = NULL,
      .max_files = 10,
      .format_if_mount_failed = true,
  };

  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    return ret;
  }

  size_t total = 0, used = 0;
  esp_spiffs_info(NULL, &total, &used);
  ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

  return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg) {
  ESP_LOGI(TAG, "Outbound dispatch started");

  while (1) {
    mimi_msg_t msg;
    if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK)
      continue;

    ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

    if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
      telegram_send_message(msg.chat_id, msg.content);
    } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
      ws_server_send(msg.chat_id, msg.content);
    } else {
      ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
    }

    free(msg.content);
  }
}

static void start_network_services(void) {
  ESP_ERROR_CHECK(telegram_bot_start());
  ESP_ERROR_CHECK(agent_loop_start());
  ESP_ERROR_CHECK(ws_server_start());

  xTaskCreatePinnedToCore(outbound_dispatch_task, "outbound", MIMI_OUTBOUND_STACK,
                          NULL, MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE);

  ESP_LOGI(TAG, "All services started!");
}

static void wifi_fail_watchdog_task(void *arg) {
  EventGroupHandle_t wifi_events = wifi_manager_get_event_group();
  while (1) {
    xEventGroupWaitBits(wifi_events, WIFI_FAIL_BIT, pdTRUE, pdFALSE,
                        portMAX_DELAY);
    if (wifi_manager_is_connected()) {
      continue;
    }
    restart_after_wifi_fail("reconnect retries exhausted");
  }
}

void app_main(void) {
  /* Silence noisy components */
  esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
  ESP_LOGI(TAG, "========================================");

  /* Print memory info */
  ESP_LOGI(TAG, "Internal free: %d bytes",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ESP_LOGI(TAG, "PSRAM free:    %d bytes",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  /* Phase 1: Core infrastructure */
  ESP_ERROR_CHECK(init_nvs());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(init_spiffs());
  migrate_spiffs_files();

  /* Initialize subsystems */
  ESP_ERROR_CHECK(message_bus_init());
  ESP_ERROR_CHECK(memory_store_init());
  ESP_ERROR_CHECK(session_mgr_init());
  ESP_ERROR_CHECK(wifi_manager_init());
  ESP_ERROR_CHECK(http_proxy_init());
  ESP_ERROR_CHECK(ui_bridge_init());
  ESP_ERROR_CHECK(telegram_bot_init());
  ESP_ERROR_CHECK(llm_proxy_init());
  ESP_ERROR_CHECK(tool_registry_init());
  media_limits_init();
  tool_time_init();
  ESP_ERROR_CHECK(agent_loop_init());
  scheduler_init();

  /* Start Serial CLI first (works without WiFi) */
  ESP_ERROR_CHECK(serial_cli_init());

  bool wifi_ready = false;

  /* Start WiFi STA first */
  esp_err_t wifi_err = wifi_manager_start();
  if (wifi_err == ESP_OK) {
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    if (wifi_manager_wait_connected(30000) == ESP_OK) {
      ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
      wifi_ready = true;
    } else {
      ESP_LOGW(TAG,
               "WiFi STA connect timeout. Starting provisioning portal...");
    }
  } else {
    ESP_LOGW(TAG,
             "No WiFi credentials found. Starting provisioning portal...");
  }

  if (!wifi_ready) {
    esp_err_t prov_err =
        wifi_manager_run_provisioning_portal(MIMI_WIFI_PROV_TIMEOUT_MS);
    if (prov_err == ESP_OK && wifi_manager_is_connected()) {
      ESP_LOGI(TAG, "Provisioning successful. WiFi connected: %s",
               wifi_manager_get_ip());
      wifi_ready = true;
    } else {
      ESP_LOGW(TAG, "Provisioning not completed. Serial CLI remains available.");
    }
  }

  if (wifi_ready) {
    start_network_services();
#if MIMI_WIFI_AUTO_RESTART_ON_FAIL
    xTaskCreatePinnedToCore(wifi_fail_watchdog_task, "wifi_fail_wd", 3072, NULL,
                            MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE);
#endif
  } else if (wifi_err == ESP_OK) {
#if MIMI_WIFI_AUTO_RESTART_ON_FAIL
    restart_after_wifi_fail("initial WiFi bring-up failed");
#endif
  }

  ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
